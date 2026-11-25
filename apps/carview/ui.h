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
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fh1 {
class Archive;
struct UiFontMap;
struct VFont;
class CjkFont;
}

namespace carview::ui {

// One shape, 96 bytes, the vertex shader's per-instance input.
struct Instance {
  float rect[4];  // bounds: x0, y0, x1, y1 (the quad drawn)
  float p[4];     // per kind, see Kind
  float q[4];     // per kind (kImage / kImageMasked tex0 atlas u0, v0, u1, v1)
  float r[4];     // kImageMasked: tex1 (mask) atlas u0, v0, u1, v1
  float m[4];     // kImageMasked: tex0 local UV transform (u_scale, v_scale, u_off, v_off)
  uint32_t color, color2, kind, flags;
};
static_assert(sizeof(Instance) == 96);

inline constexpr uint32_t kMaskBorderTransparent = 1u << 0;  // addressing mode 4: transparent outside [0,1]
inline constexpr uint32_t kMaskRepeat = 1u << 1;             // tiled pattern: fract(uv)
inline constexpr uint32_t kUiFlagFullscreen = 1u << 2;       // expand to full physical viewport (ignores HUD 16:9 lock/safe-zone)
inline constexpr uint32_t kUiFlagFullscreenCover = 1u << 3;  // full physical viewport cover-fill (m = full uv, r[0] = aspect)
inline constexpr uint32_t kUiFlagFullscreenX = 1u << 4;      // expand X to full physical viewport width while preserving Y

enum Kind : uint32_t {
  kRoundRect = 0,    // p: radius, stroke (0 fills), inset, -; q: softness; colour top to color2 bottom
  kLine = 1,         // p: ax, ay, bx, by; q: half width
  kArc = 2,          // p: cx, cy, radius, half width; q: a0, a1 (radians clockwise from 12 o'clock), square ends (> 0)
  kGlyph = 3,        // p: origin x, y, pixels per unit, stroke radius; q: first segment, count, slant
  kImage = 4,        // p: radius (or rotated: -, hw, hh, angle); q: atlas u0, v0, u1, v1, tinted by color
  kImageMasked = 5,  // p: same as kImage; q: tex0 atlas uv; r: tex1 atlas uv; m: tex0 (su, sv, ou, ov); flags: kMask*
};

// The font's segments (ax, ay, bx, by in units), uploaded once.
std::span<const std::array<float, 4>> font_segments();

constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
  return uint32_t(a) << 24 | uint32_t(r) << 16 | uint32_t(g) << 8 | b;
}
// `c` with its alpha scaled by `k` (0..1).
uint32_t fade(uint32_t c, double k);

enum class Align { kLeft, kCenter, kRight };

// FORMAT-FONTS' targets: A (sans), B (condensed slab), C, D (heavy B),
// E (heavy italic), SYM (symbols), DG1..DG5 (digital gauge fonts).
// kAuto: E for slanted text, else B.
enum class Font { kAuto, kStroke, kA, kB, kC, kD, kE, kSym, kDg1, kDg2, kDg3, kDg4, kDg5 };

// The game's fonts (any may be null: that role falls back to the stroke
// font). Clears the glyph cache.
void set_vector_fonts(std::shared_ptr<const fh1::VFont> a, std::shared_ptr<const fh1::VFont> b,
                      std::shared_ptr<const fh1::VFont> e, std::shared_ptr<const fh1::VFont> d = nullptr);
// Sets an individual vector font role (C, SYM, DG1..DG5, or A/B/D/E).
void set_named_vector_font(Font role, std::shared_ptr<const fh1::VFont> font);
// Installs the parsed fontmap.xml mapping used by resolve_font_name().
void set_font_map(const fh1::UiFontMap& map);
// Resolves an FBF font name (e.g. "horizon_d", "d bold", "horizon_dg2",
// "b_swiss_721_medium medium") through fontmap.xml + fallback rules.
Font resolve_font_name(std::string_view font_name);
// Loads fontmap.xml and all available vector fonts (aru/bru/dru/eru, c, sym,
// dg1..dg5) from media/ui/Fonts.zip. Returns true if core fonts loaded.
bool load_fonts_from_zip(fh1::Archive& fonts_zip);
bool vector_fonts_loaded();
bool font_role_loaded(Font role);
// CJK bitmap font (CHTB/CHSB/JPB/KORB) used automatically for glyphs outside
// the Latin/Cyrillic vector fonts (or standalone).
void set_cjk_font(std::shared_ptr<const fh1::CjkFont> cjk);
bool cjk_font_loaded();

