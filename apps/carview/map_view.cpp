#include "map_view.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "audio/xml.h"
#include "fh1data/archive.h"
#include "fh1data/xds.h"
#include "traffic/nav.h"
#include "traffic/roads.h"

namespace carview {

namespace {

constexpr const char* kRoadTypes[] = {"dirt", "b", "a", "freeway"};  // drawn in this order
constexpr int kRoadTypeCount = 4;
constexpr double kPi = 3.14159265358979323846;

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

MapView::Rgba colour(const audio::XmlNode* n, MapView::Rgba fallback) {
  if (!n) return fallback;
  return {float(n->number("r", fallback.r)), float(n->number("g", fallback.g)), float(n->number("b", fallback.b)),
          float(n->number("a", fallback.a))};
}

// A profile's road, rumour-region and icon groups.
MapView::Profile read_profile(const audio::XmlNode& root) {
  MapView::Profile out;
  const audio::XmlNode* profile = root.child("Profile");
  if (!profile) return out;
  for (const auto& g : profile->children) {
    if (!audio::iequals(g.name, "Group")) continue;
    const audio::XmlNode* filter = g.child("Filter");
    if (!filter) continue;
    const std::string tag = filter->attr("tag"), value = filter->attr("value");
    if (audio::iequals(tag, "road_type")) {
      MapView::LineStyle s;
      for (const auto& sym : g.children) {
        if (!audio::iequals(sym.name, "Symbolizer")) continue;
        const std::string type = sym.attr("type");
        const audio::XmlNode* size = sym.child("Size");
        if (type.find("fade") != std::string::npos) {
          s.under_size = size ? size->number("value") : 0;
          s.under = colour(sym.child("ColourPrimary"), {0, 0, 0, 128});
        } else if (type.rfind("line_coloured", 0) == 0) {
          s.line_size = size ? size->number("value") : 0;
          s.edge = colour(sym.child("ColourPrimary"), {120, 120, 120, 255});
          s.core = colour(sym.child("ColourSecondary"), s.edge);
          s.core.a = 255;
          s.edge.a = 255;
        }
      }
      out.roads[value] = s;
    } else if (audio::iequals(tag, "type") && audio::iequals(value, "barn_find_region")) {
      for (const auto& sym : g.children) {
        if (!audio::iequals(sym.name, "Symbolizer")) continue;
        if (audio::iequals(sym.attr("type"), "ellipse")) out.region_fill = colour(sym.child("ColourPrimary"), out.region_fill);
        if (audio::iequals(sym.attr("type"), "ellipse_alpha")) out.region_rim = colour(sym.child("ColourPrimary"), out.region_rim);
      }
    } else if (audio::iequals(tag, "activity_type") || audio::iequals(tag, "vehicle_type")) {
      std::vector<MapView::IconStyle> icons;
      for (const auto& sym : g.children) {
        if (!audio::iequals(sym.name, "Symbolizer")) continue;
        const audio::XmlNode* atlas = sym.child("Atlas");
        if (!atlas) continue;
        MapView::IconStyle s;
        s.cell_x = int(atlas->number("x"));
        s.cell_y = int(atlas->number("y"));
        const audio::XmlNode* size = sym.child("DynamicSize");
        if (!size) size = sym.child("Size");
        s.size = size ? size->number("value", 45) : 45;
        s.colour = colour(sym.child("ColourPrimary"), {255, 255, 255, 255});
        icons.push_back(s);
      }
      // A filter value can head several groups (the pause map's layers):
      // the first group with icons wins.
      if (!icons.empty() && !out.icons.count(value)) out.icons[value] = std::move(icons);
    }
  }
  return out;
}

bool read_xml_file(const std::filesystem::path& path, audio::XmlNode& root, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return fail(error, "cannot read " + path.string());
  std::stringstream ss;
  ss << f.rdbuf();
  std::string why;
  if (!audio::parse_xml(ss.str(), root, &why)) return fail(error, path.filename().string() + ": " + why);
  return true;
}

bool read_texture(fh1::Archive& zip, const std::string& name, MapView::Atlas& out, std::string* error) {
  auto r = zip.read(name);
  if (!r.ok()) return fail(error, name + ": " + r.error);
  auto x = fh1::decode_xds(r.data);
  if (x.levels.empty() || x.levels[0].rgba.empty()) return fail(error, name + ": " + x.error);
  out.width = x.levels[0].width;
  out.height = x.levels[0].height;
  out.rgba = std::move(x.levels[0].rgba);
  return true;
}

// A panel being drawn: sRGB-encoded colour, blended in that space (UI).
struct Canvas {
  int w = 0, h = 0;
  std::vector<float> rgb, alpha;  // straight colour and coverage (0-1)
  Canvas(int width, int height, MapView::Rgba fill)
      : w(width), h(height), rgb(size_t(width) * height * 3), alpha(size_t(width) * height, fill.a / 255.0f) {
    for (size_t i = 0; i < rgb.size(); i += 3) rgb[i] = fill.r, rgb[i + 1] = fill.g, rgb[i + 2] = fill.b;
  }
  void blend(int x, int y, const MapView::Rgba& c, float cover) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const float a = std::clamp(cover * c.a / 255.0f, 0.0f, 1.0f);
    if (a <= 0) return;
    const size_t i = size_t(y) * w + x;
    float* p = &rgb[i * 3];
    // Over: on an opaque canvas the plain mix.
    const float A = alpha[i], out = a + A * (1 - a), k = a / out;
    p[0] += (c.r - p[0]) * k;
    p[1] += (c.g - p[1]) * k;
    p[2] += (c.b - p[2]) * k;
    alpha[i] = out;
  }
  // An antialiased capsule from a to b, `width` pixels across.
  void line(double ax, double ay, double bx, double by, double width, const MapView::Rgba& c) {
    const float r = float(width * 0.5);
    const float r_out = r + 0.5f;
    const float r_in = std::max(0.0f, r - 0.5f);
    const float r_out2 = r_out * r_out;
    const float r_in2 = r_in * r_in;
    const int x0 = std::max(0, int(std::floor(std::min(ax, bx) - r_out)));
    const int x1 = std::min(w - 1, int(std::ceil(std::max(ax, bx) + r_out)));
    const int y0 = std::max(0, int(std::floor(std::min(ay, by) - r_out)));
    const int y1 = std::min(h - 1, int(std::ceil(std::max(ay, by) + r_out)));
    if (x0 > x1 || y0 > y1) return;
    const float fax = float(ax), fay = float(ay);
    const float dx = float(bx - ax), dy = float(by - ay);
    const float len2 = dx * dx + dy * dy;
    const float inv_len2 = len2 > 1e-6f ? (1.0f / len2) : 0.0f;
    const bool use_strip = std::fabs(dy) > 0.5f;
    const float slope = use_strip ? (dx / dy) : 0.0f;
    const float hx = use_strip ? (r_out * std::sqrt(1.0f + slope * slope)) : 0.0f;
    for (int y = y0; y <= y1; ++y) {
      const float py_w = float(y) + 0.5f;
      int rx0 = x0, rx1 = x1;
      if (use_strip) {
        const float x_mid = fax + (py_w - fay) * slope;
        rx0 = std::max(x0, int(std::floor(x_mid - hx)));
        rx1 = std::min(x1, int(std::ceil(x_mid + hx)));
      }
      const float py = py_w - fay;
      for (int x = rx0; x <= rx1; ++x) {
        const float px = float(x) + 0.5f - fax;
        const float t = len2 > 1e-6f ? std::clamp((px * dx + py * dy) * inv_len2, 0.0f, 1.0f) : 0.0f;
        const float ex = px - t * dx, ey = py - t * dy;
        const float d2 = ex * ex + ey * ey;
        if (d2 >= r_out2) continue;
        const float cover = (d2 <= r_in2) ? 1.0f : (r_out - std::sqrt(d2));
        if (cover > 0.0f) blend(x, y, c, cover);
      }
    }
  }
  // A filled polygon (even-odd), four sub-rows a pixel for soft edges.
  void polygon(const std::vector<std::array<double, 2>>& pts, const MapView::Rgba& c) {
    if (pts.size() < 3) return;
    double lo = 1e30, hi = -1e30;
    for (const auto& p : pts) lo = std::min(lo, p[1]), hi = std::max(hi, p[1]);
    const int y0 = std::max(0, int(std::floor(lo))), y1 = std::min(h - 1, int(std::ceil(hi)));
    std::vector<float> cover(static_cast<size_t>(w));
    std::vector<double> xs;
    for (int y = y0; y <= y1; ++y) {
      std::fill(cover.begin(), cover.end(), 0.0f);
      for (int s = 0; s < 4; ++s) {
        const double sy = y + (s + 0.5) / 4;
        xs.clear();
        for (size_t i = 0; i < pts.size(); ++i) {
          const auto& a = pts[i];
          const auto& b = pts[(i + 1) % pts.size()];
          if ((a[1] <= sy) != (b[1] <= sy)) xs.push_back(a[0] + (sy - a[1]) / (b[1] - a[1]) * (b[0] - a[0]));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
          const double l = std::max(0.0, xs[k]), r = std::min(double(w), xs[k + 1]);
          for (int x = int(std::floor(l)); x < int(std::ceil(r)) && x < w; ++x) {
            const double in = std::min(r, x + 1.0) - std::max(l, double(x));
            if (in > 0) cover[size_t(x)] += float(in) / 4;
          }
        }
      }
      for (int x = 0; x < w; ++x)
        if (cover[size_t(x)] > 0) blend(x, y, c, std::min(1.0f, cover[size_t(x)]));
    }
  }
  // One cell of an 8 x 8 icon atlas, `size` pixels square, centred on
  // (cx, cy), turned by `angle` (rad, clockwise on screen), tinted.
  void icon(const MapView::Atlas& atlas, int cell_x, int cell_y, double cx, double cy, double size, double angle,
            const MapView::Rgba& tint) {
    if (atlas.rgba.empty() || size < 1) return;
    const double cell = atlas.width / 8.0;
    const double r = size * 0.75;
    const int x0 = std::max(0, int(cx - r)), x1 = std::min(w - 1, int(cx + r));
    const int y0 = std::max(0, int(cy - r)), y1 = std::min(h - 1, int(cy + r));
    const double c = std::cos(angle), s = std::sin(angle);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        // Back through the turn into the cell's unit square.
        const double px = x + 0.5 - cx, py = y + 0.5 - cy;
        const double u = (c * px + s * py) / size + 0.5, v = (-s * px + c * py) / size + 0.5;
        if (u < 0 || v < 0 || u >= 1 || v >= 1) continue;
        // Bilinear inside the cell (half a texel in from its edges).
        const double tx = std::clamp((cell_x + u) * cell - 0.5, cell_x * cell, (cell_x + 1) * cell - 1.0);
        const double ty = std::clamp((cell_y + v) * cell - 0.5, cell_y * cell, (cell_y + 1) * cell - 1.0);
        const int ix = int(tx), iy = int(ty);
        const double fx = tx - ix, fy = ty - iy;
        float px4[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k) {
          const int sx = std::min(int(atlas.width) - 1, ix + (k & 1)), sy = std::min(int(atlas.height) - 1, iy + (k >> 1));
          const double wgt = (k & 1 ? fx : 1 - fx) * (k >> 1 ? fy : 1 - fy);
          const uint8_t* t = &atlas.rgba[(size_t(sy) * atlas.width + sx) * 4];
          // Premultiplied while filtering.
          const float a = t[3] / 255.0f;
          for (int ch = 0; ch < 3; ++ch) px4[ch] += float(wgt) * t[ch] * a;
          px4[3] += float(wgt) * a;
        }
        if (px4[3] <= 0.004f) continue;
        MapView::Rgba col{px4[0] / px4[3] * tint.r / 255.0f, px4[1] / px4[3] * tint.g / 255.0f,
                          px4[2] / px4[3] * tint.b / 255.0f, tint.a};
        blend(x, y, col, px4[3]);
      }
  }
  HudImage image() const {
    HudImage out;
    out.width = uint32_t(w);
    out.height = uint32_t(h);
    out.rgba.resize(size_t(w) * h * 4);
    for (size_t i = 0, j = 0; i < rgb.size(); i += 3, j += 4) {
      out.rgba[j] = uint8_t(std::clamp(rgb[i], 0.0f, 255.0f) + 0.5f);
      out.rgba[j + 1] = uint8_t(std::clamp(rgb[i + 1], 0.0f, 255.0f) + 0.5f);
      out.rgba[j + 2] = uint8_t(std::clamp(rgb[i + 2], 0.0f, 255.0f) + 0.5f);
      out.rgba[j + 3] = uint8_t(std::clamp(alpha[i / 3] * 255.0f * fade_at(int(i / 3) % w, int(i / 3) / w), 0.0f, 255.0f) + 0.5f);
    }
    return out;
  }
  // > 0: the coverage fades to nothing towards the panel's edge, from this
  // fraction of the half size in (an ellipse).
  float edge_fade = 0;
  float fade_at(int x, int y) const {
    if (edge_fade <= 0) return 1;
    const float dx = (x + 0.5f - 0.5f * w) / (0.5f * w), dy = (y + 0.5f - 0.5f * h) / (0.5f * h);
    const float r = std::sqrt(dx * dx + dy * dy);
    return std::clamp((1 - r) / edge_fade, 0.0f, 1.0f);
  }
};

