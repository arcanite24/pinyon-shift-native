// SPEC-EVENTS and SPEC-RACE-AI: the AI line decoder, route gates and grid,
// the race session, difficulty and rubber-banding on made-up data; with
// SHIFT_GAME_ROOT, the census, route 158's line, FR22's assignment and
// route 143's grid and gate on the disc (and, with SHIFT_ORACLE_LAP, the
// M3 oracle lap against route 158's racing line).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cars/physics.h"
#include "cars/sources.h"
#include "drive/oracle_lap.h"
#include "fh1data/archive.h"
#include "fh1data/stringtables.h"
#include "race/difficulty.h"
#include "race/event.h"
#include "race/owt.h"
#include "race/rally.h"
#include "race/race_line.h"
#include "race/rewards.h"
#include "race/route.h"
#include "race/session.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

void put32(std::vector<uint8_t>& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}
void putf(std::vector<uint8_t>& b, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  put32(b, u);
}

// A made-up OWTM file: points along +z 2 m apart, half width 5 to the left
// (lateral -x: the left of +z), offset 0.2.
std::vector<uint8_t> made_up_owt(uint32_t n, bool closed) {
  std::vector<uint8_t> b = {'O', 'W', 'T', 'M'};
  put32(b, 1), put32(b, 0), put32(b, 0), put32(b, n), put32(b, closed ? 1 : 0), put32(b, 0), put32(b, 0);
  for (uint32_t i = 0; i < n; ++i) {
    putf(b, 10), putf(b, 3), putf(b, 2.0f * float(i)), putf(b, 0);
    putf(b, -5), putf(b, 0), putf(b, 0), putf(b, 0);
    putf(b, 0.2f), putf(b, 0), putf(b, 0), putf(b, 0);
  }
  b.insert(b.end(), {'O', 'W', 'T', 'M'});
  put32(b, 1), put32(b, 0), put32(b, 0);
  return b;
}

void test_owt() {
  race::AiLineFile f;
  std::string e;
  const auto b = made_up_owt(5, true);
  CHECK(race::parse_owt(b, f, &e));
  CHECK(f.points.size() == 5 && f.closed && f.version == 1);
  CHECK(near(f.points[2].z, 4) && near(f.points[2].half_width(), 5, 1e-5));
  CHECK(near(f.points[2].race_x(), 10 - 0.2 * 5, 1e-5));  // centre + offset x lateral
  auto bad = b;
  bad.pop_back();
  CHECK(!race::parse_owt(bad, f, &e));
  auto open = made_up_owt(4, false);
  CHECK(race::parse_owt(open, f, &e) && !f.closed);
  CHECK(near(race::centre_length(f), 6, 1e-5));
}

// A made-up route: two slots facing +z, the gantry 10 m ahead facing -z
// (towards the grid), and the last finish cannons at z = 100.
const char* kRoute = R"(<TrackRoute>
  <NamedTransforms>
    <NamedTransform name='start_location_00'>
      <Transform pos.x='1.0' pos.y='0.0' pos.z='0.0' facing.x='0.0' facing.y='0.0' facing.z='1.0'/>
    </NamedTransform>
    <NamedTransform name='start_location_01'>
      <Transform pos.x='-3.0' pos.y='0.0' pos.z='-6.5' facing.x='0.0' facing.y='0.0' facing.z='1.0'/>
    </NamedTransform>
    <NamedTransform name="start_gantry_cannon_00">
      <Transform pos.x="0.0" pos.y="8.0" pos.z="10.0" facing.x="0.0" facing.y="0.0" facing.z="-1.0"/>
    </NamedTransform>
    <NamedTransform name='end_race_cannon_trigger' width='50.0'>
      <Transform pos.x='0.0' pos.y='0.0' pos.z='90.0' facing.x='0.0' facing.y='0.0' facing.z='1.0'/>
    </NamedTransform>
    <NamedTransform name='end_race_cannon_left_04'>
      <Transform pos.x='-8.0' pos.y='0.0' pos.z='100.0' facing.x='0.0' facing.y='0.0' facing.z='1.0'/>
    </NamedTransform>
    <NamedTransform name='end_race_cannon_right_04'>
      <Transform pos.x='8.0' pos.y='0.0' pos.z='100.0' facing.x='0.0' facing.y='0.0' facing.z='1.0'/>
    </NamedTransform>
  </NamedTransforms>
</TrackRoute>)";

void test_route() {
  race::Route r;
  std::string e;
  CHECK(race::parse_route(kRoute, r, &e));
  CHECK(r.transforms.size() == 6);
  CHECK(r.slots().size() == 2 && near(r.slots()[1]->z, -6.5));
  CHECK(r.find("end_race_cannon_trigger") && r.find("end_race_cannon_trigger")->width.value_or(0) == 50);
  const auto g = r.start_line();
  CHECK(g && near(g->nz, 1) && near(g->z, 10));  // the slots' facing, not the gantry's
  // 1 m behind to 1 m ahead along the slots' facing: one crossing; along
  // the gantry's facing: none forwards.
  CHECK(g->crossing(0, 9, 0, 11) >= 0 && near(g->crossing(0, 9, 0, 11), 0.5));
  CHECK(g->crossing(0, 11, 0, 9) < 0);
  CHECK(g->crossing(70, 9, 70, 11) < 0);  // beyond 60 m of the gantry
  const auto fin = r.sprint_finish();
  CHECK(fin && near(fin->x, 0) && near(fin->z, 103.5));
  // Grid: PlayerGridPosition p -> slot p - 1; 0 -> last; the rows fill the rest from pole.
  CHECK(race::player_slot(6, 8) == 5 && race::player_slot(7, 8) == 6 && race::player_slot(0, 8) == 7);
  const auto o = race::opponent_slots(5, 7);
  CHECK(o.size() == 7 && o[0] == 0 && o[4] == 4 && o[5] == 6 && o[6] == 7);
}

