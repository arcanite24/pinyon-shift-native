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
#include "drive/lamps.h"
#include "drive/oracle_lap.h"
#include "drive/save.h"
#include "fh1data/archive.h"
#include "fh1data/stringtables.h"
#include "race/campaign_flow.h"
#include "race/difficulty.h"
#include "race/event.h"
#include "race/owt.h"
#include "race/rally_codriver.h"
#include "race/rally.h"
#include "race/race_line.h"
#include "race/rewards.h"
#include "race/rivals.h"
#include "race/route.h"
#include "race/dls.h"
#include "race/session.h"
#include "race/showcase.h"
#include "race/street_line.h"

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

// WORLD-16: the [HandOfGodReset] block parsed from made-up ini text (values
// unlike the disc's so a fallback cannot pass), applied to the session's
// collision windows, and the split reset cross-fade.
void test_reset_timings() {
  const char* ini =
      "[Misc]\n"
      "Calendar\\TrophyCreditsMinTimeSeconds 2\n"
      "HandOfGodReset\\FacingSpeedThresholdMPH 41\n"
      "HandOfGodReset\\FacingCosAngle 0.5\n"
      "HandOfGodReset\\FacingTimeBeforeReset 3.5\n"
      "HandOfGodReset\\ProgressThresholdMPH 6\n"
      "HandOfGodReset\\ProgressCosAngle 0.9\n"
      "HandOfGodReset\\ProgressTimeBeforeReset 4\n"
      "HandOfGodReset\\OffTrackAmountMeters0 7\n"
      "HandOfGodReset\\OffTrackTimeBeforeReset0 8\n"
      "HandOfGodReset\\OffTrackAmountMeters1 17\n"
      "HandOfGodReset\\OffTrackTimeBeforeReset1 2.5\n"
      "HandOfGodReset\\PostResetNonCollideTime 4.5\n"
      "HandOfGodReset\\OptimalSpeedScale 0.6\n"
      "HandOfGodReset\\FadeOutTime 1.5\n"
      "HandOfGodReset\\FadeInTime 0.75\n";  // CollisionsOffTime left out
  race::HandOfGodReset h;
  std::string e;
  CHECK(race::parse_hand_of_god_reset(ini, h, &e));
  CHECK(near(h.facing_speed_mph, 41) && near(h.facing_cos, 0.5) && near(h.facing_time, 3.5));
  CHECK(near(h.progress_mph, 6) && near(h.progress_cos, 0.9) && near(h.progress_time, 4));
  CHECK(near(h.off_track_m0, 7) && near(h.off_track_time0, 8) && near(h.off_track_m1, 17) &&
        near(h.off_track_time1, 2.5));
  CHECK(near(h.post_reset_non_collide, 4.5) && near(h.optimal_speed_scale, 0.6));
  CHECK(near(h.fade_out, 1.5) && near(h.fade_in, 0.75));
  CHECK(near(h.collisions_off, 2.0));  // missing: the fallback, and listed
  CHECK(h.missing.size() == 1 && h.missing[0] == "HandOfGodReset\\CollisionsOffTime");
  race::HandOfGodReset none;
  CHECK(!race::parse_hand_of_god_reset("Calendar\\TrophyCreditsMinTimeSeconds 2\n", none, &e));
  CHECK(none.missing.size() == 15 && near(none.fade_out, 2) && near(none.post_reset_non_collide, 3));

  // The session takes the timings: no car contact over the last
  // CollisionsOffTime before the reset and PostResetNonCollideTime after it.
  h.collisions_off = 0.5;
  race::Session::Options o;
  o.countdown = 0;
  race::apply_hand_of_god(h, o);
  CHECK(near(o.reset_fade_out, 1.5) && near(o.reset_fade_in, 0.75) && near(o.post_reset_non_collide, 4.5) &&
        near(o.collisions_off, 0.5));
  race::Gate g{0, 10, 0, 1, 25};
  race::Session s(1, true, 1, g, std::nullopt, o);
  race::CarSample still;  // stopped: the no-progress rule
  double off_at = -1, reset_at = -1;
  for (int k = 0; k < 60 && reset_at < 0; ++k) {
    s.step(0.1, std::span(&still, 1));
    if (!s.collides(0) && off_at < 0) off_at = s.clock();
    if (!s.resets().empty()) reset_at = s.clock();
  }
  CHECK(reset_at > 3.95 && reset_at < 4.15);  // ProgressTimeBeforeReset 4
  CHECK(off_at > 3.45 && off_at < 3.65);      // collisions off from 0.5 s before
  s.reset_done(0);
  CHECK(!s.collides(0) && !s.car(0).collisions_off);
  {
    race::Session t(1, true, 1, g, std::nullopt, o);
    race::CarSample moving;
    moving.speed = 20;
    t.step(0.1, std::span(&moving, 1));
    t.reset_done(0);
    for (int k = 0; k < 44; ++k) t.step(0.1, std::span(&moving, 1));
    CHECK(!t.collides(0));  // 4.4 s < 4.5 s
    t.step(0.2, std::span(&moving, 1));
    CHECK(t.collides(0));
  }

  // The reset cross-fade: covered at FadeOutTime, clear after FadeInTime.
  race::TransitionState tr;
  tr.start_split(race::TransitionKind::kCrossFade, 2.0, 1.0);
  CHECK(near(tr.duration_s, 3.0) && near(tr.peak, 2.0 / 3.0));
  bool mid = false;
  for (int k = 0; k < 7; ++k) mid |= tr.update(0.25);  // 1.75 s: still covering
  CHECK(!mid && tr.cover_alpha() > 0.9f && tr.cover_alpha() < 1.0f);
  mid = tr.update(0.26);  // 2.01 s
  CHECK(mid && tr.cover_alpha() > 0.999f);
  tr.update(0.49);  // 2.5 s: half way back
  CHECK(tr.cover_alpha() > 0.4f && tr.cover_alpha() < 0.6f);
  tr.start(race::TransitionKind::kCrossFade, 1.0);
  CHECK(near(tr.peak, 0.5));

  // AIRacing.xml ResetParameters by set (made-up values).
  const char* xml =
      "<?xml version=\"1.0\"?>\n<RacingAI>\n"
      "<Settings name=\"default\"><Batch><DLS CarCost=\"400\"/><MetaAIDriver><ResetParameters\n"
      " ResetIfStuckWithinN1Waypoints_NumWaypoints=\"2\" ResetIfStuckWithinN1Waypoints_MinTime=\"11.0\"\n"
      " ResetIfStuckWithinN2Waypoints_NumWaypoints=\"5\" ResetIfStuckWithinN2Waypoints_MinTime=\"12.0\"\n"
      " ResetIfStuckWithinN3Waypoints_NumWaypoints=\"9\" ResetIfStuckWithinN3Waypoints_MinTime=\"13.0\"\n"
      " ResetIfStuckWithinThisDistance_Distance_m=\"1.5\" ResetIfStuckWithinThisDistance_MinTime=\"14.0\"\n"
      " ResetIfGoingThisSlow_Speed_mpers=\"2.5\" ResetIfGoingThisSlow_MinTime=\"15.0\"\n"
      " ResetIfGoingThisSlowUpsideDown_Speed_mpers=\"4.0\" ResetIfGoingThisSlowUpsideDown_MinTime=\"2.0\"\n"
      " ResetIfGoingThisSlowUpsideDown_UpVectorYRolloverThreshold=\"0.25\"\n"
      " ResetIfFellThisLow=\"-70.0\" CancelResetIfMovedThisFar_m=\"18.0\"/></MetaAIDriver></Batch></Settings>\n"
      "<Settings name=\"streetrace\"><Batch><MetaAIDriver><ResetParameters\n"
      " ResetIfStuckWithinN1Waypoints_NumWaypoints=\"1\"/></MetaAIDriver></Batch></Settings>\n"
      "<Settings name=\"freeroam\"><Batch/></Settings>\n"
      "</RacingAI>\n";
  race::AiResetRules ar;
  CHECK(race::parse_ai_reset_rules(xml, "default", ar, &e));
  CHECK(ar.stuck_waypoints[0].count == 2 && near(ar.stuck_waypoints[0].min_time, 11));
  CHECK(ar.stuck_waypoints[1].count == 5 && near(ar.stuck_waypoints[1].min_time, 12));
  CHECK(ar.stuck_waypoints[2].count == 9 && near(ar.stuck_waypoints[2].min_time, 13));
  CHECK(near(ar.stuck_distance_m, 1.5) && near(ar.stuck_distance_time, 14));
  CHECK(near(ar.slow_speed, 2.5) && near(ar.slow_time, 15));
  CHECK(near(ar.upside_down_speed, 4) && near(ar.upside_down_time, 2) && near(ar.upside_down_up_y, 0.25));
  CHECK(near(ar.fell_low, -70) && near(ar.cancel_moved_m, 18));
  e.clear();
  CHECK(!race::parse_ai_reset_rules(xml, "streetrace", ar, &e) && e.find("lacks") != std::string::npos);
  CHECK(!race::parse_ai_reset_rules(xml, "freeroam", ar, &e) && e.find("ResetParameters") != std::string::npos);
  CHECK(!race::parse_ai_reset_rules(xml, "nosuchset", ar, &e));
  CHECK(std::string(race::ai_settings_set(true)) == "streetrace" &&
        std::string(race::ai_settings_set(false)) == "default" &&
        std::string(race::ai_settings_set(false, true)) == "freeroam");
}