// Direct opaque RGBA8 canvas for fullscreen World Map rendering: avoids
// allocating/converting intermediate float RGB+alpha buffers and uses fixed-point
// integer blending + tight scanline capsule rasterization.
struct FastCanvas {
  int w = 0, h = 0;
  std::vector<uint8_t>& rgba;
  FastCanvas(int width, int height, std::vector<uint8_t>& buf) : w(width), h(height), rgba(buf) {
    rgba.resize(size_t(w) * h * 4);
  }
  void fill_rect_row(int y, int x0, int x1, uint8_t r, uint8_t g, uint8_t b) {
    if (x0 >= x1) return;
    uint8_t* p = &rgba[(size_t(y) * w + x0) * 4];
    for (int x = x0; x < x1; ++x, p += 4) p[0] = r, p[1] = g, p[2] = b, p[3] = 255;
  }
  inline void blend_px(uint8_t* p, int cr, int cg, int cb, int a256) {
    if (a256 >= 256) {
      p[0] = uint8_t(cr);
      p[1] = uint8_t(cg);
      p[2] = uint8_t(cb);
    } else if (a256 > 0) {
      p[0] = uint8_t(int(p[0]) + (((cr - int(p[0])) * a256) >> 8));
      p[1] = uint8_t(int(p[1]) + (((cg - int(p[1])) * a256) >> 8));
      p[2] = uint8_t(int(p[2]) + (((cb - int(p[2])) * a256) >> 8));
    }
  }
  void line(double ax, double ay, double bx, double by, double width, const MapView::Rgba& c) {
    const float r = float(width * 0.5);
    const float r_out = r + 0.5f;
    const float r_in = std::max(0.0f, r - 0.5f);
    const float r_out2 = r_out * r_out;
    const float r_in2 = r_in * r_in;
    const int x0 = std::max(0, int(std::floor(std::min(ax, bx) - r_out)));
    const int x1 = std::min(w - 1, int(std::ceil(std::max(ax, bx) + r_out)));
    const int y0 = std::max(0, int(std::floor(std::min(ay, by) - r_out)));
    const int y1 = std::min(h - 1, int(std::ceil(std::max(ay, by) + r_out)));
    if (x0 > x1 || y0 > y1) return;
    const int cr = int(std::clamp(c.r, 0.0f, 255.0f) + 0.5f);
    const int cg = int(std::clamp(c.g, 0.0f, 255.0f) + 0.5f);
    const int cb = int(std::clamp(c.b, 0.0f, 255.0f) + 0.5f);
    const float ca = std::clamp(c.a / 255.0f, 0.0f, 1.0f) * 256.0f;
    const int ca_full = int(ca + 0.5f);
    if (ca_full <= 0) return;
    const float fax = float(ax), fay = float(ay);
    const float dx = float(bx - ax), dy = float(by - ay);
    const float len2 = dx * dx + dy * dy;
    const float inv_len2 = len2 > 1e-6f ? (1.0f / len2) : 0.0f;
    const bool use_strip = std::fabs(dy) > 0.5f;
    const float slope = use_strip ? (dx / dy) : 0.0f;
    const float hx = use_strip ? (r_out * std::sqrt(1.0f + slope * slope)) : 0.0f;
    for (int y = y0; y <= y1; ++y) {
      const float py_w = float(y) + 0.5f;
      int rx0 = x0, rx1 = x1;
      if (use_strip) {
        const float x_mid = fax + (py_w - fay) * slope;
        rx0 = std::max(x0, int(std::floor(x_mid - hx)));
        rx1 = std::min(x1, int(std::ceil(x_mid + hx)));
      }
      if (rx0 > rx1) continue;
      const float py = py_w - fay;
      uint8_t* p = &rgba[(size_t(y) * w + rx0) * 4];
      for (int x = rx0; x <= rx1; ++x, p += 4) {
        const float px = float(x) + 0.5f - fax;
        const float t = len2 > 1e-6f ? std::clamp((px * dx + py * dy) * inv_len2, 0.0f, 1.0f) : 0.0f;
        const float ex = px - t * dx, ey = py - t * dy;
        const float d2 = ex * ex + ey * ey;
        if (d2 >= r_out2) continue;
        if (d2 <= r_in2) {
          blend_px(p, cr, cg, cb, ca_full);
        } else {
          const int a256 = int((r_out - std::sqrt(d2)) * ca + 0.5f);
          if (a256 > 0) blend_px(p, cr, cg, cb, a256);
        }
      }
    }
  }
  void polygon(const std::vector<std::array<double, 2>>& pts, const MapView::Rgba& c) {
    if (pts.size() < 3) return;
    double lo = 1e30, hi = -1e30, xlo = 1e30, xhi = -1e30;
    for (const auto& p : pts) {
      lo = std::min(lo, p[1]), hi = std::max(hi, p[1]);
      xlo = std::min(xlo, p[0]), xhi = std::max(xhi, p[0]);
    }
    if (xhi < 0 || xlo >= w || hi < 0 || lo >= h) return;
    const int y0 = std::max(0, int(std::floor(lo))), y1 = std::min(h - 1, int(std::ceil(hi)));
    const int cr = int(std::clamp(c.r, 0.0f, 255.0f) + 0.5f);
    const int cg = int(std::clamp(c.g, 0.0f, 255.0f) + 0.5f);
    const int cb = int(std::clamp(c.b, 0.0f, 255.0f) + 0.5f);
    const float ca = std::clamp(c.a / 255.0f, 0.0f, 1.0f) * 256.0f;
    std::vector<float> cover(static_cast<size_t>(w));
    std::vector<double> xs;
    for (int y = y0; y <= y1; ++y) {
      int min_x = w, max_x = -1;
      for (int s = 0; s < 4; ++s) {
        const double sy = y + (s + 0.5) / 4;
        xs.clear();
        for (size_t i = 0; i < pts.size(); ++i) {
          const auto& a = pts[i];
          const auto& b = pts[(i + 1) % pts.size()];
          if ((a[1] <= sy) != (b[1] <= sy)) xs.push_back(a[0] + (sy - a[1]) / (b[1] - a[1]) * (b[0] - a[0]));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
          const double l = std::max(0.0, xs[k]), r = std::min(double(w), xs[k + 1]);
          const int xl = int(std::floor(l)), xr = std::min(w, int(std::ceil(r)));
          if (xl < min_x) min_x = xl;
          if (xr - 1 > max_x) max_x = xr - 1;
          for (int x = xl; x < xr; ++x) {
            const double in = std::min(r, x + 1.0) - std::max(l, double(x));
            if (in > 0) cover[size_t(x)] += float(in) * 0.25f;
          }
        }
      }
      if (min_x <= max_x) {
        uint8_t* p = &rgba[(size_t(y) * w + min_x) * 4];
        for (int x = min_x; x <= max_x; ++x, p += 4) {
          const float cv = cover[size_t(x)];
          if (cv > 0.0f) {
            cover[size_t(x)] = 0.0f;
            blend_px(p, cr, cg, cb, int(std::min(1.0f, cv) * ca + 0.5f));
          }
        }
      }
    }
  }
  void icon(const MapView::Atlas& atlas, int cell_x, int cell_y, double cx, double cy, double size, double angle,
            const MapView::Rgba& tint) {
    if (atlas.rgba.empty() || size < 1.0) return;
    const double r = (angle == 0.0) ? (size * 0.5 + 1.0) : (size * 0.75);
    if (cx + r < 0.0 || cx - r >= w || cy + r < 0.0 || cy - r >= h) return;
    const int x0 = std::max(0, int(std::floor(cx - r))), x1 = std::min(w - 1, int(std::ceil(cx + r)));
    const int y0 = std::max(0, int(std::floor(cy - r))), y1 = std::min(h - 1, int(std::ceil(cy + r)));
    const double cell = atlas.width / 8.0;
    const int aw = int(atlas.width), ah = int(atlas.height);
    const double tmin_x = cell_x * cell, tmax_x = (cell_x + 1) * cell - 1.0;
    const double tmin_y = cell_y * cell, tmax_y = (cell_y + 1) * cell - 1.0;
    const int tr = int(std::clamp(tint.r, 0.0f, 255.0f) + 0.5f);
    const int tg = int(std::clamp(tint.g, 0.0f, 255.0f) + 0.5f);
    const int tb = int(std::clamp(tint.b, 0.0f, 255.0f) + 0.5f);
    const int ta = int(std::clamp(tint.a, 0.0f, 255.0f) + 0.5f);
    if (ta <= 0) return;

    if (angle == 0.0) {
      const double inv_sz = 1.0 / size;
      const double dtx = cell * inv_sz;
      // Find tight [ix0, ix1] where u in [0, 1).
      const int ix0 = std::max(x0, int(std::ceil(cx - size * 0.5 - 0.5)));
      const int ix1 = std::min(x1, int(std::floor(cx + size * 0.5 - 0.5)));
      if (ix0 > ix1) return;
      const int64_t tx_min_fp = int64_t(std::llround(tmin_x * 65536.0));
      const int64_t tx_max_fp = int64_t(std::llround(tmax_x * 65536.0));
      const int64_t dtx_fp = int64_t(std::llround(dtx * 65536.0));
      const double u_start = (ix0 + 0.5 - cx) * inv_sz + 0.5;
      const int64_t tx_start_fp = int64_t(std::llround(((cell_x + u_start) * cell - 0.5) * 65536.0));
      const int tr_scale = (tr * ta + 127) >> 8;
      const int tg_scale = (tg * ta + 127) >> 8;
      const int tb_scale = (tb * ta + 127) >> 8;
      for (int y = y0; y <= y1; ++y) {
        const double v = (y + 0.5 - cy) * inv_sz + 0.5;
        if (v < 0.0 || v >= 1.0) continue;
        const double ty = std::clamp((cell_y + v) * cell - 0.5, tmin_y, tmax_y);
        const int iy = int(ty), iy1 = std::min(ah - 1, iy + 1);
        const int fy = int((ty - iy) * 256.0 + 0.5), ify = 256 - fy;
        const uint8_t* row0 = &atlas.rgba[size_t(iy) * aw * 4];
        const uint8_t* row1 = &atlas.rgba[size_t(iy1) * aw * 4];
        uint8_t* dst = &rgba[(size_t(y) * w + ix0) * 4];
        int64_t tx_fp = tx_start_fp;
        for (int x = ix0; x <= ix1; ++x, tx_fp += dtx_fp, dst += 4) {
          const int64_t txc = std::clamp<int64_t>(tx_fp, tx_min_fp, tx_max_fp);
          const int ix = int(txc >> 16), ix1_s = std::min(aw - 1, ix + 1);
          const uint8_t* t00 = row0 + ix * 4;
          const uint8_t* t10 = row0 + ix1_s * 4;
          const uint8_t* t01 = row1 + ix * 4;
          const uint8_t* t11 = row1 + ix1_s * 4;
          const int a00 = t00[3], a10 = t10[3], a01 = t01[3], a11 = t11[3];
          if ((a00 | a10 | a01 | a11) == 0) continue;
          const int fx = int((txc >> 8) & 255), ifx = 256 - fx;
          const int aw00 = ifx * ify * a00, aw10 = fx * ify * a10, aw01 = ifx * fy * a01, aw11 = fx * fy * a11;
          const int asum = (aw00 + aw10 + aw01 + aw11) >> 16;  // 0..255
          if (asum <= 1) continue;
          // Premultiplied RGB in [0..255*255], shifted down to [0..255] with zero divisions!
          const int pr = (t00[0] * aw00 + t10[0] * aw10 + t01[0] * aw01 + t11[0] * aw11) >> 24;
          const int pg = (t00[1] * aw00 + t10[1] * aw10 + t01[1] * aw01 + t11[1] * aw11) >> 24;
          const int pb = (t00[2] * aw00 + t10[2] * aw10 + t01[2] * aw01 + t11[2] * aw11) >> 24;
          const int eff_a = (asum * ta + 128) >> 8;
          const int inv_a = 256 - eff_a;
          dst[0] = uint8_t(std::min(255, (dst[0] * inv_a + pr * tr_scale) >> 8));
          dst[1] = uint8_t(std::min(255, (dst[1] * inv_a + pg * tg_scale) >> 8));
          dst[2] = uint8_t(std::min(255, (dst[2] * inv_a + pb * tb_scale) >> 8));
        }
      }
      return;
    }

    const double c = std::cos(angle), s = std::sin(angle);
    const double inv_sz = 1.0 / size;
    for (int y = y0; y <= y1; ++y) {
      uint8_t* dst = &rgba[(size_t(y) * w + x0) * 4];
      for (int x = x0; x <= x1; ++x, dst += 4) {
        const double px = x + 0.5 - cx, py = y + 0.5 - cy;
        const double u = (c * px + s * py) * inv_sz + 0.5, v = (-s * px + c * py) * inv_sz + 0.5;
        if (u < 0.0 || v < 0.0 || u >= 1.0 || v >= 1.0) continue;
        const double tx = std::clamp((cell_x + u) * cell - 0.5, tmin_x, tmax_x);
        const double ty = std::clamp((cell_y + v) * cell - 0.5, tmin_y, tmax_y);
        const int ix = int(tx), iy = int(ty);
        const int ix1 = std::min(aw - 1, ix + 1), iy1 = std::min(ah - 1, iy + 1);
        const int fx = int((tx - ix) * 256.0 + 0.5), fy = int((ty - iy) * 256.0 + 0.5);
        const int ifx = 256 - fx, ify = 256 - fy;
        const uint8_t* t00 = &atlas.rgba[(size_t(iy) * aw + ix) * 4];
        const uint8_t* t10 = &atlas.rgba[(size_t(iy) * aw + ix1) * 4];
        const uint8_t* t01 = &atlas.rgba[(size_t(iy1) * aw + ix) * 4];
        const uint8_t* t11 = &atlas.rgba[(size_t(iy1) * aw + ix1) * 4];
        const int a00 = t00[3], a10 = t10[3], a01 = t01[3], a11 = t11[3];
        if ((a00 | a10 | a01 | a11) == 0) continue;
        const int aw00 = ifx * ify * a00, aw10 = fx * ify * a10, aw01 = ifx * fy * a01, aw11 = fx * fy * a11;
        const int asum = aw00 + aw10 + aw01 + aw11;
        if (asum <= 65536) continue;
        const int pr = (t00[0] * aw00 + t10[0] * aw10 + t01[0] * aw01 + t11[0] * aw11) / asum;
        const int pg = (t00[1] * aw00 + t10[1] * aw10 + t01[1] * aw01 + t11[1] * aw11) / asum;
        const int pb = (t00[2] * aw00 + t10[2] * aw10 + t01[2] * aw01 + t11[2] * aw11) / asum;
        const int alpha256 = int((int64_t(asum >> 16) * ta) / 255);
        if (alpha256 > 0) blend_px(dst, (pr * tr) / 255, (pg * tg) / 255, (pb * tb) / 255, alpha256);
      }
    }
  }
};

