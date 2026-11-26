#include "hud_screen.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game_hud.h"
#include "scene_timeline.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr uint32_t kWhite = ui::rgba(255, 255, 255);
// DIRECTION_ARROWS' material (Material14584, 0.91 grey).
constexpr uint32_t kDirectionArrowInk = ui::rgba(232, 232, 232);

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

// The x offset that puts text `t`'s left edge at canvas `left` in its node's
// box (left, centre or right aligned).
float left_at(const SceneDraw& sd, int node, std::string_view t, float left) {
  if (node < 0) return 0.0f;
  const float w = sd.measure(node, t), x = sd.at(node)[0];
  switch (sd.style(node, {}, 0).align) {
    case ui::Align::kCenter: return left + 0.5f * w - x;
    case ui::Align::kRight: return left + w - x;
    default: return left - x;
  }
}

// Shows `n` and every node under it that the scene leaves at alpha 0 (a
// script fades them in), at `alpha`.
void show_all(SceneScreen& sc, int n, float alpha = 1.0f) {
  if (n < 0) return;
  sc.show(n, alpha);
  for (int c : sc.scene().children_of(n)) show_all(sc, c, alpha);
}

// Gives every node under `n` (not `n` itself) its own alpha 1 where the
// scene rests it at 0 and the timeline does not set it: unlike show_all, the
// parents' opacity (a slide's fade on `n`) still multiplies.
void raise_all(SceneScreen& sc, int n) {
  if (n < 0) return;
  for (int c : sc.scene().children_of(n)) {
    auto& a = sc.bindings().anim[c];
    if (!a.opacity) a.opacity = 1.0f;
    raise_all(sc, c);
  }
}

