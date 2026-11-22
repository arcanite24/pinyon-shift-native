#include "map_view.h"

#include <algorithm>
#include <cmath>
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
    const double r = width / 2;
    const int x0 = std::max(0, int(std::floor(std::min(ax, bx) - r - 1)));
    const int x1 = std::min(w - 1, int(std::ceil(std::max(ax, bx) + r + 1)));
    const int y0 = std::max(0, int(std::floor(std::min(ay, by) - r - 1)));
    const int y1 = std::min(h - 1, int(std::ceil(std::max(ay, by) + r + 1)));
    if (x0 > x1 || y0 > y1) return;
    const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const double px = x + 0.5 - ax, py = y + 0.5 - ay;
        const double t = len2 > 0 ? std::clamp((px * dx + py * dy) / len2, 0.0, 1.0) : 0.0;
        const double d = std::hypot(px - t * dx, py - t * dy);
        const double cover = std::clamp(r + 0.5 - d, 0.0, 1.0);
        if (cover > 0) blend(x, y, c, float(cover));
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

// The icons a scene shows, in drawing order (profile groups by filter value).
struct Mark {
  std::string key;
  double x, z;
  bool is_new = false;
  bool completed = false;
  int marker_index = -1;
};
std::vector<Mark> marks(const MapScene& s) {
  std::vector<Mark> out;
  if (s.data && s.progress) {
    for (const auto& sign : s.data->signs) {
      const bool found = s.progress->signs_found.count(sign.id) > 0;
      if (found) out.push_back({"flyer_collected", sign.at.x, sign.at.z, false, true, -1});
      else if (s.reveal) out.push_back({"flyer", sign.at.x, sign.at.z, false, false, -1});
    }
    for (size_t i = 0; i < s.data->barns.size(); ++i) {
      const auto& b = s.data->barns[i];
      const int status = i < s.progress->barns.size() ? s.progress->barns[i].status : freeroam::kBarnNotSpawned;
      if (status == freeroam::kBarnFound) out.push_back({"barnfind_collected", b.at.x, b.at.z, false, true, -1});
      else if (s.reveal) out.push_back({"barnfind", b.at.x, b.at.z, false, false, -1});
    }
  }
  for (size_t i = 0; i < s.markers.size(); ++i) {
    const auto& m = s.markers[i];
    out.push_back({m.icon_tag, m.x, m.z, m.is_new, m.completed, int(i)});
  }
  return out;
}

}  // namespace

