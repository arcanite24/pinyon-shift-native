#include "game_hud_internal.h"
#include "pause_screen.h"
#include "touch_controls.h"

#include <cstdlib>

namespace carview {

using namespace hud_detail;

namespace {

uint32_t assist_badge_color(AssistBadgeState state, double time_s) {
  switch (state) {
    case AssistBadgeState::kDisabled:
      return ui::fade(rgba(110, 114, 122), 0.34f);
    case AssistBadgeState::kEnabledIdle:
      return rgba(244, 246, 252, 238);
    case AssistBadgeState::kIntervening:
      return assist_blink_on(time_s) ? rgba(255, 168, 28, 255) : rgba(255, 168, 28, 86);
  }
  return ui::fade(rgba(110, 114, 122), 0.34f);
}

void draw_assist_badge(ui::List& l, float cx, float y, float s, const char* label, AssistBadgeState state,
                       double time_s, bool styled_chip = false) {
  const bool blinking = (state == AssistBadgeState::kIntervening) && assist_blink_on(time_s);
  if (state == AssistBadgeState::kIntervening || styled_chip) {
    const float hw = 12.5f * s;
    const uint32_t bg = blinking ? rgba(255, 152, 24, 62)
                                 : (state == AssistBadgeState::kIntervening ? rgba(18, 20, 26, 155)
                                                                            : rgba(14, 16, 22, 115));
    l.rect(cx - hw, y - 1.8f * s, cx + hw, y + 10.6f * s, bg);
    if (blinking) {
      l.rect(cx - hw, y + 9.2f * s, cx + hw, y + 10.6f * s, rgba(255, 178, 36, 245));
    }
  }
  const uint32_t col = assist_badge_color(state, time_s);
  TextStyle ats = font(ui::Font::kB, 8.6f * s, col, Align::kCenter);
  l.text(cx, y, label, ats);
}

// Shared 1:1 FH1 gauge renderer matching out/oracle/ui/race-hud.png and
// out/oracle/ui/hud-freeroam-event-marker.png: translucent smoky dial backing,
// radial-gradient segmented pink redline band, always-visible glowing pink
// trailing arc sweep + crisp white-cored magenta needle, clean white ticks and
// numerals (0 2 4 6 8 10), sleek italicized speed digits with faint leading
// zeros, gear indicator, and etched ABS/TCS indicators.
void render_fh1_dial(ui::List& l, const DriveHud& st, float cx, float cy, float s,
                     const SceneDraw* sc, const SceneFrame* sf, int gauge_node, int ring_node) {
  constexpr float kSweep = 2.339f;  // +/-134 deg matching 947_HUD GAUGE_ANALOG
  constexpr uint32_t kNeedlePink = rgba(249, 0, 100);
  constexpr uint32_t kNeedleHot = rgba(255, 36, 126);
  constexpr uint32_t kNeedleCore = rgba(255, 232, 244, 245);

  // 1. Translucent smoky dial backing (never solid opaque black — lets road softly show through).
  std::array<float, 4> ring_uv{};
  if (sc && sf && ring_node >= 0) {
    sc->model(l, *sf, ring_node, std::clamp(sc->alpha(ring_node) * 0.78f, 0.56f, 0.78f));
  } else if (ui_texture("HUD/DIALS/MAINBACKRING", ring_uv)) {
    l.image(cx - 128 * s, cy - 128 * s, cx + 128 * s, cy + 128 * s, ring_uv, rgba(255, 255, 255, 198));
  } else {
    l.circle(cx, cy, 88 * s, rgba(16, 18, 22, 152));
    l.circle(cx, cy, 86 * s, rgba(32, 34, 40, 72));
    l.arc(cx, cy, 86.5f * s, 1.5f * s, -kSweep, kSweep, rgba(255, 255, 255, 115));
  }

  const double red = std::max(1000.0, st.redline_rpm);
  const double top = std::max(10000.0, std::ceil(red * 1.1 / 2000.0) * 2000.0);
  const bool at_redline = st.gear > 0 && st.rpm >= red * 0.92;
  const float red_pulse = at_redline ? (0.5f + 0.5f * std::sin(float(st.time) * 6.2831853f * 9.0f)) : 0.0f;
  auto angle = [&](double rpm) { return -kSweep + 2.0f * kSweep * float(std::clamp(rpm / top, 0.0, 1.0)); };

  // 2. Smooth radial-gradient redline zone with bright outer rim + subtle hatch segments.
  const float a_red = angle(red);
  if (kSweep > a_red + 0.01f) {
    l.arc(cx, cy, 65.5f * s, 9.0f * s, a_red, kSweep, ui::fade(kNeedlePink, 0.16 + 0.18 * red_pulse), true);
    l.arc(cx, cy, 73.0f * s, 8.5f * s, a_red, kSweep, ui::fade(kNeedlePink, 0.34 + 0.24 * red_pulse), true);
    l.arc(cx, cy, 80.0f * s, 8.0f * s, a_red, kSweep, ui::fade(kNeedleHot, 0.58 + 0.32 * red_pulse), true);
    l.arc(cx, cy, 84.8f * s, 3.2f * s, a_red, kSweep, ui::fade(kNeedleHot, 0.94), true);
    l.arc(cx, cy, 86.5f * s, 1.3f * s, a_red, kSweep, rgba(255, 130, 186, 235), true);
    // Crisp segmented hatch cuts across redline band (every 250 RPM).
    for (double k = std::ceil(red / 250.0) * 250.0; k < top - 1.0; k += 250.0) {
      const float ha = angle(k), sa = std::sin(ha), ca = std::cos(ha);
      l.line(cx + 64 * s * sa, cy - 64 * s * ca, cx + 84.5f * s * sa, cy - 84.5f * s * ca, 1.1f * s,
             rgba(18, 12, 18, 85));
    }
  }
  // R3-UI-11: Redline shift flash pulsing tachometer outer rim when RPM >= 92% of redline.
  if (at_redline) {
    l.arc(cx, cy, 88.2f * s, 3.2f * s, -kSweep, kSweep, ui::fade(kNeedleHot, 0.28f + 0.65f * red_pulse));
    l.arc(cx, cy, 86.4f * s, 1.4f * s, a_red, kSweep, ui::fade(rgba(255, 220, 236), 0.35f + 0.60f * red_pulse), true);
  }

  // 3. Glowing translucent Horizon pink trailing arc sweep behind the needle
  //    (always softly visible behind the needle, expanding smoothly under rev acceleration).
  const float a = angle(st.rpm);
  const float sa = std::sin(a), ca = std::cos(a);
  {
    const float accel_boost = clamp01(st.rpm_rate / 5000.0);
    const float base_span = std::min(a - (-kSweep), 0.46f + 0.30f * accel_boost);
    if (base_span > 0.02f) {
      constexpr int kSteps = 10;
      for (int i = 0; i < kSteps; ++i) {
        const float t0 = float(i) / float(kSteps);
        const float t1 = float(i + 1) / float(kSteps);
        const float seg_a0 = a - base_span * (1.0f - t0);
        const float seg_a1 = a - base_span * (1.0f - t1);
        const float strength = t1 * t1;  // smooth quadratic ramp up to the needle
        l.arc(cx, cy, 69.5f * s, 19.0f * s, seg_a0, seg_a1, ui::fade(kNeedlePink, 0.34 * strength), true);
        l.arc(cx, cy, 80.0f * s, 11.5f * s, seg_a0, seg_a1, ui::fade(kNeedleHot, 0.58 * strength), true);
        l.arc(cx, cy, 85.4f * s, 2.6f * s, seg_a0, seg_a1, ui::fade(rgba(255, 134, 188), 0.84 * strength), true);
      }
    }
  }

  // 4. Crisp white major/half ticks and numerals (0, 2, 4, 6, 8, 10) OVER redline & trail.
  std::array<float, 4> large_uv{}, small_uv{};
  const bool have_hatches =
      ui_texture("HUD/DIALS/HATCHLARGE", large_uv) && ui_texture("HUD/DIALS/HATCHSMALL", small_uv);
  const TextStyle num_ts = font(ui::Font::kB, 11.2f * s, rgba(245, 246, 250, 245), Align::kCenter);
  const TextStyle num_sh = font(ui::Font::kB, 11.2f * s, rgba(0, 0, 0, 145), Align::kCenter);
  for (double k = 0; k <= top + 1.0; k += 500.0) {
    const int q = int(std::lround(k / 250.0));
    const bool is_large = (q % 4 == 0);
    const float ta = angle(k), tsa = std::sin(ta), tca = std::cos(ta);
    if (have_hatches) {
      const float hw = (is_large ? 5.8f : 5.2f) * s;
      const float hh = (is_large ? 4.4f : 3.6f) * s;
      l.image_rotated(cx + 82.0f * s * tsa, cy - 82.0f * s * tca, hw, hh, ta, is_large ? large_uv : small_uv,
                      rgba(255, 255, 255, uint8_t(is_large ? 245 : 168)));
    } else {
      const float r0 = (is_large ? 78.5f : 81.0f) * s;
      const float r1 = 86.2f * s;
      l.line(cx + r0 * tsa, cy - r0 * tca, cx + r1 * tsa, cy - r1 * tca, (is_large ? 2.2f : 1.4f) * s,
             rgba(255, 255, 255, uint8_t(is_large ? 245 : 175)));
    }
    if (!is_large || (q / 4) % 2 != 0) continue;
    char d[8];
    std::snprintf(d, sizeof d, "%d", q / 4);
    const float nx = cx + 68.5f * s * tsa;
    const float ny = cy - 68.5f * s * tca - 0.5f * num_ts.size;
    l.text(nx + 0.8f * s, ny + 1.0f * s, d, num_sh);
    l.text(nx, ny, d, num_ts);
  }

  // 5. High-visibility glowing Horizon pink needle bar with crisp white-pink core
  //    (always legible even when pegged deep in the redline zone!).
  std::array<float, 4> nuv{};
  if (ui_texture("HUD/DIALS/NEEDLE", nuv)) {
    l.image_rotated(cx + 66 * s * sa, cy - 66 * s * ca, 13 * s, 54 * s, a, nuv, kNeedleHot);
  }
  l.line(cx + 61.0f * s * sa, cy - 61.0f * s * ca, cx + 87.2f * s * sa, cy - 87.2f * s * ca, 5.2f * s,
         rgba(249, 0, 100, 135));
  l.line(cx + 61.5f * s * sa, cy - 61.5f * s * ca, cx + 87.0f * s * sa, cy - 87.0f * s * ca, 2.8f * s, kNeedleHot);
  l.line(cx + 63.0f * s * sa, cy - 63.0f * s * ca, cx + 86.5f * s * sa, cy - 86.5f * s * ca, 1.25f * s, kNeedleCore);

  // 6. Unit label ("km/h" or "mph" per global Units setting), sleek italicized 3-digit speedo,
  //    gear digit, and etched ABS / TCS indicators matching race-hud.png & hud-freeroam-event-marker.png.
  TextStyle unit_ts = font(ui::Font::kA, 9.5f * s, rgba(218, 222, 230, 175), Align::kCenter);
  unit_ts.keep_case = true;
  const std::string unit_lbl = st.metric_units
                                   ? format_unit_label(str("UnitStrings", "IDS_Units_KMH", "km/h"), "km/h")
                                   : format_unit_label(str("UnitStrings", "IDS_Units_MPH", "mph"), "mph");
  l.text(cx, cy - 35.0f * s, unit_lbl, unit_ts);

  const double disp_spd = st.metric_units ? std::fabs(st.kmh) : (std::fabs(st.kmh) * 0.62137119);
  const int v = std::min(999, int(std::lround(disp_spd)));
  char digits[8];
  std::snprintf(digits, sizeof digits, "%03d", v);
  const size_t lead = v >= 100 ? 0 : v >= 10 ? 1 : 2;
  const float slot_w = 20.5f * s;
  const float speed_y = cy - 22.5f * s;
  for (int i = 0; i < 3; ++i) {
    const float slot_cx = cx + float(i - 1) * slot_w;
    const bool is_lead = size_t(i) < lead;
    TextStyle dts = font(ui::Font::kE, 29.0f * s, is_lead ? rgba(235, 238, 244, 68) : rgba(250, 251, 255, 255),
                         Align::kCenter);
    dts.stretch = 0.86f;
    const std::string ch(1, digits[i]);
    if (!is_lead) {
      TextStyle sh = dts;
      sh.color = rgba(0, 0, 0, 155);
      l.text(slot_cx + 1.2f * s, speed_y + 1.5f * s, ch, sh);
    }
    l.text(slot_cx, speed_y, ch, dts);
  }

  const char* gear = st.gear < 0 ? "R" : st.gear == 0 ? "N" : nullptr;
  char gb[8];
  if (!gear) std::snprintf(gb, sizeof gb, "%d", st.gear), gear = gb;
  if (at_redline) {
    l.circle(cx - 24.0f * s, cy + 28.0f * s, 12.0f * s, ui::fade(kNeedleHot, 0.25f + 0.55f * red_pulse));
  }
  const uint32_t gear_col = at_redline ? (red_pulse > 0.5f ? rgba(255, 68, 132, 255) : rgba(255, 240, 246, 255))
                                       : rgba(238, 240, 245, 245);
  TextStyle gts = font(ui::Font::kE, 18.0f * s, gear_col, Align::kCenter);
  gts.stretch = 0.92f;
  TextStyle gsh = gts;
  gsh.color = rgba(0, 0, 0, 145);
  l.text(cx - 24.0f * s + 1.0f * s, cy + 19.0f * s + 1.2f * s, gear, gsh);
  l.text(cx - 24.0f * s, cy + 19.0f * s, gear, gts);

  // R3-UI-03: Tri-state ABS / TCS / STM badges (OFF dimmed grey, ON steady white, Intervening ~7 Hz amber blink)
  // aligned cleanly below the speed digits and to the right of the gear indicator with zero overlap.
  const AssistBadgeState abs_st = compute_assist_badge_state(st.abs_enabled, st.abs, st.abs_flash_timer);
  const AssistBadgeState tcs_st = compute_assist_badge_state(st.tcs_enabled, st.tcs, st.tcs_flash_timer);
  const AssistBadgeState stm_st = compute_assist_badge_state(st.stm_enabled, st.stm, st.stm_flash_timer);

  bool drew_assists = false;
  if (sc && sf && gauge_node >= 0) {
    const int assists = sc->find("ASSIST_ICONS", gauge_node);
    if (assists >= 0) {
      for (const auto& [name, state] : {std::pair{"HUD_ABS", abs_st}, std::pair{"HUD_TCS", tcs_st}}) {
        const int on = sc->find("Rectangle_ON", sc->find(name, assists));
        const uint32_t rgb = (state == AssistBadgeState::kDisabled)
                                 ? rgba(110, 114, 122)
                                 : (state == AssistBadgeState::kEnabledIdle ? rgba(232, 236, 244)
                                                                            : rgba(255, 168, 28));
        const float alpha = (state == AssistBadgeState::kDisabled)
                                ? 0.36f
                                : (state == AssistBadgeState::kEnabledIdle ? 0.92f
                                                                           : (assist_blink_on(st.time) ? 1.0f : 0.34f));
        if (on >= 0 && sc->model(l, *sf, on, alpha, 0, 0, 0, rgb)) {
          drew_assists = true;
        }
      }
    }
  }
  if (!drew_assists) {
    draw_assist_badge(l, cx + 1.0f * s, cy + 24.5f * s, s, "ABS", abs_st, st.time, false);
    draw_assist_badge(l, cx + 23.0f * s, cy + 24.5f * s, s, "TCS", tcs_st, st.time, false);
    draw_assist_badge(l, cx + 45.0f * s, cy + 24.5f * s, s, "STM", stm_st, st.time, true);
  } else {
    draw_assist_badge(l, cx + 46.0f * s, cy + 24.5f * s, s, "STM", stm_st, st.time, true);
  }

  // Optional Forced-Induction Boost mini-bar (BAR when metric, PSI when imperial) tucked neatly below gear/assist row.
  if (st.has_boost && st.boost_max_psi > 0.5) {
    const float bf = float(std::clamp(st.boost_psi / st.boost_max_psi, 0.0, 1.0));
    const float bx0 = cx - 32.0f * s, bx1 = cx + 32.0f * s, by = cy + 44.0f * s;
    l.rect(bx0, by, bx1, by + 4.5f * s, rgba(16, 18, 22, 165));
    if (bf > 0.01f) {
      l.rect(bx0, by, bx0 + (bx1 - bx0) * bf, by + 4.5f * s, bf > 0.85f ? kNeedleHot : rgba(48, 206, 255, 240));
    }
    const std::string btxt = format_boost_unit(st.boost_psi, st.metric_units);
    l.text(cx, by + 6.5f * s, btxt, font(ui::Font::kB, 8.2f * s, rgba(210, 226, 242, 220), Align::kCenter));
  }
}

// Crisp Digital HUD tach/speedo mode (FH1 Settings -> HUD Speedometer: Digital)
// with authentic FH1 angled chrome backing, dimmed leading zeros, redline shift
// flash, and tri-state ~7 Hz blinking ABS / TCS / STM badges.
void render_digital_dial(ui::List& l, const DriveHud& st, const Layout& g) {
  float s = g.s;
  float rx = g.R(1214), by = g.B(668);
  if (st.touch_hud_active) {
    TouchRect mm_r, tach_r;
    TouchControls::hud_safe_rects(g.w, g.h, mm_r, tach_r);
    rx = tach_r.x1 * g.w;
    by = tach_r.y1 * g.h;
  }
  const float bw = 224.0f * s, bh = 90.0f * s;
  const float x0 = rx - bw, y0 = by - bh;

  const double red = std::max(1000.0, st.redline_rpm);
  const double top = std::max(10000.0, std::ceil(red * 1.1 / 2000.0) * 2000.0);
  const bool at_redline = st.gear > 0 && st.rpm >= red * 0.92;
  const float red_pulse = at_redline ? (0.5f + 0.5f * std::sin(float(st.time) * 6.2831853f * 9.0f)) : 0.0f;

  Turn tilt(l, 0.5f * (x0 + rx), 0.5f * (y0 + by), ui4::kTiltRad * 0.38f);
  // FH1 angled dark chrome backing with soft shadow, sheen edge & pink accent stripe.
  l.shadow(x0, y0 + 2.0f * s, rx, by + 3.0f * s, rgba(0, 0, 0, 155), 1.5f * s, 8.0f * s);
  l.rect(x0, y0, rx, by, rgba(14, 16, 22, 212));
  l.rect(x0, y0, rx, y0 + 1.2f * s, rgba(255, 255, 255, 52));
  l.rect(x0, y0, x0 + 4.0f * s, by, at_redline ? ui::fade(ui4::kPinkHi, 0.65f + 0.35f * red_pulse) : ui4::kPink);
  if (at_redline) {
    l.frame(x0, y0, rx, by, ui::fade(ui4::kPinkHi, 0.35f + 0.60f * red_pulse), 2.0f * s);
  }

  // Segmented RPM bar along top edge with redline shift flash.
  const float rpm_f = float(std::clamp(st.rpm / top, 0.0, 1.0));
  const float red_f = float(std::clamp(red / top, 0.0, 1.0));
  constexpr int kSegs = 24;
  const float seg_w = (bw - 20.0f * s) / float(kSegs);
  for (int i = 0; i < kSegs; ++i) {
    const float t = float(i + 1) / float(kSegs);
    const float sx0 = x0 + 11.0f * s + float(i) * seg_w;
    const bool lit = rpm_f >= t - (0.5f / float(kSegs));
    const bool is_red = t >= red_f;
    uint32_t col;
    if (lit) {
      if (at_redline && red_pulse > 0.45f) {
        col = is_red ? rgba(255, 56, 132, 255) : rgba(255, 118, 168, 250);
      } else {
        col = is_red ? rgba(249, 0, 100, 245) : rgba(245, 247, 252, 242);
      }
    } else {
      col = is_red ? rgba(110, 18, 52, 110) : rgba(72, 76, 86, 95);
    }
    l.rect(sx0, y0 + 8.0f * s, sx0 + seg_w - 2.0f * s, y0 + 20.0f * s, col);
  }

  // 3-digit speedometer with dimmed leading zeros (e.g. dimmed "00" + bright "45").
  const double disp_spd = st.metric_units ? std::fabs(st.kmh) : (std::fabs(st.kmh) * 0.62137119);
  const int v = std::min(999, int(std::lround(disp_spd)));
  char digits[8];
  std::snprintf(digits, sizeof digits, "%03d", v);
  const size_t lead = v >= 100 ? 0 : v >= 10 ? 1 : 2;
  const float slot_w = 24.0f * s;
  const float digits_right = rx - 56.0f * s;
  const float speed_y = y0 + 25.0f * s;
  for (int i = 0; i < 3; ++i) {
    const float slot_cx = digits_right - float(2 - i) * slot_w - 11.0f * s;
    const bool is_lead = size_t(i) < lead;
    TextStyle dts = font(ui::Font::kE, 35.0f * s, is_lead ? rgba(235, 238, 244, 66) : rgba(250, 251, 255, 255),
                         Align::kCenter);
    dts.stretch = 0.88f;
    const std::string ch(1, digits[i]);
    if (!is_lead) {
      TextStyle sh = dts;
      sh.color = rgba(0, 0, 0, 160);
      l.text(slot_cx + 1.2f * s, speed_y + 1.4f * s, ch, sh);
    }
    l.text(slot_cx, speed_y, ch, dts);
  }
  const std::string unit_lbl = st.metric_units ? "KM/H" : "MPH";
  l.text(rx - 56.0f * s, y0 + 66.0f * s, unit_lbl, font(ui::Font::kB, 10.5f * s, rgba(210, 214, 224, 208), Align::kRight));

  // Gear indicator box with redline shift flash.
  const char* gear = st.gear < 0 ? "R" : st.gear == 0 ? "N" : nullptr;
  char gb[8];
  if (!gear) std::snprintf(gb, sizeof gb, "%d", st.gear), gear = gb;
  const uint32_t gear_bg = at_redline ? (red_pulse > 0.5f ? rgba(255, 36, 98, 255) : rgba(255, 226, 238, 245))
                                      : rgba(249, 0, 100, 234);
  const uint32_t gear_fg = (at_redline && red_pulse <= 0.5f) ? ui4::kInk : rgba(255, 255, 255, 255);
  l.rect(rx - 46.0f * s, y0 + 28.0f * s, rx - 8.0f * s, y0 + 66.0f * s, gear_bg);
  l.text(rx - 27.0f * s, y0 + 35.0f * s, gear, font(ui::Font::kE, 24.0f * s, gear_fg, Align::kCenter));

  // Tri-state ABS / TCS / STM badges (~7 Hz amber blink on intervention) + Boost bar on left column.
  const AssistBadgeState abs_st = compute_assist_badge_state(st.abs_enabled, st.abs, st.abs_flash_timer);
  const AssistBadgeState tcs_st = compute_assist_badge_state(st.tcs_enabled, st.tcs, st.tcs_flash_timer);
  const AssistBadgeState stm_st = compute_assist_badge_state(st.stm_enabled, st.stm, st.stm_flash_timer);
  draw_assist_badge(l, x0 + 25.0f * s, y0 + 27.5f * s, s, "ABS", abs_st, st.time, false);
  draw_assist_badge(l, x0 + 25.0f * s, y0 + 41.0f * s, s, "TCS", tcs_st, st.time, false);
  draw_assist_badge(l, x0 + 25.0f * s, y0 + 54.5f * s, s, "STM", stm_st, st.time, true);

  if (st.has_boost && st.boost_max_psi > 0.5) {
    const float bf = float(std::clamp(st.boost_psi / st.boost_max_psi, 0.0, 1.0));
    const float bbx0 = x0 + 12.0f * s, bbx1 = x0 + 84.0f * s, bby = y0 + 70.0f * s;
    l.rect(bbx0, bby, bbx1, bby + 3.5f * s, rgba(255, 255, 255, 36));
    if (bf > 0.01f) {
      l.rect(bbx0, bby, bbx0 + (bbx1 - bbx0) * bf, bby + 3.5f * s,
             bf > 0.85f ? rgba(255, 36, 126, 245) : rgba(72, 212, 255, 240));
    }
    const std::string bp = format_boost_unit(st.boost_psi, st.metric_units);
    l.text(x0 + 12.0f * s, y0 + 75.5f * s, bp, font(ui::Font::kB, 8.8f * s, rgba(72, 212, 255, 235)));
  }
}

// (a) The free-roam dial fallback when 947_HUD is absent.
void dial_drawn(ui::List& l, const DriveHud& st, const Layout& g) {
  float s = g.s;
  float cx = g.R(1122), cy = g.B(587);
  if (st.touch_hud_active) {
    TouchRect mm_r, tach_r;
    TouchControls::hud_safe_rects(g.w, g.h, mm_r, tach_r);
    s = std::min(g.s, (tach_r.h() * g.h) / 184.0f);
    cx = tach_r.cx() * g.w;
    cy = tach_r.cy() * g.h;
  }
  render_fh1_dial(l, st, cx, cy, s, nullptr, nullptr, -1, -1);
}

// The dial anchored from 947_HUD's GAUGE_ANALOG (with 1:1 FH1 visual styling).
void dial(ui::List& l, const DriveHud& st, const Layout& g) {
  const SceneDraw sc = resolve_scene_draw("947_HUD");
  const int gauge = sc.find("GAUGE_ANALOG");
  const int ring = sc.find("MainBackRing", gauge);
  if (gauge < 0 || ring < 0) return dial_drawn(l, st, g);
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
  render_fh1_dial(l, st, cx, cy, s, &sc, &f, gauge, ring);
}

// High-contrast retail FH1 discovery/notification ribbon (PT-40):
// Dark translucent angled Horizon ribbon with bright magenta/pink accent stripe,
// crisp pure WHITE bold title, bright counter chip (e.g. "2 / 216"), crisp subtitle,
// animated progress fill bar, and smooth slide-in/fade-out animation.
// Avoids H_BARN_FIND_HUD's DoubleTape zero-alpha hidden nodes (which caused dark-on-dark
// unreadable text and a stray yellow rectangle above the banner).
bool popup_card(ui::List& l, const DriveHud& st, const Layout& g, float lift_canvas) {
  const double age = st.time - st.popup_at;
  if (st.popup_title.empty() || age < 0.0 || age > kPopupSeconds) return false;
  const float s = g.s;
  const float in = ease_in_time(age, 0.0, 0.28);
  const float out = 1.0f - clamp01((age - (kPopupSeconds - 0.5)) / 0.5);
  const float a = in * out;
  if (a <= 0.01f) return false;

  const PopupTitleParts parts = split_popup_title_counter(st.popup_title);
  const std::string& label_txt = parts.label.empty() ? st.popup_title : parts.label;
  const bool is_road_popup = label_txt.find("ROAD") != std::string::npos;
  const uint32_t accent =
      is_road_popup ? ui4::kPink : (st.popup_colour ? st.popup_colour : ui4::kPink);

  TextStyle title_ts = font(ui::Font::kE, 16.5f * s, ui::fade(ui4::kWhite, a));
  TextStyle cnt_ts = font(ui::Font::kE, 14.5f * s, ui::fade(ui4::kWhite, a), Align::kCenter);
  TextStyle body_ts = font(ui::Font::kB, 12.0f * s, ui::fade(rgba(224, 228, 238, 245), a));

  const float cnt_w = parts.counter.empty() ? 0.0f : (ui::List::measure(parts.counter, cnt_ts) + 18.0f * s);
  const float max_label_w = 340.0f * s - (cnt_w > 0.0f ? (cnt_w + 12.0f * s) : 0.0f);
  fit(label_txt, title_ts, std::max(140.0f * s, max_label_w), 10.5f * s);
  fit(st.popup_text, body_ts, 340.0f * s, 9.0f * s);

  const float label_w = ui::List::measure(label_txt, title_ts);
  const float body_w = st.popup_text.empty() ? 0.0f : ui::List::measure(st.popup_text, body_ts);
  const float top_row_w = label_w + (cnt_w > 0.0f ? (14.0f * s + cnt_w) : 0.0f);
  const float content_w = std::max(top_row_w, body_w);
  const float bw = std::clamp(content_w + 86.0f * s, 384.0f * s, 488.0f * s);

  const float cx = 0.5f * g.w;
  const float slide_y = (1.0f - in) * 18.0f * s;
  const float y0 = g.B(600.0f - lift_canvas) + slide_y;
  const float y1 = g.B(660.0f - lift_canvas) + slide_y;
  const float x0 = cx - 0.5f * bw;
  const float x1 = cx + 0.5f * bw;

  Turn tilt(l, cx, 0.5f * (y0 + y1), ui4::kTiltRad * 0.35f);

  // Soft drop shadow + high-contrast dark translucent Horizon ribbon backing (radius 0 + solid color turns as angled quad).
  l.shadow(x0, y0 + 2.0f * s, x1, y1 + 4.0f * s, ui::fade(rgba(0, 0, 0, 165), a), 2.0f * s, 10.0f * s);
  l.rect(x0, y0, x1, y1, ui::fade(rgba(12, 14, 20, 232), a));
  // Crisp top sheen hairline + bottom depth edge.
  l.rect(x0, y0, x1, y0 + 1.2f * s, ui::fade(rgba(255, 255, 255, 48), a));
  l.rect(x0, y1 - 1.5f * s, x1, y1, ui::fade(rgba(0, 0, 0, 110), a));
  // Bright Horizon magenta/pink left accent stripe.
  l.rect(x0, y0, x0 + 6.0f * s, y1, ui::fade(accent, a));
  l.rect(x0 + 6.0f * s, y0, x0 + 7.2f * s, y1, ui::fade(rgba(255, 255, 255, 85), a));

  // Crisp icon badge on left (clean vector road/discovery chevron — never an opaque yellow box).
  const float icx = x0 + 30.0f * s;
  const float icy = 0.5f * (y0 + y1) - (st.popup_progress >= 0.0 ? 2.0f * s : 0.0f);
  l.circle(icx, icy, 15.5f * s, ui::fade(rgba(255, 255, 255, 18), a));
  l.arc(icx, icy, 15.0f * s, 2.0f * s, 0.0f, 6.28318f, ui::fade(accent, a * 0.95f));
  if (is_road_popup) {
    // Crisp converging road perspective lanes + center dashes inside badge.
    l.line(icx - 7.5f * s, icy + 7.5f * s, icx - 2.6f * s, icy - 7.5f * s, 2.0f * s, ui::fade(ui4::kWhite, a));
    l.line(icx + 7.5f * s, icy + 7.5f * s, icx + 2.6f * s, icy - 7.5f * s, 2.0f * s, ui::fade(ui4::kWhite, a));
    l.line(icx, icy + 6.5f * s, icx, icy + 2.0f * s, 1.5f * s, ui::fade(accent, a));
    l.line(icx, icy - 1.5f * s, icx, icy - 5.5f * s, 1.5f * s, ui::fade(accent, a));
  } else {
    // Crisp double chevron badge for general notifications.
    l.line(icx - 5.0f * s, icy - 5.0f * s, icx + 2.0f * s, icy, 2.2f * s, ui::fade(ui4::kWhite, a));
    l.line(icx - 5.0f * s, icy + 5.0f * s, icx + 2.0f * s, icy, 2.2f * s, ui::fade(ui4::kWhite, a));
    l.line(icx + 1.0f * s, icy - 4.0f * s, icx + 6.5f * s, icy, 1.8f * s, ui::fade(accent, a));
    l.line(icx + 1.0f * s, icy + 4.0f * s, icx + 6.5f * s, icy, 1.8f * s, ui::fade(accent, a));
  }

  // High-contrast WHITE bold title + bright counter chip.
  const float tx = x0 + 56.0f * s;
  const float title_y = y0 + (st.popup_text.empty() ? 19.0f * s : 11.0f * s);
  {
    TextStyle sh = title_ts;
    sh.color = ui::fade(rgba(0, 0, 0, 200), a);
    l.text(tx + 1.4f * s, title_y + 1.4f * s, label_txt, sh);
    l.text(tx, title_y, label_txt, title_ts);
  }
  if (!parts.counter.empty()) {
    const float cx1 = x1 - 14.0f * s;
    const float cx0 = cx1 - cnt_w;
    const float cy0 = y0 + 9.0f * s;
    const float cy1 = cy0 + 22.0f * s;
    l.rect(cx0, cy0, cx1, cy1, ui::fade(accent, a * 0.96f));
    TextStyle csh = cnt_ts;
    csh.color = ui::fade(rgba(0, 0, 0, 150), a);
    const float cty = cy0 + 0.5f * ((cy1 - cy0) - cnt_ts.size);
    l.text(0.5f * (cx0 + cx1) + 1.0f * s, cty + 1.0f * s, parts.counter, csh);
    l.text(0.5f * (cx0 + cx1), cty, parts.counter, cnt_ts);
  }
  if (!st.popup_text.empty()) {
    const float by = y0 + 33.5f * s;
    TextStyle bsh = body_ts;
    bsh.color = ui::fade(rgba(0, 0, 0, 185), a);
    l.text(tx + 1.2f * s, by + 1.2f * s, st.popup_text, bsh);
    l.text(tx, by, st.popup_text, body_ts);
  }

  // Crisp bottom progress bar track + animated magenta/pink fill.
  if (st.popup_progress >= 0.0) {
    const float bx0 = tx, bx1 = x1 - 14.0f * s, by = y1 - 8.5f * s, bh = 4.0f * s;
    const float fill_frac = float(std::clamp(st.popup_progress, 0.0, 1.0)) * ease_in_time(age, 0.20, 0.75);
    l.rect(bx0, by, bx1, by + bh, ui::fade(rgba(255, 255, 255, 46), a));
    const float fw = std::max(fill_frac > 0.001f ? 4.0f * s : 0.0f, (bx1 - bx0) * fill_frac);
    if (fw > 0.5f) {
      l.rect(bx0, by, bx0 + fw, by + bh, ui::fade(accent, a));
      l.rect(bx0 + fw - 2.0f * s, by, bx0 + fw, by + bh, ui::fade(ui4::kWhite, a * 0.90f));
    }
  }
  return true;
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
  notify_ui_atlas_changed();
}
bool ui_texture(const std::string& path, std::array<float, 4>& uv) { return textures() && textures()(path, uv); }
namespace {

// Matches the inline rich-text style tags supported by ui::List's parse_style_tag()
// (single `{...}` or double `{{...}}`: font/font_pop/font=... and colour/color).
bool is_supported_ui_style_tag(std::string_view tag) {
  auto trim_tag = [](std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\'' || v.front() == '"')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\'' || v.back() == '"')) v.remove_suffix(1);
    return v;
  };
  std::string low;
  low.reserve(tag.size());
  for (char c : tag) low.push_back(char(std::tolower(static_cast<unsigned char>(c))));