// The icons a scene shows, in drawing order (profile groups by filter value).
struct Mark {
  std::string key;
  double x, z;
  bool is_new = false;
  bool completed = false;
  bool discovered = true;
  int marker_index = -1;
};

bool marker_matches_filter(const MapMarker& m, MapFilter f) {
  if (f == MapFilter::kAll) return true;
  switch (f) {
    case MapFilter::kEvents:
      return m.kind == MapMarker::kEvent || m.kind == MapMarker::kFestivalSite || m.kind == MapMarker::kH2hFinish;
    case MapFilter::kStreetRaces: return m.kind == MapMarker::kStreetHub || m.icon_tag == "streetrace";
    case MapFilter::kOutposts: return m.kind == MapMarker::kOutpost || m.kind == MapMarker::kFestivalSite;
    case MapFilter::kCollectibles: return false;
    case MapFilter::kPrStunts:
      return m.kind == MapMarker::kPrStunt || m.kind == MapMarker::kOutpost || m.kind == MapMarker::kSpeedTrap ||
             m.kind == MapMarker::kSpeedZone;
    default: return true;
  }
}

std::vector<Mark> marks(const MapScene& s, bool apply_filter) {
  std::vector<Mark> out;
  const bool show_collectibles = !apply_filter || s.filter == MapFilter::kAll || s.filter == MapFilter::kCollectibles;
  if (show_collectibles && s.data && s.progress) {
    for (const auto& sign : s.data->signs) {
      const bool found = s.progress->signs_found.count(sign.id) > 0;
      if (found) out.push_back({"flyer_collected", sign.at.x, sign.at.z, false, true, true, -1});
      else if (s.reveal) out.push_back({"flyer", sign.at.x, sign.at.z, false, false, true, -1});
    }
    for (size_t i = 0; i < s.data->barns.size(); ++i) {
      const auto& b = s.data->barns[i];
      const int status = i < s.progress->barns.size() ? s.progress->barns[i].status : freeroam::kBarnNotSpawned;
      if (status == freeroam::kBarnFound) out.push_back({"barnfind_collected", b.at.x, b.at.z, false, true, true, -1});
      else if (s.reveal) out.push_back({"barnfind", b.at.x, b.at.z, false, false, true, -1});
    }
  }
  for (size_t i = 0; i < s.markers.size(); ++i) {
    const auto& m = s.markers[i];
    if (apply_filter && !marker_matches_filter(m, s.filter)) continue;
    out.push_back({m.icon_tag, m.x, m.z, m.is_new, m.completed, m.discovered, int(i)});
  }
  return out;
}

}  // namespace

