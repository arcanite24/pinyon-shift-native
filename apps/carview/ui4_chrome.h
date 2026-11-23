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

// ---- Authentic Forza Horizon 1 UI4 Palette (exact FBF material values) ----
inline constexpr uint32_t kPink = ui::rgba(0xF9, 0x00, 0x64);         // primary FH1 hot pink (#F90064)
inline constexpr uint32_t kPinkHi = ui::rgba(0xFF, 0x1A, 0x72);       // highlight/active row pink
inline constexpr uint32_t kBrushBlack = ui::rgba(0x0E, 0x0E, 0x0E, 240);  // deep tape black (#0E0E0E)
inline constexpr uint32_t kCharcoal = ui::rgba(0x21, 0x21, 0x21, 232);    // distressed panel charcoal (#212121)
inline constexpr uint32_t kCharcoalDark = ui::rgba(0x0E, 0x0E, 0x0E, 240); // deep distressed card (#0E0E0E)
inline constexpr uint32_t kPillGrey = ui::rgba(0x21, 0x21, 0x21, 255);    // 0.13 material grey (#212121)
inline constexpr uint32_t kRowAlt = ui::rgba(38, 40, 46, 224);        // alternating table/list row
inline constexpr uint32_t kRowHighlight = ui::rgba(66, 68, 76, 238);  // highlighted player row (results.png)
inline constexpr uint32_t kOffWhite = ui::rgba(232, 234, 238, 244);   // off-white strip (Balance, Credits, Table Header)
inline constexpr uint32_t kWhite = ui::rgba(248, 249, 252, 255);
inline constexpr uint32_t kDim = ui::rgba(174, 180, 192, 255);
inline constexpr uint32_t kInk = ui::rgba(26, 28, 32, 255);           // dark ink on off-white / yellow tags
inline constexpr uint32_t kYellow = ui::rgba(0xDA, 0xFF, 0x00);       // skill / popularity lime-yellow (#DAFF00)
inline constexpr uint32_t kAmber = ui::rgba(0xFF, 0xB8, 0x1C);        // warm festival gold/amber
inline constexpr uint32_t kGreen = ui::rgba(0x46, 0xDC, 0x6E);        // GPS / positive stat delta / A button
inline constexpr uint32_t kRed = ui::rgba(0xEC, 0x34, 0x2C);          // negative stat delta / B button
inline constexpr uint32_t kBlue = ui::rgba(0x2C, 0x8C, 0xE8);         // X button / blue badge
inline constexpr uint32_t kSoftShadow = ui::rgba(0, 0, 0, 145);

// Signature FH1 UI4 panel & brush tilt: +3.0 deg in BSG y-up = -0.05236 rad clockwise in screen y-down.
inline constexpr float kTiltRad = -0.05236f;

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

// Cropped Horizon pill (DIRTMASKS/PILL01, PILL02, PILL03, PILL_BIG, HUDPILL01, HUDPILL02,
// MAPPILLSMALL, or KITPARTS/PILL26PX) with optional bottom-left PILLPIN / PILLCLIPON01 notch.
enum class PillKind { kMenu, kPill02, kPill03, kPillBig, kPill26, kHud01, kHud02, kMapSmall };
void pill(ui::List& l, float x0, float y0, float x1, float y1, uint32_t colour, PillKind kind = PillKind::kMenu,
          bool with_pin = false, bool with_clipon = false);

// Authentic FH1 distressed sharp-edged panel (BGNOISE01 grit overlay, zero
// generic rounded dark glass boxes). Optional top or left colour accent strip.
void panel(ui::List& l, float x0, float y0, float x1, float y1, float s, uint32_t bg = kCharcoal,
           uint32_t top_accent = 0, uint32_t left_accent = 0, bool gritty = true);