// WORLD-04: AI cars reset by their ResetParameters, the player by the Hand
// of God triggers (made-up rules).
void test_ai_reset_rules() {
  race::AiResetRules r;
  r.stuck_waypoints = {{{1, 20.0}, {3, 21.0}, {5, 22.0}}};
  r.stuck_distance_m = 2.0, r.stuck_distance_time = 9.0;
  r.slow_speed = 3.0, r.slow_time = 10.0;
  r.upside_down_speed = 3.0, r.upside_down_time = 2.0, r.upside_down_up_y = 0.3;
  r.fell_low = -50.0, r.cancel_moved_m = 20.0;
  race::Gate g{0, 1e6, 0, 1, 25};  // never crossed
  auto make = [&](const race::AiResetRules* rules, bool ai_hog = false) {
    race::Session::Options o;
    o.countdown = 0;
    o.player_index = 1;
    if (rules) o.ai_rules = *rules;
    o.ai_hand_of_god = ai_hog;
    return race::Session(2, false, 1, g, race::Gate{0, 2e6, 0, 1, 25}, o);
  };
  // Run until the first reset of each car (or `until` s); returns the times.
  struct Run {
    double ai = -1, player = -1;
    std::string ai_reason;
    bool ai_off_early = false, ai_off_late = false;
  };
  auto run = [&](race::Session& s, race::CarSample ai, race::CarSample player, double until, double ai_vx = 0,
                 double ai_vz = 0) {
    Run out;
    player.waypoint = 0;
    for (int k = 0; k < int(until / 0.1 + 0.5); ++k) {
      ai.x0 = ai.x1, ai.z0 = ai.z1;
      ai.x1 += ai_vx * 0.1, ai.z1 += ai_vz * 0.1;
      race::CarSample cs[2] = {ai, player};
      if (k == 0) cs[1].speed = 10;  // the player launches (no start-line grace)
      s.step(0.1, cs);
      if (std::fabs(s.clock() - 6.5) < 0.05) out.ai_off_early = !s.collides(0);
      if (std::fabs(s.clock() - 7.5) < 0.05) out.ai_off_late = !s.collides(0);
      for (size_t i : s.resets()) {
        if (i == 0 && out.ai < 0) out.ai = s.clock(), out.ai_reason = s.car(0).reason;
        if (i == 1 && out.player < 0) out.player = s.clock();
        if (i == 1) s.reset_done(1);
      }
      if (out.ai >= 0) break;
    }
    return out;
  };
  race::CarSample still;
  still.waypoint = 5;
  {
    // Stopped: the player goes at 3 s, the AI at 9 s (within 2 m), with its
    // collisions off over the last 2 s.
    auto s = make(&r);
    const Run o = run(s, still, still, 30);
    CHECK(o.player > 2.95 && o.player < 3.25);
    CHECK(o.ai > 8.95 && o.ai < 9.25 && o.ai_reason == "stuck in place");
    CHECK(!o.ai_off_early && o.ai_off_late);
  }
  {
    auto s = make(&r);
    race::CarSample up = still;
    up.up_y = 0.1;
    const Run o = run(s, up, still, 30);
    CHECK(o.ai > 1.9 && o.ai < 2.15 && o.ai_reason == "upside down");
  }
  {
    auto s = make(&r);
    race::CarSample low = still;
    low.y = -60;
    const Run o = run(s, low, still, 1);
    CHECK(near(o.ai, 0.1, 1e-9) && o.ai_reason == "fell");
  }
  {
    // Stuck on the line: within one point for 20 s (the other rules off).
    race::AiResetRules w = r;
    w.stuck_distance_time = 1e9, w.slow_time = 1e9;
    auto s = make(&w);
    const Run o = run(s, still, still, 30);
    CHECK(o.ai > 19.95 && o.ai < 20.25 && o.ai_reason == "stuck on the line");
  }
  {
    // Crawling at 2.5 m/s past a point a metre: too slow after 10 s. Moving
    // on (20 m by 8 s) does not cancel it: the cancel distance is measured
    // from the stuck-distance anchor, which follows the car (Q232).
    race::CarSample crawl = still;
    crawl.speed = 2.5;
    auto s = make(&r);
    Run o;
    {
      // The line point follows the car (1 m apart).
      race::CarSample ai = crawl, pl = still;
      pl.speed = 20;
      for (int k = 0; k < 300 && o.ai < 0; ++k) {
        ai.x0 = ai.x1, ai.z0 = ai.z1, ai.z1 += 0.25, ai.waypoint = std::floor(ai.z1);
        race::CarSample cs[2] = {ai, pl};
        s.step(0.1, cs);
        for (size_t i : s.resets())
          if (i == 0) o.ai = s.clock(), o.ai_reason = s.car(0).reason;
      }
    }
    CHECK(o.ai > 9.9 && o.ai < 10.25 && o.ai_reason == "too slow");
    // A jump past the cancel distance restarts the timers.
    auto c = make(&r);
    bool any = false;
    race::CarSample ai = crawl, pl = still;
    pl.speed = 20;
    for (int k = 0; k < 150; ++k) {
      ai.x0 = ai.x1, ai.z0 = ai.z1, ai.z1 += (k == 60 ? 25.0 : 0.25), ai.waypoint = std::floor(ai.z1);
      race::CarSample cs[2] = {ai, pl};
      c.step(0.1, cs);
      for (size_t i : c.resets()) any |= i == 0;
    }
    CHECK(!any);
    // The timers ride along in a Rewind snapshot.
    const auto snap = c.snapshot();
    CHECK(snap.cars[0].ai.slow_t > 8.0 && snap.cars[0].ai.slow_t < 9.1);
  }
  {
    // The waypoint bands share one timer: two points past the anchor (under
    // N2 = 3) fires at MinTime2 (21 s), not at MinTime1.
    race::AiResetRules w = r;
    w.stuck_distance_time = 1e9, w.slow_time = 1e9;
    auto s = make(&w);
    race::CarSample two = still;
    Run o;
    race::CarSample pl = still;
    pl.speed = 20;
    for (int k = 0; k < 300 && o.ai < 0; ++k) {
      two.waypoint = k < 50 ? 5 : 7;
      race::CarSample cs[2] = {two, pl};
      s.step(0.1, cs);
      for (size_t i : s.resets())
        if (i == 0) o.ai = s.clock(), o.ai_reason = s.car(0).reason;
    }
    CHECK(o.ai > 20.95 && o.ai < 21.25 && o.ai_reason == "stuck on the line");
  }
  {
    // No AI rules: the AI does not reset (or, asked, by the player's triggers).
    auto s = make(nullptr);
    const Run o = run(s, still, still, 30);
    CHECK(o.ai < 0 && o.player > 0);
    auto h = make(nullptr, true);
    const Run p = run(h, still, still, 30);
    CHECK(p.ai > 2.9 && p.ai < 3.15 && p.ai_reason == "no progress");
  }
  {
    // The Hand of God triggers follow the options (apply_hand_of_god).
    race::HandOfGodReset hog;
    hog.progress_mph = 10, hog.progress_time = 1.5, hog.off_track_m0 = 3;
    race::Session::Options o;
    o.countdown = 0;
    race::apply_hand_of_god(hog, o);
    CHECK(near(o.progress_mph, 10) && near(o.progress_time, 1.5) && near(o.off_route_near, 3));
    race::Session s(1, false, 1, g, std::nullopt, o);
    race::CarSample slow7;
    slow7.speed = 7 * 0.44704;  // under 10 mph
    double at = -1;
    for (int k = 0; k < 40 && at < 0; ++k) {
      s.step(0.1, std::span(&slow7, 1));
      if (!s.resets().empty()) at = s.clock();
    }
    CHECK(at > 1.45 && at < 1.65);
  }
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
  {
    // WORLD-05: the start boost (GlobalRegistry.xml values). Seven other
    // race AI; the player level with the car, so no gap rule.
    const race::RaceBoostTunables t;
    auto b = race::race_boost(t, 0, 0, 0, 7, 0, 5000, false);  // the leader at the start
    CHECK(near(b.torque, 0.6) && near(b.skill, 2.0) && std::string(b.kind) == "start_full");
    b = race::race_boost(t, 100, 110, 20, 7, 7, 5000, false);  // the last car, just behind the player
    CHECK(near(b.torque, 0.2) && near(b.skill, 2.0));
    CHECK(!race::race_boost(t, 100, 100, 20, 7, 7, 5000, false).active());  // the last AI level with the player
    b = race::race_boost(t, 750, 760, 20, 7, 7, 5000, false);  // last: held 250 m, then 500 of the 1,000 m decay
    CHECK(near(b.torque, 0.1) && near(b.skill, 1.5) && std::string(b.kind) == "start_decay");
    b = race::race_boost(t, 1000, 1000, 40, 7, 0, 5000, false);  // the leader: held 500 m, half decayed
    CHECK(near(b.torque, 0.3) && near(b.skill, 1.5));
    CHECK(!race::race_boost(t, 1500, 1500, 40, 7, 0, 5000, false).active());
    // More than GapPerCar x (others - ahead) ahead of the player: the minimum (0).
    CHECK(!race::race_boost(t, 300, 300 - 141, 40, 7, 0, 5000, false).active());
    CHECK(race::race_boost(t, 300, 300 - 139, 40, 7, 0, 5000, false).active());
    // Behind the player above 25 m/s: TorqueScaleBoostBehindPlayer (0); slower, the normal boost.
    CHECK(!race::race_boost(t, 300, 320, 26, 7, 3, 5000, false).active());
    CHECK(race::race_boost(t, 300, 320, 24, 7, 3, 5000, false).active());
    // The compiled-in defaults (the old spec figures) under the same rule.
    race::RaceBoostTunables d;
    d.start_torque_first = 0.4, d.start_torque_last = 0.1, d.keep_full_first = d.keep_full_last = 300, d.decay = 700;
    CHECK(near(race::race_boost(d, 0, 0, 0, 7, 0, 5000, false).torque, 0.4));
    CHECK(near(race::race_boost(d, 650, 650, 30, 7, 0, 5000, false).torque, 0.2));
  }
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

// SPEC-RACE-AI mode table: over-braking holds 1 + Amount for a randomised
// 0.5-2.0 s window, then the rolled value; under-cornering is the minimum
// less Amount (floor 0.25); the torque cut puts both at the row's minima.
void test_rubberband_modes() {
  auto e = made_up_rows();
  race::RubberbandRow r = e.rubberbands[0];
  r.modes = {{3, 1.0, 6.5}};
  race::DriverAi ai;
  ai.init(e.skills[0], &r, 11);
  ai.update(20, -1, 0.4);  // 20 m ahead: rubber band, mode 3 drawn (< 0.5 s hold)
  CHECK(ai.mode() == 3 && near(ai.braking(), 7.5));
  ai.update(21, -1, 1.7);  // 2.1 s into the mistake (> 2.0 s max hold)
  CHECK(ai.mode() == 3 && ai.braking() >= 0.5 && ai.braking() <= 0.6);
  // Across seeds, mistake durations vary inside [0.5, 2.0] s (not fixed at 1 s).
  int short_hold = 0, long_hold = 0;
  for (uint64_t seed = 1; seed <= 40; ++seed) {
    race::DriverAi m;
    m.init(e.skills[0], &r, seed);
    m.update(20, -1, 1.25);
    if (m.braking() < 1.0) ++short_hold;
    else ++long_hold;
  }
  CHECK(short_hold > 0 && long_hold > 0);
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

  // The decoded per-assist terms. The defaults (ABS on, normal steering,
  // full line, TCS on, automatic, rewind on) pay +20%.
  race::DifficultyAssists assists;
  CHECK(assists.rewind_on);
  CHECK(race::assist_difficulty_bonus_percent(assists) == 20);
  CHECK(race::total_difficulty_bonus_percent(1, assists) == 25);
  // Every assist at its easiest: nothing.
  race::DifficultyAssists easy;
  easy.braking = 0, easy.steering = 0;
  CHECK(race::assist_difficulty_bonus(easy) == 0.0);
  CHECK(race::total_difficulty_bonus_percent(0, easy) == 0);
  // One term at a time.
  {
    auto a = easy; a.driving_line = 1; CHECK(race::assist_difficulty_bonus_percent(a) == 10);
    a.driving_line = 0; CHECK(race::assist_difficulty_bonus_percent(a) == 20);
  }
  {
    auto a = easy; a.braking = 1; CHECK(race::assist_difficulty_bonus_percent(a) == 10);
    a.braking = 2; CHECK(race::assist_difficulty_bonus_percent(a) == 20);
  }
  {
    auto a = easy; a.steering = 1; CHECK(race::assist_difficulty_bonus_percent(a) == 10);
    a.steering = 2; CHECK(race::assist_difficulty_bonus_percent(a) == 20);
  }
  {
    auto a = easy; a.tcs_on = false; CHECK(race::assist_difficulty_bonus_percent(a) == 15);
  }
  {
    auto a = easy; a.shifting = 1; CHECK(race::assist_difficulty_bonus_percent(a) == 15);
    a.shifting = 2; CHECK(race::assist_difficulty_bonus_percent(a) == 20);
  }
  {
    // Stability control and damage are not counted.
    auto a = easy; a.stm_on = true; a.damage_mode = 2; CHECK(race::assist_difficulty_bonus_percent(a) == 0);
  }
  // Opponents: Easy 0, Medium 5, Hard 15, Pro 25.
  CHECK(race::total_difficulty_bonus_percent(2, easy) == 15);
  CHECK(race::total_difficulty_bonus_percent(3, easy) == 25);
  // Everything off on Pro: the 140% ceiling.
  race::DifficultyAssists hardest;
  hardest.rewind_on = false, hardest.braking = 2, hardest.steering = 2, hardest.tcs_on = false;
  hardest.shifting = 2, hardest.driving_line = 0;
  CHECK(race::total_difficulty_bonus_percent(3, hardest) == 140);
  // Session mode 3 skips the Rewind and the opponent terms.
  easy.rewind_on = false;
  CHECK(race::assist_difficulty_bonus_percent(easy) == 20);
  easy.session_mode = race::DifficultyAssists::kExcludedSessionMode;
  CHECK(race::assist_difficulty_bonus(easy) == 0.0);
  CHECK(race::total_difficulty_bonus_percent(3, easy) == 0);

  // End-to-end award_race with Rewind OFF (+20%) on Easy (0%).
  easy.session_mode = 0;
  race::CareerRecord r_bonus;
  const int pct = race::total_difficulty_bonus_percent(0, easy);
  const auto a_bonus = race::award_race(r_bonus, 24, 145, 2, 1, 2, 10000, 8, 1, true, 120, 60, 0, t, pct, 0);
  CHECK(a_bonus.base_credits == 10000);
  CHECK(a_bonus.difficulty_pct == 20);
  CHECK(a_bonus.difficulty_bonus == 2000);
  CHECK(a_bonus.credits == 12000);
  CHECK(a_bonus.driver_xp == 0 && a_bonus.affinity_xp == 0);  // the FM4-style XP extra is off

  // The bonus is taken over FINISHED + BEAT THE STAR + MILEAGE, rounded half
  // away from zero; the assist share is split out of it.
  {
    race::WristbandTables wt = t;
    wt.nemesis_bonuses = {500, 1000, 1500};
    race::RaceBonusInputs ex;
    ex.star_place = 3;
    ex.assists_pct = 10;
    ex.mileage.total_distance_m = 50000, ex.mileage.reward = 5000, ex.mileage.increment = 100;
    race::CareerRecord rr;
    // 2nd of 8 in a festival race (style 1, level 2): 10000*7/8 = 8750 -> 8800.
    const auto a = race::compute_race_award(rr, 25, 2, 1, 2, 10000, 8, 2, true, 0, wt, 25, 4900, ex);
    CHECK(a.base_credits == 8800);
    CHECK(a.nemesis_bonus == 1500);
    CHECK(a.mileage_bonus == 400);
    CHECK(a.difficulty_bonus + a.assists_bonus == 2675);  // round(10700 * 0.25)
    CHECK(a.assists_bonus == 1070);  // round(10700 * 0.10)
    CHECK(a.credits == 8800 + 1500 + 400 + 2675);
    // Behind the star, or level with no star in the field: no BEAT THE STAR.
    ex.star_place = 2;
    CHECK(race::compute_race_award(rr, 25, 2, 1, 2, 10000, 8, 2, true, 0, wt, 25, 4900, ex).nemesis_bonus == 0);
    ex.star_place = 0;
    CHECK(race::compute_race_award(rr, 25, 2, 1, 2, 10000, 8, 2, true, 0, wt, 25, 4900, ex).nemesis_bonus == 0);
    // Not a festival race: none either.
    ex.star_place = 3;
    CHECK(race::compute_race_award(rr, 25, 4, 2, 2, 10000, 8, 2, true, 0, wt, 25, 4900, ex).nemesis_bonus == 0);
    // Already won before: paid again (the payout reads no past results).
    rr.event_places[25] = 1;
    CHECK(race::compute_race_award(rr, 25, 2, 1, 2, 10000, 8, 2, true, 0, wt, 25, 4900, ex).nemesis_bonus == 1500);
    // The FM4-style XP extra, when on, is the only source of XP rows.
    ex.legacy_fm4_xp = true;
    CHECK(race::compute_race_award(rr, 25, 2, 1, 2, 10000, 8, 2, true, 0, wt, 25, 4900, ex).driver_xp > 0);
  }
  // CashPrize is rounded to two significant figures before the split.
  CHECK(race::race_credits(12345, race::Winnings::kDefault, 1 + 1, 1, true) == 12000);
  CHECK(race::race_credits(9, race::Winnings::kDefault, 8, 1, true) == 0);
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
  // The passing search: a car 10 m behind a slower one picks the first
  // clear slot beside it (3.5 m: the covered slots end at 2.5 m, the 3 m
  // slot pays half the next slot's time-to-collision cost); once past it,
  // it returns to the line.
  {
    std::vector<race::DlsCar> cars(2);
    cars[0].progress = 100, cars[0].speed = 30;
    cars[1].progress = 110, cars[1].speed = 20, cars[1].kind = race::DlsCar::Kind::kHuman;
    const race::DlsMeeting m = race::dls_predict(cars[0], 0, cars[1]);
    CHECK(m.valid && m.hit && near(m.t, (10 - 4.6) / 10, 0.04));
    float to = 0;
    uint8_t side = 0;
    const race::DlsSettings set;
    const double c = race::dls_search(rl.line, cars, 0, 0, 1, m, 0, set, to, side);
    CHECK(c < set.max_acceptable_cost && near(std::fabs(to), 3.5, 1e-4));
    // Sticking to a side: the other side costs ChangePassSidesCost more.
    float to2 = 0;
    uint8_t side2 = 0;
    race::dls_search(rl.line, cars, 0, 0, 1, m, 2, set, to2, side2);
    CHECK(to2 > 0 && side2 == 2);
    std::vector<race::DlsState> st(1);
    std::vector<float> lane{0, 0}, cap{1e9f, 1e9f};
    std::vector<race::LaneParams> lp(1);
    for (int k = 0; k < 60; ++k) {
      race::dls_update(rl.line, cars, 1, lp, set, 0.05, st, lane, cap);
      cars[0].cross = lane[0];
    }
    CHECK(st[0].target == 1 && std::fabs(lane[0]) > 3.0 && std::fabs(lane[0]) <= 3.5 + 1e-4 && lane[1] == 0);
    cars[1].progress = 80;
    for (int k = 0; k < 200; ++k) {
      race::dls_update(rl.line, cars, 1, lp, set, 0.05, st, lane, cap);
      cars[0].cross = lane[0];
    }
    CHECK(st[0].target == -1 && lane[0] == 0);
  }
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
  CHECK(fr22.ai_player_colors.size() == 159);
  std::vector<race::AiPlayerColor> disc_colors;
  CHECK(race::load_ai_player_colors(root, disc_colors) && disc_colors.size() == 159);
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
  test_reset_timings();
  test_ai_reset_rules();
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

    // Nemesis rival boost (WORLD-05): once the start boost is over, with
    // another race AI ahead SkillBoostFactor 1.1 and +0.4 torque; one on
    // one, PlayerCatchupTorqueBoost 0.6 ramped over 0 to 150 m behind the
    // player, off within 300 m of the finish.
    const race::RaceBoostTunables t;
    const auto in_start = race::race_boost(t, 800, 800, 30, 6, 2, 2000.0, true);
    CHECK(std::string(in_start.kind) == "start_decay");
    const auto multi_on = race::race_boost(t, 2000, 2000, 30, 6, 2, 2000.0, true);
    CHECK(multi_on.active() && near(multi_on.skill, 1.1) && near(multi_on.torque, 0.4));
    CHECK(near(drive::ai_boost(0.90, multi_on.skill, 1.25), 0.99) && near(drive::ai_boost(0.95, multi_on.skill, 1.0), 1.0));
    CHECK(!race::race_boost(t, 2000, 2000, 30, 6, 0, 2000.0, true).active());  // leading the other AI
    CHECK(!race::race_boost(t, 2000, 2000, 30, 6, 2, 2000.0, false).active());  // not a rival
    const auto one_v_one = race::race_boost(t, 2000, 2075, 30, 0, 0, 2000.0, true);
    CHECK(one_v_one.active() && near(one_v_one.skill, 1.0) && near(one_v_one.torque, 0.3, 1e-9));
    CHECK(near(race::race_boost(t, 2000, 2300, 30, 0, 0, 2000.0, true).torque, 0.6));
    CHECK(!race::race_boost(t, 2000, 2100, 30, 0, 0, 299.0, true).active());  // near the finish
    CHECK(!race::race_boost(t, 2000, 2100, 30, 0, 0, 2000.0, true, false).active());  // the flag off
    // The registry ids map onto the fields.
    const std::map<std::string, double> reg = {{"AI/Racing/RaceBehavior/RaceStartBoost/GapPerCar", 33.0},
                                               {"AI/Racing/RaceBehavior/NemesisBoost/TorqueBoost", 0.25}};
    const auto rt = race::RaceBoostTunables::from(reg);
    CHECK(near(rt.gap_per_car, 33.0) && near(rt.nemesis_torque, 0.25) && near(rt.start_torque_first, 0.6));

    // The engine's extra HARD+ tier (kHardPlus = 4) pays as Pro (25%).
    CHECK(race::difficulty_bonus_percent(4) == 25);

    // Street & Nemesis races enable live traffic (CareerRaceModes.xml UseTraffic="true").
    CHECK(race::event_uses_traffic(4));   // Street
    CHECK(race::event_uses_traffic(11));  // Nemesis
    CHECK(!race::event_uses_traffic(1));  // Festival
    CHECK(!race::event_uses_traffic(8));  // Showcase

    // Race AI traffic avoidance: the passing search steers around a traffic car.
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
      race::DlsCar cars[2];
      cars[0].progress = 50, cars[0].speed = 35;
      cars[1].progress = 75, cars[1].cross = 0.5f, cars[1].speed = 13.4f, cars[1].kind = race::DlsCar::Kind::kTraffic;
      float lane[1] = {0.0f}, cap[1] = {1e9f};
      race::LaneParams lp[1] = {{7.0f, 0.3f, 0.4f}};
      race::DlsState st[1];
      const race::DlsSettings set;
      for (int s = 0; s < 30; ++s) race::dls_update(rl, cars, 1, lp, set, 1.0 / 60.0, st, lane, cap);
      CHECK(st[0].target == 1 && lane[0] < -1.0f);  // moved left around the traffic car at cross = +0.5
    }

    // Airborne Showcases: all 7 PLANE_RACE events + XML parser + spring rubber-banding & timing checkpoints.
    {
      CHECK(race::is_airborne_showcase(8, "PLANE_RACE_001"));
      const auto p1 = race::showcase_spec_for("PLANE_RACE_001");
      const auto p2 = race::showcase_spec_for("PLANE_RACE_002");
      const auto p3 = race::showcase_spec_for("PLANE_RACE_003");
      const auto p4 = race::showcase_spec_for("PLANE_RACE_004");
      const auto p5 = race::showcase_spec_for("PLANE_RACE_005");
      const auto p6 = race::showcase_spec_for("PLANE_RACE_006");
      const auto p7 = race::showcase_spec_for("PLANE_RACE_007");
      CHECK(p1.event_id == 101 && p1.kind == race::ShowcaseAircraftKind::kMustangP51 && near(p1.cross_finish_time_s, 171.68, 0.02));
      CHECK(p2.event_id == 168 && p2.kind == race::ShowcaseAircraftKind::kHelicopter && near(p2.cross_finish_time_s, 121.94, 0.02));
      CHECK(p3.event_id == 172 && p3.kind == race::ShowcaseAircraftKind::kBiplane && near(p3.cross_finish_time_s, 134.70, 0.02));
      CHECK(p4.event_id == 223 && p4.kind == race::ShowcaseAircraftKind::kHotAirBalloon &&
            near(p4.cross_finish_time_s, 96.69, 0.02) && p4.on_countdown && near(p4.rumble_strength, 0.0));
      CHECK(p5.event_id == 220 && p5.kind == race::ShowcaseAircraftKind::kHelicopter && near(p5.cross_finish_time_s, 121.83, 0.02));
      CHECK(p6.event_id == 222 && p6.kind == race::ShowcaseAircraftKind::kMustangP51 && near(p6.cross_finish_time_s, 178.03, 0.02));
      CHECK(p7.event_id == 246 && p7.kind == race::ShowcaseAircraftKind::kBiplane && near(p7.cross_finish_time_s, 108.90, 0.02));

      // Tolerant airborne_challenges.xml parsing (including unclosed </DifficulySettings quirk).
      const char* sample_xml = R"(<?xml version="1.0" encoding="utf-8"?>
        <EventIDtoChallengeMap>
          <ChallengeEvent eventid="168" refname="Horizon Chopper Race">
            <Names objectfilename="ANIM_Showcase_Event_Helicopter_Race.pgeo" anim_in_race="ANIM_Showcase_Event_Helicopter_Race"/>
            <AnimStart on_countdown="false"/>
            <Timing in_race_anim_cross_start_time="0.00" in_race_anim_cross_finish_time="121.94"/>
            <TimingCheckpoints>
              <CheckPoint Time="0.0" SplineDistToNext="100.0" AdditionalRubberBandOffset="0.0"/>
              <CheckPoint Time="0.0" SplineDistToNext="500.0" AdditionalRubberBandOffset="0.0"/>
              <CheckPoint Time="60.0" SplineDistToNext="500.0" AdditionalRubberBandOffset="0.01"/>
              <CheckPoint Time="121.94" SplineDistToNext="100.0" AdditionalRubberBandOffset="0.0"/>
            </TimingCheckpoints>
            <RubberBanding rubber_band_spring="30000.0" accel_rate="0.2" decel_rate="0.1" rubber_band_offset_ahead="0.001"/>
            <DifficulySettings><AnimSpeeds base_mult="1.0" rubber_band_min_mult="0.9" rubber_band_max_mult="1.1"/></DifficulySettings
            <DifficulySettings><AnimSpeeds base_mult="1.0" rubber_band_min_mult="0.9" rubber_band_max_mult="1.1"/></DifficulySettings>
            <RumbleWhenNearPlayer strength="0.6" distance_outer="10.0" distance_inner="1.0"/>
          </ChallengeEvent>
          <ChallengeEvent eventid="223" refname="Balloon Event">
            <Names objectfilename="ANIM_Showcase_Event_Balloon_Race.pgeo" anim_in_race="ANIM_Showcase_Event_Balloon_Race"/>
            <AnimStart on_countdown="true"/>
            <Timing in_race_anim_cross_start_time="0.20" in_race_anim_cross_finish_time="96.69"/>
            <RubberBanding rubber_band_spring="300.0" accel_rate="0.1" decel_rate="0.04" rubber_band_offset_ahead="-0.006"/>
            <DifficulySettings><AnimSpeeds base_mult="1.0" rubber_band_min_mult="0.7" rubber_band_max_mult="1.3"/></DifficulySettings>
            <RumbleWhenNearPlayer strength="0.0" distance_outer="8.0" distance_inner="1.0"/>
          </ChallengeEvent>
        </EventIDtoChallengeMap>)";
      std::vector<race::ShowcaseSpec> parsed_xml;
      std::string xerr;
      CHECK(race::parse_airborne_challenges_xml(sample_xml, parsed_xml, &xerr));
      CHECK(parsed_xml.size() == 2);
      CHECK(parsed_xml[0].event_id == 168 && parsed_xml[0].kind == race::ShowcaseAircraftKind::kHelicopter &&
            parsed_xml[0].timing_checkpoints.size() == 4 && near(parsed_xml[0].rubber_band_min_mult, 0.9) &&
            near(parsed_xml[0].rubber_band_max_mult, 1.1) && near(parsed_xml[0].rumble_strength, 0.6));
      CHECK(parsed_xml[1].event_id == 223 && parsed_xml[1].kind == race::ShowcaseAircraftKind::kHotAirBalloon &&
            parsed_xml[1].on_countdown && near(parsed_xml[1].rumble_strength, 0.0));

      drive::RacingLine rl;
      for (int i = 0; i <= 100; ++i) {
        rl.x.push_back(0.0f);
        rl.y.push_back(10.0f);
        rl.z.push_back(float(i * 20));
        rl.dist.push_back(float(i * 20));
        rl.radius.push_back(1e6f);
        rl.speed.push_back(50.0f);
      }
      race::ShowcaseOpponent opp;
      opp.init(p1, rl, 0.0, 1500.0);
      CHECK(opp.active());
      for (int s = 0; s < 120; ++s) opp.step(0.05, s * 0.05, s * 2.5);
      CHECK(opp.pose().position[1] > 18.0 && opp.pose().progress_m > 200.0);
      CHECK(opp.player_place(1500.0, true, 25.0) == 1);

      // Balloon starts moving during countdown (`on_countdown=true`) and emits zero proximity rumble.
      race::ShowcaseOpponent balloon;
      balloon.init(parsed_xml[1], rl, 0.0, 1500.0);
      balloon.step(1.0, -3.0, 0.0);
      CHECK(balloon.anim_time() > 0.5 && near(balloon.near_rumble_strength(2.0), 0.0));

      // PT-26: Terrain-clearance clamping & smooth ridge-climb pitch anticipation across a steep synthetic ridge.
      auto ridge_ground = [](double x, double z) -> double {
        double h = 10.0;
        // Steep 90 m Colorado mountain ridge centered at z = 650 m (z in [500, 800]).
        if (z > 500.0 && z < 800.0) {
          const double t = 1.0 - std::fabs(z - 650.0) / 150.0;
          h = std::max(h, 10.0 + 90.0 * t);
        }
        // Canyon wall on the +X flank along z in [900, 1100] to exercise +/-6 m wingtip clearance.
        if (z > 900.0 && z < 1100.0 && x > 14.0) {
          h = std::max(h, 45.0);
        }
        return h;
      };
      race::GroundHeightFn ridge_probe = [&](double x, double z) -> std::optional<double> {
        return ridge_ground(x, z);
      };
      race::ShowcaseOpponent ridge_opp;
      ridge_opp.init(p1, rl, 0.0, 1500.0);
      ridge_opp.apply_initial_clearance(ridge_probe);
      const double min_clear = ridge_opp.min_clearance_m();
      CHECK(min_clear >= 12.0 && min_clear <= 18.0);
      double max_anticipation_pitch = -1.0;
      double max_step_dpitch = 0.0;
      double max_alt_y = -1e9;
      double post_ridge_y = 1e9;
      double prev_pitch = 0.0;
      bool have_prev_pitch = false;
      bool clearance_ok = true;
      bool finite_ok = true;
      constexpr double kDt = 0.02;
      race::ShowcaseOpponent::Snapshot mid_snap{};
      bool captured_snap = false;
      for (int s = 0; s < 1400; ++s) {
        const double t = s * kDt;
        ridge_opp.step(kDt, t, t * 50.0, ridge_probe);
        const auto& pose = ridge_opp.pose();
        if (!std::isfinite(pose.position[0]) || !std::isfinite(pose.position[1]) || !std::isfinite(pose.position[2]) ||
            !std::isfinite(pose.heading) || !std::isfinite(pose.pitch) || !std::isfinite(pose.roll)) {
          finite_ok = false;
          break;
        }
        const double rx = std::cos(pose.heading), rz = -std::sin(pose.heading);
        const double g_center = ridge_ground(pose.position[0], pose.position[2]);
        const double g_left = ridge_ground(pose.position[0] - rx * 6.0, pose.position[2] - rz * 6.0);
        const double g_right = ridge_ground(pose.position[0] + rx * 6.0, pose.position[2] + rz * 6.0);
        const double g_max = std::max({g_center, g_left, g_right});
        if (pose.position[1] < g_max + min_clear - 1e-6) clearance_ok = false;
        if (pose.position[2] >= 320.0 && pose.position[2] < 495.0) {
          max_anticipation_pitch = std::max(max_anticipation_pitch, pose.pitch);
        }
        if (have_prev_pitch) {
          max_step_dpitch = std::max(max_step_dpitch, std::fabs(pose.pitch - prev_pitch));
        }
        prev_pitch = pose.pitch;
        have_prev_pitch = true;
        max_alt_y = std::max(max_alt_y, pose.position[1]);
        if (pose.position[2] > 1180.0 && pose.position[2] < 1450.0) {
          post_ridge_y = std::min(post_ridge_y, pose.position[1]);
        }
        if (s == 450) {
          mid_snap = ridge_opp.snapshot();
          captured_snap = true;
        }
      }
      CHECK(finite_ok);
      CHECK(clearance_ok);
      CHECK(max_anticipation_pitch > 0.08);
      CHECK(max_step_dpitch <= 1.6 * kDt + 1e-5);
      CHECK(max_alt_y >= 100.0 + min_clear - 1e-6);
      CHECK(post_ridge_y < max_alt_y - 25.0);
      if (captured_snap) {
        ridge_opp.restore(mid_snap);
        CHECK(near(ridge_opp.pose().position[1], mid_snap.pose.position[1], 1e-9));
        CHECK(near(ridge_opp.pose().pitch, mid_snap.pose.pitch, 1e-9));
      }

      // PT-26: World-space exhaust & wingtip VFX emitter anchors derived from aircraft pose.
      {
        race::ShowcasePose tp{};
        tp.position = {120.0, 85.0, -340.0};
        tp.heading = 0.7;
        tp.pitch = 0.22;
        tp.roll = -0.35;
        tp.speed_mps = 55.0;
        tp.ground_y = 68.0;
        tp.agl_m = 17.0;
        const auto anc = race::compute_showcase_vfx_anchors(p1, tp);
        auto is_fin3 = [](const std::array<double, 3>& v) {
          return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
        };
        auto dot3 = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
          return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        };
        auto sub3 = [](const std::array<double, 3>& a, const std::array<double, 3>& b) -> std::array<double, 3> {
          return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        };
        auto len3 = [&](const std::array<double, 3>& v) { return std::sqrt(dot3(v, v)); };

        CHECK(anc.exhaust_count == 2 && anc.wingtip_count == 2);
        CHECK(is_fin3(anc.forward) && is_fin3(anc.right) && is_fin3(anc.up) && is_fin3(anc.velocity));
        CHECK(is_fin3(anc.exhaust_pos[0]) && is_fin3(anc.exhaust_pos[1]));
        CHECK(is_fin3(anc.wingtip_pos[0]) && is_fin3(anc.wingtip_pos[1]));
        CHECK(is_fin3(anc.prop_hub_pos) && is_fin3(anc.ground_wash_pos));
        CHECK(near(len3(anc.forward), 1.0, 1e-6) && near(len3(anc.right), 1.0, 1e-6) && near(len3(anc.up), 1.0, 1e-6));
        CHECK(near(dot3(anc.forward, anc.right), 0.0, 1e-6) && near(dot3(anc.forward, anc.up), 0.0, 1e-6));
        CHECK(near(len3(sub3(anc.wingtip_pos[1], anc.wingtip_pos[0])), 2.0 * 6.70 * p1.scale, 1e-4));
        CHECK(near(len3(sub3(anc.exhaust_pos[1], anc.exhaust_pos[0])), 2.0 * 0.68 * p1.scale, 1e-4));
        CHECK(dot3(sub3(anc.exhaust_pos[0], tp.position), anc.forward) > 0.0);
        CHECK(dot3(sub3(anc.exhaust_pos[1], anc.exhaust_pos[0]), anc.right) > 0.0);
        CHECK(dot3(sub3(anc.wingtip_pos[1], anc.wingtip_pos[0]), anc.right) > 0.0);
        CHECK(dot3(sub3(anc.prop_hub_pos, anc.exhaust_pos[0]), anc.forward) > 0.0);
        CHECK(near(anc.agl_m, 17.0, 1e-6) && near(anc.ground_wash_pos[1], 68.0, 1e-6));
      }
    }

    // Full post-race breakdown (Assists, Bounty, Nemesis): no first-win
    // override, no skills credits + Driver Level / Manufacturer Affinity progression.
    {
      race::WristbandTables wt;
      wt.levels = {{0, "Yellow"}, {1000, "Green"}};
      wt.nemesis_bonuses = {1000, 1500};
      race::CareerRecord rec;
      race::RaceBonusInputs ex;
      ex.assists_pct = 20;
      ex.skills_points = 4000;  // 1000 CR skills bonus
      ex.bounty_bonus = 500;
      const auto a1 = race::award_race(rec, 901, 901, 12, 6, 6, 10000.0, 8, 1, true, 120.0, 60.0, 0, wt, 50, 5000.0, ex);
      CHECK(a1.base_credits == 10000);  // the event's own CashPrize, winner takes all
      CHECK(a1.assists_bonus > 0 && a1.difficulty_bonus > 0 && a1.skills_bonus == 0 && a1.bounty_bonus == 500);
      CHECK(a1.credits == a1.base_credits + a1.mileage_bonus + a1.difficulty_bonus + a1.assists_bonus + a1.skills_bonus +
                              a1.bounty_bonus + a1.nemesis_bonus);
      const auto a2 = race::award_race(rec, 902, 902, 11, 5, 1, 20000.0, 2, 1, true, 90.0, 55.0, 0, wt, 15, 4000.0, ex);
      CHECK(a2.nemesis_bonus == 0);  // BEAT THE STAR is a festival (style 1) line only

      race::ProgressionTables pt;
      pt.driver_levels = {{0, 0, 0}, {1, 5000, 1}, {2, 10000, 1}};
      pt.driver_level_cars[2] = {1219};
      pt.affinity_levels = {{0, 0, 0.0, 0}, {1, 2500, 0.25, 1000}, {2, 9000, 0.50, 2000}};
      const auto pu = race::apply_progression_xp(rec, 6000, 42, 3000, pt);
      CHECK(pu.new_driver_level == 2 && pu.gift_cars.size() == 1 && pu.gift_cars[0] == 1219);
      CHECK(pu.gift_picks.empty());
      // V4-23: two or more eligible cars are a pick-one list, capped at the
      // level's MaxGiftCars; IsCarInstalledAndPurchased filters (owned cars
      // stay in).
      race::ProgressionTables gp;
      gp.driver_levels = {{0, 0, 7}, {1, 5000, 7}, {2, 10000, 5}, {3, 15000, 5}};
      gp.driver_level_cars[2] = {11, 12, 13, 14, 15, 16, 17};
      gp.driver_level_cars[3] = {21, 22};
      CHECK((gp.driver_level_gift_choices(2) == std::vector<int64_t>{11, 12, 13, 14, 15}));
      const auto not_installed = [](int64_t c) { return c != 12 && c != 22; };
      CHECK((gp.driver_level_gift_choices(2, not_installed) == std::vector<int64_t>{11, 13, 14, 15, 16}));
      CHECK((gp.driver_level_gift_choices(3, not_installed) == std::vector<int64_t>{21}));
      race::CareerRecord grec;
      const auto gu = race::apply_progression_xp(grec, 15000, 0, 0, gp, not_installed);
      CHECK(gu.new_driver_level == 4 && gu.gift_picks.size() == 1 && gu.gift_picks[0].size() == 5);
      CHECK((gu.gift_cars == std::vector<int64_t>{21}));
      CHECK(pu.new_affinity_level == 1 && near(pu.affinity_discount, 0.25) && pu.affinity_bonus_credits == 1000);
    }

    // DriveSave round-trip for Slice 6 persistence fields.
    {
      drive::DriveSave s1, s2;
      s1.car = "NIS_SkylineGTRVSpecII_02";
      s1.speed_traps = {{"speed_camera_01", 142.5}};
      s1.speed_zones = {{"average_speed_01", 118.2}};
      s1.sponsors = {{"NearMiss", 30, 1}};
      s1.h2h_races = 4, s1.h2h_wins = 3;
      s1.driver_xp = 18500;
      s1.make_affinity = {{42, 9500}};
      std::string err;
      CHECK(drive::save_from_text(drive::save_to_text(s1), s2, &err));
      CHECK(s2.speed_traps.size() == 1 && near(s2.speed_traps[0].second, 142.5, 1e-4));
      CHECK(s2.speed_zones.size() == 1 && near(s2.speed_zones[0].second, 118.2, 1e-4));
      CHECK(s2.sponsors.size() == 1 && s2.sponsors[0].id == "NearMiss" && s2.sponsors[0].count == 30 &&
            s2.sponsors[0].claimed == 1);
      CHECK(s2.h2h_races == 4 && s2.h2h_wins == 3 && s2.driver_xp == 18500);
      CHECK(s2.make_affinity.size() == 1 && s2.make_affinity[0].first == 42 && s2.make_affinity[0].second == 9500);
    }

    // P0-4: Full race Session rewind rollback, clean-lap dirty tracking, Rally co-driver & Showcase rollback.
    {
      race::Gate start{0, 0, 0, 1, 20};
      race::Session sess(2, true, 2, start, std::nullopt, {.countdown = 0.0, .resets = false});
      std::vector<race::CarSample> cs(2);
      cs[0] = {0, -5, 0, 5, 30, 1, 0, 10};
      cs[1] = {0, -5, 0, 5, 25, 1, 0, 8};
      sess.step(1.0, cs);  // clock = 1.0, both cross start line (crossings = 1)
      const auto snap_1s = sess.snapshot();
      CHECK(near(snap_1s.clock, 1.0));
      CHECK(!sess.car(1).lap_dirty && !sess.car(1).race_dirty);

      // Advance 5 more seconds: AI car 0 drives far ahead.
      for (int k = 0; k < 5; ++k) {
        cs[0] = {0, double(10 + k * 40), 0, double(50 + k * 40), 40, 1, 0, double(50 + k * 40)};
        cs[1] = {0, 10, 0, 10, 0, 1, 0, 10};
        sess.step(1.0, cs);
      }
      CHECK(near(sess.clock(), 6.0));
      CHECK(sess.car(0).distance > 150.0);

      // Rewind to t = 1.0 s: AI distance, clock, and order roll back; player's lap marked dirty!
      sess.restore(snap_1s);
      sess.mark_lap_dirty(1);
      CHECK(near(sess.clock(), 1.0));
      CHECK(sess.car(0).distance < 20.0);
      CHECK(sess.car(1).lap_dirty && sess.car(1).race_dirty);

      // Crossing start line for next lap clears lap_dirty while keeping race_dirty.
      cs[0] = {0, -5, 0, 5, 30, 1, 0, 200};
      cs[1] = {0, -5, 0, 5, 30, 1, 0, 200};
      sess.step(1.0, cs);
      CHECK(!sess.car(1).lap_dirty && sess.car(1).race_dirty);

      // Rally co-driver explicit rewind_to un-fires gates crossed after target timestamp.
      race::RallyPaceNoteTracker codriver;
      race::CoDriverAudioMap cmap;
      std::vector<race::RallyCallGate> gates(2);
      gates[0] = {"g0", 0, 0, 20, 0, 1, 30, {"EasyLeft"}};
      gates[1] = {"g1", 0, 0, 60, 0, 1, 30, {"HardRight"}};
      codriver.load(gates, cmap);
      codriver.observe(0.0, 0, 0);
      codriver.observe(2.0, 0, 30);  // fires g0
      codriver.observe(5.0, 0, 70);  // fires g1
      CHECK(codriver.fired_count() == 2);
      codriver.rewind_to(2.5);
      CHECK(codriver.fired_count() == 1);
      CHECK(codriver.pending_audio_cues() == 0);
    }

    // P0-P5: Session rollover detection (up_y < 0.5 sustained triggers "rolled over" reset).
    {
      race::Gate start{0, 0, 0, 1, 20};
      race::Session sess(1, false, 1, start, race::Gate{0, 500, 0, 1, 20},
                         {.countdown = 0.0, .resets = true, .rollover_time = 2.0});
      std::vector<race::CarSample> cs(1);
      // Car is sliding fast on-route (speed=15 m/s, off_route=0) but upside down (up_y=-0.8).
      cs[0] = {0, 10, 0, 25, 15.0, 1.0, 0.0, 25.0, -0.8};
      sess.step(1.0, cs);
      CHECK(sess.resets().empty());
      sess.step(1.1, cs);
      CHECK(sess.resets().size() == 1 && sess.resets()[0] == 0);
      CHECK(std::string(sess.car(0).reason) == "rolled over");
      sess.reset_done(0);
      CHECK(near(sess.car(0).rolled, 0.0));
    }

    // P0-I1: assign_driver() preserves event_rubberband > 10 (Street Race 14, 21..30 & Star Showdown 41..44).
    {
      race::EventData rows;
      race::SkillRow sr;
      sr.id = 20;
      sr.rubberband_id = 3;
      rows.skills.push_back(sr);
      // 1..10: adds rubberband_modifier.
      const auto d_std = race::assign_driver(20, 5, 6, 0, 0, 2, rows);
      CHECK(d_std.rubberband_id == 8);
      // Street Race rows (14, 21..30): kept verbatim without modifier.
      CHECK(race::assign_driver(20, 5, 14, 0, 0, 3, rows).rubberband_id == 14);
      for (int rb = 21; rb <= 30; ++rb) {
        CHECK(race::assign_driver(20, 5, rb, 0, 0, -2, rows).rubberband_id == rb);
      }
      // Star Showdown boss rows (41..44): kept verbatim without modifier.
      for (int rb = 41; rb <= 44; ++rb) {
        CHECK(race::assign_driver(20, 5, rb, 0, 0, 4, rows).rubberband_id == rb);
      }
      // < 1: falls back to skill->rubberband_id.
      CHECK(race::assign_driver(20, 5, 0, 0, 0, 2, rows).rubberband_id == 3);
      CHECK(race::assign_driver(20, 5, -1, 0, 0, 2, rows).rubberband_id == 3);
    }

    // P0-I5 (street_line): skip empty/"none" road_type ways and obey TurnDelay/OneWay.
    {
      traffic::NavMap nav;
      auto add_key = [&](const std::string& k) -> uint32_t {
        for (uint32_t i = 0; i < nav.keys.size(); ++i)
          if (nav.keys[i] == k) return i;
        nav.keys.push_back(k);
        return uint32_t(nav.keys.size() - 1);
      };
      auto add_val = [&](const std::string& v) -> uint32_t {
        for (uint32_t i = 0; i < nav.values.size(); ++i)
          if (nav.values[i] == v) return i;
        nav.values.push_back(v);
        return uint32_t(nav.values.size() - 1);
      };
      auto add_node = [&](float x, float z) -> uint32_t {
        traffic::NavNode nd;
        nd.id = uint32_t(nav.nodes.size() + 1);
        nd.x = x;
        nd.z = z;
        nav.nodes.push_back(nd);
        return uint32_t(nav.nodes.size() - 1);
      };
      auto add_way = [&](const std::vector<uint32_t>& ns, const std::string& rt, bool oneway = false) {
        traffic::NavWay w;
        w.node_first = uint32_t(nav.way_nodes.size());
        w.node_count = uint32_t(ns.size());
        for (uint32_t n : ns) nav.way_nodes.push_back(n);
        w.tag_first = uint32_t(nav.tags.size());
        if (!rt.empty()) nav.tags.push_back({add_key("road_type"), add_val(rt)});
        if (oneway) nav.tags.push_back({add_key("oneway"), add_val("true")});
        w.tag_count = uint32_t(nav.tags.size()) - w.tag_first;
        nav.ways.push_back(w);
      };
      // Non-road outline nodes right on top of start/goal (must be ignored!).
      const uint32_t o0 = add_node(0, 0), o1 = add_node(0, 200);
      add_way({o0, o1}, "none");
      add_way({o0, o1}, "");
      // Real road at x=10, z=0..200.
      const uint32_t r0 = add_node(10, 0), r1 = add_node(10, 100), r2 = add_node(10, 200);
      add_way({r0, r1, r2}, "a");

      race::Route route;
      route.id = 999;
      route.transforms.push_back({"start_location_00", 5, 0, 0, 0, 1, std::nullopt});
      route.transforms.push_back({"route_checkpoint_00", 5, 0, 200, 0, 1, 25.0});
      race::AiLineFile line;
      std::string err;
      CHECK(race::build_street_line(nav, route, line, &err));
      // Middle points must lie along the real road at x ~ 10, NOT the "none" outline at x = 0.
      bool stayed_on_road = false;
      for (const auto& pt : line.points)
        if (pt.z > 80.0f && pt.z < 120.0f && pt.x > 7.0f) stayed_on_road = true;
      CHECK(stayed_on_road);
    }

    // R2-4: Retail AI Auto-Upgrade scaling (1+0.20t / 1-0.167|t| friction, 1-0.286|t| down torque),
    // 10-iteration bisection, and EventParticipants.TuningLevel polish.
    {
      const auto up1 = race::ai_upgrade_scales(1.0);
      CHECK(near(up1.mass_scale, 0.9, 1e-6));
      CHECK(near(up1.torque_scale, 1.4, 1e-6));
      CHECK(near(up1.friction_scale, 1.2, 1e-6));
      const auto dn1 = race::ai_upgrade_scales(-1.0);
      CHECK(near(dn1.mass_scale, 1.0 / 0.9, 1e-4));
      CHECK(near(dn1.torque_scale, 1.0 / 1.4, 1e-4));    // ~0.7143 (1 - 0.2857)
      CHECK(near(dn1.friction_scale, 1.0 / 1.2, 1e-4));  // ~0.8333 (1 - 0.1667)

      drive::CarParams stock;
      stock.mass = 1500.0;
      stock.torque_scale = 400.0;
      stock.long_scale = {1.0, 1.0};
      stock.lat_scale = {1.0, 1.0};
      auto synthetic_pi = [&](const drive::CarParams& p) -> double {
        const double pwr_ratio = (p.torque_scale / stock.torque_scale) / (p.mass / stock.mass);
        const double grip_ratio = 0.5 * (p.lat_scale[0] + p.lat_scale[1]);
        return 450.0 + 180.0 * (pwr_ratio - 1.0) + 220.0 * (grip_ratio - 1.0);
      };
      const auto solved_up = race::solve_ai_upgrade(stock, 450.0, 520.0, synthetic_pi, 10);
      CHECK(solved_up.t > 0.0 && solved_up.t < 1.0);
      CHECK(near(solved_up.rated_pi, 520.0, 1.0));
      const auto solved_dn = race::solve_ai_upgrade(stock, 450.0, 405.0, synthetic_pi, 10);
      CHECK(solved_dn.t < 0.0 && solved_dn.t > -1.0);
      CHECK(near(solved_dn.rated_pi, 405.0, 1.0));

      drive::CarParams tuned = stock;
      race::apply_ai_upgrade(tuned, 0.5, 3);
      CHECK(near(tuned.mass, stock.mass * 0.95, 1e-6));
      CHECK(near(tuned.torque_scale, stock.torque_scale * 1.20, 1e-6));
      CHECK(near(tuned.lat_scale[0], 1.10, 1e-6));
      CHECK(tuned.spring_rate[0] > stock.spring_rate[0]);
      CHECK(tuned.bump_rate[0] > stock.bump_rate[0]);
      CHECK(tuned.gear_shift_time < stock.gear_shift_time);
    }

    // R2-4: AIPlayerColors lookup & fallback selection.
    {
      race::EventData ev;
      ev.ai_player_colors = {
          {1, 10, 0xCC1122u, true, true, 0},
          {2, 20, 0x1144AAu, true, false, 0},
      };
      race::Opponent a, b;
      a.ai_player_id = 20;
      b.ai_player_id = 99;
      const auto* ca = ev.ai_player_color(a);
      const auto* cb = ev.ai_player_color(b);
      CHECK(ca && ca->rgb == 0x1144AAu);
      CHECK(cb && cb->has_rgb);
    }

    // R2-4: Persistent AI CarLampState (15-tick brake hold, staggered headlamps, Reverse == -1 only).
    {
      drive::CarLampState ai_lamps;
      ai_lamps.rng_state = 0x12345678u;
      auto map_gear = [](int vgear) { return (vgear == -1) ? 0 : std::max(1, vgear); };
      drive::LampInputs in;
      in.is_ai = true;
      in.snap_headlamps = true;
      in.tod_sec = 12.0 * 3600.0;  // noon
      in.brake_cmd = 1.0f;
      in.brake_pedal = 1.0f;
      in.gear = map_gear(0);  // Neutral: reverse lamp must stay OFF!
      ai_lamps.update(in);
      CHECK(ai_lamps.brake_on && !ai_lamps.reverse_on && !ai_lamps.headlamps_on);
      // Release brake: persistent AI state holds brake lamp on for 15 ticks anti-flicker!
      in.brake_cmd = 0.0f;
      in.brake_pedal = 0.0f;
      in.snap_headlamps = false;
      ai_lamps.update(in);
      CHECK(ai_lamps.brake_on);
      for (int k = 0; k < 16; ++k) ai_lamps.update(in);
      CHECK(!ai_lamps.brake_on);
      // Reverse gear (-1 -> LampInputs::gear == 0): reverse lamp turns ON!
      in.gear = map_gear(-1);
      ai_lamps.update(in);
      CHECK(ai_lamps.reverse_on);
      // Dusk headlamps stagger on over ticks when snap_headlamps is false.
      in.tod_sec = 20.0 * 3600.0;
      bool turned_on = false;
      for (int k = 0; k < 200; ++k) {
        ai_lamps.update(in);
        if (ai_lamps.headlamps_on) { turned_on = true; break; }
      }
      CHECK(turned_on);
    }

    // The passing search on a road too narrow to pass: each failed search adds
    // 5 a 30 Hz frame to the frustration; past 1000 the car follows, trailing the car ahead.
    {
      drive::RacingLine rl;
      for (int i = 0; i <= 40; ++i) {
        rl.x.push_back(0.0f);
        rl.y.push_back(0.0f);
        rl.z.push_back(float(i * 10));
        rl.dist.push_back(float(i * 10));
        rl.radius.push_back(1e6f);
        rl.speed.push_back(60.0f);
        rl.room_left.push_back(0.5f);
        rl.room_right.push_back(0.5f);
      }
      race::DlsCar cars[2];
      cars[0].progress = 50, cars[0].speed = 22;
      cars[1].progress = 58, cars[1].speed = 20.5f;
      float lane[1] = {0.0f}, cap[1] = {1e9f};
      race::LaneParams lp[1] = {{7.0f, 0.3f, 0.4f, 0.5f}};
      race::DlsState st[1];
      const race::DlsSettings set;
      for (int s = 0; s < 400; ++s) race::dls_update(rl, cars, 1, lp, set, 1.0 / 60.0, st, lane, cap);
      CHECK(st[0].target == 1 && !st[0].following && near(st[0].frustration, 1000, 1e-3) && lane[0] == 0);
      race::dls_update(rl, cars, 1, lp, set, 1.0 / 60.0, st, lane, cap);
      CHECK(st[0].following);
      // Trailing at 0.75 x 7 m: 8 m apart less the 4.6 m of the two cars is inside it.
      CHECK(cap[0] < 20.5f + 0.1f);
    }

    // R2-4: Hand-of-God start-line launch grace (< 25 s && !has_launched), widened Street
    // off_route threshold (> 12 m), and 2 s fade-out / 1 s fade-in timer.
    {
      race::Gate start{0, 0, 0, 1, 20};
      race::Session::Options sopt;
      sopt.countdown = 0.0;
      sopt.resets = true;
      sopt.player_index = 0;
      sopt.off_route_near = 12.0;
      sopt.off_route_far = 25.0;
      race::Session sess(1, false, 1, start, race::Gate{0, 1000, 0, 1, 20}, sopt);
      race::CarSample cs{};
      // Sitting at the start line without launching for 10 s: NO premature reset!
      for (int k = 0; k < 100; ++k) {
        sess.step(0.1, std::span(&cs, 1));
        CHECK(sess.resets().empty());
      }
      CHECK(!sess.car(0).has_launched);
      // Launch the car (>= 5 mph), then drive 8 m off-route (inside widened 12 m Street threshold): NO reset.
      cs.speed = 10.0;
      cs.off_route = 8.0;
      for (int k = 0; k < 80; ++k) {
        sess.step(0.1, std::span(&cs, 1));
        CHECK(sess.resets().empty());
      }
      CHECK(sess.car(0).has_launched);
      // Now stop dead: after 2 s fade-out starts (< 1.0), at 3 s reset triggers, then 1 s fade-in recovers!
      cs.speed = 0.0;
      cs.off_route = 0.0;
      bool saw_fade_out = false, triggered = false;
      for (int k = 0; k < 35; ++k) {
        sess.step(0.1, std::span(&cs, 1));
        if (sess.reset_fade_alpha(0) < 0.9) saw_fade_out = true;
        if (!sess.resets().empty()) {
          triggered = true;
          sess.reset_done(0);
          break;
        }
      }
      CHECK(saw_fade_out && triggered);
      CHECK(sess.reset_fade_alpha(0) < 0.1);  // starts faded right after reset_done
      cs.speed = 15.0;
      for (int k = 0; k < 12; ++k) sess.step(0.1, std::span(&cs, 1));
      CHECK(near(sess.reset_fade_alpha(0), 1.0, 1e-6));  // fully faded back in after > 1 s
    }

    // R2-4: Rivals / GhostRun recording, interpolation, `.ghost` serialization,
    // personal-best clean-lap preference, and Rally Rivals loaner IDs (1626..1635).
    {
      const auto loaners = race::rally_rivals_loaner_ids();
      CHECK(loaners.size() == 10 && loaners.front() == 1626 && loaners.back() == 1635);
      CHECK(race::is_rally_rivals_loaner(1626) && race::is_rally_rivals_loaner(1635));
      CHECK(!race::is_rally_rivals_loaner(1625) && !race::is_rally_rivals_loaner(1636));

      race::GhostRun run;
      run.event_id = "FR22";
      run.driver_name = "RivalOne";
      run.car_media = "NIS_SkylineGTRVSpecII_02";
      run.car_id = 445;
      run.shown_pi = 500;
      run.total_time = 2.0;
      run.clean = true;
      run.sample_interval = 0.5;
      const double h90 = 0.5 * 3.14159265358979323846;
      run.record({0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 10.0, 0.0f, 0.0f, 1}, true);
      run.record({0.1, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 11.0, 0.0f, 0.0f, 1});  // throttled by 0.5 s cadence
      CHECK(run.samples.size() == 1);
      run.record({1.0, 10.0, 3.0, 20.0, 0.0, std::sin(h90 * 0.5), 0.0, std::cos(h90 * 0.5), 30.0, 0.2f, 0.5f, 3});
      CHECK(run.samples.size() == 2);

      race::GhostSample mid;
      CHECK(run.interpolate(0.5, mid));
      CHECK(near(mid.x, 5.0, 1e-4) && near(mid.y, 2.0, 1e-4) && near(mid.z, 10.0, 1e-4));
      CHECK(near(mid.speed, 20.0, 1e-4));
      const double qlen = std::sqrt(mid.qx * mid.qx + mid.qy * mid.qy + mid.qz * mid.qz + mid.qw * mid.qw);
      CHECK(near(qlen, 1.0, 1e-5));
      CHECK(near(mid.qy, std::sin(h90 * 0.25), 1e-4));

      // Save & load `.ghost` file round-trip + clean-lap personal-best preference.
      const auto tmp_dir = std::filesystem::temp_directory_path() / "shift_engine_ghost_test";
      std::error_code ec;
      std::filesystem::remove_all(tmp_dir, ec);
      bool saved = false;
      std::string gerr;
      race::GhostRun dirty_fast = run;
      dirty_fast.total_time = 1.5;
      dirty_fast.clean = false;
      CHECK(race::update_personal_best_ghost(tmp_dir, dirty_fast, &saved, &gerr) && saved);
      // A clean slower run must beat a dirty faster run!
      CHECK(race::update_personal_best_ghost(tmp_dir, run, &saved, &gerr) && saved);
      // Another dirty faster run must NOT overwrite the clean run!
      CHECK(race::update_personal_best_ghost(tmp_dir, dirty_fast, &saved, &gerr) && !saved);
      // Verify file round-trip content.
      race::GhostRun loaded;
      CHECK(race::GhostRun::load(race::ghost_file_path(tmp_dir, "FR22"), loaded, &gerr));
      CHECK(loaded.event_id == "FR22" && loaded.driver_name == "RivalOne" && loaded.car_id == 445 &&
            loaded.shown_pi == 500 && loaded.clean && near(loaded.total_time, 2.0, 1e-4) &&
            loaded.samples.size() == 2);
      std::filesystem::remove_all(tmp_dir, ec);
    }

    // R3-PHY-03: Pre-start countdown grid hold allows player throttle revving without moving
    // and without shifting Automatic transmission into Reverse, then launches cleanly on GO.
    {
      drive::CarParams cp{};
      const double r = 0.32;
      cp.wheel_centres = {drive::Vec3{-0.78, r, 1.30}, drive::Vec3{0.78, r, 1.30},
                          drive::Vec3{-0.79, r, -1.25}, drive::Vec3{0.79, r, -1.25}};
      cp.tyre_radius = {r, r, r, r};
      cp.centre_of_mass = {0, 0.5, 0.05};
      cp.has_turbo = true;
      cp.boost_max_psi = 14.0;
      drive::Vehicle car(cp);
      drive::FlatGround flat(0.0);
      car.reset({0.0, 0.0, 0.0}, 0.0);

      // Sitting off-throttle on grid (brake = 0.4, handbrake = true, clutch = 1.0): stays in 1st gear.
      for (int k = 0; k < 120; ++k) {
        drive::Input hold{};
        hold.brake = 0.4;
        hold.handbrake = true;
        hold.clutch = 1.0;
        car.step(hold, flat);
      }
      CHECK(car.gear() == 1);
      CHECK(std::fabs(car.position().z) < 0.15);

      // Player revs throttle (throttle = 0.85, brake = 1.0, handbrake = true, clutch = 1.0):
      // RPM climbs well above idle while car stays locked on the grid in 1st gear!
      const double idle_rpm = car.rpm();
      for (int k = 0; k < 120; ++k) {
        drive::Input rev{};
        rev.throttle = 0.85;
        rev.brake = 1.0;
        rev.handbrake = true;
        rev.clutch = 1.0;
        car.step(rev, flat);
      }
      CHECK(car.gear() == 1);
      CHECK(car.rpm() > idle_rpm + 1500.0);
      CHECK(std::fabs(car.position().z) < 0.20);

      // GO: release brake/handbrake/clutch with throttle held -> launches cleanly forward!
      for (int k = 0; k < 120; ++k) {
        drive::Input go{};
        go.throttle = 0.85;
        car.step(go, flat);
      }
      CHECK(car.position().z > 2.0 && car.speed() > 4.0);
    }

    // R3-PHY-14: parse_difficulty("insane"), SoloLapping_FarBehindTorqueBoost, and multi-class Session::class_place.
    {
      race::Difficulty parsed = race::Difficulty::kEasy;
      CHECK(race::parse_difficulty("insane", parsed) && parsed == race::Difficulty::kPro);
      CHECK(race::parse_difficulty("INSANE", parsed) && parsed == race::Difficulty::kPro);

      race::SkillRow sk{};
      sk.id = 10;
      sk.braking_min = 0.7;
      sk.braking_max = 0.8;
      sk.cornering_min = 0.7;
      sk.cornering_max = 0.8;
      sk.dist_behind_for_max = 100.0;
      sk.nominal_torque_scale = 1.0;
      sk.far_behind_torque_boost = 1.25;

      race::RubberbandRow rb{};
      rb.id = 1;
      rb.start_rubberbanding = 50.0;
      rb.start_catch_up = 80.0;
      rb.stop_catch_up = 40.0;
      rb.catch_up_factor = 0.5;

      // Rubber-banding ON: far behind applies far_behind_torque_boost; normal/ahead does not.
      race::DriverAi ai_on;
      ai_on.init(sk, &rb, 7, /*enabled=*/true);
      ai_on.update(-150.0, 2);
      CHECK(near(ai_on.torque(), 1.25, 1e-6));
      ai_on.update(-10.0, 0);
      CHECK(near(ai_on.torque(), 1.0, 1e-6));

      // Rubber-banding OFF: far_behind_torque_boost must stay disabled!
      race::DriverAi ai_off;
      ai_off.init(sk, &rb, 7, /*enabled=*/false);
      ai_off.update(-150.0, 2);
      CHECK(near(ai_off.torque(), 1.0, 1e-6));

      // Multi-class Session::class_place & class_order.
      race::Gate start{0, 0, 0, 1, 50};
      race::Session mc(4, false, 1, start, race::Gate{0, 500, 0, 1, 50}, {0.0, false});
      CHECK(!mc.is_multi_class());
      const int classes[4] = {10, 20, 10, 20};  // Class 10: cars 0, 2; Class 20: cars 1, 3
      mc.set_car_classes(classes);
      CHECK(mc.is_multi_class());
      // Interleaved progress: car 3 (C20, 400m) > car 2 (C10, 300m) > car 1 (C20, 200m) > car 0 (C10, 100m).
      race::CarSample cs[4]{};
      cs[0].progress = 100.0;
      cs[1].progress = 200.0;
      cs[2].progress = 300.0;
      cs[3].progress = 400.0;
      mc.step(0.1, cs);
      CHECK(mc.car(3).place == 1 && mc.class_place(3) == 1);
      CHECK(mc.car(2).place == 2 && mc.class_place(2) == 1);
      CHECK(mc.car(1).place == 3 && mc.class_place(1) == 2);
      CHECK(mc.car(0).place == 4 && mc.class_place(0) == 2);
      const auto ord10 = mc.class_order(10);
      const auto ord20 = mc.class_order(20);
      CHECK(ord10.size() == 2 && ord10[0] == 2 && ord10[1] == 0);
      CHECK(ord20.size() == 2 && ord20[0] == 3 && ord20[1] == 1);
      // Snapshot/restore preserves class assignments and class_place.
      const auto snap = mc.snapshot();
      cs[0].progress = 450.0;
      mc.step(0.1, cs);
      CHECK(mc.class_place(0) == 1);
      mc.restore(snap);
      CHECK(mc.car_class(0) == 10 && mc.class_place(0) == 2 && mc.class_place(2) == 1);
    }
  }
  if (const char* root = std::getenv("SHIFT_GAME_ROOT")) {
    disc(root);
    // Disc check: PlayerLevelDef (1000 levels), Rewards_DriverLevelv3, AffinityLevelDef (51 levels).
    race::ProgressionTables pt;
    std::string pe;
    CHECK(race::load_progression_tables(root, pt, &pe));
    std::printf("progression disc: %zu driver levels, %zu gift tiers, %zu affinity levels\n", pt.driver_levels.size(),
                pt.driver_level_cars.size(), pt.affinity_levels.size());
    CHECK(pt.driver_levels.size() == 1000 && pt.affinity_levels.size() == 51 && !pt.driver_level_cars.empty());
  } else std::printf("SHIFT_GAME_ROOT not set: disc checks skipped\n");
  std::printf(failures ? "race_tests: %d failure(s)\n" : "race_tests: ok\n", failures);
  return failures ? 1 : 0;
}
