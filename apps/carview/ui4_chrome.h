// Shared authentic Forza Horizon 1 (Anark Gameface UI4) presentation toolkit.
// Provides the retail FH1 visual language across every screen: tilted -2.8 deg
// brush-masked strips (SMUDGETAPE01 + CRISPTAPE01), distressed charcoal panels
// (BGNOISE01 / PANEL01 / MAPPANEL01), cropped HUD/menu pills (PILL01 / HUDPILL),
// real controller face/bumper/trigger glyphs (BUTTON_BAR_SCENE), class/PI badges
// (PICLASS/CLASS_*), wristbands (WRISTBANDS/*), manufacturer logos (L_MAN_*),
// car thumbnails (Thumbnails/Thumbnail_*), 5-stat comparison bars, and smooth
// FH1 brush-wipe / cross-fade animation curves.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "fh1data/ui_scene.h"
#include "ui.h"

namespace carview::ui4 {

// ---- Authentic Forza Horizon 1 UI4 Palette (measured on oracle 1280x720 frames) ----
inline constexpr uint32_t kPink = ui::rgba(0xF5, 0x04, 0x5C);         // primary FH1 hot pink accent
inline constexpr uint32_t kPinkHi = ui::rgba(0xFF, 0x03, 0x61);       // highlight/active row pink
inline constexpr uint32_t kBrushBlack = ui::rgba(14, 15, 18, 232);    // unselected brush strip charcoal
inline constexpr uint32_t kCharcoal = ui::rgba(24, 26, 30, 228);      // distressed panel body
inline constexpr uint32_t kCharcoalDark = ui::rgba(14, 16, 20, 236);  // deep distressed card
inline constexpr uint32_t kPillGrey = ui::rgba(33, 33, 33, 255);      // 0.13 material grey (947_HUD / 925_PAUSE)
inline constexpr uint32_t kRowAlt = ui::rgba(38, 40, 46, 224);        // alternating table/list row
inline constexpr uint32_t kRowHighlight = ui::rgba(66, 68, 76, 238);  // highlighted player row (results.png)
inline constexpr uint32_t kOffWhite = ui::rgba(232, 234, 238, 244);   // off-white strip (Balance, Credits, Table Header)
inline constexpr uint32_t kWhite = ui::rgba(248, 249, 252, 255);
inline constexpr uint32_t kDim = ui::rgba(174, 180, 192, 255);
inline constexpr uint32_t kInk = ui::rgba(26, 28, 32, 255);           // dark ink on off-white / yellow tags
inline constexpr uint32_t kYellow = ui::rgba(0xF7, 0xFF, 0x26);       // skill chain / highlight yellow
inline constexpr uint32_t kAmber = ui::rgba(0xFF, 0xB8, 0x1C);        // warm festival gold/amber
inline constexpr uint32_t kGreen = ui::rgba(0x46, 0xDC, 0x6E);        // GPS / positive stat delta / A button
inline constexpr uint32_t kRed = ui::rgba(0xEC, 0x34, 0x2C);          // negative stat delta / B button
inline constexpr uint32_t kBlue = ui::rgba(0x2C, 0x8C, 0xE8);         // X button / blue badge
inline constexpr uint32_t kSoftShadow = ui::rgba(0, 0, 0, 145);

// Signature FH1 UI4 panel & brush tilt: ~-2.8 deg rising left-to-right (SPEC-UI-LAYOUTS).
inline constexpr float kTiltRad = -0.049f;

// Shared texture & scene resolvers (bound once by the app or tests; also synced
// automatically by carview::set_ui_textures / carview::set_ui_scenes).
void set_texture_loader(std::function<bool(const std::string&, std::array<float, 4>&)> fn);
void set_scene_loader(std::function<const fh1::UiScene*(const std::string&)> fn);
bool texture(const std::string& path, std::array<float, 4>& uv);
const fh1::UiScene* scene(const std::string& name);

// Smooth cubic ease-out (0..1) and 3-frame FH1 selection cross-fade helper.
float ease_out3(float t);
float clamp01(float v);

// RAII tilt scope around (cx, cy).
struct TiltScope {
  ui::List& list;
  bool active = false;
  TiltScope(ui::List& l, float cx, float cy, float angle = kTiltRad) : list(l) {
    if (!l.turned() && angle != 0.0f) {
      l.turn(cx, cy, angle);
      active = true;
    }
  }
  ~TiltScope() {
    if (active) list.unturn();
  }
};

// Typography helpers.
ui::TextStyle style(ui::Font f, float size, uint32_t color, ui::Align align = ui::Align::kLeft,
                    bool keep_case = false);
void fit_text(std::string_view text, ui::TextStyle& ts, float max_width, float min_size);
float shadowed_text(ui::List& l, float x, float y, std::string_view text, const ui::TextStyle& ts, float s);

// Authentic FH1 class & wristband colour helpers.
uint32_t class_colour_by_name(std::string_view cls);
uint32_t class_colour_by_id(int class_id);
uint32_t wristband_colour(std::string_view name);
std::string format_credits(int64_t cr);

// ---- Authentic FH1 Distressed Brush / Tape / Pill / Panel Primitives ----

// Layered SMUDGETAPE01 + CRISPTAPE01 (or CRISPTAPEBIG01 when `big` is true).
// When `wipe_frac` < 1.0, animates a crisp left-to-right brush wipe.
void brush(ui::List& l, float x0, float y0, float x1, float y1, uint32_t colour, bool big = false,
           float wipe_frac = 1.0f);

// Cropped Horizon pill (DIRTMASKS/PILL01, HUDPILL01, HUDPILL02, or MAPPILLSMALL)
// with optional bottom-left PILLPIN notch.
enum class PillKind { kMenu, kHud01, kHud02, kMapSmall };
void pill(ui::List& l, float x0, float y0, float x1, float y1, uint32_t colour, PillKind kind = PillKind::kMenu,
          bool with_pin = false);

// Authentic FH1 distressed sharp-edged panel (BGNOISE01 grit overlay, zero
// generic rounded dark glass boxes). Optional top or left colour accent strip.
void panel(ui::List& l, float x0, float y0, float x1, float y1, float s, uint32_t bg = kCharcoal,
           uint32_t top_accent = 0, uint32_t left_accent = 0, bool gritty = true);

// Distressed off-white strip (Balance, Credits, Results column header).
void offwhite_strip(ui::List& l, float x0, float y0, float x1, float y1, float s, bool brushed = true);

// Iconic tilted (-2.8 deg) FH1 screen title banner with optional eyebrow strip
// (e.g. eyebrow "FESTIVAL" on black brush over "AUTOSHOW" on hot-pink brush).
void title_banner(ui::List& l, float s, float x, float y, std::string_view title,
                  std::string_view eyebrow = {}, bool pink_title = true, float max_w = 640.0f);

// Authentic FH1 horizontal tab strip with LB / RB controller bumpers.
void tab_strip(ui::List& l, float s, float x0, float y0, float max_x, const std::vector<std::string>& tabs,
               int active_idx, bool show_bumpers = true, uint32_t active_colour = kPink);

// Bottom BUTTON_BAR_SCENE prompt strip with authentic CONTROLLER/* glyphs.
struct Prompt {
  const char* button = "A";  // A, B, X, Y, LB, RB, LT, RT, START, BACK, DPAD, LS, RS, or key label
  std::string label;
};
void button_bar(ui::List& l, const std::vector<Prompt>& items, float screen_w, float screen_h,
                float left_x_canvas = 70.0f);
// Draws a single controller button glyph centred at (cx, cy) with radius `r`.
void button_glyph(ui::List& l, float cx, float cy, float r, const char* button, float alpha = 1.0f);

// Authentic PICLASS/CLASS_* badge + crisp dark PI numerals inside the white square.
void class_badge(ui::List& l, float x, float y, float h, std::string_view class_name, int pi,
                 int class_id_fallback = -1);

// Top-right Career Header Strip pixel-matched to out/oracle/ui/pause-freeroam.png:
// Wristband goal + WRISTBANDS/<COLOR> graphic + Car Name & PI + Off-white Credits strip.
struct CareerStripData {
  std::string band_name;
  std::string band_next;
  int band_level = 0;
  int band_points = 0;
  int band_hi = -1;
  bool band_collect_ready = false;
  std::string car_name;
  std::string car_class;
  int car_pi = 0;
  int64_t credits = -1;
  int popularity_rank = 0;
  int discount_percent = 0;  // optional upgrade discount tag
};
void career_strip(ui::List& l, const CareerStripData& data, float screen_w, float screen_h);

// Authentic FH1 segmented 5-stat bar (Speed / Handling / Acceleration / Launch / Braking, 0..10).
void stat_bar(ui::List& l, float x, float y, float w, float h, std::string_view label, double cur_val,
              double preview_val, bool show_diff, float s);

// Authentic manufacturer logo from Textures.zip (UI4/Logos/Manufacturer_Logos/L_MAN_*_Logo).
// Returns true if texture was drawn; false if caller should draw text fallback.
bool manufacturer_logo(ui::List& l, float x0, float y0, float x1, float y1, std::string_view make_name,
                       uint32_t tint = kWhite);

// Authentic car thumbnail from Textures.zip (Thumbnails/Thumbnail_<CarId>).
// Returns true if texture was drawn.
bool car_thumbnail(ui::List& l, float x0, float y0, float x1, float y1, int64_t car_id, uint32_t tint = kWhite);

// Subtle edge vignette (NEVER a muddy dark box over Colorado).
void subtle_vignette(ui::List& l, float w, float h, float strength = 0.25f);

}  // namespace carview::ui4