  if (low == "font_pop" || low == "font" || low == "colour_pop" || low == "color_pop" ||
      low == "colour" || low == "color") {
    return true;
  }
  if (low.rfind("font=", 0) == 0) {
    return !trim_tag(std::string_view(low).substr(5)).empty();
  }
  if (low.rfind("colour=", 0) == 0 || low.rfind("color=", 0) == 0) {
    const size_t eq = low.find('=');
    const std::string v(trim_tag(std::string_view(low).substr(eq + 1)));
    const char* p = v.c_str();
    int n = 0;
    while (*p && n < 4) {
      while (*p == ' ' || *p == ',' || *p == '\t') ++p;
      if (!*p) break;
      char* end = nullptr;
      (void)std::strtol(p, &end, 10);
      if (end == p) break;
      ++n;
      p = end;
    }
    return n >= 3;
  }
  return false;
}

}  // namespace

std::string strip_ui_inline_tags(std::string s) {
  // Preserve rich-text inline tags handled by ui::List (e.g. {font='SYM'}, {font_pop},
  // {colour='R G B A'}, {colour_pop}, single or doubled braces) while stripping
  // unrecognized {...} placeholders and [UI.*] flow tokens so no raw markup leaks.
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '{') {
      const bool dbl = (i + 1 < s.size() && s[i + 1] == '{');
      const size_t tag_start = i + (dbl ? 2 : 1);
      const size_t close = dbl ? s.find("}}", tag_start) : s.find('}', tag_start);
      if (close != std::string::npos && close - tag_start <= 64) {
        const std::string_view tag(s.data() + tag_start, close - tag_start);
        const size_t end = close + (dbl ? 2 : 1);
        if (is_supported_ui_style_tag(tag)) {
          out.append(s, i, end - i);
        }
        i = end;
        continue;
      }
    } else if (s[i] == '[' && i + 4 < s.size() && s.compare(i, 4, "[UI.") == 0) {
      const size_t close = s.find(']', i + 4);
      if (close != std::string::npos && close - i < 64) {
        i = close + 1;
        continue;
      }
    }
    out.push_back(s[i++]);
  }
  return out;
}

