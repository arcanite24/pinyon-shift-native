#include "ui.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <tuple>

#include "fh1data/archive.h"
#include "fh1data/cjk_font.h"
#include "fh1data/ui_scene.h"
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

void List::turned(float& x, float& y) const {
  const float c = std::cos(turn_a_), s = std::sin(turn_a_), dx = x - turn_x_, dy = y - turn_y_;
  x = turn_x_ + c * dx - s * dy, y = turn_y_ + s * dx + c * dy;
}

void List::add_hit(float x0, float y0, float x1, float y1, uint32_t group, int index) {
  if (x1 < x0) std::swap(x0, x1);
  if (y1 < y0) std::swap(y0, y1);
  if (turn_a_ != 0.0f) {
    float xs[4] = {x0, x1, x1, x0};
    float ys[4] = {y0, y0, y1, y1};
    float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
    for (int k = 0; k < 4; ++k) {
      turned(xs[k], ys[k]);
      min_x = std::min(min_x, xs[k]);
      min_y = std::min(min_y, ys[k]);
      max_x = std::max(max_x, xs[k]);
      max_y = std::max(max_y, ys[k]);
    }
    x0 = min_x;
    y0 = min_y;
    x1 = max_x;
    y1 = max_y;
  }
  hits_.push_back({x0, y0, x1, y1, group, index});
}

bool hit_test_zones(std::span<const HitZone> zones, float mx, float my, HitZone& out) {
  for (size_t i = zones.size(); i > 0; --i) {
    const auto& z = zones[i - 1];
    if (mx >= z.x0 && mx <= z.x1 && my >= z.y0 && my <= z.y1) {
      out = z;
      return true;
    }
  }
  return false;
}

void List::rect(float x0, float y0, float x1, float y1, uint32_t color, float radius, uint32_t color2) {
  if (turn_a_ != 0) {
    std::array<float, 4> white;
    static const std::array<uint8_t, 64> kWhite = [] { std::array<uint8_t, 64> w; w.fill(255); return w; }();
    if (radius <= 0 && (!color2 || color2 == color) &&
        (atlas_find("ui.white", white) || atlas_image("ui.white", 4, 4, kWhite, white))) {
      float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
      turned(cx, cy);
      const float du = 0.25f * (white[2] - white[0]), dv = 0.25f * (white[3] - white[1]);
      const float a = turn_a_;
      turn_a_ = 0;
      image_rotated(cx, cy, 0.5f * (x1 - x0), 0.5f * (y1 - y0), a, {white[0] + du, white[1] + dv, white[2] - du, white[3] - dv}, color);
      turn_a_ = a;
      return;
    }
    float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1), ox = cx, oy = cy;
    turned(cx, cy);
    x0 += cx - ox, x1 += cx - ox, y0 += cy - oy, y1 += cy - oy;
  }
  items_.push_back({{x0, y0, x1, y1}, {radius, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, color,
                    color2 ? color2 : color, kRoundRect, 0});
}

