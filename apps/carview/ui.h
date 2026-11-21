// carview's UI layer (ours: the game's HUD and menus are not specified).
// Screens build a flat list of shape instances each frame (rounded panels,
// lines, gauge arcs, glyphs, images); the renderer draws the whole list in
// one instanced pass after the post chain, each shape's edge anti-aliased
// from its signed distance in the fragment shader (shaders/ui.frag). The
// same list rasterizes on the CPU (rasterize) for headless PNGs and tests.
//
// Text uses the game's vector fonts when they are loaded (set_vector_fonts,
// FORMAT-FONTS): each glyph is rasterized once per size on the CPU by the
// spec's coverage rule into the UI atlas (atlas_pixels, uploaded by the
// renderer from atlas_take_dirty) and drawn as an image quad. Without them
// it is the engine's own stroke font: capitals, digits and some
// punctuation as polylines on a 10-unit cap height, drawn as round-capped
// strokes; a glyph is one instance whose shader takes the nearest of its
// segments (from a storage buffer), so strokes join without overlap.
//
// Coordinates are pixels, x right and y down from the top-left. Colours are
// sRGB-encoded RGBA8 packed as 0xAARRGGBB, blended in sRGB.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fh1 {
struct VFont;
}

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
  kArc = 2,        // p: cx, cy, radius, half width; q: a0, a1 (radians clockwise from 12 o'clock, a0 <= a1), square ends (> 0)
  kGlyph = 3,      // p: origin x, y (top of the cap height), pixels per unit, stroke radius (units); q: first segment, count, slant
  kImage = 4,      // p: radius (or rotated: -, half width, half height, radians clockwise about the rect's centre); q: atlas u0, v0, u1, v1 (0..1), tinted by color
};

// The font's segments (ax, ay, bx, by in units), uploaded once.
std::span<const std::array<float, 4>> font_segments();

constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
  return uint32_t(a) << 24 | uint32_t(r) << 16 | uint32_t(g) << 8 | b;
}
// `c` with its alpha scaled by `k` (0..1).
uint32_t fade(uint32_t c, double k);

enum class Align { kLeft, kCenter, kRight };

// FORMAT-FONTS' targets (interim roles: titles and menu items E, labels and
// numbers B, paragraphs A). kAuto: E for slanted text, else B.
enum class Font { kAuto, kStroke, kA, kB, kD, kE };

// The game's fonts (any may be null: that role falls back to the stroke
// font). Clears the glyph cache.
void set_vector_fonts(std::shared_ptr<const fh1::VFont> a, std::shared_ptr<const fh1::VFont> b,
                      std::shared_ptr<const fh1::VFont> e, std::shared_ptr<const fh1::VFont> d = nullptr);
bool vector_fonts_loaded();

// The UI atlas (kAtlasSize square, RGBA8, white with the glyphs' coverage
// in alpha), owned here; the renderer uploads the rectangles that changed.
// The glyphs use the rows above kGlyphRows; the images (the game's UI
// textures) the rows below, kept for the run.
inline constexpr uint32_t kAtlasSize = 2048, kGlyphRows = 1024;
const std::vector<uint8_t>& atlas_pixels();
std::vector<std::array<uint32_t, 4>> atlas_take_dirty();  // x, y, w, h
// An image (straight-alpha RGBA, sRGB) in the atlas under `key`, once; its
// uv rectangle (0..1). False when it does not fit.
bool atlas_image(const std::string& key, uint32_t w, uint32_t h, std::span<const uint8_t> rgba,
                 std::array<float, 4>& uv);
// An image put before; false when there is none.
bool atlas_find(const std::string& key, std::array<float, 4>& uv);
// Like atlas_image, but a key already there at the same size has its
// pixels replaced in place (a panel redrawn on the CPU, as the minimap).
bool atlas_update(const std::string& key, uint32_t w, uint32_t h, std::span<const uint8_t> rgba,
                  std::array<float, 4>& uv);

struct TextStyle {
  float size = 20;        // cap height, px
  float weight = 0.11f;   // stroke radius over the cap height
  float slant = 0;        // x shift per unit of height (0.18: italic)
  float tracking = 0;     // extra space between letters, over the cap height
  uint32_t color = rgba(255, 255, 255);
  Align align = Align::kLeft;
  Font font = Font::kAuto;
  bool keep_case = false;  // vector fonts: draw lower case as written (else upper-cased)
  float stretch = 1;       // vector fonts: the glyphs' width scale (a scene text's x over y scale)
};

class List {
 public:
  void clear() { items_.clear(), turn_a_ = 0; }
  // Turns what follows `angle` radians clockwise about (cx, cy) until
  // unturn(): images and the game fonts' glyphs turn; flat rects (radius 0)
  // turn as quads; other shapes only move with the turn.
  void turn(float cx, float cy, float angle) { turn_x_ = cx, turn_y_ = cy, turn_a_ = angle; }
  void unturn() { turn_a_ = 0; }
  bool turned() const { return turn_a_ != 0; }
  std::span<const Instance> items() const { return items_; }
  bool empty() const { return items_.empty(); }

  void rect(float x0, float y0, float x1, float y1, uint32_t color, float radius = 0, uint32_t color2 = 0);
  void frame(float x0, float y0, float x1, float y1, uint32_t color, float width, float radius = 0);
  // A soft shadow under a panel: `blur` px of falloff.
  void shadow(float x0, float y0, float x1, float y1, uint32_t color, float radius, float blur);
  void line(float ax, float ay, float bx, float by, float width, uint32_t color);
  void circle(float cx, float cy, float r, uint32_t color);
  // `square`: flat radial ends (else round caps).
  void arc(float cx, float cy, float radius, float width, float a0, float a1, uint32_t color, bool square = false);
  void image(float x0, float y0, float x1, float y1, std::array<float, 4> uv, uint32_t tint = rgba(255, 255, 255),
             float radius = 0);
  // An image quad centred at (cx, cy), half size hw x hh, turned `angle`
  // radians clockwise (y down).
  void image_rotated(float cx, float cy, float hw, float hh, float angle, std::array<float, 4> uv,
                     uint32_t tint = rgba(255, 255, 255));
  // Draws `text` with its cap-height top at y; returns its width.
  float text(float x, float y, std::string_view text, const TextStyle& style);
  static float measure(std::string_view text, const TextStyle& style);

 private:
  std::vector<Instance> items_;
  float turn_x_ = 0, turn_y_ = 0, turn_a_ = 0;
  // A point under the turn.
  void turned(float& x, float& y) const;
};

// The CPU rasterizer: `out` (sRGB RGBA8, width x height) gets the list
// blended over it.
void rasterize(std::span<const Instance> items, std::vector<uint8_t>& out, uint32_t width, uint32_t height);

}  // namespace carview::ui