std::string game_text(const std::string& value) {
  std::string resolved = texts() ? texts()(value) : value;
  if (resolved.find('{') != std::string::npos || resolved.find("[UI.") != std::string::npos) {
    resolved = strip_ui_inline_tags(std::move(resolved));
  }
  return resolved;
}
std::string game_text_raw(const std::string& value) { return texts() ? texts()(value) : value; }
std::string car_name(const std::string& media) {
  std::string out = names() ? names()(media) : media;
  if (out.empty()) out = media;
  if (out.find('_') != std::string::npos) {
    const std::string pretty = ui4::pretty_car_media_name(out);
    if (!pretty.empty()) return pretty;
  }
  return out;
}

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

std::string format_speed_unit(double mph, bool metric, bool decimals) {
  char buf[48];
  const double v = metric ? (mph * 1.609344) : mph;
  const char* u = metric ? "KM/H" : "MPH";
  if (decimals) std::snprintf(buf, sizeof buf, "%.1f %s", v, u);
  else std::snprintf(buf, sizeof buf, "%d %s", int(std::lround(v)), u);
  return buf;
}

std::string format_distance_unit(double meters, bool metric) {
  char buf[48];
  const double m = std::max(0.0, meters);
  if (metric) {
    if (m >= 1000.0) std::snprintf(buf, sizeof buf, "%.1f KM", m / 1000.0);
    else std::snprintf(buf, sizeof buf, "%d M", int(std::lround(m)));
  } else {
    const double mi = m / 1609.344;
    if (mi >= 0.1) std::snprintf(buf, sizeof buf, "%.2f MI", mi);
    else std::snprintf(buf, sizeof buf, "%d FT", int(std::lround(m * 3.28084)));
  }
  return buf;
}

