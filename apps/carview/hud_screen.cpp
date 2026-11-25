#include "hud_screen.h"

#include <algorithm>
#include <cctype>
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
  if (sk.active || lost || banked) {
    show_all(sc, score);
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
    const float fade = lost && !sk.active ? 1.0f - float(std::clamp((since_lost - 1.5) / 0.7, 0.0, 1.0))
                       : banked && !sk.active ? 1.0f - float(std::clamp((since_bank - 3.0) / 0.5, 0.0, 1.0))
                                              : 1.0f;
    if (total_n >= 0) {
      sc.at(total_n).tint = lost && !sk.active ? kLostRed : kSkillYellow;
      sc.at(total_n).alpha = fade;
    }
    if (x_n >= 0) sc.at(x_n).alpha = fade;
    if (m_n >= 0) sc.at(m_n).alpha = fade;
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
    if (const int glyph = child_path(sd, def, "Icon/States/Icon"); glyph >= 0) sc.at(glyph).texture = "CONTROLLER/X_BUTTON";
    sc.hide(child_path(sd, def, "Icon/Rectangle"));
  }
  return sc.draw(l, hud_frame(HudAnchor::kTop, w, h, s), prompt) > 0;
}

}  // namespace carview