// Authentic FH1 DIRTMASKS/PANEL01 compact detail card (used by H_FIXED_SETTING_LIST,
// 300_I_OPT_*, 104_C_CAR_DETAILS, and Phase 4 blades).
void panel01(ui::List& l, float x0, float y0, float x1, float y1, float s, uint32_t bg = kCharcoalDark,
             uint32_t header_accent = 0);

// Authentic FH1 TitleTape (TAPECLIPON01/02 + SMUDGETAPE01 + CRISPTAPE01 + DIRTTILE01).
void title_tape(ui::List& l, float s, float x, float y, std::string_view title, uint32_t colour = kBrushBlack,
                float font_size = 38.0f, float max_w = 560.0f, bool clipon_left = true);

// Authentic FH1 OPTIONS/OPT_BAR_ARROW selector arrow (dir < 0 = left, dir > 0 = right).
bool opt_arrow(ui::List& l, float cx, float cy, float size, int dir, uint32_t tint = kWhite);

// Authentic FH1 OPTIONS/OPT_SLIDER_FRAME + DIRTTEXTURES/STATBARFILL/STATBAREMPTY slider bar.
void opt_slider(ui::List& l, float x0, float y0, float x1, float y1, float norm, float s, uint32_t fill_col = kPink,
                bool focused = false, float default_norm = -1.0f);

// Authentic FH1 vertical COMMON/SCROLLINDICATOR + SCROLLINDICATORMASK + SPECULAR/SPEC_SHEEN2 bar.
void scroll_indicator(ui::List& l, float x0, float y0, float x1, float y1, int first_idx, int visible_count,
                      int total_count, float s);

// Authentic FH1 upgrade part icon from Textures.zip (UI4/Upgrades/*, 479 icons).
bool upgrade_part_icon(ui::List& l, float x0, float y0, float x1, float y1, int part_enum, int level, bool is_stock,
                       uint32_t tint = kWhite);

// Authentic FH1 tuning icon from Textures.zip (UI4/Tuning/*, 31 textures).
bool tuning_icon(ui::List& l, float x0, float y0, float x1, float y1, int tune_category, std::string_view slider_label,
                 uint32_t tint = kWhite);

// Authentic FH1 Event Ticket artwork from Horizon.zip (EventTickets/*, 142+ textures).
// style: 1=Festival, 2=Street, 3=Showcase, 4=Nemesis/Showdown, 10=PR Speed, 11=PR Skill, 12=PR Photo.
bool event_ticket_art(ui::List& l, float x0, float y0, float x1, float y1, int event_id, int style,
                      uint32_t tint = kWhite);

// Authentic FH1 Sponsor Logo Tile from Horizon.zip (SponsorLogoTile/*.xds, all 18 logos).
bool sponsor_logo_tile(ui::List& l, float x0, float y0, float x1, float y1, std::string_view sponsor_or_id,
                       uint32_t tint = kWhite);

// Authentic FH1 medal / trophy / badge artwork (Icon_Gold, TicketStickerGold/Silver/Bronze, Trophy, Padlock).
enum class BadgeIconKind { kGoldMedal, kSilverMedal, kBronzeMedal, kTrophy, kLocked, kNew, kComplete };
bool badge_icon(ui::List& l, float x0, float y0, float x1, float y1, BadgeIconKind kind, uint32_t tint = kWhite);

// Clean human-readable car display name resolver for internal media IDs (e.g. FER_F40_87 -> 1987 Ferrari F40).
std::string pretty_car_media_name(std::string_view raw_media);

// Bottom BUTTON_BAR_SCENE prompt strip entry with authentic CONTROLLER/* glyphs.
struct Prompt {
  const char* button = "A";  // A, B, X, Y, LB, RB, LT, RT, START, BACK, DPAD, LS, RS, or key label
  std::string label;
};