std::string format_gap_unit(double meters, bool metric) {
  char buf[48];
  if (metric) {
    if (std::fabs(meters) >= 1000.0) std::snprintf(buf, sizeof buf, "%+.2f KM", meters / 1000.0);
    else std::snprintf(buf, sizeof buf, "%+.0f M", meters);
  } else {
    const double ft = meters * 3.28084;
    if (std::fabs(meters) >= 160.9344) std::snprintf(buf, sizeof buf, "%+.2f MI", meters / 1609.344);
    else std::snprintf(buf, sizeof buf, "%+.0f FT", ft);
  }
  return buf;
}

std::string format_boost_unit(double boost_psi, bool metric) {
  char buf[32];
  const double psi = std::max(0.0, boost_psi);
  if (metric) {
    std::snprintf(buf, sizeof buf, "%.2f BAR", psi * 0.06894757);
  } else {
    std::snprintf(buf, sizeof buf, "%.1f PSI", psi);
  }
  return buf;
}

std::string format_split_delta(double delta_s) {
  char buf[32];
  if (std::fabs(delta_s) < 0.005) {
    std::snprintf(buf, sizeof buf, "+0.00s");
  } else {
    std::snprintf(buf, sizeof buf, "%+.2fs", delta_s);
  }
  return buf;
}