void test_session() {
  // A circuit of 2 laps, the gate at z = 10: a car driven along +z through
  // it three times (teleported back between) finishes on the third.
  race::Gate g{0, 10, 0, 1, 60};
  race::Session s(1, true, 2, g, std::nullopt, {6.0, false});
  CHECK(s.clock() == -6.0 && s.digit() == 0);
  const double dt = 0.5;
  race::CarSample c;
  int digits = 0;
  while (!s.started()) {
    s.step(dt, std::span(&c, 1));
    digits += s.digit() > 0;
  }
  CHECK(digits == 6);  // 3, 3, 2, 2, 1, 1 at half-second steps
  for (int lap = 0; lap < 3; ++lap) {
    c.x0 = c.x1 = 0, c.z0 = 9.0, c.z1 = 9.0;
    for (int k = 0; k < 4; ++k) {
      c.z0 = c.z1, c.z1 = c.z0 + 0.6;
      c.progress = 100.0 * lap + c.z1;
      s.step(dt, std::span(&c, 1));
    }
  }
  CHECK(s.car(0).crossings == 3 && s.finished(0) && s.car(0).laps.size() == 2);
  // The second step of each pass moves 9.6 -> 10.2: the plane at 2/3 of it.
  CHECK(near(s.car(0).laps[0], 2.0, 1e-9));
  CHECK(near(s.car(0).finish, 0.5 + 0.5 * (0.4 / 0.6) + 4.0, 1e-9));
  // Places: a car ahead in crossings leads; then progress.
  race::Session p(3, true, 3, g, std::nullopt, {0.0, false});
  race::CarSample cs[3];
  cs[0].progress = 50, cs[1].progress = 80, cs[2].progress = 20;
  cs[2].x0 = 0, cs[2].z0 = 9, cs[2].x1 = 0, cs[2].z1 = 11;
  p.step(0.1, cs);
  CHECK(p.car(2).place == 1 && p.car(1).place == 2 && p.car(0).place == 3);
  // Resets: slower than 5 mph for 3 s.
  race::Session r(1, true, 1, g, std::nullopt, {0.0, true});
  race::CarSample still;
  int resets = 0;
  for (int k = 0; k < 40; ++k) {
    r.step(0.1, std::span(&still, 1));
    resets += !r.resets().empty();
    if (!r.resets().empty()) r.reset_done(0);
  }
  CHECK(resets == 1 && !r.collides(0));
  // A checkpoint plane counts either way within its reach; a one-way gate
  // only forwards.
  race::Gate cp{0, 10, 0, 1, 25};
  CHECK(cp.crossing(0, 11, 0, 9) < 0 && cp.crossing(0, 9, 0, 11) >= 0);
  cp.both_ways = true;
  CHECK(cp.crossing(0, 11, 0, 9) >= 0 && cp.crossing(30, 11, 30, 9) < 0);
  // A sprint's finish does not count while held (an earlier checkpoint
  // missed), and counts once released.
  race::Session f(1, false, 1, g, cp, {0.0, false});
  race::CarSample m;
  m.x0 = 0, m.z0 = 9, m.x1 = 0, m.z1 = 11;
  f.hold_finish(0, true);
  f.step(0.1, std::span(&m, 1));
  CHECK(!f.finished(0));
  f.hold_finish(0, false);
  m.z0 = 11, m.z1 = 9;
  f.step(0.1, std::span(&m, 1));
  CHECK(f.finished(0));
}

// Made-up difficulty rows.
race::EventData made_up_rows() {
  race::EventData e;
  race::SkillRow a;
  a.id = 30, a.braking_min = 0.5, a.braking_max = 0.6, a.cornering_min = 0.4, a.cornering_max = 0.5;
  a.dist_behind_for_max = 200, a.rubberband_id = 3;
  race::SkillRow b = a;
  b.id = 33, b.rubberband_id = 4;
  e.skills = {a, b};
  race::RubberbandRow r;
  r.id = 3, r.start_rubberbanding = 5, r.start_torque_cut = 100, r.max_torque_cut = 400, r.torque_cut_factor = 0.5;
  r.start_catch_up = 50, r.stop_catch_up = 30, r.catch_up_factor = 0.5, r.catch_up_gap_per_position = 10;
  r.modes = {{0, 1.0, 0}};
  e.rubberbands = {r};
  return e;
}

void test_difficulty() {
  const auto e = made_up_rows();
  auto d = race::assign_driver(30, 4, 0, 3, -9, 2, e);
  CHECK(d.skill == 33 && d.temperament == 1 && d.rubberband_id == 4);  // the skill row's id (event id 0)
  d = race::assign_driver(30, 4, 2, 0, 0, 1, e);
  CHECK(d.rubberband_id == 3);  // event 2 + modifier 1
  CHECK(race::assign_driver(79, 9, 0, 5, 5, 0, e).skill == 80);
  const auto& row = e.rubberbands[0];
  CHECK(near(race::torque_cut(row, 50), 1) && near(race::torque_cut(row, 100), 1));
  CHECK(near(race::torque_cut(row, 250), 0.75) && near(race::torque_cut(row, 400), 0.5) && near(race::torque_cut(row, 900), 0.5));
  CHECK(near(race::start_boost(0, 8, 0), 0.4) && near(race::start_boost(7, 8, 299), 0.1));
  CHECK(near(race::start_boost(0, 8, 650), 0.2) && race::start_boost(3, 8, 1000) == 0);
  race::DriverAi ai;
  ai.init(e.skills[0], &row, 7);
  CHECK(ai.braking() >= 0.5 && ai.braking() <= 0.6 && ai.cornering() >= 0.4 && ai.cornering() <= 0.5);
  ai.update(250, -1);  // leading by 250 m: torque cut
  CHECK(ai.state() == race::DriverAi::State::kRubberband && ai.mode() == 8 && near(ai.torque(), 0.75));
  ai.update(-20, 1);  // 20 m behind, one place: catch-up starts above 60 m
  CHECK(ai.state() == race::DriverAi::State::kNormal && near(ai.torque(), 1));
  ai.update(-25, 0);
  ai.update(-55, 0);  // above 50: on
  CHECK(ai.state() == race::DriverAi::State::kCatchUp && ai.braking() > 0.75 && ai.cornering() > 0.8);
  ai.update(-35, 0);  // still above the 30 m stop
  CHECK(ai.state() == race::DriverAi::State::kCatchUp);
  ai.update(-29, 0);
  CHECK(ai.state() == race::DriverAi::State::kNormal);
  ai.update(-250, 0);  // more than 200 m behind: the maxima, with catch-up on top
  CHECK(ai.state() == race::DriverAi::State::kCatchUp && near(ai.braking(), 0.8) && near(ai.cornering(), 0.875));
  race::DriverAi lone;  // no rubber-band row: the maxima alone
  lone.init(e.skills[0], nullptr, 7);
  lone.update(-250, 0);
  CHECK(lone.state() == race::DriverAi::State::kMaxPerformance && near(lone.braking(), 0.6) && near(lone.cornering(), 0.5));
  race::DriverAi off;
  off.init(e.skills[0], &row, 7, false);
  off.update(250, -1);
  CHECK(off.state() == race::DriverAi::State::kNormal && near(off.torque(), 1));
}