// Unicode UTF-8 uppercase folding covering ASCII, Latin-1 Supplement (à..þ, ß -> SS),
// Latin Extended-A (Polish/Czech/Hungarian/etc.), and Cyrillic (а..я, ё -> Ё).
uint32_t unicode_to_upper_cp(uint32_t cp);
std::string unicode_uppercase_utf8(std::string_view text);

// The UI atlas (kAtlasSize square, RGBA8, white with the glyphs' coverage
// in alpha), owned here; the renderer uploads the rectangles that changed.
// Rows 0..kGlyphRows: glyph cache.
// Rows kGlyphRows..kTransientY0: persistent HUD/UI4 chrome textures.
// Rows kTransientY0..kAtlasSize: LRU-evicted transient artwork cache
// (EventTickets, Thumbnails, BGLoadingImages, Upgrade icons, Logos).
inline constexpr uint32_t kAtlasSize = 4096, kGlyphRows = 1024, kTransientY0 = 2560;
struct AtlasPatch {
  uint32_t x = 0, y = 0, w = 0, h = 0;
  std::vector<uint8_t> rgba;
};
// Configures runtime atlas side (2048 or 4096) and GPU-only patch mode (avoids
// keeping a permanent 64 MiB CPU shadow copy when rendering with Vulkan).
void set_atlas_size(uint32_t side);
// Forgets every glyph and image in the atlas (they are put again on next
// use). For a fresh renderer whose atlas starts empty: in GPU atlas mode
// there is no CPU copy to upload in full, so cached entries would sample
// blank texels.
void atlas_reset();
uint32_t atlas_size();
void set_gpu_atlas_mode(bool on);
bool gpu_atlas_mode();
size_t atlas_cpu_bytes();
const std::vector<uint8_t>& atlas_pixels();
std::vector<std::array<uint32_t, 4>> atlas_take_dirty();  // x, y, w, h
std::vector<AtlasPatch> atlas_take_dirty_patches();
// Advances the atlas frame counter (also auto-advanced on atlas_take_dirty*).
// Glyphs and transient shelves used within the last 4 frames are protected
// against eviction so in-flight GPU frames never sample overwritten texels.
void atlas_begin_frame();
// An image (straight-alpha RGBA, sRGB) in the atlas under `key`, once; its
// uv rectangle (0..1). Transient artwork keys automatically route to the LRU
// transient cache (never failing on overflow).
bool atlas_image(const std::string& key, uint32_t w, uint32_t h, std::span<const uint8_t> rgba,
                 std::array<float, 4>& uv, bool transient = false);
// An image put before; false when there is none (bumps LRU tick if transient).
bool atlas_find(const std::string& key, std::array<float, 4>& uv);
// Like atlas_image, but a key already there at the same size has its
// pixels replaced in place (a panel redrawn on the CPU, as the minimap).
bool atlas_update(const std::string& key, uint32_t w, uint32_t h, std::span<const uint8_t> rgba,
                  std::array<float, 4>& uv);
uint64_t atlas_transient_evictions();
uint64_t atlas_glyph_evictions();
void atlas_clear_transient();

struct AtlasStats {
  uint32_t atlas_size = kAtlasSize;
  uint32_t glyph_rows_used = 0;
  uint32_t persistent_rows_used = 0;
  uint32_t transient_rows_used = 0;
  double utilization_pct = 0.0;
  uint64_t transient_evictions = 0;
  uint64_t glyph_evictions = 0;
  uint32_t glyph_count = 0;
  uint32_t image_count = 0;
};
AtlasStats atlas_stats();

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

struct HitZone {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // screen-space AABB (accounts for active turn())
  uint32_t group = 0;
  int index = -1;
};
inline constexpr uint32_t kHitRow = 1;
inline constexpr uint32_t kHitTab = 2;
inline constexpr uint32_t kHitStepLeft = 3;
inline constexpr uint32_t kHitStepRight = 4;
inline constexpr uint32_t kHitConfirm = 5;
inline constexpr uint32_t kHitCancel = 6;

