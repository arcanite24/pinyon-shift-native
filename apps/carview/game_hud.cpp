#include "game_hud.h"

#include "hud_scene.h"
#include "touch_controls.h"
#include "ui4_chrome.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace carview {
namespace {

using ui::Align;
using ui::rgba;
using ui::TextStyle;

// The palette: dark glass panels, white text, an amber accent.
constexpr uint32_t kPanel = rgba(10, 12, 17, 200);
constexpr uint32_t kPanelLow = rgba(6, 7, 10, 225);
constexpr uint32_t kShadow = rgba(0, 0, 0, 110);
constexpr uint32_t kText = rgba(244, 245, 248);
constexpr uint32_t kDim = rgba(160, 166, 178);
constexpr uint32_t kAccent = rgba(255, 186, 20);
constexpr uint32_t kBlue = rgba(0x2C, 0x8C, 0xE8);  // the X button's disc without its texture (ours)
constexpr uint32_t kRed = rgba(236, 52, 44);
constexpr uint32_t kGreen = rgba(70, 220, 110);
constexpr uint32_t kInk = rgba(14, 14, 16);
constexpr float kDialA0 = -2.36f, kDialA1 = 2.36f;  // the dial's sweep, radians from 12 o'clock

float clamp01(double v) { return float(std::clamp(v, 0.0, 1.0)); }
// 0 to 1 over `len` s from `t0`, eased out.
float ease_in_time(double t, double t0, double len) {
  const float k = clamp01((t - t0) / len);
  return 1 - (1 - k) * (1 - k) * (1 - k);
}

void panel(ui::List& l, float x0, float y0, float x1, float y1, float s, uint32_t color = kPanel) {
  l.shadow(x0, y0 + 3 * s, x1, y1 + 3 * s, kShadow, 10 * s, 12 * s);
  l.rect(x0, y0, x1, y1, color, 10 * s, ui::fade(color, 1.15));
}

void label(ui::List& l, float x, float y, const char* text, float s, uint32_t color = kAccent,
           Align align = Align::kLeft) {
  l.text(x, y, text, TextStyle{.size = 8 * s, .weight = 0.12f, .tracking = 0.3f, .color = color, .align = align});
}

// A wristband's colour by its name (brightened for dark glass legibility).
uint32_t band_colour(const std::string& name) {
  std::string n;
  for (char c : name) n += char(std::tolower(static_cast<unsigned char>(c)));
  const std::pair<const char*, uint32_t> named[] = {
      {"yellow", rgba(255, 214, 40)}, {"green", rgba(75, 220, 110)},  {"orange", rgba(255, 146, 36)},
      {"blue", rgba(80, 175, 255)},   {"purple", rgba(198, 120, 255)}, {"red", rgba(236, 52, 44)},
      {"gold", rgba(246, 206, 78)},   {"silver", rgba(205, 210, 220)}, {"black", rgba(120, 124, 134)},
      {"white", rgba(244, 244, 246)}, {"pink", rgba(255, 110, 185)}};
  for (const auto& [k, c] : named)
    if (n.find(k) != std::string::npos) return c;
  return kAccent;
}

// The wristband row of the results card: the band's name, the points and
// a bar that fills from before to after (from the new band's start when it
// went up).
void wristband_row(ui::List& l, const RaceHud& r, float x, float y, float width, float s, double since, float in) {
  const uint32_t c = band_colour(r.band_name);
  l.text(x, y, (r.band_name.empty() ? std::string("NO") : r.band_name) + " WRISTBAND",
         TextStyle{.size = 10 * s, .weight = 0.12f, .tracking = 0.12f, .color = ui::fade(c, in)});
  const float k = ease_in_time(since, 1.0, 1.3);
  const double from = r.band_up ? r.band_lo : r.band_before;
  const double now = from + (r.band_after - from) * k;
  char pts[64];
  if (r.band_hi > 0)
    std::snprintf(pts, sizeof pts, "%s / %s", thousands(int64_t(std::llround(now))).c_str(),
                  thousands(r.band_hi).c_str());
  else
    std::snprintf(pts, sizeof pts, "%s", thousands(int64_t(std::llround(now))).c_str());
  l.text(x + width, y, pts, TextStyle{.size = 10 * s, .weight = 0.11f, .color = ui::fade(kDim, in), .align = Align::kRight});
  const float by = y + 18 * s, bh = 8 * s;
  l.rect(x, by, x + width, by + bh, rgba(255, 255, 255, uint8_t(36 * in)), 4 * s);
  const double span = r.band_hi > r.band_lo ? double(r.band_hi - r.band_lo) : 1.0;
  auto at = [&](double v) { return x + width * float(std::clamp((v - r.band_lo) / span, 0.0, 1.0)); };
  if (r.band_hi <= 0) {
    l.rect(x, by, x + width, by + bh, ui::fade(c, in), 4 * s);
  } else {
    l.rect(x, by, std::max(x + bh, at(now)), by + bh, ui::fade(c, in), 4 * s);
    if (!r.band_up && r.band_before > r.band_lo)  // the points that were there, a shade darker
      l.rect(x, by, std::max(x + bh, at(r.band_before)), by + bh, ui::fade(rgba(0, 0, 0, 90), in), 4 * s);
  }
  if (!r.band_next.empty() && r.band_hi > 0)
    l.text(x + width, by + bh + 6 * s, "NEXT: " + r.band_next,
           TextStyle{.size = 7.5f * s, .weight = 0.11f, .tracking = 0.2f, .color = ui::fade(kDim, in),
                     .align = Align::kRight});
}

// ---- SPEC-UI-LAYOUTS: the title's 1280x720 layout, scaled to the screen's
// height and anchored to the edges it sits by. Brushes are flat rects until
// the scene reader brings the textures (FORMAT-UISCENES).
constexpr uint32_t kPink = rgba(0xF5, 0x04, 0x5C);
constexpr uint32_t kPinkRow = rgba(0xFF, 0x03, 0x61);
constexpr uint32_t kBlack60 = rgba(0, 0, 0, 153);
constexpr uint32_t kBrush = rgba(0, 0, 0, 215);
constexpr uint32_t kGear = rgba(0xBA, 0xB9, 0xB8);
constexpr uint32_t kDark = rgba(0x2D, 0x2D, 0x2D);
constexpr uint32_t kLight = rgba(0xD8, 0xD8, 0xD8, 235);
constexpr uint32_t kSoft = rgba(0, 0, 0, 120);

struct Layout {
  float w, h, s;
  float L(float x) const { return x * s; }                  // left-anchored x
  float R(float x) const { return w - (1280 - x) * s; }     // right-anchored x
  float T(float y) const { return y * s; }                  // top-anchored y
  float B(float y) const { return h - (720 - y) * s; }      // bottom-anchored y
  float C(float x) const { return 0.5f * w + (x - 640) * s; }  // centred x
};

// A brush (FORMAT-UISCENES: the scenes' tapes are white alpha masks,
// HORIZON\DIRTMASKS\SMUDGETAPE01 under CRISPTAPE01, tinted by the
// material): the masks stretched over the rect; a flat rect without them.
void brush(ui::List& l, float x0, float y0, float x1, float y1, uint32_t colour) {
  std::array<float, 4> smudge, crisp;
  if (!ui::atlas_find("tape.smudge", smudge) || !ui::atlas_find("tape.crisp", crisp)) {
    l.rect(x0, y0, x1, y1, colour);
    return;
  }
  // The tapes' ragged ends reach past the rect a little.
  const float px = 0.03f * (y1 - y0), py = 0.1f * (y1 - y0);
  l.image(x0 - px, y0 - py, x1 + px, y1 + py, smudge, colour);
  l.image(x0 - px, y0 - py, x1 + px, y1 + py, crisp, colour);
}

// A HUD pill (947_HUD's leaderboard and timer bars: DIRTMASKS/HUD/HUDPILL01
// or 02, 1024 x 32 with the pill in rows 3-28, tinted; the
// scene crops it with a second, offset copy, here the strip's left part
// and its right end): a flat rect without the texture.
constexpr uint32_t kPillGrey = rgba(33, 33, 33);  // the bars' material, 0.13
void pill(ui::List& l, float x0, float y0, float x1, float y1, uint32_t colour, bool second = false,
          const char* texture = nullptr, float texels = 1024) {
  std::array<float, 4> uv;
  if (!texture) texture = second ? "DIRTMASKS/HUD/HUDPILL02" : "DIRTMASKS/HUD/HUDPILL01";
  if (!ui_texture(texture, uv)) {
    l.rect(x0, y0, x1, y1, colour == kPillGrey ? kBlack60 : colour);
    return;
  }
  const float texel = (y1 - y0) / 26.0f;  // px per texel
  const float ty0 = y0 - 3 * texel, ty1 = y1 + 3 * texel;
  const float cap = std::min(12 * texel, 0.5f * (x1 - x0));
  const float du = (uv[2] - uv[0]) / texels;
  const float body = (x1 - cap - x0) / texel;  // texels
  l.image(x0, ty0, x1 - cap, ty1, {uv[0], uv[1], uv[0] + du * std::min(body, texels - 12), uv[3]}, colour);
  l.image(x1 - cap, ty0, x1, ty1, {uv[2] - du * 12, uv[1], uv[2], uv[3]}, colour);
}

// A string-table text (FORMAT-STRINGTABLES `_&N`: the table's and the id's
// hashes); `fallback` when the tables do not resolve it.
uint16_t table_hash(std::string_view v) {
  uint16_t h = 0xFFFF;
  for (char c : v) {
    h ^= uint16_t(std::tolower(static_cast<unsigned char>(c)));
    h = uint16_t((h << 7) | (h >> 9));
  }
  return h;
}
std::string str(std::string_view table, std::string_view id, const char* fallback) {
  const uint32_t key = uint32_t(table_hash(table)) << 16 | table_hash(id);
  const std::string ref = "_&" + std::to_string(key);
  std::string t = game_text(ref);
  if (t.empty() || t == ref) return fallback;
  return t;
}

TextStyle font(ui::Font f, float size, uint32_t color, Align align = Align::kLeft) {
  return TextStyle{.size = size, .weight = 0.11f, .color = color, .align = align, .font = f};
}
// Text with the HUD's soft drop shadow.
float shadowed(ui::List& l, float x, float y, std::string_view t, TextStyle ts, float s) {
  TextStyle sh = ts;
  sh.color = ui::fade(kSoft, float((ts.color >> 24) & 0xFF) / 255.0f);
  l.text(x + 1.5f * s, y + 1.5f * s, t, sh);
  return l.text(x, y, t, ts);
}
// Shrinks `ts` until `t` fits `room` px.
void fit(std::string_view t, TextStyle& ts, float room, float floor) {
  while (ts.size > floor && ui::List::measure(t, ts) > room) ts.size *= 0.96f;
}

// "01:23.456".
std::string clock_ms(double t) {
  if (t < 0) t = 0;
  const int64_t ms = int64_t(std::llround(t * 1000));
  char b[32];
  std::snprintf(b, sizeof b, "%02lld:%02lld.%03lld", (long long)(ms / 60000), (long long)(ms / 1000 % 60),
                (long long)(ms % 1000));
  return b;
}
// "+1.234".
std::string gap_ms(double t) {
  char b[32];
  std::snprintf(b, sizeof b, "+%.3f", std::max(0.0, t));
  return b;
}

// A controller button's help item on a black panel: the face's coloured
// disc, its letter and the action (B 14).
void help_item(ui::List& l, const Layout& g, float x0, float x1, float y0, float y1, const char* face,
               uint32_t colour, const std::string& text, bool bottom = true) {
  auto Y = [&](float y) { return bottom ? g.B(y) : g.T(y); };
  const float s = g.s;
  // 925_PAUSE_MENU's help item: DIRTMASKS\PILL01 (512 wide) tinted 0.13
  // grey, its PILLPIN tab (16 px) under the left end.
  std::array<float, 4> pin;
  if (ui_texture("DIRTMASKS/PILL01", pin)) {
    pill(l, g.L(x0), Y(y0), g.L(x1), Y(y1) - 10 * s, kPillGrey, false, "DIRTMASKS/PILL01", 512);
    if (ui_texture("DIRTMASKS/PILLPIN", pin)) l.image(g.L(x0), Y(y1) - 14 * s, g.L(x0) + 16 * s, Y(y1) + 2 * s, pin, kPillGrey);
  } else {
    l.rect(g.L(x0), Y(y0), g.L(x1), Y(y1), kBlack60);
  }
  const float cy = 0.5f * (Y(y0) + Y(y1) - 10 * s), cx = g.L(x0) + 16 * s;
  std::array<float, 4> uv;
  if (ui_texture(std::string("CONTROLLER/") + face + "_BUTTON", uv)) {
    l.image(cx - 11 * s, cy - 11 * s, cx + 11 * s, cy + 11 * s, uv);  // 22 px (925_PAUSE_MENU BUTTON_LOGO)
  } else {
    l.circle(cx, cy, 9.5f * s, colour);
    l.text(cx, cy - 5 * s, face, font(ui::Font::kB, 10 * s, kInk, Align::kCenter));
  }
  l.text(cx + 16 * s, cy - 7 * s, text, font(ui::Font::kB, 14 * s, kText));
}

// (a) The free-roam dial: centre 1122,587, ±137° from 12 o'clock.
void dial_drawn(ui::List& l, const DriveHud& st, const Layout& g) {
  float s = g.s;
  float cx = g.R(1123), cy = g.B(593.3f);
  float nx = g.R(1122), ny = g.B(587);
  if (st.touch_hud_active) {
    TouchRect mm_r, tach_r;
    TouchControls::hud_safe_rects(g.w, g.h, mm_r, tach_r);
    s = std::min(g.s, (tach_r.h() * g.h) / 184.0f);
    cx = tach_r.cx() * g.w;
    cy = tach_r.cy() * g.h;
    nx = cx - 1.0f * s;
    ny = cy - 6.3f * s;
  }
  const float ox = cx - 1123 * s, oy = cy - 593.3f * s;
  constexpr float kSweep = 2.391f;  // 137°
  std::array<float, 4> ring;
  const bool textured = ui_texture("HUD/DIALS/MAINBACKRING", ring);
  if (textured) l.image(ox + 995 * s, oy + 465 * s, ox + 1251 * s, oy + 721 * s, ring, rgba(255, 255, 255, 242));
  else l.circle(cx, cy, 88 * s, rgba(0, 0, 0, 140));
  const double red = std::max(1000.0, st.redline_rpm);
  // The scale: whole thousands, at least 10, numerals every second one.
  const double top = std::max(10000.0, std::ceil(red * 1.1 / 2000) * 2000);
  auto angle = [&](double rpm) { return -kSweep + 2 * kSweep * float(std::clamp(rpm / top, 0.0, 1.0)); };
  if (!textured) l.arc(cx, cy, 87 * s, 2 * s, -kSweep, kSweep, rgba(255, 255, 255, 230));
  l.arc(cx, cy, 84 * s, 6 * s, angle(red), kSweep, kPink);  // the redline (REDLINE's mask is ours)
  const TextStyle num = font(ui::Font::kB, 15 * s, kText, Align::kCenter);
  // Hatches: large at the thousands, mid between, small at the quarters.
  for (double k = 0; k <= top + 1; k += 250) {
    const int q = int(std::lround(k / 250));
    const bool large = q % 4 == 0, mid = q % 2 == 0;
    const float a = angle(k), sa = std::sin(a), ca = -std::cos(a);
    const float w = (large ? 3.0f : mid ? 2.0f : 1.5f) * s;
    const uint32_t c = large ? rgba(255, 255, 255) : mid ? rgba(255, 255, 255, 230) : rgba(255, 255, 255, 122);
    l.line(cx + 80 * s * sa, cy + 80 * s * ca, cx + 87.5f * s * sa, cy + 87.5f * s * ca, w, c);  // ours: 2.5 px out, off the numerals
    if (!large || (q / 4) % 2) continue;
    char d[8];
    std::snprintf(d, sizeof d, "%d", q / 4);
    l.text(nx + 78.5f * s * sa, ny + 78.5f * s * ca - 7.5f * s, d, num);
  }
  // The needle: NEEDLE's bar.
  const float a = angle(st.rpm), sa = std::sin(a), ca = -std::cos(a);
  l.line(cx + 44 * s * sa, cy + 44 * s * ca, cx + 72 * s * sa, cy + 72 * s * ca, 3 * s, kPink);
  // The speed: three digits, the leading zeros at half white.
  const int v = std::min(999, int(std::lround(std::fabs(st.kmh))));
  char digits[8];
  std::snprintf(digits, sizeof digits, "%03d", v);
  const std::string all = digits;
  const size_t lead = v >= 100 ? 0 : v >= 10 ? 1 : 2;
  const TextStyle sp = font(ui::Font::kE, 28 * s, kText, Align::kRight);
  const float x1 = ox + 1162 * s, y = oy + 571 * s;
  const float tail = l.text(x1, y, all.substr(lead), sp);
  if (lead) {
    TextStyle z = sp;
    z.color = rgba(255, 255, 255, 128);
    l.text(x1 - tail, y, all.substr(0, lead), z);
  }
  TextStyle unit = font(ui::Font::kA, 10 * s, kDim);
  unit.keep_case = true;
  l.text(ox + 1112 * s, oy + 553 * s, str("UnitStrings", "IDS_Units_KMH", "km/h"), unit);
  const char* gear = st.gear < 0 ? "R" : st.gear == 0 ? "N" : nullptr;
  char gb[8];
  if (!gear) std::snprintf(gb, sizeof gb, "%d", st.gear), gear = gb;
  l.text(ox + 1093 * s, oy + 607 * s, gear, font(ui::Font::kE, 17 * s, kGear));
}

// The dial from 947_HUD's GAUGE_ANALOG: MainBackRing, the hatches
// (HATCHLARGE / MID / SMALL quads at radius 82, turned to their angle),
// the numerals (B 10 at radius 70), the NEEDLE quad (32 x 128, centred 50
// out) with its NEEDLETRAIL swooshes, the speed's three E 17 x 1.6 digit
// slots, the gear and the unit. The sweep is the numerals' rotation, 2.339
// rad either side of 12 o'clock. Ours: the scale's top (whole thousands
// past the redline), the trails' fade by the rev rate, and the redline
// band, which follows REDLINE's look (a pink band brightening outward to a
// rim at radius 85) as arcs, as how the scene turns its half ring isn't
// known. Without the scene: the drawn dial above.
void dial(ui::List& l, const DriveHud& st, const Layout& g) {
  static const SceneDraw sc(ui_scene("947_HUD"));
  const int gauge = sc.find("GAUGE_ANALOG");
  const int ring = sc.find("MainBackRing", gauge), needle = sc.find("Rectangle", sc.find("_P_Tach_Needle", gauge));
  std::array<float, 4> large, mid, small, nuv, trail;
  if (gauge < 0 || ring < 0 || needle < 0 || !ui_texture("HUD/DIALS/HATCHLARGE", large) ||
      !ui_texture("HUD/DIALS/HATCHMID", mid) || !ui_texture("HUD/DIALS/HATCHSMALL", small) ||
      !ui_texture("HUD/DIALS/NEEDLE", nuv))
    return dial_drawn(l, st, g);
  const bool trails = ui_texture("HUD/DIALS/NEEDLETRAIL", trail);
  const auto [gx, gy] = sc.at(gauge);
  float s = g.s;
  SceneFrame f{g.w - 1280 * s, g.h - 720 * s, s};  // bottom-right anchored
  if (st.touch_hud_active) {
    TouchRect mm_r, tach_r;
    TouchControls::hud_safe_rects(g.w, g.h, mm_r, tach_r);
    s = std::min(g.s, (tach_r.h() * g.h) / 184.0f);
    f = SceneFrame{tach_r.cx() * g.w - gx * s, tach_r.cy() * g.h - gy * s, s};
  }
  const float cx = f.x(gx), cy = f.y(gy);
  constexpr float kSweep = 2.339f;
  constexpr uint32_t kNeedle = rgba(250, 0, 99);  // the needle's material, 0.98 0 0.39
  sc.model(l, f, ring, sc.alpha(ring));
  const double red = std::max(1000.0, st.redline_rpm);
  const double top = std::max(10000.0, std::ceil(red * 1.1 / 2000) * 2000);
  auto angle = [&](double rpm) { return -kSweep + 2 * kSweep * float(std::clamp(rpm / top, 0.0, 1.0)); };
  // The redline band.
  const float a_red = angle(red);
  // (Measured on the original's race frame: a solid band from radius 61
  // to 86, deepening outward, with a bright rim.)
  l.arc(cx, cy, 73.5f * s, 25 * s, a_red, kSweep, ui::fade(kNeedle, 0.5), true);
  l.arc(cx, cy, 81.5f * s, 9 * s, a_red, kSweep, ui::fade(kNeedle, 0.3), true);
  l.arc(cx, cy, 85.5f * s, 2 * s, a_red, kSweep, kNeedle, true);
  // Hatches and numerals.
  const TextStyle num = [&] {
    TextStyle t = font(ui::Font::kB, 10 * s, kText, Align::kCenter);
    return t;
  }();
  // A hatch every 500 (the original's frames), large at the thousands.
  for (double k = 0; k <= top + 1; k += 500) {
    const int q = int(std::lround(k / 250));
    const bool is_large = q % 4 == 0, is_mid = false;
    const float a = angle(k), sa = std::sin(a), ca = std::cos(a);
    const float hw = (is_large || is_mid ? 6.5f : 7.0f) * s, hh = 4.5f * s;
    l.image_rotated(cx + 82 * s * sa, cy - 82 * s * ca, hw, hh, a, is_large ? large : is_mid ? mid : small,
                    rgba(255, 255, 255, uint8_t(is_large ? 255 : is_mid ? 230 : 122)));
    if (!is_large || (q / 4) % 2) continue;
    char d[8];
    std::snprintf(d, sizeof d, "%d", q / 4);
    l.text(cx + 70 * s * sa, cy - 70 * s * ca - 0.5f * num.size, d, num);
  }
  // The needle and its trails (the trail behind the needle's travel).
  const float a = angle(st.rpm), sa = std::sin(a), ca = std::cos(a);
  if (trails) {
    static double last_rpm = 0, last_t = -1, rate = 0;
    const double dt = st.time - last_t;
    if (last_t >= 0 && dt > 1e-4 && dt < 0.5) rate += (((st.rpm - last_rpm) / dt) - rate) * std::min(1.0, dt * 10);
    last_rpm = st.rpm, last_t = st.time;
    const float up = clamp01(rate / 6000), down = clamp01(-rate / 6000);
    // Up: 50 left of the needle and 65 out; Down mirrored to the right.
    for (int side : {-1, 1}) {
      const float k = side < 0 ? up : down;
      if (k <= 0.02f) continue;
      const float ox = 50.0f * side, oy = -65.0f;
      const float px = cx + s * (ox * ca - oy * sa), py = cy + s * (ox * sa + oy * ca);
      std::array<float, 4> uv = trail;
      if (side > 0) std::swap(uv[0], uv[2]);
      l.image_rotated(px, py, 32 * s, 32 * s, a, uv, ui::fade(kNeedle, k));
    }
  }
  // The needle's bar runs from radius 60 to 86 in the original's frames:
  // the quad's centre 65 out (the scene's static 50 is moved by code).
  l.image_rotated(cx + 65 * s * sa, cy - 65 * s * ca, 16 * s, 64 * s, a, nuv, kNeedle);
  // The assists' labels (ASSIST_ICONS: ABSON and TCS ON; 947_HUD Material17
  // is white/neutral grey, so active uses amber and off uses dim kPillGrey).
  const int assists = sc.find("ASSIST_ICONS", gauge);
  for (const auto& [name, active] : {std::pair{"HUD_ABS", st.abs}, std::pair{"HUD_TCS", st.tcs}}) {
    const int on = sc.find("Rectangle_ON", sc.find(name, assists));
    sc.model(l, f, on, active ? 1.0f : sc.alpha(on), 0, 0, 0, active ? rgba(255, 196, 40) : kPillGrey);
  }
  // The speed: three digit slots, the leading zeros at half white (ours).
  const int v = std::min(999, int(std::lround(std::fabs(st.kmh))));
  char digits[8];
  std::snprintf(digits, sizeof digits, "%03d", v);
  const size_t lead = v >= 100 ? 0 : v >= 10 ? 1 : 2;
  const int speedo = sc.find("Speedo", gauge);
  for (int i = 0; i < 3; ++i) {
    const int node = sc.find("digit" + std::to_string(i), speedo);
    sc.text(l, f, node, std::string(1, digits[i]), size_t(i) < lead ? rgba(255, 255, 255, 128) : kText, 0, 0, false);
  }
  const int unit = sc.find("TEXT_SPEED_UNIT_ANALOG", gauge);
  sc.text(l, f, unit, str("UnitStrings", "IDS_Units_KMH", "km/h"), ui::fade(kText, sc.alpha(unit)), 0, 0, false);
  const char* gear = st.gear < 0 ? "R" : st.gear == 0 ? "N" : nullptr;
  char gb[8];
  if (!gear) std::snprintf(gb, sizeof gb, "%d", st.gear), gear = gb;
  sc.text(l, f, sc.find("TEXT_GEAR_ANALOG", gauge), gear, kGear, 0, 0, false);
}

// The discovery card (the interim of SPEC-UI-LAYOUTS: an E line on a pink
// brush, a B line under it, top centre): drops in, holds, fades.
void popup_card_drawn(ui::List& l, const DriveHud& st, const Layout& g) {
  const double age = st.time - st.popup_at;
  if (st.popup_title.empty() || age < 0 || age > kPopupSeconds) return;
  const float s = g.s;
  const float in = ease_in_time(age, 0, 0.3);
  const float a = in * (1 - clamp01((age - (kPopupSeconds - 0.5)) / 0.5));
  // H_BARN_FIND_HUD's CombinedNotification (DoubleTape), bottom centre:
  // the HUDPANEL02 panel tinted 0.13 grey from x 450 (the pill in rows 4-58
  // of 64, so 603-657 here), the notification icon at 473-537 x 598-662,
  // TEXT2 (B 16) over TEXT1 (B 12) from x 532. The text boxes' anchor is
  // taken as their centre (ours); the meter under the text is ours.
  TextStyle ts = font(ui::Font::kB, 16 * s, ui::fade(kText, a));
  TextStyle body = font(ui::Font::kB, 12 * s, ui::fade(kText, a));
  fit(st.popup_title, ts, g.w - g.C(560) - 40 * s, 9 * s);
  const float tw = std::max(ui::List::measure(st.popup_title, ts), ui::List::measure(st.popup_text, body));
  const float x0 = g.C(450), x1 = std::max(g.C(917), g.C(532) + tw + 30 * s);
  const float dy = (1 - in) * 20 * s;
  const float y0 = g.B(599) + dy, y1 = g.B(663) + dy;
  std::array<float, 4> uv;
  if (ui_texture("DIRTMASKS/HUD/HUDPANEL02", uv)) {
    const float texel = (y1 - y0) / 64.0f, cap = 24 * texel, du = (uv[2] - uv[0]) / 1024.0f;
    const uint32_t c = ui::fade(kPillGrey, a);
    l.image(x0, y0, x1 - cap, y1, {uv[0], uv[1], uv[0] + du * std::min((x1 - cap - x0) / texel, 1000.0f), uv[3]}, c);
    l.image(x1 - cap, y0, x1, y1, {uv[2] - du * 24, uv[1], uv[2], uv[3]}, c);
  } else {
    l.rect(x0, y0 + 4 * s, x1, y1 - 6 * s, ui::fade(kBlack60, a));
  }
  if (ui_texture("ICONS/NOTIFICATION/MISCTEMP", uv))
    l.image(g.C(473), g.B(598) + dy, g.C(537), g.B(662) + dy, uv, ui::fade(rgba(255, 255, 255), a));
  else
    l.rect(g.C(489), g.B(614) + dy, g.C(521), g.B(646) + dy, ui::fade(st.popup_colour ? st.popup_colour : kPink, a));
  l.text(g.C(532) + 10 * s, g.B(620) - 0.5f * ts.size + dy, st.popup_title, ts);
  if (!st.popup_text.empty()) l.text(g.C(532) + 10 * s, g.B(643) - 0.5f * body.size + dy, st.popup_text, body);
  if (st.popup_progress >= 0) {
    const float bx0 = g.C(542), bx1 = x1 - 30 * s, by = g.B(654) + dy;
    const float k = float(std::clamp(st.popup_progress, 0.0, 1.0)) * ease_in_time(age, 0.3, 0.8);
    l.rect(bx0, by, bx1, by + 3 * s, ui::fade(rgba(255, 255, 255, 50), a));
    l.rect(bx0, by, bx0 + (bx1 - bx0) * k, by + 3 * s, ui::fade(st.popup_colour ? st.popup_colour : kPink, a));
  }
}

// The notification from the scene itself (H_BARN_FIND_HUD,
// CombinedNotification/State/DoubleTape): Panel and Panel2 (HUDPANEL02 crops
// tinted 0.13), the MiscIcon, TEXT2 over TEXT1. The panels' visible width
// follows the text out to RightAnchor at least (ours: the scene's crop is
// the width for its sample strings); the slide, fade and meter are ours.
// Without the scene: the drawn version above.
void popup_card(ui::List& l, const DriveHud& st, const Layout& g) {
  const double age = st.time - st.popup_at;
  if (st.popup_title.empty() || age < 0 || age > kPopupSeconds) return;
  static const SceneDraw sc(ui_scene("H_BARN_FIND_HUD"));
  const int tape = sc.find("DoubleTape");
  const int panel = sc.find("Panel", tape), panel2 = sc.find("Panel2", tape);
  const int title = sc.find("TEXT2", tape), body = sc.find("TEXT1", tape), icon = sc.find("MiscIcon", tape);
  if (panel < 0 || panel2 < 0 || title < 0 || body < 0) return popup_card_drawn(l, st, g);
  const float in = ease_in_time(age, 0, 0.3);
  const float a = in * (1 - clamp01((age - (kPopupSeconds - 0.5)) / 0.5));
  // Bottom-centre anchored.
  const SceneFrame f{0.5f * g.w - 640 * g.s, g.h - 720 * g.s, g.s};
  const float dy = (1 - in) * 20;
  const float tx = sc.at(title)[0];
  ui::TextStyle ts = sc.style(title, f, kText), bs = sc.style(body, f, kText);
  const float tw = std::max(ui::List::measure(st.popup_title, ts), ui::List::measure(st.popup_text, bs)) / g.s;
  const float right = std::max(sc.at(sc.find("RightAnchor", tape))[0], tx + std::min(tw, sc.text_width(title)) + 30);
  for (int p : {panel2, panel}) {
    const float x0 = sc.rect(p)[0];
    sc.model(l, f, p, a, 0, dy, right - x0);
  }
  if (!sc.model(l, f, icon, a, 0, dy))
    l.rect(f.x(489), f.y(614 + dy), f.x(521), f.y(646 + dy), ui::fade(kPink, a));
  sc.text(l, f, title, st.popup_title, ui::fade(kText, a), 0, dy);
  if (!st.popup_text.empty()) sc.text(l, f, body, st.popup_text, ui::fade(kText, a), 0, dy);
  if (st.popup_progress >= 0) {
    const float bx0 = tx, bx1 = right - 30, by = 654 + dy;
    const float k = float(std::clamp(st.popup_progress, 0.0, 1.0)) * ease_in_time(age, 0.3, 0.8);
    l.rect(f.x(bx0), f.y(by), f.x(bx1), f.y(by + 3), ui::fade(rgba(255, 255, 255, 50), a));
    l.rect(f.x(bx0), f.y(by), f.x(bx0 + (bx1 - bx0) * k), f.y(by + 3), ui::fade(st.popup_colour ? st.popup_colour : kPink, a));
  }
}

// (b1) The event's title card & pre-race grid menu (race-card.png & race-grid.png).
void event_card(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
  constexpr float kCardTilt = -0.052f;
  // Lift the card slightly when the grid menu's help bar sits beneath it.
  const float lift = r.grid_menu_open ? 34.0f : 0.0f;
  // Full-bleed-left pink brush with event name, overlapping black brush with event type.
  {
    l.turn(g.L(484), g.B(597 - lift), kCardTilt);
    brush(l, -24 * s, g.B(547 - lift), g.L(968), g.B(647 - lift), kPink);
    TextStyle ns = font(ui::Font::kE, 47 * s, kText);
    fit(r.event, ns, g.L(940) - g.L(100), 20 * s);
    l.text(g.L(100), g.B(588 - lift) + (47 * s - ns.size) * 0.5f, r.event, ns);
    l.unturn();
  }
  {
    l.turn(g.L(257), g.B(562 - lift), kCardTilt);
    const std::string type =
        !r.event_type.empty() ? r.event_type : str("CareerEventUI", "IDS_TitleFestivalRace", "FESTIVAL RACE");
    TextStyle ts = font(ui::Font::kE, 36 * s, kText);
    fit(type, ts, g.L(440) - g.L(100), 18 * s);
    const float tw = ui::List::measure(type, ts);
    brush(l, g.L(75), g.B(535 - lift), std::max(g.L(420), g.L(100) + tw + 34 * s), g.B(588 - lift), kBrush);
    l.text(g.L(100), g.B(544 - lift) + (36 * s - ts.size) * 0.5f, type, ts);
    l.unturn();
  }
  if (!r.grid_menu_open || r.grid_menu.empty()) return;
  // Tilted left-side Grid Menu (race-grid.png: START RACE, DIFFICULTY, QUIT RACE).
  l.turn(g.L(226), g.T(195), kCardTilt);
  for (size_t i = 0; i < r.grid_menu.size(); ++i) {
    const bool sel = int(i) == r.grid_selected;
    const float bottom = g.T(210 + 52.0f * float(i) + (int(i) >= r.grid_selected && r.grid_selected > 0 ? 10.0f : 0.0f));
    const std::string& item = r.grid_menu[i];
    TextStyle it = font(ui::Font::kE, (sel ? 38 : 34) * s, kText);
    fit(item, it, g.L(680), 14 * s);
    const float tw = ui::List::measure(item, it);
    if (sel) {
      brush(l, g.L(145), bottom - 56 * s, std::max(g.L(380), g.L(184) + tw + 38 * s), bottom + 8 * s, kPink);
      l.text(g.L(184), bottom - it.size, item, it);
    } else {
      brush(l, g.L(158), bottom - it.size - 5 * s, g.L(186) + tw + 24 * s, bottom + 5 * s, kBrush);
      l.text(g.L(186), bottom - it.size, item, it);
    }
  }
  l.unturn();
  if (!r.grid_car_info.empty()) {
    ui4::panel(l, g.R(840), g.T(40), g.R(1210), g.T(78), s, ui4::kCharcoalDark, ui4::kPink, 0, true);
    TextStyle cs = font(ui::Font::kB, 16 * s, kText, Align::kRight);
    cs.keep_case = true;
    fit(r.grid_car_info, cs, g.R(1196) - g.R(854), 9 * s);
    l.text(g.R(1196), g.T(51) + (16 * s - cs.size) * 0.5f, r.grid_car_info, cs);
  }
  ui4::button_bar(l, {{"A", str("HelpButtons", "IDS_Select", "SELECT")}, {"B", "QUIT RACE"}}, g.w, g.h);
}

// (b2) The race HUD: laps (or the distance), the timers, place, standings.
void race_hud(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
  const bool racing = r.phase == RaceHud::kRacing;
  // 947_HUD's EndConditionBlock and PlaceBlock (E 48 current, E 26 slash
  // and total, E 16 label; right-aligned boxes end at their node); the
  // drop shadow is ours. Without the scene: the drawn blocks below.
  static const SceneDraw sc(ui_scene("947_HUD"));
  const int endcon = sc.find("EndConditionBlock"), placeblk = sc.find("PlaceBlock");
  const bool scened = endcon >= 0 && placeblk >= 0;
  auto stext = [&](const SceneFrame& f, int node, const std::string& t) {
    sc.text(l, f, node, t, rgba(0, 0, 0, 140), 1.5f, 1.5f, false);
    sc.text(l, f, node, t, kText, 0, 0, false);
  };
  if (scened) {
    const SceneFrame tl{0, 0, s}, tr{g.w - 1280 * s, 0, s};
    if (r.circuit) {
      stext(tl, sc.find("TEXT_ENDCON_CURRENT", endcon), std::to_string(racing ? std::max(1, r.lap) : 0));
      stext(tl, sc.find("TEXT_ENDCON_SLASH", endcon), "/");
      stext(tl, sc.find("TEXT_ENDCON_FINAL", endcon), std::to_string(r.laps));
      stext(tl, sc.find("TEXT_ENDCON_UNITS", endcon), str("InGame", "IDS_Laps", "LAPS"));
    } else {
      stext(tl, sc.find("TEXT_ENDCON_CURRENT", endcon), std::to_string(int(std::lround(r.progress * 100))));
      stext(tl, sc.find("TEXT_ENDCON_UNITS", endcon), "%");
    }
    stext(tr, sc.find("TEXT_PLACE_LABEL", placeblk), str("InGame", "IDS_Place", "PLACE"));
    stext(tr, sc.find("TEXT_PLACE", placeblk), std::to_string(std::max(1, r.place)));
    stext(tr, sc.find("TEXT_PLACE_SLASH", placeblk), "/");
    stext(tr, sc.find("TEXT_PLACE_MAX", placeblk), std::to_string(r.cars));
  }
  // Laps: "1/3 LAPS"; sprints: the distance done.
  if (!scened) {
    char big[16], of[16];
    if (r.circuit) {
      std::snprintf(big, sizeof big, "%d", racing ? std::max(1, r.lap) : 0);
      std::snprintf(of, sizeof of, "/%d", r.laps);
    } else {
      std::snprintf(big, sizeof big, "%d", int(std::lround(r.progress * 100)));
      std::snprintf(of, sizeof of, "%%");
    }
    const float bw = shadowed(l, g.L(70), g.T(34), big, font(ui::Font::kE, 48 * s, kText), s);
    const float ow = shadowed(l, std::max(g.L(118), g.L(70) + bw + 1 * s), g.T(33), of,
                              font(ui::Font::kE, 27 * s, kText), s);
    if (r.circuit)
      shadowed(l, std::max(g.L(182), g.L(70) + bw + ow + 18 * s), g.T(34), str("InGame", "IDS_Laps", "LAPS"),
               font(ui::Font::kE, 16 * s, kText), s);
  }
  // The timers: RACE from the start, LAP on circuits, LAST and BEST after a lap.
  {
    struct Row { std::string name; double t; };
    std::vector<Row> rows;
    rows.push_back({str("InGame", "IDS_Race", "RACE"), std::max(0.0, r.clock)});
    if (r.circuit) {
      rows.push_back({str("InGame", "IDS_CurrentLapTimeLabel", "LAP"), std::max(0.0, r.lap_time)});
      if (r.last_lap > 0) rows.push_back({str("InGame", "IDS_Last", "LAST"), r.last_lap});
      if (r.best_lap > 0) rows.push_back({str("InGame", "IDS_Best", "BEST"), r.best_lap});
    }
    const float ink[] = {115, 143, 171, 198};
    for (size_t i = 0; i < rows.size() && i < 4; ++i) {
      const float y = g.T(ink[i]);
      pill(l, g.L(64), y - 5 * s, g.L(224), y + 19 * s, kPillGrey, true);
      l.text(g.L(74), y, rows[i].name, font(ui::Font::kB, 14 * s, kText));
      l.text(g.L(215), y, clock_ms(rows[i].t), font(ui::Font::kB, 14 * s, kText, Align::kRight));
    }
  }
  // Place: "PLACE 3/8".
  if (!scened) {
    char p[8], of[8];
    std::snprintf(p, sizeof p, "%d", std::max(1, r.place));
    std::snprintf(of, sizeof of, "/%d", r.cars);
    shadowed(l, g.R(1024), g.T(34), str("InGame", "IDS_Place", "PLACE"), font(ui::Font::kE, 16 * s, kText), s);
    const float ow = shadowed(l, g.R(1217), g.T(33), of, font(ui::Font::kE, 27 * s, kText, Align::kRight), s);
    shadowed(l, g.R(1217) - ow - 1 * s, g.T(32), p, font(ui::Font::kE, 50 * s, kText, Align::kRight), s);
  }
  // The standings: the leader and the rows round the player, four in all.
  if (!r.rows.empty()) {
    std::vector<size_t> pick;
    const size_t n = r.rows.size();
    size_t me = 0;
    for (size_t i = 0; i < n; ++i)
      if (r.rows[i].player) me = i;
    if (n <= 4 || me < 3) {
      for (size_t i = 0; i < std::min<size_t>(n, 4); ++i) pick.push_back(i);
    } else {
      const size_t last = std::min(n - 1, me + 1);
      pick = {0, last - 2, last - 1, last};
    }
    const float ty[] = {116, 143, 172, 200};
    for (size_t k = 0; k < pick.size(); ++k) {
      const RaceHudRow& row = r.rows[pick[k]];
      const float y = g.T(ty[k]);
      // As the original's race frames show (out/oracle race-hud): one pill
      // per row from x 1043 to 1213, the driver's name in B as written,
      // right-aligned to 1181, the place in E right-aligned to 1206; no gap
      // column.
      pill(l, g.R(1043), y - 5 * s, g.R(1213), y + 19 * s, row.player ? kPinkRow : kPillGrey);
      char pos[8];
      std::snprintf(pos, sizeof pos, "%d", row.place);
      l.text(g.R(1206), y - 0.5f * s, pos, font(ui::Font::kE, 15 * s, kText, Align::kRight));
      TextStyle ns = font(ui::Font::kB, 14 * s, kText, Align::kRight);
      ns.keep_case = true;
      fit(row.name, ns, g.R(1181) - g.R(1052), 9 * s);
      l.text(g.R(1181), y + (14 * s - ns.size) * 0.5f, row.name, ns);
    }
  }
  // GO!: 40 frames at the start signal.
  if (racing && r.clock < 40.0 / 60.0) {
    const float a = 1 - clamp01((r.clock - 0.5) / 0.17);
    shadowed(l, g.C(637.5f), g.T(230), "GO!", font(ui::Font::kE, 64 * s, ui::fade(kText, a), Align::kCenter), s);
  }
  // Wrong way: 947_HUD's WrongWay sign (WRONGWAYICON, 582,174-698,290),
  // pulsing (ours: the scene's animation is not read); without the
  // texture, a red plate.
  std::array<float, 4> ww_uv;
  if (racing && r.wrong_way && ui_texture("HUD/WRONGWAYICON", ww_uv)) {
    const float pulse = 0.75f + 0.25f * float(std::cos(r.clock * 6.2832 * 1.5));
    l.image(g.C(582), g.T(174), g.C(698), g.T(290), ww_uv, ui::fade(rgba(255, 255, 255), pulse));
  } else if (racing && r.wrong_way) {
    const float pulse = 0.75f + 0.25f * float(std::cos(r.clock * 6.2832 * 1.5));
    const TextStyle ws = font(ui::Font::kE, 30 * s, kText, Align::kCenter);
    const float ww = ui::List::measure("WRONG WAY", ws) + 60 * s, wy = g.T(140);
    l.rect(0.5f * g.w - 0.5f * ww, wy, 0.5f * g.w + 0.5f * ww, wy + 48 * s, ui::fade(kRed, pulse));
    l.text(0.5f * g.w, wy + 9 * s, "WRONG WAY", ws);
  }
}

// (c) After the finish: the FINISHED banner with the place, then the cash
// reward page.
// The post-race and pause scenes' RotationHelper / Option_List: 0.052 rad,
// rising to the right (FORMAT-UISCENES; the frames agree). Each element
// turns about its own measured centre, so the measured rects hold.
constexpr float kTilt = -0.052f;
struct Turn {
  ui::List& l;
  Turn(ui::List& list, float cx, float cy, float a = kTilt) : l(list) { l.turn(cx, cy, a); }
  ~Turn() { l.unturn(); }
};

// The RACE RESULTS table (SPEC-UI-LAYOUTS c, the race results table; X
// from the FINISHED banner, B back): H_POST_RACE_RESULTS as laid out, each
// node turned by its own rotation; the rows' text from the session. The
// scene's opacities are its intro's start (0): drawn at full.
void results_table(ui::List& l, const RaceHud& r, const Layout& g) {
  static const SceneDraw sc(ui_scene("H_POST_RACE_RESULTS"), true);
  const float s = g.s;
  help_item(l, g, 70, 150, 653, 689, "B", kRed, str("HelpButtons", "IDS_Back", "BACK"));
  if (!sc.ok()) return;
  const SceneFrame f{0.5f * g.w - 640 * s, 0.5f * g.h - 360 * s, s};
  sc.model(l, f, sc.find("BG_Noise"), 0.5f);
  // The title on its tape: the tapes cropped to the text (ours: a 40 px
  // margin past the text, as the frame shows).
  const int tape = sc.find("TitleTape");
  {
    const int tt = sc.find("TitleTapeText", tape);
    const std::string title = str("ScreenTitles", "IDS_PostRace_RaceResults", "RACE RESULTS");
    const float tw = ui::List::measure(title, sc.style(tt, f, kText)) / s;
    const float tx = sc.at(tt)[0];
    sc.model(l, f, sc.find("Clipon_2", tape));
    for (const char* name : {"Smudge_Tape", "Crisp_Tape"}) {
      const int n = sc.find(name, tape);
      sc.model(l, f, n, 1, 0, 0, tx + tw + 40 - sc.rect(n)[0]);
    }
    sc.text(l, f, tt, title, kText);
  }
  // The heading strip.
  const int head = sc.find("Heading");
  sc.model(l, f, sc.find("Pill_left", head));
  sc.model(l, f, sc.find("Pill_right", head));
  const std::pair<const char*, std::string> heads[] = {
      {"TEXT_POSITION", str("Scoreboard", "IDS_Pos", "POS")},
      {"TEXT_PLAYER", str("Scoreboard", "IDS_Driver", "DRIVER")},
      {"TEXT_CLASS", str("Scoreboard", "IDS_Class", "CLASS")},
      {"TEXT_CAR", str("Scoreboard", "IDS_Car", "CAR")},
      {"TEXT_BEST_LAP", str("InGame", "IDS_Header_Best_Lap", "BEST LAP")},
      {"TEXT_TOTAL_TIME", str("InGame", "IDS_Header_Total", "TOTAL")}};
  for (const auto& [node, text] : heads) {
    const int n = sc.find(node, head);
    // Upper case, as the frame shows.
    std::string up = text;
    for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
    sc.text(l, f, n, up, kDark);
  }
  // The rows: I_LIST_ITEM, I_LIST_ITEM2 ... I_LIST_ITEM8, by place.
  for (size_t i = 0; i < r.rows.size() && i < 8; ++i) {
    const RaceHudRow& row = r.rows[i];
    const int item = sc.find(i == 0 ? std::string("I_LIST_ITEM") : "I_LIST_ITEM" + std::to_string(i + 1));
    if (item < 0) break;
    // The player's row on a lighter grey (measured, about 0.24).
    const uint32_t tint = row.player ? rgba(61, 61, 61) : 0;
    const int pill = sc.find("Pill", item);
    sc.model(l, f, sc.find("Pill_left", pill), 1, 0, 0, 0, tint);
    sc.model(l, f, sc.find("Pill_right", pill), 1, 0, 0, 0, tint);
    sc.text(l, f, sc.find("TEXT_POSITION", item), std::to_string(row.place), kText);
    sc.text(l, f, sc.find("TEXT_PLAYER", item), row.name, kText);
    sc.text(l, f, sc.find("TEXT_CAR", item), row.car, kText);
    sc.text(l, f, sc.find("TEXT_BEST_LAP", item), row.best_lap > 0 ? clock_ms(row.best_lap) : "--:--.---", kText);
    sc.text(l, f, sc.find("TEXT_TOTAL_TIME", item), row.finished ? clock_ms(row.time) : "--:--.---", kText);
    // The class badge: PICLASS/CLASS_<class> at CLASS_A's place, scaled by
    // its CLASS_SM group (0.75); the index on its white part.
    const int badge = sc.find("CLASS_A", item);
    std::array<float, 4> uv;
    if (!row.car_class.empty() && badge >= 0 && ui_texture("PICLASS/CLASS_" + row.car_class, uv)) {
      const auto b = sc.rect(badge);
      const float cx = 0.5f * (b[0] + b[2]), cy = 0.5f * (b[1] + b[3]);
      // (The frame's badge is CLASS_A's full rect, about 52 px of ink.)
      const float hw = 0.5f * (b[2] - b[0]), hh = 0.5f * (b[3] - b[1]);
      const float angle = -sc.angle(badge);
      l.image_rotated(f.x(cx), f.y(cy), hw * s, hh * s, angle, uv);
      if (row.pi >= 0) sc.text(l, f, sc.find("TEXT_PI", item), std::to_string(row.pi), kDark);
    }
  }
}

// (c) The wristband points page (SPEC-UI-LAYOUTS, the wristband points
// banner, measured): POINTS EARNED on a black brush, the band's large
// wristband, COLLECT IT FROM RACE CENTRAL on a light panel when a level is
// reached (else the band's progress, ours), PLEASE WAIT bottom left. The
// scene (H_POST_RACE_CAREER_POINTS) starts off screen and its animation is
// not read: the frame's rects stand.
void points_page(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
  // The grainy dark panel behind the wristband (the frame: x 0-520, y 235-365).
  {
    Turn t(l, g.L(260), g.T(300), -0.03f);
    std::array<float, 4> noise;
    if (ui_texture("DIRTMASKS/BGNOISE01", noise)) l.image(g.L(-10), g.T(238), g.L(520), g.T(362), noise, rgba(0, 0, 0, 235));
    else l.rect(g.L(-10), g.T(238), g.L(520), g.T(362), rgba(0, 0, 0, 120));
  }
  {
    Turn t(l, g.L(339), g.T(192));
    brush(l, g.L(95), g.T(156), g.L(585), g.T(229), kBrush);
    const std::string title = str("PostRaceFlow", "IDS_PointsEarned", "POINTS EARNED");
    TextStyle ts = font(ui::Font::kE, 56 * s, kText);
    fit(title, ts, g.L(570) - g.L(108), 24 * s);
    l.text(g.L(108), g.T(166) + (56 * s - ts.size) * 0.5f, title, ts);
  }
  // The band's texture by the colour in its name (HORIZON\WRISTBANDS\<colour>XL).
  std::string lower, band;
  for (char c : r.band_name) lower += char(std::tolower(static_cast<unsigned char>(c)));
  for (const char* k : {"yellow", "green", "orange", "blue", "purple", "pink", "gold", "platinum"})
    if (lower.find(k) != std::string::npos) {
      for (const char* p = k; *p; ++p) band += char(std::toupper(static_cast<unsigned char>(*p)));
      if (band != "PLATINUM") band += "XL";
      break;
    }
  std::array<float, 4> uv;
  if (!band.empty() && ui_texture("WRISTBANDS/" + band, uv)) {
    // The texture's margins: its ink fills the frame's 95,222-454,361.
    l.image_rotated(g.L(274.5f), g.T(291.5f), 255 * s, 99 * s, -0.07f, uv);
  } else if (!r.band_name.empty()) {
    Turn t(l, g.L(274.5f), g.T(291.5f), -0.07f);
    l.rect(g.L(95), g.T(232), g.L(454), g.T(351), band_colour(r.band_name));
  }
  if (r.band_up) {
    Turn t(l, g.L(270), g.T(375));
    l.rect(g.L(100), g.T(359), g.L(440), g.T(391), kLight);
    const std::string c = str("PostRaceFlow", "IDS_WristbandAvailableLn2", "COLLECT IT FROM RACE CENTRAL");
    TextStyle cs = font(ui::Font::kB, 18 * s, kDark);
    fit(c, cs, g.L(432) - g.L(106), 10 * s);
    l.text(g.L(106), g.T(366) + (18 * s - cs.size) * 0.5f, c, cs);
  } else if (r.band_after >= 0) {
    const double since = r.clock - r.finish_time;
    wristband_row(l, r, g.L(100), g.T(368), g.L(440) - g.L(100), s, since, 1.0f);
  }
  // PLEASE WAIT: the festival spinner and the words on a dark panel.
  std::array<float, 4> sp;
  if (ui_texture("LOADING/LOADINGSPINNER50PX", sp)) l.image(g.L(60), g.B(638), g.L(112), g.B(690), sp);
  l.rect(g.L(110), g.B(653), g.L(234), g.B(681), kBlack60);
  l.text(g.L(118), g.B(660), str("Main", "IDS_Please_Wait_Title", "PLEASE WAIT"), font(ui::Font::kB, 15 * s, kText));
}

// (c4) Prize Car Won screen (H_POST_RACE_WON_CAR style).
void won_car_page(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
  ui4::subtle_vignette(l, g.w, g.h, 0.30f);
  {
    Turn t(l, g.L(340), g.T(215));
    TextStyle ts = font(ui::Font::kE, 48 * s, kText);
    const float tw = ui::List::measure("NEW CAR WON!", ts);
    brush(l, g.L(92), g.T(178), std::max(g.L(560), g.L(112) + tw + 38 * s), g.T(250), kBrush);
    l.text(g.L(112), g.T(188), "NEW CAR WON!", ts);
  }
  {
    Turn t(l, g.L(440), g.T(310));
    TextStyle ns = font(ui::Font::kE, 52 * s, kText);
    ns.keep_case = true;
    fit(r.won_car_name, ns, g.L(880) - g.L(108), 22 * s);
    const float nw = ui::List::measure(r.won_car_name, ns);
    brush(l, g.L(75), g.T(258), std::max(g.L(640), g.L(108) + nw + 46 * s), g.T(362), kPink);
    l.text(g.L(108), g.T(280) + (52 * s - ns.size) * 0.5f, r.won_car_name, ns);
  }
  {
    Turn t(l, g.L(280), g.T(398));
    ui4::offwhite_strip(l, g.L(105), g.T(378), g.L(470), g.T(418), s, true);
    l.text(g.L(122), g.T(389), "ADDED TO YOUR GARAGE", font(ui::Font::kB, 18 * s, kDark));
  }
  ui4::button_bar(l, {{"A", str("HelpButtons", "IDS_ContinueMenu", "CONTINUE")}}, g.w, g.h);
}

void results(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
  const double since = r.clock - r.finish_time;
  if (r.results_page == 3 && !r.won_car_name.empty()) {
    won_car_page(l, r, g);
    return;
  }
  if (r.results_page == 2) {
    points_page(l, r, g);
    return;
  }
  if (r.results_page == 0 && r.results_table) {
    results_table(l, r, g);
    return;
  }
  if (r.results_page == 0) {
    const float in = ease_in_time(since, 0, 0.35);
    const float dx = (1 - in) * -120 * s;
    {
      Turn t(l, g.L(376) + dx, g.T(270));
      brush(l, dx, g.T(204), g.L(753) + dx, g.T(337), kPink);
      const int place = r.stage ? r.stage_place : r.place;
      char p[16];
      std::snprintf(p, sizeof p, "%d%s ", place, place_suffix(place));
      const std::string text = p + str("InGame", "IDS_Place", "PLACE");
      TextStyle ps = font(ui::Font::kE, 80 * s, kText);
      fit(text, ps, g.L(735) - g.L(95), 30 * s);
      l.text(g.L(95) + dx, g.T(230) + (80 * s - ps.size) * 0.5f, text, ps);
    }
    {
      Turn t(l, g.L(229) + dx, g.T(200));
      brush(l, g.L(75) + dx, g.T(167), g.L(383) + dx, g.T(233), kBrush);
      TextStyle fs = font(ui::Font::kE, 45 * s, kText);
      fit(str("InGame", "IDS_Finished", "FINISHED"), fs, g.L(372) - g.L(85), 20 * s);
      l.text(g.L(85) + dx, g.T(171), str("InGame", "IDS_Finished", "FINISHED"), fs);
    }
    ui4::button_bar(l,
                    {{"A", str("HelpButtons", "IDS_ContinueMenu", "CONTINUE")},
                     {"Y", str("HelpButtons", "IDS_RestartMenu", "RESTART")},
                     {"X", str("InGame", "IDS_RaceResultsMenu", "RACE RESULTS")},
                     {"V", "VIEW REPLAY"}},
                    g.w, g.h);
    return;
  }
  // The cash reward (oracle results-credits.png).
  ui4::subtle_vignette(l, g.w, g.h, 0.25f);
  {
    Turn t(l, g.L(313), g.T(180));
    brush(l, g.L(98), g.T(141), g.L(529), g.T(218), kBrush);
    TextStyle ts = font(ui::Font::kE, 57 * s, kText);
    const std::string title = str("ScreenTitles", "IDS_PostRace_Winnings", "CASH REWARD");
    fit(title, ts, g.L(519) - g.L(108), 24 * s);
    l.text(g.L(108), g.T(151), title, ts);
  }
  const std::string cr = str("UnitStrings", "IDS_Units_Credits", "Cr");
  char line[64], diff_buf[32], assist_buf[32], xp_buf[64];
  const int place = r.stage ? r.stage_place : r.place;
  std::snprintf(line, sizeof line, "%s %d%s", str("InGame", "IDS_Finished", "FINISHED").c_str(), place, place_suffix(place));
  const int ai_pct = std::max(0, r.difficulty_pct - r.assists_pct);
  std::snprintf(diff_buf, sizeof diff_buf, "+%d%% (%s %s)", ai_pct, thousands(r.difficulty_bonus).c_str(), cr.c_str());
  const int64_t base_cr = r.base_credits > 0 ? r.base_credits : r.credits;
  std::vector<std::pair<std::string, std::string>> rows;
  rows.emplace_back(line, thousands(base_cr) + " " + cr);
  rows.emplace_back(str("OnlineRewards", "IDS_Mileage_Bonus_Heading", "MILEAGE BONUS"),
                    thousands(r.mileage_bonus) + " " + cr);
  rows.emplace_back(str("OnlineRewards", "IDS_Difficulty_Bonus_Heading", "AI DIFFICULTY BONUS"), diff_buf);
  if (r.assists_pct > 0 || r.assists_bonus > 0) {
    std::snprintf(assist_buf, sizeof assist_buf, "+%d%% (%s %s)", r.assists_pct, thousands(r.assists_bonus).c_str(),
                  cr.c_str());
    rows.emplace_back("ASSISTS BONUS", assist_buf);
  }
  if (r.skills_bonus > 0) rows.emplace_back("SKILLS BONUS", thousands(r.skills_bonus) + " " + cr);
  if (r.nemesis_bonus > 0) rows.emplace_back("NEMESIS BONUS", thousands(r.nemesis_bonus) + " " + cr);
  if (r.bounty_bonus > 0) rows.emplace_back("BOUNTY BONUS", thousands(r.bounty_bonus) + " " + cr);
  if (r.driver_xp > 0) {
    std::snprintf(xp_buf, sizeof xp_buf, "+%s XP  •  AFFINITY +%s XP", thousands(r.driver_xp).c_str(),
                  thousands(r.affinity_xp).c_str());
    rows.emplace_back("DRIVER & AFFINITY XP", xp_buf);
  }
  const float row_step = rows.size() > 4 ? 24.0f : 28.0f;
  for (size_t i = 0; i < rows.size(); ++i) {
    const float y = g.T(234 + row_step * float(i));
    const TextStyle ls = font(ui::Font::kB, (rows.size() > 5 ? 16 : 20) * s, kText);
    TextStyle vs = font(ui::Font::kE, (rows.size() > 5 ? 15 : 18) * s, kText);
    vs.keep_case = true;
    const float lw = ui::List::measure(rows[i].first, ls), vw = ui::List::measure(rows[i].second, vs);
    Turn t(l, g.L(148) + 0.5f * lw, y + 10 * s);
    brush(l, g.L(140), y - 4 * s, g.L(148) + lw + 6 * s, y + 22 * s, kBrush);
    l.text(g.L(148), y, rows[i].first, ls);
    const float vx = g.L(148) + lw + 10 * s;
    brush(l, vx - 4 * s, y - 2 * s, vx + vw + 6 * s, y + 20 * s, kPink);
    l.text(vx, y + 1 * s, rows[i].second, vs);
  }
  // TOTAL and its value.
  const float count = ease_in_time(since, 0.3, 1.0);
  {
    Turn tn(l, g.L(295), g.T(430));
    const std::string t = str("OnlineRewards", "IDS_Total", "TOTAL");
    TextStyle ts = font(ui::Font::kE, 75 * s, kText);
    fit(t, ts, g.L(449) - g.L(140), 30 * s);
    l.text(g.L(140), g.T(390) + (75 * s - ts.size), t, ts);
  }
  Turn tv(l, g.L(678), g.T(412));
  brush(l, g.L(476), g.T(360), g.L(880), g.T(464), kPink);
  TextStyle vs = font(ui::Font::kE, 70 * s, kText, Align::kRight);
  vs.keep_case = true;
  const std::string total = thousands(int64_t(std::llround(double(r.credits) * count))) + " " + cr;
  fit(total, vs, g.L(860) - g.L(490), 30 * s);
  l.text(g.L(860), g.T(377) + (70 * s - vs.size) * 0.5f, total, vs);
  l.unturn();
  if (r.balance >= 0) {
    Turn tb(l, g.L(280), g.T(497));
    ui4::offwhite_strip(l, g.L(150), g.T(475), g.L(410), g.T(520), s, true);
    TextStyle bs = font(ui::Font::kB, 16 * s, kDark);
    l.text(g.L(162), g.T(490), str("OnlineRewards", "IDS_Balance", "BALANCE"), bs);
    bs.align = Align::kRight;
    bs.keep_case = true;  // "117,881 Cr" as shown
    l.text(g.L(398), g.T(490), thousands(r.balance) + " " + cr, bs);
  }
  ui4::button_bar(l, {{"A", str("HelpButtons", "IDS_ContinueMenu", "CONTINUE")}}, g.w, g.h);
}

// A class's colour (B: #FF9203 per the layouts; the others ours).
uint32_t class_colour(const std::string& c) {
  if (c == "D") return rgba(0x2C, 0xB8, 0xF0);
  if (c == "C") return rgba(0xF5, 0xD0, 0x1C);
  if (c == "B") return rgba(0xFF, 0x92, 0x03);
  if (c == "A") return rgba(0xF0, 0x2C, 0x2C);
  if (c == "S") return rgba(0xB0, 0x3C, 0xE8);
  if (!c.empty() && c[0] == 'R') return rgba(0x3C, 0x6C, 0xF0);
  return rgba(0x80, 0x80, 0x80);
}

// (d) The pause menu & sub-menus: the items down the left on tilted brushes,
// the career's panels top right, optional right-hand detail card, help bar.
void draw_career_header(ui::List& l, const DriveHud& st, const Layout& g) {
  ui4::CareerStripData cs;
  cs.band_name = st.band_name;
  cs.band_next = st.band_next;
  cs.band_level = st.band_level;
  cs.band_points = st.band_points;
  cs.band_hi = st.band_hi;
  cs.band_collect_ready = st.band_collect_ready;
  cs.car_name = st.car_name;
  cs.car_class = st.car_class;
  cs.car_pi = st.car_pi;
  cs.credits = st.credits;
  cs.popularity_rank = st.popularity_rank;
  ui4::career_strip(l, cs, g.w, g.h);
}

void menu_panel(ui::List& l, const DriveHud& st, const Layout& g) {
  const float s = g.s;
  const bool pause = st.pause || st.menu_title == "paused";
  // Retail FH1 pause menu keeps Colorado crystal clear behind the tilted brushes
  // (out/oracle/ui/pause-freeroam.png); sub-menus use a subtle edge vignette only.
  ui4::subtle_vignette(l, g.w, g.h, pause ? 0.22f : 0.45f);
  std::string title = st.menu_title, hint;
  if (auto c = title.find(':'); c != std::string::npos) {
    hint = title.substr(c + 1);
    title = title.substr(0, c);
    while (!hint.empty() && hint.front() == ' ') hint.erase(hint.begin());
  }
  if (!pause && !title.empty()) {
    ui4::title_banner(l, s, g.L(92), g.T(40), title, hint, false, 640.0f);
  }
  const int n = int(st.menu.size());
  const bool has_right = st.career != nullptr || !st.detail_lines.empty();
  // Authentic 925_PAUSE_MENU SUPER_STACKER 50 px pitch (SPEC-UI-LAYOUTS d).
  const float row_step = pause ? 50.0f : 44.0f;
  const float first_bottom = pause ? 186.0f : 204.0f;
  const int fit_rows = std::clamp(int((g.B(612) - g.T(first_bottom)) / (row_step * s)) + 1, 1, pause ? 8 : 8);
  const int first = std::clamp(st.menu_selected - fit_rows / 2, 0, std::max(0, n - fit_rows));
  const float max_item_w = st.career ? g.L(265) : (has_right ? g.L(495) : (g.w - g.L(300)));
  const float left_shift = st.career ? -62.0f * s : 0.0f;
  Turn list_turn(l, g.L(226) + left_shift, g.T(185), ui4::kTiltRad);
  if (first > 0) {
    l.text(g.L(188) + left_shift, g.T(first_bottom - 52.0f), "^ MORE", ui4::style(ui::Font::kB, 11 * s, ui4::kDim));
  }
  for (int i = first; i < n && i < first + fit_rows; ++i) {
    const bool sel = i == st.menu_selected;
    const float extra = (i > st.menu_selected ? 6.0f : 0.0f);
    const float bottom = g.T(first_bottom + row_step * float(i - first) + extra);
    const std::string& item = st.menu[size_t(i)];
    const float base_sz = pause ? (sel ? 40.0f : 35.0f) : (sel ? 29.0f : 24.0f);
    TextStyle it = font(ui::Font::kE, base_sz * s, kText);
    fit(item, it, max_item_w, 13 * s);
    const float tw = ui::List::measure(item, it);
    if (sel) {
      ui4::brush(l, g.L(147) + left_shift, bottom - it.size - (pause ? 15 : 11) * s,
                 std::max(g.L(362) + left_shift, g.L(185) + left_shift + tw + 36 * s), bottom + 8 * s, ui4::kPink,
                 true);
      l.text(g.L(185) + left_shift, bottom - it.size, item, it);
    } else {
      ui4::brush(l, g.L(158) + left_shift, bottom - it.size - 6 * s,
                 std::max(g.L(270) + left_shift, g.L(188) + left_shift + tw + 26 * s), bottom + 5 * s, ui4::kBrushBlack,
                 false);
      l.text(g.L(188) + left_shift, bottom - it.size, item, it);
    }
  }
  if (first + fit_rows < n) {
    const float end_y = g.T(first_bottom + row_step * float(fit_rows) + 4.0f);
    l.text(g.L(188), end_y, "v MORE", ui4::style(ui::Font::kB, 11 * s, ui4::kDim));
  }
  l.unturn();

  // Optional right-hand detail card (Regional Outpost Hub / Events / Race Central) —
  // authentic FH1 sharp-edged distressed panel + tilted brush title header.
  if (!st.detail_lines.empty() && !st.career) {
    const float x0 = g.R(740), x1 = g.R(1214), y0 = g.T(152);
    const float card_h = std::max(210 * s, (88.0f + 29.0f * float(st.detail_lines.size())) * s);
    ui4::panel(l, x0, y0, x1, y0 + card_h, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
    if (!st.detail_title.empty()) {
      Turn dt_turn(l, 0.5f * (x0 + x1), y0 + 24 * s, ui4::kTiltRad * 0.6f);
      TextStyle dt = font(ui::Font::kE, 22 * s, ui4::kWhite);
      fit(st.detail_title, dt, x1 - x0 - 44 * s, 12 * s);
      const float dw = ui::List::measure(st.detail_title, dt);
      ui4::brush(l, x0 - 10 * s, y0 + 10 * s, std::min(x1 + 8 * s, x0 + dw + 40 * s), y0 + 48 * s, ui4::kPink);
      l.text(x0 + 18 * s, y0 + 18 * s, st.detail_title, dt);
    }
    float dy = y0 + 66 * s;
    for (size_t li = 0; li < st.detail_lines.size(); ++li) {
      const auto& line = st.detail_lines[li];
      uint32_t col = ui4::kWhite;
      bool highlight_row = false;
      if (line.rfind("INELIGIBLE", 0) == 0 || line.rfind("LOCKED", 0) == 0) col = rgba(255, 110, 95);
      else if (line.rfind("READY", 0) == 0 || line.rfind("1ST PLACE", 0) == 0 || line.rfind("BONUS CAR", 0) == 0 ||
               line.rfind("DISCOVERED", 0) == 0 || line.rfind("COMPLETED", 0) == 0) {
        col = ui4::kYellow;
        highlight_row = true;
      }
      if (highlight_row) {
        l.rect(x0 + 12 * s, dy - 3 * s, x1 - 12 * s, dy + 22 * s, ui::rgba(245, 4, 92, 46));
      } else if (li % 2 == 1) {
        l.rect(x0 + 12 * s, dy - 3 * s, x1 - 12 * s, dy + 22 * s, ui::rgba(255, 255, 255, 10));
      }
      TextStyle ls = font(ui::Font::kB, 14.5f * s, col);
      ls.keep_case = true;
      fit(line, ls, x1 - x0 - 40 * s, 9 * s);
      l.text(x0 + 20 * s, dy + 1 * s, line, ls);
      dy += 28 * s;
    }
  }
  // Authentic BUTTON_BAR_SCENE bottom-left help strip.
  ui4::button_bar(
      l,
      {{"A", str("HelpButtons", "IDS_Select", "SELECT")},
       {"B", pause ? str("HelpButtons", "IDS_Resume", "RESUME") : str("HelpButtons", "IDS_Back", "BACK")}},
      g.w, g.h);
  if (pause || st.credits >= 0) draw_career_header(l, st, g);
}

// (e) Authentic Forza Horizon Title Screen (120_P_PRE_SPLASH parity).
void title_screen_hud(ui::List& l, const TitleScreenHud& ts, const Layout& g, double time) {
  const float s = g.s;
  // Subtle cinematic gradient over live 3D Colorado & car.
  l.rect(0, 0, g.w, g.h, rgba(8, 10, 14, ts.menu_phase ? 128 : 96), 0, rgba(6, 8, 12, ts.menu_phase ? 170 : 165));

  std::array<float, 4> logo_uv;
  const bool have_logo = ui_texture("E3/E3LARGELOGO", logo_uv) || ui_texture("E3/E3LargeLogo", logo_uv);

  if (!ts.menu_phase) {
    // Splash phase: big iconic Forza Horizon logo centred, pulsing PRESS START pill below.
    if (have_logo) {
      const float lw = 640 * s, lh = 192 * s;
      l.image(0.5f * g.w - 0.5f * lw, g.T(165), 0.5f * g.w + 0.5f * lw, g.T(165) + lh, logo_uv);
    } else {
      Turn t(l, 0.5f * g.w, g.T(260));
      brush(l, g.C(410), g.T(190), g.C(870), g.T(250), kBrush);
      l.text(0.5f * g.w, g.T(198), "FORZA", font(ui::Font::kE, 46 * s, kText, Align::kCenter));
      brush(l, g.C(370), g.T(246), g.C(910), g.T(324), kPink);
      l.text(0.5f * g.w, g.T(256), "HORIZON", font(ui::Font::kE, 62 * s, kText, Align::kCenter));
    }
    // Pulsing Press Start pill (120_P_PRE_SPLASH START_BUTTON).
    const float pulse = 0.72f + 0.28f * float(std::cos(time * 3.4));
    const std::string prompt = "PRESS ENTER OR [A] TO START";
    TextStyle ps = font(ui::Font::kB, 17 * s, ui::fade(kText, pulse), Align::kCenter);
    const float pw = ui::List::measure(prompt, ps) + 84 * s;
    const float px0 = 0.5f * g.w - 0.5f * pw, px1 = 0.5f * g.w + 0.5f * pw;
    const float py0 = g.B(545), py1 = g.B(585);
    pill(l, px0, py0, px1, py1, kPillGrey, false, "DIRTMASKS/PILL01", 512);
    std::array<float, 4> abtn;
    const float cy = 0.5f * (py0 + py1);
    if (ui_texture("CONTROLLER/A_BUTTON", abtn))
      l.image(px0 + 16 * s, cy - 11 * s, px0 + 38 * s, cy + 11 * s, abtn, ui::fade(rgba(255, 255, 255), pulse));
    else
      l.circle(px0 + 27 * s, cy, 9.5f * s, ui::fade(kGreen, pulse));
    l.text(0.5f * g.w + 10 * s, cy - 8.5f * s, prompt, ps);
    l.text(g.R(1210), g.B(672), "FORZA HORIZON  //  COLORADO FESTIVAL",
           font(ui::Font::kB, 12 * s, rgba(220, 224, 232, 180), Align::kRight));
    return;
  }

  // Menu phase: logo top-right, tilted brush menu down the left, career save card right.
  if (have_logo) {
    l.image(g.R(810), g.T(32), g.R(1210), g.T(152), logo_uv);
  } else {
    Turn t(l, g.R(1010), g.T(84));
    brush(l, g.R(840), g.T(46), g.R(1200), g.T(86), kBrush);
    l.text(g.R(1020), g.T(52), "FORZA", font(ui::Font::kE, 26 * s, kText, Align::kCenter));
    brush(l, g.R(820), g.T(82), g.R(1210), g.T(132), kPink);
    l.text(g.R(1015), g.T(90), "HORIZON", font(ui::Font::kE, 36 * s, kText, Align::kCenter));
  }

  // Left tilted brush menu (120_P_PRE_SPLASH SPLASH_MENU).
  Turn mt(l, g.L(226), g.T(195));
  for (size_t i = 0; i < ts.menu.size(); ++i) {
    const bool sel = !ts.confirm_new_career && int(i) == ts.selected;
    const float extra = (!ts.confirm_new_career && int(i) >= ts.selected && ts.selected > 0 ? 10.0f : 0.0f);
    const float bottom = g.T(224 + 54.0f * float(i) + extra);
    const std::string& item = ts.menu[i];
    TextStyle it = font(ui::Font::kE, (sel ? 40 : 34) * s, kText);
    fit(item, it, g.L(520), 16 * s);
    const float tw = ui::List::measure(item, it);
    if (sel) {
      brush(l, g.L(145), bottom - it.size - 14 * s, std::max(g.L(380), g.L(184) + tw + 38 * s), bottom + 8 * s, kPink);
      l.text(g.L(184), bottom - it.size, item, it);
    } else {
      brush(l, g.L(160), bottom - it.size - 5 * s, g.L(188) + tw + 24 * s, bottom + 5 * s, kBrush);
      l.text(g.L(188), bottom - it.size, item, it);
    }
  }
  l.unturn();

  // Right-side Career Save preview card (or New Career confirmation modal) —
  // authentic FH1 sharp distressed panel + tilted brush headers, zero rounded boxes.
  if (ts.confirm_new_career) {
    const float x0 = g.R(710), x1 = g.R(1214), y0 = g.T(192), y1 = g.T(446);
    ui4::panel(l, x0, y0, x1, y1, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
    {
      Turn ct(l, 0.5f * (x0 + x1), y0 + 26 * s, ui4::kTiltRad * 0.6f);
      ui4::brush(l, x0 - 10 * s, y0 + 12 * s, x0 + 360 * s, y0 + 54 * s, ui4::kPink);
      l.text(x0 + 18 * s, y0 + 20 * s, "START NEW CAREER?", font(ui::Font::kE, 26 * s, kText));
    }
    l.text(x0 + 22 * s, y0 + 72 * s, "THIS WILL OVERWRITE YOUR EXISTING CAREER SAVE",
           font(ui::Font::kB, 14 * s, kDim));
    l.text(x0 + 22 * s, y0 + 94 * s, "AND START FRESH IN THE 1995 VW CORRADO VR6.",
           font(ui::Font::kB, 14 * s, kDim));
    const char* opts[2] = {"CONFIRM NEW CAREER", "CANCEL"};
    for (int i = 0; i < 2; ++i) {
      const bool sel = ts.confirm_selected == i;
      const float by = y0 + 136 * s + float(i) * 48 * s;
      ui4::brush(l, x0 + 22 * s, by, x1 - 22 * s, by + 38 * s, sel ? ui4::kPink : ui4::kBrushBlack);
      l.text(x0 + 40 * s, by + 8 * s, opts[i], font(ui::Font::kE, 22 * s, kText));
    }
  } else if (ts.has_save) {
    const float x0 = g.R(740), x1 = g.R(1214), y0 = g.T(178), y1 = g.T(462);
    const uint32_t bc = ui4::wristband_colour(ts.save_band_name);
    ui4::panel(l, x0, y0, x1, y1, s, ui4::kCharcoalDark, bc, 0, true);
    {
      Turn stt(l, x0 + 120 * s, y0 + 20 * s, ui4::kTiltRad * 0.5f);
      ui4::brush(l, x0 - 8 * s, y0 + 10 * s, x0 + 190 * s, y0 + 36 * s, ui4::kBrushBlack);
      l.text(x0 + 14 * s, y0 + 16 * s, "SAVED CAREER", font(ui::Font::kE, 14 * s, ui4::kWhite));
    }
    std::string cw = ts.save_band_name.substr(0, ts.save_band_name.find(' '));
    for (char& ch : cw) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    std::array<float, 4> wb_uv;
    const bool have_wb = !cw.empty() && ui_texture("WRISTBANDS/" + cw, wb_uv);
    if (have_wb) l.image(x1 - 168 * s, y0 + 16 * s, x1 - 14 * s, y0 + 58 * s, wb_uv);
    const std::string wb_label = ts.save_band_name.empty() ? std::string("YELLOW WRISTBAND") : ts.save_band_name;
    TextStyle wbs = font(ui::Font::kE, 24 * s, bc);
    fit(wb_label, wbs, (x1 - x0) - (have_wb ? 202 * s : 44 * s), 13 * s);
    l.text(x0 + 22 * s, y0 + 48 * s + (24 * s - wbs.size) * 0.5f, wb_label, wbs);

    char line1[96], line2[96];
    std::snprintf(line1, sizeof line1, "%s PTS   •   POPULARITY %d%s", thousands(ts.save_band_points).c_str(),
                  ts.save_pop_rank, place_suffix(ts.save_pop_rank));
    l.text(x0 + 22 * s, y0 + 84 * s, line1, font(ui::Font::kB, 14 * s, ui4::kYellow));

    l.text(x0 + 22 * s, y0 + 118 * s, "CURRENT CAR", font(ui::Font::kB, 11 * s, kDim));
    TextStyle cn = font(ui::Font::kB, 18 * s, kText);
    cn.keep_case = true;
    fit(ts.save_car_name, cn, x1 - x0 - 134 * s, 10 * s);
    l.text(x0 + 22 * s, y0 + 138 * s, ts.save_car_name, cn);
    if (!ts.save_car_class.empty()) {
      ui4::class_badge(l, x1 - 102 * s, y0 + 134 * s, 23 * s, ts.save_car_class, ts.save_car_pi);
    }
    l.rect(x0 + 22 * s, y0 + 178 * s, x1 - 22 * s, y0 + 179 * s, rgba(255, 255, 255, 35));
    l.text(x0 + 22 * s, y0 + 194 * s, "CREDITS", font(ui::Font::kB, 11 * s, kDim));
    ui4::offwhite_strip(l, x0 + 20 * s, y0 + 214 * s, x0 + 228 * s, y0 + 248 * s, s, true);
    TextStyle cr = font(ui::Font::kE, 21 * s, ui4::kInk);
    cr.keep_case = true;
    l.text(x0 + 32 * s, y0 + 220 * s, thousands(ts.save_credits) + " Cr", cr);
    std::snprintf(line2, sizeof line2, "RACES %d   •   WINS %d", ts.save_races, ts.save_wins);
    l.text(x1 - 22 * s, y0 + 224 * s, line2, font(ui::Font::kB, 14 * s, kDim, Align::kRight));
  }

  ui4::button_bar(
      l, {{"A", str("HelpButtons", "IDS_Select", "SELECT")}, {"B", str("HelpButtons", "IDS_Back", "BACK")}}, g.w, g.h);
}

// (f) Free-roam world-space projected event / Festival trigger marker (hud-freeroam-event-marker.png).
void world_marker_hud(ui::List& l, const WorldMarkerHud& wm, const Layout& g) {
  if (!wm.visible) return;
  const float s = g.s;
  const float mx = wm.in_range ? std::clamp(wm.screen_x, g.L(160), g.w - g.L(260)) : wm.screen_x;
  const float my = wm.in_range ? std::clamp(wm.screen_y, g.T(120), g.h - g.T(185)) : wm.screen_y;
  if (mx < -100 || mx > g.w + 100 || my < -80 || my > g.h + 80) return;

  // Circular event/festival disc on the left.
  const float r = (wm.in_range ? 22.0f : 17.0f) * s;
  const uint32_t ring_col = wm.is_festival ? rgba(255, 214, 40) : (wm.eligible ? kPink : rgba(180, 180, 190));
  l.circle(mx, my, r + 3 * s, rgba(0, 0, 0, 180));
  l.circle(mx, my, r, ring_col);
  l.circle(mx, my, r - 4 * s, rgba(22, 24, 30, 235));
  l.text(mx, my - (wm.in_range ? 9.5f : 7.5f) * s, wm.is_festival ? "H" : "!",
         font(ui::Font::kE, (wm.in_range ? 19 : 15) * s, ring_col, Align::kCenter));

  // Tilted title + subtitle + ENTER NOW [X] prompt.
  const float tx = mx + r + 12 * s;
  Turn t(l, tx + 90 * s, my);
  if (!wm.badge.empty()) {
    const uint32_t bc = wm.badge == "NEW" ? kPink : wm.badge == "LOCKED" ? rgba(120, 120, 130) : rgba(255, 196, 40);
    const float bw = ui::List::measure(wm.badge, font(ui::Font::kB, 11 * s, kText)) + 14 * s;
    l.rect(tx, my - 30 * s, tx + bw, my - 13 * s, bc);
    l.text(tx + 7 * s, my - 27 * s, wm.badge, font(ui::Font::kB, 11 * s, kText));
  }
  TextStyle ts = font(ui::Font::kE, (wm.in_range ? 26 : 20) * s, kText);
  fit(wm.title, ts, g.L(360), 13 * s);
  shadowed(l, tx, my - 10 * s, wm.title, ts, s);

  if (!wm.subtitle.empty()) {
    TextStyle ss = font(ui::Font::kB, 12 * s, kText);
    fit(wm.subtitle, ss, g.L(340), 9 * s);
    const float sw = ui::List::measure(wm.subtitle, ss);
    l.rect(tx - 4 * s, my + 16 * s, tx + sw + 10 * s, my + 34 * s, rgba(0, 0, 0, 175));
    l.text(tx + 2 * s, my + 19 * s, wm.subtitle, ss);
  }

  if (wm.in_range) {
    const std::string ptxt = wm.prompt.empty() ? str("Activities", "IDS_EnterNow", "ENTER NOW") : wm.prompt;
    TextStyle ps = font(ui::Font::kE, 17 * s, kText);
    const float pw = ui::List::measure(ptxt, ps);
    const float py0 = my + 38 * s, py1 = my + 66 * s;
    brush(l, tx - 4 * s, py0, tx + pw + 44 * s, py1, wm.eligible ? kPink : rgba(110, 110, 118, 230));
    l.text(tx + 6 * s, py0 + 5 * s, ptxt, ps);
    const float bx = tx + pw + 24 * s, by = 0.5f * (py0 + py1);
    std::array<float, 4> xuv;
    if (ui_texture("CONTROLLER/X_BUTTON", xuv))
      l.image(bx - 10 * s, by - 10 * s, bx + 10 * s, by + 10 * s, xuv);
    else {
      l.circle(bx, by, 9 * s, kBlue);
      l.text(bx, by - 5 * s, "X", font(ui::Font::kB, 10 * s, kText, Align::kCenter));
    }
  } else if (wm.distance_m > 0) {
    char db[24];
    std::snprintf(db, sizeof db, "%.0f M", wm.distance_m);
    shadowed(l, tx, my + 38 * s, db, font(ui::Font::kB, 12 * s, rgba(220, 224, 232)), s);
  }
}

// (g) Top-centre Skill Chain & Popularity Rank Disc (race-hud.png, race-hud-skill-banked.png).
void skill_chain_hud(ui::List& l, const SkillChainHud& sk, const Layout& g, double time) {
  const double since_bank = time - sk.banked_at;
  const double since_lost = time - sk.lost_at;
  const double since_rank = time - sk.rank_up_at;
  const bool show_bank = since_bank >= 0 && since_bank < 3.5;
  const bool show_lost = since_lost >= 0 && since_lost < 2.2;
  const bool show_rank = since_rank >= 0 && since_rank < 4.0;
  if (!sk.active && !show_bank && !show_lost && !show_rank) return;

  const float s = g.s;
  const float cx = g.C(640), cy = g.T(58);
  const float r = 31 * s;
  constexpr uint32_t kSkillYellow = rgba(0xF7, 0xFF, 0x26);

  // Popularity rank disc + radial progress arc.
  std::array<float, 4> pbg;
  if (ui_texture("HUD/POPULARITYBAR/POPULARITYBARBACKGROUND", pbg))
    l.image(cx - 38 * s, cy - 38 * s, cx + 38 * s, cy + 38 * s, pbg);
  else
    l.circle(cx, cy, r, rgba(16, 18, 22, 220));
  l.circle(cx, cy, r - 4 * s, rgba(14, 16, 20, 210));
  l.arc(cx, cy, r - 2 * s, 4 * s, -3.14159f, 3.14159f, rgba(255, 255, 255, 45), true);
  const float sweep = float(std::clamp(sk.popularity_progress, 0.02, 1.0)) * 6.28318f;
  l.arc(cx, cy, r - 2 * s, 4.5f * s, -1.5708f, -1.5708f + sweep, show_rank ? kPink : kSkillYellow, true);

  char rbuf[16];
  std::snprintf(rbuf, sizeof rbuf, "%d", sk.popularity_rank);
  const float rsz = sk.popularity_rank >= 100 ? 15.5f : 19.0f;
  TextStyle rts = font(ui::Font::kE, rsz * s, kText, Align::kCenter);
  shadowed(l, cx, cy - 0.65f * rts.size, rbuf, rts, s);
  l.text(cx, cy + 7.5f * s, place_suffix(sk.popularity_rank),
         font(ui::Font::kB, 8.5f * s, kSkillYellow, Align::kCenter));

  // Skill chain score & label beneath disc.
  if (sk.active) {
    char score[48];
    std::snprintf(score, sizeof score, "%s  x%d.0", thousands(sk.chain_points).c_str(), sk.multiplier);
    Turn t(l, cx, g.T(106));
    shadowed(l, cx, g.T(95), score, font(ui::Font::kE, 27 * s, kSkillYellow, Align::kCenter), s);
    if (!sk.last_skill.empty()) {
      char sl[96];
      std::snprintf(sl, sizeof sl, "%s  +%d", sk.last_skill.c_str(), sk.last_points);
      TextStyle ls = font(ui::Font::kB, 13 * s, kText, Align::kCenter);
      const float lw = ui::List::measure(sl, ls) + 22 * s;
      l.rect(cx - 0.5f * lw, g.T(124), cx + 0.5f * lw, g.T(144), rgba(0, 0, 0, 175));
      l.text(cx, g.T(128), sl, ls);
    }
  } else if (show_rank) {
    const float a = 1.0f - clamp01((since_rank - 3.0) / 1.0);
    Turn t(l, cx, g.T(108));
    char rl[64];
    std::snprintf(rl, sizeof rl, "POPULARITY RANK %d%s!", sk.popularity_rank, place_suffix(sk.popularity_rank));
    const TextStyle ts = font(ui::Font::kE, 22 * s, ui::fade(kText, a), Align::kCenter);
    const float w = ui::List::measure(rl, ts) + 34 * s;
    brush(l, cx - 0.5f * w, g.T(93), cx + 0.5f * w, g.T(126), ui::fade(kPink, a));
    l.text(cx, g.T(99), rl, ts);
  } else if (show_bank) {
    const float a = 1.0f - clamp01((since_bank - 2.6) / 0.9);
    Turn t(l, cx, g.T(106));
    const std::string bl = "+" + thousands(sk.banked_total) + " BANKED";
    shadowed(l, cx, g.T(95), bl, font(ui::Font::kE, 24 * s, ui::fade(kSkillYellow, a), Align::kCenter), s);
  } else if (show_lost) {
    const float a = 1.0f - clamp01((since_lost - 1.5) / 0.7);
    Turn t(l, cx, g.T(106));
    shadowed(l, cx, g.T(95), "SKILL CHAIN LOST", font(ui::Font::kE, 22 * s, ui::fade(kRed, a), Align::kCenter), s);
  }
}

// (h) Opponent overhead name tags in races (race-hud.png).
void opponent_name_tags(ui::List& l, const std::vector<NameTagHud>& tags, const Layout& g) {
  const float s = g.s;
  for (const auto& t : tags) {
    if (t.screen_x < 40 * s || t.screen_x > g.w - 40 * s || t.screen_y < 30 * s || t.screen_y > g.h - 120 * s)
      continue;
    const std::string text = (t.leader ? "* " : "") + t.name;
    TextStyle ts = font(ui::Font::kB, 13 * s, t.leader ? rgba(255, 224, 96) : rgba(244, 246, 250, 235), Align::kCenter);
    shadowed(l, t.screen_x, t.screen_y, text, ts, s);
  }
}

// (i) Wristband Collection Ceremony overlay at Race Central.
void ceremony_overlay(ui::List& l, const CeremonyHud& c, const Layout& g) {
  const float s = g.s;
  l.rect(0, 0, g.w, g.h, rgba(0, 0, 0, 155));
  const uint32_t bc = band_colour(c.band_colour);
  {
    Turn t(l, g.L(340), g.T(185));
    brush(l, g.L(95), g.T(150), g.L(610), g.T(218), kBrush);
    TextStyle hs = font(ui::Font::kE, 44 * s, kText);
    fit(c.headline, hs, g.L(490), 20 * s);
    l.text(g.L(115), g.T(160), c.headline, hs);
  }
  std::array<float, 4> uv;
  if (!c.band_colour.empty() && ui_texture("WRISTBANDS/" + c.band_colour + "XL", uv)) {
    l.image_rotated(g.L(300), g.T(292), 255 * s, 99 * s, -0.06f, uv);
  }
  {
    Turn t(l, g.L(410), g.T(392));
    brush(l, g.L(88), g.T(354), g.L(740), g.T(432), bc);
    TextStyle bs = font(ui::Font::kE, 48 * s, kInk);
    fit(c.band_name, bs, g.L(620), 20 * s);
    l.text(g.L(112), g.T(368), c.band_name, bs);
  }
  if (!c.subtitle.empty()) {
    Turn t(l, g.L(340), g.T(465));
    l.rect(g.L(105), g.T(445), g.L(640), g.T(486), kLight);
    l.text(g.L(122), g.T(456), c.subtitle, font(ui::Font::kB, 17 * s, kDark));
  }
  if (!c.bonus_car.empty()) {
    Turn t(l, g.L(340), g.T(518));
    brush(l, g.L(105), g.T(495), g.L(660), g.T(542), kPink);
    const std::string bc_txt = "BONUS CAR ADDED TO GARAGE: " + c.bonus_car;
    TextStyle cs = font(ui::Font::kB, 16 * s, kText);
    cs.keep_case = true;
    fit(bc_txt, cs, g.L(530), 10 * s);
    l.text(g.L(122), g.T(509), bc_txt, cs);
  }
  help_item(l, g, 70, 196, 653, 689, "A", kGreen, str("HelpButtons", "IDS_ContinueMenu", "CONTINUE"));
}

// (j) Interactive Pause Map HUD overlay (H_MAP UI4 parity: tilted destination
// card with brush title, LB/RB filter tab pills, Roads Discovered counter,
// Treasure Map tag, and authentic BUTTON_BAR_SCENE + road shading legend).
void map_overlay_hud(ui::List& l, const MapOverlayHud& mo, const Layout& g) {
  const float s = g.s;
  (void)ui_scene("H_MAP");

  // Top-left authentic FH1 destination card (tilted pink brush title + gritty info strip).
  if (!mo.title.empty()) {
    const float x0 = g.L(48), y0 = g.T(40), x1 = g.L(472), y1 = g.T(146);
    ui4::panel(l, x0, y0 + 22 * s, x1, y1, s, ui4::kCharcoalDark, 0, ui4::kPink, true);
    {
      Turn tt(l, 0.5f * (x0 + x1), y0 + 22 * s, ui4::kTiltRad * 0.7f);
      TextStyle ts = font(ui::Font::kE, 26 * s, ui4::kWhite);
      fit(mo.title, ts, x1 - x0 - 34 * s, 13 * s);
      const float tw = ui::List::measure(mo.title, ts);
      ui4::brush(l, x0 - 12 * s, y0, std::min(x1 + 12 * s, x0 + std::max(240 * s, tw + 46 * s)), y0 + 44 * s,
                 ui4::kPink);
      l.text(x0 + 16 * s, y0 + 9 * s, mo.title, ts);
    }
    if (!mo.subtitle.empty()) {
      TextStyle ss = font(ui::Font::kB, 13.5f * s, ui4::kWhite);
      fit(mo.subtitle, ss, x1 - x0 - 32 * s, 9 * s);
      l.text(x0 + 18 * s, y0 + 56 * s, mo.subtitle, ss);
    }
    if (mo.distance_m >= 0) {
      char db[48];
      if (mo.distance_m >= 1000.0) std::snprintf(db, sizeof db, "%.1f KM AWAY", mo.distance_m / 1000.0);
      else std::snprintf(db, sizeof db, "%.0f M AWAY", mo.distance_m);
      ui4::pill(l, x0 + 16 * s, y0 + 80 * s, x0 + 158 * s, y0 + 102 * s, ui4::kPillGrey, ui4::PillKind::kMapSmall);
      l.text(x0 + 26 * s, y0 + 85 * s, db, font(ui::Font::kB, 12.5f * s, ui4::kYellow));
    }
    if (mo.gps_active) {
      ui4::brush(l, x1 - 128 * s, y0 + 78 * s, x1 - 14 * s, y0 + 102 * s, ui4::kGreen);
      l.text(x1 - 71 * s, y0 + 84 * s, "GPS ACTIVE", font(ui::Font::kE, 12 * s, ui4::kInk, Align::kCenter));
    }
  }

  // Top-right category filter tab strip (LB/RB bumpers + Horizon pills).
  if (!mo.filter_tabs.empty()) {
    ui4::tab_strip(l, s, g.R(540), g.T(40), g.R(1232), mo.filter_tabs, mo.active_filter, true, ui4::kPink);

    // Roads Discovered distressed status strip right under the tabs.
    const float sy0 = g.T(82), sy1 = g.T(114), sx0 = g.R(908), sx1 = g.R(1232);
    ui4::panel(l, sx0, sy0, sx1, sy1, s, ui4::kCharcoalDark, 0, ui4::kGreen, true);
    char rb[96];
    std::snprintf(rb, sizeof rb, "ROADS DISCOVERED   %d / %d", mo.roads_discovered, mo.roads_total);
    l.text(sx0 + 16 * s, sy0 + 8 * s, rb, font(ui::Font::kB, 13 * s, ui4::kWhite));
    if (mo.zoom >= 2.0) {
      ui4::brush(l, sx1 - 46 * s, sy0 + 5 * s, sx1 - 8 * s, sy1 - 5 * s, ui4::kGreen);
      l.text(sx1 - 27 * s, sy0 + 9 * s, "HD", font(ui::Font::kE, 12 * s, ui4::kInk, Align::kCenter));
    }
    // Thin progress bar along bottom edge of Roads Discovered card.
    const float rfrac = mo.roads_total > 0 ? float(std::clamp(double(mo.roads_discovered) / mo.roads_total, 0.0, 1.0)) : 0.0f;
    l.rect(sx0 + 5 * s, sy1 - 4 * s, sx1, sy1, rgba(255, 255, 255, 28));
    if (rfrac > 0.0f) l.rect(sx0 + 5 * s, sy1 - 4 * s, sx0 + 5 * s + (sx1 - sx0 - 5 * s) * rfrac, sy1, ui4::kGreen);

    if (mo.treasure_map) {
      const float ty0 = g.T(122), ty1 = g.T(148);
      ui4::brush(l, sx1 - 186 * s, ty0, sx1, ty1, ui4::kAmber);
      l.text(sx1 - 93 * s, ty0 + 6 * s, "TREASURE MAP ON", font(ui::Font::kE, 12 * s, ui4::kInk, Align::kCenter));
    }
  }

  // Authentic BUTTON_BAR_SCENE bottom help strip + road discovery legend pill.
  ui4::button_bar(
      l,
      {{"A", mo.action_hint.empty() ? "FAST TRAVEL" : mo.action_hint},
       {"X", mo.gps_hint.empty() ? "SET GPS ROUTE" : mo.gps_hint},
       {"LB", "FILTER"},
       {"LT", "ZOOM"},
       {"B", str("HelpButtons", "IDS_Back", "BACK")}},
      g.w, g.h, 52.0f);

  // Road shading legend pill bottom-right (matches BUTTON_BAR vertical anchor).
  const float ly0 = g.B(654), ly1 = g.B(682);
  ui4::pill(l, g.R(984), ly0, g.R(1232), ly1, ui4::kPillGrey, ui4::PillKind::kMenu);
  l.rect(g.R(1002), ly0 + 11 * s, g.R(1024), ly0 + 17 * s, rgba(235, 238, 244));
  l.text(g.R(1030), ly0 + 8 * s, "DRIVEN", font(ui::Font::kB, 11.5f * s, ui4::kWhite));
  l.rect(g.R(1106), ly0 + 11 * s, g.R(1128), ly0 + 17 * s, rgba(98, 102, 110));
  l.text(g.R(1134), ly0 + 8 * s, "UNDRIVEN", font(ui::Font::kB, 11.5f * s, ui4::kDim));
}

// (j2) Free-roam GPS turn-by-turn guidance pill above the minimap.
void gps_turn_hud(ui::List& l, const GpsHud& gps, const Layout& g) {
  if (!gps.active) return;
  const float s = g.s;
  const float x0 = g.L(64), y0 = g.B(344), x1 = g.L(316), y1 = g.B(406);
  ui4::panel(l, x0, y0, x1, y1, s, ui4::kCharcoalDark, 0, ui4::kGreen, true);

  // Crisp direction badge on the left.
  const float bx0 = x0 + 12 * s, by0 = y0 + 10 * s, bx1 = x0 + 52 * s, by1 = y1 - 10 * s;
  l.rect(bx0, by0, bx1, by1, ui::rgba(70, 220, 110, 44));
  l.rect(bx0, by0, bx0 + 3 * s, by1, ui4::kGreen);
  const char* glyph = "^";
  switch (gps.turn_kind) {
    case 2: glyph = "\\"; break;   // bear left
    case 3: glyph = "<"; break;    // left
    case 4: glyph = "<<"; break;   // sharp left
    case 5: glyph = "/"; break;    // bear right
    case 6: glyph = ">"; break;    // right
    case 7: glyph = ">>"; break;   // sharp right
    case 8: glyph = "U"; break;    // u-turn
    case 9: glyph = "*"; break;    // arrive
    default: glyph = "^"; break;   // straight
  }
  l.text(0.5f * (bx0 + bx1), by0 + 7 * s, glyph, font(ui::Font::kE, 25 * s, ui4::kGreen, Align::kCenter));

  auto fmt_dist = [](double m, char* buf, size_t n) {
    if (m >= 1000.0) std::snprintf(buf, n, "%.1f KM", m / 1000.0);
    else std::snprintf(buf, n, "%d M", std::max(0, int(std::lround(m / 10.0) * 10)));
  };
  char dt_buf[32], dd_buf[32];
  fmt_dist(gps.dist_to_turn_m, dt_buf, sizeof dt_buf);
  fmt_dist(gps.dist_to_dest_m, dd_buf, sizeof dd_buf);

  const float tx = bx1 + 10 * s;
  const float max_w = x1 - tx - 10 * s;
  std::string line1 = gps.turn_text.empty() ? std::string("FOLLOW ROUTE") : gps.turn_text;
  TextStyle ts = font(ui::Font::kE, 17 * s, ui4::kWhite);
  fit(line1, ts, max_w, 10 * s);
  l.text(tx, y0 + 9 * s, line1, ts);

  std::string line2 = std::string(dt_buf) + "  •  DEST " + dd_buf;
  TextStyle ds = font(ui::Font::kB, 11.5f * s, ui4::kGreen);
  fit(line2, ds, max_w, 8 * s);
  l.text(tx, y0 + 30 * s, line2, ds);

  if (!gps.dest_label.empty()) {
    TextStyle ls = font(ui::Font::kB, 9.5f * s, ui4::kDim);
    fit(gps.dest_label, ls, max_w, 7.5f * s);
    l.text(tx, y0 + 45 * s, gps.dest_label, ls);
  }
}

// (j3) Active Outpost PR Stunt HUD (anchored top-left beneath Driver Level pill
// so top-centre remains exclusively clear for the Popularity Rank & Skill Chain).
void pr_stunt_hud(ui::List& l, const PrStuntHud& ps, const Layout& g) {
  if (!ps.active) return;
  const float s = g.s;
  const float cw = 430 * s;
  const float x0 = g.L(64), x1 = x0 + cw;
  const float y0 = g.T(68), y1 = y0 + 114 * s;
  const uint32_t kStuntAccent = ps.in_zone ? ui4::kGreen : ui4::kPink;
  ui4::panel(l, x0, y0 + 14 * s, x1, y1, s, ui4::kCharcoalDark, kStuntAccent, 0, true);

  // Tilted FH1 brush badge (top-left) + countdown clock (top-right).
  const std::string badge = ps.kind_badge.empty() ? std::string("PR STUNT") : ps.kind_badge;
  TextStyle bs = font(ui::Font::kE, 13 * s, ui4::kWhite);
  const float bw = ui::List::measure(badge, bs) + 24 * s;
  {
    Turn bt(l, x0 + 0.5f * bw, y0 + 16 * s, ui4::kTiltRad * 0.6f);
    ui4::brush(l, x0 - 8 * s, y0 + 2 * s, x0 + bw + 8 * s, y0 + 30 * s, kStuntAccent);
    l.text(x0 + 10 * s, y0 + 9 * s, badge, bs);
  }

  float timer_w = 0.0f;
  if (ps.time_remaining_s > 0.0) {
    const bool urgent = ps.time_remaining_s < 15.0;
    const uint32_t tc = urgent ? ui4::kRed : ui4::kWhite;
    const std::string clk = race_clock(ps.time_remaining_s);
    const TextStyle cs = font(ui::Font::kE, 21 * s, tc, Align::kRight);
    timer_w = ui::List::measure(clk, cs) + 18 * s;
    l.text(x1 - 14 * s, y0 + 20 * s, clk, cs);
  }

  // Clean dedicated row for title (zero overlap with badge or timer).
  TextStyle ts = font(ui::Font::kE, 16 * s, ui4::kWhite);
  fit(ps.title, ts, std::max(80 * s, cw - timer_w - 28 * s), 10 * s);
  l.text(x0 + 14 * s, y0 + 35 * s, ps.title, ts);

  if (!ps.instruction.empty()) {
    TextStyle is = font(ui::Font::kB, 11 * s, ui4::kDim);
    fit(ps.instruction, is, cw - 28 * s, 8.5f * s);
    l.text(x0 + 14 * s, y0 + 56 * s, ps.instruction, is);
  }
  if (!ps.status_line.empty()) {
    TextStyle ss = font(ui::Font::kB, 12.5f * s, ps.in_zone ? ui4::kGreen : ui4::kYellow);
    fit(ps.status_line, ss, cw - 28 * s, 9 * s);
    l.text(x0 + 14 * s, y0 + 73 * s, ps.status_line, ss);
  }

  // Sharp FH1 progress bar.
  const float bx0 = x0 + 14 * s, bx1 = x1 - 14 * s, by0 = y0 + 92 * s, by1 = by0 + 7 * s;
  l.rect(bx0, by0, bx1, by1, rgba(255, 255, 255, 34));
  const float p = float(std::clamp(ps.progress, 0.0, 1.0));
  if (p > 0.0f) {
    l.rect(bx0, by0, std::max(bx0 + 6 * s, bx0 + (bx1 - bx0) * p), by1, kStuntAccent);
  }

  const char* hint = ps.is_photo ? "[P] SNAP PHOTO (IN ZONE)   •   [ESC] CANCEL" : "[ESC] CANCEL STUNT";
  l.text(x1 - 14 * s, y0 + 102 * s, hint, font(ui::Font::kB, 8.5f * s, ui4::kDim, Align::kRight));
}

// A tile: a big number and its label.
void stat_tile(ui::List& l, float x0, float y0, float x1, float y1, const std::string& value, const char* name,
               float s, uint32_t colour = kText) {
  panel(l, x0, y0, x1, y1, s, rgba(10, 12, 17, 190));
  TextStyle vs{.size = 26 * s, .weight = 0.12f, .slant = 0.1f, .color = colour};
  while (vs.size > 10 * s && ui::List::measure(value, vs) > x1 - x0 - 32 * s) vs.size *= 0.92f;
  l.text(x0 + 16 * s, y0 + 16 * s + (26 * s - vs.size), value, vs);
  label(l, x0 + 16 * s, y1 - 22 * s, name, s, kDim);
}

// A collection row: the name, "found / total" and a bar.
void collection_row(ui::List& l, float x0, float x1, float y, const char* name, int found, int total, float s,
                    uint32_t colour = kAccent) {
  l.text(x0, y, name, TextStyle{.size = 11 * s, .weight = 0.12f, .tracking = 0.1f, .color = kText});
  char v[48];
  if (total > 0) std::snprintf(v, sizeof v, "%d / %d", found, total);
  else std::snprintf(v, sizeof v, "%d", found);
  l.text(x1, y, v, TextStyle{.size = 11 * s, .weight = 0.11f, .color = kDim, .align = Align::kRight});
  const float by = y + 19 * s, bh = 6 * s;
  l.rect(x0, by, x1, by + bh, rgba(255, 255, 255, 30), 3 * s);
  if (total > 0 && found > 0) {
    const float k = float(std::clamp(double(found) / total, 0.0, 1.0));
    l.rect(x0, by, std::max(x0 + bh, x0 + (x1 - x0) * k), by + bh, found >= total ? kGreen : colour, 3 * s);
  }
}

void career_board(ui::List& l, const CareerBoard& c, float w, float h, float s) {
  const float x0 = std::min(w, 480 * s) + 26 * s, x1 = w - 48 * s;
  if (x1 - x0 < 300 * s) return;
  float y = 38 * s;

  // Authentic FH1 My Profile Wristband hero card (sharp gritty panel + WRISTBANDS/* artwork + off-white credits strip).
  const bool band = !c.band_name.empty();
  const uint32_t bc = band ? band_colour(c.band_name) : ui4::kDim;
  const float ch = 128 * s;
  ui4::panel(l, x0, y, x1, y + ch, s, ui4::kCharcoalDark, bc, 0, true);

  // Real WRISTBANDS/<COLOR> graphic if available.
  std::string wb_word = c.band_name.substr(0, c.band_name.find(' '));
  for (char& ch_w : wb_word) ch_w = char(std::toupper(static_cast<unsigned char>(ch_w)));
  std::array<float, 4> wb_uv{};
  const bool has_wb = !wb_word.empty() && ui4::texture("WRISTBANDS/" + wb_word, wb_uv);
  const float text_x = has_wb ? (x0 + 132 * s) : (x0 + 22 * s);
  if (has_wb) {
    l.image(x0 + 14 * s, y + 12 * s, x0 + 122 * s, y + 64 * s, wb_uv);
  }

  l.text(text_x, y + 14 * s, "HORIZON WRISTBAND", ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
  ui::TextStyle bts = ui4::style(ui::Font::kE, 27 * s, bc);
  const std::string btitle = band ? c.band_name : std::string("NO WRISTBAND YET");
  ui4::fit_text(btitle, bts, (x1 - 210 * s) - text_x, 14 * s);
  l.text(text_x, y + 32 * s, btitle, bts);

  // Off-white Credits strip top-right inside card.
  ui4::offwhite_strip(l, x1 - 196 * s, y + 16 * s, x1 - 18 * s, y + 48 * s, s, true);
  ui::TextStyle crs = ui4::style(ui::Font::kB, 17 * s, ui4::kInk, Align::kRight, true);
  const std::string crstr = ui4::format_credits(c.credits);
  ui4::fit_text(crstr, crs, 162 * s, 10 * s);
  l.text(x1 - 26 * s, y + 23 * s, crstr, crs);

  // Sharp FH1 wristband progress bar.
  const float bx0 = x0 + 20 * s, bx1 = x1 - 20 * s, by = y + 82 * s, bh = 10 * s;
  l.rect(bx0, by, bx1, by + bh, rgba(255, 255, 255, 30));
  const float k = c.band_hi > c.band_lo
                      ? float(std::clamp(double(c.points - c.band_lo) / double(c.band_hi - c.band_lo), 0.0, 1.0))
                      : 1.0f;
  if (band || c.points > 0) l.rect(bx0, by, std::max(bx0 + 6 * s, bx0 + (bx1 - bx0) * k), by + bh, bc);
  char pts[64];
  if (c.band_hi > 0)
    std::snprintf(pts, sizeof pts, "%s / %s POINTS", thousands(c.points).c_str(), thousands(c.band_hi).c_str());
  else
    std::snprintf(pts, sizeof pts, "%s POINTS", thousands(c.points).c_str());
  l.text(bx0, by + bh + 9 * s, pts, ui4::style(ui::Font::kB, 11.5f * s, ui4::kWhite));
  if (c.band_hi > 0 && !c.band_next.empty())
    l.text(bx1, by + bh + 9 * s, "NEXT: " + c.band_next + "  (" + thousands(c.band_hi - c.points) + " TO GO)",
           ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim, Align::kRight));
  else if (band)
    l.text(bx1, by + bh + 9 * s, "TOP WRISTBAND", ui4::style(ui::Font::kE, 12 * s, ui4::kYellow, Align::kRight));
  y += ch + 14 * s;

  // Authentic gritty FH1 stat tiles (no rounded glass!).
  auto fh1_tile = [&](float tx0, float ty0, float tx1, float ty1, const std::string& val, const char* lbl,
                      uint32_t accent, uint32_t vcol) {
    ui4::panel(l, tx0, ty0, tx1, ty1, s, ui4::kCharcoalDark, accent, 0, true);
    l.text(tx0 + 14 * s, ty0 + 10 * s, lbl, ui4::style(ui::Font::kB, 10.5f * s, ui4::kDim));
    ui::TextStyle vs = ui4::style(ui::Font::kE, 26 * s, vcol);
    ui4::fit_text(val, vs, (tx1 - tx0) - 26 * s, 12 * s);
    l.text(tx0 + 14 * s, ty0 + 32 * s + 0.5f * (26 * s - vs.size), val, vs);
  };

  const float gap = 10 * s, tw = (x1 - x0 - 3 * gap) / 4, th = 72 * s;
  char rate[16];
  std::snprintf(rate, sizeof rate, "%d%%", c.races > 0 ? int(std::lround(100.0 * c.wins / c.races)) : 0);
  const std::pair<std::string, const char*> tiles[] = {
      {std::to_string(c.races), "RACES"}, {std::to_string(c.wins), "WINS"},
      {std::to_string(c.podiums), "PODIUMS"}, {rate, "WIN RATE"}};
  for (int i = 0; i < 4; ++i) {
    const float tx = x0 + i * (tw + gap);
    fh1_tile(tx, y, tx + tw, y + th, tiles[i].first, tiles[i].second, i == 1 ? ui4::kPink : rgba(255, 255, 255, 36),
             i == 1 ? ui4::kYellow : ui4::kWhite);
  }
  y += th + gap;

  const float tw2 = (x1 - x0 - 2 * gap) / 3;
  fh1_tile(x0, y, x0 + tw2, y + th, ui4::format_credits(c.winnings), "TOTAL WINNINGS", ui4::kPink, ui4::kWhite);
  fh1_tile(x0 + tw2 + gap, y, x0 + 2 * tw2 + gap, y + th,
           "LVL " + std::to_string(std::max(1, c.driver_level)) + " • #" + std::to_string(c.popularity_rank),
           "DRIVER LEVEL / FAME", ui4::kYellow, ui4::kYellow);
  fh1_tile(x0 + 2 * (tw2 + gap), y, x1, y + th, std::to_string(c.garage) + (c.garage == 1 ? " CAR" : " CARS"),
           "GARAGE", ui4::kBlue, ui4::kWhite);
  y += th + 14 * s;

  // Collections & Progress card (sharp gritty FH1 panel + flat-capped bars).
  const bool has_outposts = c.outposts_total > 0;
  const bool has_speed = c.speed_traps_total > 0 || c.speed_zones_total > 0;
  const float rows = (c.rally ? 6.f : 5.f) + (has_outposts ? 2.f : 0.f) + (has_speed ? 2.f : 0.f);
  const float rh = (has_outposts || has_speed) ? 27 * s : 36 * s;
  const float ph = 42 * s + rows * rh;
  if (y + ph > h - 68 * s) return;
  ui4::panel(l, x0, y, x1, y + ph, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  ui4::offwhite_strip(l, x0 + 14 * s, y + 10 * s, x1 - 14 * s, y + 32 * s, s, false);
  l.text(x0 + 24 * s, y + 14 * s, "COLORADO EXPLORATION & COLLECTIONS", ui4::style(ui::Font::kE, 13 * s, ui4::kInk));

  const float rx0 = x0 + 20 * s, rx1 = x1 - 20 * s;
  float ry = y + 40 * s;
  auto fh1_coll_row = [&](const std::string& name, int done, int total, uint32_t col) {
    l.text(rx0, ry + 2 * s, name, ui4::style(ui::Font::kB, 11.5f * s, ui4::kWhite));
    char cnt[32];
    std::snprintf(cnt, sizeof cnt, "%d / %d", done, total);
    l.text(rx1, ry + 2 * s, cnt, ui4::style(ui::Font::kE, 12 * s, done >= total && total > 0 ? ui4::kGreen : ui4::kWhite, Align::kRight));
    const float by0 = ry + 18 * s;
    l.rect(rx0, by0, rx1, by0 + 5 * s, rgba(255, 255, 255, 26));
    if (total > 0 && done > 0) {
      const float frac = std::clamp(float(done) / float(total), 0.0f, 1.0f);
      l.rect(rx0, by0, rx0 + (rx1 - rx0) * frac, by0 + 5 * s, col);
    }
    ry += rh;
  };

  fh1_coll_row("ROADS DISCOVERED", c.roads, c.roads_total, ui4::kGreen);
  if (has_outposts) {
    fh1_coll_row("HORIZON OUTPOSTS", c.outposts, c.outposts_total, rgba(255, 146, 3));
    fh1_coll_row("OUTPOST PR STUNTS", c.pr_stunts, c.pr_stunts_total, ui4::kAmber);
  }
  if (has_speed) {
    fh1_coll_row("SPEED TRAPS", c.speed_traps, c.speed_traps_total, ui4::kBlue);
    fh1_coll_row("SPEED ZONES", c.speed_zones, c.speed_zones_total, ui4::kPink);
  }
  fh1_coll_row("DISCOUNT SIGNS", c.signs, c.signs_total, ui4::kPink);
  fh1_coll_row("BARN FINDS", c.barns, c.barns_total, ui4::kYellow);
  fh1_coll_row("1000 CLUB CHALLENGES", c.club_done, c.club_total, ui4::kAmber);
  if (c.rally) {
    char rn[64];
    std::snprintf(rn, sizeof rn, "RALLY  LVL %d  (%s XP)", c.rally_level, thousands(c.rally_xp).c_str());
    fh1_coll_row(rn, c.rally_wins, c.rallies, ui4::kBlue);
  }
  fh1_coll_row("CONTENT PACKS", c.packs_owned, c.packs_total, rgba(200, 205, 215));
}

// Airborne Showcase live progress & distance-gap bar (PLANE_RACE_001..007).
void showcase_hud(ui::List& l, const RaceHud& r, const Layout& g) {
  if (!r.showcase || r.phase != RaceHud::kRacing) return;
  const float s = g.s;
  const float cx = 0.5f * g.w;
  const float bw = 440 * s, bh = 54 * s;
  const float x0 = cx - 0.5f * bw, x1 = cx + 0.5f * bw;
  const float y0 = g.T(24), y1 = y0 + bh;
  ui4::panel(l, x0, y0, x1, y1, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  char gap_buf[48];
  const int gm = int(std::llround(std::fabs(r.showcase_gap_m)));
  std::snprintf(gap_buf, sizeof gap_buf, "%s %d M", r.showcase_gap_m >= 0 ? "AHEAD" : "BEHIND", gm);
  l.text(x0 + 12 * s, y0 + 10 * s, "SHOWCASE  //  VS " + r.showcase_aircraft, font(ui::Font::kB, 13 * s, kText));
  l.text(x1 - 12 * s, y0 + 10 * s, gap_buf,
         font(ui::Font::kE, 14 * s, r.showcase_gap_m >= 0 ? ui4::kGreen : ui4::kAmber, Align::kRight));
  const float tx0 = x0 + 14 * s, tx1 = x1 - 14 * s, ty = y0 + 35 * s, th = 8 * s;
  l.rect(tx0, ty, tx1, ty + th, rgba(255, 255, 255, 35));
  const float ax = tx0 + float(std::clamp(r.showcase_progress, 0.0, 1.0)) * (tx1 - tx0);
  const float px = tx0 + float(std::clamp(r.progress, 0.0, 1.0)) * (tx1 - tx0);
  l.rect(tx0, ty, ax, ty + 4 * s, ui4::kAmber);
  l.rect(tx0, ty + 4 * s, px, ty + th, ui4::kPink);
  l.circle(ax, ty + 2 * s, 5 * s, ui4::kYellow);
  l.circle(px, ty + 6 * s, 5 * s, kText);
}

// Speed Trap & Speed Zone camera flash, trigger banner, and live average-speed strip.
void speed_activity_hud(ui::List& l, const freeroam::SpeedActivityHud& sh, const Layout& g) {
  const float s = g.s;
  if (sh.flash_alpha > 0.01f) {
    l.rect(0, 0, g.w, g.h, rgba(255, 255, 255, uint8_t(std::clamp(sh.flash_alpha * 255.0f, 0.0f, 220.0f))));
  }
  if (sh.zone_active) {
    const float cx = 0.5f * g.w;
    const float x0 = cx - 190 * s, x1 = cx + 190 * s, y0 = g.T(124), y1 = y0 + 48 * s;
    ui4::panel(l, x0, y0, x1, y1, s, ui4::kCharcoalDark, ui4::kBlue, 0, true);
    char avg[64];
    std::snprintf(avg, sizeof avg, "AVG %.1f MPH  •  %.0f M", sh.live_avg_mph, sh.zone_distance_m);
    l.text(cx, y0 + 8 * s, sh.active_zone_id + " ACTIVE", font(ui::Font::kB, 12 * s, ui4::kBlue, Align::kCenter));
    l.text(cx, y0 + 24 * s, avg, font(ui::Font::kE, 18 * s, kText, Align::kCenter));
  }
  if (sh.banner_active) {
    const float cx = 0.5f * g.w;
    const float x0 = cx - 235 * s, x1 = cx + 235 * s, y0 = g.T(178), y1 = y0 + 90 * s;
    Turn t(l, cx, 0.5f * (y0 + y1), ui4::kTiltRad * 0.6f);
    ui4::brush(l, x0, y0, x1, y0 + 30 * s, ui4::kBrushBlack);
    l.text(cx, y0 + 6 * s, sh.banner_title + (sh.banner_new_pb ? "  •  NEW PERSONAL BEST!" : ""),
           font(ui::Font::kB, 14 * s, sh.banner_new_pb ? ui4::kYellow : kText, Align::kCenter));
    ui4::brush(l, x0 + 18 * s, y0 + 28 * s, x1 - 18 * s, y1, sh.banner_is_zone ? ui4::kPink : ui4::kBlue, true);
    char spd[64], best[48];
    std::snprintf(spd, sizeof spd, "%.1f MPH%s", sh.banner_speed_mph, sh.banner_is_zone ? " AVG" : "");
    std::snprintf(best, sizeof best, "PERSONAL BEST  %.1f MPH", sh.banner_best_mph);
    l.text(cx, y0 + 34 * s, spd, font(ui::Font::kE, 29 * s, kText, Align::kCenter));
    l.text(cx, y0 + 69 * s, best, font(ui::Font::kB, 11 * s, rgba(255, 255, 255, 230), Align::kCenter));
  }
}

// Free-roam Head-to-Head challenge prompt, active H2H HUD, and finish banner.
void h2h_hud(ui::List& l, const freeroam::HeadToHeadHud& hh, const Layout& g) {
  const float s = g.s;
  const float cx = 0.5f * g.w;
  if (hh.prompt_visible && !hh.active) {
    const float x0 = cx - 255 * s, x1 = cx + 255 * s, y0 = g.B(542), y1 = y0 + 54 * s;
    ui4::panel(l, x0, y0, x1, y1, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
    char sub[128];
    std::snprintf(sub, sizeof sub, "VS %s [%s %d]  •  WAGER %s CR", hh.prompt_driver.c_str(),
                  hh.prompt_car_class.c_str(), hh.prompt_pi, thousands(hh.prompt_wager).c_str());
    l.text(cx, y0 + 10 * s, "PRESS [X] OR [H] TO CHALLENGE TO HEAD-TO-HEAD",
           font(ui::Font::kE, 16 * s, ui4::kYellow, Align::kCenter));
    l.text(cx, y0 + 31 * s, sub, font(ui::Font::kB, 13 * s, kText, Align::kCenter));
  }
  if (hh.active) {
    const float x0 = cx - 235 * s, x1 = cx + 235 * s, y0 = g.T(134), y1 = y0 + 64 * s;
    ui4::panel(l, x0, y0, x1, y1, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
    char hdr[96], stats[128];
    std::snprintf(hdr, sizeof hdr, "HEAD-TO-HEAD  VS  %s", hh.opponent_name.c_str());
    std::snprintf(stats, sizeof stats, "%d%s / 2   •   %.2f KM LEFT   •   GAP %+.0f M   •   WAGER %s CR", hh.place,
                  place_suffix(hh.place), hh.player_remaining_m / 1000.0, hh.gap_m,
                  thousands(hh.wager_credits).c_str());
    l.text(cx, y0 + 11 * s, hdr, font(ui::Font::kE, 18 * s, kText, Align::kCenter));
    l.text(cx, y0 + 33 * s, stats,
           font(ui::Font::kB, 13 * s, hh.place == 1 ? ui4::kGreen : ui4::kAmber, Align::kCenter));
    const float bx0 = x0 + 14 * s, bx1 = x1 - 14 * s, by = y1 - 9 * s;
    l.rect(bx0, by, bx1, by + 4 * s, rgba(255, 255, 255, 35));
    l.rect(bx0, by, bx0 + float(hh.progress) * (bx1 - bx0), by + 4 * s, ui4::kPink);
  }
  if (hh.finish_banner) {
    const float x0 = cx - 230 * s, x1 = cx + 230 * s, y0 = g.T(210), y1 = y0 + 70 * s;
    Turn t(l, cx, 0.5f * (y0 + y1), ui4::kTiltRad * 0.6f);
    ui4::brush(l, x0, y0, x1, y1, hh.finish_won ? ui4::kPink : ui4::kBrushBlack, true);
    l.text(cx, y0 + 10 * s, hh.finish_won ? "HEAD-TO-HEAD VICTORY!" : "HEAD-TO-HEAD DEFEAT",
           font(ui::Font::kE, 26 * s, kText, Align::kCenter));
    char detail[96];
    if (hh.finish_won)
      std::snprintf(detail, sizeof detail, "BEAT %s   •   +%s CR  +1,000 FAME", hh.finish_opponent.c_str(),
                    thousands(hh.finish_credits).c_str());
    else std::snprintf(detail, sizeof detail, "%s REACHED THE FINISH FIRST", hh.finish_opponent.c_str());
    l.text(cx, y0 + 44 * s, detail, font(ui::Font::kB, 13 * s, ui4::kYellow, Align::kCenter));
  }
}

// Free-roam Driver Level / Manufacturer Affinity pill & SPEC-UI-LAYOUTS Sponsor Notification.
void progression_hud(ui::List& l, const DriveHud& st, const Layout& g) {
  const float s = g.s;
  // Compact Horizon pill top-left (uses HUDPILL01, zero rounded dark box).
  if (!st.race && st.driver_level > 0) {
    const float x0 = g.L(64), y0 = g.T(32), x1 = x0 + 266 * s, y1 = y0 + 26 * s;
    ui4::pill(l, x0, y0, x1, y1, ui4::kPillGrey, ui4::PillKind::kHud01);
    l.rect(x0, y0, x0 + 4 * s, y1, ui4::kYellow);
    char l1[80];
    std::snprintf(l1, sizeof l1, "LVL %d  •  %s XP  •  AFFINITY %d", st.driver_level, thousands(st.driver_xp).c_str(),
                  st.affinity_level);
    l.text(x0 + 12 * s, y0 + 6 * s, l1, font(ui::Font::kB, 12.5f * s, ui4::kWhite));
  }
  // Authentic SPEC-UI-LAYOUTS Sponsor Notification (yellow tag over gritty dark strip).
  const double age = st.time - st.sponsor_at;
  if (!st.sponsor_title.empty() && age >= 0.0 && age < 5.5) {
    const float cx = 0.5f * g.w;
    const float x0 = cx - 160 * s, x1 = cx + 160 * s, y0 = g.B(576), y1 = g.B(650);
    ui4::brush(l, cx - 108 * s, y0, cx + 108 * s, y0 + 24 * s, ui4::kYellow);
    l.text(cx, y0 + 5 * s, "SPONSOR CHALLENGE!", font(ui::Font::kE, 13 * s, ui4::kInk, Align::kCenter));
    ui4::panel(l, x0, y0 + 24 * s, x1, y1, s, ui4::kCharcoalDark, 0, ui4::kYellow, true);
    TextStyle stt = font(ui::Font::kE, 16 * s, ui4::kWhite);
    fit(st.sponsor_title, stt, (x1 - x0) - 28 * s, 10 * s);
    TextStyle stdt = font(ui::Font::kB, 12.5f * s, ui4::kYellow);
    fit(st.sponsor_detail, stdt, (x1 - x0) - 28 * s, 9 * s);
    l.text(x0 + 16 * s, y0 + 32 * s, st.sponsor_title, stt);
    l.text(x0 + 16 * s, y0 + 52 * s, st.sponsor_detail, stdt);
  }
}

}  // namespace

namespace {
std::function<std::string(const std::string&)>& names() {
  static std::function<std::string(const std::string&)> f;
  return f;
}
}  // namespace

std::function<std::string(const std::string&)>& texts() {
  static std::function<std::string(const std::string&)> f;
  return f;
}

void set_car_names(std::function<std::string(const std::string&)> source) { names() = std::move(source); }
void set_game_text(std::function<std::string(const std::string&)> source) { texts() = std::move(source); }
namespace {
std::function<bool(const std::string&, std::array<float, 4>&)>& textures() {
  static std::function<bool(const std::string&, std::array<float, 4>&)> f;
  return f;
}
}  // namespace
void set_ui_textures(std::function<bool(const std::string&, std::array<float, 4>&)> source) {
  textures() = source;
  ui4::set_texture_loader(std::move(source));
}
bool ui_texture(const std::string& path, std::array<float, 4>& uv) { return textures() && textures()(path, uv); }
std::string game_text(const std::string& value) { return texts() ? texts()(value) : value; }
std::string car_name(const std::string& media) { return names() ? names()(media) : media; }

std::string thousands(int64_t v) {
  std::string d = std::to_string(v < 0 ? -v : v), out;
  for (size_t i = 0; i < d.size(); ++i) {
    if (i && (d.size() - i) % 3 == 0) out += ',';
    out += d[i];
  }
  return v < 0 ? "-" + out : out;
}

const char* place_suffix(int p) {
  if (p % 100 >= 11 && p % 100 <= 13) return "TH";
  switch (p % 10) {
    case 1: return "ST";
    case 2: return "ND";
    case 3: return "RD";
    default: return "TH";
  }
}

std::string race_clock(double t) {
  if (t < 0) t = 0;
  const int m = int(t / 60);
  char b[32];
  std::snprintf(b, sizeof b, "%d:%05.2f", m, t - 60 * m);
  return b;
}

float drive_hud_dial_height(float h) { return (720 - 587 + 88) * (h / 720.0f); }

void build_drive_hud(ui::List& l, const DriveHud& st, float w, float h) {
  const float s = h / 720.0f;
  const Layout g{w, h, s};
  if (st.title_screen && st.title_screen->active) {
    title_screen_hud(l, *st.title_screen, g, st.time);
    return;
  }
  if (st.ceremony && st.ceremony->active) {
    ceremony_overlay(l, *st.ceremony, g);
    return;
  }
  if (st.map_overlay && st.map_overlay->active) {
    map_overlay_hud(l, *st.map_overlay, g);
    return;
  }
  if (st.rally_hub && st.rally_hub->active) {
    build_rally_hub_screen(l, *st.rally_hub, w, h);
    return;
  }
  if (st.rally_service && st.rally_service->active) {
    build_rally_service_area_screen(l, *st.rally_service, w, h);
    return;
  }
  if (st.rally_results && st.rally_results->active) {
    build_rally_championship_results_screen(l, *st.rally_results, w, h);
    return;
  }
  const bool menu = !st.menu_title.empty() || !st.menu.empty();
  // The driving HUD hides under a menu (SPEC-UI-LAYOUTS d).
  if (st.driving && !menu) {
    if (!st.name_tags.empty() && (!st.race || st.race->phase == RaceHud::kRacing))
      opponent_name_tags(l, st.name_tags, g);
    if (st.world_marker && !st.race) world_marker_hud(l, *st.world_marker, g);
    if (st.gps && st.gps->active && !st.race) gps_turn_hud(l, *st.gps, g);
    if (st.pr_stunt && st.pr_stunt->active && !st.race) pr_stunt_hud(l, *st.pr_stunt, g);
    if (st.h2h && !st.race) h2h_hud(l, *st.h2h, g);
    if (st.speed_hud && !st.race) speed_activity_hud(l, *st.speed_hud, g);
    if (st.skills && !(st.race && (st.race->phase == RaceHud::kFinished || st.race->grid_menu_open)))
      skill_chain_hud(l, *st.skills, g, st.time);
    if (st.race) {
      const RaceHud& r = *st.race;
      if (r.phase == RaceHud::kGrid) event_card(l, r, g);
      else if (r.phase == RaceHud::kFinished) results(l, r, g);
      else {
        race_hud(l, r, g);
        showcase_hud(l, r, g);
      }
    }
    if (st.rally_stage && st.rally_stage->active) build_rally_pace_notes_hud(l, *st.rally_stage, w, h);
    progression_hud(l, st, g);
    popup_card(l, st, g);
    const bool hide_dial = st.race && (st.race->phase == RaceHud::kFinished || st.race->grid_menu_open);
    if (!hide_dial) dial(l, st, g);
  }
  if (!st.toast.empty() && !menu) {
    TextStyle ts = font(ui::Font::kB, 14 * s, kText, Align::kCenter);
    fit(st.toast, ts, w - 120 * s, 8 * s);
    const float tw = ui::List::measure(st.toast, ts);
    const float y = g.B(653);
    l.rect(0.5f * w - 0.5f * tw - 14 * s, y, 0.5f * w + 0.5f * tw + 14 * s, y + 28 * s, kBlack60);
    l.text(0.5f * w, y + 7 * s, st.toast, ts);
  }
  if (menu) menu_panel(l, st, g);
  if (menu && st.career) career_board(l, *st.career, w, h, s);
}

}  // namespace carview