// SPEC-RACE-AI mode table: over-braking holds 1 + Amount for 1 s, then the
// rolled value; under-cornering is the minimum less Amount (floor 0.25);
// the torque cut puts both at the row's minima.
void test_rubberband_modes() {
  auto e = made_up_rows();
  race::RubberbandRow r = e.rubberbands[0];
  r.modes = {{3, 1.0, 6.5}};
  race::DriverAi ai;
  ai.init(e.skills[0], &r, 11);
  ai.update(20, -1, 0.5);  // 20 m ahead: rubber band, mode 3 drawn
  CHECK(ai.mode() == 3 && near(ai.braking(), 7.5));
  ai.update(21, -1, 0.6);  // 1.1 s into the mistake
  CHECK(ai.mode() == 3 && ai.braking() >= 0.5 && ai.braking() <= 0.6);
  ai.update(150, -1, 0.1);  // past the torque cut
  CHECK(ai.mode() == 8 && near(ai.braking(), 0.5) && near(ai.cornering(), 0.4));
  r.modes = {{2, 1.0, 0.1}};
  ai.init(e.skills[0], &r, 11);
  ai.update(20, -1);
  CHECK(ai.mode() == 2 && near(ai.cornering(), 0.3));
  r.modes = {{2, 1.0, 0.3}};
  ai.init(e.skills[0], &r, 11);
  ai.update(20, -1);
  CHECK(near(ai.cornering(), 0.25));
  // No row: the built-in choices (under-cornering, under-braking, over-braking).
  int seen[4] = {};
  for (uint64_t seed = 1; seed <= 400; ++seed) {
    race::DriverAi b;
    b.init(e.skills[0], nullptr, seed);
    b.update(150, -1);
    if (b.mode() >= 1 && b.mode() <= 3) ++seen[b.mode()];
  }
  CHECK(seen[2] > seen[1] && seen[1] > seen[3] && seen[3] > 0 && seen[1] + seen[2] + seen[3] == 400);
}

