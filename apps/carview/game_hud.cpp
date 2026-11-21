#include "game_hud.h"

#include <algorithm>
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

void dial(ui::List& l, const DriveHud& st, float w, float h, float s) {
  const float R = 80 * s;
  const float cx = w - 30 * s - R, cy = h - 26 * s - R;
  l.shadow(cx - R - 16 * s, cy - R - 16 * s, cx + R + 16 * s, cy + R + 16 * s, kShadow, R + 16 * s, 14 * s);
  l.circle(cx, cy, R + 16 * s, rgba(8, 10, 14, 205));
  const double red = std::max(1000.0, st.redline_rpm);
  const double top = std::ceil(red * 1.1 / 1000) * 1000;  // the dial's end, whole thousands
  auto angle = [&](double rpm) { return kDialA0 + (kDialA1 - kDialA0) * float(std::clamp(rpm / top, 0.0, 1.0)); };
  const float track = 7 * s;
  l.arc(cx, cy, R, track, kDialA0, kDialA1, rgba(255, 255, 255, 38));
  l.arc(cx, cy, R, track, angle(red), kDialA1, rgba(236, 52, 44, 110));
  const bool shift = st.rpm > 0.94 * red;
  const bool blink = st.rpm > 0.985 * red && std::fmod(st.time, 0.16) < 0.08;
  const uint32_t fill = blink ? rgba(255, 255, 255) : shift ? kRed : kText;
  l.arc(cx, cy, R, track, kDialA0, std::max(kDialA0 + 0.01f, angle(st.rpm)), fill);
  // A tick and digit per thousand.
  TextStyle digits{.size = 8.5f * s, .weight = 0.11f, .color = kDim, .align = Align::kCenter};
  for (double k = 0; k <= top + 1; k += 1000) {
    const float a = angle(k);
    const float sa = std::sin(a), ca = -std::cos(a);
    const bool in_red = k >= red;
    l.line(cx + (R - 12 * s) * sa, cy + (R - 12 * s) * ca, cx + (R - 6 * s) * sa, cy + (R - 6 * s) * ca, 2 * s,
           in_red ? kRed : rgba(255, 255, 255, 150));
    digits.color = in_red ? kRed : kDim;
    char d[8];
    std::snprintf(d, sizeof d, "%d", int(k / 1000));
    l.text(cx + (R - 24 * s) * sa, cy + (R - 24 * s) * ca - 4.25f * s, d, digits);
  }
  // The speed and the gear.
  char speed[16];
  std::snprintf(speed, sizeof speed, "%d", int(std::lround(std::fabs(st.kmh))));
  l.text(cx + 2 * s, cy - 22 * s, speed,
         TextStyle{.size = 34 * s, .weight = 0.12f, .slant = 0.12f, .color = kText, .align = Align::kCenter});
  l.text(cx, cy + 19 * s, "KM/H",
         TextStyle{.size = 7.5f * s, .weight = 0.11f, .tracking = 0.35f, .color = kDim, .align = Align::kCenter});
  const char* gear = st.gear < 0 ? "R" : st.gear == 0 ? "N" : nullptr;
  char g[8];
  if (!gear) std::snprintf(g, sizeof g, "%d", st.gear), gear = g;
  const float gy = cy + 52 * s;
  l.rect(cx - 15 * s, gy - 15 * s, cx + 15 * s, gy + 15 * s, shift ? kRed : kAccent, 7 * s);
  l.text(cx, gy - 9 * s, gear, TextStyle{.size = 18 * s, .weight = 0.14f, .color = kInk, .align = Align::kCenter});
  if (st.abs)
    l.text(cx - R - 22 * s, cy + R - 4 * s, "ABS",
           TextStyle{.size = 10 * s, .weight = 0.13f, .color = kAccent, .align = Align::kRight});
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

// Free roam's career chip: credits, then the wristband with its bar.
void career_chip(ui::List& l, const DriveHud& st, float w, float s) {
  const float cw = 260 * s, x1 = w - 26 * s, x0 = x1 - cw, y0 = 26 * s;
  const bool band = !st.band_name.empty();
  panel(l, x0, y0, x1, y0 + (band ? 62 : 38) * s, s, rgba(10, 12, 17, 175));
  l.circle(x0 + 22 * s, y0 + 19 * s, 8 * s, kAccent);
  l.text(x0 + 22 * s, y0 + 14.5f * s, "C", TextStyle{.size = 9 * s, .weight = 0.15f, .color = kInk, .align = Align::kCenter});
  l.text(x0 + 38 * s, y0 + 12 * s, thousands(st.credits) + " CR",
         TextStyle{.size = 14 * s, .weight = 0.12f, .color = kText});
  if (!band) return;
  const uint32_t c = band_colour(st.band_name);
  l.text(x0 + 14 * s, y0 + 36 * s, st.band_name, TextStyle{.size = 8 * s, .weight = 0.12f, .tracking = 0.2f, .color = c});
  if (st.band_hi > st.band_lo) {
    char pts[48];
    std::snprintf(pts, sizeof pts, "%s / %s", thousands(st.band_points).c_str(), thousands(st.band_hi).c_str());
    l.text(x1 - 14 * s, y0 + 36 * s, pts,
           TextStyle{.size = 8 * s, .weight = 0.11f, .color = kDim, .align = Align::kRight});
  }
  const float bx0 = x0 + 14 * s, bx1 = x1 - 14 * s, by = y0 + 49 * s;
  l.rect(bx0, by, bx1, by + 5 * s, rgba(255, 255, 255, 36), 2.5f * s);
  const float k = st.band_hi > st.band_lo
                      ? float(std::clamp(double(st.band_points - st.band_lo) / double(st.band_hi - st.band_lo), 0.0, 1.0))
                      : 1.0f;
  l.rect(bx0, by, std::max(bx0 + 5 * s, bx0 + (bx1 - bx0) * k), by + 5 * s, c, 2.5f * s);
}

// The discovery card: drops in, holds, fades.
void popup_card(ui::List& l, const DriveHud& st, float w, float s) {
  const double age = st.time - st.popup_at;
  if (st.popup_title.empty() || age < 0 || age > kPopupSeconds) return;
  const float in = ease_in_time(age, 0, 0.35);
  const float out = 1 - clamp01((age - (kPopupSeconds - 0.5)) / 0.5);
  const float a = in * out;
  const uint32_t c = st.popup_colour ? st.popup_colour : kAccent;
  const TextStyle ts{.size = 20 * s, .weight = 0.13f, .slant = 0.1f, .color = ui::fade(kText, a), .align = Align::kCenter};
  TextStyle body{.size = 10 * s, .weight = 0.11f, .tracking = 0.08f, .color = ui::fade(kDim, a), .align = Align::kCenter};
  const float cw = std::max({300 * s, ui::List::measure(st.popup_title, ts) + 60 * s,
                             ui::List::measure(st.popup_text, body) + 60 * s});
  const bool meter = st.popup_progress >= 0;
  const float ch = (meter ? 90 : 74) * s;
  const float x0 = 0.5f * w - 0.5f * cw, y0 = 26 * s - (1 - in) * 30 * s;
  l.shadow(x0, y0 + 3 * s, x0 + cw, y0 + ch + 3 * s, ui::fade(kShadow, a), 10 * s, 12 * s);
  l.rect(x0, y0, x0 + cw, y0 + ch, ui::fade(kPanelLow, a), 10 * s);
  l.rect(x0, y0, x0 + cw, y0 + 4 * s, ui::fade(c, a), 2 * s);
  l.text(0.5f * w, y0 + 14 * s, st.popup_title, ts);
  l.text(0.5f * w, y0 + 50 * s, st.popup_text, body);
  if (meter) {
    const float bx0 = x0 + 30 * s, bx1 = x0 + cw - 30 * s, by = y0 + 72 * s;
    const float k = float(std::clamp(st.popup_progress, 0.0, 1.0)) * ease_in_time(age, 0.3, 0.8);
    l.rect(bx0, by, bx1, by + 5 * s, rgba(255, 255, 255, uint8_t(36 * a)), 2.5f * s);
    l.rect(bx0, by, std::max(bx0 + 5 * s, bx0 + (bx1 - bx0) * k), by + 5 * s, ui::fade(c, a), 2.5f * s);
  }
}

void race_panels(ui::List& l, const RaceHud& r, float w, float h, float s, double t) {
  const float x0 = 26 * s, y0 = 26 * s;
  if (r.phase == RaceHud::kGrid || r.phase == RaceHud::kCountdown) {
    // The event's card, top centre.
    const float cw = 300 * s;
    panel(l, 0.5f * w - 0.5f * cw, y0, 0.5f * w + 0.5f * cw, y0 + 66 * s, s);
    l.text(0.5f * w, y0 + 14 * s, r.event,
           TextStyle{.size = 20 * s, .weight = 0.12f, .color = kText, .align = Align::kCenter});
    char sub[64];
    if (r.stage) std::snprintf(sub, sizeof sub, "RALLY STAGE");
    else if (r.circuit) std::snprintf(sub, sizeof sub, "CIRCUIT   %d LAPS   %d CARS", r.laps, r.cars);
    else std::snprintf(sub, sizeof sub, "SPRINT   %d CARS", r.cars);
    label(l, 0.5f * w, y0 + 44 * s, sub, s, kDim, Align::kCenter);
  }
  if (r.phase == RaceHud::kCountdown && r.digit > 0) {
    // 3, 2, 1: a digit over a ring that empties with the second.
    const float k = float(r.clock - std::floor(r.clock));  // 0 at the second's start
    const float cx = 0.5f * w, cy = 0.42f * h, R = 62 * s;
    const uint32_t c = r.digit == 3 ? kRed : kAccent;
    l.circle(cx, cy, R + 12 * s, rgba(8, 10, 14, 170));
    l.arc(cx, cy, R, 8 * s, -3.1416f, -3.1416f + 6.2832f * (1 - k), c);
    char d[4];
    std::snprintf(d, sizeof d, "%d", r.digit);
    const float pop = 1 + 0.25f * (1 - ease_in_time(k, 0, 0.25));
    l.text(cx, cy - 32 * s * pop, d,
           TextStyle{.size = 64 * s * pop, .weight = 0.13f, .slant = 0.1f, .color = kText, .align = Align::kCenter});
  }
  if (r.phase == RaceHud::kRacing && r.clock < 1.5) {
    const float a = 1 - clamp01((r.clock - 0.9) / 0.6);
    const float grow = 1 + 0.3f * float(r.clock / 1.5);
    l.text(0.5f * w, 0.42f * h - 36 * s * grow, "GO!",
           TextStyle{.size = 72 * s * grow, .weight = 0.14f, .slant = 0.14f, .color = ui::fade(kGreen, a),
                     .align = Align::kCenter});
  }
  if (r.phase == RaceHud::kRacing) {
    // Position.
    const float pw = 132 * s, ph = 86 * s;
    panel(l, x0, y0, x0 + pw, y0 + ph, s);
    label(l, x0 + 16 * s, y0 + 14 * s, "POSITION", s);
    char p[8], of[8];
    std::snprintf(p, sizeof p, "%d", r.place);
    std::snprintf(of, sizeof of, "/%d", r.cars);
    const TextStyle big{.size = 40 * s, .weight = 0.13f, .slant = 0.12f, .color = kText};
    const float pwid = l.text(x0 + 16 * s, y0 + 32 * s, p, big);
    l.text(x0 + 22 * s + pwid, y0 + 52 * s, of, TextStyle{.size = 18 * s, .weight = 0.12f, .color = kDim});
    // Lap or progress, and time.
    const float lx = x0 + pw + 10 * s, lw = 168 * s;
    panel(l, lx, y0, lx + lw, y0 + ph, s);
    if (r.circuit) {
      label(l, lx + 16 * s, y0 + 14 * s, "LAP", s);
      char lap[16];
      std::snprintf(lap, sizeof lap, "%d/%d", std::max(1, r.lap), r.laps);
      l.text(lx + 16 * s, y0 + 30 * s, lap, TextStyle{.size = 20 * s, .weight = 0.12f, .color = kText});
    } else {
      label(l, lx + 16 * s, y0 + 14 * s, r.stage ? "STAGE" : "PROGRESS", s);
      const float bx0 = lx + 16 * s, bx1 = lx + lw - 16 * s, by = y0 + 38 * s;
      l.rect(bx0, by, bx1, by + 6 * s, rgba(255, 255, 255, 40), 3 * s);
      l.rect(bx0, by, bx0 + std::max(6 * s, (bx1 - bx0) * float(r.progress)), by + 6 * s, kAccent, 3 * s);
    }
    l.text(lx + 16 * s, y0 + 58 * s, race_clock(r.clock),
           TextStyle{.size = 15 * s, .weight = 0.11f, .color = kText});
    if (r.circuit && r.best_lap > 0) {
      label(l, lx + lw - 16 * s, y0 + 14 * s, "BEST", s, kDim, Align::kRight);
      l.text(lx + lw - 16 * s, y0 + 30 * s, race_clock(r.best_lap),
             TextStyle{.size = 11 * s, .weight = 0.11f, .color = kDim, .align = Align::kRight});
    }
    // The standings, below.
    const float ry = y0 + ph + 10 * s, rh = 21 * s, rw = pw + 10 * s + lw;
    const size_t rows = std::min<size_t>(r.rows.size(), 8);
    panel(l, x0, ry, x0 + rw, ry + 10 * s + rh * float(rows), s, rgba(10, 12, 17, 160));
    for (size_t i = 0; i < rows; ++i) {
      const auto& row = r.rows[i];
      const float y = ry + 5 * s + rh * float(i);
      if (row.player) l.rect(x0 + 5 * s, y, x0 + rw - 5 * s, y + rh, ui::fade(kAccent, 0.9), 5 * s);
      const uint32_t ink = row.player ? kInk : kText;
      char n[8];
      std::snprintf(n, sizeof n, "%d", row.place);
      l.text(x0 + 24 * s, y + 6 * s, n, TextStyle{.size = 9 * s, .weight = 0.12f, .color = row.player ? kInk : kDim,
                                                    .align = Align::kRight});
      TextStyle name{.size = 9 * s, .weight = 0.1f, .color = ink};
      std::string text = row.name;
      const float room = rw - (row.finished ? 90 : 46) * s;
      while (name.size > 7.5f * s && ui::List::measure(text, name) > room) name.size *= 0.95f;
      // Still too long: whole words, then a cut.
      while (ui::List::measure(text, name) > room && text.find(' ') != std::string::npos) text.erase(text.rfind(' '));
      while (text.size() > 4 && ui::List::measure(text, name) > room) text.pop_back();
      l.text(x0 + 34 * s, y + 6 * s, text, name);
      if (row.finished)
        l.text(x0 + rw - 14 * s, y + 6 * s, race_clock(row.time),
               TextStyle{.size = 9 * s, .weight = 0.1f, .color = row.player ? kInk : kDim, .align = Align::kRight});
    }
  }
  if (r.phase == RaceHud::kFinished) {
    // The results card, sliding up.
    const double since = r.clock - r.finish_time;
    const float in = ease_in_time(since, 0, 0.5);
    const float cw = 520 * s;
    const size_t rows = std::min<size_t>(r.rows.size(), 8);
    const bool band = r.band_after >= 0;
    const float ch = 214 * s + 24 * s * float(rows) + (band ? 62 * s : 0);
    const float cx0 = 0.5f * w - 0.5f * cw, cy0 = 0.5f * h - 0.5f * ch + (1 - in) * 40 * s;
    l.rect(0, 0, w, h, rgba(0, 0, 0, uint8_t(90 * in)));
    panel(l, cx0, cy0, cx0 + cw, cy0 + ch, s, ui::fade(kPanelLow, in));
    label(l, cx0 + 28 * s, cy0 + 22 * s, r.stage ? "STAGE COMPLETE" : "RACE COMPLETE", s);
    char place[16];
    if (r.stage) std::snprintf(place, sizeof place, "%d%s", r.stage_place, place_suffix(r.stage_place));
    else std::snprintf(place, sizeof place, "%d%s", r.place, place_suffix(r.place));
    l.text(cx0 + 28 * s, cy0 + 40 * s, place,
           TextStyle{.size = 44 * s, .weight = 0.13f, .slant = 0.12f, .color = r.place == 1 ? kAccent : kText});
    l.text(cx0 + cw - 28 * s, cy0 + 46 * s, race_clock(r.finish_time),
           TextStyle{.size = 24 * s, .weight = 0.12f, .color = kText, .align = Align::kRight});
    if (r.stage) {
      char of[48];
      std::snprintf(of, sizeof of, "OF %d DRIVERS", r.stage_field);
      label(l, cx0 + cw - 28 * s, cy0 + 80 * s, of, s, kDim, Align::kRight);
    }
    const float ty = cy0 + 108 * s;
    for (size_t i = 0; i < rows; ++i) {
      const auto& row = r.rows[i];
      const float y = ty + 24 * s * float(i);
      const float a = ease_in_time(since, 0.25 + 0.06 * double(i), 0.3);
      if (row.player) l.rect(cx0 + 18 * s, y - 5 * s, cx0 + cw - 18 * s, y + 17 * s, ui::fade(kAccent, a), 5 * s);
      const uint32_t ink = ui::fade(row.player ? kInk : kText, a);
      char n[8];
      std::snprintf(n, sizeof n, "%d", row.place);
      l.text(cx0 + 44 * s, y, n, TextStyle{.size = 11 * s, .weight = 0.12f, .color = ink, .align = Align::kRight});
      TextStyle ns{.size = 11 * s, .weight = 0.1f, .color = ink};
      const float room = cw - (58 + 32 + 84) * s;
      while (ns.size > 8 * s && ui::List::measure(row.name, ns) > room) ns.size *= 0.95f;
      l.text(cx0 + 58 * s, y + (11 * s - ns.size) * 0.5f, row.name, ns);
      l.text(cx0 + cw - 32 * s, y, row.finished ? race_clock(row.time) : "--",
             TextStyle{.size = 11 * s, .weight = 0.1f, .color = ink, .align = Align::kRight});
    }
    // The rewards, counting up.
    const float ry = ty + 24 * s * float(rows) + 14 * s;
    l.line(cx0 + 28 * s, ry, cx0 + cw - 28 * s, ry, 1.5f * s, rgba(255, 255, 255, uint8_t(40 * in)));
    const float count = ease_in_time(since, 0.8, 1.2);
    std::string cr = "+" + thousands(int64_t(std::llround(double(r.credits) * count))) + " CR";
    l.circle(cx0 + 38 * s, ry + 26 * s, 9 * s, ui::fade(kAccent, in));
    l.text(cx0 + 38 * s, ry + 21 * s, "C", TextStyle{.size = 10 * s, .weight = 0.15f, .color = kInk, .align = Align::kCenter});
    l.text(cx0 + 56 * s, ry + 17 * s, cr, TextStyle{.size = 18 * s, .weight = 0.12f, .color = ui::fade(kText, in)});
    if (r.wristband > 0) {
      char wb[32];
      std::snprintf(wb, sizeof wb, "+%d WRISTBAND", r.wristband);
      l.text(cx0 + cw - 28 * s, ry + 19 * s, wb,
             TextStyle{.size = 13 * s, .weight = 0.12f, .color = ui::fade(kAccent, in), .align = Align::kRight});
    }
    if (band) wristband_row(l, r, cx0 + 28 * s, ry + 52 * s, cw - 56 * s, s, since, in);
    if (band && r.band_up) {
      // A new band: a banner over the card.
      const float a = ease_in_time(since, 2.3, 0.35);
      const float pop = 1 + 0.2f * (1 - ease_in_time(since, 2.3, 0.5));
      const std::string text = "NEW WRISTBAND: " + r.band_name;
      const TextStyle bs{.size = 18 * s * pop, .weight = 0.13f, .slant = 0.1f, .color = ui::fade(kInk, a),
                         .align = Align::kCenter};
      const float bw = ui::List::measure(text, bs) + 48 * s, by = cy0 - 58 * s;
      l.shadow(0.5f * w - 0.5f * bw, by + 3 * s, 0.5f * w + 0.5f * bw, by + 43 * s, ui::fade(kShadow, a), 10 * s, 12 * s);
      l.rect(0.5f * w - 0.5f * bw, by, 0.5f * w + 0.5f * bw, by + 40 * s, ui::fade(band_colour(r.band_name), a), 8 * s);
      l.text(0.5f * w, by + 20 * s - 0.5f * bs.size, text, bs);
    }
    label(l, 0.5f * w, cy0 + ch - 22 * s, "A / ENTER CONTINUE      Y RESTART", s, ui::fade(kDim, in), Align::kCenter);
  }
}

void menu_panel(ui::List& l, const DriveHud& st, float w, float h, float s) {
  l.rect(0, 0, w, h, rgba(0, 0, 0, 80));
  const float pw = std::min(w, 440 * s);
  l.rect(0, 0, pw, h, rgba(9, 11, 15, 238), 0, rgba(9, 11, 15, 210));
  l.rect(pw - 2 * s, 0, pw, h, ui::fade(kAccent, 0.5));
  const float x = 40 * s;
  float y = 44 * s;
  // The title: the part before ":" large, the rest (a page's hint) small.
  std::string title = st.menu_title, hint;
  if (auto c = title.find(':'); c != std::string::npos) {
    hint = title.substr(c + 1);
    title = title.substr(0, c);
    while (!hint.empty() && hint.front() == ' ') hint.erase(hint.begin());
  }
  l.rect(x, y, x + 5 * s, y + 24 * s, kAccent, 2 * s);
  TextStyle ts{.size = 24 * s, .weight = 0.12f, .slant = 0.1f, .color = kText};
  while (ts.size > 12 * s && ui::List::measure(title, ts) > pw - x - 40 * s) ts.size *= 0.92f;
  l.text(x + 18 * s, y, title, ts);
  y += 50 * s;
  if (!hint.empty()) {
    TextStyle hs{.size = 8.5f * s, .weight = 0.11f, .tracking = 0.15f, .color = kDim};
    while (hs.size > 6 * s && ui::List::measure(hint, hs) > pw - x - 30 * s) hs.size *= 0.92f;
    l.text(x, y, hint, hs);
    y += 24 * s;
  }
  y += 10 * s;
  // The items, scrolled so the selected one shows.
  const float row = 36 * s;
  const int fit = std::max(1, int((h - y - 70 * s) / row));
  const int n = int(st.menu.size());
  const int first = std::clamp(st.menu_selected - fit / 2, 0, std::max(0, n - fit));
  for (int i = first; i < n && i < first + fit; ++i) {
    const bool sel = i == st.menu_selected;
    const float ry = y + row * float(i - first);
    if (sel) {
      l.rect(x - 14 * s, ry - 9 * s, pw - 24 * s, ry + 25 * s, kAccent, 6 * s);
    }
    TextStyle it{.size = 15 * s, .weight = 0.11f, .color = sel ? kInk : kText};
    while (it.size > 8 * s && ui::List::measure(st.menu[size_t(i)], it) > pw - x - 44 * s) it.size *= 0.93f;
    l.text(x, ry + (15 * s - it.size) * 0.5f, st.menu[size_t(i)], it);
  }
  if (first > 0) label(l, pw - 40 * s, y - 16 * s, "MORE", s, kDim, Align::kRight);
  if (first + fit < n) label(l, pw - 40 * s, h - 66 * s, "MORE", s, kDim, Align::kRight);
  label(l, x, h - 40 * s, st.menu_footer.c_str(), s, kDim);
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

float drive_hud_dial_height(float h) { return (26 + 2 * 80 + 16) * (h / 720.0f); }

void build_drive_hud(ui::List& l, const DriveHud& st, float w, float h) {
  const float s = h / 720.0f;
  const bool menu = !st.menu_title.empty() || !st.menu.empty();
  if (st.driving) {
    if (st.race) race_panels(l, *st.race, w, h, s, st.time);
    else if (st.credits >= 0 && !menu) career_chip(l, st, w, s);
    if (!menu) popup_card(l, st, w, s);
    if (!(st.race && st.race->phase == RaceHud::kFinished)) dial(l, st, w, h, s);
  }
  if (!st.toast.empty() && !menu) {
    TextStyle ts{.size = 10 * s, .weight = 0.11f, .tracking = 0.08f, .color = kText, .align = Align::kCenter};
    while (ts.size > 7 * s && ui::List::measure(st.toast, ts) > w - 120 * s) ts.size *= 0.93f;
    const float tw = ui::List::measure(st.toast, ts);
    const float y = h - 52 * s;
    panel(l, 0.5f * w - 0.5f * tw - 18 * s, y, 0.5f * w + 0.5f * tw + 18 * s, y + 30 * s, s);
    l.text(0.5f * w, y + 15 * s - 0.5f * ts.size, st.toast, ts);
  }
  if (menu) menu_panel(l, st, w, h, s);
}

}  // namespace carview