// ---- Phase 4 Unified +3.0 deg Tilted Blade Chrome (H_FIXED_SETTING_LIST / 300_I_OPT_*) ----
struct BladeRow {
  std::string label;
  std::string subtitle;
  std::string value;
  uint32_t value_color = kWhite;
  bool selected = false;
  bool arrows = false;            // draws real OPTIONS/OPT_BAR_ARROW left/right
  bool has_slider = false;        // draws DIRTTEXTURES/STATBARFILL / OPT_SLIDER_FRAME
  float slider_norm = 0.0f;       // 0..1
  uint32_t slider_color = kYellow;
  uint32_t row_accent = 0;        // optional focused-row highlight override
  bool has_checkbox = false;
  bool checked = false;
  uint32_t checkbox_color = kGreen;
  std::string badge_tag;
  uint32_t badge_color = kAmber;
  int badge_icon_kind = -1;       // >= 0 casts to BadgeIconKind
  std::string index_label;
};

struct BladeDetailKv {
  std::string key;
  std::string value;
  uint32_t value_color = kWhite;
};

struct TiltedBladeSpec {
  std::string title;
  std::string eyebrow;
  std::string scene_hint = "H_FIXED_SETTING_LIST";
  // Compact top-right status strip (NO full-screen dark wall!)
  std::string status_kicker;
  std::string status_value;
  std::string status_tag;
  uint32_t status_color = kGreen;
  // Optional tilted LB/RB tab strip
  std::vector<std::string> tabs;
  int active_tab = 0;
  bool show_tab_bumpers = true;
  // Floating PILL02/PILL03 list column
  std::string list_header;
  std::string list_header_right;
  std::string empty_message;
  std::vector<BladeRow> rows;
  int selected_index = 0;
  int max_visible = 9;
  // Compact right-hand DIRTMASKS/PANEL01 detail card (tightly sized to content — ZERO empty charcoal void!)
  std::string detail_header;
  std::string detail_title;
  uint32_t detail_title_color = kPink;
  std::vector<std::string> detail_paragraphs;
  std::vector<BladeDetailKv> detail_kvs;
  float detail_progress = -1.0f;
  std::string detail_progress_label;
  uint32_t detail_progress_color = kYellow;
  int detail_icon_kind = -1;      // >= 0 casts to BadgeIconKind
  std::string detail_footer;
  std::string notice;
  std::vector<Prompt> prompts;
};

// Renders a unified +3.0 deg FH1 tilted blade screen over open Colorado using real
// H_FIXED_SETTING_LIST / 300_I_OPT_ASSISTS_DIFFICULTY 2-tex textures (TitleTape,
// floating PILL02/PILL03 rows, OPT_BAR_ARROW, STATBARFILL, SCROLLINDICATOR+SPEC_SHEEN2,
// compact PANEL01 detail card, and BUTTON_BAR_SCENE).
void tilted_blade_screen(ui::List& l, float w, float h, const TiltedBladeSpec& spec);

// Distressed off-white strip (Balance, Credits, Results column header).
void offwhite_strip(ui::List& l, float x0, float y0, float x1, float y1, float s, bool brushed = true);

// Iconic tilted (-2.8 deg) FH1 screen title banner with optional eyebrow strip
// (e.g. eyebrow "FESTIVAL" on black brush over "AUTOSHOW" on hot-pink brush).
void title_banner(ui::List& l, float s, float x, float y, std::string_view title,
                  std::string_view eyebrow = {}, bool pink_title = true, float max_w = 640.0f);

// Authentic FH1 horizontal tab strip with LB / RB controller bumpers.
void tab_strip(ui::List& l, float s, float x0, float y0, float max_x, const std::vector<std::string>& tabs,
               int active_idx, bool show_bumpers = true, uint32_t active_colour = kPink);

// Controller glyph family shown in BUTTON_BAR_SCENE & menus (automatically switched
// when a DualSense / DualShock 4 is active, defaulting to Xbox CONTROLLER/* textures).
enum class GlyphSet : uint8_t { kXbox = 0, kPlayStation = 1 };
void set_glyph_set(GlyphSet set);
GlyphSet glyph_set();

// Bottom BUTTON_BAR_SCENE prompt strip with authentic CONTROLLER/* glyphs.
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