bool MapView::open(const std::filesystem::path& game_root, std::string* error, const std::string& lang) {
  const auto media = game_root / "media";
  traffic::NavMap nav;
  if (!traffic::read_nav(media / "tracks" / "colorado" / "colorado.nav", nav, error)) return false;
  traffic::Roads built;
  built.build(nav);
  roads_.clear();
  min_x_ = min_z_ = 1e30;
  max_x_ = max_z_ = -1e30;
  for (const auto& r : built.roads()) {
    const auto it = std::find(std::begin(kRoadTypes), std::end(kRoadTypes), r.type);
    if (it == std::end(kRoadTypes) || r.points.size() < 2) continue;  // none, shortcut: not drawn
    Road road;
    road.style = int(it - std::begin(kRoadTypes));
    road.nodes = r.nodes;
    road.min_x = road.min_z = 1e30;
    road.max_x = road.max_z = -1e30;
    road.points.reserve(r.points.size());
    for (const auto& p : r.points) {
      road.points.push_back({p.x, p.z});
      road.min_x = std::min(road.min_x, p.x), road.max_x = std::max(road.max_x, p.x);
      road.min_z = std::min(road.min_z, p.z), road.max_z = std::max(road.max_z, p.z);
      min_x_ = std::min(min_x_, p.x), max_x_ = std::max(max_x_, p.x);
      min_z_ = std::min(min_z_, p.z), max_z_ = std::max(max_z_, p.z);
    }
    roads_.push_back(std::move(road));
  }
  if (roads_.empty()) return fail(error, "colorado.nav has no drawn roads");
  nx_ = int((max_x_ - min_x_) / cell_) + 1;
  nz_ = int((max_z_ - min_z_) / cell_) + 1;
  grid_.assign(size_t(nx_) * nz_, {});
  for (uint32_t r = 0; r < roads_.size(); ++r)
    for (uint32_t s = 0; s + 1 < roads_[r].points.size(); ++s) {
      const auto& a = roads_[r].points[s];
      const auto& b = roads_[r].points[s + 1];
      const int cx0 = int((std::min(a[0], b[0]) - min_x_) / cell_), cx1 = int((std::max(a[0], b[0]) - min_x_) / cell_);
      const int cz0 = int((std::min(a[1], b[1]) - min_z_) / cell_), cz1 = int((std::max(a[1], b[1]) - min_z_) / cell_);
      for (int cz = cz0; cz <= cz1; ++cz)
        for (int cx = cx0; cx <= cx1; ++cx) grid_[size_t(cz) * nx_ + cx].push_back({r, s});
    }

  audio::XmlNode mini, full;
  if (!read_xml_file(media / "ui" / "MapProfileMinimap.xml", mini, error)) return false;
  if (!read_xml_file(media / "ui" / "MapProfileFullscreen.xml", full, error)) return false;
  mini_ = read_profile(mini);
  full_ = read_profile(full);

  std::string why;
  horizon_zip_path_ = media / "ui" / "textures" / "Horizon.zip";
  high_tile_state_.fill(0);
  for (auto& t : high_tiles_) t = Atlas{};
  base_cache_ = PauseBaseCache{};
  auto zip = fh1::Archive::open(horizon_zip_path_, &why);
  if (!zip) return fail(error, why);
  if (!read_texture(*zip, "Map/background/low-0-0.xds", background_, error)) return false;
  const std::string lcode = lang.empty() ? std::string("EN") : lang;
  const std::string loc_small = "Map/icons/MapIcons/" + lcode + "/MapIconSheetSmall.xds";
  const std::string loc_large = "Map/icons/MapIcons/" + lcode + "/MapIconSheet.xds";
  if ((lcode == "EN" || !read_texture(*zip, loc_small, small_icons_, nullptr)) &&
      !read_texture(*zip, "Map/icons/MapIcons/EN/MapIconSheetSmall.xds", small_icons_, error)) {
    return false;
  }
  if ((lcode == "EN" || !read_texture(*zip, loc_large, large_icons_, nullptr)) &&
      !read_texture(*zip, "Map/icons/MapIcons/EN/MapIconSheet.xds", large_icons_, error)) {
    return false;
  }
  return true;
}

int MapView::loaded_high_tiles() const {
  int n = 0;
  for (uint8_t s : high_tile_state_)
    if (s == 1) ++n;
  return n;
}

void MapView::ensure_high_tiles(int c0, int c1, int r0, int r1) const {
  if (horizon_zip_path_.empty()) return;
  c0 = std::clamp(c0, 0, kHighCols - 1);
  c1 = std::clamp(c1, 0, kHighCols - 1);
  r0 = std::clamp(r0, 0, kHighRows - 1);
  r1 = std::clamp(r1, 0, kHighRows - 1);
  bool need = false;
  for (int r = r0; r <= r1 && !need; ++r)
    for (int c = c0; c <= c1; ++c)
      if (high_tile_state_[size_t(r * kHighCols + c)] == 0) {
        need = true;
        break;
      }
  if (!need) return;
  auto zip = fh1::Archive::open(horizon_zip_path_, nullptr);
  if (!zip) return;
  for (int r = r0; r <= r1; ++r) {
    for (int c = c0; c <= c1; ++c) {
      const size_t idx = size_t(r * kHighCols + c);
      if (high_tile_state_[idx] != 0) continue;
      const std::string member = "Map/background/high-" + std::to_string(c) + "-" + std::to_string(r) + ".xds";
      if (read_texture(*zip, member, high_tiles_[idx], nullptr) &&
          high_tiles_[idx].width == uint32_t(kHighTileSize) &&
          high_tiles_[idx].height == uint32_t(kHighTileSize)) {
        high_tile_state_[idx] = 1;
      } else {
        high_tile_state_[idx] = 2;
      }
    }
  }
}

bool MapView::sample_high_pixel(int px, int py, uint8_t out_rgb[3]) const {
  px = std::clamp(px, 0, kHighCols * kHighTileSize - 1);
  py = std::clamp(py, 0, kHighRows * kHighTileSize - 1);
  const int c = px / kHighTileSize, r = py / kHighTileSize;
  const size_t idx = size_t(r * kHighCols + c);
  if (high_tile_state_[idx] != 1) return false;
  const int lx = px - c * kHighTileSize, ly = py - r * kHighTileSize;
  const uint8_t* p = &high_tiles_[idx].rgba[(size_t(ly) * kHighTileSize + lx) * 4];
  out_rgb[0] = p[0];
  out_rgb[1] = p[1];
  out_rgb[2] = p[2];
  return true;
}