std::string format_unit_label(std::string_view raw, std::string_view fallback) {
  if (raw.empty() || raw.rfind("_&", 0) == 0) return std::string(fallback);
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size();) {
    if (raw[i] == '{') {
      const size_t close = raw.find('}', i + 1);
      if (close != std::string_view::npos && close > i + 1) {
        i = close + 1;
        continue;
      }
    }
    if (raw[i] == '%' && i + 1 < raw.size()) {
      size_t j = i + 1;
      if (raw[j] == '%') {
        out += '%';
        i += 2;
        continue;
      }
      while (j < raw.size() && (std::isdigit(static_cast<unsigned char>(raw[j])) || raw[j] == '.' || raw[j] == '-' ||
                                raw[j] == '+' || raw[j] == '#')) {
        ++j;
      }
      if (j < raw.size() && std::strchr("sdifuxXoegG", raw[j]) != nullptr) {
        i = j + 1;
        continue;
      }
    }
    out += raw[i++];
  }
  // Collapse whitespace and trim ends.
  std::string clean;
  clean.reserve(out.size());
  bool prev_space = true;
  for (char c : out) {
    const bool sp = std::isspace(static_cast<unsigned char>(c)) != 0;
    if (sp) {
      if (!prev_space) clean += ' ';
      prev_space = true;
    } else {
      clean += c;
      prev_space = false;
    }
  }
  while (!clean.empty() && std::isspace(static_cast<unsigned char>(clean.back()))) clean.pop_back();
  return clean.empty() ? std::string(fallback) : clean;
}

