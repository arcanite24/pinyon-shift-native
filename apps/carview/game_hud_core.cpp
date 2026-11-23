#include "game_hud_internal.h"
#include "touch_controls.h"

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
    const uint32_t abs_col = st.abs ? ui::fade(rgba(232, 236, 244), 0.92f) : ui::fade(rgba(110, 114, 122), 0.36f);
    const uint32_t tcs_col = st.tcs ? ui::fade(rgba(232, 236, 244), 0.92f) : ui::fade(rgba(110, 114, 122), 0.36f);
    TextStyle ats = font(ui::Font::kB, 9.2f * s, abs_col, Align::kCenter);
    l.text(cx + 3.0f * s, cy + 23.0f * s, "ABS", ats);
    ats.color = tcs_col;
    l.text(cx + 25.5f * s, cy + 23.0f * s, "TCS", ats);
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
  static const SceneDraw sc(ui_scene("947_HUD"));
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
  static const SceneDraw sc(ui_scene("H_BARN_FIND_HUD"));
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
}
bool ui_texture(const std::string& path, std::array<float, 4>& uv) { return textures() && textures()(path, uv); }
std::string game_text(const std::string& value) { return texts() ? texts()(value) : value; }
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
  // active, lift bottom-center cards cleanly above the subtitle bar + speaker pill, and
  // stack concurrent cards upward so zero banners ever overlap!
  float bottom_lift = st.subtitle_active ? 96.0f : 0.0f;
  // The driving HUD hides under a menu (SPEC-UI-LAYOUTS d).
  if (st.driving && !menu) {
    if (!st.name_tags.empty() && (!st.race || st.race->phase == RaceHud::kRacing))
      opponent_name_tags(l, st.name_tags, g);
    if (st.world_marker && !st.race) world_marker_hud(l, *st.world_marker, g);
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
      if (r.phase == RaceHud::kGrid) event_card(l, r, g);
      else if (r.phase == RaceHud::kFinished) results(l, r, g);
      else {
        race_hud(l, r, g);
        showcase_hud(l, r, g);
      }
    }
    if (st.rally_stage && st.rally_stage->active) build_rally_pace_notes_hud(l, *st.rally_stage, w, h);
    if (progression_hud(l, st, g, bottom_lift)) bottom_lift += 94.0f;
    if (popup_card(l, st, g, bottom_lift)) bottom_lift += 72.0f;
    const bool hide_dial = st.race && (st.race->phase == RaceHud::kFinished || st.race->grid_menu_open);
    if (!hide_dial) dial(l, st, g);
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