// SPEC-EVENT-REWARDS, Tests (synthetic).
void test_rewards() {
  using race::Winnings;
  CHECK(race::race_credits(10000, Winnings::kDefault, 8, 3, true) == 7500);
  CHECK(race::race_credits(10000, Winnings::kDefault, 8, 2, true) == 8800);
  CHECK(race::race_credits(10000, Winnings::kDefault, 8, 8, true) == 1300);
  CHECK(race::race_credits(10000, Winnings::kTopThree, 8, 2, true) == 5000);
  CHECK(race::race_credits(10000, Winnings::kTopThree, 8, 4, true) == 0);
  CHECK(race::race_credits(10000, Winnings::kWinnerTakesAll, 8, 2, true) == 0);
  CHECK(race::race_credits(10000, Winnings::kDefault, 1, 1, true) == 0);
  CHECK(race::race_credits(10000, Winnings::kDefault, 8, 1, false) == 0);
  CHECK(race::round_prize(9) == 0 && race::round_prize(10) == 10 && race::round_prize(1250) == 1300);
  race::WristbandTables t;
  for (int p = 1; p <= 8; ++p) t.points[2][p] = 90 - 10 * p;  // made up
  t.levels = {{0, "a"}, {100, "b"}};
  race::CareerRecord r;
  auto a = race::award_race(r, 22, 143, 2, 1, 2, 10000, 8, 3, true, 160.9, 60, 0, t);
  CHECK(a.credits == 7500 && a.wristband_points == 60 && a.first_completion && r.event_places[22] == 3);
  a = race::award_race(r, 22, 143, 2, 1, 2, 10000, 8, 5, true, 161, 60, 0, t);
  CHECK(a.wristband_points == 0 && r.event_places[22] == 3 && r.race_places[143] == 3);
  a = race::award_race(r, 22, 143, 2, 1, 2, 10000, 8, 1, true, 155, 61, 777, t);
  CHECK(a.wristband_points == 20 && a.won && r.event_places[22] == 1 && a.prize_car == 777);
  CHECK(r.num_races == 3 && r.num_victories == 1 && r.num_podiums == 2 && r.wristband_points == 80 && t.level_index(80) == 0);
  a = race::award_race(r, 22, 143, 2, 1, 2, 10000, 8, 1, true, 155, 61, 777, t);
  CHECK(a.prize_car == 0 && r.prize_cars.size() == 1);
  const int races = r.num_races;
  a = race::award_race(r, 23, 144, 2, 1, 2, 10000, 8, 2, false, 0, 0, 0, t);
  CHECK(a.credits == 0 && r.num_races == races && !r.event_places.count(23));

  // SPEC-REWIND section 7 & SPEC-EVENT-REWARDS: per-assist difficulty bonus.
  CHECK(race::rewind_difficulty_bonus(true) == 0.0);
  CHECK(race::rewind_difficulty_bonus(false) == 0.20);
  CHECK(race::rewind_difficulty_bonus(false, 0) == 0.20);
  CHECK(race::rewind_difficulty_bonus(false, race::DifficultyAssists::kExcludedSessionMode) == 0.0);
  CHECK(race::rewind_difficulty_bonus(true, race::DifficultyAssists::kExcludedSessionMode) == 0.0);

  race::DifficultyAssists assists;
  CHECK(assists.rewind_on);
  CHECK(race::assist_difficulty_bonus(assists) == 0.0);
  CHECK(race::assist_difficulty_bonus_percent(assists) == 0);
  CHECK(race::total_difficulty_bonus_percent(1, assists) == 15);

  assists.rewind_on = false;
  CHECK(race::assist_difficulty_bonus(assists) == 0.20);
  CHECK(race::assist_difficulty_bonus_percent(assists) == 20);
  CHECK(race::total_difficulty_bonus_percent(0, assists) == 20);
  CHECK(race::total_difficulty_bonus_percent(1, assists) == 35);
  CHECK(race::total_difficulty_bonus_percent(2, assists) == 50);
  CHECK(race::total_difficulty_bonus_percent(3, assists) == 65);

  // Session mode 3 excludes the Rewind bonus.
  assists.session_mode = race::DifficultyAssists::kExcludedSessionMode;
  CHECK(race::assist_difficulty_bonus(assists) == 0.0);
  CHECK(race::assist_difficulty_bonus_percent(assists) == 0);
  CHECK(race::total_difficulty_bonus_percent(1, assists) == 15);

  // End-to-end award_race with Rewind OFF (+20%) on Easy (0%).
  assists.session_mode = 0;
  race::CareerRecord r_bonus;
  const int pct = race::total_difficulty_bonus_percent(0, assists);
  const auto a_bonus = race::award_race(r_bonus, 24, 145, 2, 1, 2, 10000, 8, 1, true, 120, 60, 0, t, pct, 0);
  CHECK(a_bonus.base_credits == 10000);
  CHECK(a_bonus.difficulty_pct == 20);
  CHECK(a_bonus.difficulty_bonus == 2000);
  CHECK(a_bonus.credits == 12000);
}

void test_line_and_ground() {
  // A made-up closed circle of radius 50 m (points 2 m apart), half width 6, offset 0.
  race::AiLineFile f;
  f.closed = true;
  const int n = 157;
  for (int i = 0; i < n; ++i) {
    const double a = 2 * 3.14159265358979 * i / n;
    race::AiLinePoint p;
    p.x = float(50 * std::sin(a)), p.z = float(50 * std::cos(a)), p.y = 2;
    // Driving towards +x at the top of the circle: the left (-right, the
    // right of (x, z) being (z, -x)) points away from the centre.
    p.lx = float(6 * std::sin(a)), p.lz = float(6 * std::cos(a));
    f.points.push_back(p);
  }
  race::Route r;
  r.transforms.push_back({"start_location_00", 0, 2, 50, 1, 0, std::nullopt, ""});
  r.transforms.push_back({"start_gantry_cannon_00", 10, 6, 49, -1, 0, std::nullopt, ""});
  const auto rl = race::build_race_line(f, &r, 3);
  CHECK(rl.closed && near(rl.lap_length, 2 * 3.14159265358979 * 50, 0.2));
  CHECK(near(rl.line.length(), 3 * rl.lap_length - 2, 1.0));  // three copies, the last without its closing segment
  CHECK(near(rl.gate_progress, rl.lap_length + 10, 0.3));
  CHECK(near(rl.line.radius[40], 50, 0.5));
  CHECK(near(rl.line.room(5, true), 6, 1e-4) && near(rl.line.room(5, false), 6, 1e-4));
  // Per-car speed: a corner at 10 m/s^2 on a 50 m circle -> sqrt(500).
  drive::AiField field(rl.line, 1);
  field.place(0, 100, 0);
  field.set_limits(0, 10, 14);
  CHECK(near(field.planned_speed(0, 100), std::sqrt(500.0), 0.3));
  // The ribbon ground: a ray down at the centre line hits at y = 2.
  race::RibbonGround ground(f);
  drive::Hit hit;
  CHECK(ground.raycast({0, 10, 50}, {0, -1, 0}, 20, hit) && near(hit.distance, 8, 1e-3));
  CHECK(!ground.raycast({0, 10, 0}, {0, -1, 0}, 20, hit));  // the circle's middle: no road
  // Lanes: a car 5 m behind a slower one moves aside; with nothing ahead it returns.
  std::vector<double> prog{100, 105};
  std::vector<float> cross{0, 0}, speed{30, 20}, lane{0, 0};
  std::vector<race::LaneParams> lp(2);
  for (int k = 0; k < 60; ++k) race::plan_lanes(rl.line, prog, cross, speed, lp, 1, lane, 0.05);
  CHECK(std::fabs(lane[0]) > 3.0 && std::fabs(lane[0]) <= 0.5 * 6 + 0.3 + 1e-4 && lane[1] == 0);
  prog[1] = 90;
  for (int k = 0; k < 80; ++k) race::plan_lanes(rl.line, prog, cross, speed, lp, 1, lane, 0.05);
  CHECK(lane[0] == 0);
}

// ---- the disc ----