HudImage MapView::minimap(const MapScene& scene, int size) const {
  // SPEC-UI-LAYOUTS a (out/oracle/ui/hud-freeroam.png): authentic frameless
  // transparent circular FH1 radar with crisp road ribbons, dark road outlines,
  // player arrow, and smooth outer edge fade (zero dark backdrop disc!).
  Canvas cv(size, size, {0, 0, 0, 0});
  cv.edge_fade = 0.34f;
  // Speed-responsive dynamic minimap zoom: pulls higher & further ahead as speed rises.
  const double map_heading = scene.north_up ? 0.0 : scene.heading;
  const double fx = std::sin(map_heading), fz = std::cos(map_heading);
  const double rx = fz, rz = -fx;  // right of the heading (+X when facing +Z)
  const double speed_t = std::clamp(std::fabs(scene.speed_mps) / 67.0, 0.0, 1.0);
  const double kHeight = 108.0 + 88.0 * speed_t;
  const double kBack = 54.0 + 42.0 * speed_t;
  constexpr double kS = 0.70710678118654752;
  const double focal = size, cxp = size / 2.0, cyp = size / 2.0;
  struct P {
    double x, y, depth;
  };
  auto project = [&](double x, double z) {
    const double dx = x - scene.x, dz = z - scene.z;
    const double a = dx * fx + dz * fz + kBack, c = dx * rx + dz * rz;
    const double depth = kS * (kHeight + a);
    if (depth < 1) return P{0, 0, -1};
    return P{cxp + focal * c / depth, cyp - focal * kS * (a - kHeight) / depth, depth};
  };
  const double reach = 420.0 + 300.0 * speed_t;  // m around the car that can show

  // Rumour circles (sort order 21, under the roads).
  if (scene.data && scene.progress)
    for (size_t i = 0; i < scene.data->barns.size() && i < scene.progress->barns.size(); ++i) {
      if (scene.progress->barns[i].status != freeroam::kBarnRumour) continue;
      const auto& b = scene.data->barns[i];
      if (std::hypot(b.hint_x - scene.x, b.hint_z - scene.z) > b.hint_rx + reach) continue;
      std::vector<std::array<double, 2>> poly;
      std::vector<P> ring;
      for (int k = 0; k < 96; ++k) {
        const double t = 2 * kPi * k / 96;
        // Clipped to the near side of the camera.
        double x = b.hint_x + b.hint_rx * std::cos(t), z = b.hint_z + b.hint_rz * std::sin(t);
        const double a = (x - scene.x) * fx + (z - scene.z) * fz;
        if (a < -kBack - kHeight + 20) {
          const double shift = (-kBack - kHeight + 20) - a;
          x += shift * fx, z += shift * fz;
        }
        const P p = project(x, z);
        ring.push_back(p);
        poly.push_back({p.x, p.y});
      }
      cv.polygon(poly, mini_.region_fill);
      for (size_t k = 0; k < ring.size(); ++k) {
        const P& p = ring[k];
        const P& q = ring[(k + 1) % ring.size()];
        cv.line(p.x, p.y, q.x, q.y, 3, mini_.region_rim);
      }
    }

  // Roads near the car, from the grid, each road_type in profile order:
  // under-lines first, then the lines with their core.
  std::vector<std::pair<uint32_t, uint32_t>> segs;
  {
    const int cx0 = std::max(0, int((scene.x - reach - min_x_) / cell_)), cx1 = std::min(nx_ - 1, int((scene.x + reach - min_x_) / cell_));
    const int cz0 = std::max(0, int((scene.z - reach - min_z_) / cell_)), cz1 = std::min(nz_ - 1, int((scene.z + reach - min_z_) / cell_));
    for (int cz = cz0; cz <= cz1; ++cz)
      for (int cx = cx0; cx <= cx1; ++cx)
        for (const auto& rs : grid_[size_t(cz) * nx_ + cx]) segs.push_back(rs);
    std::sort(segs.begin(), segs.end());
    segs.erase(std::unique(segs.begin(), segs.end()), segs.end());
  }
  // As the original's frames show (SPEC-UI-LAYOUTS a: roads white): wide
  // white roads with a faint shadow, each road_type's width by its
  // profile line size against the widest (the main road about 9 m, ours).
  double widest = 1;
  for (const auto& [k, ls] : mini_.roads) widest = std::max(widest, double(ls.line_size));
  for (int pass = 0; pass < 2; ++pass)
    for (int style = 0; style < kRoadTypeCount; ++style) {
      const auto st = mini_.roads.find(kRoadTypes[style]);
      if (st == mini_.roads.end()) continue;
      const LineStyle& ls = st->second;
      for (const auto& [r, s] : segs) {
        if (roads_[r].style != style) continue;
        const auto& a = roads_[r].points[s];
        const auto& b = roads_[r].points[s + 1];
        const P pa = project(a[0], a[1]), pb = project(b[0], b[1]);
        if (pa.depth < 1 || pb.depth < 1) continue;
        const double px = focal / ((pa.depth + pb.depth) / 2) * 9.0 * std::max(0.35, ls.line_size / widest);
        bool driven = true;
        if (scene.road_discovery && scene.road_discovery->ready() && s + 1 < roads_[r].nodes.size()) {
          driven = scene.road_discovery->is_node_discovered(roads_[r].nodes[s]) ||
                   scene.road_discovery->is_node_discovered(roads_[r].nodes[s + 1]);
        }
        const Rgba undriven_col{std::max(182.0f, ls.edge.r), std::max(186.0f, ls.edge.g),
                                std::max(194.0f, ls.edge.b), std::max(220.0f, ls.edge.a)};
        const Rgba road_col = driven ? ls.core : undriven_col;
        const float under_a = std::max(185.0f, ls.under.a);
        if (pass == 0) cv.line(pa.x, pa.y, pb.x, pb.y, std::max(3.0, px + 3.4), Rgba{10, 12, 16, under_a});
        if (pass == 1) cv.line(pa.x, pa.y, pb.x, pb.y, std::max(1.8, px), road_col);
      }
    }

  // A race's route, green over the roads (SPEC-UI-LAYOUTS a; the width and
  // shades ours, from the race frames).
  for (int pass = 0; pass < 2; ++pass)
    for (size_t i = 0; i + 1 < scene.route.size(); ++i) {
      const P pa = project(scene.route[i][0], scene.route[i][1]), pb = project(scene.route[i + 1][0], scene.route[i + 1][1]);
      if (pa.depth < 1 || pb.depth < 1) continue;
      const double wpx = focal / ((pa.depth + pb.depth) / 2) * 9.0;  // the main road's width
      if (pass == 0) cv.line(pa.x, pa.y, pb.x, pb.y, std::max(2.0, wpx + 1.5), Rgba{20, 60, 25, 140});
      else cv.line(pa.x, pa.y, pb.x, pb.y, std::max(1.5, wpx), Rgba{40, 128, 52, 235});
    }

  // Free-roam GPS route (MapProfileMinimap.xml group "route": r=74 g=238 b=97).
  if (!scene.gps_route.empty()) {
    for (int pass = 0; pass < 2; ++pass)
      for (size_t i = 0; i + 1 < scene.gps_route.size(); ++i) {
        const P pa = project(scene.gps_route[i][0], scene.gps_route[i][1]);
        const P pb = project(scene.gps_route[i + 1][0], scene.gps_route[i + 1][1]);
        if (pa.depth < 1 || pb.depth < 1) continue;
        const double wpx = focal / ((pa.depth + pb.depth) / 2) * 9.5;
        if (pass == 0) cv.line(pa.x, pa.y, pb.x, pb.y, std::max(2.5, wpx + 2.2), Rgba{14, 72, 24, 185});
        else cv.line(pa.x, pa.y, pb.x, pb.y, std::max(1.8, wpx), Rgba{74, 238, 97, 250});
      }
  }

  // Icons, standing up (a constant size on screen).
  const double unit = size / 300.0;
  if (scene.has_gps_dest && std::hypot(scene.gps_dest_x - scene.x, scene.gps_dest_z - scene.z) <= reach) {
    const P dp = project(scene.gps_dest_x, scene.gps_dest_z);
    if (dp.depth >= 1) {
      cv.icon(small_icons_, 2, 4, dp.x, dp.y - 20 * unit, 52 * unit, 0, Rgba{74, 238, 97, 255});
    }
  }
  for (const auto& m : marks(scene, false)) {
    if (std::hypot(m.x - scene.x, m.z - scene.z) > reach) continue;
    auto it = mini_.icons.find(m.key);
    if (it == mini_.icons.end()) it = mini_.icons.find("gas_station");
    if (it == mini_.icons.end()) continue;
    const P p = project(m.x, m.z);
    if (p.depth < 1) continue;
    double top_y = p.y;
    for (const auto& ic : it->second) {
      Rgba tint = ic.colour;
      if (!m.discovered) tint = {145, 145, 155, tint.a * 0.72f};
      else if (m.completed && m.marker_index >= 0) tint.a *= 0.6f;
      const double iy = p.y - ic.size * unit * 0.4;
      top_y = std::min(top_y, iy - ic.size * unit * 0.45);
      cv.icon(small_icons_, ic.cell_x, ic.cell_y, p.x, iy, ic.size * unit, 0, tint);
    }
    // Crisp FH1 pink "NEW" text badge above uncompleted events (SPEC-UI-LAYOUTS:69 — never a blank line!).
    if (m.is_new) {
      const double sc = std::max(0.85, unit * 1.15);
      const double bw = 20.0 * sc, bh = 8.5 * sc;
      const double by = top_y - bh * 0.65;
      cv.line(p.x - bw * 0.5, by, p.x + bw * 0.5, by, bh + 2.0, Rgba{12, 12, 16, 220});
      cv.line(p.x - bw * 0.5, by, p.x + bw * 0.5, by, bh, Rgba{249, 0, 100, 250});
      // Crisp micro-stroke "N E W" letters inside the pink badge.
      const double h2 = 2.4 * sc, gw = 3.2 * sc, st = std::max(1.0, 0.95 * sc);
      const Rgba wh{255, 255, 255, 255};
      // N
      const double nx = p.x - 6.6 * sc;
      cv.line(nx, by + h2, nx, by - h2, st, wh);
      cv.line(nx, by - h2, nx + gw, by + h2, st, wh);
      cv.line(nx + gw, by + h2, nx + gw, by - h2, st, wh);
      // E
      const double ex = p.x - 1.6 * sc;
      cv.line(ex, by - h2, ex, by + h2, st, wh);
      cv.line(ex, by - h2, ex + gw, by - h2, st, wh);
      cv.line(ex, by, ex + gw * 0.85, by, st, wh);
      cv.line(ex, by + h2, ex + gw, by + h2, st, wh);
      // W
      const double wx = p.x + 3.4 * sc;
      cv.line(wx, by - h2, wx + gw * 0.3, by + h2, st, wh);
      cv.line(wx + gw * 0.3, by + h2, wx + gw * 0.65, by - h2 * 0.2, st, wh);
      cv.line(wx + gw * 0.65, by - h2 * 0.2, wx + gw, by + h2, st, wh);
      cv.line(wx + gw, by + h2, wx + gw * 1.3, by - h2, st, wh);
    }
  }
  // Free-roam traffic vehicles: compact grey-white car dots on the minimap.
  for (const auto& t : scene.traffic) {
    if (std::hypot(t[0] - scene.x, t[1] - scene.z) > reach) continue;
    const P p = project(t[0], t[1]);
    if (p.depth < 1) continue;
    cv.line(p.x, p.y, p.x, p.y, 10 * unit + 1.5, Rgba{25, 28, 34, 210});
    cv.line(p.x, p.y, p.x, p.y, 8 * unit, Rgba{198, 204, 212, 235});
  }
  // Roaming Festival AI racers: distinct Horizon-pink discs with bright rim.
  for (const auto& f : scene.festival_ai) {
    if (std::hypot(f[0] - scene.x, f[1] - scene.z) > reach) continue;
    const P p = project(f[0], f[1]);
    if (p.depth < 1) continue;
    cv.line(p.x, p.y, p.x, p.y, 15 * unit + 2, Rgba{255, 255, 255, 240});
    cv.line(p.x, p.y, p.x, p.y, 15 * unit - 1.5, Rgba{245, 0, 96, 255});
  }
  // Opponents: white discs with a grey rim, as the race frames show.
  for (const auto& o : scene.opponents) {
    if (std::hypot(o[0] - scene.x, o[1] - scene.z) > reach) continue;
    const P p = project(o[0], o[1]);
    if (p.depth < 1) continue;
    cv.line(p.x, p.y, p.x, p.y, 15 * unit + 2, Rgba{120, 120, 125, 230});
    cv.line(p.x, p.y, p.x, p.y, 15 * unit - 1, Rgba{236, 236, 238, 255});
  }
  // The player: heading up in rotating mode, or compass-rotated in north-up mode.
  if (const auto it = mini_.icons.find("player_local"); it != mini_.icons.end()) {
    const P p = project(scene.x, scene.z);
    const double player_angle = scene.north_up ? scene.heading : 0.0;
    // About 35 px at 720 lines, as the frames show.
    for (const auto& ic : it->second)
      cv.icon(small_icons_, ic.cell_x, ic.cell_y, p.x, p.y, ic.size * unit * 1.8, player_angle, ic.colour);
  }
  // No frame (SPEC-UI-LAYOUTS a).
  return cv.image();
}

