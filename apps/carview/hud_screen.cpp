#include "hud_screen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "game_hud.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr uint32_t kWhite = ui::rgba(255, 255, 255);

// The direct child of `parent` called `name`.
int child(const SceneDraw& sd, int parent, std::string_view name) {
  if (parent < 0) return -1;
  for (int c : sd.children_of(parent))
    if (sd.object_name(c) == name) return c;
  return -1;
}

// The first descendant of `parent` reached by direct children along `path`
// ("a/b/c"): unlike find_path(), a repeated name picks the direct child.
int child_path(const SceneDraw& sd, int parent, std::string_view path) {
  int n = parent;
  while (n >= 0 && !path.empty()) {
    const size_t cut = path.find('/');
    n = child(sd, n, path.substr(0, cut));
    path = cut == std::string_view::npos ? std::string_view{} : path.substr(cut + 1);
  }
  return n;
}

// Shows `n` and every node under it that the scene leaves at alpha 0 (a
// script fades them in), at `alpha`.
void show_all(SceneScreen& sc, int n, float alpha = 1.0f) {
  if (n < 0) return;
  sc.show(n, alpha);
  for (int c : sc.scene().children_of(n)) show_all(sc, c, alpha);
}

std::string unit_label(const StringRef& ref) {
  std::string s = text_of(ref);
  if (auto p = s.find("{0}"); p != std::string::npos) {
    s.erase(p, 3);
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
  }
  return s;
}

}  // namespace

DialScale dial_scale(double redline_rpm) {
  const int top = int(std::ceil(std::clamp(redline_rpm, 1000.0, 20000.0) / 1000.0 - 1e-6)) + 2;
  if (top <= 9) return {top, 1, top - 1};
  const int even = (top + 1) / 2 * 2;
  return {even, 2, even};
}

SceneFrame hud_frame(HudAnchor a, float w, float h, float s) {
  const float cx = 0.5f * w - 640.0f * s, right = w - 1280.0f * s;
  const float cy = 0.5f * h - 360.0f * s, bottom = h - 720.0f * s;
  switch (a) {
    case HudAnchor::kTopLeft: return {0, 0, s};
    case HudAnchor::kTop: return {cx, 0, s};
    case HudAnchor::kTopRight: return {right, 0, s};
    case HudAnchor::kBottomLeft: return {0, bottom, s};
    case HudAnchor::kBottom: return {cx, bottom, s};
    case HudAnchor::kBottomRight: return {right, bottom, s};
    case HudAnchor::kCentre: return {cx, cy, s};
  }
  return {cx, cy, s};
}

// --- GAUGE_ANALOG ---------------------------------------------------------

namespace {

// The dial sweeps from the scene's zero (the numerals' and small hatches'
// laid-out rotation, 2.34 rad counter-clockwise from the top) to the mirror
// angle on the right.
constexpr float kDialZero = 2.34f;
// The red half ring's edge relative to its rotation node (calibrated on
// 05_roam_hud_day_drive_orig.png: the zone starts at the redline).
constexpr float kRedlineEdge = -0.23f;
// The needle's bar sits further out than the scene lays it (the same
// capture: from 58 to 83 px off the hub).
constexpr float kNeedleOut = -11.0f;
// The dimmed leading zeros of the speed and the gear's grey (the same
// capture).
constexpr uint32_t kLeadingZero = ui::rgba(200, 200, 200, 80);
// The assist badges: dark while on, white while one intervenes.
constexpr uint32_t kAssistIdle = ui::rgba(33, 33, 33);
constexpr uint32_t kGearGrey = ui::rgba(0xBA, 0xB9, 0xB8);

float dial_angle(double v, const DialScale& ds) {
  const double t = std::clamp(v / double(ds.top), 0.0, 1.0);
  return kDialZero - 2.0f * kDialZero * float(t);
}

// Turns each `_/rotation` slot under `group` to `angles[i]`; slots past the
// list are hidden.
void place_slots(SceneScreen& sc, int group, const std::vector<float>& angles) {
  const SceneDraw& sd = sc.scene();
  const std::vector<int> slots = sd.children_of(group);
  for (size_t i = 0; i < slots.size(); ++i) {
    const int rot = child(sd, slots[i], "rotation");
    if (i >= angles.size() || rot < 0) {
      sc.hide(slots[i]);
      continue;
    }
    sc.at(rot).turn = angles[i] - sd.angle(rot);
  }
}

}  // namespace

