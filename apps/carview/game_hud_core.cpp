#include "game_hud_internal.h"
#include "touch_controls.h"

#include <cstdlib>

namespace carview {

using namespace hud_detail;

namespace {

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
  auto angle = [&](double rpm) { return -kSweep + 2.0f * kSweep * float(std::clamp(rpm / top, 0.0, 1.0)); };

  // 2. Smooth radial-gradient redline zone with bright outer rim + subtle hatch segments.
  const float a_red = angle(red);
  if (kSweep > a_red + 0.01f) {
    l.arc(cx, cy, 65.5f * s, 9.0f * s, a_red, kSweep, ui::fade(kNeedlePink, 0.16), true);
    l.arc(cx, cy, 73.0f * s, 8.5f * s, a_red, kSweep, ui::fade(kNeedlePink, 0.34), true);
    l.arc(cx, cy, 80.0f * s, 8.0f * s, a_red, kSweep, ui::fade(kNeedlePink, 0.58), true);
    l.arc(cx, cy, 84.8f * s, 3.2f * s, a_red, kSweep, ui::fade(kNeedleHot, 0.94), true);
    l.arc(cx, cy, 86.5f * s, 1.3f * s, a_red, kSweep, rgba(255, 130, 186, 235), true);
    // Crisp segmented hatch cuts across redline band (every 250 RPM).
    for (double k = std::ceil(red / 250.0) * 250.0; k < top - 1.0; k += 250.0) {
      const float ha = angle(k), sa = std::sin(ha), ca = std::cos(ha);
      l.line(cx + 64 * s * sa, cy - 64 * s * ca, cx + 84.5f * s * sa, cy - 84.5f * s * ca, 1.1f * s,
             rgba(18, 12, 18, 85));
    }
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
  l.text(cx, cy - 31.5f * s, unit_lbl, unit_ts);

  const double disp_spd = st.metric_units ? std::fabs(st.kmh) : (std::fabs(st.kmh) * 0.62137119);
  const int v = std::min(999, int(std::lround(disp_spd)));
  char digits[8];
  std::snprintf(digits, sizeof digits, "%03d", v);
  const size_t lead = v >= 100 ? 0 : v >= 10 ? 1 : 2;
  const float slot_w = 21.5f * s;
  const float speed_y = cy - 16.5f * s;
  for (int i = 0; i < 3; ++i) {
    const float slot_cx = cx + float(i - 1) * slot_w;
    const bool is_lead = size_t(i) < lead;
    TextStyle dts = font(ui::Font::kE, 30.5f * s, is_lead ? rgba(235, 238, 244, 68) : rgba(250, 251, 255, 255),
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
  TextStyle gts = font(ui::Font::kE, 18.5f * s, rgba(238, 240, 245, 245), Align::kCenter);
  gts.stretch = 0.92f;
  TextStyle gsh = gts;
  gsh.color = rgba(0, 0, 0, 145);
  l.text(cx - 21.0f * s + 1.0f * s, cy + 19.5f * s + 1.2f * s, gear, gsh);
  l.text(cx - 21.0f * s, cy + 19.5f * s, gear, gts);

  bool drew_assists = false;
  if (sc && sf && gauge_node >= 0) {
    const int assists = sc->find("ASSIST_ICONS", gauge_node);
    if (assists >= 0) {
      for (const auto& [name, active] : {std::pair{"HUD_ABS", st.abs}, std::pair{"HUD_TCS", st.tcs}}) {
        const int on = sc->find("Rectangle_ON", sc->find(name, assists));
        if (on >= 0 && sc->model(l, *sf, on, active ? 0.92f : 0.36f, 0, 0, 0,
                                 active ? rgba(232, 236, 244) : rgba(110, 114, 122))) {
          drew_assists = true;
        }
      }
    }
  }
  if (!drew_assists) {
    const uint32_t abs_col = st.abs ? ui::fade(rgba(232, 236, 244), 0.94f) : ui::fade(rgba(110, 114, 122), 0.36f);
    const uint32_t tcs_col = st.tcs ? ui::fade(rgba(255, 212, 56), 0.96f) : ui::fade(rgba(110, 114, 122), 0.36f);
    const uint32_t stm_col = st.stm ? ui::fade(rgba(255, 148, 56), 0.96f) : ui::fade(rgba(110, 114, 122), 0.36f);
    TextStyle ats = font(ui::Font::kB, 8.6f * s, abs_col, Align::kCenter);
    l.text(cx - 1.5f * s, cy + 23.0f * s, "ABS", ats);
    ats.color = tcs_col;
    l.text(cx + 17.5f * s, cy + 23.0f * s, "TCS", ats);
    ats.color = stm_col;
    l.text(cx + 36.5f * s, cy + 23.0f * s, "STM", ats);
  } else if (st.stm) {
    TextStyle sts = font(ui::Font::kB, 8.6f * s, ui::fade(rgba(255, 148, 56), 0.96f), Align::kCenter);
    l.text(cx + 42.0f * s, cy + 23.0f * s, "STM", sts);
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

// Crisp Digital HUD tach/speedo mode (FH1 Settings -> HUD Speedometer: Digital).
void render_digital_dial(ui::List& l, const DriveHud& st, const Layout& g) {
  float s = g.s;
  float rx = g.R(1214), by = g.B(668);
  if (st.touch_hud_active) {
    TouchRect mm_r, tach_r;
    TouchControls::hud_safe_rects(g.w, g.h, mm_r, tach_r);
    rx = tach_r.x1 * g.w;
    by = tach_r.y1 * g.h;
  }
  const float bw = 208.0f * s, bh = 86.0f * s;
  const float x0 = rx - bw, y0 = by - bh;
  l.rect(x0, y0, rx, by, rgba(14, 16, 20, 175));
  // Segmented RPM bar along top edge.
  const double red = std::max(1000.0, st.redline_rpm);
  const double top = std::max(10000.0, std::ceil(red * 1.1 / 2000.0) * 2000.0);
  const float rpm_f = float(std::clamp(st.rpm / top, 0.0, 1.0));
  const float red_f = float(std::clamp(red / top, 0.0, 1.0));
  constexpr int kSegs = 24;
  const float seg_w = (bw - 16.0f * s) / float(kSegs);
  for (int i = 0; i < kSegs; ++i) {
    const float t = float(i + 1) / float(kSegs);
    const float sx0 = x0 + 8.0f * s + float(i) * seg_w;
    const bool lit = rpm_f >= t - (0.5f / float(kSegs));
    const bool is_red = t >= red_f;
    const uint32_t col = lit ? (is_red ? rgba(249, 0, 100, 245) : rgba(245, 247, 252, 240))
                             : (is_red ? rgba(110, 18, 52, 110) : rgba(72, 76, 86, 95));
    l.rect(sx0, y0 + 8.0f * s, sx0 + seg_w - 2.0f * s, y0 + 20.0f * s, col);
  }
  // Speed + unit + gear.
  const double disp_spd = st.metric_units ? std::fabs(st.kmh) : (std::fabs(st.kmh) * 0.62137119);
  char sbuf[16];
  std::snprintf(sbuf, sizeof sbuf, "%03d", std::min(999, int(std::lround(disp_spd))));
  l.text(rx - 52.0f * s, y0 + 26.0f * s, sbuf, font(ui::Font::kE, 36.0f * s, rgba(250, 251, 255, 255), Align::kRight));
  const std::string unit_lbl = st.metric_units ? "KM/H" : "MPH";
  l.text(rx - 52.0f * s, y0 + 64.0f * s, unit_lbl, font(ui::Font::kB, 10.5f * s, rgba(210, 214, 224, 200), Align::kRight));
  const char* gear = st.gear < 0 ? "R" : st.gear == 0 ? "N" : nullptr;
  char gb[8];
  if (!gear) std::snprintf(gb, sizeof gb, "%d", st.gear), gear = gb;
  l.rect(rx - 44.0f * s, y0 + 28.0f * s, rx - 8.0f * s, y0 + 64.0f * s, rgba(249, 0, 100, 230));
  l.text(rx - 26.0f * s, y0 + 34.0f * s, gear, font(ui::Font::kE, 24.0f * s, rgba(255, 255, 255, 255), Align::kCenter));
  // Separate ABS / TCS / STM + Boost bar/readout (BAR when metric, PSI when imperial) on left side.
  l.text(x0 + 10.0f * s, y0 + 28.0f * s, "ABS",
         font(ui::Font::kB, 9.5f * s, st.abs ? rgba(240, 244, 252, 245) : rgba(110, 114, 122, 95)));
  l.text(x0 + 10.0f * s, y0 + 41.0f * s, "TCS",
         font(ui::Font::kB, 9.5f * s, st.tcs ? rgba(255, 212, 56, 245) : rgba(110, 114, 122, 95)));
  l.text(x0 + 10.0f * s, y0 + 54.0f * s, "STM",
         font(ui::Font::kB, 9.5f * s, st.stm ? rgba(255, 148, 56, 245) : rgba(110, 114, 122, 95)));
  if (st.has_boost && st.boost_max_psi > 0.5) {
    const float bf = float(std::clamp(st.boost_psi / st.boost_max_psi, 0.0, 1.0));
    const float bbx0 = x0 + 10.0f * s, bbx1 = x0 + 78.0f * s, bby = y0 + 67.0f * s;
    l.rect(bbx0, bby, bbx1, bby + 3.5f * s, rgba(255, 255, 255, 36));
    if (bf > 0.01f) {
      l.rect(bbx0, bby, bbx0 + (bbx1 - bbx0) * bf, bby + 3.5f * s,
             bf > 0.85f ? rgba(255, 36, 126, 245) : rgba(72, 212, 255, 240));
    }
    const std::string bp = format_boost_unit(st.boost_psi, st.metric_units);
    l.text(x0 + 10.0f * s, y0 + 72.5f * s, bp, font(ui::Font::kB, 8.8f * s, rgba(72, 212, 255, 235)));
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

// The discovery card (the interim of SPEC-UI-LAYOUTS: an E line on a pink
// brush, a B line under it, top centre): drops in, holds, fades.
void popup_card_drawn(ui::List& l, const DriveHud& st, const Layout& g, float lift_canvas) {
  const double age = st.time - st.popup_at;
  if (st.popup_title.empty() || age < 0 || age > kPopupSeconds) return;
  const float s = g.s;
  const float in = ease_in_time(age, 0, 0.3);
  const float a = in * (1 - clamp01((age - (kPopupSeconds - 0.5)) / 0.5));
  TextStyle ts = font(ui::Font::kB, 16 * s, ui::fade(kText, a));
  TextStyle body = font(ui::Font::kB, 12 * s, ui::fade(kText, a));
  fit(st.popup_title, ts, g.w - g.C(560) - 40 * s, 9 * s);
  const float tw = std::max(ui::List::measure(st.popup_title, ts), ui::List::measure(st.popup_text, body));
  const float x0 = g.C(450), x1 = std::max(g.C(917), g.C(532) + tw + 30 * s);
  const float dy = ((1 - in) * 20 - lift_canvas) * s;
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

bool popup_card(ui::List& l, const DriveHud& st, const Layout& g, float lift_canvas) {
  const double age = st.time - st.popup_at;
  if (st.popup_title.empty() || age < 0 || age > kPopupSeconds) return false;
  const SceneDraw sc = resolve_scene_draw("H_BARN_FIND_HUD");
  const int tape = sc.find("DoubleTape");
  const int panel = sc.find("Panel", tape), panel2 = sc.find("Panel2", tape);
  const int title = sc.find("TEXT2", tape), body = sc.find("TEXT1", tape), icon = sc.find("MiscIcon", tape);
  if (panel < 0 || panel2 < 0 || title < 0 || body < 0) {
    popup_card_drawn(l, st, g, lift_canvas);
    return true;
  }
  const float in = ease_in_time(age, 0, 0.3);
  const float a = in * (1 - clamp01((age - (kPopupSeconds - 0.5)) / 0.5));
  // Bottom-centre anchored, lifted cleanly above active subtitles / stacked banners.
  const SceneFrame f{0.5f * g.w - 640 * g.s, g.h - 720 * g.s, g.s};
  const float dy = (1 - in) * 20 - lift_canvas;
  const float tx = sc.at(title)[0];
  ui::TextStyle ts = sc.style(title, f, kText), bs = sc.style(body, f, kText);
  const float tw = std::max(ui::List::measure(st.popup_title, ts), ui::List::measure(st.popup_text, bs)) / g.s;
  const int right_anchor = sc.find("RightAnchor", tape);
  const float anchor_x = right_anchor >= 0 ? sc.at(right_anchor)[0] : 917.0f;
  const float right = std::max(anchor_x, tx + std::min(tw, sc.text_width(title) > 0 ? sc.text_width(title) : tw) + 30);

  // Render DoubleTape subtree via SceneDraw::draw_subtree() with dynamic crop widths & bound text.
  SceneBindings sb;
  sb.ignore_zero_alpha = true;
  sb.opacity = a;
  sb.dy = dy;
  sb.default_text_color = kText;
  sb.index(panel2).visible_width = right - sc.rect(panel2)[0];
  sb.index(panel).visible_width = right - sc.rect(panel)[0];
  sb.index(title).text = st.popup_title;
  if (!st.popup_text.empty()) sb.index(body).text = st.popup_text;
  const int drawn = sc.draw_subtree(l, f, tape, sb);
  if (drawn == 0) {
    popup_card_drawn(l, st, g, lift_canvas);
    return true;
  }
  if (icon < 0) {
    l.rect(f.x(489), f.y(614 + dy), f.x(521), f.y(646 + dy), ui::fade(st.popup_colour ? st.popup_colour : kPink, a));
  }
  if (st.popup_progress >= 0) {
    const float bx0 = tx, bx1 = right - 30, by = 654 + dy;
    const float k = float(std::clamp(st.popup_progress, 0.0, 1.0)) * ease_in_time(age, 0.3, 0.8);
    l.rect(f.x(bx0), f.y(by), f.x(bx1), f.y(by + 3), ui::fade(rgba(255, 255, 255, 50), a));
    l.rect(f.x(bx0), f.y(by), f.x(bx0 + (bx1 - bx0) * k), f.y(by + 3), ui::fade(st.popup_colour ? st.popup_colour : kPink, a));
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
                          bool at_oldest_slot, bool settling) {
  // 921_I_REWIND overlay: KITPARTS/REWINDLOGO, KITPARTS/REWINDARROW,
  // MASKS/GRADIATEDBAR timeline + clickable BUTTON_BAR_SCENE.
  const float sf = h / 720.0f;
  (void)ui_scene("921_I_REWIND");
  ui4::subtle_vignette(list, w, h, 0.35f);
  const char* rw_title = "REWIND";
  std::vector<ui4::Prompt> rw_prompts;
  if (rewinding) {
    rw_title = loop_rewind ? "REWINDING FURTHER" : "REWINDING";
    rw_prompts = {{"B", "CANCEL"}};
  } else if (paused_at_keyframe) {
    rw_title = "REWIND PAUSED";
    if (at_oldest_slot) rw_prompts = {{"A", "RESUME"}, {"X", "VIEW REPLAY"}, {"B", "CANCEL"}};
    else rw_prompts = {{"A", "RESUME"}, {"Y", "REWIND FURTHER"}, {"X", "VIEW REPLAY"}, {"B", "CANCEL"}};
  } else if (settling) {
    rw_title = "RESUMING";
  }
  const float cx = 0.5f * w;
  const float logo_y = h - 190.0f * sf;
  std::array<float, 4> rw_logo{}, rw_arrow{}, rw_bar{};
  if (ui_texture("KITPARTS/REWINDLOGO", rw_logo)) {
    list.image(cx - 120.0f * sf, logo_y, cx + 120.0f * sf, logo_y + 44.0f * sf, rw_logo, ui4::kWhite);
  }
  if (ui_texture("KITPARTS/REWINDARROW", rw_arrow)) {
    list.image(cx - 168.0f * sf, logo_y + 6.0f * sf, cx - 126.0f * sf, logo_y + 38.0f * sf, rw_arrow,
               rewinding ? ui4::kPink : ui4::kWhite);
  }
  const float pill_y = logo_y + 52.0f * sf;
  ui4::brush(list, cx - 180.0f * sf, pill_y, cx + 180.0f * sf, pill_y + 32.0f * sf, ui4::kPink);
  list.text(cx, pill_y + 7.0f * sf, rw_title, ui4::style(ui::Font::kE, 18.0f * sf, ui4::kWhite, ui::Align::kCenter));
  const float bar_y = pill_y + 40.0f * sf;
  if (ui_texture("MASKS/GRADIATEDBAR", rw_bar)) {
    list.image(cx - 220.0f * sf, bar_y, cx + 220.0f * sf, bar_y + 10.0f * sf, rw_bar, ui4::kPink);
  } else {
    list.rect(cx - 220.0f * sf, bar_y, cx + 220.0f * sf, bar_y + 8.0f * sf, ui4::kBrushBlack);
    list.rect(cx - 220.0f * sf, bar_y, cx + 120.0f * sf, bar_y + 8.0f * sf, ui4::kPink);
  }
  ui4::button_bar(list, rw_prompts, w, h);
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
    if (!st.name_tags.empty() && (!st.race || st.race->phase == RaceHud::kRacing))
      opponent_name_tags(l, st.name_tags, g);
    if (st.world_marker && !st.race) world_marker_hud(l, *st.world_marker, g, st.metric_units);
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
    // Pulsing Barn Find proximity radar badge above minimap when inside an active rumour circle.
    if (st.barn_radar_proximity > 0.04 && !st.race) {
      const float prox = float(std::clamp(st.barn_radar_proximity, 0.0, 1.0));
      const float pulse_hz = 1.5f + 4.0f * prox;
      const float pulse = 0.5f + 0.5f * std::sin(float(st.time) * 6.28318f * pulse_hz);
      const float rx = g.L(92), ry = g.B(352);
      l.circle(rx, ry, (10.0f + 7.0f * pulse) * s, ui::fade(rgba(255, 192, 42), 0.22f + 0.35f * prox * (1.0f - pulse)));
      l.circle(rx, ry, 7.5f * s, rgba(255, 192, 42, 235));
      l.text(rx + 18.0f * s, ry - 6.0f * s,
             prox > 0.72f ? "BARN FIND NEARBY!" : "BARN RUMOUR ZONE",
             font(ui::Font::kE, 12.0f * s, rgba(255, 212, 74, 245)));
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
  if (menu) menu_panel(l, st, g);
  if (menu && st.career) career_board(l, *st.career, w, h, s);
}


}  // namespace carview