// Puts the component whose root node is `root` on `slide`, `ms` into it, on
// `tl` (kept by the caller across frames) and writes the timeline's look into
// the frame. Entering a slide bakes and restarts its curves, so a slide is
// entered once and then scrubbed. False without the scene's timeline, the
// component or the slide.
bool play_slide(SceneScreen& sc, SceneTimeline& tl, int root, std::string_view slide, double ms) {
  const SceneDraw& sd = sc.scene();
  tl.bind(sd.scene());
  if (!tl.ok() || !scene_timelines_enabled() || root < 0 || slide.empty()) return false;
  const int comp = tl.item_of_node(root);
  if (comp < 0 || sd.scene()->bgf.find_slide(uint32_t(comp), slide) < 0) return false;
  if (tl.slide(uint32_t(comp)) != slide) tl.go_to(uint32_t(comp), slide);
  tl.set_time(uint32_t(comp), float(ms));
  SceneBindings all;
  tl.apply(all);
  for (const auto& [n, a] : all.anim) sc.bindings().anim[n] = a;
  return true;
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
// The timers' grey tapes (the race-hud-notification oracle).
constexpr float kTimerTapeAlpha = 0.45f;
// The player's standings row.
constexpr uint32_t kPlayerRow = ui::rgba(0xFF, 0x03, 0x61);

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

// --- Race blocks ----------------------------------------------------------

std::string hud_clock(double seconds) {
  const long long ms = std::llround(std::max(0.0, seconds) * 1000.0);
  char b[32];
  std::snprintf(b, sizeof b, "%02lld:%02lld.%03lld", ms / 60000, ms / 1000 % 60, ms % 1000);
  return b;
}

namespace {

// The standings' tapes end at the place column and start this far left of
// the widest name (16_race_hud_orig.png and the race-hud-notification
// oracle: 38 px).
constexpr float kStandingsRight = 1213.0f;
constexpr float kStandingsPad = 38.0f;
// A sprint's progress: the percentage from x 68, its label 15 px after it.
constexpr float kEndConLeft = 68.0f, kEndConGap = 15.0f;
// The popularity disc's fill along its groove (the banked oracle).
constexpr float kDiscFillRadius = 31.0f, kDiscFillWidth = 5.0f;
// The fill runs clockwise from the groove's open end (7:30) round 270 degrees.
constexpr float kDiscFillStart = -2.356f, kDiscFillSweep = 4.712f;
// The event prompt's stacker, measured on 09_event_marker_prompt_orig.png
// (the same as 30_skill_toast_orig.png): the title, name and call to
// action lines' offsets from where the scene lays them, and the tapes'
// ends.
// GO!: its place and size on 15b_countdown_go_orig.png, and the fade out.
constexpr float kGoDx = -6.0f, kGoDy = -1.0f, kGoScale = 1.26f;
constexpr double kGoFade = 0.35;
constexpr float kPromptTitleDx = -27.0f, kPromptTitleDy = -1.0f;
constexpr float kPromptNameDx = -19.0f, kPromptNameDy = -1.0f;
// The name line draws a little larger than laid out.
constexpr float kPromptLineScale = 1.07f;
// The type icon sits lower and larger than laid out.
constexpr float kPromptIconDx = 3.0f, kPromptIconDy = 7.0f, kPromptIconScale = 1.15f;
constexpr float kPromptCtaDx = 101.0f, kPromptCtaDy = 57.0f;
constexpr float kPromptTapePad = 10.0f, kPromptPillLead = 4.0f, kPromptPillTail = 30.0f;

void bind_end_condition(SceneScreen& sc, const RaceHud& r) {
  const SceneDraw& sd = sc.scene();
  const int block = sc.node("EndConditionBlock");
  const int cur = child(sd, block, "TEXT_ENDCON_CURRENT"), slash = child(sd, block, "TEXT_ENDCON_SLASH");
  const int fin = child(sd, block, "TEXT_ENDCON_FINAL"), units = child(sd, block, "TEXT_ENDCON_UNITS");
  if (r.circuit) {
    const bool racing = r.phase == RaceHud::kRacing;
    sc.text(cur, std::to_string(racing ? std::max(1, std::min(r.lap, r.laps)) : 0));
    sc.text(slash, "/");
    sc.text(fin, std::to_string(r.laps));
    sc.text(units, text_of(hud_text::kLaps));
  } else {
    // Sprints: "0%" in the big box, PROGRESS after it (the block's stacker
    // packs them; measured on 16_race_hud_orig.png).
    const std::string pct = std::to_string(int(std::lround(std::clamp(r.progress, 0.0, 1.0) * 100))) + "%";
    const std::string label = text_of(hud_text::kProgress);
    sc.text(cur, pct);
    sc.hide(slash);
    sc.hide(fin);
    sc.text(units, label);
    if (cur >= 0) sc.at(cur).dx = left_at(sd, cur, pct, kEndConLeft);
    if (units >= 0) sc.at(units).dx = left_at(sd, units, label, kEndConLeft + sd.measure(cur, pct) + kEndConGap);
  }
}

void bind_place(SceneScreen& sc, const RaceHud& r) {
  const SceneDraw& sd = sc.scene();
  const int block = sc.node("PlaceBlock");
  sc.text(child(sd, block, "TEXT_PLACE_LABEL"), text_of(hud_text::kPlace));
  sc.text(child(sd, block, "TEXT_PLACE"), std::to_string(std::max(1, r.place)));
  sc.text(child(sd, block, "TEXT_PLACE_SLASH"), "/");
  sc.text(child(sd, block, "TEXT_PLACE_MAX"), std::to_string(std::max(1, r.cars)));
}

// HUDTimes: RACE from the start; on circuits LAP, then LAST and BEST once
// set, each on its grey tape (a sprint's single RACE line has none).
int bind_timers(SceneScreen& sc, const RaceHud& r) {
  const SceneDraw& sd = sc.scene();
  const int lines = child_path(sd, sc.node("HUDTimes"), "HUDTimes");
  if (lines < 0) return -1;
  struct Line {
    std::string title, value;
  };
  std::vector<Line> rows;
  rows.push_back({text_of(hud_text::kRace), hud_clock(r.clock)});
  if (r.circuit) {
    rows.push_back({text_of(hud_text::kLap), hud_clock(r.lap_time)});
    if (r.last_lap > 0) rows.push_back({text_of(hud_text::kLast), hud_clock(r.last_lap)});
    if (r.best_lap > 0) rows.push_back({text_of(hud_text::kBest), hud_clock(r.best_lap)});
  }
  // The engine's lap split (an extra) on the next line.
  const bool derived = !r.has_lap_split_delta && r.circuit && r.best_lap > 0.0 && r.last_lap > 0.0 &&
                       std::fabs(r.last_lap - r.best_lap) > 0.004;
  const double delta = r.has_lap_split_delta ? r.lap_split_delta : r.last_lap - r.best_lap;
  int split_line = -1;
  if (r.phase == RaceHud::kRacing && (r.has_lap_split_delta || derived)) {
    split_line = int(rows.size());
    rows.push_back({text_of(hud_text::kSplit), format_split_delta(delta)});
  }
  for (int i = 0; i < 5; ++i) {
    const std::string n = std::to_string(i + 1);
    const int line = child(sd, lines, "LINE" + n);
    if (line < 0) continue;
    if (i >= int(rows.size())) {
      sc.hide(line);
      continue;
    }
    sc.show(line);
    const int value_group = child(sd, line, "value");
    const int title = child(sd, line, "TEXT_TITLE_" + n);
    const int value = child(sd, value_group, "TEXT_VALUE_" + n);
    sc.show(title);
    sc.show(value_group);
    sc.show(value);
    sc.text(title, rows[size_t(i)].title);
    sc.text(value, rows[size_t(i)].value);
    if (i == split_line && value >= 0) sc.at(value).tint = delta <= 0.0 ? ui4::kGreen : ui4::kRed;
    const int bar = child(sd, line, "Bar");
    if (rows.size() > 1) sc.show(bar, kTimerTapeAlpha);
    else sc.hide(bar);
    // The icons (penalties) are not used.
    for (int c : sd.children_of(value_group))
      if (c != value) sc.hide(c);
  }
  return lines;
}

// PositionIndicators: the leader and the rows round the player, four in all.
void bind_standings(SceneScreen& sc, const RaceHud& r) {
  const SceneDraw& sd = sc.scene();
  const int bars = child(sd, sc.node("PositionIndicators"), "LeaderboardBars");
  const std::vector<int> slots = sd.children_of(bars);
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
  // One tape width for all rows: the widest name's.
  float left = kStandingsRight;
  for (size_t k = 0; k < pick.size() && k < slots.size(); ++k) {
    const int name = child_path(sd, slots[k], "HUDPositionBar/Bar/TEXT_GAMERTAG");
    if (name >= 0) left = std::min(left, sd.at(name)[0] - sd.measure(name, r.rows[pick[k]].name) - kStandingsPad);
  }
  for (size_t k = 0; k < slots.size(); ++k) {
    if (k >= pick.size()) {
      sc.hide(slots[k]);
      continue;
    }
    const RaceHudRow& row = r.rows[pick[k]];
    const int bar = child_path(sd, slots[k], "HUDPositionBar/Bar");
    sc.text(child(sd, bar, "TEXT_GAMERTAG"), row.name);
    sc.text(child(sd, bar, "TEXT_POSITION"), std::to_string(row.place));
    for (int icon : sd.children_of(child(sd, bar, "icon"))) sc.hide(icon);
    const int tape = child_path(sd, bar, "Bar/Bar");
    if (tape >= 0) {
      sc.at(tape).dx = left - sd.rect(tape)[0];
      sc.at(tape).visible_width = kStandingsRight - left;
      if (row.player) {
        sc.at(tape).tint = kPlayerRow;
        sc.at(tape).alpha = 1.0f;
      }
    }
    // The time box (a gap or lap time column) is not shown in a race.
    sc.hide(child(sd, slots[k], "HUDPositionTime"));
  }
}

}  // namespace

bool draw_hud_countdown(ui::List& l, const RaceHud& r, float w, float h, float s) {
  SceneScreen sc(kHudScene);
  const SceneDraw& sd = sc.scene();
  const int root = sc.node("I_321_Go"), group = child(sd, root, "Group");
  if (root < 0 || group < 0) return false;
  sc.bindings().fit_text = false;
  sc.hide(child(sd, group, "Paused"));
  // No count on screen at a gantry: its LED panels count (Q74).
  // interim (SPEC_QUESTIONS Q173): a start without one (a street race's open
  // road) counts 3, 2, 1 on TEXT_COUNT, the node gantry starts leave empty.
  if (!r.start_gantry && r.phase == RaceHud::kCountdown && r.clock < 0.0 && r.clock >= -3.0) {
    const int tc = child(sd, group, "TEXT_COUNT");
    if (tc < 0) return true;
    const int count = std::clamp(r.digit > 0 ? r.digit : int(std::ceil(-r.clock)), 1, 3);
    sc.hide(child(sd, group, "GO"));
    sc.show(tc);
    sc.text(tc, std::to_string(count));
    return sc.draw(l, hud_frame(HudAnchor::kCentre, w, h, s), root) >= 0;
  }

  if (r.phase != RaceHud::kRacing || r.clock < 0.0 || r.clock >= kGoSeconds) return true;
  // GO! over the road, fading out (its tape stays at the scene's alpha 0,
  // as on 15b_countdown_go_orig.png).
  sc.bindings().opacity = float(std::clamp((kGoSeconds - r.clock) / kGoFade, 0.0, 1.0));
  sc.hide(child(sd, group, "TEXT_COUNT"));
  const int go = child(sd, group, "GO");
  sc.show(go);
  sc.text(go, text_of(hud_text::kGo));
  sc.at(go).dx = kGoDx, sc.at(go).dy = kGoDy;
  sc.at(go).scale = kGoScale;
  return sc.draw(l, hud_frame(HudAnchor::kCentre, w, h, s), root) >= 0;
}

bool draw_hud_wrong_way(ui::List& l, const RaceHud& r, float w, float h, float s, float dy) {
  SceneScreen sc(kHudScene);
  const SceneDraw& sd = sc.scene();
  const int root = sc.node("WrongWay"), sign = child(sd, root, "Sign_Top");
  if (root < 0 || sign < 0) return false;
  if (!r.wrong_way) return true;
  show_all(sc, root);
  sc.bindings().dy = dy;
  return sc.draw(l, hud_frame(HudAnchor::kTop, w, h, s), root) >= 0;
}

bool draw_hud_race(ui::List& l, const RaceHud& r, float w, float h, float s, bool timers) {
  SceneScreen sc(kHudScene);
  const int endcon = sc.node("EndConditionBlock"), place = sc.node("PlaceBlock");
  const int times = sc.node("HUDTimes"), standings = sc.node("PositionIndicators");
  if (endcon < 0 || place < 0 || times < 0 || standings < 0) return false;
  sc.bindings().fit_text = false;
  bind_end_condition(sc, r);
  bind_place(sc, r);
  const SceneFrame tl = hud_frame(HudAnchor::kTopLeft, w, h, s), tr = hud_frame(HudAnchor::kTopRight, w, h, s);
  sc.draw(l, tl, endcon);
  sc.draw(l, tr, place);
  if (timers) {
    const int lines = bind_timers(sc, r);
    if (lines >= 0) sc.draw(l, tl, lines);
  }
  if (!r.rows.empty()) {
    bind_standings(sc, r);
    sc.draw(l, tr, standings);
  }
  return true;
}

// --- DrivingSkill and PopularityBar ---------------------------------------

namespace {

// The skill chain's yellow (TEXT_TOTAL, SkillValue) and the popularity
// disc's text (16_race_hud_orig.png, the race-hud-skill-banked oracle).
constexpr uint32_t kSkillYellow = ui::rgba(0xDA, 0xFF, 0x00);
constexpr uint32_t kDiscInk = ui::rgba(20, 20, 22);
constexpr uint32_t kLostRed = ui::rgba(236, 52, 44);
// The rows' stackers centre them on x 640; the gaps between their texts
// (measured on the same captures).
constexpr float kScoreGap = 6.0f, kSkillGap = 4.0f;
// How long the banked total and the disc stay up, and the lost chain.
constexpr double kBankSeconds = 3.5, kLostSeconds = 2.2, kRankSeconds = 4.0;
// With ScoreGroup's slides: the banked total holds until HIDE at 3 s (the
// measured fade's start); a lost chain's ABORT runs its 0.6 s timer, then
// HIDE; SHOW is over by 0.3 s.
constexpr double kBankHoldSeconds = 3.0, kScoreAbortSeconds = 0.6, kScoreShowMs = 300.0;

// Packs texts on one row centred on canvas x `cx` by their left edges.
void pack_row(SceneScreen& sc, const std::vector<std::pair<int, std::string>>& items, float cx,
              const std::vector<float>& gaps) {
  const SceneDraw& sd = sc.scene();
  float total = 0.0f;
  for (size_t i = 0; i < items.size(); ++i) total += sd.measure(items[i].first, items[i].second) * 1.0f;
  for (size_t i = 0; i + 1 < items.size(); ++i) total += i < gaps.size() ? gaps[i] : 0.0f;
  float x = cx - 0.5f * total;
  for (size_t i = 0; i < items.size(); ++i) {
    const int n = items[i].first;
    if (n < 0) continue;
    sc.text(n, items[i].second);
    sc.at(n).dx = left_at(sd, n, items[i].second, x);
    x += sd.measure(n, items[i].second) + (i < gaps.size() ? gaps[i] : 0.0f);
  }
}

}  // namespace

std::string skill_multiplier_text(const SkillChainHud& sk) {
  const float m = sk.multiplier_f > 0.0f ? sk.multiplier_f : float(std::max(1, sk.multiplier));
  char b[16];
  std::snprintf(b, sizeof b, "%.1f", m);
  return b;
}

bool draw_hud_skills(ui::List& l, const SkillChainHud& sk, float w, float h, float s, double time) {
  SceneScreen sc(kHudScene);
  const SceneDraw& sd = sc.scene();
  const int skill = sc.node("DrivingSkill"), pop = sc.node("PopularityBar");
  const int score = child_path(sd, skill, "CompleteComponent/ScoreGroup/anim_helper");
  const int skill1 = child_path(sd, skill, "CompleteComponent/SkillGroup/Skill1/anim_helper/Skill");
  if (skill < 0 || pop < 0 || score < 0 || skill1 < 0) return false;
  sc.bindings().fit_text = false;
  const SceneFrame f = hud_frame(HudAnchor::kTop, w, h, s);
  const double since_bank = time - sk.banked_at, since_lost = time - sk.lost_at, since_rank = time - sk.rank_up_at;
  const bool banked = since_bank >= 0 && since_bank < kBankSeconds;
  const bool lost = since_lost >= 0 && since_lost < kLostSeconds;
  const bool rank_up = (since_rank >= 0 && since_rank < kRankSeconds) ||
                       (sk.pop_rank_anim_t >= 0.0 && sk.pop_rank_anim_t < 0.999);
  if (!sk.active && !banked && !lost && !rank_up) return true;

  // The chain: the score and multiplier, the last skill and its points.
  const int mult = child(sd, score, "Multiplier");
  const int mult_h = child(sd, mult, "anim_helper");
  const int total_n = child_path(sd, score, "Total/anim_helper/TEXT_TOTAL");
  const int x_n = child(sd, mult_h, "TEXT_X"), m_n = child_path(sd, mult_h, "anim_helper/TEXT_MULTIPLIER");
  const float cx = 640.0f;
  // ScoreGroup's slides: SHOW as a chain starts, ABORT as it is lost (its
  // 0.6 s timer) then HIDE, and HIDE as the banked total goes (Q243).
  static SceneTimeline score_tl;
  static bool was_active = false;
  static double chain_started = -1e9;
  // A chain starts with its first skill: the last one as it goes active.
  if (sk.active && (!was_active || time < chain_started))
    chain_started = sk.last_skill_at > -1e8 && sk.last_skill_at <= time ? sk.last_skill_at : time;
  was_active = sk.active;
  std::string_view score_slide;
  double score_ms = 0.0;
  if (sk.active) {
    score_slide = "SHOW", score_ms = (time - chain_started) * 1000.0;
  } else if (lost) {
    score_slide = since_lost < kScoreAbortSeconds ? "ABORT" : "HIDE";
    score_ms = (since_lost < kScoreAbortSeconds ? since_lost : since_lost - kScoreAbortSeconds) * 1000.0;
  } else if (banked) {
    score_slide = since_bank < kBankHoldSeconds ? "SHOW" : "HIDE";
    score_ms = since_bank < kBankHoldSeconds ? kScoreShowMs : (since_bank - kBankHoldSeconds) * 1000.0;
  }
  if (sk.active || lost || banked) {
    const bool played =
        play_slide(sc, score_tl, child_path(sd, skill, "CompleteComponent/ScoreGroup"), score_slide, score_ms);
    if (played) raise_all(sc, score);
    else show_all(sc, score);
    for (int c : {child(sd, mult_h, "Background"), child(sd, mult_h, "Smudge_Tape"), child(sd, mult, "Flash"),
                  child_path(sd, score, "Total/Flash")})
      sc.hide(c);
    const int shown = banked && !sk.active ? sk.banked_total : sk.chain_points;
    const std::string total = thousands(shown);
    if (sk.active) {
      pack_row(sc, {{total_n, total}, {x_n, "X"}, {m_n, skill_multiplier_text(sk)}}, cx, {kScoreGap, 0.0f});
    } else {
      sc.hide(mult);
      pack_row(sc, {{total_n, total}}, cx, {});
    }
    if (total_n >= 0) sc.at(total_n).tint = lost && !sk.active ? kLostRed : kSkillYellow;
    // Without the slides, the measured fades (an alpha override, which the
    // slides' fade on the group would not reach).
    if (!played) {
      const float fade = lost && !sk.active     ? 1.0f - float(std::clamp((since_lost - 1.5) / 0.7, 0.0, 1.0))
                         : banked && !sk.active ? 1.0f - float(std::clamp((since_bank - 3.0) / 0.5, 0.0, 1.0))
                                                : 1.0f;
      for (int n : {total_n, x_n, m_n})
        if (n >= 0) sc.at(n).alpha = fade;
    }
  } else {
    sc.hide(score);
  }
  const int skill_group = child_path(sd, skill, "CompleteComponent/SkillGroup");
  sc.hide(child_path(sd, skill_group, "Skill2"));
  if (sk.active && !sk.last_skill.empty()) {
    show_all(sc, skill1);
    for (int c : {child(sd, skill1, "Background"), child(sd, skill1, "Smudge_Tape"), child(sd, skill1, "ComboIcon")})
      sc.hide(c);
    sc.hide(child_path(sd, skill_group, "Skill1/Flash"));
    const int name = child(sd, skill1, "TEXT_POP_SKILLNAME"), value = child(sd, skill1, "SkillValue");
    pack_row(sc, {{name, sk.last_skill}, {value, std::to_string(sk.last_points)}}, cx, {kSkillGap});
    if (value >= 0) sc.at(value).tint = kSkillYellow;
  } else {
    sc.hide(skill_group);
  }
  sc.draw(l, f, skill);

  // The popularity disc: shown when a chain banks or the rank changes.
  if (banked || rank_up) {
    const PopularityBadgeAnimFrame anim = evaluate_popularity_badge_anim(sk, time);
    const int value_type = child_path(sd, pop, "ValueScale/ValueType");
    const int level = child(sd, value_type, "TEXT_LEVEL"), suffix = child(sd, value_type, "TEXT_LEVEL_SUFFIX");
    const int rank = std::max(1, anim.display_rank);
    sc.text(level, std::to_string(rank));
    sc.text(suffix, place_suffix(rank));
    if (level >= 0) sc.at(level).tint = kDiscInk;
    if (suffix >= 0) {
      sc.at(suffix).tint = kDiscInk;
      // The suffix follows the number (its stacker).
      sc.at(suffix).dx = sd.at(level)[0] - sd.at(suffix)[0] + 1.0f;
    }
    sc.hide(child_path(sd, pop, "ValueScale/Stars"));
    sc.hide(child(sd, pop, "Bar"));
    for (int c : {child(sd, pop, "CircleFlash"), child(sd, pop, "Flash_SmokeRing")})
      if (c >= 0) sc.at(c).alpha = anim.ring_flash_alpha;
    sc.hide(child(sd, pop, "Flash_SmokeRing2"));
    const float fade = banked ? 1.0f - float(std::clamp((since_bank - 3.0) / 0.5, 0.0, 1.0)) : 1.0f;
    sc.bindings().opacity = fade;
    sc.draw(l, f, pop);
    // The ring's fill (the scene's quarter pieces are turned by the game; an
    // arc along the same groove here).
    const auto c = sd.at(pop);
    const float r = kDiscFillRadius * f.s, a0 = kDiscFillStart;
    l.arc(f.x(c[0]), f.y(c[1]), r, kDiscFillWidth * f.s, a0, a0 + anim.radial_progress * kDiscFillSweep,
          ui::fade(kSkillYellow, fade), true);
  }
  return true;
}

// --- ActivationPrompt -----------------------------------------------------

std::string event_sponsor(int64_t event_id) {
  const std::string id = "IDS_Sponsor_" + std::to_string(event_id);
  const auto t = lookup(StringRef{"Events", id, 0, {}});
  return t ? *t : std::string();
}

std::string event_name_without_sponsor(std::string name, std::string_view sponsor) {
  if (sponsor.empty() || name.size() <= sponsor.size()) return name;
  for (size_t i = 0; i < sponsor.size(); ++i)
    if (std::toupper(static_cast<unsigned char>(name[i])) != std::toupper(static_cast<unsigned char>(sponsor[i])))
      return name;
  size_t cut = sponsor.size();
  while (cut < name.size() && name[cut] == ' ') ++cut;
  return name.substr(cut);
}

namespace {

std::string upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

bool hud_prompt_available() {
  const SceneScreen sc(kHudScene);
  return sc.node("ActivationPrompt") >= 0;
}

bool draw_hud_prompt(ui::List& l, const WorldMarkerHud& wm, float w, float h, float s) {
  SceneScreen sc(kHudScene);
  const SceneDraw& sd = sc.scene();
  const int prompt = sc.node("ActivationPrompt");
  const int all = child(sd, prompt, "All");
  const int name_group = child(sd, all, "Event_Name"), cta = child(sd, all, "Call_to_Action");
  const int title = child(sd, all, "Title");
  if (prompt < 0 || name_group < 0 || cta < 0 || title < 0) return false;
  sc.bindings().fit_text = false;

  // The title line (a sponsor's "HORIZON PRESENTS"), the event's name with
  // its type icon, and the call to action under them: the group's stacker
  // packs the three (measured on 09_event_marker_prompt_orig.png).
  if (!wm.sponsor.empty()) {
    show_all(sc, title);
    sc.text(child(sd, title, "Text"), upper(wm.sponsor));
    sc.hide(child(sd, title, "Tape"));
    sc.at(title).dx = kPromptTitleDx;
    sc.at(title).dy = kPromptTitleDy;
  } else {
    sc.hide(title);
  }
  const int name_text = child(sd, name_group, "Text");
  sc.text(name_text, upper(wm.title));
  sc.at(name_group).dx = kPromptNameDx;
  sc.at(name_group).dy = kPromptNameDy;
  sc.at(name_group).scale = kPromptLineScale;
  if (const int icon_group = child(sd, name_group, "Icon"); icon_group >= 0) {
    sc.at(icon_group).dx = kPromptIconDx;
    sc.at(icon_group).dy = kPromptIconDy;
    sc.at(icon_group).scale = kPromptIconScale;
    if (const int icon = child(sd, icon_group, "DEFAULT"); icon >= 0 && !wm.icon.empty()) sc.at(icon).texture = wm.icon;
  }
  // The name's tape runs from the icon to past the name.
  if (const int tape = child(sd, name_group, "Tape"); tape >= 0) {
    for (int c : sd.children_of(tape)) {
      if (sd.object_name(c) == "Dirt_Tile") continue;
      const float right = sd.at(name_text)[0] + sd.measure(name_text, upper(wm.title)) + kPromptTapePad;
      sc.at(c).visible_width = std::max(0.0f, right - sd.rect(c)[0]);
    }
  }
  const int locked = child(sd, cta, "Locked"), def = child(sd, cta, "Default");
  const int on = wm.eligible ? def : locked;
  sc.hide(wm.eligible ? locked : def);
  const int cta_text = child(sd, on, "Text");
  sc.text(cta_text, upper(wm.prompt));
  sc.at(cta).dx = kPromptCtaDx;
  sc.at(cta).dy = kPromptCtaDy;
  if (const int bar = child(sd, on, "Bar"); bar >= 0 && cta_text >= 0) {
    // The pill ends a little past the right-aligned text and starts before
    // its button.
    const float left = sd.at(cta_text)[0] - sd.measure(cta_text, upper(wm.prompt)) - kPromptPillLead;
    sc.at(bar).dx = left - sd.rect(bar)[0];
    sc.at(bar).visible_width = sd.at(cta_text)[0] + kPromptPillTail - left;
  }
  if (wm.eligible) {
    // The Activate binding: the pad's X button art, else its own glyph (the
    // E key on a keyboard, Square on a PlayStation pad), not a bare "X".
    if (const int glyph = child_path(sd, def, "Icon/States/Icon"); glyph >= 0)
      sc.at(glyph).texture =
          ui4::glyph_set() == ui4::GlyphSet::kXbox ? "CONTROLLER/X_BUTTON" : "CONTROLLER/ACTIVATE";
    sc.hide(child_path(sd, def, "Icon/Rectangle"));
  }
  return sc.draw(l, hud_frame(HudAnchor::kTop, w, h, s), prompt) > 0;
}

// --- DirectionArrows -------------------------------------------------------

DirectionArrowCell direction_arrow_cell(int turn_kind) {
  switch (turn_kind) {
    case 2: return {1, true};   // bear left
    case 3: return {2, true};   // turn left
    case 4: return {3, true};   // sharp left
    case 5: return {1, false};  // bear right
    case 6: return {2, false};  // turn right
    case 7: return {3, false};  // sharp right
    case 8: return {4, false};  // U-turn
    case 9: return {5, false};  // arrived
    default: return {0, false};
  }
}

bool draw_hud_direction_arrow(ui::List& l, const GpsHud& gps, float w, float h, float s) {
  SceneScreen sc(kHudScene);
  const SceneDraw& sd = sc.scene();
  const int root = sc.node("DirectionArrows");
  const int box = sc.node("DirectionArrows/graphics/Component/Background");
  const int arrow = sc.node("DirectionArrows/graphics/Component/Group/bg2");
  if (root < 0 || box < 0 || arrow < 0) return false;
  if (gps.recalculating) return true;
  std::array<float, 4> uv{};
  if (!ui_texture(sd.texture_key_of(arrow), uv)) return false;
  // The box rests at alpha 0 (the scene fades it in); the arrow and the
  // junction text are drawn here.
  sc.show(box);
  sc.hide(arrow);
  sc.hide(sc.node("DirectionArrows/TEXT_JUNCTION"));
  const SceneFrame f = hud_frame(HudAnchor::kTop, w, h, s);
  if (sc.draw(l, f, root) <= 0) return false;
  const DirectionArrowCell c = direction_arrow_cell(gps.turn_kind);
  const float cw = (uv[2] - uv[0]) / float(kDirectionArrowCells);
  float u0 = uv[0] + cw * float(c.cell), u1 = u0 + cw;
  if (c.mirror) std::swap(u0, u1);
  const auto r = sd.rect(arrow);
  l.image(f.x(r[0]), f.y(r[1]), f.x(r[2]), f.y(r[3]), {u0, uv[1], u1, uv[3]}, kDirectionArrowInk);
  return true;
}

// --- Notifications ----------------------------------------------------------

const std::vector<NotifyStrings>& notify_strings() {
  using K = NotifyKind;
  static const std::vector<NotifyStrings> table = {
      {K::kRoadsDriven, {}, {"Activities", "IDS_RoadDiscovered_Title", 0, "ROADS DRIVEN"},
       {"Activities", "IDS_RoadDiscovered_Message", 0, "{0}/{1}"}},
      {K::kDiscountSigns, {}, {"Activities", "IDS_FlyerCollected", 0, "{0}/{1} DISCOUNT SIGNS SMASHED"},
       {"Activities", "IDS_FlyerCollected_Message", 0, "Total upgrades discount: {0}"}},
      {K::kBarnRumour, {}, {"Barnfind", "IDS_Spawned_Body", 0, "NEW BARN FIND RUMOR"}, {}},
      {K::kBarnRestoring, {}, {"Activities", "IDS_BarnfindBeingRestored_Message", 0, "{0} IS BEING RESTORED"},
       {"Activities", "IDS_BarnfindBeingRestored_Title", 0, "DAK WILL CALL YOU WHEN IT'S READY"}},
      {K::kBarnReady, {}, {"Barnfind", "IDS_Ready_Message", 0, "BARN FIND READY - {0}"}, {}},
      {K::kOutpostDiscovered, {}, {"Missions", "IDS_DiscoverGasStation_Title", 0, "HORIZON OUTPOST DISCOVERED"},
       {"Missions", "IDS_DiscoverGasStation_Msg1", 0, "NEW FAST TRAVEL DESTINATION UNLOCKED"}},
      {K::kSpeedStuntDone, {}, {"GasStation", "IDS_SpeedStuntCompleted", 0, "SPEED STUNT COMPLETED"},
       {"GasStation", "IDS_MissionCompletedDiscount", 0, "FAST TRAVEL DISCOUNT EARNED"}},
      {K::kSkillStuntDone, {}, {"GasStation", "IDS_PrStuntCompleted", 0, "SKILL STUNT COMPLETED"},
       {"GasStation", "IDS_MissionCompletedDiscount", 0, "FAST TRAVEL DISCOUNT EARNED"}},
      {K::kPhotoShootDone, {}, {"GasStation", "IDS_PhotoShootCompleted", 0, "PHOTO SHOOT COMPLETED"},
       {"GasStation", "IDS_MissionCompletedDiscount", 0, "FAST TRAVEL DISCOUNT EARNED"}},
      {K::kSpeedStuntFailed, {}, {"InGame", "IDS_SpeedStuntFail", 0, "SPEED STUNT"},
       {"Missions", "IDS_SpeedStuntFailTimeout", 0, "YOU FAILED TO SET A SPEED"}},
      // The engine's stunts fail on time alone, so the skill stunt and the
      // photo shoot say TRY AGAIN rather than their shortfall or wreck lines.
      {K::kSkillStuntFailed, {}, {"InGame", "IDS_PRStuntFail", 0, "PR STUNT"},
       {"InGame", "IDS_MissionFail", 0, "TRY AGAIN"}},
      {K::kPhotoShootFailed, {}, {"InGame", "IDS_PhotoShootFail", 0, "PHOTO SHOOT"},
       {"InGame", "IDS_MissionFail", 0, "TRY AGAIN"}},
      {K::kShowcaseUnlocked, {}, {"InGame", "IDS_ShowcaseEventUnlocked_Message", 0, "SHOWCASE EVENT UNLOCKED!"},
       {"InGame", "IDS_ShowcaseEventUnlocked_CallToAction", 0, "Show me!"}},
      {K::kSponsorRank, {"SponsorChallengeScreen", "IDS_RankUnlocked", 0, "RANK {0} UNLOCKED!"},
       {"UnitStrings", "IDS_Units_Credits", 0, "{0} Cr"}, {}, NotifyIcon::kSponsor},
      {K::kWristbandAvailable, {}, {"InGame", "IDS_WristbandNewAvailable", 0, "NEW WRISTBAND AT RACE CENTRAL"}, {}},
  };
  return table;
}

const NotifyStrings* notify_strings(NotifyKind kind) {
  for (const auto& e : notify_strings())
    if (e.kind == kind) return &e;
  return nullptr;
}

std::optional<std::string> format_args(std::string_view fmt, const std::vector<std::string>& args) {
  std::string out;
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] == '{' && i + 2 < fmt.size() && fmt[i + 1] >= '0' && fmt[i + 1] <= '9' && fmt[i + 2] == '}') {
      const size_t a = size_t(fmt[i + 1] - '0');
      if (a >= args.size()) return std::nullopt;
      out += args[a];
      i += 2;
    } else {
      out += fmt[i];
    }
  }
  return out;
}