bool draw_hud_dial(ui::List& l, const DriveHud& st, const SceneFrame& f) {
  SceneScreen sc(kHudScene);
  const SceneDraw& sd = sc.scene();
  const int gauge = sc.node("GAUGE_ANALOG");
  const int rpm_root = child(sd, gauge, "RPM_ANALOG");
  const int elements = child(sd, rpm_root, "Elements");
  if (gauge < 0 || elements < 0) return false;
  const DialScale ds = dial_scale(st.redline_rpm);
  const int marks = ds.top / ds.step;
  const int numerals = ds.last / ds.step;

  // Hatches: large at each numeral, mid at the half step, small at the quarters.
  std::vector<float> large, mid, small;
  for (int i = 0; i <= marks; ++i) large.push_back(dial_angle(i * ds.step, ds));
  for (int i = 0; i < marks; ++i) {
    mid.push_back(dial_angle((i + 0.5) * ds.step, ds));
    small.push_back(dial_angle((i + 0.25) * ds.step, ds));
    small.push_back(dial_angle((i + 0.75) * ds.step, ds));
  }
  place_slots(sc, child(sd, elements, "Large_Hatches"), large);
  place_slots(sc, child(sd, elements, "Mid_Hatches"), mid);
  place_slots(sc, child(sd, elements, "Small_Hatches"), small);

  // Numerals: upright, on the large hatches.
  const int numbers = child(sd, elements, "Numbers");
  const std::vector<int> num_slots = sd.children_of(numbers);
  for (size_t i = 0; i < num_slots.size(); ++i) {
    const int rot = child(sd, num_slots[i], "rotation");
    const int text = child(sd, rot, "Text");
    if (int(i) > numerals || rot < 0 || text < 0) {
      sc.hide(num_slots[i]);
      continue;
    }
    const float turn = large[i] - sd.angle(rot);
    sc.at(rot).turn = turn;
    sc.text(text, std::to_string(int(i) * ds.step));
    sc.at(text).extra_angle = -(sd.angle(text) + turn);
  }

  // The red zone: its half ring turned so it starts at the redline.
  const int red_rot = child_path(sd, child(sd, elements, "RPM_and_MPH"), "Redline/rotation");
  if (red_rot >= 0) sc.at(red_rot).turn = dial_angle(st.redline_rpm / 1000.0, ds) + kRedlineEdge - sd.angle(red_rot);

  // The needle and its trails (the swoosh behind it while the revs climb or
  // fall).
  const int needle = child(sd, elements, "_P_Tach_Needle");
  if (needle >= 0) sc.at(needle).turn = dial_angle(st.rpm / 1000.0, ds);
  if (const int bar = child_path(sd, needle, "Group/Rectangle"); bar >= 0) sc.at(bar).dy = kNeedleOut;
  const int swooshes = child_path(sd, needle, "Group/Swooshes");
  const float rate = float(std::clamp(st.rpm_rate / 6000.0, -1.0, 1.0));
  if (const int up = child_path(sd, swooshes, "Up/Uptrail"); up >= 0) sc.at(up).alpha = std::max(0.0f, rate);
  if (const int down = child_path(sd, swooshes, "Down/DownTrail"); down >= 0)
    sc.at(down).alpha = std::max(0.0f, -rate);

  // Speed: three digits, the leading zeros dimmed.
  const double shown = st.metric_units ? std::fabs(st.kmh) : std::fabs(st.kmh) * 0.62137119;
  const int v = std::min(999, int(std::lround(shown)));
  char digits[8];
  std::snprintf(digits, sizeof digits, "%03d", v);
  const int lead = v >= 100 ? 0 : v >= 10 ? 1 : 2;
  const int speedo = child_path(sd, child(sd, gauge, "Speedo"), "Group");
  for (int i = 0; i < 3; ++i) {
    const int d = child(sd, speedo, "digit" + std::to_string(i));
    sc.text(d, std::string(1, digits[i]));
    if (d >= 0 && i < lead) sc.at(d).tint = kLeadingZero;
  }
  sc.text(child(sd, gauge, "TEXT_SPEED_UNIT_ANALOG"), unit_label(st.metric_units ? hud_text::kKmh : hud_text::kMph));
  const int gear = child(sd, gauge, "TEXT_GEAR_ANALOG");
  sc.text(gear, st.gear < 0 ? "R" : st.gear == 0 ? "N" : std::to_string(st.gear));
  if (gear >= 0) sc.at(gear).tint = kGearGrey;

  // Assists: shown when on, the glow while one intervenes.
  const int assists = child(sd, gauge, "ASSIST_ICONS");
  auto assist = [&](std::string_view name, bool enabled, bool active, double flash) {
    const int a = child(sd, assists, name);
    if (a < 0) return;
    if (!enabled) {
      sc.hide(a);
      return;
    }
    const bool lit = active || flash > 0.0;
    sc.show(a, 1.0f);
    if (const int on = child(sd, a, "Rectangle_ON"); on >= 0) {
      sc.at(on).alpha = std::max(0.5f, sd.alpha(on));
      sc.at(on).tint = lit ? kWhite : kAssistIdle;
    }
    if (const int off = child(sd, a, "Rectangle_OFF"); off >= 0) sc.hide(off);
    if (const int glow = child(sd, a, "Glow"); glow >= 0) {
      if (lit) sc.at(glow).alpha = 1.0f;
      else sc.hide(glow);
    }
  };
  assist("HUD_ABS", st.abs_enabled, st.abs, st.abs_flash_timer);
  assist("HUD_TCS", st.tcs_enabled, st.tcs, st.tcs_flash_timer);
  assist("HUD_STM", st.stm_enabled, st.stm, st.stm_flash_timer);
  sc.hide(child(sd, assists, "HUD_SHIFT_LIGHT"));
  sc.hide(child(sd, gauge, "FUEL_ANALOG"));
  return sc.draw(l, f, gauge) > 0;
}

}  // namespace carview