double MapView::map_scale(int width, int height, double zoom) const {
  zoom = std::max(zoom, 0.25);
  const double span_x = max_x_ > min_x_ ? (max_x_ - min_x_) : freeroam::kMapWidth;
  const double span_z = max_z_ > min_z_ ? (max_z_ - min_z_) : freeroam::kMapHeight;
  return std::max(span_x / std::max(1, width), span_z / std::max(1, height)) * 1.04 / zoom;
}

uint32_t MapView::high_tiles_mask() const {
  uint32_t mask = 0;
  for (size_t i = 0; i < high_tile_state_.size(); ++i)
    if (high_tile_state_[i] == 1) mask |= (1u << i);
  return mask;
}

void MapView::render_pause_base(std::vector<uint8_t>& rgba, int width, int height, double scale, double hi_blend,
                                double cx, double cz, const freeroam::RoadDiscovery* road_discovery) const {
  FastCanvas cv(width, height, rgba);
  const double half_w = width * 0.5, half_h = height * 0.5;
  const double inv_scale = 1.0 / scale;

  // Seamless 2-level map background pyramid using 16.16 fixed-point scanline stepping.
  if (!background_.rgba.empty()) {
    const int bw = int(background_.width), bh = int(background_.height);
    const int hw = kHighCols * kHighTileSize, hh = kHighRows * kHighTileSize;
    const double un0 = (cx + (0.5 - half_w) * scale - freeroam::kMapWest) / freeroam::kMapWidth;
    const double dun = scale / freeroam::kMapWidth;
    const double u0 = un0 * bw - 0.5, du = dun * bw;
    const double hu0 = un0 * hw - 0.5, dhu = dun * hw;
    const int x_lo = std::clamp(int(std::ceil((-0.5 - u0) / du)), 0, width);
    const int x_hi = std::clamp(int(std::floor((bw - 0.5 - u0) / du)) + 1, 0, width);
    const int64_t u_start_fp = int64_t(std::llround((u0 + x_lo * du) * 65536.0));
    const int64_t du_fp = int64_t(std::llround(du * 65536.0));
    const int64_t u_max_fp = int64_t(bw - 1) << 16;
    const int64_t hu_start_fp = int64_t(std::llround((hu0 + x_lo * dhu) * 65536.0));
    const int64_t dhu_fp = int64_t(std::llround(dhu * 65536.0));
    const int64_t hu_max_fp = int64_t(hw - 1) << 16;
    const int kb256 = int(std::lround(std::clamp(hi_blend, 0.0, 1.0) * 256.0));
    const int ikb256 = 256 - kb256;
    const bool use_lo_only = kb256 <= 10;
    const bool use_hi_only = kb256 >= 246;

    auto render_rows = [&](int y_begin, int y_end) {
      for (int y = y_begin; y < y_end; ++y) {
        const double z = cz - (y + 0.5 - half_h) * scale;
        const double vn = (freeroam::kMapNorth - z) / freeroam::kMapHeight;
        const double v = vn * bh - 0.5;
        if (v < -0.5 || v > bh - 0.5 || x_lo >= x_hi) {
          cv.fill_rect_row(y, 0, width, 12, 12, 14);
          continue;
        }
        if (x_lo > 0) cv.fill_rect_row(y, 0, x_lo, 12, 12, 14);
        if (x_hi < width) cv.fill_rect_row(y, x_hi, width, 12, 12, 14);

        const int iy = std::clamp(int(std::floor(v)), 0, bh - 1), iy1 = std::min(bh - 1, iy + 1);
        const int fy = int(std::clamp(v - iy, 0.0, 1.0) * 256.0 + 0.5), ify = 256 - fy;
        const uint8_t* lo_row0 = &background_.rgba[size_t(iy) * bw * 4];
        const uint8_t* lo_row1 = &background_.rgba[size_t(iy1) * bw * 4];
        uint8_t* dst = &rgba[(size_t(y) * width + x_lo) * 4];

        if (use_lo_only) {
          int64_t u_fp = u_start_fp;
          for (int x = x_lo; x < x_hi; ++x, u_fp += du_fp, dst += 4) {
            const int64_t uc = std::clamp<int64_t>(u_fp, 0, u_max_fp);
            const int ix = int(uc >> 16), ix1 = std::min(bw - 1, ix + 1);
            const int fx = int((uc >> 8) & 255), ifx = 256 - fx;
            const uint8_t* a = lo_row0 + ix * 4;
            const uint8_t* b = lo_row0 + ix1 * 4;
            const uint8_t* c = lo_row1 + ix * 4;
            const uint8_t* d = lo_row1 + ix1 * 4;
            const int w00 = ifx * ify, w10 = fx * ify, w01 = ifx * fy, w11 = fx * fy;
            dst[0] = uint8_t((a[0] * w00 + b[0] * w10 + c[0] * w01 + d[0] * w11 + 32768) >> 16);
            dst[1] = uint8_t((a[1] * w00 + b[1] * w10 + c[1] * w01 + d[1] * w11 + 32768) >> 16);
            dst[2] = uint8_t((a[2] * w00 + b[2] * w10 + c[2] * w01 + d[2] * w11 + 32768) >> 16);
            dst[3] = 255;
          }
        } else {
          const double hv = vn * hh - 0.5;
          const int hiy = std::clamp(int(std::floor(hv)), 0, hh - 1), hiy1 = std::min(hh - 1, hiy + 1);
          const int hfy = int(std::clamp(hv - hiy, 0.0, 1.0) * 256.0 + 0.5), ihfy = 256 - hfy;
          const int tr0 = hiy >> 10, tr1 = hiy1 >> 10;
          const int ly0 = hiy & 1023, ly1 = hiy1 & 1023;
          const uint8_t* hi_row0[kHighCols];
          const uint8_t* hi_row1[kHighCols];
          for (int tc = 0; tc < kHighCols; ++tc) {
            const size_t i0 = size_t(tr0 * kHighCols + tc);
            const size_t i1 = size_t(tr1 * kHighCols + tc);
            hi_row0[tc] = (high_tile_state_[i0] == 1) ? &high_tiles_[i0].rgba[size_t(ly0) * kHighTileSize * 4] : nullptr;
            hi_row1[tc] = (high_tile_state_[i1] == 1) ? &high_tiles_[i1].rgba[size_t(ly1) * kHighTileSize * 4] : nullptr;
          }
          int64_t u_fp = u_start_fp;
          int64_t hu_fp = hu_start_fp;
          for (int x = x_lo; x < x_hi; ++x, u_fp += du_fp, hu_fp += dhu_fp, dst += 4) {
            const int64_t huc = std::clamp<int64_t>(hu_fp, 0, hu_max_fp);
            const int hix = int(huc >> 16), hix1 = std::min(hw - 1, hix + 1);
            const int tc0 = hix >> 10, tc1 = hix1 >> 10;
            const uint8_t* r0a = hi_row0[tc0];
            const uint8_t* r0b = hi_row0[tc1];
            const uint8_t* r1a = hi_row1[tc0];
            const uint8_t* r1b = hi_row1[tc1];
            const bool got_hi = (r0a && r0b && r1a && r1b);
            if (got_hi && use_hi_only) {
              const int hfx = int((huc >> 8) & 255), ihfx = 256 - hfx;
              const uint8_t* ha = r0a + (hix & 1023) * 4;
              const uint8_t* hb = r0b + (hix1 & 1023) * 4;
              const uint8_t* hc = r1a + (hix & 1023) * 4;
              const uint8_t* hd = r1b + (hix1 & 1023) * 4;
              const int w00 = ihfx * ihfy, w10 = hfx * ihfy, w01 = ihfx * hfy, w11 = hfx * hfy;
              dst[0] = uint8_t((ha[0] * w00 + hb[0] * w10 + hc[0] * w01 + hd[0] * w11 + 32768) >> 16);
              dst[1] = uint8_t((ha[1] * w00 + hb[1] * w10 + hc[1] * w01 + hd[1] * w11 + 32768) >> 16);
              dst[2] = uint8_t((ha[2] * w00 + hb[2] * w10 + hc[2] * w01 + hd[2] * w11 + 32768) >> 16);
              dst[3] = 255;
              continue;
            }
            const int64_t uc = std::clamp<int64_t>(u_fp, 0, u_max_fp);
            const int ix = int(uc >> 16), ix1 = std::min(bw - 1, ix + 1);
            const int fx = int((uc >> 8) & 255), ifx = 256 - fx;
            const uint8_t* a = lo_row0 + ix * 4;
            const uint8_t* b = lo_row0 + ix1 * 4;
            const uint8_t* c = lo_row1 + ix * 4;
            const uint8_t* d = lo_row1 + ix1 * 4;
            const int lw00 = ifx * ify, lw10 = fx * ify, lw01 = ifx * fy, lw11 = fx * fy;
            const int lr = (a[0] * lw00 + b[0] * lw10 + c[0] * lw01 + d[0] * lw11 + 32768) >> 16;
            const int lg = (a[1] * lw00 + b[1] * lw10 + c[1] * lw01 + d[1] * lw11 + 32768) >> 16;
            const int lb = (a[2] * lw00 + b[2] * lw10 + c[2] * lw01 + d[2] * lw11 + 32768) >> 16;
            if (got_hi) {
              const int hfx = int((huc >> 8) & 255), ihfx = 256 - hfx;
              const uint8_t* ha = r0a + (hix & 1023) * 4;
              const uint8_t* hb = r0b + (hix1 & 1023) * 4;
              const uint8_t* hc = r1a + (hix & 1023) * 4;
              const uint8_t* hd = r1b + (hix1 & 1023) * 4;
              const int hw00 = ihfx * ihfy, hw10 = hfx * ihfy, hw01 = ihfx * hfy, hw11 = hfx * hfy;
              const int hr = (ha[0] * hw00 + hb[0] * hw10 + hc[0] * hw01 + hd[0] * hw11 + 32768) >> 16;
              const int hg = (ha[1] * hw00 + hb[1] * hw10 + hc[1] * hw01 + hd[1] * hw11 + 32768) >> 16;
              const int hb_c = (ha[2] * hw00 + hb[2] * hw10 + hc[2] * hw01 + hd[2] * hw11 + 32768) >> 16;
              dst[0] = uint8_t((lr * ikb256 + hr * kb256 + 128) >> 8);
              dst[1] = uint8_t((lg * ikb256 + hg * kb256 + 128) >> 8);
              dst[2] = uint8_t((lb * ikb256 + hb_c * kb256 + 128) >> 8);
            } else {
              dst[0] = uint8_t(lr);
              dst[1] = uint8_t(lg);
              dst[2] = uint8_t(lb);
            }
            dst[3] = 255;
          }
        }
      }
    };

    render_rows(0, height);
  } else {
    for (int y = 0; y < height; ++y) cv.fill_rect_row(y, 0, width, 12, 12, 14);
  }

  // World-space viewport AABB for fast per-road culling.
  const double pad_m = 12.0 * scale;
  const double view_min_x = cx - half_w * scale - pad_m;
  const double view_max_x = cx + half_w * scale + pad_m;
  const double view_min_z = cz - half_h * scale - pad_m;
  const double view_max_z = cz + half_h * scale + pad_m;
  const bool has_disc = road_discovery && road_discovery->ready();

  // Roads: AABB-culled, tight-band capsule rasterized with sub-pixel vertex decimation.
  for (int pass = 0; pass < 2; ++pass) {
    for (int style = 0; style < kRoadTypeCount; ++style) {
      const auto st = full_.roads.find(kRoadTypes[style]);
      if (st == full_.roads.end()) continue;
      const LineStyle& ls = st->second;
      const double line = std::clamp(ls.line_size * 0.9 * inv_scale, 1.2, 7.0);
      const Rgba driven_col{std::max(238.0f, ls.core.r), std::max(232.0f, ls.core.g), std::max(210.0f, ls.core.b),
                            252.0f};
      const Rgba undriven_col{106, 110, 120, 236};
      const Rgba outline_col{14, 16, 20, 155};
      for (const auto& road : roads_) {
        if (road.style != style) continue;
        if (road.max_x < view_min_x || road.min_x > view_max_x || road.max_z < view_min_z || road.min_z > view_max_z)
          continue;
        double px = half_w + (road.points[0][0] - cx) * inv_scale;
        double py = half_h - (road.points[0][1] - cz) * inv_scale;
        size_t prev_k = 0;
        const size_t npts = road.points.size();
        for (size_t k = 1; k < npts; ++k) {
          const double curx = half_w + (road.points[k][0] - cx) * inv_scale;
          const double cury = half_h - (road.points[k][1] - cz) * inv_scale;
          // Skip sub-pixel intermediate vertices when zoomed out to avoid redundant capsule draws.
          if (k + 1 < npts) {
            const double ddx = curx - px, ddy = cury - py;
            if (ddx * ddx + ddy * ddy < 2.25) continue;
          }
          const bool off = (px < -8 && curx < -8) || (px > width + 8 && curx > width + 8) ||
                           (py < -8 && cury < -8) || (py > height + 8 && cury > height + 8);
          if (!off) {
            if (pass == 0) {
              cv.line(px, py, curx, cury, line + 1.8, outline_col);
            } else {
              bool driven = true;
              if (has_disc && k < road.nodes.size()) {
                driven = road_discovery->is_node_discovered(road.nodes[prev_k]) ||
                         road_discovery->is_node_discovered(road.nodes[k]);
              }
              cv.line(px, py, curx, cury, line, driven ? driven_col : undriven_col);
            }
          }
          px = curx;
          py = cury;
          prev_k = k;
        }
      }
    }
  }
}