NotifyText notify_text(NotifyKind kind, const std::vector<std::string>& tag_args,
                       const std::vector<std::string>& title_args, const std::vector<std::string>& message_args) {
  NotifyText out;
  const NotifyStrings* e = notify_strings(kind);
  if (!e) return out;
  auto line = [](const StringRef& ref, const std::vector<std::string>& args) {
    if (ref.table.empty() && ref.fallback.empty()) return std::string();
    return format_args(text_of(ref), args).value_or(std::string());
  };
  out.tag = line(e->tag, tag_args);
  out.title = line(e->title, title_args);
  out.message = line(e->message, message_args);
  return out;
}

namespace {

// CombinedNotification's packing (its stackers are not run), measured on
// the race-hud-notification oracle: the panel's crop (its texture clear for
// 3 px at the left and feathered over 8 px at the right) runs from 12 px
// left of the 60 px icon to 20 px past the longer line, centred on x 644;
// the lines start 10 px after the icon. The tag's pill pads its text 8 px;
// both draw 8 px higher than laid out.
constexpr float kNotifyIconW = 60.0f, kNotifyPadL = 12.0f, kNotifyGap = 10.0f, kNotifyPadR = 20.0f;
constexpr float kNotifyCentreX = 644.0f;
constexpr float kNotifyTagPad = 8.0f, kNotifyTagDy = -8.0f;
// The sponsor's logo tile draws wider than its texture's own shape (60 by
// 38 px on the oracle), 2 px right of the icon's middle.
constexpr float kNotifyLogoStretch = 1.33f, kNotifyLogoDx = 2.0f;
// The height one takes in the bottom stack (the panel, 64 px, and a gap),
// and the tag's extra.
constexpr float kNotifyStep = 72.0f, kNotifyTagStep = 22.0f;
// Fade in and out (the original slides its tape in; a fade here).
constexpr double kNotifyIn = 0.25, kNotifyOut = 0.5;

float notify_alpha(double age, double life) {
  if (age < 0.0 || age > life) return 0.0f;
  const float in = float(std::clamp(age / kNotifyIn, 0.0, 1.0));
  const float out = float(std::clamp((life - age) / kNotifyOut, 0.0, 1.0));
  return std::min(in, out);
}

// The panel's own SHOW and HIDE slides (CombinedNotification's component,
// docs/UI_SCENE_ANIMATION.md): SHOW fades its State in over 0.166 s while it
// grows from 0.9 past 1.03 to 1 at 0.218 s; HIDE fades it out and shrinks it
// to 0.8 over 0.002..0.137 s, ending the notification's life. Writes the
// State node's look into the frame; false without the scene's timeline.
bool notify_state_anim(SceneScreen& sc, int root, int state, double age, double life) {
  static SceneTimeline tl;
  tl.bind(sc.scene().scene());
  if (!tl.ok() || !scene_timelines_enabled() || root < 0 || state < 0) return false;
  const int comp = tl.item_of_node(root);
  const auto& d = sc.scene().scene()->bgf;
  if (comp < 0) return false;
  const int hide = d.find_slide(uint32_t(comp), "HIDE");
  if (hide < 0 || d.find_slide(uint32_t(comp), "SHOW") < 0) return false;
  // HIDE's length: its curves' last key.
  float hide_ms = 0.0f;
  for (const auto& tr : d.slides[size_t(hide)].tracks)
    for (const auto& r : tr.refs)
      if (!r.action && r.enabled && r.index < d.curves.size() && !d.curves[r.index].keys.empty())
        hide_ms = std::max(hide_ms, d.curves[r.index].keys.back().time_ms);
  const double left_ms = (life - age) * 1000.0;
  // Enter a slide once (entering bakes and restarts its curves), then scrub.
  const char* want = left_ms <= hide_ms ? "HIDE" : "SHOW";
  if (tl.slide(uint32_t(comp)) != want) tl.go_to(uint32_t(comp), want);
  tl.set_time(uint32_t(comp), float(left_ms <= hide_ms ? hide_ms - left_ms : age * 1000.0));
  SceneBindings all;
  tl.apply(all);
  if (auto it = all.anim.find(state); it != all.anim.end()) sc.bindings().anim[state] = it->second;
  return true;
}

float draw_popularity_unlock(ui::List& l, const NotifyView& st, float w, float h, float s, float a) {
  SceneScreen sc(kHudScene);
  const SceneDraw& sd = sc.scene();
  const int root = sc.node("PopularityUnlock");
  const int main = child(sd, root, "MainMessage"), show = child(sd, root, "ShowMe");
  if (root < 0 || main < 0) return 0.0f;
  sc.bindings().fit_text = false;
  sc.bindings().opacity = a;
  const int title = child(sd, main, "TEXT_TITLE");
  sc.text(title, st.title);
  // The tape centred under the title.
  if (const int tape = child(sd, main, "Crisp_Tape"); tape >= 0 && title >= 0) {
    const float tw = sd.measure(title, st.title) + 2.0f * kNotifyTagPad * 2.0f;
    sc.at(tape).dx = sd.at(title)[0] - 0.5f * tw - sd.rect(tape)[0];
    sc.at(tape).visible_width = tw;
  }
  // The call to action (Show me!) only when the engine acts on its button.
  if (show >= 0 && st.action && !st.message.empty()) {
    show_all(sc, show);
    const int t = child(sd, show, "TEXT_SHOW");
    sc.text(t, st.message);
    if (const int bar = child(sd, show, "Bar"); bar >= 0 && t >= 0) {
      const float left = sd.rect(child_path(sd, show, "Icon/States/Icon"))[0] - kNotifyTagPad;
      sc.at(bar).dx = left - sd.rect(bar)[0];
      sc.at(bar).visible_width = sd.at(t)[0] + sd.measure(t, st.message) + kNotifyTagPad - left;
    }
    sc.hide(child_path(sd, show, "Icon/Rectangle"));
  } else {
    sc.hide(show);
  }
  return sc.draw(l, hud_frame(HudAnchor::kTop, w, h, s), root) > 0 ? 1.0f : 0.0f;
}

}  // namespace