void List::frame(float x0, float y0, float x1, float y1, uint32_t color, float width, float radius) {
  if (turn_a_ != 0 && radius <= 0.0f) {
    rect(x0, y0, x1, y0 + width, color);
    rect(x0, y1 - width, x1, y1, color);
    rect(x0, y0 + width, x0 + width, y1 - width, color);
    rect(x1 - width, y0 + width, x1, y1 - width, color);
    return;
  }
  items_.push_back({{x0, y0, x1, y1}, {radius, width, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, color, color,
                    kRoundRect, 0});
}

void List::shadow(float x0, float y0, float x1, float y1, uint32_t color, float radius, float blur) {
  items_.push_back({{x0 - blur - 1, y0 - blur - 1, x1 + blur + 1, y1 + blur + 1},
                    {radius, 0, blur + 1, 0},
                    {blur, 0, 0, 0},
                    {0, 0, 0, 0},
                    {0, 0, 0, 0},
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
                    {0, 0, 0, 0},
                    {0, 0, 0, 0},
                    color,
                    color,
                    kLine,
                    0});
}

void List::circle(float cx, float cy, float r, uint32_t color) { rect(cx - r, cy - r, cx + r, cy + r, color, r); }

void List::arc(float cx, float cy, float radius, float width, float a0, float a1, uint32_t color, bool square) {
  if (a1 <= a0) return;
  const float h = radius + 0.5f * width + 1;
  items_.push_back({{cx - h, cy - h, cx + h, cy + h}, {cx, cy, radius, 0.5f * width},
                    {a0, a1, square ? 1.0f : 0.0f, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, color, color, kArc, 0});
}

void List::image(float x0, float y0, float x1, float y1, std::array<float, 4> uv, uint32_t tint, float radius) {
  if (turn_a_ != 0) return image_rotated(0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (x1 - x0), 0.5f * (y1 - y0), 0, uv, tint);
  items_.push_back({{x0, y0, x1, y1}, {radius, 0, 0, 0}, {uv[0], uv[1], uv[2], uv[3]}, {0, 0, 0, 0}, {0, 0, 0, 0},
                    tint, tint, kImage, 0});
}

void List::fullscreen_backdrop(float x0, float y0, float x1, float y1, uint32_t color) {
  items_.push_back({{x0, y0, x1, y1}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, color, color,
                    kRoundRect, kUiFlagFullscreen});
}

void List::fullscreen_band(float x0, float y0, float x1, float y1, uint32_t color, uint32_t color2) {
  items_.push_back({{x0, y0, x1, y1}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, color,
                    color2 ? color2 : color, kRoundRect, kUiFlagFullscreenX});
}

void List::fullscreen_cover_image(float x0, float y0, float x1, float y1, std::array<float, 4> uv,
                                  std::array<float, 4> full_uv, float content_aspect, uint32_t tint) {
  items_.push_back({{x0, y0, x1, y1},
                    {0, 0, 0, 0},
                    {uv[0], uv[1], uv[2], uv[3]},
                    {content_aspect, 0, 0, 0},
                    {full_uv[0], full_uv[1], full_uv[2], full_uv[3]},
                    tint,
                    tint,
                    kImage,
                    kUiFlagFullscreenCover});
}

void List::image_rotated(float cx, float cy, float hw, float hh, float angle, std::array<float, 4> uv, uint32_t tint) {
  if (hw <= 0 || hh <= 0) return;
  if (turn_a_ != 0) turned(cx, cy), angle += turn_a_;
  const float c = std::fabs(std::cos(angle)), s = std::fabs(std::sin(angle));
  const float ex = c * hw + s * hh + 1, ey = s * hw + c * hh + 1;
  items_.push_back({{cx - ex, cy - ey, cx + ex, cy + ey}, {0, hw, hh, angle}, {uv[0], uv[1], uv[2], uv[3]},
                    {0, 0, 0, 0}, {0, 0, 0, 0}, tint, tint, kImage, 0});
}

void List::image_masked(float x0, float y0, float x1, float y1, std::array<float, 4> uv0, std::array<float, 4> uv1,
                        uint32_t tint, std::array<float, 4> uv0_xform, uint32_t flags, float radius) {
  if (turn_a_ != 0)
    return image_masked_rotated(0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (x1 - x0), 0.5f * (y1 - y0), 0, uv0, uv1,
                                tint, uv0_xform, flags);
  items_.push_back({{x0, y0, x1, y1},
                    {radius, 0, 0, 0},
                    {uv0[0], uv0[1], uv0[2], uv0[3]},
                    {uv1[0], uv1[1], uv1[2], uv1[3]},
                    {uv0_xform[0], uv0_xform[1], uv0_xform[2], uv0_xform[3]},
                    tint,
                    tint,
                    kImageMasked,
                    flags});
}

void List::image_masked_rotated(float cx, float cy, float hw, float hh, float angle, std::array<float, 4> uv0,
                                std::array<float, 4> uv1, uint32_t tint, std::array<float, 4> uv0_xform,
                                uint32_t flags) {
  if (hw <= 0 || hh <= 0) return;
  if (turn_a_ != 0) turned(cx, cy), angle += turn_a_;
  const float c = std::fabs(std::cos(angle)), s = std::fabs(std::sin(angle));
  const float ex = c * hw + s * hh + 1, ey = s * hw + c * hh + 1;
  items_.push_back({{cx - ex, cy - ey, cx + ex, cy + ey},
                    {0, hw, hh, angle},
                    {uv0[0], uv0[1], uv0[2], uv0[3]},
                    {uv1[0], uv1[1], uv1[2], uv1[3]},
                    {uv0_xform[0], uv0_xform[1], uv0_xform[2], uv0_xform[3]},
                    tint,
                    tint,
                    kImageMasked,
                    flags});
}

// The vector fonts and their glyph atlas.
namespace {

constexpr int kNumVFonts = 11;  // 0:A, 1:B, 2:E, 3:D, 4:C, 5:SYM, 6:DG1, 7:DG2, 8:DG3, 9:DG4, 10:DG5
constexpr int kCjkCacheFontId = 16;
// Number of frames a glyph or transient shelf must remain unused before its
// atlas texels may be recycled, strictly exceeding kMaxFramesInFlight (2..3)
// so no in-flight GPU frame ever samples overwritten texels.
constexpr uint64_t kAtlasInFlightGuard = 4;

using GlyphKey = std::tuple<int, uint32_t, int>;  // (font, code point, px)

struct GlyphShelf {
  uint32_t y = 0;
  uint32_t h = 0;
  uint32_t x = 0;
  uint64_t last_frame = 0;
  uint64_t last_tick = 0;
  std::vector<GlyphKey> keys;
};

struct TransientShelf {
  uint32_t y = 0;
  uint32_t h = 0;
  uint32_t x = 0;
  uint64_t last_frame = 0;
  uint64_t last_tick = 0;
  std::vector<std::string> keys;
};

struct VectorFonts {
  std::shared_ptr<const fh1::VFont> font[kNumVFonts];
  fh1::UiFontMap font_map;
  std::shared_ptr<const fh1::CjkFont> cjk;    // CHTB / CHSB / JPB / KORB
  uint32_t atlas_size = kAtlasSize;
  uint32_t glyph_rows = kGlyphRows;
  uint32_t transient_y0 = kTransientY0;
  bool gpu_mode = false;
  std::vector<uint8_t> atlas;  // lazily allocated only when !gpu_mode (or atlas_pixels() requested)
  std::vector<std::array<uint32_t, 4>> dirty;
  std::vector<AtlasPatch> dirty_patches;
  uint32_t glyph_y_next = 0;
  std::vector<GlyphShelf> glyph_shelves;
  uint64_t glyph_evictions = 0;
  uint32_t image_x = 0, image_y = kGlyphRows, image_h = 0;  // persistent image shelf (glyph_rows..transient_y0)
  uint32_t trans_y_next = kTransientY0;
  // 0 until atlas_begin_frame() is called (offline CPU/test mode permits LRU eviction
  // within a single call batch); live rendering increments this once per frame (1, 2, ...).
  uint64_t frame_tick = 0;
  uint64_t lru_tick = 1;
  uint64_t transient_evictions = 0;
  std::vector<TransientShelf> trans_shelves;
  std::map<std::string, int> trans_key_shelf;  // key -> index into trans_shelves
  std::map<std::string, std::array<float, 4>> images;
  struct Entry {
    float u0, v0, u1, v1;  // atlas texels
    float left, top;       // the bitmap's top-left from the pen, px at the cached size (y down from the baseline)
    float advance;         // px advance (used by CJK bitmap glyphs)
    int px = 1;            // rasterized pixel size for scaling
    int shelf = -1;        // index into glyph_shelves
    bool empty = false;
  };
  // font index: 0..10 VFont, 16 CjkFont.
  std::map<GlyphKey, Entry> cache;

  void ensure_cpu_atlas() {
    const size_t want = size_t(atlas_size) * atlas_size * 4;
    if (atlas.size() != want) {
      atlas.assign(want, uint8_t(0));
      const size_t glyph_bytes = size_t(atlas_size) * glyph_rows * 4;
      for (size_t i = 0; i < glyph_bytes; i += 4) atlas[i] = atlas[i + 1] = atlas[i + 2] = 255;
    }
  }

  // Clears the glyph rows (the images stay). Only called when font tables are
  // explicitly swapped/reloaded, NEVER mid-frame during glyph rasterization.
  void reset() {
    const uint32_t used_h = std::min(glyph_rows, glyph_y_next);
    if (!gpu_mode) {
      ensure_cpu_atlas();
      const size_t glyph_bytes = size_t(atlas_size) * glyph_rows * 4;
      std::fill(atlas.begin(), atlas.begin() + ptrdiff_t(glyph_bytes), uint8_t(0));
      for (size_t i = 0; i < glyph_bytes; i += 4) atlas[i] = atlas[i + 1] = atlas[i + 2] = 255;
      if (used_h > 0) dirty.push_back({0, 0, atlas_size, used_h});
    } else if (used_h > 0) {
      AtlasPatch p;
      p.x = 0, p.y = 0, p.w = atlas_size, p.h = used_h;
      p.rgba.assign(size_t(atlas_size) * used_h * 4, uint8_t(0));
      for (size_t i = 0; i < p.rgba.size(); i += 4) p.rgba[i] = p.rgba[i + 1] = p.rgba[i + 2] = 255;
      dirty_patches.push_back(std::move(p));
    }
    cache.clear();
    glyph_shelves.clear();
    glyph_y_next = 0;
  }

  void clear_transient() {
    for (const auto& sh : trans_shelves) {
      for (const auto& k : sh.keys) {
        images.erase(k);
        trans_key_shelf.erase(k);
        ++transient_evictions;
      }
    }
    trans_shelves.clear();
    trans_y_next = transient_y0;
  }
};

VectorFonts& vf() {
  static VectorFonts v;
  return v;
}

bool is_auto_transient_key(const std::string& key) {
  std::string up;
  up.reserve(std::min<size_t>(key.size(), 32));
  for (size_t i = 0; i < key.size() && i < 32; ++i)
    up.push_back(char(std::toupper(static_cast<unsigned char>(key[i]))));
  return up.rfind("EVENTTICKETS/", 0) == 0 || up.rfind("T:THUMBNAILS/", 0) == 0 || up.rfind("LOADING/", 0) == 0 ||
         up.rfind("T:UI4/UPGRADES/", 0) == 0 || up.rfind("T:UI4/LOGOS/", 0) == 0 ||
         up.rfind("T:UI4/DYNAMICLOADING/", 0) == 0 || up.rfind("SPONSORLOGOTILE/", 0) == 0 ||
         up.rfind("FMV.", 0) == 0 || up.rfind("INSPECTOR.", 0) == 0;
}

}  // namespace

namespace {
// Copies w x h pixels to (ax, ay) with one texel of edge padding all round,
// so bilinear taps stay inside.
void atlas_put(VectorFonts& v, uint32_t ax, uint32_t ay, uint32_t w, uint32_t h, std::span<const uint8_t> rgba) {
  if (!w || !h || ax < 1 || ay < 1 || ax + w + 1 > v.atlas_size || ay + h + 1 > v.atlas_size) return;
  const uint32_t pw = w + 2, ph = h + 2;
  if (v.gpu_mode) {
    AtlasPatch patch;
    patch.x = ax - 1;
    patch.y = ay - 1;
    patch.w = pw;
    patch.h = ph;
    patch.rgba.resize(size_t(pw) * ph * 4);
    for (int y = -1; y <= int(h); ++y) {
      const uint32_t sy = uint32_t(std::clamp(y, 0, int(h) - 1));
      uint8_t* dst_row = &patch.rgba[size_t(y + 1) * pw * 4];
      const uint8_t* src_row = &rgba[size_t(sy) * w * 4];
      std::memcpy(dst_row, src_row, 4);
      std::memcpy(dst_row + 4, src_row, size_t(w) * 4);
      std::memcpy(dst_row + (size_t(w) + 1) * 4, src_row + (size_t(w) - 1) * 4, 4);
    }
    v.dirty_patches.push_back(std::move(patch));
    return;
  }
  v.ensure_cpu_atlas();
  for (int y = -1; y <= int(h); ++y)
    for (int x = -1; x <= int(w); ++x) {
      const uint32_t sx = uint32_t(std::clamp(x, 0, int(w) - 1)), sy = uint32_t(std::clamp(y, 0, int(h) - 1));
      std::memcpy(&v.atlas[((size_t(int(ay) + y)) * v.atlas_size + size_t(int(ax) + x)) * 4],
                  &rgba[(size_t(sy) * w + sx) * 4], 4);
    }
  v.dirty.push_back({ax - 1, ay - 1, pw, ph});
}

bool atlas_alloc_transient(VectorFonts& v, const std::string& key, uint32_t w, uint32_t h,
                           std::span<const uint8_t> rgba, std::array<float, 4>& uv) {
  const uint32_t need_w = w + 2, need_h = h + 2;
  if (need_w > v.atlas_size || need_h > (v.atlas_size - v.transient_y0)) return false;

  // 1. Try appending to an existing transient shelf with enough remaining width and compatible height.
  int best_shelf = -1;
  uint32_t best_waste = 0xFFFFFFFFu;
  for (int i = 0; i < int(v.trans_shelves.size()); ++i) {
    const auto& sh = v.trans_shelves[size_t(i)];
    if (sh.x + need_w <= v.atlas_size && sh.h >= need_h) {
      const uint32_t waste = sh.h - need_h;
      if (waste < best_waste) {
        best_waste = waste;
        best_shelf = i;
      }
    }
  }
  if (best_shelf >= 0) {
    auto& sh = v.trans_shelves[size_t(best_shelf)];
    const uint32_t ax = sh.x + 1, ay = sh.y + 1;
    atlas_put(v, ax, ay, w, h, rgba);
    sh.x += need_w;
    sh.last_frame = v.frame_tick;
    sh.last_tick = ++v.lru_tick;
    sh.keys.push_back(key);
    const float k = 1.0f / float(v.atlas_size);
    uv = {float(ax) * k, float(ay) * k, float(ax + w) * k, float(ay + h) * k};
    v.images[key] = uv;
    v.trans_key_shelf[key] = best_shelf;
    return true;
  }

  // 2. Open a new transient shelf if vertical space remains.
  if (v.trans_y_next + need_h <= v.atlas_size) {
    TransientShelf sh;
    sh.y = v.trans_y_next;
    sh.h = need_h;
    sh.x = need_w;
    sh.last_frame = v.frame_tick;
    sh.last_tick = ++v.lru_tick;
    sh.keys.push_back(key);
    const uint32_t ax = 1, ay = sh.y + 1;
    atlas_put(v, ax, ay, w, h, rgba);
    v.trans_y_next += need_h;
    const int idx = int(v.trans_shelves.size());
    v.trans_shelves.push_back(std::move(sh));
    const float k = 1.0f / float(v.atlas_size);
    uv = {float(ax) * k, float(ay) * k, float(ax + w) * k, float(ay + h) * k};
    v.images[key] = uv;
    v.trans_key_shelf[key] = idx;
    return true;
  }

  // 3. LRU eviction: prefer recycling the oldest transient shelf not sampled
  // by an in-flight GPU frame (last_frame + kAtlasInFlightGuard <= frame_tick),
  // falling back only to shelves from an earlier frame (NEVER evicting a shelf
  // already referenced in the current frame!).
  int victim = -1;
  uint64_t oldest = UINT64_MAX;
  for (int pass = 0; pass < 2 && victim < 0; ++pass) {
    for (int i = 0; i < int(v.trans_shelves.size()); ++i) {
      const auto& sh = v.trans_shelves[size_t(i)];
      if (sh.h < need_h) continue;
      if (pass == 0 && sh.last_frame + kAtlasInFlightGuard > v.frame_tick) continue;
      if (pass == 1 && v.frame_tick > 0 && sh.last_frame >= v.frame_tick) continue;
      if (sh.last_tick < oldest) {
        oldest = sh.last_tick;
        victim = i;
      }
    }
  }
  if (victim >= 0) {
    auto& sh = v.trans_shelves[size_t(victim)];
    for (const auto& old_k : sh.keys) {
      v.images.erase(old_k);
      v.trans_key_shelf.erase(old_k);
      ++v.transient_evictions;
    }
    sh.keys.clear();
    sh.x = need_w;
    sh.last_frame = v.frame_tick;
    sh.last_tick = ++v.lru_tick;
    sh.keys.push_back(key);
    const uint32_t ax = 1, ay = sh.y + 1;
    atlas_put(v, ax, ay, w, h, rgba);
    const float k = 1.0f / float(v.atlas_size);
    uv = {float(ax) * k, float(ay) * k, float(ax + w) * k, float(ay + h) * k};
    v.images[key] = uv;
    v.trans_key_shelf[key] = victim;
    return true;
  }

  // 4. Merge a contiguous run of idle transient shelves (plus trailing space when
  // reaching the last shelf) so tall transient artwork (e.g. 1280x720 FMV/loading)
  // can reclaim fragmented rows without touching any live current-frame shelf.
  for (int pass = 0; pass < 2; ++pass) {
    int best_lo = -1, best_hi = -1;
    uint64_t best_max_tick = UINT64_MAX;
    const int n_sh = int(v.trans_shelves.size());
    for (int lo = 0; lo < n_sh; ++lo) {
      uint32_t span_h = 0;
      uint64_t max_tick = 0;
      for (int hi = lo; hi < n_sh; ++hi) {
        const auto& sh = v.trans_shelves[size_t(hi)];
        if (pass == 0 && sh.last_frame + kAtlasInFlightGuard > v.frame_tick) break;
        if (pass == 1 && v.frame_tick > 0 && sh.last_frame >= v.frame_tick) break;
        span_h += sh.h;
        max_tick = std::max(max_tick, sh.last_tick);
        const uint32_t avail_h = span_h + (hi == n_sh - 1 ? (v.atlas_size - v.trans_y_next) : 0u);
        if (avail_h >= need_h) {
          if (max_tick < best_max_tick) {
            best_max_tick = max_tick;
            best_lo = lo;
            best_hi = hi;
          }
          break;
        }
      }
    }
    if (best_lo >= 0) {
      uint32_t merged_h = 0;
      for (int i = best_lo; i <= best_hi; ++i) {
        auto& sh = v.trans_shelves[size_t(i)];
        merged_h += sh.h;
        for (const auto& old_k : sh.keys) {
          v.images.erase(old_k);
          v.trans_key_shelf.erase(old_k);
          ++v.transient_evictions;
        }
      }
      const bool touches_end = (best_hi == n_sh - 1);
      auto& dst = v.trans_shelves[size_t(best_lo)];
      dst.keys.clear();
      dst.h = touches_end ? need_h : merged_h;
      dst.x = need_w;
      dst.last_frame = v.frame_tick;
      dst.last_tick = ++v.lru_tick;
      dst.keys.push_back(key);
      if (touches_end) v.trans_y_next = dst.y + dst.h;
      const int removed = best_hi - best_lo;
      if (removed > 0) {
        v.trans_shelves.erase(v.trans_shelves.begin() + best_lo + 1, v.trans_shelves.begin() + best_hi + 1);
        for (auto& [tk, si] : v.trans_key_shelf) {
          if (si > best_hi) si -= removed;
        }
      }
      const uint32_t ax = 1, ay = dst.y + 1;
      atlas_put(v, ax, ay, w, h, rgba);
      const float k = 1.0f / float(v.atlas_size);
      uv = {float(ax) * k, float(ay) * k, float(ax + w) * k, float(ay + h) * k};
      v.images[key] = uv;
      v.trans_key_shelf[key] = best_lo;
      return true;
    }
  }

  // 5. Full transient compaction reset — ONLY when zero transient shelves were
  // sampled on the current frame (so an already-queued quad in this frame never
  // has its atlas texels overwritten before vkCmdDraw!). Never touches rows
  // 0..glyph_rows (font glyphs) or glyph_rows..transient_y0 (persistent chrome).
  for (const auto& sh : v.trans_shelves) {
    if (v.frame_tick > 0 && sh.last_frame >= v.frame_tick) return false;
  }
  v.clear_transient();
  TransientShelf sh;
  sh.y = v.trans_y_next;
  sh.h = need_h;
  sh.x = need_w;
  sh.last_frame = v.frame_tick;
  sh.last_tick = ++v.lru_tick;
  sh.keys.push_back(key);
  const uint32_t ax = 1, ay = sh.y + 1;
  atlas_put(v, ax, ay, w, h, rgba);
  v.trans_y_next += need_h;
  v.trans_shelves.push_back(std::move(sh));
  const float k = 1.0f / float(v.atlas_size);
  uv = {float(ax) * k, float(ay) * k, float(ax + w) * k, float(ay + h) * k};
  v.images[key] = uv;
  v.trans_key_shelf[key] = 0;
  return true;
}
}  // namespace

bool atlas_update(const std::string& key, uint32_t w, uint32_t h, std::span<const uint8_t> rgba,
                  std::array<float, 4>& uv) {
  VectorFonts& v = vf();
  if (auto it = v.images.find(key); it != v.images.end()) {
    const auto& u = it->second;
    const uint32_t ax = uint32_t(std::lround(u[0] * v.atlas_size)), ay = uint32_t(std::lround(u[1] * v.atlas_size));
    const uint32_t ew = uint32_t(std::lround(u[2] * v.atlas_size)) - ax, eh = uint32_t(std::lround(u[3] * v.atlas_size)) - ay;
    if (ew == w && eh == h && rgba.size() >= size_t(w) * h * 4) {
      atlas_put(v, ax, ay, w, h, rgba);
      if (auto sit = v.trans_key_shelf.find(key); sit != v.trans_key_shelf.end() &&
          size_t(sit->second) < v.trans_shelves.size()) {
        v.trans_shelves[size_t(sit->second)].last_frame = v.frame_tick;
        v.trans_shelves[size_t(sit->second)].last_tick = ++v.lru_tick;
      }
      return uv = u, true;
    }
    v.images.erase(it);  // another size: release old transient key metadata before reallocating
    if (auto sit = v.trans_key_shelf.find(key); sit != v.trans_key_shelf.end()) {
      const int idx = sit->second;
      v.trans_key_shelf.erase(sit);
      if (idx >= 0 && size_t(idx) < v.trans_shelves.size()) {
        auto& sh = v.trans_shelves[size_t(idx)];
        sh.keys.erase(std::remove(sh.keys.begin(), sh.keys.end(), key), sh.keys.end());
        if (sh.keys.empty()) {
          sh.x = 0;
          sh.last_frame = 0;
          sh.last_tick = 0;
        }
      }
    }
  }
  return atlas_image(key, w, h, rgba, uv);
}

bool atlas_image(const std::string& key, uint32_t w, uint32_t h, std::span<const uint8_t> rgba,
                 std::array<float, 4>& uv, bool transient) {
  VectorFonts& v = vf();
  if (auto it = v.images.find(key); it != v.images.end()) {
    if (auto sit = v.trans_key_shelf.find(key); sit != v.trans_key_shelf.end() &&
        size_t(sit->second) < v.trans_shelves.size()) {
      v.trans_shelves[size_t(sit->second)].last_frame = v.frame_tick;
      v.trans_shelves[size_t(sit->second)].last_tick = ++v.lru_tick;
    }
    return uv = it->second, true;
  }
  if (!w || !h || w + 2 > v.atlas_size || rgba.size() < size_t(w) * h * 4) return false;
  if (transient || is_auto_transient_key(key)) {
    return atlas_alloc_transient(v, key, w, h, rgba, uv);
  }
  if (v.image_x + w + 2 > v.atlas_size) v.image_x = 0, v.image_y += v.image_h, v.image_h = 0;
  if (v.image_y + h + 2 > v.transient_y0) {
    // Persistent shelf full: spill gracefully into transient LRU cache rather than failing!
    return atlas_alloc_transient(v, key, w, h, rgba, uv);
  }
  const uint32_t ax = v.image_x + 1, ay = v.image_y + 1;
  atlas_put(v, ax, ay, w, h, rgba);
  v.image_x += w + 2;
  v.image_h = std::max(v.image_h, h + 2);
  const float k = 1.0f / float(v.atlas_size);
  uv = {float(ax) * k, float(ay) * k, float(ax + w) * k, float(ay + h) * k};
  v.images[key] = uv;
  return true;
}

bool atlas_find(const std::string& key, std::array<float, 4>& uv) {
  VectorFonts& v = vf();
  if (auto it = v.images.find(key); it != v.images.end()) {
    if (auto sit = v.trans_key_shelf.find(key); sit != v.trans_key_shelf.end() &&
        size_t(sit->second) < v.trans_shelves.size()) {
      v.trans_shelves[size_t(sit->second)].last_frame = v.frame_tick;
      v.trans_shelves[size_t(sit->second)].last_tick = ++v.lru_tick;
    }
    return uv = it->second, true;
  }
  return false;
}

uint64_t atlas_transient_evictions() { return vf().transient_evictions; }
uint64_t atlas_glyph_evictions() { return vf().glyph_evictions; }
void atlas_clear_transient() { vf().clear_transient(); }
void atlas_begin_frame() { ++vf().frame_tick; }

AtlasStats atlas_stats() {
  const VectorFonts& v = vf();
  AtlasStats s{};
  s.atlas_size = v.atlas_size;
  s.glyph_rows_used = std::min<uint32_t>(v.glyph_rows, v.glyph_y_next);
  const uint32_t pers_end = std::max<uint32_t>(v.glyph_rows, v.image_y + v.image_h);
  s.persistent_rows_used = std::min<uint32_t>(v.transient_y0 - v.glyph_rows, pers_end - v.glyph_rows);
  s.transient_rows_used = std::min<uint32_t>(v.atlas_size - v.transient_y0,
                                             v.trans_y_next > v.transient_y0 ? v.trans_y_next - v.transient_y0 : 0u);
  const uint32_t total_rows = s.glyph_rows_used + s.persistent_rows_used + s.transient_rows_used;
  s.utilization_pct = v.atlas_size > 0 ? 100.0 * double(total_rows) / double(v.atlas_size) : 0.0;
  s.transient_evictions = v.transient_evictions;
  s.glyph_evictions = v.glyph_evictions;
  s.glyph_count = uint32_t(v.cache.size());
  s.image_count = uint32_t(v.images.size());
  return s;
}

namespace {

int font_slot(ui::Font role) {
  switch (role) {
    case ui::Font::kA: return 0;
    case ui::Font::kB: return 1;
    case ui::Font::kE: return 2;
    case ui::Font::kD: return 3;
    case ui::Font::kC: return 4;
    case ui::Font::kSym: return 5;
    case ui::Font::kDg1: return 6;
    case ui::Font::kDg2: return 7;
    case ui::Font::kDg3: return 8;
    case ui::Font::kDg4: return 9;
    case ui::Font::kDg5: return 10;
    default: return -1;
  }
}

// The style's font: 0..10 VFont, -1 the stroke font.
int font_of(const TextStyle& s) {
  if (s.font == ui::Font::kStroke) return -1;
  int f = s.font == ui::Font::kAuto ? (s.slant > 0.05f ? 2 : 1) : font_slot(s.font);
  if (f >= 0 && f < kNumVFonts && vf().font[f]) return f;
  // Graceful fallback to B when a specialized font (C, SYM, DG1..5) wasn't loaded.
  if (f >= 4 && vf().font[1]) return 1;
  return -1;
}

// The next code point of UTF-8 text (full Unicode Latin-1/Extended-A/Cyrillic
// uppercase folding when `!keep_case`); 0xFFFD for a bad byte.
uint32_t next_code_point(std::string_view t, size_t& i, bool keep_case = false) {
  const auto b = static_cast<unsigned char>(t[i++]);
  if (b < 0x80) return !keep_case && b >= 'a' && b <= 'z' ? uint32_t(b - 32) : b;
  int n = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : -1;
  if (n < 0) return 0xFFFD;
  uint32_t c = b & (0x3F >> n);
  for (; n > 0 && i < t.size(); --n) c = c << 6 | (static_cast<unsigned char>(t[i++]) & 0x3F);
  return keep_case ? c : ui::unicode_to_upper_cp(c);
}

void append_utf8_cp(std::string& out, uint32_t cp) {
  if (cp < 0x80u) {
    out.push_back(char(cp));
  } else if (cp < 0x800u) {
    out.push_back(char(0xC0u | (cp >> 6)));
    out.push_back(char(0x80u | (cp & 0x3Fu)));
  } else if (cp < 0x10000u) {
    out.push_back(char(0xE0u | (cp >> 12)));
    out.push_back(char(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(char(0x80u | (cp & 0x3Fu)));
  } else {
    out.push_back(char(0xF0u | (cp >> 18)));
    out.push_back(char(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(char(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(char(0x80u | (cp & 0x3Fu)));
  }
}

// Finds a glyph in `preferred_slot`, falling back across all other loaded VFonts
// before dropping to CJK/stroke/space so accented European & Cyrillic glyphs render cleanly.
const fh1::VFontGlyph* find_vfont_glyph(int preferred_slot, uint32_t cp, int& out_slot) {
  if (cp == ' ') return nullptr;
  const VectorFonts& v = vf();
  if (preferred_slot >= 0 && preferred_slot < kNumVFonts && v.font[preferred_slot]) {
    if (const fh1::VFontGlyph* g = v.font[preferred_slot]->find(cp)) {
      out_slot = preferred_slot;
      return g;
    }
  }
  static constexpr int kFallbackOrder[kNumVFonts] = {1, 0, 3, 2, 4, 5, 6, 7, 8, 9, 10};
  for (int cand : kFallbackOrder) {
    if (cand == preferred_slot || !v.font[cand]) continue;
    if (const fh1::VFontGlyph* g = v.font[cand]->find(cp)) {
      out_slot = cand;
      return g;
    }
  }
  return nullptr;
}

// px per em for a cap height of `size` px.
float em_px(const fh1::VFont& f, float size) { return size / f.cap_height(); }

// Quantized glyph raster sizes (bilinearly sampled on GPU): 6 crisp buckets
// up to 64 px (never upscaling below 64 px) limit distinct atlas bitmaps per
// font across 1080p/1440p/4K/fit_text screens so glyphs stay hot with zero
// mid-frame evictions.
int cached_px(float px) {
  if (px <= 12.0f) return 12;
  if (px <= 18.0f) return 18;
  if (px <= 26.0f) return 26;
  if (px <= 38.0f) return 38;
  if (px <= 52.0f) return 52;
  return 64;
}

bool glyph_alloc_shelf(VectorFonts& v, uint32_t w, uint32_t h, const GlyphKey& key, uint32_t& out_ax, uint32_t& out_ay,
                       int& out_shelf) {
  const uint32_t need_w = w + 1;
  const uint32_t need_h = h + 1;
  if (need_w > v.atlas_size || need_h > v.glyph_rows) return false;

  // 1. Tight fit in an existing glyph shelf (height slack <= 12 px).
  int best = -1;
  uint32_t best_waste = 0xFFFFFFFFu;
  for (int i = 0; i < int(v.glyph_shelves.size()); ++i) {
    const auto& sh = v.glyph_shelves[size_t(i)];
    if (sh.x + need_w <= v.atlas_size && sh.h >= need_h) {
      const uint32_t waste = sh.h - need_h;
      if (waste <= 12u && waste < best_waste) {
        best_waste = waste;
        best = i;
      }
    }
  }
  if (best >= 0) {
    auto& sh = v.glyph_shelves[size_t(best)];
    out_ax = sh.x;
    out_ay = sh.y;
    out_shelf = best;
    sh.x += need_w;
    sh.last_frame = v.frame_tick;
    sh.last_tick = ++v.lru_tick;
    sh.keys.push_back(key);
    return true;
  }

  // 2. Open a new shelf inside rows 0..glyph_rows.
  const uint32_t shelf_h = std::min(v.glyph_rows - v.glyph_y_next, (need_h + 7u) & ~7u);
  if (v.glyph_y_next + need_h <= v.glyph_rows && shelf_h >= need_h) {
    GlyphShelf sh;
    sh.y = v.glyph_y_next;
    sh.h = shelf_h;
    sh.x = need_w;
    sh.last_frame = v.frame_tick;
    sh.last_tick = ++v.lru_tick;
    sh.keys.push_back(key);
    out_ax = 0;
    out_ay = sh.y;
    out_shelf = int(v.glyph_shelves.size());
    v.glyph_y_next += shelf_h;
    v.glyph_shelves.push_back(std::move(sh));
    return true;
  }

  // 3. Relaxed fit in any existing shelf with room.
  for (int i = 0; i < int(v.glyph_shelves.size()); ++i) {
    const auto& sh = v.glyph_shelves[size_t(i)];
    if (sh.x + need_w <= v.atlas_size && sh.h >= need_h) {
      const uint32_t waste = sh.h - need_h;
      if (waste < best_waste) {
        best_waste = waste;
        best = i;
      }
    }
  }
  if (best >= 0) {
    auto& sh = v.glyph_shelves[size_t(best)];
    out_ax = sh.x;
    out_ay = sh.y;
    out_shelf = best;
    sh.x += need_w;
    sh.last_frame = v.frame_tick;
    sh.last_tick = ++v.lru_tick;
    sh.keys.push_back(key);
    return true;
  }

  // 4. Multi-frame-safe LRU recycle: ONLY recycle a shelf that has been idle
  // for at least kAtlasInFlightGuard frames (never touching current or in-flight
  // GPU frames, and never wiping the rest of the glyph band!).
  int victim = -1;
  uint64_t oldest = UINT64_MAX;
  for (int i = 0; i < int(v.glyph_shelves.size()); ++i) {
    const auto& sh = v.glyph_shelves[size_t(i)];
    if (sh.h < need_h) continue;
    if (sh.last_frame + kAtlasInFlightGuard > v.frame_tick) continue;
    if (sh.last_tick < oldest) {
      oldest = sh.last_tick;
      victim = i;
    }
  }
  if (victim >= 0) {
    auto& sh = v.glyph_shelves[size_t(victim)];
    for (const auto& old_k : sh.keys) {
      v.cache.erase(old_k);
      ++v.glyph_evictions;
    }
    sh.keys.clear();
    sh.x = need_w;
    sh.last_frame = v.frame_tick;
    sh.last_tick = ++v.lru_tick;
    sh.keys.push_back(key);
    out_ax = 0;
    out_ay = sh.y;
    out_shelf = victim;
    return true;
  }

  // 5. Merge a contiguous run of idle glyph shelves (plus trailing space when
  // reaching the last shelf) so tall glyphs are never permanently starved when
  // the glyph band was initially filled by shorter shelves.
  int best_lo = -1, best_hi = -1;
  uint64_t best_max_tick = UINT64_MAX;
  const int n_sh = int(v.glyph_shelves.size());
  for (int lo = 0; lo < n_sh; ++lo) {
    uint32_t span_h = 0;
    uint64_t max_tick = 0;
    for (int hi = lo; hi < n_sh; ++hi) {
      const auto& sh = v.glyph_shelves[size_t(hi)];
      if (sh.last_frame + kAtlasInFlightGuard > v.frame_tick) break;
      span_h += sh.h;
      max_tick = std::max(max_tick, sh.last_tick);
      const uint32_t avail_h = span_h + (hi == n_sh - 1 ? (v.glyph_rows - v.glyph_y_next) : 0u);
      if (avail_h >= need_h) {
        if (max_tick < best_max_tick) {
          best_max_tick = max_tick;
          best_lo = lo;
          best_hi = hi;
        }
        break;
      }
    }
  }
  if (best_lo >= 0) {
    uint32_t merged_h = 0;
    for (int i = best_lo; i <= best_hi; ++i) {
      auto& sh = v.glyph_shelves[size_t(i)];
      merged_h += sh.h;
      for (const auto& old_k : sh.keys) {
        v.cache.erase(old_k);
        ++v.glyph_evictions;
      }
    }
    const bool touches_end = (best_hi == n_sh - 1);
    auto& dst = v.glyph_shelves[size_t(best_lo)];
    const uint32_t end_h = std::min(v.glyph_rows - dst.y, (need_h + 7u) & ~7u);
    dst.keys.clear();
    dst.h = touches_end ? std::max(need_h, end_h) : merged_h;
    dst.x = need_w;
    dst.last_frame = v.frame_tick;
    dst.last_tick = ++v.lru_tick;
    dst.keys.push_back(key);
    if (touches_end) v.glyph_y_next = dst.y + dst.h;
    const int removed = best_hi - best_lo;
    if (removed > 0) {
      v.glyph_shelves.erase(v.glyph_shelves.begin() + best_lo + 1, v.glyph_shelves.begin() + best_hi + 1);
      for (auto& [ck, ce] : v.cache) {
        if (ce.shelf > best_hi) ce.shelf -= removed;
      }
    }
    out_ax = 0;
    out_ay = dst.y;
    out_shelf = best_lo;
    return true;
  }
  return false;
}

const VectorFonts::Entry* find_fallback_glyph(VectorFonts& v, int font, uint32_t cp, int target_px) {
  const VectorFonts::Entry* best = nullptr;
  int best_diff = INT32_MAX;
  auto it = v.cache.lower_bound(std::make_tuple(font, cp, 0));
  for (; it != v.cache.end() && std::get<0>(it->first) == font && std::get<1>(it->first) == cp; ++it) {
    const int diff = std::abs(std::get<2>(it->first) - target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = &it->second;
    }
  }
  if (best && best->shelf >= 0 && size_t(best->shelf) < v.glyph_shelves.size()) {
    v.glyph_shelves[size_t(best->shelf)].last_frame = v.frame_tick;
    v.glyph_shelves[size_t(best->shelf)].last_tick = ++v.lru_tick;
  }
  return best;
}

const VectorFonts::Entry* glyph_entry(int font, uint32_t cp, int px, bool allow_step_down = true) {
  VectorFonts& v = vf();
  const auto key = std::make_tuple(font, cp, px);
  if (auto it = v.cache.find(key); it != v.cache.end()) {
    if (it->second.shelf >= 0 && size_t(it->second.shelf) < v.glyph_shelves.size()) {
      auto& sh = v.glyph_shelves[size_t(it->second.shelf)];
      sh.last_frame = v.frame_tick;
      sh.last_tick = ++v.lru_tick;
    }
    return &it->second;
  }
  const fh1::VFontGlyph* g = v.font[font]->find(cp);
  if (!g) return nullptr;
  const float k = float(px);
  const int pad = 2;
  const int gx0 = int(std::floor(g->min_x * k)) - pad, gx1 = int(std::ceil(g->max_x * k)) + pad;
  const int gy0 = int(std::floor(-g->max_y * k)) - pad, gy1 = int(std::ceil(-g->min_y * k)) + pad;
  const int w = gx1 - gx0, h = gy1 - gy0;
  if (w <= 0 || h <= 0 || w > int(v.atlas_size) || h > int(v.glyph_rows)) return nullptr;
  uint32_t ax = 0, ay = 0;
  int shelf_idx = -1;
  if (!glyph_alloc_shelf(v, uint32_t(w), uint32_t(h), key, ax, ay, shelf_idx)) {
    if (const auto* fb = find_fallback_glyph(v, font, cp, px)) return fb;
    if (allow_step_down) {
      static constexpr int kStepDown[] = {38, 26, 18, 12};
      for (int smaller : kStepDown) {
        if (smaller < px) {
          if (const auto* se = glyph_entry(font, cp, smaller, false)) return se;
        }
      }
    }
    return nullptr;
  }
  std::vector<uint8_t> cov;
  fh1::rasterize_vfont_glyph(*g, k, float(-gx0), float(-gy0), cov, w, h);
  if (v.gpu_mode) {
    AtlasPatch patch;
    patch.x = ax, patch.y = ay, patch.w = uint32_t(w), patch.h = uint32_t(h);
    patch.rgba.resize(size_t(w) * size_t(h) * 4, 255);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        patch.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4 + 3] = cov[size_t(y) * size_t(w) + size_t(x)];
    v.dirty_patches.push_back(std::move(patch));
  } else {
    v.ensure_cpu_atlas();
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        v.atlas[((size_t(ay) + size_t(y)) * v.atlas_size + ax + size_t(x)) * 4 + 3] =
            cov[size_t(y) * size_t(w) + size_t(x)];
    v.dirty.push_back({ax, ay, uint32_t(w), uint32_t(h)});
  }
  VectorFonts::Entry e{float(ax), float(ay), float(ax + uint32_t(w)), float(ay + uint32_t(h)), float(gx0), float(gy0),
                       0.0f, px, shelf_idx, false};
  return &(v.cache[key] = e);
}

int cjk_cached_px(float cap_size) {
  const float cell = std::clamp(cap_size * 1.32f, 8.0f, 64.0f);
  if (cell <= 32.0f) return std::max(8, int(std::lround(cell / 4.0f)) * 4);
  return std::min(64, int(std::lround(cell / 8.0f)) * 8);
}

const VectorFonts::Entry* cjk_glyph_entry(uint32_t cp, int cell_px) {
  VectorFonts& v = vf();
  if (!v.cjk || !v.cjk->valid()) return nullptr;
  const auto key = std::make_tuple(kCjkCacheFontId, cp, cell_px);
  if (auto it = v.cache.find(key); it != v.cache.end()) {
    if (it->second.shelf >= 0 && size_t(it->second.shelf) < v.glyph_shelves.size()) {
      auto& sh = v.glyph_shelves[size_t(it->second.shelf)];
      sh.last_frame = v.frame_tick;
      sh.last_tick = ++v.lru_tick;
    }
    return &it->second;
  }
  std::vector<uint8_t> cov;
  int w = 0, h = 0;
  float adv = 0.0f;
  if (!v.cjk->rasterize_scaled(cp, cell_px, cov, w, h, adv)) return nullptr;
  if (w <= 0 || h <= 0 || w > int(v.atlas_size) || h > int(v.glyph_rows)) return nullptr;
  uint32_t ax = 0, ay = 0;
  int shelf_idx = -1;
  if (!glyph_alloc_shelf(v, uint32_t(w), uint32_t(h), key, ax, ay, shelf_idx)) {
    return find_fallback_glyph(v, kCjkCacheFontId, cp, cell_px);
  }
  if (v.gpu_mode) {
    AtlasPatch patch;
    patch.x = ax, patch.y = ay, patch.w = uint32_t(w), patch.h = uint32_t(h);
    patch.rgba.resize(size_t(w) * size_t(h) * 4, 255);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        patch.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4 + 3] = cov[size_t(y) * size_t(w) + size_t(x)];
    v.dirty_patches.push_back(std::move(patch));
  } else {
    v.ensure_cpu_atlas();
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        v.atlas[((size_t(ay) + size_t(y)) * v.atlas_size + ax + size_t(x)) * 4 + 3] =
            cov[size_t(y) * size_t(w) + size_t(x)];
    v.dirty.push_back({ax, ay, uint32_t(w), uint32_t(h)});
  }
  // Position bitmap top relative to baseline using font's ascent ratio.
  const float ascent_ratio = float(v.cjk->ascent()) / float(std::max(1, v.cjk->cell_size()));
  const float top_from_base = -float(h) * (ascent_ratio - v.cjk->baseline_shift());
  VectorFonts::Entry e{float(ax), float(ay), float(ax + uint32_t(w)), float(ay + uint32_t(h)), 0.0f, top_from_base,
                       adv, cell_px, shelf_idx, false};
  return &(v.cache[key] = e);
}

float cjk_advance_px(uint32_t cp, float cap_size) {
  const auto& cjk = vf().cjk;
  if (!cjk || !cjk->valid()) return cap_size * 0.5f;
  const fh1::CjkGlyphMetrics* m = cjk->metrics(cp);
  if (!m) return cap_size * 0.5f;
  const float cell_px = std::clamp(cap_size * 1.32f, 8.0f, 96.0f);
  return std::max(2.0f, float(std::max(1, m->advance())) * (cell_px / float(std::max(1, cjk->cell_size()))));
}

struct StyledRun {
  std::string text;
  ui::Font font = ui::Font::kAuto;
  uint32_t color = 0;
};

struct StyledLine {
  std::vector<StyledRun> runs;
};

bool parse_style_tag(std::string_view tag, ui::Font base_font, uint32_t base_color, std::vector<ui::Font>& font_stack,
                     uint32_t& cur_color) {
  auto trim = [](std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\'' || s.front() == '"')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\'' || s.back() == '"')) s.remove_suffix(1);
    return s;
  };
  std::string low;
  low.reserve(tag.size());
  for (char c : tag) low.push_back(char(std::tolower(static_cast<unsigned char>(c))));

  if (low == "font_pop" || low == "font") {
    if (font_stack.size() > 1) font_stack.pop_back();
    else font_stack = {base_font};
    return true;
  }
  if (low == "colour_pop" || low == "color_pop" || low == "colour" || low == "color") {
    cur_color = base_color;
    return true;
  }
  if (low.rfind("font=", 0) == 0) {
    const std::string_view v = trim(std::string_view(low).substr(5));
    ui::Font nf = base_font;
    if (v == "a" || v == "horizon_a") nf = ui::Font::kA;
    else if (v == "b" || v == "horizon_b") nf = ui::Font::kB;
    else if (v == "c" || v == "horizon_c") nf = ui::Font::kC;
    else if (v == "d" || v == "horizon_d") nf = ui::Font::kD;
    else if (v == "e" || v == "horizon_e") nf = ui::Font::kE;
    else if (v == "sym" || v == "horizon_sym" || v == "symbols") nf = ui::Font::kSym;
    else if (v == "dg1" || v == "horizon_dg1") nf = ui::Font::kDg1;
    else if (v == "dg2" || v == "horizon_dg2") nf = ui::Font::kDg2;
    else if (v == "dg3" || v == "horizon_dg3") nf = ui::Font::kDg3;
    else if (v == "dg4" || v == "horizon_dg4") nf = ui::Font::kDg4;
    else if (v == "dg5" || v == "horizon_dg5") nf = ui::Font::kDg5;
    else {
      const ui::Font resolved = ui::resolve_font_name(v);
      if (resolved != ui::Font::kAuto) nf = resolved;
    }
    font_stack.push_back(nf);
    return true;
  }
  if (low.rfind("colour=", 0) == 0 || low.rfind("color=", 0) == 0) {
    const size_t eq = low.find('=');
    const std::string v(trim(std::string_view(low).substr(eq + 1)));
    int rgba_vals[4] = {255, 255, 255, 255};
    int n = 0;
    const char* p = v.c_str();
    while (*p && n < 4) {
      while (*p == ' ' || *p == ',' || *p == '\t') ++p;
      if (!*p) break;
      char* end = nullptr;
      const long val = std::strtol(p, &end, 10);
      if (end == p) break;
      rgba_vals[n++] = int(std::clamp(val, 0l, 255l));
      p = end;
    }
    if (n >= 3) {
      // Preserve base alpha factor when styling faded text.
      const uint32_t base_a = (base_color >> 24) & 0xFFu;
      const uint32_t a = (uint32_t(rgba_vals[3]) * base_a) / 255u;
      cur_color = rgba(uint8_t(rgba_vals[0]), uint8_t(rgba_vals[1]), uint8_t(rgba_vals[2]), uint8_t(a));
    }
    return true;
  }
  return false;
}

std::vector<StyledLine> parse_styled_lines(std::string_view text, const TextStyle& base) {
  std::vector<StyledLine> lines(1);
  std::vector<ui::Font> font_stack = {base.font};
  uint32_t cur_color = base.color;
  auto push_text = [&](std::string_view chunk) {
    if (chunk.empty()) return;
    auto& runs = lines.back().runs;
    const ui::Font cf = font_stack.back();
    if (!runs.empty() && runs.back().font == cf && runs.back().color == cur_color) {
      runs.back().text.append(chunk);
    } else {
      runs.push_back({std::string(chunk), cf, cur_color});
    }
  };

  size_t i = 0;
  size_t plain_start = 0;
  while (i < text.size()) {
    if (text[i] == '\n') {
      push_text(text.substr(plain_start, i - plain_start));
      lines.emplace_back();
      ++i;
      plain_start = i;
      continue;
    }
    if (text[i] == '{') {
      const bool dbl = (i + 1 < text.size() && text[i + 1] == '{');
      const size_t tag_start = i + (dbl ? 2 : 1);
      const size_t close = dbl ? text.find("}}", tag_start) : text.find('}', tag_start);
      if (close != std::string_view::npos && close - tag_start <= 64) {
        const std::string_view tag = text.substr(tag_start, close - tag_start);
        std::vector<ui::Font> test_stack = font_stack;
        uint32_t test_col = cur_color;
        if (parse_style_tag(tag, base.font, base.color, test_stack, test_col)) {
          push_text(text.substr(plain_start, i - plain_start));
          font_stack = std::move(test_stack);
          cur_color = test_col;
          i = close + (dbl ? 2 : 1);
          plain_start = i;
          continue;
        }
      }
    }
    ++i;
  }
  push_text(text.substr(plain_start, text.size() - plain_start));
  return lines;
}

float measure_run_single_line(std::string_view text, const TextStyle& s) {
  const int f = font_of(s);
  const bool have_cjk = vf().cjk && vf().cjk->valid();
  std::string folded_storage;
  std::string_view src = text;
  if (!s.keep_case) {
    folded_storage = ui::unicode_uppercase_utf8(text);
    src = folded_storage;
  }
  if (f >= 0) {
    const fh1::VFont& font = *vf().font[f];
    const float k = em_px(font, s.size);
    float w = 0;
    size_t n = 0;
    for (size_t i = 0; i < src.size();) {
      const uint32_t cp = next_code_point(src, i, true);
      int actual_slot = f;
      const fh1::VFontGlyph* g = find_vfont_glyph(f, cp, actual_slot);
      if (g) {
        const float gk = (actual_slot == f) ? k : em_px(*vf().font[actual_slot], s.size);
        w += g->advance * gk + s.tracking * s.size;
      } else if (cp != ' ' && have_cjk && vf().cjk->has_glyph(cp)) {
        w += cjk_advance_px(cp, s.size) + s.tracking * s.size;
      } else {
        w += font.space_width * k + s.tracking * s.size;
      }
      ++n;
    }
    if (n) w -= s.tracking * s.size;
    return w * s.stretch;
  }
  if (have_cjk) {
    float w = 0;
    size_t n = 0;
    const float r = s.weight * 10;
    for (size_t i = 0; i < src.size();) {
      const uint32_t cp = next_code_point(src, i, true);
      if (cp >= 0x80 && vf().cjk->has_glyph(cp)) {
        w += cjk_advance_px(cp, s.size) + s.tracking * s.size;
      } else {
        w += advance_units(cp < 128 ? glyph(char(cp)) : nullptr, r, s.tracking * 10) * (s.size / 10.0f);
      }
      ++n;
    }
    if (n) w -= s.tracking * s.size;
    return w;
  }
  const float r = s.weight * 10;
  float units = 0;
  size_t count = 0;
  for (size_t i = 0; i < src.size();) {
    const uint32_t cp = next_code_point(src, i, true);
    units += advance_units(cp < 128 ? glyph(char(cp)) : nullptr, r, s.tracking * 10);
    ++count;
  }
  if (count > 0) units -= 1.8f + s.tracking * 10;
  return units * s.size / 10;
}

}  // namespace

uint32_t unicode_to_upper_cp(uint32_t cp) {
  if (cp >= 'a' && cp <= 'z') return cp - 32u;
  // Latin-1 Supplement: à..ö (0xE0..0xF6), ø..þ (0xF8..0xFE), ÿ (0xFF -> 0x178)
  if (cp >= 0x00E0u && cp <= 0x00F6u) return cp - 0x20u;
  if (cp >= 0x00F8u && cp <= 0x00FEu) return cp - 0x20u;
  if (cp == 0x00FFu) return 0x0178u;
  // Latin Extended-A (Polish, Czech, Hungarian, Turkish, etc.)
  if (cp >= 0x0100u && cp <= 0x012Fu) return (cp & 1u) ? (cp - 1u) : cp;
  if (cp == 0x0131u) return 'I';
  if (cp >= 0x0132u && cp <= 0x0137u) return (cp & 1u) ? (cp - 1u) : cp;
  if (cp >= 0x0139u && cp <= 0x0148u) return ((cp & 1u) == 0u) ? (cp - 1u) : cp;
  if (cp >= 0x014Au && cp <= 0x0177u) return (cp & 1u) ? (cp - 1u) : cp;
  if (cp >= 0x0179u && cp <= 0x017Eu) return ((cp & 1u) == 0u) ? (cp - 1u) : cp;
  // Cyrillic: а..я (0x0430..0x044F), ѐ..џ (0x0450..0x045F, incl. ё -> Ё), ґ -> Ґ
  if (cp >= 0x0430u && cp <= 0x044Fu) return cp - 0x20u;
  if (cp >= 0x0450u && cp <= 0x045Fu) return cp - 0x50u;
  if (cp == 0x0491u) return 0x0490u;
  return cp;
}

std::string unicode_uppercase_utf8(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (size_t i = 0; i < text.size();) {
    const uint32_t cp = next_code_point(text, i, true);
    if (cp == 0x00DFu) {  // German sharp s: ß -> SS
      out.push_back('S');
      out.push_back('S');
    } else {
      append_utf8_cp(out, unicode_to_upper_cp(cp));
    }
  }
  return out;
}

void set_vector_fonts(std::shared_ptr<const fh1::VFont> a, std::shared_ptr<const fh1::VFont> b,
                      std::shared_ptr<const fh1::VFont> e, std::shared_ptr<const fh1::VFont> d) {
  VectorFonts& v = vf();
  v.font[0] = std::move(a), v.font[1] = std::move(b), v.font[2] = std::move(e), v.font[3] = std::move(d);
  v.reset();
}
void set_named_vector_font(ui::Font role, std::shared_ptr<const fh1::VFont> font) {
  const int slot = font_slot(role);
  if (slot < 0 || slot >= kNumVFonts) return;
  VectorFonts& v = vf();
  v.font[slot] = std::move(font);
  v.reset();
}
void set_font_map(const fh1::UiFontMap& map) { vf().font_map = map; }
ui::Font resolve_font_name(std::string_view font_name) {
  const std::string target = vf().font_map.resolve(font_name);
  if (target == "A") return ui::Font::kA;
  if (target == "B") return ui::Font::kB;
  if (target == "C") return ui::Font::kC;
  if (target == "D") return ui::Font::kD;
  if (target == "E") return ui::Font::kE;
  if (target == "SYM") return ui::Font::kSym;
  if (target == "DG1") return ui::Font::kDg1;
  if (target == "DG2") return ui::Font::kDg2;
  if (target == "DG3") return ui::Font::kDg3;
  if (target == "DG4") return ui::Font::kDg4;
  if (target == "DG5") return ui::Font::kDg5;
  return ui::Font::kB;
}
bool load_fonts_from_zip(fh1::Archive& fonts_zip) {
  auto load = [&](const char* name) -> std::shared_ptr<const fh1::VFont> {
    auto r = fonts_zip.read(name);
    if (!r.ok()) return nullptr;
    auto f = std::make_shared<fh1::VFont>();
    if (!fh1::parse_vfont(r.data, *f, nullptr)) return nullptr;
    return f;
  };
  if (auto fm = fonts_zip.read("fontmap.xml"); fm.ok()) {
    fh1::UiFontMap map;
    std::string_view xml(reinterpret_cast<const char*>(fm.data.data()), fm.data.size());
    if (fh1::parse_fontmap_xml(xml, map)) set_font_map(map);
  }
  VectorFonts& v = vf();
  auto a = load("aru_vector_aa.dt");
  if (!a) a = load("a_vector_aa.dt");
  auto b = load("bru_vector_aa.dt");
  if (!b) b = load("b_vector_aa.dt");
  auto e = load("eru_vector_aa.dt");
  if (!e) e = load("e_vector_aa.dt");
  auto d = load("dru_vector_aa.dt");
  if (!d) d = load("d_vector_aa.dt");
  v.font[0] = std::move(a);
  v.font[1] = std::move(b);
  v.font[2] = std::move(e);
  v.font[3] = std::move(d);
  v.font[4] = load("c_vector_aa.dt");
  v.font[5] = load("sym_vector_aa.dt");
  v.font[6] = load("dg1_vector_aa.dt");
  v.font[7] = load("dg2_vector_aa.dt");
  v.font[8] = load("dg3_vector_aa.dt");
  v.font[9] = load("dg4_vector_aa.dt");
  v.font[10] = load("dg5_vector_aa.dt");
  v.reset();
  return v.font[1] != nullptr;
}
bool vector_fonts_loaded() { return vf().font[1] != nullptr; }
bool font_role_loaded(ui::Font role) {
  const int slot = font_slot(role);
  return slot >= 0 && slot < kNumVFonts && vf().font[slot] != nullptr;
}
void set_cjk_font(std::shared_ptr<const fh1::CjkFont> cjk) {
  VectorFonts& v = vf();
  v.cjk = std::move(cjk);
  v.reset();
}
bool cjk_font_loaded() { return vf().cjk != nullptr && vf().cjk->valid(); }
void set_atlas_size(uint32_t side) {
  VectorFonts& v = vf();
  const uint32_t clamped = side <= 2048 ? 2048u : kAtlasSize;
  if (v.atlas_size == clamped) return;
  v.atlas_size = clamped;
  v.glyph_rows = clamped == 2048u ? 640u : kGlyphRows;
  v.transient_y0 = clamped == 2048u ? 1280u : kTransientY0;
  atlas_reset();
}
void atlas_reset() {
  VectorFonts& v = vf();
  v.image_x = 0;
  v.image_y = v.glyph_rows;
  v.image_h = 0;
  v.trans_y_next = v.transient_y0;
  v.trans_shelves.clear();
  v.trans_key_shelf.clear();
  v.images.clear();
  v.cache.clear();
  v.glyph_shelves.clear();
  v.glyph_y_next = 0;
  v.dirty.clear();
  v.dirty_patches.clear();
  std::vector<uint8_t>().swap(v.atlas);
}
uint32_t atlas_size() { return vf().atlas_size; }
void set_gpu_atlas_mode(bool on) {
  VectorFonts& v = vf();
  v.gpu_mode = on;
  if (on && !v.atlas.empty()) {
    std::vector<uint8_t>().swap(v.atlas);
  }
}
bool gpu_atlas_mode() { return vf().gpu_mode; }
size_t atlas_cpu_bytes() { return vf().atlas.size(); }
const std::vector<uint8_t>& atlas_pixels() {
  VectorFonts& v = vf();
  v.ensure_cpu_atlas();
  return v.atlas;
}
std::vector<std::array<uint32_t, 4>> atlas_take_dirty() {
  VectorFonts& v = vf();
  ++v.frame_tick;
  return std::exchange(v.dirty, {});
}
std::vector<AtlasPatch> atlas_take_dirty_patches() {
  VectorFonts& v = vf();
  ++v.frame_tick;
  return std::exchange(v.dirty_patches, {});
}

std::string List::strip_style_tags(std::string_view text) {
  TextStyle base{};
  const auto lines = parse_styled_lines(text, base);
  std::string out;
  for (size_t li = 0; li < lines.size(); ++li) {
    if (li > 0) out.push_back('\n');
    for (const auto& run : lines[li].runs) out.append(run.text);
  }
  return out;
}

float List::measure(std::string_view text, const TextStyle& s) {
  const auto lines = parse_styled_lines(text, s);
  float max_w = 0.0f;
  for (const auto& ln : lines) {
    float lw = 0.0f;
    for (const auto& run : ln.runs) {
      TextStyle rs = s;
      rs.font = run.font;
      rs.color = run.color;
      lw += measure_run_single_line(run.text, rs);
    }
    max_w = std::max(max_w, lw);
  }
  return max_w;
}

float List::measure_height(std::string_view text, const TextStyle& s) {
  const auto lines = parse_styled_lines(text, s);
  if (lines.empty()) return 0.0f;
  return s.size + float(lines.size() - 1) * (s.size * 1.35f);
}

float List::text(float x, float y, std::string_view text, const TextStyle& s) {
  const auto lines = parse_styled_lines(text, s);
  const float line_step = s.size * 1.35f;
  const bool have_cjk = vf().cjk && vf().cjk->valid();
  const int cjk_px = have_cjk ? cjk_cached_px(s.size) : 0;
  const float inv = 1.0f / float(vf().atlas_size);
  float max_w = 0.0f;

  for (size_t li = 0; li < lines.size(); ++li) {
    const auto& ln = lines[li];
    float lw = 0.0f;
    for (const auto& run : ln.runs) {
      TextStyle rs = s;
      rs.font = run.font;
      rs.color = run.color;
      lw += measure_run_single_line(run.text, rs);
    }
    max_w = std::max(max_w, lw);

    float pen = x;
    if (s.align == Align::kCenter) pen -= 0.5f * lw;
    if (s.align == Align::kRight) pen -= lw;
    const float ly = y + float(li) * line_step;
    const float base = ly + s.size;

    for (const auto& run : ln.runs) {
      TextStyle rs = s;
      rs.font = run.font;
      rs.color = run.color;
      std::string folded_storage;
      std::string_view run_src = run.text;
      if (!rs.keep_case) {
        folded_storage = ui::unicode_uppercase_utf8(run.text);
        run_src = folded_storage;
      }
      const int f = font_of(rs);
      if (f >= 0) {
        const fh1::VFont& font = *vf().font[f];
        const float k = em_px(font, rs.size);
        const int px = cached_px(k);
        for (size_t i = 0; i < run_src.size();) {
          const uint32_t cp = next_code_point(run_src, i, true);
          int actual_slot = f;
          const fh1::VFontGlyph* g = find_vfont_glyph(f, cp, actual_slot);
          if (g) {
            const fh1::VFont& gf = *vf().font[actual_slot];
            const float gk = (actual_slot == f) ? k : em_px(gf, rs.size);
            const int gpx = (actual_slot == f) ? px : cached_px(gk);
            if (const auto* e = glyph_entry(actual_slot, cp, gpx)) {
              const float scale = gk / float(std::max(1, e->px));
              const float sx = scale * rs.stretch;
              float gx = pen + e->left * sx, gy = base + e->top * scale;
              if (scale == 1.0f && rs.stretch == 1.0f) gx = std::round(gx), gy = std::round(gy);
              image(gx, gy, gx + (e->u1 - e->u0) * sx, gy + (e->v1 - e->v0) * scale,
                    {e->u0 * inv, e->v0 * inv, e->u1 * inv, e->v1 * inv}, rs.color);
            } else if (cp < 128) {
              // Last-resort safety fallback: never drop an ASCII character into blank space.
              if (const Glyph* sg = glyph(char(cp)); sg && sg->count) {
                const float sk = rs.size / 10.0f;
                const float sr = rs.weight * 10.0f;
                const float gp = pen + sr * sk;
                const float pad = (sr + 1.5f) * sk;
                const float lean = rs.slant * 11.0f * sk;
                items_.push_back({{gp - pad - std::max(0.0f, -lean), ly - pad,
                                   gp + sg->width * sk + pad + std::max(0.0f, lean), ly + 10 * sk + pad + 1.5f * sk},
                                  {gp, ly, sk, sr},
                                  {float(sg->first), float(sg->count), rs.slant, 0},
                                  {0, 0, 0, 0},
                                  {0, 0, 0, 0},
                                  rs.color,
                                  rs.color,
                                  kGlyph,
                                  0});
              }
            }
            pen += (g->advance * gk + rs.tracking * rs.size) * rs.stretch;
          } else if (cp != ' ' && have_cjk && vf().cjk->has_glyph(cp)) {
            if (const auto* e = cjk_glyph_entry(cp, cjk_px)) {
              const float cs = std::clamp(rs.size * 1.32f, 8.0f, 96.0f) / float(std::max(1, e->px));
              const float sx = cs * rs.stretch;
              float gx = pen + e->left * sx, gy = base + e->top * cs;
              image(gx, gy, gx + (e->u1 - e->u0) * sx, gy + (e->v1 - e->v0) * cs,
                    {e->u0 * inv, e->v0 * inv, e->u1 * inv, e->v1 * inv}, rs.color);
              pen += (e->advance * cs + rs.tracking * rs.size) * rs.stretch;
            } else {
              pen += (cjk_advance_px(cp, rs.size) + rs.tracking * rs.size) * rs.stretch;
            }
          } else {
            pen += (font.space_width * k + rs.tracking * rs.size) * rs.stretch;
          }
        }
      } else {
        const float k = rs.size / 10;
        const float r = rs.weight * 10;
        for (size_t i = 0; i < run_src.size();) {
          const uint32_t cp = next_code_point(run_src, i, true);
          if (cp >= 0x80 && have_cjk && vf().cjk->has_glyph(cp)) {
            if (const auto* e = cjk_glyph_entry(cp, cjk_px)) {
              const float cs = std::clamp(rs.size * 1.32f, 8.0f, 96.0f) / float(std::max(1, e->px));
              float gx = pen + e->left * cs, gy = base + e->top * cs;
              image(gx, gy, gx + (e->u1 - e->u0) * cs, gy + (e->v1 - e->v0) * cs,
                    {e->u0 * inv, e->v0 * inv, e->u1 * inv, e->v1 * inv}, rs.color);
              pen += e->advance * cs + rs.tracking * rs.size;
            } else {
              pen += cjk_advance_px(cp, rs.size) + rs.tracking * rs.size;
            }
            continue;
          }
          const Glyph* g = cp < 128 ? glyph(char(cp)) : nullptr;
          if (g && g->count) {
            const float gp = pen + r * k;
            const float pad = (r + 1.5f) * k;
            const float lean = rs.slant * 11 * k;
            items_.push_back({{gp - pad - std::max(0.0f, -lean), ly - pad, gp + g->width * k + pad + std::max(0.0f, lean),
                               ly + 10 * k + pad + 1.5f * k},
                              {gp, ly, k, r},
                              {float(g->first), float(g->count), rs.slant, 0},
                              {0, 0, 0, 0},
                              {0, 0, 0, 0},
                              rs.color,
                              rs.color,
                              kGlyph,
                              0});
          }
          pen += advance_units(g, r, rs.tracking * 10) * k;
        }
      }
    }
  }
  return max_w;
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

// An image quad's (u, v) over its rect (0..1) at a pixel, and its edge
// coverage (1 inside; rotated quads fade over their edge pixel).
void image_t(const Instance& it, float px, float py, float& tx, float& ty, float& edge) {
  if (it.p[1] > 0) {
    const float dx = px - 0.5f * (it.rect[0] + it.rect[2]), dy = py - 0.5f * (it.rect[1] + it.rect[3]);
    const float c = std::cos(it.p[3]), s = std::sin(it.p[3]);
    const float lx = c * dx + s * dy, ly = -s * dx + c * dy;
    tx = std::clamp(lx / (2 * it.p[1]) + 0.5f, 0.0f, 1.0f), ty = std::clamp(ly / (2 * it.p[2]) + 0.5f, 0.0f, 1.0f);
    edge = std::clamp(0.5f - std::max(std::fabs(lx) - it.p[1], std::fabs(ly) - it.p[2]), 0.0f, 1.0f);
    return;
  }
  tx = (px - it.rect[0]) / std::max(1.0f, it.rect[2] - it.rect[0]);
  ty = (py - it.rect[1]) / std::max(1.0f, it.rect[3] - it.rect[1]);
  edge = 1;
}

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
      } else if (it.q[2] > 0) {
        const float past = std::min(rel - (it.q[1] - it.q[0]), 2 * kPi - rel) * std::hypot(dx, dy);
        d = std::max(std::fabs(std::hypot(dx, dy) - it.p[2]) - it.p[3], past);
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
      float tx, ty, edge = 1;
      image_t(it, px, py, tx, ty, edge);
      const uint32_t asz = atlas_size();
      const float u = (it.q[0] + (it.q[2] - it.q[0]) * tx) * asz - 0.5f;
      const float v = (it.q[1] + (it.q[3] - it.q[1]) * ty) * asz - 0.5f;
      const auto& a = atlas_pixels();
      auto at = [&](int x, int y) {
        x = std::clamp(x, 0, int(asz) - 1), y = std::clamp(y, 0, int(asz) - 1);
        return float(a[(size_t(y) * asz + size_t(x)) * 4 + 3]) / 255.0f;
      };
      const int x0 = int(std::floor(u)), y0 = int(std::floor(v));
      const float fx = u - float(x0), fy = v - float(y0);
      return edge * ((at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
                     (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy);
    }
    default:
      return 0;
  }
}

float to_linear_unit(uint32_t c, int shift) { return float((c >> shift) & 0xFF) / 255.0f; }

void sample_atlas_rect(const float uv[4], float tx, float ty, float out_rgba[4]) {
  const uint32_t asz = atlas_size();
  const float u_lo = std::min(uv[0], uv[2]) * asz;
  const float u_hi = std::max(uv[0], uv[2]) * asz;
  const float v_lo = std::min(uv[1], uv[3]) * asz;
  const float v_hi = std::max(uv[1], uv[3]) * asz;
  const int min_x = std::clamp(int(std::floor(u_lo)), 0, int(asz) - 1);
  const int max_x = std::clamp(int(std::ceil(u_hi)) - 1, min_x, int(asz) - 1);
  const int min_y = std::clamp(int(std::floor(v_lo)), 0, int(asz) - 1);
  const int max_y = std::clamp(int(std::ceil(v_hi)) - 1, min_y, int(asz) - 1);
  const float u = (uv[0] + (uv[2] - uv[0]) * tx) * asz - 0.5f;
  const float v = (uv[1] + (uv[3] - uv[1]) * ty) * asz - 0.5f;
  const int x0 = int(std::floor(u)), y0 = int(std::floor(v));
  const float fx = u - float(x0), fy = v - float(y0);
  const auto& a = atlas_pixels();
  auto sample = [&](int x, int y) {
    x = std::clamp(x, min_x, max_x);
    y = std::clamp(y, min_y, max_y);
    return &a[(size_t(y) * asz + size_t(x)) * 4];
  };
  const uint8_t* p00 = sample(x0, y0);
  const uint8_t* p10 = sample(x0 + 1, y0);
  const uint8_t* p01 = sample(x0, y0 + 1);
  const uint8_t* p11 = sample(x0 + 1, y0 + 1);
  const float w00 = (1.0f - fx) * (1.0f - fy);
  const float w10 = fx * (1.0f - fy);
  const float w01 = (1.0f - fx) * fy;
  const float w11 = fx * fy;
  for (int ch = 0; ch < 4; ++ch) {
    out_rgba[ch] = (p00[ch] * w00 + p10[ch] * w10 + p01[ch] * w01 + p11[ch] * w11) / 255.0f;
  }
}

void image_masked_rgba(const Instance& it, float px, float py, float c[4], float& edge) {
  float tx = 0.0f, ty = 0.0f;
  image_t(it, px, py, tx, ty, edge);
  if (it.p[1] <= 0.0f && it.p[0] > 0.0f) {
    const float cx = 0.5f * (it.rect[0] + it.rect[2]), cy = 0.5f * (it.rect[1] + it.rect[3]);
    const float hx = 0.5f * (it.rect[2] - it.rect[0]), hy = 0.5f * (it.rect[3] - it.rect[1]);
    const float rr = std::min({it.p[0], hx, hy});
    const float qx = std::fabs(px - cx) - hx + rr, qy = std::fabs(py - cy) - hy + rr;
    const float d = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - rr;
    edge = std::clamp(0.5f - d, 0.0f, 1.0f);
  }
  if (edge <= 0.0f) {
    c[3] = 0.0f;
    return;
  }
  float t0x = tx * it.m[0] + it.m[2];
  float t0y = ty * it.m[1] + it.m[3];
  float s0[4] = {0, 0, 0, 0};
  if ((it.flags & kMaskRepeat) != 0u) {
    t0x = t0x - std::floor(t0x);
    t0y = t0y - std::floor(t0y);
    sample_atlas_rect(it.q, t0x, t0y, s0);
  } else if ((it.flags & kMaskBorderTransparent) != 0u) {
    if (t0x >= 0.0f && t0x <= 1.0f && t0y >= 0.0f && t0y <= 1.0f) {
      sample_atlas_rect(it.q, t0x, t0y, s0);
    }
  } else {
    sample_atlas_rect(it.q, std::clamp(t0x, 0.0f, 1.0f), std::clamp(t0y, 0.0f, 1.0f), s0);
  }
  float s1[4] = {0, 0, 0, 0};
  sample_atlas_rect(it.r, std::clamp(tx, 0.0f, 1.0f), std::clamp(ty, 0.0f, 1.0f), s1);
  for (int ch = 0; ch < 4; ++ch) c[ch] *= s0[ch] * s1[ch];
}

// An image quad's texel colour (bilinear, clamped to the image's atlas rect), times the tint in `c`.
void image_rgb(const Instance& it, float px, float py, float c[4]) {
  float tx, ty, edge;
  image_t(it, px, py, tx, ty, edge);
  float s[4] = {0, 0, 0, 0};
  sample_atlas_rect(it.q, std::clamp(tx, 0.0f, 1.0f), std::clamp(ty, 0.0f, 1.0f), s);
  for (int ch = 0; ch < 3; ++ch) c[ch] *= s[ch];
}

}  // namespace

void rasterize(std::span<const Instance> items, std::vector<uint8_t>& out, uint32_t width, uint32_t height) {
  out.resize(size_t(width) * height * 4);
  for (const Instance& it : items) {
    const int x0 = std::max(0, int(std::floor(it.rect[0]))), x1 = std::min(int(width), int(std::ceil(it.rect[2])));
    const int y0 = std::max(0, int(std::floor(it.rect[1]))), y1 = std::min(int(height), int(std::ceil(it.rect[3])));
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        const float px = float(x) + 0.5f, py = float(y) + 0.5f;
        float c[4];
        float a = 0.0f;
        if (it.kind == kImageMasked) {
          for (int ch = 0; ch < 4; ++ch) {
            const int shift = ch == 3 ? 24 : 16 - 8 * ch;
            c[ch] = to_linear_unit(it.color, shift);
          }
          float edge = 1.0f;
          image_masked_rgba(it, px, py, c, edge);
          a = c[3] * edge;
          if (a <= 0.0f) continue;
        } else {
          float mix;
          const float cov = coverage(it, px, py, mix);
          if (cov <= 0) continue;
          for (int ch = 0; ch < 4; ++ch) {
            const int shift = ch == 3 ? 24 : 16 - 8 * ch;
            c[ch] = to_linear_unit(it.color, shift) * (1 - mix) + to_linear_unit(it.color2, shift) * mix;
          }
          if (it.kind == kImage) image_rgb(it, px, py, c);
          a = c[3] * cov;
        }
        uint8_t* p = &out[(size_t(y) * width + size_t(x)) * 4];
        for (int ch = 0; ch < 3; ++ch) p[ch] = uint8_t(std::lround(c[ch] * 255 * a + p[ch] * (1 - a)));
        p[3] = uint8_t(std::lround(255 * a + p[3] * (1 - a)));
      }
  }
}

namespace {

std::function<std::string(const std::string&)>& ui_text_lookup_fn() {
  static std::function<std::string(const std::string&)> fn;
  return fn;
}

}  // namespace

void set_text_lookup(std::function<std::string(const std::string&)> fn) {
  ui_text_lookup_fn() = std::move(fn);
}

std::string lookup_text(const std::string& ref) {
  if (const auto& fn = ui_text_lookup_fn(); fn && !ref.empty()) {
    return fn(ref);
  }
  return {};
}

}  // namespace carview::ui