bool should_show_gps_turn_banner(const GpsHud& gps) {
  if (!gps.active) return false;
  if (gps.recalculating) return true;
  if (gps.turn_kind == 8) {
    // Suppress ghost "U TURN AROUND 0 FT" prompts when near 0 distance or not a sustained active-route wrong-way turn.
    if (gps.dist_to_dest_m < 30.0) return false;
    if (!gps.confirmed_uturn && gps.dist_to_turn_m < 5.0) return false;
  }
  if (gps.turn_kind == 0 && gps.turn_text.empty() && gps.dist_to_turn_m < 5.0) return false;
  return true;
}

bool should_show_gps_turn_distance(const GpsHud& gps) {
  if (gps.turn_kind == 8) return false;  // U-Turn never displays "0 FT" / "0 M"
  return gps.dist_to_turn_m >= 5.0;
}

float compute_subtitle_bottom_lift(bool subtitle_active, int subtitle_lines, float subtitle_box_h_canvas) {
  if (!subtitle_active) return 0.0f;
  // Subtitle box bottom sits at y=658 (720p canvas), speaker badge extends 22px above box top
  // (speaker_top = 636 - box_h). Bottom-center cards sit down to y=663 - lift.
  // Adding 56px clearance above box_h guarantees >= 29px visible air gap above the speaker pill.
  if (subtitle_box_h_canvas > 0.0f) return std::max(104.0f, subtitle_box_h_canvas + 56.0f);
  if (subtitle_lines > 0) {
    const float est_box_h = std::max(46.0f, float(subtitle_lines) * 20.0f + 20.0f);
    return std::max(104.0f, est_box_h + 56.0f);
  }
  return 136.0f;  // safe 3-line default clearance
}

RewindOverlayAction rewind_overlay_action_for_button(bool rewinding, bool paused_at_keyframe, bool at_oldest_slot,
                                                     int button_index) {
  if (button_index < 0) return RewindOverlayAction::kNone;
  if (rewinding) {
    return button_index == 0 ? RewindOverlayAction::kCancel : RewindOverlayAction::kNone;
  }
  if (paused_at_keyframe) {
    if (at_oldest_slot) {
      if (button_index == 0) return RewindOverlayAction::kResume;
      if (button_index == 1) return RewindOverlayAction::kViewReplay;
      if (button_index == 2) return RewindOverlayAction::kCancel;
      return RewindOverlayAction::kNone;
    }
    if (button_index == 0) return RewindOverlayAction::kResume;
    if (button_index == 1) return RewindOverlayAction::kRewindFurther;
    if (button_index == 2) return RewindOverlayAction::kViewReplay;
    if (button_index == 3) return RewindOverlayAction::kCancel;
  }
  return RewindOverlayAction::kNone;
}