bool hud_notification_available() {
  const SceneScreen sc(kHudScene);
  return sc.node("CombinedNotification") >= 0;
}

float draw_hud_notification(ui::List& l, const NotifyView& st, float w, float h, float s, float lift) {
  const float a = notify_alpha(st.age, st.life);
  if (st.title.empty() || a <= 0.01f) return 0.0f;
  // The showcase banner sits at the top, clear of the bottom stack.
  if (st.kind == NotifyKind::kShowcaseUnlocked) {
    draw_popularity_unlock(l, st, w, h, s, a);
    return 0.0f;
  }

  SceneScreen sc(kHudScene);
  const SceneDraw& sd = sc.scene();
  const int root = sc.node("CombinedNotification");
  const int tape = child_path(sd, root, "State/DoubleTape");
  if (root < 0 || tape < 0) return 0.0f;
  sc.bindings().fit_text = false;
  // The panel's SHOW / HIDE slides; without them the measured fade.
  if (!notify_state_anim(sc, root, child(sd, root, "State"), st.age, st.life)) sc.bindings().opacity = a;
  sc.bindings().dy = -lift;

  const int t1 = child(sd, tape, "TEXT1"), t2 = child(sd, tape, "TEXT2");
  sc.hide(child(sd, tape, "TEXT3"));
  const bool one_line = st.message.empty();
  const float tw = std::max(sd.measure(t2, st.title), one_line ? 0.0f : sd.measure(t1, st.message));
  const float total = kNotifyPadL + kNotifyIconW + kNotifyGap + tw + kNotifyPadR;
  const float left = kNotifyCentreX - 0.5f * total;
  const float text_x = left + kNotifyPadL + kNotifyIconW + kNotifyGap;
  sc.text(t2, st.title);
  if (t2 >= 0) {
    sc.at(t2).dx = left_at(sd, t2, st.title, text_x);
    // A lone line sits in the middle of the panel.
    if (one_line) sc.at(t2).dy = 0.5f * (sd.at(t1)[1] - sd.at(t2)[1]);
  }
  if (one_line) {
    sc.hide(t1);
  } else {
    sc.text(t1, st.message);
    if (t1 >= 0) {
      sc.at(t1).dx = left_at(sd, t1, st.message, text_x);
      sc.at(t1).tint = kSkillYellow;
    }
  }
  for (const char* p : {"Panel", "Panel2"}) {
    const int panel = child(sd, tape, p);
    if (panel < 0) continue;
    sc.at(panel).dx = left - sd.rect(panel)[0];
    sc.at(panel).visible_width = total;
  }
  // The icon: the sponsor's logo, or the misc or mail icon.
  const int icons = child(sd, tape, "Icons");
  const float icon_cx = left + kNotifyPadL + 0.5f * kNotifyIconW;
  const bool sponsor = st.icon == NotifyIcon::kSponsor;
  for (int c : sd.children_of(icons)) {
    const std::string_view n = sd.object_name(c);
    const bool on = sponsor ? n == "SponsorScaler" : (st.icon == NotifyIcon::kMail ? n == "MailIcon" : n == "MiscIcon");
    if (!on) {
      sc.hide(c);
      continue;
    }
    if (sponsor) show_all(sc, c);
    sc.at(c).dx = icon_cx - sd.at(c)[0];
  }
  if (const int logo = child_path(sd, icons, "SponsorScaler/Sponsor"); sponsor && logo >= 0) {
    if (!st.logo.empty()) sc.at(logo).texture = st.logo;
    sc.at(logo).stretch_x = kNotifyLogoStretch;
    sc.at(logo).dx = kNotifyLogoDx;
  }
  // The tag over the panel (a sponsor's RANK 4 UNLOCKED!).
  const int rank = child(sd, tape, "RankIcon");
  if (!st.tag.empty()) {
    show_all(sc, rank);
    const int bar = child(sd, rank, "Bar");
    const int text = child(sd, bar, "Text"), pill = child(sd, bar, "Bar");
    sc.text(text, st.tag);
    const float ttw = sd.measure(text, st.tag);
    if (text >= 0) {
      sc.at(text).dx = left_at(sd, text, st.tag, 640.0f - 0.5f * ttw);
      sc.at(text).dy = kNotifyTagDy;
      sc.at(text).tint = kDiscInk;
    }
    if (pill >= 0) {
      const float pl = 640.0f - 0.5f * ttw - kNotifyTagPad;
      sc.at(pill).dx = pl - sd.rect(pill)[0];
      sc.at(pill).dy = kNotifyTagDy;
      sc.at(pill).visible_width = ttw + 2.0f * kNotifyTagPad;
    }
  } else {
    sc.hide(rank);
  }
  if (sc.draw(l, hud_frame(HudAnchor::kBottom, w, h, s), root) <= 0) return 0.0f;
  return st.tag.empty() ? kNotifyStep : kNotifyStep + kNotifyTagStep;
}

