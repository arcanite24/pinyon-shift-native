// carview's UI layer (ours: the game's HUD and menus are not specified).
// Screens build a flat list of shape instances each frame (rounded panels,
// lines, gauge arcs, glyphs, images); the renderer draws the whole list in
// one instanced pass after the post chain, each shape's edge anti-aliased
// from its signed distance in the fragment shader (shaders/ui.frag). The
// same list rasterizes on the CPU (rasterize) for headless PNGs and tests.
//
// Text is the engine's own stroke font: capitals, digits and some
// punctuation as polylines on a 10-unit cap height, drawn as round-capped
// strokes; a glyph is one instance whose shader takes the nearest of its
// segments (from a storage buffer), so strokes join without overlap.
//
// Coordinates are pixels, x right and y down from the top-left. Colours are
// sRGB-encoded RGBA8 packed as 0xAARRGGBB, blended in sRGB.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace carview::ui {

// One shape, 64 bytes, the vertex shader's per-instance input.
struct Instance {
  float rect[4];  // bounds: x0, y0, x1, y1 (the quad drawn)
  float p[4];     // per kind, see Kind
  float q[4];
  uint32_t color, color2, kind, flags;
};
static_assert(sizeof(Instance) == 64);

enum Kind : uint32_t {
  kRoundRect = 0,  // p: radius, stroke (0 fills), inset (the shape inside the bounds), -; q: softness (a blurred edge, px); colour top to color2 bottom
  kLine = 1,       // p: ax, ay, bx, by; q: half width
  kArc = 2,        // p: cx, cy, radius, half width; q: a0, a1 (radians clockwise from 12 o'clock, a0 <= a1)
  kGlyph = 3,      // p: origin x, y (top of the cap height), pixels per unit, stroke radius (units); q: first segment, count, slant
  kImage = 4,      // p: radius; q: atlas u0, v0, u1, v1 (0..1), tinted by color
};

// The font's segments (ax, ay, bx, by in units), uploaded once.
std::span<const std::array<float, 4>> font_segments();

constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
  return uint32_t(a) << 24 | uint32_t(r) << 16 | uint32_t(g) << 8 | b;
}
// `c` with its alpha scaled by `k` (0..1).
uint32_t fade(uint32_t c, double k);

enum class Align { kLeft, kCenter, kRight };

struct TextStyle {
  float size = 20;        // cap height, px
  float weight = 0.11f;   // stroke radius over the cap height
  float slant = 0;        // x shift per unit of height (0.18: italic)
  float tracking = 0;     // extra space between letters, over the cap height
  uint32_t color = rgba(255, 255, 255);
  Align align = Align::kLeft;
};

class List {
 public:
  void clear() { items_.clear(); }
  std::span<const Instance> items() const { return items_; }
  bool empty() const { return items_.empty(); }

  void rect(float x0, float y0, float x1, float y1, uint32_t color, float radius = 0, uint32_t color2 = 0);
  void frame(float x0, float y0, float x1, float y1, uint32_t color, float width, float radius = 0);
  // A soft shadow under a panel: `blur` px of falloff.
  void shadow(float x0, float y0, float x1, float y1, uint32_t color, float radius, float blur);
  void line(float ax, float ay, float bx, float by, float width, uint32_t color);
  void circle(float cx, float cy, float r, uint32_t color);
  void arc(float cx, float cy, float radius, float width, float a0, float a1, uint32_t color);
  void image(float x0, float y0, float x1, float y1, std::array<float, 4> uv, uint32_t tint = rgba(255, 255, 255),
             float radius = 0);
  // Draws `text` with its cap-height top at y; returns its width.
  float text(float x, float y, std::string_view text, const TextStyle& style);
  static float measure(std::string_view text, const TextStyle& style);

 private:
  std::vector<Instance> items_;
};

// The CPU rasterizer: `out` (sRGB RGBA8, width x height) gets the list
// blended over it.
void rasterize(std::span<const Instance> items, std::vector<uint8_t>& out, uint32_t width, uint32_t height);

}  // namespace carview::ui
