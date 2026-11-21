#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>
#include <tuple>

#include "fh1data/vfont.h"

namespace carview::ui {
namespace {

// The stroke font: each glyph is polylines on a 10-unit cap height (y down
// from the cap line, 10 the baseline), chamfered corners for curves. A
// polyline is its points in order; {-1, -1} ends one.
struct GlyphDef {
  char c;
  float width;
  std::vector<float> xy;  // x, y pairs; -1, -1 separates polylines
};

constexpr float E = -1;  // a polyline break

const std::vector<GlyphDef>& glyph_defs() {
  static const std::vector<GlyphDef> defs = {
      {'A', 6, {0, 10, 0, 2, 2, 0, 4, 0, 6, 2, 6, 10, E, E, 0, 6, 6, 6}},
      {'B', 6, {0, 5, 4.5, 5, 6, 3.5, 6, 1.5, 4.5, 0, 0, 0, 0, 10, 4.5, 10, 6, 8.5, 6, 6.5, 4.5, 5}},
      {'C', 6, {6, 2, 4, 0, 2, 0, 0, 2, 0, 8, 2, 10, 4, 10, 6, 8}},
      {'D', 6, {0, 0, 4, 0, 6, 2, 6, 8, 4, 10, 0, 10, 0, 0}},
      {'E', 5.5f, {5.5f, 0, 0, 0, 0, 10, 5.5f, 10, E, E, 0, 5, 4.5f, 5}},
      {'F', 5.5f, {5.5f, 0, 0, 0, 0, 10, E, E, 0, 5, 4.5f, 5}},
      {'G', 6, {6, 2, 4, 0, 2, 0, 0, 2, 0, 8, 2, 10, 4, 10, 6, 8, 6, 5.5f, 3.5f, 5.5f}},
      {'H', 6, {0, 0, 0, 10, E, E, 6, 0, 6, 10, E, E, 0, 5, 6, 5}},
      {'I', 0, {0, 0, 0, 10}},
      {'J', 6, {6, 0, 6, 8, 4, 10, 2, 10, 0, 8}},
      {'K', 6, {0, 0, 0, 10, E, E, 6, 0, 0.5f, 5.5f, E, E, 2.2f, 4.2f, 6, 10}},
      {'L', 5.5f, {0, 0, 0, 10, 5.5f, 10}},
      {'M', 7, {0, 10, 0, 0, 3.5f, 5, 7, 0, 7, 10}},
      {'N', 6, {0, 10, 0, 0, 6, 10, 6, 0}},
      {'O', 6, {2, 0, 4, 0, 6, 2, 6, 8, 4, 10, 2, 10, 0, 8, 0, 2, 2, 0}},
      {'P', 6, {0, 10, 0, 0, 4.5f, 0, 6, 1.5f, 6, 4, 4.5f, 5.5f, 0, 5.5f}},
      {'Q', 6, {2, 0, 4, 0, 6, 2, 6, 8, 4, 10, 2, 10, 0, 8, 0, 2, 2, 0, E, E, 3.6f, 7.6f, 6.4f, 10.4f}},
      {'R', 6, {0, 10, 0, 0, 4.5f, 0, 6, 1.5f, 6, 4, 4.5f, 5.5f, 0, 5.5f, E, E, 3, 5.5f, 6, 10}},
      {'S', 6, {6, 1.5f, 4.5f, 0, 1.5f, 0, 0, 1.5f, 0, 3.5f, 1.5f, 5, 4.5f, 5, 6, 6.5f, 6, 8.5f, 4.5f, 10, 1.5f, 10, 0,
                8.5f}},
      {'T', 6, {0, 0, 6, 0, E, E, 3, 0, 3, 10}},
      {'U', 6, {0, 0, 0, 8, 2, 10, 4, 10, 6, 8, 6, 0}},
      {'V', 6, {0, 0, 3, 10, 6, 0}},
      {'W', 8, {0, 0, 2, 10, 4, 3, 6, 10, 8, 0}},
      {'X', 6, {0, 0, 6, 10, E, E, 6, 0, 0, 10}},
      {'Y', 6, {0, 0, 3, 5, 6, 0, E, E, 3, 5, 3, 10}},
      {'Z', 6, {0, 0, 6, 0, 0, 10, 6, 10}},
      {'0', 6, {2, 0, 4, 0, 6, 2, 6, 8, 4, 10, 2, 10, 0, 8, 0, 2, 2, 0}},
      {'1', 4, {0.5f, 2, 2.5f, 0, 2.5f, 10}},
      {'2', 6, {0, 2, 2, 0, 4, 0, 6, 2, 6, 4, 0, 10, 6, 10}},
      {'3', 6, {0, 1.5f, 1.5f, 0, 4.5f, 0, 6, 1.5f, 6, 3.5f, 4.5f, 5, 2, 5, E, E, 4.5f, 5, 6, 6.5f, 6, 8.5f, 4.5f, 10,
                1.5f, 10, 0, 8.5f}},
      {'4', 6, {4.5f, 10, 4.5f, 0, 0, 6.5f, 6, 6.5f}},
      {'5', 6, {6, 0, 0, 0, 0, 4.5f, 4.5f, 4.5f, 6, 6, 6, 8.5f, 4.5f, 10, 1.5f, 10, 0, 8.5f}},
      {'6', 6, {5.5f, 0, 2, 0, 0, 2, 0, 8.5f, 1.5f, 10, 4.5f, 10, 6, 8.5f, 6, 6, 4.5f, 4.5f, 0, 4.5f}},
      {'7', 6, {0, 0, 6, 0, 6, 2, 2, 10}},
      {'8', 6, {1.5f, 5, 0, 3.5f, 0, 1.5f, 1.5f, 0, 4.5f, 0, 6, 1.5f, 6, 3.5f, 4.5f, 5, 1.5f, 5, 0, 6.5f, 0, 8.5f, 1.5f,
                10, 4.5f, 10, 6, 8.5f, 6, 6.5f, 4.5f, 5}},
      {'9', 6, {0.5f, 10, 4, 10, 6, 8, 6, 1.5f, 4.5f, 0, 1.5f, 0, 0, 1.5f, 0, 4, 1.5f, 5.5f, 6, 5.5f}},
      {'.', 0, {0, 10, 0, 10}},
      {',', 0.6f, {0.6f, 9.6f, 0, 11.5f}},
      {':', 0, {0, 3.5f, 0, 3.5f, E, E, 0, 10, 0, 10}},
      {'-', 4, {0, 5.5f, 4, 5.5f}},
      {'+', 5, {0, 5.5f, 5, 5.5f, E, E, 2.5f, 3, 2.5f, 8}},
      {'/', 4, {0, 10, 4, 0}},
      {'%', 6, {0, 10, 6, 0, E, E, 0.5f, 1.5f, 0.5f, 1.5f, E, E, 5.5f, 8.5f, 5.5f, 8.5f}},
      {'(', 2, {2, 0, 0, 2.5f, 0, 7.5f, 2, 10}},
      {')', 2, {0, 0, 2, 2.5f, 2, 7.5f, 0, 10}},
      {'[', 2, {2, 0, 0, 0, 0, 10, 2, 10}},
      {']', 2, {0, 0, 2, 0, 2, 10, 0, 10}},
      {'!', 0, {0, 0, 0, 6.8f, E, E, 0, 10, 0, 10}},
      {'?', 6, {0, 2, 2, 0, 4, 0, 6, 2, 6, 3.5f, 3, 6, 3, 7, E, E, 3, 10, 3, 10}},
      {'<', 4, {4, 1.5f, 0, 5.5f, 4, 9.5f}},
      {'>', 4, {0, 1.5f, 4, 5.5f, 0, 9.5f}},
      {'\'', 0, {0, 0, 0, 3}},
      {'"', 2, {0, 0, 0, 3, E, E, 2, 0, 2, 3}},
      {'_', 6, {0, 10.8f, 6, 10.8f}},
      {'=', 5, {0, 4, 5, 4, E, E, 0, 7, 5, 7}},
      {'*', 4, {2, 1, 2, 5, E, E, 0, 2, 4, 4, E, E, 4, 2, 0, 4}},
      {'#', 6, {2, 0, 1, 10, E, E, 5, 0, 4, 10, E, E, 0, 3.5f, 6, 3.5f, E, E, 0, 6.5f, 6, 6.5f}},
      {'&', 6, {6, 10, 1, 3.5f, 1, 1.5f, 2.5f, 0, 4, 0, 5, 1.5f, 5, 3, 0, 6.5f, 0, 8.5f, 1.5f, 10, 3.5f, 10, 6, 6}},
  };
  return defs;
}

struct Glyph {
  uint32_t first = 0, count = 0;
  float width = 0;
  bool present = false;
};

struct Font {
  std::vector<std::array<float, 4>> segments;
  std::array<Glyph, 128> glyphs{};
  Font() {
    for (const GlyphDef& d : glyph_defs()) {
      Glyph& g = glyphs[size_t(uint8_t(d.c))];
      g.first = uint32_t(segments.size());
      g.width = d.width;
      g.present = true;
      for (size_t i = 0; i + 3 < d.xy.size(); i += 2) {
        if (d.xy[i] == E || d.xy[i + 2] == E) continue;
        segments.push_back({d.xy[i], d.xy[i + 1], d.xy[i + 2], d.xy[i + 3]});
      }
      g.count = uint32_t(segments.size()) - g.first;
    }
  }
};

const Font& font() {
  static const Font f;
  return f;
}

const Glyph* glyph(char c) {
  if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  const uint8_t k = uint8_t(c);
  if (k >= 128) return nullptr;
  const Glyph& g = font().glyphs[k];
  return g.present ? &g : nullptr;
}

constexpr float kSpaceUnits = 4.0f;

float advance_units(const Glyph* g, float radius, float tracking) {
  // The stroke's two radii, a gap of 1.8 units, and the tracking.
  return (g ? g->width : kSpaceUnits) + 2 * radius + 1.8f + tracking;
}

}  // namespace

std::span<const std::array<float, 4>> font_segments() { return font().segments; }

uint32_t fade(uint32_t c, double k) {
  const double a = double(c >> 24) * std::clamp(k, 0.0, 1.0);
  return (c & 0x00FFFFFFu) | uint32_t(std::lround(a)) << 24;
}

void List::rect(float x0, float y0, float x1, float y1, uint32_t color, float radius, uint32_t color2) {
  items_.push_back({{x0, y0, x1, y1}, {radius, 0, 0, 0}, {0, 0, 0, 0}, color, color2 ? color2 : color, kRoundRect, 0});
}

void List::frame(float x0, float y0, float x1, float y1, uint32_t color, float width, float radius) {
  items_.push_back({{x0, y0, x1, y1}, {radius, width, 0, 0}, {0, 0, 0, 0}, color, color, kRoundRect, 0});
}

void List::shadow(float x0, float y0, float x1, float y1, uint32_t color, float radius, float blur) {
  items_.push_back({{x0 - blur - 1, y0 - blur - 1, x1 + blur + 1, y1 + blur + 1},
                    {radius, 0, blur + 1, 0},
                    {blur, 0, 0, 0},
                    color,
                    color,
                    kRoundRect,
                    0});
}

void List::line(float ax, float ay, float bx, float by, float width, uint32_t color) {
  const float h = 0.5f * width + 1;
  items_.push_back({{std::min(ax, bx) - h, std::min(ay, by) - h, std::max(ax, bx) + h, std::max(ay, by) + h},
                    {ax, ay, bx, by},
                    {0.5f * width, 0, 0, 0},
                    color,
                    color,
                    kLine,
                    0});
}

void List::circle(float cx, float cy, float r, uint32_t color) { rect(cx - r, cy - r, cx + r, cy + r, color, r); }

void List::arc(float cx, float cy, float radius, float width, float a0, float a1, uint32_t color) {
  if (a1 <= a0) return;
  const float h = radius + 0.5f * width + 1;
  items_.push_back({{cx - h, cy - h, cx + h, cy + h}, {cx, cy, radius, 0.5f * width}, {a0, a1, 0, 0}, color, color,
                    kArc, 0});
}

void List::image(float x0, float y0, float x1, float y1, std::array<float, 4> uv, uint32_t tint, float radius) {
  items_.push_back({{x0, y0, x1, y1}, {radius, 0, 0, 0}, {uv[0], uv[1], uv[2], uv[3]}, tint, tint, kImage, 0});
}

// The vector fonts and their glyph atlas.
namespace {

struct VectorFonts {
  std::shared_ptr<const fh1::VFont> font[4];  // A, B, E, D
  std::vector<uint8_t> atlas = std::vector<uint8_t>(size_t(kAtlasSize) * kAtlasSize * 4, 0);
  std::vector<std::array<uint32_t, 4>> dirty;
  uint32_t shelf_x = 0, shelf_y = 0, shelf_h = 0;
  uint32_t image_x = 0, image_y = kGlyphRows, image_h = 0;  // the image shelf
  std::map<std::string, std::array<float, 4>> images;
  struct Entry {
    float u0, v0, u1, v1;  // atlas texels
    float left, top;       // the bitmap's top-left from the pen, px at the cached size (y down from the baseline)
    bool empty;
  };
  std::map<std::tuple<int, uint32_t, int>, Entry> cache;  // (font, code point, px per em) -> entry