// --- SpeedCamera -------------------------------------------------------------

namespace {

// SHOW's length (the trap's speed up before its result), and NEWPB's and
// OLDPB's fade out of SpeedGroup (1.8 to 2 s): the panel's time up.
constexpr double kSpeedCamShowMs = 1001.0, kSpeedCamResultMs = 2000.0;
// ABORT fades SpeedGroup out over its first 0.2 s.
constexpr double kSpeedCamAbortMs = 200.0;
// The pills' and tapes' reach past their text (canvas px).
constexpr float kSpeedCamPillPad = 10.0f, kSpeedCamTapePad = 14.0f, kSpeedCamUnitGap = 8.0f;

// Puts `pill` (a two-texture bar) under the centred text `t` drawn as `s`.
void pill_under(SceneScreen& sc, int pill, int t, std::string_view s, float pad) {
  const SceneDraw& sd = sc.scene();
  if (pill < 0 || t < 0) return;
  const float tw = sd.measure(t, s), left = sd.at(t)[0] - 0.5f * tw - pad;
  sc.at(pill).dx = left - sd.rect(pill)[0];
  sc.at(pill).visible_width = tw + 2.0f * pad;
}

// Stackers are not run (docs/UI_SCENE_SCREENS.md): a row group's texts and
// bars sit on the speed's line in the scene and its Stacker where the row
// goes. Moves the row's content (all but the Stacker) onto the Stacker's
// line, plus `extra` canvas px down.
void stack_row(SceneScreen& sc, int group, float extra = 0.0f) {
  const SceneDraw& sd = sc.scene();
  const int stacker = child(sd, group, "Stacker");
  if (group < 0 || stacker < 0) return;
  int text = -1;
  for (int c : sd.children_of(group))
    if (sd.object_name(c) != "Stacker" && sd.object_name(c) != "Bar" && sd.object_name(c) != "Tape" && text < 0)
      text = c;
  if (text < 0) return;
  const float dy = sd.at(stacker)[1] - sd.at(text)[1] + extra;
  for (int c : sd.children_of(group))
    if (c != stacker) sc.at(c).dy += dy;
}

// A tape group's crisp and smudge strips from canvas x `left` to `right`.
void tape_span(SceneScreen& sc, int tape, float left, float right) {
  const SceneDraw& sd = sc.scene();
  for (int c : sd.children_of(tape)) {
    if (sd.object_name(c) == "Dirt_Tile") {
      sc.at(c).dx = left - sd.rect(c)[0];
      continue;
    }
    sc.at(c).dx = left - sd.rect(c)[0];
    sc.at(c).visible_width = std::max(0.0f, right - left);
  }
}

}  // namespace