std::string read_text(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void disc(const std::filesystem::path& root) {
  std::string e;
  // Census: route files and transforms; AI lines; events, races, tracks.
  int files = 0;
  size_t transforms = 0;
  for (const auto& de : std::filesystem::directory_iterator(root / "media/tracks/colorado/Ribbon_00")) {
    const auto n = de.path().filename().string();
    if (n.rfind("TrackRoute", 0) != 0 || de.path().extension() != ".xml") continue;
    race::Route r;
    if (race::parse_route(read_text(de.path()), r, &e)) ++files, transforms += r.transforms.size();
  }
  std::printf("census: %d route files, %zu transforms\n", files, transforms);
  CHECK(files == 243 && transforms == 7877);
  const auto members = race::owt_members(root, &e);
  int closed = 0, open = 0, decoded = 0;
  {
    auto zip = fh1::Archive::open(root / "media" / "aiopenworld.zip", &e);
    for (const auto& m : members) {
      race::AiLineFile f;
      auto rd = zip->read(m);
      if (rd.ok() && race::parse_owt(rd.data, f, &e)) ++decoded, (f.closed ? closed : open)++;
    }
  }
  std::printf("census: %zu AI lines, %d decoded (%d closed, %d open)\n", members.size(), decoded, closed, open);
  CHECK(members.size() == 46 && decoded == 46 && closed == 25 && open == 21);
  race::EventCensus c;
  CHECK(race::event_census(root, c, &e));
  std::printf("census: %d events, %d races, %d tracks, %d resolve to a route file\n", c.events, c.races, c.tracks,
              c.resolved);
  CHECK(c.events == 119 && c.races == 119 && c.tracks == 150 && c.resolved == 119);
  if (const char* state = std::getenv("SHIFT_STATE_ROOT");
      state && *state && std::filesystem::exists(std::filesystem::path(state) / "title-update-v4" / "media.zip")) {
    cars::Install inst;
    cars::PartsDb pdb;
    CHECK(cars::discover_install(root, state, "", inst, &e) && pdb.open(inst, &e));
    race::EventCensus mc;
    CHECK(race::event_census(root, mc, &e, state, &pdb));
    std::printf("merged census: %d events, %d races, %d tracks, %d resolve\n", mc.events, mc.races, mc.tracks,
                mc.resolved);
    CHECK(mc.events == 155 && mc.races == 155 && mc.tracks == 184);
    std::vector<race::EventSummary> evs;
    CHECK(race::list_events(root, evs, &e, state, &pdb));
    int rally_count = 0;
    int rally_stage_route = 0;
    for (const auto& ev : evs) {
      if (ev.career_type == 9 && ev.environment_id == 111) {
        ++rally_count;
        if (rally_stage_route == 0 && ev.ribbon == 8 && ev.route_id > 0 && ev.horizon_id != "RALLY_HEATS")
          rally_stage_route = ev.route_id;
      }
    }
    CHECK(rally_count == 36);
    CHECK(rally_stage_route > 0);
    // Verify every EventParticipants row across all events (including DLC
    // participants) resolves a non-empty Data_Car.MediaName via merged PartsDb.
    int total_opponents = 0, missing_media = 0;
    for (const auto& ev : evs) {
      race::EventData ev_data;
      if (race::load_event(root, ev.horizon_id, race::Difficulty::kMed, ev_data, &e, state, &pdb)) {
        for (const auto& op : ev_data.opponents) {
          ++total_opponents;
          if (op.car_media.empty()) ++missing_media;
        }
      }
    }
    std::printf("merged PartsDb EventParticipants: %d loaded, %d missing media\n", total_opponents, missing_media);
    CHECK(total_opponents >= 500 && missing_media == 0);
    if (rally_stage_route > 0) {
      race::StageTrack st_db, st_v4;
      CHECK(race::load_stage_track(state, rally_stage_route, st_db, &e, &pdb));
      CHECK(race::load_stage_track(state, rally_stage_route, st_v4, &e, nullptr));
      CHECK(st_db.track_id > 0 && st_db.track_id == st_v4.track_id && st_db.route_id == rally_stage_route);
      CHECK(st_db.splits == st_v4.splits && !st_db.best.empty());
      race::StageTrack st_missing;
      std::string err_missing;
      CHECK(!race::load_stage_track(state, -999, st_missing, &err_missing, &pdb));
      CHECK(err_missing.find("no ColoradoDirt track") != std::string::npos);
    }
  }

  // Route 158's line.
  race::AiLineFile l158;
  CHECK(race::load_owt(root, 158, l158, &e));
  float hw0 = 1e9f, hw1 = 0, of0 = 1e9f, of1 = -1e9f;
  for (const auto& p : l158.points)
    hw0 = std::min(hw0, p.half_width()), hw1 = std::max(hw1, p.half_width()), of0 = std::min(of0, p.offset),
    of1 = std::max(of1, p.offset);
  std::printf("route 158: %zu points, closed %d, centre %.1f m, racing %.1f m, half width %.2f-%.2f, offsets %.3f to %.3f\n",
              l158.points.size(), int(l158.closed), race::centre_length(l158), race::racing_length(l158), hw0, hw1,
              of0, of1);
  CHECK(l158.points.size() == 577 && l158.closed);
  CHECK(near(race::centre_length(l158), 1164.4, 0.1) && near(race::racing_length(l158), 1147.1, 0.1));
  CHECK(near(hw0, 5.98, 0.005) && near(hw1, 6.02, 0.005) && near(of0, -0.838, 0.0005) && near(of1, 0.817, 0.0005));

  // FR22 at Med: skills 50 (five drivers), 45 and 51; rubber-band rows 6, 5 and 6.
  race::EventData fr22;
  CHECK(race::load_event(root, "FR22", race::Difficulty::kMed, fr22, &e));
  CHECK(fr22.route_id == 143 && fr22.laps == 4 && fr22.opponents.size() == 7 && fr22.player_grid_position == 6);
  int s50 = 0, s45 = 0, s51 = 0, rows_ok = 0;
  for (const auto& o : fr22.opponents) {
    const auto d = race::assign_driver(fr22.event_skill, fr22.event_temperament, fr22.event_rubberband, o.skill_modifier,
                                       o.aggro_modifier, o.rubberband_modifier, fr22);
    s50 += d.skill == 50, s45 += d.skill == 45, s51 += d.skill == 51;
    rows_ok += (d.skill == 45 ? d.rubberband_id == 5 : d.rubberband_id == 6);
  }
  std::printf("FR22: skills 50 x%d, 45 x%d, 51 x%d; rubber-band rows as the spec %d/7\n", s50, s45, s51, rows_ok);
  CHECK(s50 == 5 && s45 == 1 && s51 == 1 && rows_ok == 7);
  if (const auto* r6 = fr22.rubberband(6)) {
    const double gaps[] = {100, 140, 200.5, 500, 800}, want[] = {1, 1, 0.933, 0.6, 0.6};
    for (int k = 0; k < 5; ++k) CHECK(near(race::torque_cut(*r6, gaps[k]), want[k], 0.0005));
  } else CHECK(false);
  race::EventData fr16;
  CHECK(race::load_event(root, "FR16", race::Difficulty::kMed, fr16, &e));
  CHECK(fr16.route_id == 154 && fr16.laps == 1 && fr16.opponents.size() == 7 && fr16.player_grid_position == 7);

  // Route 143: p = 6 -> slot 05; the start line from the slots' facing.
  race::Route r143;
  CHECK(race::load_route(root, 143, r143, &e));
  const auto slots = r143.slots();
  const auto gate = r143.start_line();
  CHECK(slots.size() == 8 && gate);
  CHECK(race::player_slot(fr22.player_grid_position, 8) == 5);
  if (gate) {
    const auto* g = r143.find("start_gantry_cannon_00");
    const double fx = g->fx, fz = g->fz;
    CHECK(gate->crossing(g->x - gate->nx, g->z - gate->nz, g->x + gate->nx, g->z + gate->nz) >= 0);
    // Along the gantry's own facing (towards the grid on this route): no forward crossing.
    CHECK(gate->crossing(g->x - fx, g->z - fz, g->x + fx, g->z + fz) < 0);
    // Every slot stands behind the line.
    int behind = 0;
    for (const auto* s : slots) behind += gate->side(s->x, s->z) < 0;
    CHECK(behind == 8);
  }
  race::AiLineFile l143;
  CHECK(race::load_owt(root, 143, l143, &e));
  const auto rl = race::build_race_line(l143, &r143, 7);
  // The slots sit near the racing line, behind the gate on the second copy.
  int near_line = 0;
  for (const auto* s : slots) {
    const double p = rl.line.nearest(s->x, s->z, rl.gate_progress - 0.5 * rl.lap_length, rl.gate_progress);
    double x, y, z, h;
    rl.line.pose(p, 0, x, y, z, h);
    near_line += std::hypot(x - s->x, z - s->z) < 8 && p < rl.gate_progress;
  }
  std::printf("route 143: lap %.1f m, gate at %.1f m, %d of 8 slots within 8 m of the line behind the gate\n",
              rl.lap_length, rl.gate_progress, near_line);
  CHECK(near_line == 8);

  // The oracle lap against route 158's racing line (game frame both).
  if (const char* lap = std::getenv("SHIFT_ORACLE_LAP")) {
    drive::OracleLine ol;
    if (drive::parse_oracle_line(read_text(lap), ol, &e)) {
      const auto r158 = race::build_race_line(l158, nullptr, 3);
      std::vector<double> err;
      double p = r158.lap_length * 0.5;
      bool first = true;
      for (size_t k = 0; k < ol.samples.size(); k += 30) {
        const auto& s = ol.samples[k];
        if (s.t < 0) continue;
        p = first ? r158.line.nearest(s.x, s.z, 0, 2 * r158.lap_length) : r158.line.nearest(s.x, s.z, p - 20, p + 120);
        first = false;
        double x, y, z, h;
        r158.line.pose(p, 0, x, y, z, h);
        err.push_back(std::hypot(x - s.x, z - s.z));
      }
      std::sort(err.begin(), err.end());
      const double med = err.empty() ? 1e9 : err[err.size() / 2];
      std::printf("oracle lap vs route 158 racing line: %zu samples, median %.2f m\n", err.size(), med);
      CHECK(med < 0.5);
    }
  }
}

// A Rally stage's timed field (SPEC-V4-CONTENT, the interim rule) on a
// file in rallyaidifficulty.xml's form (ours): times are the best total
// over a fraction between the difficulty's Min and Max; a track override
// wins; the place counts faster drivers.
void test_rally_field() {
  const char* kXml = R"(<RallyAITimeGeneration>
  <GlobalDifficulty>
    <Difficulty Min="0.5" Max="0.6"/><Difficulty Min="0.8" Max="0.9"/>
    <Difficulty Min="0.85" Max="0.95"/><Difficulty Min="0.9" Max="1.0"/>
  </GlobalDifficulty>
  <TrackDifficultyOverride>
    <Track Id="7"><Difficulty Min="0.1" Max="0.2"/><Difficulty Min="0.4" Max="0.4"/>
      <Difficulty Min="0.85" Max="0.95"/><Difficulty Min="0.9" Max="1.0"/></Track>
  </TrackDifficultyOverride>
  <AIDrivers SkillVariance="0.1">
    <AIDriver AIPlayerId="1" Skill="1.0"/><AIDriver AIPlayerId="2" Skill="0.5"/><AIDriver AIPlayerId="3" Skill="0.0"/>
  </AIDrivers>
</RallyAITimeGeneration>)";
  race::RallyField field;
  std::string e;
  CHECK(race::parse_rally_field(kXml, field, &e));
  CHECK(field.drivers.size() == 3 && field.skill_variance == 0.1 && field.overrides.size() == 1);
  race::StageTrack track;
  track.track_id = 1;
  track.best = {40, 60};
  const auto t = race::field_times(field, track, 1, 1);
  CHECK(t.size() == 3);
  for (double v : t) CHECK(v >= 100 / 0.9 - 1e-9 && v <= 100 / 0.8 + 1e-9);
  CHECK(t[0] < t[2]);  // skill 1 against 0 with a 0.1 variance
  track.track_id = 7;  // the override: Medium is exactly 0.4
  for (double v : race::field_times(field, track, 1, 2)) CHECK(std::fabs(v - 250) < 1e-9);
  CHECK(race::rally_place({100, 120, 140}, 110) == 2 && race::rally_place({100, 120, 140}, 90) == 1 &&
        race::rally_place({100, 120, 140}, 150) == 4);
}

}  // namespace