bool MapView::open(const std::filesystem::path& game_root, std::string* error) {
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
    for (const auto& p : r.points) {
      road.points.push_back({p.x, p.z});
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
  auto zip = fh1::Archive::open(media / "ui" / "textures" / "Horizon.zip", &why);
  if (!zip) return fail(error, why);
  if (!read_texture(*zip, "Map/background/low-0-0.xds", background_, error)) return false;
  if (!read_texture(*zip, "Map/icons/MapIcons/EN/MapIconSheetSmall.xds", small_icons_, error)) return false;
  if (!read_texture(*zip, "Map/icons/MapIcons/EN/MapIconSheet.xds", large_icons_, error)) return false;
  return true;
}

HudImage MapView::minimap(const MapScene& scene, int size) const {
  // SPEC-UI-LAYOUTS a: no frame; the roads fade to transparent towards the
  // edge (ours: over the outer 45 % of the half size, as an ellipse).
  Canvas cv(size, size, {0, 0, 0, 0});
  cv.edge_fade = 0.3f;
  // The camera (ours, the spec's interim): 120 m up and 60 m behind the
  // point it looks at, 60 m ahead of the car, pitched down 45 degrees, a
  // half field of view of atan(0.5): the far edge 300 m ahead, the car at
  // five sixths of the panel's height.
  const double fx = std::sin(scene.heading), fz = std::cos(scene.heading);
  const double rx = fz, rz = -fx;  // right of the heading (+X when facing +Z)
  constexpr double kHeight = 120, kBack = 60, kS = 0.70710678118654752;
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
  const double reach = 450;  // m around the car that can show

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
        if (pass == 0) cv.line(pa.x, pa.y, pb.x, pb.y, std::max(2.0, px + 2.5), Rgba{0, 0, 0, 50});
        if (pass == 1) cv.line(pa.x, pa.y, pb.x, pb.y, std::max(1.2, px), Rgba{246, 246, 246, 255});
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

  // Icons, standing up (a constant size on screen).
  const double unit = size / 300.0;
  for (const auto& m : marks(scene)) {
    if (std::hypot(m.x - scene.x, m.z - scene.z) > reach) continue;
    const auto it = mini_.icons.find(m.key);
    if (it == mini_.icons.end()) continue;
    const P p = project(m.x, m.z);
    if (p.depth < 1) continue;
    double top_y = p.y;
    for (const auto& ic : it->second) {
      Rgba tint = ic.colour;
      if (m.completed && m.marker_index >= 0) tint.a *= 0.6f;
      const double iy = p.y - ic.size * unit * 0.4;
      top_y = std::min(top_y, iy - ic.size * unit * 0.45);
      cv.icon(small_icons_, ic.cell_x, ic.cell_y, p.x, iy, ic.size * unit, 0, tint);
    }
    // Pink NEW dot/tag above uncompleted events (hud-freeroam-event-marker.png).
    if (m.is_new) {
      const double bw = 14 * unit, bh = 5 * unit;
      cv.line(p.x - bw * 0.5, top_y - bh * 0.5, p.x + bw * 0.5, top_y - bh * 0.5, bh, Rgba{245, 4, 92, 245});
    }
  }
  // Opponents: white discs with a grey rim, as the race frames show.
  for (const auto& o : scene.opponents) {
    if (std::hypot(o[0] - scene.x, o[1] - scene.z) > reach) continue;
    const P p = project(o[0], o[1]);
    if (p.depth < 1) continue;
    cv.line(p.x, p.y, p.x, p.y, 15 * unit + 2, Rgba{120, 120, 125, 230});
    cv.line(p.x, p.y, p.x, p.y, 15 * unit - 1, Rgba{236, 236, 238, 255});
  }
  // The player, heading up.
  if (const auto it = mini_.icons.find("player_local"); it != mini_.icons.end()) {
    const P p = project(scene.x, scene.z);
    // About 35 px at 720 lines, as the frames show.
    for (const auto& ic : it->second) cv.icon(small_icons_, ic.cell_x, ic.cell_y, p.x, p.y, ic.size * unit * 1.8, 0, ic.colour);
  }
  // No frame (SPEC-UI-LAYOUTS a).
  return cv.image();
}

HudImage MapView::pause_map(const MapScene& scene, int width, int height, double zoom, double cx, double cz) const {
  Canvas cv(width, height, {12, 12, 14, 255});
  zoom = std::max(zoom, 0.25);
  // Metres per pixel: zoom 1 fits the road network.
  const double scale = std::max((max_x_ - min_x_) / width, (max_z_ - min_z_) / height) * 1.04 / zoom;
  auto to_screen = [&](double x, double z) {
    return std::array<double, 2>{width / 2.0 + (x - cx) / scale, height / 2.0 - (z - cz) / scale};
  };

  // The background's low level, bilinear.
  if (!background_.rgba.empty()) {
    const int bw = int(background_.width), bh = int(background_.height);
    for (int y = 0; y < height; ++y) {
      const double z = cz - (y + 0.5 - height / 2.0) * scale;
      const double v = (freeroam::kMapNorth - z) / freeroam::kMapHeight * bh - 0.5;
      if (v < -0.5 || v > bh - 0.5) continue;
      const int iy = std::clamp(int(std::floor(v)), 0, bh - 1), iy1 = std::min(bh - 1, iy + 1);
      const double fy = std::clamp(v - iy, 0.0, 1.0);
      float* row = &cv.rgb[size_t(y) * width * 3];
      for (int x = 0; x < width; ++x) {
        const double wx = cx + (x + 0.5 - width / 2.0) * scale;
        const double u = (wx - freeroam::kMapWest) / freeroam::kMapWidth * bw - 0.5;
        if (u < -0.5 || u > bw - 0.5) continue;
        const int ix = std::clamp(int(std::floor(u)), 0, bw - 1), ix1 = std::min(bw - 1, ix + 1);
        const double fx = std::clamp(u - ix, 0.0, 1.0);
        const uint8_t* a = &background_.rgba[(size_t(iy) * bw + ix) * 4];
        const uint8_t* b = &background_.rgba[(size_t(iy) * bw + ix1) * 4];
        const uint8_t* c = &background_.rgba[(size_t(iy1) * bw + ix) * 4];
        const uint8_t* d = &background_.rgba[(size_t(iy1) * bw + ix1) * 4];
        for (int ch = 0; ch < 3; ++ch)
          row[x * 3 + ch] = float((a[ch] * (1 - fx) + b[ch] * fx) * (1 - fy) + (c[ch] * (1 - fx) + d[ch] * fx) * fy);
      }
    }
  }

  // Roads: a width in metres from the profile size, kept between 1.2 and 7
  // pixels (ours).
  for (int pass = 0; pass < 2; ++pass)
    for (int style = 0; style < kRoadTypeCount; ++style) {
      const auto st = full_.roads.find(kRoadTypes[style]);
      if (st == full_.roads.end()) continue;
      const LineStyle& ls = st->second;
      const double line = std::clamp(ls.line_size * 0.9 / scale, 1.2, 7.0);
      for (const auto& road : roads_) {
        if (road.style != style) continue;
        auto prev = to_screen(road.points[0][0], road.points[0][1]);
        for (size_t k = 1; k < road.points.size(); ++k) {
          const auto cur = to_screen(road.points[k][0], road.points[k][1]);
          const bool off = (prev[0] < -8 && cur[0] < -8) || (prev[0] > width + 8 && cur[0] > width + 8) ||
                           (prev[1] < -8 && cur[1] < -8) || (prev[1] > height + 8 && cur[1] > height + 8);
          if (!off) {
            if (pass == 0) cv.line(prev[0], prev[1], cur[0], cur[1], line + 1.5, Rgba{ls.under.r, ls.under.g, ls.under.b, 110});
            else cv.line(prev[0], prev[1], cur[0], cur[1], line, ls.core);
          }
          prev = cur;
        }
      }
    }

  // Rumour circles: the region's outline over the map, filled faintly.
  if (scene.data && scene.progress)
    for (size_t i = 0; i < scene.data->barns.size() && i < scene.progress->barns.size(); ++i) {
      if (scene.progress->barns[i].status != freeroam::kBarnRumour) continue;
      const auto& b = scene.data->barns[i];
      std::vector<std::array<double, 2>> poly;
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
  // the zoom.
  const double unit = std::min(2.0, 0.8 + zoom * 0.2) * std::max(1.0, height / 900.0);
  for (const auto& m : marks(scene)) {
    const auto it = full_.icons.find(m.key);
    if (it == full_.icons.end()) continue;
    const auto p = to_screen(m.x, m.z);
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
      if (m.completed && m.marker_index >= 0) tint.a *= 0.65f;
      const double sz = ic.size * unit * (sel ? 1.25 : 1.0);
      cv.icon(large_icons_, ic.cell_x, ic.cell_y, p[0], p[1], sz, 0, tint);
    }
    if (m.is_new) {
      const double oy = 18 * unit;
      cv.line(p[0] - 8 * unit, p[1] - oy, p[0] + 8 * unit, p[1] - oy, 4.5 * unit, Rgba{245, 4, 92, 250});
    }
  }
  for (const auto& o : scene.opponents) {
    const auto p = to_screen(o[0], o[1]);
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
  // Central map cursor crosshair (subtle white reticle).
  {
    const double mx = width / 2.0, my = height / 2.0, g = 7.0, l = 16.0;
    const Rgba ret{255, 255, 255, 210};
    cv.line(mx - l, my, mx - g, my, 2.0, ret);
    cv.line(mx + g, my, mx + l, my, 2.0, ret);
    cv.line(mx, my - l, mx, my - g, 2.0, ret);
    cv.line(mx, my + g, mx, my + l, 2.0, ret);
  }
  return cv.image();
}

}  // namespace carview