bool hit_test_zones(std::span<const HitZone> zones, float mx, float my, HitZone& out);

class List {
 public:
  void clear() { items_.clear(), hits_.clear(), turn_a_ = 0; }
  // Turns what follows `angle` radians clockwise about (cx, cy) until
  // unturn(): images and the game fonts' glyphs turn; flat rects (radius 0)
  // turn as quads; other shapes only move with the turn.
  void turn(float cx, float cy, float angle) { turn_x_ = cx, turn_y_ = cy, turn_a_ = angle; }
  void unturn() { turn_a_ = 0; }
  bool turned() const { return turn_a_ != 0; }
  std::span<const Instance> items() const { return items_; }
  bool empty() const { return items_.empty(); }

  void add_hit(float x0, float y0, float x1, float y1, uint32_t group, int index);
  std::span<const HitZone> hits() const { return hits_; }
  bool hit_test(float mx, float my, HitZone& out) const { return hit_test_zones(hits_, mx, my, out); }

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
  // Full-viewport backdrop / cover-fill video quad: fills the physical screen
  // edge-to-edge even when HUD 16:9 aspect lock or safe-zone margins are active.
  void fullscreen_backdrop(float x0, float y0, float x1, float y1, uint32_t color);
  void fullscreen_band(float x0, float y0, float x1, float y1, uint32_t color, uint32_t color2 = 0);
  void fullscreen_cover_image(float x0, float y0, float x1, float y1, std::array<float, 4> uv,
                              std::array<float, 4> full_uv, float content_aspect,
                              uint32_t tint = rgba(255, 255, 255));
  // An image quad centred at (cx, cy), half size hw x hh, turned `angle`
  // radians clockwise (y down).
  void image_rotated(float cx, float cy, float hw, float hh, float angle, std::array<float, 4> uv,
                     uint32_t tint = rgba(255, 255, 255));
  // Two-texture masked / crop multiply image: color = tint * tex0(t*su+ou, t*sv+ov) * tex1(t).
  void image_masked(float x0, float y0, float x1, float y1, std::array<float, 4> uv0, std::array<float, 4> uv1,
                    uint32_t tint = rgba(255, 255, 255), std::array<float, 4> uv0_xform = {1, 1, 0, 0},
                    uint32_t flags = kMaskBorderTransparent, float radius = 0);
  void image_masked_rotated(float cx, float cy, float hw, float hh, float angle, std::array<float, 4> uv0,
                            std::array<float, 4> uv1, uint32_t tint = rgba(255, 255, 255),
                            std::array<float, 4> uv0_xform = {1, 1, 0, 0}, uint32_t flags = kMaskBorderTransparent);
  // Draws `text` with its cap-height top at y; returns its width. Supports
  // multi-line `\n` breaks and `{font='A'}` / `{font_pop}` / `{colour='R G B A'}`
  // (single or doubled brace) style tags.
  float text(float x, float y, std::string_view text, const TextStyle& style);
  static float measure(std::string_view text, const TextStyle& style);
  static float measure_height(std::string_view text, const TextStyle& style);
  // Strips `{font=...}` / `{colour=...}` / `{font_pop}` style tags to plain UTF-8.
  static std::string strip_style_tags(std::string_view text);

 private:
  std::vector<Instance> items_;
  std::vector<HitZone> hits_;
  float turn_x_ = 0, turn_y_ = 0, turn_a_ = 0;
  // A point under the turn.
  void turned(float& x, float& y) const;
};

// The CPU rasterizer: `out` (sRGB RGBA8, width x height) gets the list
// blended over it.
void rasterize(std::span<const Instance> items, std::vector<uint8_t>& out, uint32_t width, uint32_t height);

// Optional shared StringTables text lookup hook usable from all UI targets
// (including lightweight unit-test binaries that do not link carview_hud).
void set_text_lookup(std::function<std::string(const std::string&)> fn);
std::string lookup_text(const std::string& ref);

}  // namespace carview::ui