int main() {
  CHECK(race::CarNames::split_words("CamaroZL1") == "Camaro ZL1");
  CHECK(race::CarNames::split_words("Exige240") == "Exige 240");
  CHECK(race::CarNames::split_words("F40Competizione") == "F40 Competizione");
  CHECK(race::CarNames::split_words("AstonMartin") == "Aston Martin");
  CHECK(race::CarNames::split_words("SkylineGTRVSpecII") == "Skyline GTRV Spec II");
  if (const char* root = std::getenv("SHIFT_GAME_ROOT")) {
    // FORMAT-STRINGTABLES, What resolves: every disc car's ModelShort.
    const char* state = std::getenv("SHIFT_STATE_ROOT");
    race::CarNames names;
    std::string e;
    CHECK(names.load(root, state ? state : "", &e, "EN"));
    std::printf("car names: %zu resolved; %s, %s\n", names.resolved(), names.full_name("FER_California_09").c_str(),
                names.short_name("NIS_SkylineGTRVSpecII_02").c_str());
    CHECK(names.short_name("NIS_SkylineGTRVSpecII_02") == "Nissan Skyline GT-R V-spec II");
    CHECK(names.resolved() >= 176);
    if (state && *state && std::filesystem::exists(std::filesystem::path(state) / "title-update-v4" / "media.zip")) {
      CHECK(names.resolved() >= 250);
    }
    // Language selection & cache-key isolation ("EN" vs "FR"/"fr" vs "br"/"BR").
    const uint32_t back_id = uint32_t(fh1::string_hash("HelpButtons")) << 16 | fh1::string_hash("IDS_Back");
    const std::string back_ref = "_&" + std::to_string(back_id);
    const std::string back_en = names.text(back_ref);
    race::CarNames names_fr, names_fr_low, names_br;
    CHECK(names_fr.load(root, "", &e, "FR"));
    CHECK(names_fr_low.load(root, "", &e, "fr"));
    CHECK(names_br.load(root, "", &e, "br"));
    const std::string back_fr = names_fr.text(back_ref);
    const std::string back_br = names_br.text(back_ref);
    CHECK(back_fr != back_en && back_fr == names_fr_low.text(back_ref));
    CHECK(back_br != back_en && back_br.rfind("_&", 0) != 0);
  }
  // SPEC-RACE-AI faked PI offset: California 1062 -> +1 (x 0x0DB2AA51).
  CHECK(race::faked_pi_offset(1062) == 1);
  CHECK(race::faked_pi_offset(1419, true) == 5);
  // The two measured grids (EVENTS Grid): FR06 and HEADLINE_EVENT's AI.
  const std::pair<int64_t, int> measured[] = {{454, 1},   {1103, 0},  {348, 0},   {433, -2}, {325, -4},
                                              {1006, 3},  {1175, 0},  {1247, 0},  {1539, -4},
                                              {1023, -4}, {1020, -4}};
  for (const auto& [v, r] : measured) {
    if (race::faked_pi_offset(v) != r) std::printf("faked_pi_offset(%lld) = %d, want %d\n", (long long)v, race::faked_pi_offset(v), r);
    CHECK(race::faked_pi_offset(v) == r);
  }
  test_rally_field();
  test_owt();
  test_route();
  test_session();
  test_difficulty();
  test_rubberband_modes();
  test_rewards();
  test_line_and_ground();
  // Slice 6: Pinyon Shift HARD AI mode, AISkillAdjustments dynamic difficulty,
  // and Nemesis rival boost.
  {
    CHECK(race::is_pinyon_hard(race::Difficulty::kHardPlus, race::AiMode::kStock));
    CHECK(race::is_pinyon_hard(race::Difficulty::kMed, race::AiMode::kHard));
    CHECK(!race::is_pinyon_hard(race::Difficulty::kPro, race::AiMode::kStock));
    CHECK(race::effective_db_difficulty(race::Difficulty::kEasy, race::AiMode::kHard) == 1);
    CHECK(race::effective_db_difficulty(race::Difficulty::kMed, race::AiMode::kHard) == 2);
    CHECK(race::effective_db_difficulty(race::Difficulty::kHard, race::AiMode::kHard) == 3);
    CHECK(race::effective_db_difficulty(race::Difficulty::kPro, race::AiMode::kHard) == 3);
    CHECK(race::effective_db_difficulty(race::Difficulty::kHardPlus, race::AiMode::kStock) == 3);
    race::AiMode pm = race::AiMode::kStock;
    CHECK(race::parse_ai_mode("fair", pm) && pm == race::AiMode::kFair);
    CHECK(race::parse_ai_mode("hard", pm) && pm == race::AiMode::kHard);
    CHECK(race::parse_ai_mode("stock", pm) && pm == race::AiMode::kStock);

    race::EventData ed;
    ed.skills.push_back({50, 0.686, 0.763, 0.63, 0.70, 20, 1, 1, 6});
    ed.skills.push_back({1, 0.95, 1.0, 1.188, 1.25, 10, 1, 1, 1});
    const auto hard_med = race::assign_driver_for_mode(35, 5, 0, 0, 0, 0, race::Difficulty::kMed, race::AiMode::kHard, ed);
    CHECK(hard_med.temperament == 1 && hard_med.rubberband_id == 0 && hard_med.skill == 35);
    const auto hard_pro = race::assign_driver_for_mode(20, 5, 6, 0, 0, 0, race::Difficulty::kPro, race::AiMode::kHard, ed);
    CHECK(hard_pro.skill == 1 && hard_pro.temperament == 1 && hard_pro.rubberband_id == 0);
    const auto hard_plus =
        race::assign_driver_for_mode(20, 5, 6, 0, 0, 0, race::Difficulty::kHardPlus, race::AiMode::kStock, ed);
    CHECK(hard_plus.skill == 1 && hard_plus.temperament == 1 && hard_plus.rubberband_id == 0);

    // Dynamic difficulty (AISkillAdjustments): fast player lowers AISkill (harder);
    // disabled under kFair and kHard.
    const std::vector<std::pair<int, int>> empty_tbl;
    CHECK(race::lookup_skill_adjustment(90, empty_tbl) == -9);
    CHECK(race::lookup_skill_adjustment(100, empty_tbl) == 2);
    CHECK(race::lookup_skill_adjustment(110, empty_tbl) == 15);
    const int harder = race::update_dynamic_ai_skill(16, 92.0, 100.0, 1, race::AiMode::kStock, empty_tbl);
    CHECK(harder == 7);  // 16 + (-9)/1
    const int damped = race::update_dynamic_ai_skill(16, 92.0, 100.0, 5, race::AiMode::kStock, empty_tbl);
    CHECK(damped == 13);  // 16 + round(-9 / 3)
    CHECK(race::update_dynamic_ai_skill(16, 92.0, 100.0, 1, race::AiMode::kFair, empty_tbl) == 16);
    CHECK(race::update_dynamic_ai_skill(16, 92.0, 100.0, 1, race::AiMode::kHard, empty_tbl) == 16);
    CHECK(race::difficulty_from_dynamic_skill(1) == race::Difficulty::kPro);
    CHECK(race::difficulty_from_dynamic_skill(4) == race::Difficulty::kHard);
    CHECK(race::difficulty_from_dynamic_skill(16) == race::Difficulty::kMed);
    CHECK(race::difficulty_from_dynamic_skill(20) == race::Difficulty::kEasy);

    // Nemesis rival boost: multi-AI vs 1v1 torque ramp.
    const auto off_start = race::nemesis_rival_boost(true, false, 6, 2, 80.0, 2000.0, 0.84, 0.90);
    CHECK(!off_start.active && off_start.torque_add == 0.0 && near(off_start.cornering, 0.90));
    const auto multi_on = race::nemesis_rival_boost(true, true, 6, 2, 0.0, 2000.0, 0.84, 0.90);
    CHECK(multi_on.active && near(multi_on.braking, 1.0) && near(multi_on.cornering, 1.25) &&
          near(multi_on.torque_add, 0.3));
    const auto multi_lead = race::nemesis_rival_boost(true, true, 6, 0, 0.0, 2000.0, 0.84, 0.90);
    CHECK(!multi_lead.active && multi_lead.torque_add == 0.0);
    const auto one_v_one = race::nemesis_rival_boost(true, true, 0, 0, 75.0, 2000.0, 0.84, 0.90);
    CHECK(one_v_one.active && near(one_v_one.braking, 0.84) && near(one_v_one.cornering, 0.90) &&
          near(one_v_one.torque_add, 0.075, 1e-4));
    const auto near_fin = race::nemesis_rival_boost(true, true, 0, 0, 100.0, 400.0, 0.84, 0.90);
    CHECK(!near_fin.active && near_fin.torque_add == 0.0);

    // Difficulty bonus for HARD+ (kHardPlus = 4 -> 60%).
    CHECK(race::difficulty_bonus_percent(4) == 60);

    // Street & Nemesis races enable live traffic (CareerRaceModes.xml UseTraffic="true").
    CHECK(race::event_uses_traffic(4));   // Street
    CHECK(race::event_uses_traffic(11));  // Nemesis
    CHECK(!race::event_uses_traffic(1));  // Festival
    CHECK(!race::event_uses_traffic(8));  // Showcase

    // Race AI traffic avoidance: plan_lanes steers around traffic obstacle & caps speed if blocked.
    {
      drive::RacingLine rl;
      for (int i = 0; i <= 40; ++i) {
        rl.x.push_back(0.0f);
        rl.y.push_back(0.0f);
        rl.z.push_back(float(i * 10));
        rl.dist.push_back(float(i * 10));
        rl.radius.push_back(1e6f);
        rl.speed.push_back(60.0f);
        rl.room_left.push_back(4.5f);
        rl.room_right.push_back(4.5f);
      }
      double prog[1] = {50.0};
      float cross[1] = {0.0f}, spd[1] = {35.0f}, lane[1] = {0.0f}, cap[1] = {1e9f};
      race::LaneParams lp[1] = {{7.0f, 0.3f, 0.4f}};
      race::TrafficObstacle obs[1] = {{75.0, 0.5f, 13.4f, false}};
      for (int s = 0; s < 30; ++s) race::plan_lanes(rl, prog, cross, spd, lp, 1, lane, 1.0 / 60.0, 3.2, cap, obs);
      CHECK(lane[0] < -1.0f);  // moved left around traffic car at cross = +0.5
    }
  }
  if (const char* root = std::getenv("SHIFT_GAME_ROOT")) disc(root);
  else std::printf("SHIFT_GAME_ROOT not set: disc checks skipped\n");
  std::printf(failures ? "race_tests: %d failure(s)\n" : "race_tests: ok\n", failures);
  return failures ? 1 : 0;
}