SpeedCameraPhase speed_camera_phase(const freeroam::SpeedActivityHud& sh) {
  SpeedCameraPhase p;
  if (sh.banner_active) {
    double ms = sh.banner_age_s * 1000.0;
    p.zone = sh.banner_is_zone;
    p.new_pb = sh.banner_new_pb;
    p.speed_mph = sh.banner_speed_mph;
    p.best_mph = sh.banner_best_mph;
    p.title = sh.banner_title;
    // A trap shows its speed (SHOW) before the result; a zone's speed is
    // already up.
    if (!p.zone) {
      if (ms < kSpeedCamShowMs) {
        p.slide = "SHOW";
        p.ms = ms;
        return p;
      }
      ms -= kSpeedCamShowMs;
    }
    if (ms < kSpeedCamResultMs) {
      p.slide = p.new_pb ? "NEWPB" : "OLDPB";
      p.ms = ms;
      return p;
    }
  }
  if (sh.zone_active) {
    const double ms = sh.zone_elapsed_s * 1000.0;
    p.zone = true;
    p.speed_mph = sh.live_avg_mph;
    p.title = sh.active_zone_id;
    p.slide = ms < kSpeedCamShowMs ? "SHOW" : "ON";
    p.ms = ms < kSpeedCamShowMs ? ms : 0.0;
    return p;
  }
  if (sh.abort_active && sh.abort_age_s * 1000.0 < kSpeedCamAbortMs) {
    p.zone = true;
    p.speed_mph = sh.abort_speed_mph;
    p.title = sh.abort_title;
    p.slide = "ABORT";
    p.ms = sh.abort_age_s * 1000.0;
    return p;
  }
  return p;
}