void build_rewind_overlay(ui::List& list, float w, float h, bool rewinding, bool loop_rewind, bool paused_at_keyframe,
                          bool at_oldest_slot, bool settling, const RewindScrubberHud* scrubber) {
  // 921_I_REWIND overlay: KITPARTS/REWINDLOGO, KITPARTS/REWINDARROW,
  // live animated scrubber bar (buffer fill, moving playhead & -X.Xs readout)
  // + clickable BUTTON_BAR_SCENE.
  const float sf = h / 720.0f;
  (void)ui_scene("921_I_REWIND");
  ui4::subtle_vignette(list, w, h, 0.35f);
  const std::string base_rewind = str("InGame", "IDS_Rewind", "REWIND");
  std::string rw_title = base_rewind;
  std::vector<ui4::Prompt> rw_prompts;
  if (rewinding) {
    rw_title = loop_rewind ? (base_rewind + "ING FURTHER") : str("InGame", "IDS_Rewinding", "REWINDING");
    rw_prompts = {{"B", str("HelpButtons", "IDS_Cancel", "CANCEL")}};
  } else if (paused_at_keyframe) {
    rw_title = base_rewind + " PAUSED";
    if (at_oldest_slot) {
      rw_prompts = {{"A", str("InGame", "IDS_Resume", "RESUME")},
                    {"X", "VIEW REPLAY"},
                    {"B", str("HelpButtons", "IDS_Cancel", "CANCEL")}};
    } else {
      rw_prompts = {{"A", str("InGame", "IDS_Resume", "RESUME")},
                    {"Y", "REWIND FURTHER"},
                    {"X", "VIEW REPLAY"},
                    {"B", str("HelpButtons", "IDS_Cancel", "CANCEL")}};
    }
  } else if (settling) {
    rw_title = "RESUMING";
  }

  const RewindScrubberHud fallback_sc =
      compute_rewind_scrubber(8, 8, at_oldest_slot ? 0 : (rewinding ? 4 : 5), 0.0, 20.0,
                              at_oldest_slot ? 0.0 : (rewinding ? 10.0 : 12.5), 0.0);
  const RewindScrubberHud& sc = scrubber ? *scrubber : fallback_sc;
  const float buf_frac = std::clamp(sc.buffer_fraction, 0.05f, 1.0f);
  const float head_frac = std::clamp(sc.playhead_fraction, 0.0f, 1.0f);
  const float pulse = rewinding ? (0.5f + 0.5f * std::sin(float(sc.anim_time) * 14.0f)) : 0.0f;

  const float cx = 0.5f * w;
  const float logo_y = h - 204.0f * sf;
  std::array<float, 4> rw_logo{}, rw_arrow{}, rw_bar{};
  if (ui_texture("KITPARTS/REWINDLOGO", rw_logo)) {
    list.image(cx - 120.0f * sf, logo_y, cx + 120.0f * sf, logo_y + 44.0f * sf, rw_logo, ui4::kWhite);
  }
  if (ui_texture("KITPARTS/REWINDARROW", rw_arrow)) {
    const float arrow_nudge = rewinding ? (-6.0f * sf * pulse) : 0.0f;
    list.image(cx - 168.0f * sf + arrow_nudge, logo_y + 6.0f * sf, cx - 126.0f * sf + arrow_nudge, logo_y + 38.0f * sf,
               rw_arrow, rewinding ? ui4::kPink : ui4::kWhite);
  }
  const float pill_y = logo_y + 50.0f * sf;
  ui4::brush(list, cx - 190.0f * sf, pill_y, cx + 190.0f * sf, pill_y + 32.0f * sf, ui4::kPink);
  list.text(cx, pill_y + 7.0f * sf, rw_title, ui4::style(ui::Font::kE, 18.0f * sf, ui4::kWhite, ui::Align::kCenter));

  // Animated Rewind scrubber bar with buffer capacity fill, keyframe ticks, moving playhead & live -X.Xs readout.
  const float bar_x0 = cx - 224.0f * sf;
  const float bar_x1 = cx + 224.0f * sf;
  const float bar_w = bar_x1 - bar_x0;
  const float bar_y = pill_y + 42.0f * sf;
  const float bar_h = 10.0f * sf;

  // Dark full-length track + top hairline.
  list.rect(bar_x0 - 2.0f * sf, bar_y - 2.0f * sf, bar_x1 + 2.0f * sf, bar_y + bar_h + 2.0f * sf,
            rgba(8, 10, 14, 215));
  list.rect(bar_x0, bar_y, bar_x1, bar_y + bar_h, rgba(44, 48, 56, 175));

  // Filled rewind buffer span anchored to right edge (older history grows leftward up to bar_x0).
  const float fill_x0 = bar_x1 - bar_w * buf_frac;
  list.rect(fill_x0, bar_y, bar_x1, bar_y + bar_h, rgba(96, 38, 66, 215));

  // Moving playhead position within [fill_x0, bar_x1].
  const float head_x = fill_x0 + (bar_x1 - fill_x0) * head_frac;
  if (head_x > fill_x0 + 1.0f) {
    if (ui_texture("MASKS/GRADIATEDBAR", rw_bar)) {
      list.image(fill_x0, bar_y, head_x, bar_y + bar_h, rw_bar, ui4::kPink);
    } else {
      list.rect(fill_x0, bar_y, head_x, bar_y + bar_h, ui4::kPink);
    }
  }
  // Highlight rewound tail from playhead to trigger point (bar_x1) in bright FH1 pink-amber glow.
  if (bar_x1 > head_x + 1.0f) {
    list.rect(head_x, bar_y + 2.0f * sf, bar_x1, bar_y + bar_h - 2.0f * sf,
              ui::fade(ui4::kAmber, 0.55f + 0.30f * pulse));
  }
  // Keyframe slot notch ticks inside populated buffer.
  if (sc.total_slots > 1) {
    for (int k = 0; k < sc.total_slots; ++k) {
      const float frac = float(k) / float(sc.total_slots - 1);
      const float tx = fill_x0 + (bar_x1 - fill_x0) * frac;
      list.rect(tx - 0.8f * sf, bar_y, tx + 0.8f * sf, bar_y + bar_h, rgba(255, 255, 255, 110));
    }
  }
  // Moving playhead vertical needle + glowing marker cap.
  list.rect(head_x - 2.5f * sf, bar_y - 5.0f * sf, head_x + 2.5f * sf, bar_y + bar_h + 5.0f * sf, ui4::kWhite);
  list.circle(head_x, bar_y + 0.5f * bar_h, 5.5f * sf, rewinding ? ui4::kYellow : ui4::kPinkHi);
  list.circle(head_x, bar_y + 0.5f * bar_h, 2.5f * sf, ui4::kWhite);

  // Live "-X.Xs" rewound offset & remaining buffer seconds readout beneath scrubber.
  char rw_off[32], rw_rem[48];
  std::snprintf(rw_off, sizeof rw_off, "-%.1fs", std::max(0.0, sc.seconds_rewound));
  if (sc.total_slots > 0) {
    std::snprintf(rw_rem, sizeof rw_rem, "%.1fs LEFT  •  SLOT %d/%d", std::max(0.0, sc.seconds_remaining),
                  sc.current_slot + 1, sc.total_slots);
  } else {
    std::snprintf(rw_rem, sizeof rw_rem, "%.1fs LEFT", std::max(0.0, sc.seconds_remaining));
  }
  const float meta_y = bar_y + bar_h + 7.0f * sf;
  list.text(bar_x0, meta_y, rw_rem, ui4::style(ui::Font::kB, 11.5f * sf, ui4::kOffWhite));
  list.text(bar_x1, meta_y - 1.0f * sf, rw_off,
            ui4::style(ui::Font::kE, 14.0f * sf, ui4::kYellow, ui::Align::kRight, true));

  ui4::button_bar(list, rw_prompts, w, h);
}