HudImage MapView::pause_map(const MapScene& scene, int width, int height, double zoom, double cx, double cz) const {
  zoom = std::max(zoom, 0.25);
  // Metres per pixel: zoom 1 fits the road network.
  const double scale = map_scale(width, height, zoom);
  const double hi_blend = std::clamp((zoom - 1.8) / 1.7, 0.0, 1.0);

  const int disc_count = (scene.road_discovery && scene.road_discovery->ready())
                             ? scene.road_discovery->discovered_count()
                             : -1;
  const bool same_scale =
      base_cache_.width == width && base_cache_.height == height && std::fabs(scale - base_cache_.scale) <= scale * 1e-6;
  // When panning at steady zoom, cache a +256 px padded base map (background + roads)
  // so subsequent pan frames copy the viewport in ~0.4 ms instead of re-rasterizing.
  // Single-shot or active-zoom frames render exact width x height (pad = 0).
  const int want_pad = same_scale ? std::min(256, std::max(width, height) / 2) : 0;

  if (hi_blend > 0.0) {
    const double hw_m = (width * 0.5 + want_pad) * scale;
    const double hh_m = (height * 0.5 + want_pad) * scale;
    const double u0 = (cx - hw_m - freeroam::kMapWest) / freeroam::kMapWidth;
    const double u1 = (cx + hw_m - freeroam::kMapWest) / freeroam::kMapWidth;
    const double v0 = (freeroam::kMapNorth - (cz + hh_m)) / freeroam::kMapHeight;
    const double v1 = (freeroam::kMapNorth - (cz - hh_m)) / freeroam::kMapHeight;
    ensure_high_tiles(int(std::floor(u0 * kHighCols)) - 1, int(std::floor(u1 * kHighCols)) + 1,
                      int(std::floor(v0 * kHighRows)) - 1, int(std::floor(v1 * kHighRows)) + 1);
  }
  const uint32_t tmask = high_tiles_mask();

  HudImage out;
  out.width = uint32_t(width);
  out.height = uint32_t(height);
  out.rgba.resize(size_t(width) * height * 4);

  double eff_cx = cx, eff_cz = cz;
  bool cache_hit = false;
  if (same_scale && !base_cache_.rgba.empty() && base_cache_.road_disc == scene.road_discovery &&
      base_cache_.road_disc_count == disc_count && base_cache_.loaded_tiles_mask == tmask) {
    const int ox = int(std::lround(base_cache_.pad + (cx - base_cache_.cx) / scale));
    const int oy = int(std::lround(base_cache_.pad - (cz - base_cache_.cz) / scale));
    if (ox >= 0 && ox <= 2 * base_cache_.pad && oy >= 0 && oy <= 2 * base_cache_.pad) {
      const int cw = base_cache_.width + 2 * base_cache_.pad;
      const size_t row_bytes = size_t(width) * 4;
      for (int y = 0; y < height; ++y) {
        std::memcpy(&out.rgba[size_t(y) * row_bytes],
                    &base_cache_.rgba[(size_t(oy + y) * cw + ox) * 4], row_bytes);
      }
      eff_cx = base_cache_.cx + (ox - base_cache_.pad) * scale;
      eff_cz = base_cache_.cz - (oy - base_cache_.pad) * scale;
      cache_hit = true;
    }
  }

  if (!cache_hit) {
    const int cw = width + 2 * want_pad;
    const int ch = height + 2 * want_pad;
    base_cache_.width = width;
    base_cache_.height = height;
    base_cache_.pad = want_pad;
    base_cache_.scale = scale;
    base_cache_.cx = cx;
    base_cache_.cz = cz;
    base_cache_.road_disc = scene.road_discovery;
    base_cache_.road_disc_count = disc_count;
    base_cache_.loaded_tiles_mask = tmask;
    if (want_pad == 0) {
      render_pause_base(out.rgba, width, height, scale, hi_blend, cx, cz, scene.road_discovery);
      base_cache_.rgba.clear();
    } else {
      render_pause_base(base_cache_.rgba, cw, ch, scale, hi_blend, cx, cz, scene.road_discovery);
      const size_t row_bytes = size_t(width) * 4;
      for (int y = 0; y < height; ++y) {
        std::memcpy(&out.rgba[size_t(y) * row_bytes],
                    &base_cache_.rgba[(size_t(want_pad + y) * cw + want_pad) * 4], row_bytes);
      }
    }
  }

  FastCanvas cv(width, height, out.rgba);
  const double inv_scale = 1.0 / scale;
  auto to_screen = [&](double x, double z) {
    return std::array<double, 2>{width * 0.5 + (x - eff_cx) * inv_scale, height * 0.5 - (z - eff_cz) * inv_scale};
  };

  // Free-roam GPS route line on fullscreen map (MapProfileFullscreen.xml "route": r=74 g=238 b=97).
  const auto& active_route = !scene.gps_route.empty() ? scene.gps_route : scene.route;
  if (active_route.size() >= 2) {
    const double rw = std::clamp(14.0 * inv_scale, 3.0, 9.0);
    for (int pass = 0; pass < 2; ++pass) {
      auto prev = to_screen(active_route[0][0], active_route[0][1]);
      for (size_t i = 1; i < active_route.size(); ++i) {
        const auto cur = to_screen(active_route[i][0], active_route[i][1]);
        const bool off = (prev[0] < -12 && cur[0] < -12) || (prev[0] > width + 12 && cur[0] > width + 12) ||
                         (prev[1] < -12 && cur[1] < -12) || (prev[1] > height + 12 && cur[1] > height + 12);
        if (!off) {
          if (pass == 0) cv.line(prev[0], prev[1], cur[0], cur[1], rw + 3.0, Rgba{12, 62, 22, 210});
          else cv.line(prev[0], prev[1], cur[0], cur[1], rw, Rgba{74, 238, 97, 255});
        }
        prev = cur;
      }
    }
  }

  // Rumour circles: the region's outline over the map, filled faintly.
  if ((scene.filter == MapFilter::kAll || scene.filter == MapFilter::kCollectibles) && scene.data && scene.progress)
    for (size_t i = 0; i < scene.data->barns.size() && i < scene.progress->barns.size(); ++i) {
      if (scene.progress->barns[i].status != freeroam::kBarnRumour) continue;
      const auto& b = scene.data->barns[i];
      std::vector<std::array<double, 2>> poly;
      poly.reserve(96);
      for (int k = 0; k < 96; ++k) {
        const double t = 2 * kPi * k / 96;
        poly.push_back(to_screen(b.hint_x + b.hint_rx * std::cos(t), b.hint_z + b.hint_rz * std::sin(t)));
      }
      cv.polygon(poly, Rgba{255, 255, 255, 60});
      for (size_t k = 0; k < poly.size(); ++k) {
        const auto& p = poly[k];
        const auto& q = poly[(k + 1) % poly.size()];
        cv.line(p[0], p[1], q[0], q[1], 2, Rgba{255, 255, 255, 220});
      }
    }

  // Icons lying flat; profile sizes in our pixels, growing a little with
  // the zoom (frustum-culled to viewport).
  const double unit = std::min(2.0, 0.8 + zoom * 0.2) * std::max(1.0, height / 900.0);
  const double icon_pad = 96.0 * unit;
  for (const auto& m : marks(scene, true)) {
    const auto p = to_screen(m.x, m.z);
    if (p[0] < -icon_pad || p[0] > width + icon_pad || p[1] < -icon_pad || p[1] > height + icon_pad) continue;
    auto it = full_.icons.find(m.key);
    if (it == full_.icons.end()) it = full_.icons.find("gas_station");
    if (it == full_.icons.end()) continue;
    const bool sel = m.marker_index >= 0 && m.marker_index == scene.selected_marker;
    if (sel) {
      // Bright pink halo around hovered/selected map marker.
      const double r = 24.0 * unit;
      for (int k = 0; k < 24; ++k) {
        const double a0 = 2 * kPi * k / 24, a1 = 2 * kPi * (k + 1) / 24;
        cv.line(p[0] + r * std::cos(a0), p[1] + r * std::sin(a0), p[0] + r * std::cos(a1), p[1] + r * std::sin(a1), 3.0,
                Rgba{245, 4, 92, 255});
      }
    }
    for (const auto& ic : it->second) {
      Rgba tint = ic.colour;
      if (!m.discovered) tint = {140, 145, 155, tint.a * 0.7f};
      else if (m.completed && m.marker_index >= 0) tint.a *= 0.65f;
      const double sz = ic.size * unit * (sel ? 1.25 : 1.0);
      cv.icon(large_icons_, ic.cell_x, ic.cell_y, p[0], p[1], sz, 0, tint);
    }
    if (m.is_new) {
      const double oy = 18 * unit;
      cv.line(p[0] - 8 * unit, p[1] - oy, p[0] + 8 * unit, p[1] - oy, 4.5 * unit, Rgba{245, 4, 92, 250});
    }
  }
  // GPS destination marker on fullscreen map (Atlas 2,4 in bright green).
  if (scene.has_gps_dest) {
    const auto dp = to_screen(scene.gps_dest_x, scene.gps_dest_z);
    if (dp[0] >= -icon_pad && dp[0] <= width + icon_pad && dp[1] >= -icon_pad && dp[1] <= height + icon_pad) {
      const double r = 18.0 * unit;
      for (int k = 0; k < 24; ++k) {
        const double a0 = 2 * kPi * k / 24, a1 = 2 * kPi * (k + 1) / 24;
        cv.line(dp[0] + r * std::cos(a0), dp[1] + r * std::sin(a0), dp[0] + r * std::cos(a1), dp[1] + r * std::sin(a1),
                3.0, Rgba{74, 238, 97, 255});
      }
      cv.icon(large_icons_, 2, 4, dp[0], dp[1] - 10.0 * unit, 52.0 * unit, 0, Rgba{74, 238, 97, 255});
    }
  }
  for (const auto& f : scene.festival_ai) {
    const auto p = to_screen(f[0], f[1]);
    if (p[0] < -20 || p[0] > width + 20 || p[1] < -20 || p[1] > height + 20) continue;
    cv.line(p[0], p[1], p[0], p[1], 13, Rgba{255, 255, 255, 235});
    cv.line(p[0], p[1], p[0], p[1], 9, Rgba{245, 0, 96, 255});
  }
  for (const auto& o : scene.opponents) {
    const auto p = to_screen(o[0], o[1]);
    if (p[0] < -20 || p[0] > width + 20 || p[1] < -20 || p[1] > height + 20) continue;
    cv.line(p[0], p[1], p[0], p[1], 12, Rgba{20, 20, 20, 220});
    cv.line(p[0], p[1], p[0], p[1], 9, Rgba{230, 60, 50, 255});
  }
  // The player's arrow, turned with the heading (clockwise from north).
  if (const auto it = full_.icons.find("player_local"); it != full_.icons.end()) {
    const auto p = to_screen(scene.x, scene.z);
    for (const auto& ic : it->second) {
      if ((ic.cell_x == 2 || ic.cell_x == 3) && ic.cell_y == 3)
        cv.icon(large_icons_, ic.cell_x, ic.cell_y, p[0], p[1], std::max(28.0, ic.size * unit), scene.heading, ic.colour);
    }
  }
  return out;
}