bool hud_speed_camera_available() {
  const SceneScreen sc(kHudScene);
  return sc.node("SpeedCamera") >= 0 && scene_timelines_enabled();
}

bool draw_hud_speed_camera(ui::List& l, const SpeedCameraView& v, float w, float h, float s, float lift) {
  if (v.slide.empty()) return false;
  SceneScreen sc(kHudScene);
  const SceneDraw& sd = sc.scene();
  const int root = sc.node("SpeedCamera");
  const int group = child(sd, root, "SpeedGroup");
  if (root < 0 || group < 0) return false;
  sc.bindings().fit_text = false;
  sc.bindings().dy = -lift;

  // The slide's look: SpeedCamera is its own component in 947_HUD.
  static SceneTimeline tl;
  if (!play_slide(sc, tl, root, v.slide, v.ms)) return false;

  // The camera's name in its pill.
  const int title = child(sd, group, "Title");
  const int title_text = child(sd, title, "Text");
  sc.text(title_text, v.title);
  pill_under(sc, child(sd, title, "Bar"), title_text, v.title, kSpeedCamPillPad);
  stack_row(sc, title);

  // The speed and its unit side by side on one tape, about the group's
  // middle.
  const int speed = child(sd, group, "Speed");
  const int num = child(sd, speed, "speed"), unit = child(sd, speed, "mph");
  sc.text(num, v.speed);
  sc.text(unit, v.unit);
  if (num >= 0 && unit >= 0) {
    const float nw = sd.measure(num, v.speed), uw = sd.measure(unit, v.unit);
    const float total = nw + kSpeedCamUnitGap + uw;
    const float left = sd.at(group)[0] - 0.5f * total;
    sc.at(num).dx = left_at(sd, num, v.speed, left);
    sc.at(unit).dx = left_at(sd, unit, v.unit, left + nw + kSpeedCamUnitGap);
    tape_span(sc, child(sd, speed, "Tape"), left - kSpeedCamTapePad, left + total + kSpeedCamTapePad);
  }

  // The personal best (OLDPB slides it in): its slide puts it on the
  // title's line, so it goes one row further down (Q242).
  const int pb = child(sd, group, "SpeedPB");
  const int pb_text = child(sd, pb, "Text");
  sc.text(pb_text, v.pb);
  pill_under(sc, child(sd, pb, "Bar"), pb_text, v.pb, kSpeedCamPillPad);
  const float row = sd.at(child(sd, title, "Stacker"))[1] - sd.at(child(sd, speed, "Stacker"))[1];
  stack_row(sc, pb, row);

  // NEW PB! in the speed's place (NEWPB pops it in). Its text and tape rest
  // at alpha 0 under the animated group: their own alpha goes to 1, and the
  // group's still fades them.
  const int icon = child(sd, group, "PBIcon");
  raise_all(sc, icon);
  if (const int t = child(sd, icon, "NEW_PB"); t >= 0) {
    sc.text(t, v.new_pb);
    const float tw = sd.measure(t, v.new_pb), left = sd.at(t)[0] - 0.5f * tw;
    if (const int tape = child(sd, icon, "Tape"); tape >= 0)
      tape_span(sc, tape, left - kSpeedCamTapePad, left + tw + kSpeedCamTapePad);
  }

  // No leaderboard or rival data offline: the comparison lines, the summary
  // and the leaderboard prompt stay down (Q242).
  sc.hide(child(sd, root, "CompLines"));
  sc.hide(child(sd, root, "Summary"));
  sc.hide(child(sd, root, "LeaderboardText"));
  return sc.draw(l, hud_frame(HudAnchor::kBottom, w, h, s), root) > 0;
}

}  // namespace carview