  // Clears the glyph rows (the images stay).
  void reset() {
    const size_t glyph_bytes = size_t(kAtlasSize) * kGlyphRows * 4;
    std::fill(atlas.begin(), atlas.begin() + ptrdiff_t(glyph_bytes), uint8_t(0));
    for (size_t i = 0; i < glyph_bytes; i += 4) atlas[i] = atlas[i + 1] = atlas[i + 2] = 255;
    cache.clear();
    shelf_x = shelf_y = shelf_h = 0;
    dirty.push_back({0, 0, kAtlasSize, kGlyphRows});
  }
};

VectorFonts& vf() {
  static VectorFonts v = [] {
    VectorFonts x;
    x.reset();
    return x;
  }();
  return v;
}

}  // namespace

bool atlas_image(const std::string& key, uint32_t w, uint32_t h, std::span<const uint8_t> rgba,
                 std::array<float, 4>& uv) {
  VectorFonts& v = vf();
  if (auto it = v.images.find(key); it != v.images.end()) return uv = it->second, true;
  if (!w || !h || w + 2 > kAtlasSize || rgba.size() < size_t(w) * h * 4) return false;
  if (v.image_x + w + 2 > kAtlasSize) v.image_x = 0, v.image_y += v.image_h, v.image_h = 0;
  if (v.image_y + h + 2 > kAtlasSize) return false;
  // One texel of edge padding all round, so bilinear taps stay inside.
  const uint32_t ax = v.image_x + 1, ay = v.image_y + 1;
  for (int y = -1; y <= int(h); ++y)
    for (int x = -1; x <= int(w); ++x) {
      const uint32_t sx = uint32_t(std::clamp(x, 0, int(w) - 1)), sy = uint32_t(std::clamp(y, 0, int(h) - 1));
      std::memcpy(&v.atlas[((size_t(int(ay) + y)) * kAtlasSize + size_t(int(ax) + x)) * 4], &rgba[(size_t(sy) * w + sx) * 4], 4);
    }
  v.dirty.push_back({ax - 1, ay - 1, w + 2, h + 2});
  v.image_x += w + 2;
  v.image_h = std::max(v.image_h, h + 2);
  const float k = 1.0f / float(kAtlasSize);
  uv = {float(ax) * k, float(ay) * k, float(ax + w) * k, float(ay + h) * k};
  v.images[key] = uv;
  return true;
}

bool atlas_find(const std::string& key, std::array<float, 4>& uv) {
  const VectorFonts& v = vf();
  if (auto it = v.images.find(key); it != v.images.end()) return uv = it->second, true;
  return false;
}

namespace {

// The style's font: 0 A, 1 B, 2 E, -1 the stroke font.
int font_of(const TextStyle& s) {
  int f = -1;
  switch (s.font) {
    case ui::Font::kStroke: return -1;
    case ui::Font::kA: f = 0; break;
    case ui::Font::kB: f = 1; break;
    case ui::Font::kE: f = 2; break;
    case ui::Font::kD: f = 3; break;
    case ui::Font::kAuto: f = s.slant > 0.05f ? 2 : 1; break;
  }
  return vf().font[f] ? f : -1;
}

// The next code point of UTF-8 text (ASCII letters upper-cased, as the
// stroke font draws them); 0xFFFD for a bad byte.
uint32_t next_code_point(std::string_view t, size_t& i, bool keep_case = false) {
  const auto b = static_cast<unsigned char>(t[i++]);
  if (b < 0x80) return !keep_case && b >= 'a' && b <= 'z' ? uint32_t(b - 32) : b;
  int n = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : -1;
  if (n < 0) return 0xFFFD;
  uint32_t c = b & (0x3F >> n);
  for (; n > 0 && i < t.size(); --n) c = c << 6 | (static_cast<unsigned char>(t[i++]) & 0x3F);
  return c;
}

// px per em for a cap height of `size` px.
float em_px(const fh1::VFont& f, float size) { return size / f.cap_height(); }

// The cached size: whole px up to 48 px per em, then steps of 4 (scaled).
int cached_px(float px) { return px <= 48 ? std::max(1, int(std::lround(px))) : int(std::lround(px / 4)) * 4; }

const VectorFonts::Entry* glyph_entry(int font, uint32_t cp, int px) {
  VectorFonts& v = vf();
  const auto key = std::make_tuple(font, cp, px);
  if (auto it = v.cache.find(key); it != v.cache.end()) return &it->second;
  const fh1::VFontGlyph* g = v.font[font]->find(cp);
  if (!g) return nullptr;
  const float k = float(px);
  const int pad = 2;
  const int gx0 = int(std::floor(g->min_x * k)) - pad, gx1 = int(std::ceil(g->max_x * k)) + pad;
  const int gy0 = int(std::floor(-g->max_y * k)) - pad, gy1 = int(std::ceil(-g->min_y * k)) + pad;
  const int w = gx1 - gx0, h = gy1 - gy0;
  if (w <= 0 || h <= 0 || w > int(kAtlasSize) || h > int(kAtlasSize)) return nullptr;
  if (v.shelf_x + uint32_t(w) > kAtlasSize) v.shelf_x = 0, v.shelf_y += v.shelf_h, v.shelf_h = 0;
  if (v.shelf_y + uint32_t(h) > kGlyphRows) v.reset();  // full: start over (this frame may show stale glyphs)
  const uint32_t ax = v.shelf_x, ay = v.shelf_y;
  v.shelf_x += uint32_t(w) + 1;
  v.shelf_h = std::max(v.shelf_h, uint32_t(h) + 1);
  std::vector<uint8_t> cov;
  fh1::rasterize_vfont_glyph(*g, k, float(-gx0), float(-gy0), cov, w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) v.atlas[((size_t(ay) + size_t(y)) * kAtlasSize + ax + size_t(x)) * 4 + 3] = cov[size_t(y) * size_t(w) + size_t(x)];
  v.dirty.push_back({ax, ay, uint32_t(w), uint32_t(h)});
  VectorFonts::Entry e{float(ax), float(ay), float(ax + uint32_t(w)), float(ay + uint32_t(h)), float(gx0), float(gy0), false};
  return &(v.cache[key] = e);
}

float vector_measure(int font, std::string_view text, const TextStyle& s) {
  const fh1::VFont& f = *vf().font[font];
  const float k = em_px(f, s.size);
  float w = 0;
  size_t n = 0;
  for (size_t i = 0; i < text.size();) {
    const uint32_t cp = next_code_point(text, i, s.keep_case);
    const fh1::VFontGlyph* g = cp == ' ' ? nullptr : f.find(cp);
    w += (g ? g->advance : f.space_width) * k + s.tracking * s.size;
    ++n;
  }
  if (n) w -= s.tracking * s.size;
  return w;
}

}  // namespace

void set_vector_fonts(std::shared_ptr<const fh1::VFont> a, std::shared_ptr<const fh1::VFont> b,
                      std::shared_ptr<const fh1::VFont> e, std::shared_ptr<const fh1::VFont> d) {
  VectorFonts& v = vf();
  v.font[0] = std::move(a), v.font[1] = std::move(b), v.font[2] = std::move(e), v.font[3] = std::move(d);
  v.reset();
}
bool vector_fonts_loaded() { return vf().font[1] != nullptr; }
const std::vector<uint8_t>& atlas_pixels() { return vf().atlas; }
std::vector<std::array<uint32_t, 4>> atlas_take_dirty() { return std::exchange(vf().dirty, {}); }

float List::measure(std::string_view text, const TextStyle& s) {
  if (const int f = font_of(s); f >= 0) return vector_measure(f, text, s);
  const float r = s.weight * 10;
  float units = 0;
  for (char c : text) units += advance_units(glyph(c), r, s.tracking * 10);
  if (!text.empty()) units -= 1.8f + s.tracking * 10;  // no gap after the last
  return units * s.size / 10;
}

float List::text(float x, float y, std::string_view text, const TextStyle& s) {
  const float w = measure(text, s);
  if (s.align == Align::kCenter) x -= 0.5f * w;
  if (s.align == Align::kRight) x -= w;
  if (const int f = font_of(s); f >= 0) {
    // The game's font: the cap top at y, so the baseline at y + size.
    const fh1::VFont& font = *vf().font[f];
    const float k = em_px(font, s.size);
    const int px = cached_px(k);
    const float scale = k / float(px);
    const float base = y + s.size;
    float pen = x;
    for (size_t i = 0; i < text.size();) {
      const uint32_t cp = next_code_point(text, i, s.keep_case);
      const fh1::VFontGlyph* g = cp == ' ' ? nullptr : font.find(cp);
      if (g) {
        if (const auto* e = glyph_entry(f, cp, px)) {
          // At the cached size the quad sits on whole pixels (no resampling blur).
          float gx = pen + e->left * scale, gy = base + e->top * scale;
          if (scale == 1.0f) gx = std::round(gx), gy = std::round(gy);
          const float inv = 1.0f / float(kAtlasSize);
          items_.push_back({{gx, gy, gx + (e->u1 - e->u0) * scale, gy + (e->v1 - e->v0) * scale},
                            {0, 0, 0, 0},
                            {e->u0 * inv, e->v0 * inv, e->u1 * inv, e->v1 * inv},
                            s.color,
                            s.color,
                            kImage,
                            0});
        }
      }
      pen += (g ? g->advance : font.space_width) * k + s.tracking * s.size;
    }
    return w;
  }
  const float k = s.size / 10;  // px per unit
  const float r = s.weight * 10;
  float pen = x + r * k;  // the first stroke's left edge at x
  for (char c : text) {
    const Glyph* g = glyph(c);
    if (g && g->count) {
      const float pad = (r + 1.5f) * k;
      const float lean = s.slant * 11 * k;
      items_.push_back({{pen - pad - std::max(0.0f, -lean), y - pad, pen + g->width * k + pad + std::max(0.0f, lean),
                         y + 10 * k + pad + 1.5f * k},
                        {pen, y, k, r},
                        {float(g->first), float(g->count), s.slant, 0},
                        s.color,
                        s.color,
                        kGlyph,
                        0});
    }
    pen += advance_units(g, r, s.tracking * 10) * k;
  }
  return w;
}

// The CPU twin of shaders/ui.frag.
namespace {

float seg_dist(float px, float py, const std::array<float, 4>& s) {
  const float bax = s[2] - s[0], bay = s[3] - s[1];
  const float pax = px - s[0], pay = py - s[1];
  const float l = bax * bax + bay * bay;
  const float t = l > 0 ? std::clamp((pax * bax + pay * bay) / l, 0.0f, 1.0f) : 0;
  return std::hypot(pax - t * bax, pay - t * bay);
}

constexpr float kPi = 3.14159265f;

// Coverage (0..1) and the colour's blend between color (0) and color2 (1).
float coverage(const Instance& it, float px, float py, float& mix) {
  mix = 0;
  switch (it.kind) {
    case kRoundRect: {
      const float cx = 0.5f * (it.rect[0] + it.rect[2]), cy = 0.5f * (it.rect[1] + it.rect[3]);
      const float hx = 0.5f * (it.rect[2] - it.rect[0]) - it.p[2], hy = 0.5f * (it.rect[3] - it.rect[1]) - it.p[2];
      const float r = std::min({it.p[0], hx, hy});
      const float qx = std::fabs(px - cx) - hx + r, qy = std::fabs(py - cy) - hy + r;
      float d = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
      if (it.p[1] > 0) d = std::fabs(d + 0.5f * it.p[1]) - 0.5f * it.p[1];
      mix = std::clamp((py - it.rect[1]) / std::max(1.0f, it.rect[3] - it.rect[1]), 0.0f, 1.0f);
      const float soft = it.q[0];
      if (soft > 0) {  // a smooth falloff from soft inside the edge to soft outside
        const float t = std::clamp((soft - d) / (2 * soft), 0.0f, 1.0f);
        return t * t * (3 - 2 * t);
      }
      return std::clamp(0.5f - d, 0.0f, 1.0f);
    }
    case kLine: {
      const float d = seg_dist(px, py, {it.p[0], it.p[1], it.p[2], it.p[3]}) - it.q[0];
      return std::clamp(0.5f - d, 0.0f, 1.0f);
    }
    case kArc: {
      const float dx = px - it.p[0], dy = py - it.p[1];
      float a = std::atan2(dx, -dy);
      float rel = std::fmod(a - it.q[0], 2 * kPi);
      if (rel < 0) rel += 2 * kPi;
      float d;
      if (rel <= it.q[1] - it.q[0]) {
        d = std::fabs(std::hypot(dx, dy) - it.p[2]) - it.p[3];
      } else {
        const float e0x = it.p[0] + it.p[2] * std::sin(it.q[0]), e0y = it.p[1] - it.p[2] * std::cos(it.q[0]);
        const float e1x = it.p[0] + it.p[2] * std::sin(it.q[1]), e1y = it.p[1] - it.p[2] * std::cos(it.q[1]);
        d = std::min(std::hypot(px - e0x, py - e0y), std::hypot(px - e1x, py - e1y)) - it.p[3];
      }
      return std::clamp(0.5f - d, 0.0f, 1.0f);
    }
    case kGlyph: {
      const float k = it.p[2];
      float ux = (px - it.p[0]) / k, uy = (py - it.p[1]) / k;
      ux -= it.q[2] * (10 - uy);
      const auto segs = font_segments();
      float best = 1e9f;
      const size_t first = size_t(it.q[0]), count = size_t(it.q[1]);
      for (size_t i = first; i < first + count && i < segs.size(); ++i) best = std::min(best, seg_dist(ux, uy, segs[i]));
      const float d = (best - it.p[3]) * k;
      return std::clamp(0.5f - d, 0.0f, 1.0f);
    }
    case kImage: {
      // The atlas's alpha, bilinear like the GPU's sampler; the colour is
      // white (glyphs) times the tint, so only coverage changes.
      const float tx = (px - it.rect[0]) / std::max(1.0f, it.rect[2] - it.rect[0]);
      const float ty = (py - it.rect[1]) / std::max(1.0f, it.rect[3] - it.rect[1]);
      const float u = (it.q[0] + (it.q[2] - it.q[0]) * tx) * kAtlasSize - 0.5f;
      const float v = (it.q[1] + (it.q[3] - it.q[1]) * ty) * kAtlasSize - 0.5f;
      const auto& a = atlas_pixels();
      auto at = [&](int x, int y) {
        x = std::clamp(x, 0, int(kAtlasSize) - 1), y = std::clamp(y, 0, int(kAtlasSize) - 1);
        return float(a[(size_t(y) * kAtlasSize + size_t(x)) * 4 + 3]) / 255.0f;
      };
      const int x0 = int(std::floor(u)), y0 = int(std::floor(v));
      const float fx = u - float(x0), fy = v - float(y0);
      return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
             (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
    }
    default:
      return 0;
  }
}

float to_linear_unit(uint32_t c, int shift) { return float((c >> shift) & 0xFF) / 255.0f; }

// An image quad's texel colour (nearest), times the tint in `c`.
void image_rgb(const Instance& it, float px, float py, float c[4]) {
  const float tx = (px - it.rect[0]) / std::max(1.0f, it.rect[2] - it.rect[0]);
  const float ty = (py - it.rect[1]) / std::max(1.0f, it.rect[3] - it.rect[1]);
  const int x = std::clamp(int((it.q[0] + (it.q[2] - it.q[0]) * tx) * kAtlasSize), 0, int(kAtlasSize) - 1);
  const int y = std::clamp(int((it.q[1] + (it.q[3] - it.q[1]) * ty) * kAtlasSize), 0, int(kAtlasSize) - 1);
  const uint8_t* t = &atlas_pixels()[(size_t(y) * kAtlasSize + size_t(x)) * 4];
  for (int ch = 0; ch < 3; ++ch) c[ch] *= float(t[ch]) / 255.0f;
}

}  // namespace

void rasterize(std::span<const Instance> items, std::vector<uint8_t>& out, uint32_t width, uint32_t height) {
  out.resize(size_t(width) * height * 4);
  for (const Instance& it : items) {
    const int x0 = std::max(0, int(std::floor(it.rect[0]))), x1 = std::min(int(width), int(std::ceil(it.rect[2])));
    const int y0 = std::max(0, int(std::floor(it.rect[1]))), y1 = std::min(int(height), int(std::ceil(it.rect[3])));
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        float mix;
        const float cov = coverage(it, float(x) + 0.5f, float(y) + 0.5f, mix);
        if (cov <= 0) continue;
        float c[4];
        for (int ch = 0; ch < 4; ++ch) {
          const int shift = ch == 3 ? 24 : 16 - 8 * ch;
          c[ch] = to_linear_unit(it.color, shift) * (1 - mix) + to_linear_unit(it.color2, shift) * mix;
        }
        if (it.kind == kImage) image_rgb(it, float(x) + 0.5f, float(y) + 0.5f, c);
        const float a = c[3] * cov;
        uint8_t* p = &out[(size_t(y) * width + size_t(x)) * 4];
        for (int ch = 0; ch < 3; ++ch) p[ch] = uint8_t(std::lround(c[ch] * 255 * a + p[ch] * (1 - a)));
        p[3] = uint8_t(std::lround(255 * a + p[3] * (1 - a)));
      }
  }
}

}  // namespace carview::ui
