#include "game_hud.h"

#include "hud_scene.h"

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

// A wristband's colour by its name (ours); amber for an unknown name.
uint32_t band_colour(const std::string& name) {
  std::string n;
  for (char c : name) n += char(std::tolower(static_cast<unsigned char>(c)));
  const std::pair<const char*, uint32_t> named[] = {
      {"yellow", rgba(255, 214, 40)}, {"green", rgba(70, 210, 100)},  {"orange", rgba(255, 140, 30)},
      {"blue", rgba(60, 140, 255)},   {"purple", rgba(170, 90, 240)}, {"red", rgba(236, 52, 44)},
      {"gold", rgba(240, 196, 70)},   {"silver", rgba(200, 205, 215)}, {"black", rgba(90, 92, 100)},
      {"white", rgba(240, 240, 240)}, {"pink", rgba(255, 110, 180)}};
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
  const float s = g.s;
  // The numerals (SPEC-UI-LAYOUTS a, measured: centre 1122,587, radius
  // 78.5); the face, ticks and needle from 947_HUD GAUGE_ANALOG (centre
  // 1123,593.3; MAINBACKRING 256 px; hatches 13 x 9 at radius 82; NEEDLE's
  // bar from radius 44 to 72, tinted pink).
  const float nx = g.R(1122), ny = g.B(587);
  const float cx = g.R(1123), cy = g.B(593.3f);
  constexpr float kSweep = 2.391f;  // 137°
  std::array<float, 4> ring;
  const bool textured = ui_texture("HUD/DIALS/MAINBACKRING", ring);
  if (textured) l.image(g.R(995), g.B(465), g.R(1251), g.B(721), ring, rgba(255, 255, 255, 242));
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
  const float x1 = g.R(1162), y = g.B(571);
  const float tail = l.text(x1, y, all.substr(lead), sp);
  if (lead) {
    TextStyle z = sp;
    z.color = rgba(255, 255, 255, 128);
    l.text(x1 - tail, y, all.substr(0, lead), z);
  }
  TextStyle unit = font(ui::Font::kA, 10 * s, kDim);
  unit.keep_case = true;
  l.text(g.R(1112), g.B(553), str("UnitStrings", "IDS_Units_KMH", "km/h"), unit);
  const char* gear = st.gear < 0 ? "R" : st.gear == 0 ? "N" : nullptr;
  char gb[8];
  if (!gear) std::snprintf(gb, sizeof gb, "%d", st.gear), gear = gb;
  l.text(g.R(1093), g.B(607), gear, font(ui::Font::kE, 17 * s, kGear));
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
  const float s = g.s;
  const SceneFrame f{g.w - 1280 * s, g.h - 720 * s, s};  // bottom-right anchored
  const auto [gx, gy] = sc.at(gauge);
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
  // The assists' labels (ASSIST_ICONS: ABSON and TCS ON at half opacity).
  const int assists = sc.find("ASSIST_ICONS", gauge);
  for (const char* name : {"HUD_ABS", "HUD_TCS"}) {
    const int on = sc.find("Rectangle_ON", sc.find(name, assists));
    sc.model(l, f, on, sc.alpha(on), 0, 0, 0, kPillGrey);  // grey: off, as the frames show
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

// (b1) The event's title card, on the grid.
void event_card(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
  brush(l, 0, g.B(547), g.L(968), g.B(647), kPink);
  brush(l, g.L(75), g.B(535), g.L(440), g.B(590), kBrush);
  const std::string type =
      !r.event_type.empty() ? r.event_type : str("CareerEventUI", "IDS_TitleFestivalRace", "FESTIVAL RACE");
  TextStyle ts = font(ui::Font::kE, 36 * s, kText);
  fit(type, ts, g.L(440) - g.L(100) - 10 * s, 18 * s);
  l.text(g.L(100), g.B(546) + (36 * s - ts.size) * 0.5f, type, ts);
  TextStyle ns = font(ui::Font::kE, 47 * s, kText);
  fit(r.event, ns, g.L(950) - g.L(100), 20 * s);
  l.text(g.L(100), g.B(588) + (47 * s - ns.size) * 0.5f, r.event, ns);
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

void results(ui::List& l, const RaceHud& r, const Layout& g) {
  const float s = g.s;
  const double since = r.clock - r.finish_time;
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
    help_item(l, g, 70, 190, 653, 689, "A", kGreen, str("HelpButtons", "IDS_ContinueMenu", "CONTINUE"));
    help_item(l, g, 195, 307, 653, 689, "Y", kAccent, str("HelpButtons", "IDS_RestartMenu", "RESTART"));
    return;
  }
  // The cash reward.
  l.rect(0, 0, g.w, g.h, rgba(0, 0, 0, 90));
  {
    Turn t(l, g.L(313), g.T(180));
    brush(l, g.L(98), g.T(141), g.L(529), g.T(218), kBrush);
    TextStyle ts = font(ui::Font::kE, 57 * s, kText);
    const std::string title = str("ScreenTitles", "IDS_PostRace_Winnings", "CASH REWARD");
    fit(title, ts, g.L(519) - g.L(108), 24 * s);
    l.text(g.L(108), g.T(151), title, ts);
  }
  const std::string cr = str("UnitStrings", "IDS_Units_Credits", "Cr");
  // The breakdown (SPEC-UI-LAYOUTS c): labels in B 20 from x 148 at rows
  // 243, 271, 299 on dark tape, each value on a pink tag just after its
  // label. SPEC-EVENT-REWARDS: the finish prize, then Mileage and
  // Difficulty & assists, whose values are not decoded (0 and +0%).
  char line[48];
  const int place = r.stage ? r.stage_place : r.place;
  std::snprintf(line, sizeof line, "%s %d%s", str("InGame", "IDS_Finished", "FINISHED").c_str(), place, place_suffix(place));
  const std::pair<std::string, std::string> rows[] = {
      {line, thousands(r.credits) + " " + cr},
      {str("OnlineRewards", "IDS_Mileage_Bonus_Heading", "MILEAGE BONUS"), "0 " + cr},
      {str("OnlineRewards", "IDS_Difficulty_Bonus_Heading", "DIFFICULTY & ASSISTS BONUS"), "+0%"}};
  for (int i = 0; i < 3; ++i) {
    const float y = g.T(243 + 28.0f * i);
    const TextStyle ls = font(ui::Font::kB, 20 * s, kText);
    TextStyle vs = font(ui::Font::kE, 18 * s, kText);
    vs.keep_case = true;
    const float lw = ui::List::measure(rows[i].first, ls), vw = ui::List::measure(rows[i].second, vs);
    Turn t(l, g.L(148) + 0.5f * lw, y + 10 * s);
    brush(l, g.L(140), y - 4 * s, g.L(148) + lw + 6 * s, y + 24 * s, kBrush);
    l.text(g.L(148), y, rows[i].first, ls);
    const float vx = g.L(148) + lw + 10 * s;
    brush(l, vx - 4 * s, y - 2 * s, vx + vw + 6 * s, y + 21 * s, kPink);
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
    l.rect(g.L(150), g.T(475), g.L(410), g.T(520), kLight);
    TextStyle bs = font(ui::Font::kB, 16 * s, kDark);
    l.text(g.L(162), g.T(490), str("OnlineRewards", "IDS_Balance", "BALANCE"), bs);
    bs.align = Align::kRight;
    bs.keep_case = true;  // "117,881 Cr" as shown
    l.text(g.L(398), g.T(490), thousands(r.balance) + " " + cr, bs);
  }
  // The wristband (ours: where the title shows it is not specified).
  if (r.band_after >= 0) wristband_row(l, r, g.L(150), g.T(545), g.L(880) - g.L(150), s, since - 1.0, 1.0f);
  help_item(l, g, 70, 190, 653, 689, "A", kGreen, str("HelpButtons", "IDS_ContinueMenu", "CONTINUE"));
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

// (d) The pause menu: the items down the left on brushes, the career's
// panels top right, the help bar.
void menu_panel(ui::List& l, const DriveHud& st, const Layout& g) {
  const float s = g.s;
  l.rect(0, 0, g.w, g.h, rgba(0, 0, 0, 110));
  const bool pause = st.pause || st.menu_title == "paused";
  // A page's title (ours: the pause menu has none).
  std::string title = st.menu_title, hint;
  if (auto c = title.find(':'); c != std::string::npos) {
    hint = title.substr(c + 1);
    title = title.substr(0, c);
    while (!hint.empty() && hint.front() == ' ') hint.erase(hint.begin());
  }
  if (!pause && !title.empty()) {
    TextStyle ts = font(ui::Font::kE, 30 * s, kText);
    fit(title, ts, g.L(800), 14 * s);
    brush(l, g.L(130), g.T(50), g.L(150) + ui::List::measure(title, ts) + 20 * s, g.T(94), kBrush);
    l.text(g.L(150), g.T(57), title, ts);
    if (!hint.empty()) {
      TextStyle hs = font(ui::Font::kB, 12 * s, kDim);
      fit(hint, hs, g.w - g.L(150) - 40 * s, 7 * s);
      l.text(g.L(150), g.T(102), hint, hs);
    }
  }
  // The items, scrolled so the selected one shows: bottoms 186 + 50 each.
  const int n = int(st.menu.size());
  const int fit_rows = std::max(1, int((g.B(610) - g.T(186)) / (50 * s)) + 1);
  const int first = std::clamp(st.menu_selected - fit_rows / 2, 0, std::max(0, n - fit_rows));
  // 925_PAUSE_MENU's Option_List turns the rows 0.052 rad about its first.
  Turn list_turn(l, g.L(226), g.T(165));
  for (int i = first; i < n && i < first + fit_rows; ++i) {
    const bool sel = i == st.menu_selected;
    // The selected brush is taller: the items from it down move 12 px.
    const float bottom = g.T(186 + 50.0f * float(i - first) + (i >= st.menu_selected && st.menu_selected > first ? 12 : 0));
    const std::string& item = st.menu[size_t(i)];
    TextStyle it = font(ui::Font::kE, (sel ? 40 : 37) * s, kText);
    fit(item, it, g.w - g.L(260), 12 * s);
    const float tw = ui::List::measure(item, it);
    if (sel) {
      brush(l, g.L(147), bottom - 61 * s, std::max(g.L(362), g.L(185) + tw + 39 * s), bottom + 9 * s, kPink);
      l.text(g.L(185), bottom - it.size, item, it);
    } else {
      brush(l, g.L(160), bottom - it.size - 8 * s, g.L(188) + tw + 24 * s, bottom + 8 * s, kBrush);
      l.text(g.L(188), bottom - it.size, item, it);
    }
  }
  l.unturn();
  // The help bar.
  help_item(l, g, 70, 168, 654, 684, "A", kGreen, str("HelpButtons", "IDS_Select", "SELECT"));
  help_item(l, g, 173, 275, 654, 684, "B", kRed, str("HelpButtons", "IDS_Resume", pause ? "RESUME" : "BACK"));
  if (!pause) return;
  // The career's panels, top right.
  if (!st.band_next.empty()) {
    const uint32_t c = band_colour(st.band_next);
    l.rect(g.R(825), g.T(36), g.R(1040), g.T(62), kBlack60);
    std::string goal = str("InGame", "IDS_CollectWristband", "COLLECT %s WRISTBAND");
    if (auto p = goal.find("%s"); p != std::string::npos) goal.replace(p, 2, st.band_next);
    else if (auto q = goal.find("{0}"); q != std::string::npos) goal.replace(q, 3, st.band_next);
    TextStyle gs = font(ui::Font::kB, 14 * s, c);
    fit(goal, gs, g.R(1034) - g.R(833), 8 * s);
    l.text(g.R(833), g.T(42), goal, gs);
  }
  std::array<float, 4> band_uv;
  std::string colour_word = st.band_name.substr(0, st.band_name.find(' '));
  for (char& ch : colour_word) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  if (!colour_word.empty() && ui_texture("WRISTBANDS/" + colour_word, band_uv)) {
    // The band's texture (925_PAUSE_MENU Wristband: 1000,15-1256,79).
    l.image(g.R(1000), g.T(15), g.R(1256), g.T(79), band_uv);
  } else if (!st.band_name.empty()) {
    l.rect(g.R(1045), g.T(34), g.R(1209), g.T(61), band_colour(st.band_name));
    TextStyle bs = font(ui::Font::kE, 16 * s, kText, Align::kCenter);
    std::string band = "HORIZON 2012";
    if (st.band_level > 0) band += " " + std::to_string(st.band_level);
    fit(band, bs, g.R(1203) - g.R(1051), 8 * s);
    l.text(0.5f * (g.R(1045) + g.R(1209)), g.T(39), band, bs);
  }
  if (!st.car_name.empty()) {
    l.rect(g.R(912), g.T(62), g.R(1208), g.T(89), kBlack60);
    TextStyle cs = font(ui::Font::kB, 19 * s, kText);
    cs.keep_case = true;  // the display name as written (SPEC-UI-LAYOUTS d)
    fit(st.car_name, cs, g.R(1120) - g.R(922), 9 * s);
    l.text(g.R(922), g.T(69), st.car_name, cs);
    std::array<float, 4> class_uv;
    if (!st.car_class.empty() && ui_texture("PICLASS/CLASS_" + st.car_class, class_uv)) {
      // The class badge (925_PAUSE_MENU CLASS_A: 1125,63-1214,86), the index on its white part.
      l.image(g.R(1125), g.T(63), g.R(1214), g.T(86), class_uv);
      char pi[8];
      std::snprintf(pi, sizeof pi, "%d", st.car_pi);
      // The texture's white box spans u 56-99 of 128: x 1164-1194 here.
      TextStyle ps = font(ui::Font::kB, 14 * s, kDark, Align::kRight);
      fit(pi, ps, g.R(1192) - g.R(1166), 8 * s);
      l.text(g.R(1192), g.T(68) + (14 * s - ps.size) * 0.5f, pi, ps);
    } else if (!st.car_class.empty()) {
      l.rect(g.R(1144), g.T(64), g.R(1172), g.T(86), class_colour(st.car_class));
      l.text(g.R(1158), g.T(66), st.car_class, font(ui::Font::kE, 17 * s, kText, Align::kCenter));
      l.rect(g.R(1172), g.T(64), g.R(1205), g.T(86), rgba(255, 255, 255));
      char pi[8];
      std::snprintf(pi, sizeof pi, "%d", st.car_pi);
      l.text(g.R(1202), g.T(68), pi, font(ui::Font::kB, 14 * s, kDark, Align::kRight));
    }
  }
  if (st.credits >= 0) {
    l.rect(g.R(1090), g.T(88), g.R(1202), g.T(113), kLight);
    // UnitStrings / IDS_Units_Credits, mixed case as shown ("117,700 Cr").
    const std::string c = thousands(st.credits) + " " + str("UnitStrings", "IDS_Units_Credits", "Cr");
    TextStyle cs = font(ui::Font::kB, 16 * s, kDark, Align::kRight);
    cs.keep_case = true;
    fit(c, cs, g.R(1196) - g.R(1096), 8 * s);
    l.text(g.R(1196), g.T(93) + (16 * s - cs.size) * 0.5f, c, cs);
  }
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
  const float x0 = std::min(w, 440 * s) + 44 * s, x1 = w - 44 * s;
  if (x1 - x0 < 300 * s) return;
  float y = 44 * s;
  // The wristband card.
  const bool band = !c.band_name.empty();
  const uint32_t bc = band ? band_colour(c.band_name) : kDim;
  const float ch = 132 * s;
  panel(l, x0, y, x1, y + ch, s, rgba(10, 12, 17, 205));
  l.rect(x0, y, x0 + 8 * s, y + ch, bc, 4 * s);
  label(l, x0 + 28 * s, y + 18 * s, "WRISTBAND", s, kDim);
  l.text(x0 + 28 * s, y + 36 * s, band ? c.band_name : std::string("NO WRISTBAND YET"),
         TextStyle{.size = 30 * s, .weight = 0.13f, .slant = 0.1f, .color = bc});
  label(l, x1 - 24 * s, y + 18 * s, "CREDITS", s, kDim, Align::kRight);
  l.text(x1 - 24 * s, y + 38 * s, thousands(c.credits) + " CR",
         TextStyle{.size = 22 * s, .weight = 0.12f, .color = kText, .align = Align::kRight});
  const float bx0 = x0 + 28 * s, bx1 = x1 - 24 * s, by = y + 86 * s, bh = 10 * s;
  l.rect(bx0, by, bx1, by + bh, rgba(255, 255, 255, 32), 5 * s);
  const float k = c.band_hi > c.band_lo
                      ? float(std::clamp(double(c.points - c.band_lo) / double(c.band_hi - c.band_lo), 0.0, 1.0))
                      : 1.0f;
  if (band || c.points > 0) l.rect(bx0, by, std::max(bx0 + bh, bx0 + (bx1 - bx0) * k), by + bh, bc, 5 * s);
  char pts[64];
  if (c.band_hi > 0)
    std::snprintf(pts, sizeof pts, "%s / %s POINTS", thousands(c.points).c_str(), thousands(c.band_hi).c_str());
  else
    std::snprintf(pts, sizeof pts, "%s POINTS", thousands(c.points).c_str());
  l.text(bx0, by + bh + 8 * s, pts, TextStyle{.size = 9 * s, .weight = 0.11f, .tracking = 0.12f, .color = kDim});
  if (c.band_hi > 0 && !c.band_next.empty())
    l.text(bx1, by + bh + 8 * s, "NEXT: " + c.band_next + "  IN " + thousands(c.band_hi - c.points),
           TextStyle{.size = 9 * s, .weight = 0.11f, .tracking = 0.12f, .color = kDim, .align = Align::kRight});
  else if (band)
    l.text(bx1, by + bh + 8 * s, "TOP WRISTBAND",
           TextStyle{.size = 9 * s, .weight = 0.11f, .tracking = 0.12f, .color = kAccent, .align = Align::kRight});
  y += ch + 16 * s;
  // Records: four tiles.
  const float gap = 12 * s, tw = (x1 - x0 - 3 * gap) / 4, th = 84 * s;
  char rate[16];
  std::snprintf(rate, sizeof rate, "%d%%", c.races > 0 ? int(std::lround(100.0 * c.wins / c.races)) : 0);
  const std::pair<std::string, const char*> tiles[] = {
      {std::to_string(c.races), "RACES"}, {std::to_string(c.wins), "WINS"},
      {std::to_string(c.podiums), "PODIUMS"}, {rate, "WIN RATE"}};
  for (int i = 0; i < 4; ++i) {
    const float tx = x0 + i * (tw + gap);
    stat_tile(l, tx, y, tx + tw, y + th, tiles[i].first, tiles[i].second, s, i == 1 ? kAccent : kText);
  }
  y += th + gap;
  const float tw2 = (x1 - x0 - gap) / 2;
  stat_tile(l, x0, y, x0 + tw2, y + th, thousands(c.winnings) + " CR", "TOTAL WINNINGS", s);
  stat_tile(l, x0 + tw2 + gap, y, x1, y + th, std::to_string(c.garage) + (c.garage == 1 ? " CAR" : " CARS"), "GARAGE",
            s);
  y += th + 16 * s;
  // Collections.
  const float rows = c.rally ? 5.f : 4.f;
  const float rh = 40 * s, ph = 24 * s + rows * rh;
  if (y + ph > h - 30 * s) return;
  panel(l, x0, y, x1, y + ph, s, rgba(10, 12, 17, 190));
  const float rx0 = x0 + 24 * s, rx1 = x1 - 24 * s;
  float ry = y + 16 * s;
  collection_row(l, rx0, rx1, ry, "DISCOUNT SIGNS", c.signs, c.signs_total, s), ry += rh;
  collection_row(l, rx0, rx1, ry, "BARN FINDS", c.barns, c.barns_total, s), ry += rh;
  collection_row(l, rx0, rx1, ry, "1000 CLUB CHALLENGES", c.club_done, c.club_total, s), ry += rh;
  if (c.rally) {
    char rn[64];
    std::snprintf(rn, sizeof rn, "RALLY  LEVEL %d  %s XP", c.rally_level, thousands(c.rally_xp).c_str());
    collection_row(l, rx0, rx1, ry, rn, c.rally_wins, c.rallies, s, rgba(60, 140, 255)), ry += rh;
  }
  collection_row(l, rx0, rx1, ry, "CONTENT PACKS", c.packs_owned, c.packs_total, s, rgba(200, 205, 215));
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
  textures() = std::move(source);
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
  const bool menu = !st.menu_title.empty() || !st.menu.empty();
  // The driving HUD hides under a menu (SPEC-UI-LAYOUTS d).
  if (st.driving && !menu) {
    if (st.race) {
      const RaceHud& r = *st.race;
      if (r.phase == RaceHud::kGrid) event_card(l, r, g);
      else if (r.phase == RaceHud::kFinished) results(l, r, g);
      else race_hud(l, r, g);
    }
    popup_card(l, st, g);
    if (!(st.race && st.race->phase == RaceHud::kFinished)) dial(l, st, g);
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
