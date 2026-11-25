#include "map_view.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

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
        const std::string stype = sym.attr("type");
        if (stype.find("_new") != std::string::npos) continue;  // dynamic "NEW" badge is drawn separately via m.is_new
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
  void blit(const MapView::Atlas& img, double cx, double cy, double draw_w, double draw_h, const MapView::Rgba& tint) {
    if (img.rgba.empty() || img.width == 0 || img.height == 0 || draw_w < 1.0 || draw_h < 1.0) return;
    const int x0 = std::max(0, int(std::floor(cx - draw_w * 0.5)));
    const int x1 = std::min(w - 1, int(std::ceil(cx + draw_w * 0.5)));
    const int y0 = std::max(0, int(std::floor(cy - draw_h * 0.5)));
    const int y1 = std::min(h - 1, int(std::ceil(cy + draw_h * 0.5)));
    for (int y = y0; y <= y1; ++y) {
      const double v = (y + 0.5 - (cy - draw_h * 0.5)) / draw_h;
      if (v < 0.0 || v >= 1.0) continue;
      const double ty = std::clamp(v * img.height - 0.5, 0.0, double(img.height - 1));
      const int iy = int(ty), iy1 = std::min(int(img.height) - 1, iy + 1);
      const double fy = ty - iy;
      for (int x = x0; x <= x1; ++x) {
        const double u = (x + 0.5 - (cx - draw_w * 0.5)) / draw_w;
        if (u < 0.0 || u >= 1.0) continue;
        const double tx = std::clamp(u * img.width - 0.5, 0.0, double(img.width - 1));
        const int ix = int(tx), ix1 = std::min(int(img.width) - 1, ix + 1);
        const double fx = tx - ix;
        float px4[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k) {
          const int sx = (k & 1) ? ix1 : ix, sy = (k >> 1) ? iy1 : iy;
          const double wgt = ((k & 1) ? fx : 1.0 - fx) * ((k >> 1) ? fy : 1.0 - fy);
          const uint8_t* t = &img.rgba[(size_t(sy) * img.width + sx) * 4];
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
      const int tr_scale = ((tr * ta + 128) * 257) >> 16;
      const int tg_scale = ((tg * ta + 128) * 257) >> 16;
      const int tb_scale = ((tb * ta + 128) * 257) >> 16;
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
          const uint64_t a00 = t00[3], a10 = t10[3], a01 = t01[3], a11 = t11[3];
          if ((a00 | a10 | a01 | a11) == 0) continue;
          const uint64_t fx = uint64_t((txc >> 8) & 255), ifx = 256 - fx;
          const uint64_t aw00 = ifx * uint64_t(ify) * a00;
          const uint64_t aw10 = fx * uint64_t(ify) * a10;
          const uint64_t aw01 = ifx * uint64_t(fy) * a01;
          const uint64_t aw11 = fx * uint64_t(fy) * a11;
          const uint32_t asum = uint32_t((aw00 + aw10 + aw01 + aw11) >> 16);  // 0..255
          if (asum <= 1) continue;
          // 64-bit premultiplied RGB sum (max 255 * 65536 * 255 = 4.26e9 > INT32_MAX — never overflows!).
          const int pr = int((((uint64_t(t00[0]) * aw00 + uint64_t(t10[0]) * aw10 +
                                uint64_t(t01[0]) * aw01 + uint64_t(t11[0]) * aw11) >> 16) + 128ULL) * 257ULL >> 16);
          const int pg = int((((uint64_t(t00[1]) * aw00 + uint64_t(t10[1]) * aw10 +
                                uint64_t(t01[1]) * aw01 + uint64_t(t11[1]) * aw11) >> 16) + 128ULL) * 257ULL >> 16);
          const int pb = int((((uint64_t(t00[2]) * aw00 + uint64_t(t10[2]) * aw10 +
                                uint64_t(t01[2]) * aw01 + uint64_t(t11[2]) * aw11) >> 16) + 128ULL) * 257ULL >> 16);
          const int eff_a = ((int(asum) * ta + 128) * 257) >> 16;
          const int inv_a = 255 - eff_a;
          dst[0] = uint8_t(std::clamp(((int(dst[0]) * inv_a + pr * tr_scale + 128) * 257) >> 16, 0, 255));
          dst[1] = uint8_t(std::clamp(((int(dst[1]) * inv_a + pg * tg_scale + 128) * 257) >> 16, 0, 255));
          dst[2] = uint8_t(std::clamp(((int(dst[2]) * inv_a + pb * tb_scale + 128) * 257) >> 16, 0, 255));
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
        const uint64_t a00 = t00[3], a10 = t10[3], a01 = t01[3], a11 = t11[3];
        if ((a00 | a10 | a01 | a11) == 0) continue;
        const uint64_t aw00 = uint64_t(ifx) * uint64_t(ify) * a00;
        const uint64_t aw10 = uint64_t(fx) * uint64_t(ify) * a10;
        const uint64_t aw01 = uint64_t(ifx) * uint64_t(fy) * a01;
        const uint64_t aw11 = uint64_t(fx) * uint64_t(fy) * a11;
        const uint64_t asum = aw00 + aw10 + aw01 + aw11;
        if (asum <= 65536ULL) continue;
        const int pr = int(std::min<uint64_t>(255ULL, (uint64_t(t00[0]) * aw00 + uint64_t(t10[0]) * aw10 +
                                                       uint64_t(t01[0]) * aw01 + uint64_t(t11[0]) * aw11) / asum));
        const int pg = int(std::min<uint64_t>(255ULL, (uint64_t(t00[1]) * aw00 + uint64_t(t10[1]) * aw10 +
                                                       uint64_t(t01[1]) * aw01 + uint64_t(t11[1]) * aw11) / asum));
        const int pb = int(std::min<uint64_t>(255ULL, (uint64_t(t00[2]) * aw00 + uint64_t(t10[2]) * aw10 +
                                                       uint64_t(t01[2]) * aw01 + uint64_t(t11[2]) * aw11) / asum));
        const int alpha256 = int(((asum >> 16) * uint64_t(ta)) / 255ULL);
        if (alpha256 > 0) blend_px(dst, (pr * tr) / 255, (pg * tg) / 255, (pb * tb) / 255, alpha256);
      }
    }
  }
  void blit(const MapView::Atlas& img, double cx, double cy, double draw_w, double draw_h, const MapView::Rgba& tint) {
    if (img.rgba.empty() || img.width == 0 || img.height == 0 || draw_w < 1.0 || draw_h < 1.0) return;
    if (cx + draw_w * 0.5 < 0.0 || cx - draw_w * 0.5 >= w || cy + draw_h * 0.5 < 0.0 || cy - draw_h * 0.5 >= h) return;
    const int x0 = std::max(0, int(std::floor(cx - draw_w * 0.5)));
    const int x1 = std::min(w - 1, int(std::ceil(cx + draw_w * 0.5)));
    const int y0 = std::max(0, int(std::floor(cy - draw_h * 0.5)));
    const int y1 = std::min(h - 1, int(std::ceil(cy + draw_h * 0.5)));
    const int aw = int(img.width), ah = int(img.height);
    const int tr = int(std::clamp(tint.r, 0.0f, 255.0f) + 0.5f);
    const int tg = int(std::clamp(tint.g, 0.0f, 255.0f) + 0.5f);
    const int tb = int(std::clamp(tint.b, 0.0f, 255.0f) + 0.5f);
    const int ta = int(std::clamp(tint.a, 0.0f, 255.0f) + 0.5f);
    if (ta <= 0) return;
    const double ox = cx - draw_w * 0.5, oy = cy - draw_h * 0.5;
    for (int y = y0; y <= y1; ++y) {
      const double v = (y + 0.5 - oy) / draw_h;
      if (v < 0.0 || v >= 1.0) continue;
      const double ty = std::clamp(v * ah - 0.5, 0.0, double(ah - 1));
      const int iy = int(ty), iy1 = std::min(ah - 1, iy + 1);
      const int fy = int((ty - iy) * 256.0 + 0.5), ify = 256 - fy;
      uint8_t* dst = &rgba[(size_t(y) * w + x0) * 4];
      for (int x = x0; x <= x1; ++x, dst += 4) {
        const double u = (x + 0.5 - ox) / draw_w;
        if (u < 0.0 || u >= 1.0) continue;
        const double tx = std::clamp(u * aw - 0.5, 0.0, double(aw - 1));
        const int ix = int(tx), ix1 = std::min(aw - 1, ix + 1);
        const int fx = int((tx - ix) * 256.0 + 0.5), ifx = 256 - fx;
        const uint8_t* t00 = &img.rgba[(size_t(iy) * aw + ix) * 4];
        const uint8_t* t10 = &img.rgba[(size_t(iy) * aw + ix1) * 4];
        const uint8_t* t01 = &img.rgba[(size_t(iy1) * aw + ix) * 4];
        const uint8_t* t11 = &img.rgba[(size_t(iy1) * aw + ix1) * 4];
        const uint64_t a00 = t00[3], a10 = t10[3], a01 = t01[3], a11 = t11[3];
        if ((a00 | a10 | a01 | a11) == 0) continue;
        const uint64_t aw00 = uint64_t(ifx) * uint64_t(ify) * a00;
        const uint64_t aw10 = uint64_t(fx) * uint64_t(ify) * a10;
        const uint64_t aw01 = uint64_t(ifx) * uint64_t(fy) * a01;
        const uint64_t aw11 = uint64_t(fx) * uint64_t(fy) * a11;
        const uint64_t asum = aw00 + aw10 + aw01 + aw11;
        if (asum <= 65536ULL) continue;
        const int pr = int(std::min<uint64_t>(255ULL, (uint64_t(t00[0]) * aw00 + uint64_t(t10[0]) * aw10 +
                                                       uint64_t(t01[0]) * aw01 + uint64_t(t11[0]) * aw11) / asum));
        const int pg = int(std::min<uint64_t>(255ULL, (uint64_t(t00[1]) * aw00 + uint64_t(t10[1]) * aw10 +
                                                       uint64_t(t01[1]) * aw01 + uint64_t(t11[1]) * aw11) / asum));
        const int pb = int(std::min<uint64_t>(255ULL, (uint64_t(t00[2]) * aw00 + uint64_t(t10[2]) * aw10 +
                                                       uint64_t(t01[2]) * aw01 + uint64_t(t11[2]) * aw11) / asum));
        const int alpha256 = int(((asum >> 16) * uint64_t(ta)) / 255ULL);
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

std::vector<Mark> marks(const MapScene& s, bool apply_filter) {
  std::vector<Mark> out;
  const bool show_signs = !apply_filter || s.filter.shows(MapCategory::kDiscountSign);
  const bool show_barns = !apply_filter || s.filter.shows(MapCategory::kBarnFind);
  if (s.data && s.progress) {
    if (show_signs) {
      for (const auto& sign : s.data->signs) {
        const bool found = s.progress->signs_found.count(sign.id) > 0;
        if (found) out.push_back({"flyer_collected", sign.at.x, sign.at.z, false, true, true, -1});
        else if (s.reveal) out.push_back({"flyer", sign.at.x, sign.at.z, false, false, true, -1});
      }
    }
    if (show_barns) {
      for (size_t i = 0; i < s.data->barns.size(); ++i) {
        const auto& b = s.data->barns[i];
        const int status = i < s.progress->barns.size() ? s.progress->barns[i].status : freeroam::kBarnNotSpawned;
        if (status == freeroam::kBarnFound) out.push_back({"barnfind_collected", b.at.x, b.at.z, false, true, true, -1});
        else if (s.reveal) out.push_back({"barnfind", b.at.x, b.at.z, false, false, true, -1});
      }
    }
  }
  for (size_t i = 0; i < s.markers.size(); ++i) {
    const auto& m = s.markers[i];
    if (apply_filter && !map_marker_matches_filter(m, s.filter)) continue;
    out.push_back({m.icon_tag, m.x, m.z, m.is_new, m.completed, m.discovered, int(i)});
  }
  return out;
}

}  // namespace

// A separable box blur of `passes` passes (about a Gaussian), radius r px.
static void box_blur_rgba(std::vector<uint8_t>& px, int w, int h, int r, int passes) {
  if (w <= 0 || h <= 0 || r <= 0 || px.size() < size_t(w) * h * 4) return;
  std::vector<uint8_t> tmp(px.size());
  const int n = 2 * r + 1;
  auto pass = [&](const std::vector<uint8_t>& src, std::vector<uint8_t>& dst, bool horiz) {
    const int lines = horiz ? h : w, len = horiz ? w : h;
    for (int li = 0; li < lines; ++li) {
      auto at = [&](int i) -> size_t {
        i = std::clamp(i, 0, len - 1);
        return horiz ? (size_t(li) * w + i) * 4 : (size_t(i) * w + li) * 4;
      };
      int acc[4] = {0, 0, 0, 0};
      for (int i = -r; i <= r; ++i)
        for (int c = 0; c < 4; ++c) acc[c] += src[at(i) + c];
      for (int i = 0; i < len; ++i) {
        const size_t o = at(i);
        for (int c = 0; c < 4; ++c) dst[o + c] = uint8_t(acc[c] / n);
        const size_t add = at(i + r + 1), sub = at(i - r);
        for (int c = 0; c < 4; ++c) acc[c] += int(src[add + c]) - int(src[sub + c]);
      }
    }
  };
  for (int k = 0; k < passes; ++k) {
    pass(px, tmp, true);
    pass(tmp, px, false);
  }
}

MapCategory map_marker_category(const MapMarker& m) {
  switch (m.kind) {
    case MapMarker::kEvent:
      if (m.icon_tag == "exhibition") return MapCategory::kShowcase;
      if (m.icon_tag == "nemesisrace") return MapCategory::kStarShowdown;
      return MapCategory::kFestivalEvent;
    case MapMarker::kStreetHub: return MapCategory::kStreetRaceHub;
    case MapMarker::kOutpost: return MapCategory::kOutpost;
    case MapMarker::kSpeedTrap:
    case MapMarker::kSpeedZone: return MapCategory::kSpeedCamera;
    default: return MapCategory::kAlways;
  }
}

bool map_marker_matches_filter(const MapMarker& m, const MapFilterSet& f) {
  const MapCategory c = map_marker_category(m);
  const bool event = m.kind == MapMarker::kEvent || m.kind == MapMarker::kStreetHub;
  return f.shows(c, m.is_new, event && !m.completed, false);
}

MapView::MapView() = default;
MapView::~MapView() = default;
MapView::MapView(MapView&&) noexcept = default;
MapView& MapView::operator=(MapView&&) noexcept = default;

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
  horizon_zip_ = std::shared_ptr<fh1::Archive>(std::move(zip));
  if (!read_texture(*horizon_zip_, "Map/background/low-0-0.xds", background_, error)) return false;
  find_art_bounds();
  const std::string lcode = lang.empty() ? std::string("EN") : lang;
  const std::string loc_small = "Map/icons/MapIcons/" + lcode + "/MapIconSheetSmall.xds";
  const std::string loc_large = "Map/icons/MapIcons/" + lcode + "/MapIconSheet.xds";
  if ((lcode == "EN" || !read_texture(*horizon_zip_, loc_small, small_icons_, nullptr)) &&
      !read_texture(*horizon_zip_, "Map/icons/MapIcons/EN/MapIconSheetSmall.xds", small_icons_, error)) {
    return false;
  }
  if ((lcode == "EN" || !read_texture(*horizon_zip_, loc_large, large_icons_, nullptr)) &&
      !read_texture(*horizon_zip_, "Map/icons/MapIcons/EN/MapIconSheet.xds", large_icons_, error)) {
    return false;
  }
  // Optional retail New.xds badge for uncompleted events (falls back to crisp vector "NEW" badge if absent).
  new_badge_ = Atlas{};
  const std::string loc_new = "Map/icons/MapIcons/" + lcode + "/New.xds";
  if (lcode == "EN" || !read_texture(*horizon_zip_, loc_new, new_badge_, nullptr)) {
    if (!read_texture(*horizon_zip_, "Map/icons/MapIcons/EN/New.xds", new_badge_, nullptr)) {
      if (!read_texture(*horizon_zip_, "Map/icons/New.xds", new_badge_, nullptr)) {
        read_texture(*horizon_zip_, "Icons/TicketStickerNew.xds", new_badge_, nullptr);
      }
    }
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
  std::vector<std::pair<int, int>> missing;
  for (int r = r0; r <= r1; ++r)
    for (int c = c0; c <= c1; ++c)
      if (high_tile_state_[size_t(r * kHighCols + c)] == 0) missing.push_back({c, r});
  if (missing.empty()) return;
  if (!horizon_zip_) {
    auto opened = fh1::Archive::open(horizon_zip_path_, nullptr);
    if (!opened) return;
    horizon_zip_ = std::shared_ptr<fh1::Archive>(std::move(opened));
  }
  auto decode_tile = [&](int c, int r) {
    const size_t idx = size_t(r * kHighCols + c);
    const std::string member = "Map/background/high-" + std::to_string(c) + "-" + std::to_string(r) + ".xds";
    Atlas decoded;
    if (read_texture(*horizon_zip_, member, decoded, nullptr) &&
        decoded.width == uint32_t(kHighTileSize) &&
        decoded.height == uint32_t(kHighTileSize)) {
      high_tiles_[idx] = std::move(decoded);
      high_tile_state_[idx] = 1;
    } else {
      high_tile_state_[idx] = 2;
    }
  };
  if (missing.size() == 1) {
    decode_tile(missing[0].first, missing[0].second);
    return;
  }
  const size_t n_workers = std::min<size_t>(missing.size(), std::max<size_t>(1, std::min<unsigned>(4u, std::thread::hardware_concurrency())));
  if (n_workers <= 1) {
    for (const auto& [c, r] : missing) decode_tile(c, r);
    return;
  }
  std::vector<std::thread> workers;
  workers.reserve(n_workers - 1);
  for (size_t wi = 0; wi + 1 < n_workers; ++wi) {
    workers.emplace_back([&, wi, n_workers]() {
      for (size_t i = wi; i < missing.size(); i += n_workers) decode_tile(missing[i].first, missing[i].second);
    });
  }
  for (size_t i = n_workers - 1; i < missing.size(); i += n_workers) decode_tile(missing[i].first, missing[i].second);
  for (auto& th : workers) th.join();
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
  // A fixed camera behind and above the car (whether the original's moves
  // with speed is open, Q89): the player's arrow about 100 px down a 180 px panel
  // (y 520 at 720 lines in 04_roam_hud_day_idle_orig.png), about 160 m
  // ahead to the top and 45 m behind to the bottom.
  const double map_heading = scene.north_up ? 0.0 : scene.heading;
  const double fx = std::sin(map_heading), fz = std::cos(map_heading);
  const double rx = fz, rz = -fx;  // right of the heading (+X when facing +Z)
  constexpr double kHeight = 108.0;
  constexpr double kBack = 54.0;
  constexpr double kS = 0.70710678118654752;
  const double focal = size * (150.0 / 180.0), cxp = size / 2.0, cyp = size * (50.0 / 180.0);
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
  const double reach = 420.0;  // m around the car that can show

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
  // Helper: subdivides long world segments into <= 28 m sub-segments and clips
  // each piece against the camera near plane so long straight highway segments
  // crossing behind the car are never dropped and perspective width tapers smoothly.
  auto draw_projected_segment = [&](double ax, double az, double bx, double bz, double world_width, double pad_px,
                                    double min_px, const Rgba& col) {
    const double seg_len = std::hypot(bx - ax, bz - az);
    const int steps = std::clamp(int(std::ceil(seg_len / 28.0)), 1, 24);
    const double inv_steps = 1.0 / double(steps);
    constexpr double kNearDepth = 10.0;
    for (int k = 0; k < steps; ++k) {
      const double t0 = double(k) * inv_steps;
      const double t1 = double(k + 1) * inv_steps;
      double x0 = ax + (bx - ax) * t0, z0 = az + (bz - az) * t0;
      double x1 = ax + (bx - ax) * t1, z1 = az + (bz - az) * t1;
      const double d0 = kS * (kHeight + (x0 - scene.x) * fx + (z0 - scene.z) * fz + kBack);
      const double d1 = kS * (kHeight + (x1 - scene.x) * fx + (z1 - scene.z) * fz + kBack);
      if (d0 < kNearDepth && d1 < kNearDepth) continue;
      if (d0 < kNearDepth) {
        const double ct = (kNearDepth - d0) / std::max(1e-6, d1 - d0);
        x0 += (x1 - x0) * ct;
        z0 += (z1 - z0) * ct;
      } else if (d1 < kNearDepth) {
        const double ct = (kNearDepth - d1) / std::max(1e-6, d0 - d1);
        x1 += (x0 - x1) * ct;
        z1 += (z0 - z1) * ct;
      }
      const P pa = project(x0, z0);
      const P pb = project(x1, z1);
      if (pa.depth < 1.0 || pb.depth < 1.0) continue;
      if ((pa.x < -32 && pb.x < -32) || (pa.x > size + 32 && pb.x > size + 32) ||
          (pa.y < -32 && pb.y < -32) || (pa.y > size + 32 && pb.y > size + 32)) {
        continue;
      }
      const double wpx = focal / (0.5 * (pa.depth + pb.depth)) * world_width;
      cv.line(pa.x, pa.y, pb.x, pb.y, std::max(min_px, wpx + pad_px), col);
    }
  };

  // Wide crisp road ribbons with dark outline casing (SPEC-UI-LAYOUTS a), wide
  // enough that the centered GPS/race route always leaves visible grey/white
  // road shoulders and dark casing on both sides.
  double widest = 1;
  for (const auto& [k, ls] : mini_.roads) widest = std::max(widest, double(ls.line_size));
  for (int pass = 0; pass < 2; ++pass)
    for (int style = 0; style < kRoadTypeCount; ++style) {
      const auto st = mini_.roads.find(kRoadTypes[style]);
      if (st == mini_.roads.end()) continue;
      const LineStyle& ls = st->second;
      const double road_world_w = 12.6 * std::max(0.55, ls.line_size / widest);
      for (const auto& [r, s] : segs) {
        if (roads_[r].style != style) continue;
        const auto& a = roads_[r].points[s];
        const auto& b = roads_[r].points[s + 1];
        // The profile's under-line (line_fade_lr, black at its own alpha)
        // and line: white roads with a soft edge, every road alike
        // (04_roam_hud_day_idle_orig.png).
        if (pass == 0) {
          draw_projected_segment(a[0], a[1], b[0], b[1], road_world_w, 2.4, 3.0, ls.under);
        } else {
          draw_projected_segment(a[0], a[1], b[0], b[1], road_world_w, 0.0, 2.0,
                                 Rgba{ls.core.r, ls.core.g, ls.core.b, 252.0f});
        }
      }
    }

  // A race's route, green nested inside the road ribbon so road shoulders remain visible.
  for (int pass = 0; pass < 2; ++pass)
    for (size_t i = 0; i + 1 < scene.route.size(); ++i) {
      if (pass == 0) {
        draw_projected_segment(scene.route[i][0], scene.route[i][1], scene.route[i + 1][0], scene.route[i + 1][1],
                               6.2, 1.6, 2.2, Rgba{20, 60, 25, 180});
      } else {
        draw_projected_segment(scene.route[i][0], scene.route[i][1], scene.route[i + 1][0], scene.route[i + 1][1],
                               6.2, 0.0, 1.6, Rgba{46, 152, 62, 242});
      }
    }

  // Free-roam GPS route (MapProfileMinimap.xml group "route": r=74 g=238 b=97),
  // centered cleanly inside the wider road ribbon (~46% width) so both road
  // shoulders and dark road outlines remain crisp underneath.
  if (!scene.gps_route.empty()) {
    for (int pass = 0; pass < 2; ++pass)
      for (size_t i = 0; i + 1 < scene.gps_route.size(); ++i) {
        if (pass == 0) {
          draw_projected_segment(scene.gps_route[i][0], scene.gps_route[i][1],
                                 scene.gps_route[i + 1][0], scene.gps_route[i + 1][1],
                                 5.8, 1.8, 2.4, Rgba{14, 72, 24, 205});
        } else {
          draw_projected_segment(scene.gps_route[i][0], scene.gps_route[i][1],
                                 scene.gps_route[i + 1][0], scene.gps_route[i + 1][1],
                                 5.8, 0.0, 1.6, Rgba{74, 238, 97, 255});
        }
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
    // Retail New.xds badge (or crisp FH1 pink "NEW" lettered badge fallback — never a blank line!).
    if (m.is_new) {
      const double sc = std::max(0.85, unit * 1.15);
      if (!new_badge_.rgba.empty() && new_badge_.height > 0) {
        const double bh = 12.0 * sc;
        const double aspect = std::clamp(double(new_badge_.width) / double(new_badge_.height), 0.75, 4.0);
        cv.blit(new_badge_, p.x, top_y - bh * 0.55, bh * aspect, bh, Rgba{255, 255, 255, 255});
      } else {
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
  }
  // Traffic and the Festival's roaming drivers: the profile's vehicle
  // icon (MapProfileMinimap.xml vehicle_festival_traffic and vehicle_ai,
  // cell 5,3 at size 45), untinted.
  auto vehicle = [&](double x, double z) {
    if (std::hypot(x - scene.x, z - scene.z) > reach) return;
    const P p = project(x, z);
    if (p.depth < 1) return;
    cv.icon(small_icons_, 5, 3, p.x, p.y, 45.0 * unit, 0, Rgba{255, 255, 255, 255});
  };
  for (const auto& t : scene.traffic) vehicle(t[0], t[1]);
  for (const auto& f : scene.festival_ai) vehicle(f[0], f[1]);
  // Opponents: white discs with a grey rim, as the race frames show.
  for (const auto& o : scene.opponents) {
    if (std::hypot(o[0] - scene.x, o[1] - scene.z) > reach) continue;
    const P p = project(o[0], o[1]);
    if (p.depth < 1) continue;
    cv.line(p.x, p.y, p.x, p.y, 15 * unit + 2, Rgba{120, 120, 125, 230});
    cv.line(p.x, p.y, p.x, p.y, 15 * unit - 1, Rgba{236, 236, 238, 255});
  }
  // R3-UI-12: Race leader highlight dot (distinct gold disc with Horizon-pink outer rim).
  if (scene.has_race_leader && std::hypot(scene.race_leader[0] - scene.x, scene.race_leader[1] - scene.z) <= reach) {
    const P lp = project(scene.race_leader[0], scene.race_leader[1]);
    if (lp.depth >= 1) {
      cv.line(lp.x, lp.y, lp.x, lp.y, 20 * unit + 2.5, Rgba{14, 16, 22, 230});
      cv.line(lp.x, lp.y, lp.x, lp.y, 18 * unit + 1.5, Rgba{249, 0, 100, 255});
      cv.line(lp.x, lp.y, lp.x, lp.y, 13 * unit, Rgba{255, 214, 48, 255});
    }
  }
  // R3-UI-12: Next race checkpoint beacon (bright cyan/pink concentric ring, rim-clamped if ahead).
  if (scene.has_next_checkpoint) {
    const P cp = project(scene.next_checkpoint[0], scene.next_checkpoint[1]);
    double bx = cp.x, by = cp.y;
    const double max_r = size * 0.39;
    bool on_screen = (cp.depth >= 1.0) && (std::hypot(bx - cxp, by - cyp) <= max_r);
    if (!on_screen) {
      const double dx = scene.next_checkpoint[0] - scene.x;
      const double dz = scene.next_checkpoint[1] - scene.z;
      const double sx = dx * rx + dz * rz;
      const double sy = -(dx * fx + dz * fz);
      const double sl = std::hypot(sx, sy);
      if (sl > 1e-4) {
        bx = cxp + (sx / sl) * max_r;
        by = cyp + (sy / sl) * max_r;
        on_screen = true;
      }
    }
    if (on_screen) {
      const double r = std::max(5.5, 11.5 * unit);
      for (int k = 0; k < 16; ++k) {
        const double a0 = 2.0 * kPi * k / 16.0;
        const double a1 = 2.0 * kPi * (k + 1) / 16.0;
        cv.line(bx + r * std::cos(a0), by + r * std::sin(a0), bx + r * std::cos(a1), by + r * std::sin(a1),
                std::max(2.6, 5.2 * unit), Rgba{12, 14, 20, 225});
        cv.line(bx + r * std::cos(a0), by + r * std::sin(a0), bx + r * std::cos(a1), by + r * std::sin(a1),
                std::max(1.8, 3.2 * unit), Rgba{249, 0, 100, 250});
      }
      cv.line(bx, by, bx, by, std::max(3.8, 7.6 * unit), Rgba{56, 224, 255, 250});
    }
  }
  // R3-UI-12: Checkered race finish flag marker.
  if (scene.has_finish && std::hypot(scene.finish_pos[0] - scene.x, scene.finish_pos[1] - scene.z) <= reach * 1.4) {
    const P fp = project(scene.finish_pos[0], scene.finish_pos[1]);
    if (fp.depth >= 1.0 && fp.x >= -16 && fp.x <= size + 16 && fp.y >= -16 && fp.y <= size + 16) {
      const double cell = std::max(2.8, 4.8 * unit);
      const double fw = cell * 3.0, fh = cell * 2.0;
      const double fx0 = fp.x - fw * 0.5, fy0 = fp.y - fh * 0.5 - 4.0 * unit;
      // Dark outer casing + Horizon pink top header trim.
      cv.line(fp.x - fw * 0.5 - 1.5, fy0 + fh * 0.5, fp.x + fw * 0.5 + 1.5, fy0 + fh * 0.5, fh + 4.5,
              Rgba{14, 16, 22, 235});
      cv.line(fx0, fy0 - 1.5, fx0 + fw, fy0 - 1.5, std::max(1.8, 2.4 * unit), Rgba{249, 0, 100, 250});
      for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
          const bool light = ((row + col) % 2) == 0;
          const double x0 = fx0 + col * cell, y0 = fy0 + row * cell;
          const std::vector<std::array<double, 2>> sq = {
              {x0, y0}, {x0 + cell, y0}, {x0 + cell, y0 + cell}, {x0, y0 + cell}};
          cv.polygon(sq, light ? Rgba{250, 250, 252, 255} : Rgba{18, 20, 24, 255});
        }
      }
    }
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
  HudImage img = cv.image();

  // No compass: the original's minimap turns with the car and shows no "N".
  return img;
}

std::array<int, 2> MapView::pause_internal_size(int width, int height) {
  width = std::max(1, width);
  height = std::max(1, height);
  constexpr int kMaxInternalW = 1600;
  constexpr int kMaxInternalH = 900;
  if (width <= kMaxInternalW && height <= kMaxInternalH) return {width, height};
  const double down = std::max(double(width) / double(kMaxInternalW), double(height) / double(kMaxInternalH));
  const int rw = std::max(64, int(std::lround(double(width) / down)));
  const int rh = std::max(64, int(std::lround(double(height) / down)));
  return {rw, rh};
}

void MapView::find_art_bounds() {
  art_west_ = freeroam::kMapWest, art_east_ = freeroam::kMapWest + freeroam::kMapWidth;
  art_south_ = freeroam::kMapNorth - freeroam::kMapHeight, art_north_ = freeroam::kMapNorth;
  const int w = int(background_.width), h = int(background_.height);
  if (w < 8 || h < 8 || background_.rgba.size() < size_t(w) * h * 4) return;
  // A row or column is art when a few percent of it is brighter than black.
  std::vector<int> col(w, 0), row(h, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint8_t* p = &background_.rgba[(size_t(y) * w + x) * 4];
      if (std::max({p[0], p[1], p[2]}) > 40) ++col[x], ++row[y];
    }
  int x0 = 0, x1 = w - 1, y0 = 0, y1 = h - 1;
  while (x0 < x1 && col[x0] * 50 < h) ++x0;
  while (x1 > x0 && col[x1] * 50 < h) --x1;
  while (y0 < y1 && row[y0] * 50 < w) ++y0;
  while (y1 > y0 && row[y1] * 50 < w) --y1;
  if (x1 - x0 < w / 2 || y1 - y0 < h / 2) return;  // not what was expected: keep the whole texture
  art_west_ = freeroam::kMapWest + freeroam::kMapWidth * double(x0 + 1) / w;
  art_east_ = freeroam::kMapWest + freeroam::kMapWidth * double(x1) / w;
  art_north_ = freeroam::kMapNorth - freeroam::kMapHeight * double(y0 + 1) / h;
  art_south_ = freeroam::kMapNorth - freeroam::kMapHeight * double(y1) / h;
}

double MapView::min_zoom(int width, int height) const {
  width = std::max(1, width);
  height = std::max(1, height);
  const double span_x = max_x_ > min_x_ ? (max_x_ - min_x_) : freeroam::kMapWidth;
  const double span_z = max_z_ > min_z_ ? (max_z_ - min_z_) : freeroam::kMapHeight;
  const double base_scale = std::max(span_x / double(width), span_z / double(height)) * 1.04;
  // Cap maximum metres/pixel so the view fits inside the painted part of the
  // background at any aspect ratio (no black borders).
  const double max_scale = std::min((art_east_ - art_west_) / double(width),
                                    (art_north_ - art_south_) / double(height));
  return std::max(1.0, base_scale / std::max(1e-6, max_scale));
}

double MapView::map_scale(int width, int height, double zoom) const {
  width = std::max(1, width);
  height = std::max(1, height);
  const double eff_zoom = std::max(zoom, min_zoom(width, height));
  const double span_x = max_x_ > min_x_ ? (max_x_ - min_x_) : freeroam::kMapWidth;
  const double span_z = max_z_ > min_z_ ? (max_z_ - min_z_) : freeroam::kMapHeight;
  const double raw = std::max(span_x / double(width), span_z / double(height)) * 1.04 / eff_zoom;
  const double max_scale = std::min((art_east_ - art_west_) / double(width),
                                    (art_north_ - art_south_) / double(height));
  return std::min(raw, max_scale);
}

void MapView::clamp_camera(int width, int height, double zoom, double& cx, double& cz) const {
  width = std::max(1, width);
  height = std::max(1, height);
  const double scale = map_scale(width, height, zoom);
  const double half_w_m = 0.5 * double(width) * scale;
  const double half_h_m = 0.5 * double(height) * scale;
  const double west = art_west_, east = art_east_, south = art_south_, north = art_north_;
  const double min_cx = west + half_w_m;
  const double max_cx = east - half_w_m;
  const double min_cz = south + half_h_m;
  const double max_cz = north - half_h_m;
  cx = (min_cx <= max_cx) ? std::clamp(cx, min_cx, max_cx) : 0.5 * (west + east);
  cz = (min_cz <= max_cz) ? std::clamp(cz, min_cz, max_cz) : 0.5 * (south + north);
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

  // Seamless 2-level map background pyramid using 16.16 fixed-point scanline stepping,
  // parallelized across horizontal stripes for < 1 ms rendering.
  if (!background_.rgba.empty()) {
    const int bw = int(background_.width), bh = int(background_.height);
    const int hw = kHighCols * kHighTileSize, hh = kHighRows * kHighTileSize;
    const double un0 = (cx + (0.5 - half_w) * scale - freeroam::kMapWest) / freeroam::kMapWidth;
    const double dun = scale / freeroam::kMapWidth;
    const double u0 = un0 * bw - 0.5, du = dun * bw;
    const double hu0 = un0 * hw - 0.5, dhu = dun * hw;
    const int64_t u_start_fp = int64_t(std::llround(u0 * 65536.0));
    const int64_t du_fp = int64_t(std::llround(du * 65536.0));
    const int64_t u_max_fp = int64_t(bw - 1) << 16;
    const int64_t hu_start_fp = int64_t(std::llround(hu0 * 65536.0));
    const int64_t dhu_fp = int64_t(std::llround(dhu * 65536.0));
    const int64_t hu_max_fp = int64_t(hw - 1) << 16;
    const int kb256 = int(std::lround(std::clamp(hi_blend, 0.0, 1.0) * 256.0));
    const int ikb256 = 256 - kb256;
    const bool use_lo_only = kb256 <= 10;
    const bool use_hi_only = kb256 >= 246;

    auto render_rows = [&](int y_begin, int y_end) {
      for (int y = y_begin; y < y_end; ++y) {
        const double z = cz - (y + 0.5 - half_h) * scale;
        const double vn = std::clamp((freeroam::kMapNorth - z) / freeroam::kMapHeight, 0.0, 1.0);
        const double v = std::clamp(vn * bh - 0.5, 0.0, double(bh - 1));
        const int iy = std::clamp(int(std::floor(v)), 0, bh - 1), iy1 = std::min(bh - 1, iy + 1);
        const int fy = int(std::clamp(v - iy, 0.0, 1.0) * 256.0 + 0.5), ify = 256 - fy;
        const uint8_t* lo_row0 = &background_.rgba[size_t(iy) * bw * 4];
        const uint8_t* lo_row1 = &background_.rgba[size_t(iy1) * bw * 4];
        uint8_t* dst = &rgba[size_t(y) * width * 4];

        if (use_lo_only) {
          int64_t u_fp = u_start_fp;
          for (int x = 0; x < width; ++x, u_fp += du_fp, dst += 4) {
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
          const double hv = std::clamp(vn * hh - 0.5, 0.0, double(hh - 1));
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
          for (int x = 0; x < width; ++x, u_fp += du_fp, hu_fp += dhu_fp, dst += 4) {
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

    const unsigned n_threads = (size_t(width) * size_t(height) >= 250000u)
                                   ? std::min(8u, std::max(1u, std::thread::hardware_concurrency()))
                                   : 1u;
    if (n_threads <= 1u) {
      render_rows(0, height);
    } else {
      std::vector<std::thread> workers;
      workers.reserve(n_threads - 1u);
      const int stripe = (height + int(n_threads) - 1) / int(n_threads);
      for (unsigned t = 0; t + 1u < n_threads; ++t) {
        const int yb = int(t) * stripe;
        const int ye = std::min(height, yb + stripe);
        if (yb < ye) workers.emplace_back(render_rows, yb, ye);
      }
      const int last_yb = int(n_threads - 1u) * stripe;
      if (last_yb < height) render_rows(last_yb, height);
      for (auto& th : workers) th.join();
    }
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
      const double line = std::clamp(ls.line_size * 0.9 * inv_scale, 1.2, 5.5);
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
          // Skip sub-pixel intermediate vertices when zoomed out to avoid redundant capsule draws,
          // while preserving visited/unvisited boundaries so newly driven road segments stay exact.
          if (k + 1 < npts) {
            const double ddx = curx - px, ddy = cury - py;
            const bool same_vis = !has_disc || k >= road.nodes.size() ||
                                  (road_discovery->is_node_discovered(road.nodes[k]) ==
                                   road_discovery->is_node_discovered(road.nodes[prev_k]));
            if (same_vis && ddx * ddx + ddy * ddy < 2.25) continue;
          }
          const bool off = (px < -8 && curx < -8) || (px > width + 8 && curx > width + 8) ||
                           (py < -8 && cury < -8) || (py > height + 8 && cury > height + 8);
          if (!off) {
            if (pass == 0) {
              cv.line(px, py, curx, cury, line + 1.8, outline_col);
            } else {
              bool driven = true;
              if (has_disc && k < road.nodes.size()) {
                driven = road_discovery->is_node_discovered(road.nodes[prev_k]) &&
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

void MapView::upscale_pause_rgba(const std::vector<uint8_t>& src, int sw, int sh,
                                 std::vector<uint8_t>& dst, int dw, int dh) const {
  if (upscale_lut_.src_w != sw || upscale_lut_.dst_w != dw) {
    upscale_lut_.src_w = sw;
    upscale_lut_.dst_w = dw;
    upscale_lut_.x0.resize(size_t(dw));
    upscale_lut_.x1.resize(size_t(dw));
    upscale_lut_.fx.resize(size_t(dw));
    const double step_x = double(sw) / double(dw);
    for (int x = 0; x < dw; ++x) {
      const double sx = std::clamp((x + 0.5) * step_x - 0.5, 0.0, double(sw - 1));
      const int ix0 = int(std::floor(sx));
      upscale_lut_.x0[size_t(x)] = ix0;
      upscale_lut_.x1[size_t(x)] = std::min(sw - 1, ix0 + 1);
      upscale_lut_.fx[size_t(x)] = uint16_t(std::clamp(int(std::lround((sx - ix0) * 256.0)), 0, 256));
    }
  }
  const int* lx0 = upscale_lut_.x0.data();
  const int* lx1 = upscale_lut_.x1.data();
  const uint16_t* lfx = upscale_lut_.fx.data();
  const double step_y = double(sh) / double(dh);

  auto upscale_rows = [&](int y_begin, int y_end) {
    for (int y = y_begin; y < y_end; ++y) {
      const double sy = std::clamp((y + 0.5) * step_y - 0.5, 0.0, double(sh - 1));
      const int iy0 = int(std::floor(sy));
      const int iy1 = std::min(sh - 1, iy0 + 1);
      const uint32_t fy = uint32_t(std::clamp(int(std::lround((sy - iy0) * 256.0)), 0, 256));
      const uint32_t ify = 256u - fy;
      const uint32_t* srow0 = reinterpret_cast<const uint32_t*>(src.data() + size_t(iy0) * sw * 4);
      const uint32_t* srow1 = reinterpret_cast<const uint32_t*>(src.data() + size_t(iy1) * sw * 4);
      uint32_t* drow = reinterpret_cast<uint32_t*>(dst.data() + size_t(y) * dw * 4);
      for (int x = 0; x < dw; ++x) {
        const int ix0 = lx0[x], ix1 = lx1[x];
        const uint32_t fx = lfx[x], ifx = 256u - fx;
        const uint32_t p00 = srow0[ix0], p10 = srow0[ix1];
        const uint32_t p01 = srow1[ix0], p11 = srow1[ix1];
        const uint32_t rb0 = ((p00 & 0x00FF00FFu) * ifx + (p10 & 0x00FF00FFu) * fx) >> 8;
        const uint32_t rb1 = ((p01 & 0x00FF00FFu) * ifx + (p11 & 0x00FF00FFu) * fx) >> 8;
        const uint32_t rb = (((rb0 & 0x00FF00FFu) * ify + (rb1 & 0x00FF00FFu) * fy) >> 8) & 0x00FF00FFu;
        const uint32_t g0 = (((p00 >> 8) & 0xFFu) * ifx + ((p10 >> 8) & 0xFFu) * fx) >> 8;
        const uint32_t g1 = (((p01 >> 8) & 0xFFu) * ifx + ((p11 >> 8) & 0xFFu) * fx) >> 8;
        const uint32_t g = (g0 * ify + g1 * fy) & 0xFF00u;
        drow[x] = rb | g | 0xFF000000u;
      }
    }
  };

  const unsigned n_threads = (size_t(dw) * size_t(dh) >= 400000u)
                                 ? std::min(8u, std::max(1u, std::thread::hardware_concurrency()))
                                 : 1u;
  if (n_threads <= 1u) {
    upscale_rows(0, dh);
  } else {
    std::vector<std::thread> workers;
    workers.reserve(n_threads - 1u);
    const int stripe = (dh + int(n_threads) - 1) / int(n_threads);
    for (unsigned t = 0; t + 1u < n_threads; ++t) {
      const int yb = int(t) * stripe;
      const int ye = std::min(dh, yb + stripe);
      if (yb < ye) workers.emplace_back(upscale_rows, yb, ye);
    }
    const int last_yb = int(n_threads - 1u) * stripe;
    if (last_yb < dh) upscale_rows(last_yb, dh);
    for (auto& th : workers) th.join();
  }
}

HudImage MapView::pause_map(const MapScene& scene, int width, int height, double zoom, double cx, double cz) const {
  width = std::max(1, width);
  height = std::max(1, height);
  zoom = std::max(zoom, min_zoom(width, height));
  clamp_camera(width, height, zoom, cx, cz);

  // Render internally at <= 1600x900 (matching authored map tile detail) and
  // parallel-upscale to high-DPI displays (e.g. 4K 3840x2071) for 120 FPS panning.
  const auto [rw, rh] = pause_internal_size(width, height);
  const bool needs_upscale = (rw != width || rh != height);
  const double out_scale = map_scale(width, height, zoom);
  const double scale = out_scale * (double(width) / double(rw));
  const double hi_blend = std::clamp((zoom - 1.8) / 1.7, 0.0, 1.0);

  const bool has_rd = scene.road_discovery && scene.road_discovery->ready();
  const int disc_count = has_rd ? scene.road_discovery->discovered_count() : -1;
  const size_t vis_nodes = has_rd ? scene.road_discovery->visited_node_count() : 0u;
  const size_t vis_segs = has_rd ? scene.road_discovery->visited_segment_count() : 0u;
  const bool same_scale =
      base_cache_.width == rw && base_cache_.height == rh && std::fabs(scale - base_cache_.scale) <= scale * 1e-6;
  // At steady zoom, cache a +256 px padded internal base map (background + roads)
  // so dozens of consecutive pan frames hit the cache with zero re-rasterization.
  const int want_pad = same_scale ? std::min(256, std::max(rw, rh) / 2) : 0;

  if (hi_blend > 0.0) {
    const double hw_m = (rw * 0.5 + want_pad) * scale;
    const double hh_m = (rh * 0.5 + want_pad) * scale;
    const double u0 = (cx - hw_m - freeroam::kMapWest) / freeroam::kMapWidth;
    const double u1 = (cx + hw_m - freeroam::kMapWest) / freeroam::kMapWidth;
    const double v0 = (freeroam::kMapNorth - (cz + hh_m)) / freeroam::kMapHeight;
    const double v1 = (freeroam::kMapNorth - (cz - hh_m)) / freeroam::kMapHeight;
    ensure_high_tiles(int(std::floor(u0 * kHighCols)), int(std::floor(u1 * kHighCols)),
                      int(std::floor(v0 * kHighRows)), int(std::floor(v1 * kHighRows)));
  }
  const uint32_t tmask = high_tiles_mask();

  HudImage out;
  out.width = uint32_t(width);
  out.height = uint32_t(height);
  out.rgba.resize(size_t(width) * height * 4);
  if (needs_upscale) work_rgba_.resize(size_t(rw) * rh * 4);
  std::vector<uint8_t>& draw_buf = needs_upscale ? work_rgba_ : out.rgba;

  double eff_cx = cx, eff_cz = cz;
  bool cache_hit = false;
  if (same_scale && !base_cache_.rgba.empty() && base_cache_.road_disc == scene.road_discovery &&
      base_cache_.road_disc_count == disc_count && base_cache_.road_visited_nodes == vis_nodes &&
      base_cache_.road_visited_segs == vis_segs && base_cache_.loaded_tiles_mask == tmask) {
    const int ox = int(std::lround(base_cache_.pad + (cx - base_cache_.cx) / scale));
    const int oy = int(std::lround(base_cache_.pad - (cz - base_cache_.cz) / scale));
    if (ox >= 0 && ox <= 2 * base_cache_.pad && oy >= 0 && oy <= 2 * base_cache_.pad) {
      const int cw = base_cache_.width + 2 * base_cache_.pad;
      const size_t row_bytes = size_t(rw) * 4;
      for (int y = 0; y < rh; ++y) {
        std::memcpy(&draw_buf[size_t(y) * row_bytes],
                    &base_cache_.rgba[(size_t(oy + y) * cw + ox) * 4], row_bytes);
      }
      eff_cx = base_cache_.cx + (ox - base_cache_.pad) * scale;
      eff_cz = base_cache_.cz - (oy - base_cache_.pad) * scale;
      cache_hit = true;
    }
  }

  if (!cache_hit) {
    const int cw = rw + 2 * want_pad;
    const int ch = rh + 2 * want_pad;
    base_cache_.width = rw;
    base_cache_.height = rh;
    base_cache_.pad = want_pad;
    base_cache_.scale = scale;
    base_cache_.cx = cx;
    base_cache_.cz = cz;
    base_cache_.road_disc = scene.road_discovery;
    base_cache_.road_disc_count = disc_count;
    base_cache_.road_visited_nodes = vis_nodes;
    base_cache_.road_visited_segs = vis_segs;
    base_cache_.loaded_tiles_mask = tmask;
    render_pause_base(base_cache_.rgba, cw, ch, scale, hi_blend, cx, cz, scene.road_discovery);
    if (want_pad == 0) {
      std::memcpy(draw_buf.data(), base_cache_.rgba.data(), size_t(rw) * rh * 4);
    } else {
      const size_t row_bytes = size_t(rw) * 4;
      for (int y = 0; y < rh; ++y) {
        std::memcpy(&draw_buf[size_t(y) * row_bytes],
                    &base_cache_.rgba[(size_t(want_pad + y) * cw + want_pad) * 4], row_bytes);
      }
    }
  }

  FastCanvas cv(rw, rh, draw_buf);
  const double inv_scale = 1.0 / scale;
  auto to_screen = [&](double x, double z) {
    return std::array<double, 2>{rw * 0.5 + (x - eff_cx) * inv_scale, rh * 0.5 - (z - eff_cz) * inv_scale};
  };

  // Free-roam GPS route line on fullscreen map (MapProfileFullscreen.xml "route": r=74 g=238 b=97).
  const auto& active_route = !scene.gps_route.empty() ? scene.gps_route : scene.route;
  if (active_route.size() >= 2) {
    const double rw_px = std::clamp(14.0 * inv_scale, 3.0, 8.0);
    for (int pass = 0; pass < 2; ++pass) {
      auto prev = to_screen(active_route[0][0], active_route[0][1]);
      for (size_t i = 1; i < active_route.size(); ++i) {
        const auto cur = to_screen(active_route[i][0], active_route[i][1]);
        const bool off = (prev[0] < -12 && cur[0] < -12) || (prev[0] > rw + 12 && cur[0] > rw + 12) ||
                         (prev[1] < -12 && cur[1] < -12) || (prev[1] > rh + 12 && cur[1] > rh + 12);
        if (!off) {
          if (pass == 0) cv.line(prev[0], prev[1], cur[0], cur[1], rw_px + 3.0, Rgba{12, 62, 22, 210});
          else cv.line(prev[0], prev[1], cur[0], cur[1], rw_px, Rgba{74, 238, 97, 255});
        }
        prev = cur;
      }
    }
  }

  // Rumour regions: the profile's barn_find_region ellipse, flat white at
  // its own alpha with no rim (MapProfileFullscreen.xml).
  if (scene.filter.shows(MapCategory::kBarnFind) && scene.data && scene.progress)
    for (size_t i = 0; i < scene.data->barns.size() && i < scene.progress->barns.size(); ++i) {
      if (scene.progress->barns[i].status != freeroam::kBarnRumour) continue;
      const auto& b = scene.data->barns[i];
      std::vector<std::array<double, 2>> poly;
      poly.reserve(96);
      for (int k = 0; k < 96; ++k) {
        const double t = 2 * kPi * k / 96;
        poly.push_back(to_screen(b.hint_x + b.hint_rx * std::cos(t), b.hint_z + b.hint_rz * std::sin(t)));
      }
      cv.polygon(poly, full_.region_fill);
    }

  // Icons lying flat; profile sizes in internal pixels, growing a little with
  // the zoom (frustum-culled to viewport).
  const double unit = std::min(2.0, 0.8 + zoom * 0.2) * std::max(0.85, rh / 900.0);
  const double icon_pad = 96.0 * unit;
  for (const auto& m : marks(scene, true)) {
    const auto p = to_screen(m.x, m.z);
    if (p[0] < -icon_pad || p[0] > rw + icon_pad || p[1] < -icon_pad || p[1] > rh + icon_pad) continue;
    auto it = full_.icons.find(m.key);
    if (it == full_.icons.end()) it = full_.icons.find("gas_station");
    if (it == full_.icons.end()) continue;
    // The icon under the cursor grows and the others dim
    // (map_popup_outpost_orig.png); its ring is H_MAP's lock-on.
    const bool sel = m.marker_index >= 0 && m.marker_index == scene.selected_marker;
    const bool dim = scene.selected_marker >= 0 && !sel;
    for (const auto& ic : it->second) {
      Rgba tint = ic.colour;
      if (!m.discovered) tint = {140, 145, 155, tint.a * 0.7f};
      else if (m.completed && m.marker_index >= 0) tint.a *= 0.65f;
      if (dim) tint.a *= 0.6f;
      const double sz = ic.size * unit * (sel ? 1.25 : 1.0);
      cv.icon(large_icons_, ic.cell_x, ic.cell_y, p[0], p[1], sz, 0, tint);
    }
    if (m.is_new) {
      const double oy = 20.0 * unit;
      if (!new_badge_.rgba.empty() && new_badge_.height > 0) {
        const double bh = 14.0 * unit;
        const double aspect = std::clamp(double(new_badge_.width) / double(new_badge_.height), 0.75, 4.0);
        cv.blit(new_badge_, p[0], p[1] - oy, bh * aspect, bh, Rgba{255, 255, 255, 255});
      } else {
        const double sc = std::max(0.95, unit * 1.10);
        const double bw = 20.0 * sc, bh = 8.5 * sc;
        const double by = p[1] - oy;
        cv.line(p[0] - bw * 0.5, by, p[0] + bw * 0.5, by, bh + 2.0, Rgba{12, 12, 16, 220});
        cv.line(p[0] - bw * 0.5, by, p[0] + bw * 0.5, by, bh, Rgba{249, 0, 100, 250});
        const double h2 = 2.4 * sc, gw = 3.2 * sc, st = std::max(1.0, 0.95 * sc);
        const Rgba wh{255, 255, 255, 255};
        const double nx = p[0] - 6.6 * sc;
        cv.line(nx, by + h2, nx, by - h2, st, wh);
        cv.line(nx, by - h2, nx + gw, by + h2, st, wh);
        cv.line(nx + gw, by + h2, nx + gw, by - h2, st, wh);
        const double ex = p[0] - 1.6 * sc;
        cv.line(ex, by - h2, ex, by + h2, st, wh);
        cv.line(ex, by - h2, ex + gw, by - h2, st, wh);
        cv.line(ex, by, ex + gw * 0.85, by, st, wh);
        cv.line(ex, by + h2, ex + gw, by + h2, st, wh);
        const double wx = p[0] + 3.4 * sc;
        cv.line(wx, by - h2, wx + gw * 0.3, by + h2, st, wh);
        cv.line(wx + gw * 0.3, by + h2, wx + gw * 0.65, by - h2 * 0.2, st, wh);
        cv.line(wx + gw * 0.65, by - h2 * 0.2, wx + gw, by + h2, st, wh);
        cv.line(wx + gw, by + h2, wx + gw * 1.3, by - h2, st, wh);
      }
    }
  }
  // GPS destination marker on fullscreen map (Atlas 2,4 in bright green).
  if (scene.has_gps_dest) {
    const auto dp = to_screen(scene.gps_dest_x, scene.gps_dest_z);
    if (dp[0] >= -icon_pad && dp[0] <= rw + icon_pad && dp[1] >= -icon_pad && dp[1] <= rh + icon_pad) {
      const double r = 18.0 * unit;
      for (int k = 0; k < 24; ++k) {
        const double a0 = 2 * kPi * k / 24, a1 = 2 * kPi * (k + 1) / 24;
        cv.line(dp[0] + r * std::cos(a0), dp[1] + r * std::sin(a0), dp[0] + r * std::cos(a1), dp[1] + r * std::sin(a1),
                3.0, Rgba{74, 238, 97, 255});
      }
      cv.icon(large_icons_, 2, 4, dp[0], dp[1] - 10.0 * unit, 52.0 * unit, 0, Rgba{74, 238, 97, 255});
    }
  }
  // The Festival's roaming drivers: the full-screen profile's vehicle_ai
  // icon (its shadow cell 4,3 offset, then cell 5,3; size 25).
  for (const auto& f : scene.festival_ai) {
    const auto p = to_screen(f[0], f[1]);
    if (p[0] < -20 || p[0] > rw + 20 || p[1] < -20 || p[1] > rh + 20) continue;
    cv.icon(large_icons_, 4, 3, p[0] - 3.0 * unit, p[1] + 3.0 * unit, 25.0 * unit, 0, Rgba{255, 255, 255, 220});
    cv.icon(large_icons_, 5, 3, p[0], p[1], 25.0 * unit, 0, Rgba{255, 255, 255, 255});
  }
  for (const auto& o : scene.opponents) {
    const auto p = to_screen(o[0], o[1]);
    if (p[0] < -20 || p[0] > rw + 20 || p[1] < -20 || p[1] > rh + 20) continue;
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

  if (scene.blur) box_blur_rgba(draw_buf, rw, rh, std::max(2, rh / 180), 3);
  if (needs_upscale) {
    upscale_pause_rgba(work_rgba_, rw, rh, out.rgba, width, height);
  }
  return out;
}

void MapCameraController::set_viewport(int width, int height, const MapView* view) {
  view_w_ = std::max(64, width);
  view_h_ = std::max(64, height);
  map_view_ = view;
  const double zmin = min_zoom();
  zoom_ = std::clamp(zoom_, zmin, 24.0);
  target_zoom_ = std::clamp(target_zoom_, zmin, 24.0);
  clamp_bounds();
}

double MapCameraController::min_zoom() const {
  if (map_view_ && map_view_->ready()) return map_view_->min_zoom(view_w_, view_h_);
  const double base_scale =
      std::max(freeroam::kMapWidth / double(view_w_), freeroam::kMapHeight / double(view_h_)) * 1.04;
  const double max_scale =
      std::min((freeroam::kMapWidth * 0.996) / double(view_w_), (freeroam::kMapHeight * 0.996) / double(view_h_));
  return std::max(1.0, base_scale / std::max(1e-6, max_scale));
}

void MapCameraController::reset(double cx, double cz, double zoom) {
  cx_ = cx;
  cz_ = cz;
  vx_ = 0.0;
  vz_ = 0.0;
  zoom_ = target_zoom_ = std::clamp(zoom, min_zoom(), 24.0);
  snapped_marker_ = -1;
  clamp_bounds();
}

void MapCameraController::nudge_target_zoom(double factor) {
  target_zoom_ = std::clamp(target_zoom_ * factor, min_zoom(), 24.0);
}

void MapCameraController::pan_direct(double dx, double dz) {
  cx_ += dx;
  cz_ += dz;
  vx_ = 0.0;
  vz_ = 0.0;
  clamp_bounds();
}

void MapCameraController::clamp_bounds() {
  const double zmin = min_zoom();
  zoom_ = std::clamp(zoom_, zmin, 24.0);
  target_zoom_ = std::clamp(target_zoom_, zmin, 24.0);
  const double prev_x = cx_, prev_z = cz_;
  if (map_view_ && map_view_->ready()) {
    map_view_->clamp_camera(view_w_, view_h_, zoom_, cx_, cz_);
  } else {
    const double max_scale = std::min((freeroam::kMapWidth * 0.996) / double(view_w_),
                                      (freeroam::kMapHeight * 0.996) / double(view_h_));
    const double scale = max_scale * (zmin / std::max(zmin, zoom_));
    const double half_w_m = 0.5 * double(view_w_) * scale;
    const double half_h_m = 0.5 * double(view_h_) * scale;
    const double west = freeroam::kMapWest;
    const double east = freeroam::kMapWest + freeroam::kMapWidth;
    const double south = freeroam::kMapNorth - freeroam::kMapHeight;
    const double north = freeroam::kMapNorth;
    const double inset_x = freeroam::kMapWidth * 0.0015;
    const double inset_z = freeroam::kMapHeight * 0.0015;
    const double min_cx = west + inset_x + half_w_m;
    const double max_cx = east - inset_x - half_w_m;
    const double min_cz = south + inset_z + half_h_m;
    const double max_cz = north - inset_z - half_h_m;
    cx_ = (min_cx <= max_cx) ? std::clamp(cx_, min_cx, max_cx) : 0.5 * (west + east);
    cz_ = (min_cz <= max_cz) ? std::clamp(cz_, min_cz, max_cz) : 0.5 * (south + north);
  }
  if (cx_ != prev_x) vx_ = 0.0;
  if (cz_ != prev_z) vz_ = 0.0;
}

bool MapCameraController::step(double dt, double pan_x, double pan_z, double zoom_rate,
                               const std::vector<MapMarker>& visible_markers) {
  dt = std::clamp(dt, 0.0005, 0.05);
  const double prev_cx = cx_, prev_cz = cz_, prev_zoom = zoom_;
  const double zmin = min_zoom();

  // Smooth continuous zoom (analog triggers / held +/-) + exponential interpolation to target_zoom.
  if (std::fabs(zoom_rate) > 1e-3) {
    target_zoom_ = std::clamp(target_zoom_ * std::pow(2.8, zoom_rate * dt), zmin, 24.0);
  }
  const double zoom_alpha = 1.0 - std::exp(-14.0 * dt);
  if (std::fabs(target_zoom_ - zoom_) > 1e-4) {
    zoom_ *= std::pow(target_zoom_ / zoom_, zoom_alpha);
    zoom_ = std::clamp(zoom_, zmin, 24.0);
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