PopupTitleParts split_popup_title_counter(std::string_view title) {
  auto trim = [](std::string_view v) {
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.remove_prefix(1);
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.remove_suffix(1);
    return v;
  };
  title = trim(title);
  const size_t slash = title.rfind('/');
  if (slash != std::string_view::npos && slash > 0 && slash + 1 < title.size()) {
    size_t r = slash + 1;
    while (r < title.size() && title[r] == ' ') ++r;
    const size_t den_start = r;
    while (r < title.size() && std::isdigit(static_cast<unsigned char>(title[r]))) ++r;
    const bool has_den = (r > den_start) && (r == title.size());

    size_t l = slash;
    while (l > 0 && title[l - 1] == ' ') --l;
    const size_t num_end = l;
    while (l > 0 && std::isdigit(static_cast<unsigned char>(title[l - 1]))) --l;
    const bool has_num = (num_end > l);

    if (has_num && has_den && l > 0 && std::isspace(static_cast<unsigned char>(title[l - 1]))) {
      const std::string_view lbl = trim(title.substr(0, l));
      const std::string_view cnt = trim(title.substr(l));
      if (!lbl.empty() && !cnt.empty()) {
        return {std::string(lbl), std::string(cnt)};
      }
    }
  }
  return {std::string(title), {}};
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
    const bool has_bar = st.credits >= 0 || !st.car_name.empty();
    map_overlay_hud(l, *st.map_overlay, g, has_bar, st.metric_units);
    if (has_bar) draw_career_header(l, st, g);
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
  // Bottom-center HUD stack allocator (720p canvas units): when campaign subtitles are
  // active, lift bottom-center cards dynamically above the subtitle box + speaker pill, and
  // stack concurrent cards upward so zero banners ever overlap!
  float bottom_lift = compute_subtitle_bottom_lift(st.subtitle_active, st.subtitle_lines, st.subtitle_box_h_canvas);
  // The driving HUD hides under a menu (SPEC-UI-LAYOUTS d).
  if (st.driving && !menu) {
    const bool show_tags = !st.name_tags.empty() && (!st.race || st.race->phase == RaceHud::kRacing);
    const WorldMarkerHud* active_wm = (st.world_marker && st.world_marker->visible && !st.race) ? st.world_marker : nullptr;
    if (show_tags || active_wm) {
      static const std::vector<NameTagHud> kEmptyTags;
      render_world_labels_and_nameplates(l, show_tags ? st.name_tags : kEmptyTags, active_wm, st.race != nullptr, w, h,
                                         st.metric_units);
    }
    if (st.gps && st.gps->active && !st.race) gps_turn_hud(l, *st.gps, g, st.metric_units);
    if (st.pr_stunt && st.pr_stunt->active && !st.race) pr_stunt_hud(l, *st.pr_stunt, g);
    if (st.h2h && !st.race) {
      const float used = h2h_hud(l, *st.h2h, g, st.metric_units, bottom_lift);
      if (used > 0.0f) bottom_lift += used + 8.0f;
    }
    if (st.speed_hud && !st.race) {
      const float used = speed_activity_hud(l, *st.speed_hud, g, st.metric_units, bottom_lift);
      if (used > 0.0f) bottom_lift += used + 8.0f;
    }
    if (st.skills && !(st.race && (st.race->phase == RaceHud::kFinished || st.race->grid_menu_open)))
      skill_chain_hud(l, *st.skills, g, st.time);
    if (st.race) {
      const RaceHud& r = *st.race;
      if (r.phase != RaceHud::kFinished) reset_post_race_page_timer();
      if (r.phase == RaceHud::kGrid) event_card(l, r, g);
      else if (r.phase == RaceHud::kFinished) results(l, r, st, g);
      else {
        race_hud(l, r, g);
        showcase_hud(l, r, g, st.metric_units);
      }
    } else {
      reset_post_race_page_timer();
    }
    if (st.rally_stage && st.rally_stage->active) build_rally_pace_notes_hud(l, *st.rally_stage, w, h);
    if (progression_hud(l, st, g, bottom_lift)) bottom_lift += 94.0f;
    if (popup_card(l, st, g, bottom_lift)) bottom_lift += 72.0f;
    const bool hide_dial = st.race && (st.race->phase == RaceHud::kFinished || st.race->grid_menu_open);
    if (!hide_dial) {
      if (st.speedo_mode == 1) render_digital_dial(l, st, g);
      else if (st.speedo_mode == 0) dial(l, st, g);
    }
    // R3-UI-11: Clean dark-backed Barn Find proximity radar pill positioned strictly above
    // SatNav / GPS turn banner (never overlaps SatNav at y=376..408!).
    if (st.barn_radar_proximity > 0.04 && !st.race) {
      const float prox = float(std::clamp(st.barn_radar_proximity, 0.0, 1.0));
      const float pulse_hz = 1.5f + 4.0f * prox;
      const float pulse = 0.5f + 0.5f * std::sin(float(st.time) * 6.28318f * pulse_hz);
      const bool gps_vis = st.gps && should_show_gps_turn_banner(*st.gps);
      const float bx0 = g.L(64), bx1 = g.L(296);
      const float by0 = gps_vis ? g.B(334) : g.B(372);
      const float by1 = by0 + 30.0f * s;
      const uint32_t accent = prox > 0.72f ? ui4::kPink : rgba(212, 84, 198, 255);
      ui4::pill(l, bx0, by0, bx1, by1, ui4::kBrushBlack, ui4::PillKind::kHud01);
      l.rect(bx0, by0, bx0 + 4.0f * s, by1, ui::fade(accent, 0.72f + 0.28f * pulse));
      const float rx = bx0 + 21.0f * s, ry = 0.5f * (by0 + by1);
      l.circle(rx, ry, (7.0f + 5.5f * pulse) * s, ui::fade(accent, 0.20f + 0.38f * prox * (1.0f - pulse)));
      l.circle(rx, ry, 5.2f * s, rgba(255, 206, 62, 245));
      l.text(bx0 + 36.0f * s, by0 + 8.0f * s,
             prox > 0.72f ? "BARN FIND NEARBY!" : "BARN RUMOUR ZONE",
             font(ui::Font::kE, 11.8f * s, prox > 0.72f ? ui4::kYellow : ui4::kWhite));
      // Compact 4-bar proximity signal meter on right edge of pill.
      const float mx0 = bx1 - 36.0f * s;
      for (int bi = 0; bi < 4; ++bi) {
        const float bh_i = (5.0f + 3.2f * float(bi)) * s;
        const bool lit = prox >= (0.18f + 0.22f * float(bi));
        l.rect(mx0 + float(bi) * 6.5f * s, by1 - 6.0f * s - bh_i, mx0 + float(bi) * 6.5f * s + 4.2f * s,
               by1 - 6.0f * s, lit ? ui4::kYellow : rgba(255, 255, 255, 42));
      }
    }
  }
  if (!st.toast.empty() && !menu) {
    TextStyle ts = font(ui::Font::kB, 14 * s, kText, Align::kCenter);
    fit(st.toast, ts, w - 120 * s, 8 * s);
    const float tw = ui::List::measure(st.toast, ts);
    const float y = g.B(653 - bottom_lift);
    l.rect(0.5f * w - 0.5f * tw - 14 * s, y, 0.5f * w + 0.5f * tw + 14 * s, y + 28 * s, kBlack60);
    l.text(0.5f * w, y + 7 * s, st.toast, ts);
  }
  // The pause menu from its own scene (925_PAUSE_MENU and the screens it
  // opens); the shared menu panel when the scene is missing.
  if (st.pause_screen && draw_pause_screen(l, st, *st.pause_screen, w, h)) return;
  if (menu) menu_panel(l, st, g);
  if (menu && st.career) career_board(l, *st.career, w, h, s);
}


}  // namespace carview