void MapCameraController::reset(double cx, double cz, double zoom) {
  cx_ = cx;
  cz_ = cz;
  vx_ = 0.0;
  vz_ = 0.0;
  zoom_ = target_zoom_ = std::clamp(zoom, 1.0, 24.0);
  snapped_marker_ = -1;
  clamp_bounds();
}

void MapCameraController::nudge_target_zoom(double factor) {
  target_zoom_ = std::clamp(target_zoom_ * factor, 1.0, 24.0);
}

void MapCameraController::pan_direct(double dx, double dz) {
  cx_ += dx;
  cz_ += dz;
  vx_ = 0.0;
  vz_ = 0.0;
  clamp_bounds();
}

void MapCameraController::clamp_bounds() {
  const double west = freeroam::kMapWest;
  const double east = freeroam::kMapWest + freeroam::kMapWidth;
  const double south = freeroam::kMapNorth - freeroam::kMapHeight;
  const double north = freeroam::kMapNorth;
  cx_ = std::clamp(cx_, west, east);
  cz_ = std::clamp(cz_, south, north);
}

bool MapCameraController::step(double dt, double pan_x, double pan_z, double zoom_rate,
                               const std::vector<MapMarker>& visible_markers) {
  dt = std::clamp(dt, 0.0005, 0.05);
  const double prev_cx = cx_, prev_cz = cz_, prev_zoom = zoom_;

  // Smooth continuous zoom (analog triggers / held +/-) + exponential interpolation to target_zoom.
  if (std::fabs(zoom_rate) > 1e-3) {
    target_zoom_ = std::clamp(target_zoom_ * std::pow(2.8, zoom_rate * dt), 1.0, 24.0);
  }
  const double zoom_alpha = 1.0 - std::exp(-14.0 * dt);
  if (std::fabs(target_zoom_ - zoom_) > 1e-4) {
    zoom_ *= std::pow(target_zoom_ / zoom_, zoom_alpha);
    zoom_ = std::clamp(zoom_, 1.0, 24.0);
  } else {
    zoom_ = target_zoom_;
  }

  // Clamp diagonal input vector to unit circle so diagonal movement matches cardinal speed.
  double ix = std::clamp(pan_x, -1.0, 1.0);
  double iz = std::clamp(pan_z, -1.0, 1.0);
  const double mag = std::hypot(ix, iz);
  if (mag > 1.0) ix /= mag, iz /= mag;

  // Smooth dt-integrated velocity with crisp acceleration and exponential braking.
  const double max_speed = 4200.0 / std::max(0.5, zoom_);
  const double target_vx = ix * max_speed;
  const double target_vz = iz * max_speed;
  const double vel_rate = (mag > 0.02) ? 14.0 : 18.0;
  const double vel_alpha = 1.0 - std::exp(-vel_rate * dt);
  vx_ += (target_vx - vx_) * vel_alpha;
  vz_ += (target_vz - vz_) * vel_alpha;
  if (mag <= 0.02 && std::hypot(vx_, vz_) < 4.0 / std::max(0.5, zoom_)) {
    vx_ = vz_ = 0.0;
  }

  cx_ += vx_ * dt;
  cz_ += vz_ * dt;

  // Gentle magnetic snap to nearest visible marker when cursor settles near an icon.
  snapped_marker_ = -1;
  const double snap_r = 250.0 / std::max(0.5, zoom_);
  double best_d = snap_r;
  for (size_t i = 0; i < visible_markers.size(); ++i) {
    const double d = std::hypot(visible_markers[i].x - cx_, visible_markers[i].z - cz_);
    if (d < best_d) {
      best_d = d;
      snapped_marker_ = int(i);
    }
  }
  if (snapped_marker_ >= 0 && mag < 0.05 && std::hypot(vx_, vz_) < 220.0 / std::max(0.5, zoom_)) {
    const auto& m = visible_markers[size_t(snapped_marker_)];
    const double pull = 1.0 - std::exp(-10.0 * dt);
    cx_ += (m.x - cx_) * pull;
    cz_ += (m.z - cz_) * pull;
  }

  clamp_bounds();
  const double pos_eps = 0.25 / std::max(0.5, zoom_);
  return std::hypot(cx_ - prev_cx, cz_ - prev_cz) > pos_eps || std::fabs(zoom_ - prev_zoom) > 1e-4;
}

}  // namespace carview
