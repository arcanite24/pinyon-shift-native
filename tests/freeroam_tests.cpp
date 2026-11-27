// Free-roam collectibles (SPEC-FREEROAM-COLLECTIBLES): the spec's synthetic
// cases for the map projection and the upgrade discount, the step rules on
// a small made-up set, and, with SHIFT_GAME_ROOT, the counts and
// placements read from the player's disc.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <algorithm>

#include "freeroam/collectibles.h"
#include "freeroam/festival_ai.h"
#include "freeroam/skills.h"
#include "freeroam/speed_zones.h"

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

const char* kFlyers = R"(<ActivityManager>
  <Activity type="ActivityFlyers" name="flyer_001" />
  <Activity type="ActivityFlyers" name="flyer_002" />
</ActivityManager>)";

const char* kBarns = R"(<ActivityManager>
  <!-- <Activity type="ActivityBarnFind" name="barnfind_template"><TriggerZone object="NOPE"/></Activity> -->
  <Activity type="ActivityBarnFindController" name="barnfind_controller" />
  <Activity type="ActivityBarnFind" name="barnfind_01">
    <HintRegion x_offset="-20" y_offset="30" angle="0" x_radius="400" y_radius="400" />
    <DoorInfo open_id="BF_A_OPEN" closed_id="BF_A_CLOSED" open_coll_id="BF_A_OPENC" closed_coll_id="BF_A_CLOSEDC" />
    <SpawnInfo weighting="100" min_probability="1" min_distance="0" max_probability="1" max_distance="100" />
    <TriggerZone object="BARNFIND_A" name="BARNFIND_A" radius="10" maxMPH="50" />
    <DistanceDriven distanceMin="5" distanceMax="10" threshold="10"/>
    <UnlockCar id="9001" stringTableNameId="IDS_Barnfind_Car_A"/>
  </Activity>
  <Activity type="ActivityBarnFind" name="barnfind_02">
    <HintRegion x_offset="0" y_offset="0" angle="0" x_radius="300" y_radius="300" />
    <DoorInfo open_id="BF_B_OPEN" closed_id="BF_B_CLOSED" open_coll_id="BF_B_OPENC" closed_coll_id="BF_B_CLOSEDC" />
    <SpawnInfo weighting="100" min_probability="10" min_distance="0" max_probability="1" max_distance="200" />
    <TriggerZone object="BARNFIND_B" name="BARNFIND_B" radius="10" maxMPH="50" />
    <DistanceDriven distanceMin="5" distanceMax="10" threshold="10"/>
    <UnlockCar id="9002" stringTableNameId="IDS_Barnfind_Car_B"/>
  </Activity>
</ActivityManager>)";

const char* kObjs = R"(<?xml version="1.0" ?>
<GameObjs>
  <Obj0 GameplayID="flyer_001"><Pos x="100" y="10" z="200"/></Obj0>
  <Obj1 GameplayID="flyer_002"><Pos x="-50" y="0" z="0"/></Obj1>
  <Obj2 GameplayID="BARNFIND_A"><Pos x="1000" y="5" z="1000"/></Obj2>
  <Obj3 GameplayID="BARNFIND_B"><Pos x="-1000" y="5" z="-1000"/></Obj3>
</GameObjs>)";

}  // namespace

int main() {
  using namespace freeroam;

  // The projection's corners and centre.
  auto uv = map_uv(-9583.3, 4600);
  CHECK(near(uv[0], 0) && near(uv[1], 0));
  uv = map_uv(9583.3, -6900);
  CHECK(near(uv[0], 1, 1e-5) && near(uv[1], 1));
  uv = map_uv(0, -1150);
  CHECK(near(uv[0], 0.5, 1e-5) && near(uv[1], 0.5));
  // The reference capture's player: u 0.72160, v 0.34120.
  uv = map_uv(4247.4, 676.2);
  CHECK(near(uv[0], 0.72160, 5e-5) && near(uv[1], 0.34120, 5e-5));

  // The discount.
  CHECK(discounted_price(10000, 85) == 1500);
  CHECK(discounted_price(10000, 0) == 10000);
  CHECK(discounted_price(10000, 100) == 0);
  CHECK(discounted_price(999, 1) == 989);
  CHECK(discounted_price(50, 1) == 50);
  CHECK(discounted_price(6200, 85) == 930);
  CHECK(upgrade_discount(85) == 85 && upgrade_discount(140) == 100);

  // The step rules on a made-up set.
  Collectibles data;
  std::string error;
  CHECK(parse_collectibles(kFlyers, kBarns, kObjs, data, &error));
  CHECK(data.signs.size() == 2 && data.barns.size() == 2);
  if (data.barns.size() == 2) {
    const Barn& a = data.barns[0];
    CHECK(a.name == "barnfind_01" && a.trigger == "barnfind_a" && a.car_id == 9001);
    CHECK(near(a.hint_x, 980) && near(a.hint_z, 1030) && near(a.hint_rx, 400));
    CHECK(a.open_id == "bf_a_open" && a.closed_coll_id == "bf_a_closedc");
  }
  Progress progress;
  FreeRoam fr(data, progress);
  CHECK(fr.hidden()->count("bf_a_open") && fr.hidden()->count("bf_a_openc") && !fr.hidden()->count("bf_a_closed"));
  CHECK(!fr.hidden()->count("flyer_001"));

  CarState car;
  car.x = 100, car.y = 10.5, car.z = 198.0;  // the sign 2 m ahead, inside the body's 2.2 m half length
  car.speed = 0.5;
  CHECK(fr.step(car).empty());  // too slow to break it
  car.speed = 10;
  auto ev = fr.step(car);
  CHECK(ev.size() == 1 && ev[0].kind == Event::kSignSmashed && ev[0].index == 0);
  CHECK(fr.signs_found() == 1 && fr.hidden()->count("flyer_001"));
  CHECK(fr.step(car).empty());  // once only
  car.x = 103;  // 3 m to the side: outside the 0.9 + 0.6 m half width
  car.z = 200;
  CHECK(fr.step(car).empty());

  // The barn controller (the title's rules, disc tuning).
  if (data.barns.size() == 2) {
    const Barn& a = data.barns[0];
    CHECK(a.spawn_weighting == 100 && near(a.spawn_max_distance_m, 160934.4, 0.01));
    CHECK(near(a.restore_min_m, 8046.72, 0.01) && near(a.restore_max_m, 16093.44, 0.01) &&
          near(a.restore_threshold_m, 16093.44, 0.01));
    // R = max + (min - max) * clamp(found / threshold): 10 mi found at 0, 5 mi from 10 mi on.
    CHECK(near(barn_restore_distance(a, 0), 16093.44, 0.01));
    CHECK(near(barn_restore_distance(a, 8046.72), 12070.08, 0.01));
    CHECK(near(barn_restore_distance(a, 50000), 8046.72, 0.01));
  }
  {
    // A scripted random source: the weighted pick, then the roll.
    std::vector<double> draws;
    size_t next = 0;
    fr.set_random([&] { return next < draws.size() ? draws[next++] : 0.0; });
    car.x = 5000, car.z = 5000, car.speed = 15, car.moved = 0;
    car.dt = 299;
    CHECK(fr.step(car).empty());  // no tick before 300 s
    // Tick: pick 0.1 * 200 -> barn 01 (1-in-1: certain) -> pending, no rumour yet.
    draws = {0.1, 0.9};
    car.dt = 1;
    CHECK(fr.step(car).empty());
    CHECK(progress.barns[0].status == kBarnPending && progress.barns[1].status == kBarnNotSpawned);
    // The rumour after 15 s above 20 mph (8.94 m/s); none on a slower car.
    // (The spawn step already ran the timer for its 1 s.)
    car.speed = 8, car.dt = 20;
    CHECK(fr.step(car).empty() && progress.barns[0].status == kBarnPending);
    car.speed = 15, car.dt = 13;
    CHECK(fr.step(car).empty());
    car.dt = 1;
    ev = fr.step(car);
    CHECK(ev.size() == 1 && ev[0].kind == Event::kRumourStarted && ev[0].index == 0);
    CHECK(progress.barns[0].status == kBarnRumour);
    // Blocked (a PR stunt running): no discovery inside the zone.
    car.dt = 0;
    car.x = 1005, car.z = 1000, car.speed = 20;
    CHECK(fr.step(car, BarnContext{true, true, false}).empty());
    // Discovery: within 10 m and below 50 mph (22.35 m/s) -> restoring, not in the garage.
    car.speed = 25;
    CHECK(fr.step(car).empty());
    car.speed = 20;
    const double found_odo = progress.total_distance;
    ev = fr.step(car);
    CHECK(ev.size() == 1 && ev[0].kind == Event::kBarnFound && ev[0].index == 0);
    CHECK(progress.barns[0].status == kBarnRestoring && progress.garage.empty());
    CHECK(near(progress.barns[0].found_at, std::trunc(found_odo)));
    CHECK(fr.barns_found() == 1);
    CHECK(fr.hidden()->count("bf_a_closed") && fr.hidden()->count("barnfind_a") && !fr.hidden()->count("bf_a_open"));
    CHECK(!fr.collect_barn(0));  // NOT READY
    // Ready after the restoration distance (10 mi: found below 1 km), above 25 mph.
    const double r = barn_restore_distance(data.barns[0], progress.barns[0].found_at);
    car.x = 5000, car.z = 5000, car.speed = 15;
    car.moved = r - 10;
    CHECK(fr.step(car).empty());
    car.moved = 20, car.speed = 10;  // far enough, but below 25 mph
    CHECK(fr.step(car).empty() && progress.barns[0].status == kBarnRestoring);
    car.moved = 0, car.speed = 12;
    ev = fr.step(car);
    CHECK(ev.size() == 1 && ev[0].kind == Event::kBarnReady && ev[0].index == 0);
    CHECK(progress.barns[0].status == kBarnReady && progress.garage.empty());
    // COLLECT CAR at the garage: status 2 and the car in the garage, once.
    CHECK(fr.collect_barn(0) && progress.barns[0].status == kBarnCollected);
    CHECK(progress.garage.size() == 1 && progress.garage[0] == 9001);
    CHECK(!fr.collect_barn(0) && fr.barns_found() == 1);

    // Barn 02: 1-in-10 at 0 mi; round(u * 9) == 0 is the hit.
    car.speed = 15, car.dt = 300, car.moved = 0;
    draws = {0.5, 0.2};  // the pick, then round(1.8) = 2: miss
    next = 0;
    CHECK(fr.step(car).empty() && progress.barns[1].status == kBarnNotSpawned);
    draws = {0.5, 0.05};  // round(0.45) = 0: hit
    next = 0;
    CHECK(fr.step(car).empty() && progress.barns[1].status == kBarnPending);
    // Its rumour waits for 30 mi past barn 01's rumour start.
    car.dt = 20;
    CHECK(fr.step(car).empty() && progress.barns[1].status == kBarnPending);
    car.dt = 0, car.moved = 48280.32;
    CHECK(fr.step(car).empty());
    car.dt = 15, car.moved = 0;
    ev = fr.step(car);
    CHECK(ev.size() == 1 && ev[0].kind == Event::kRumourStarted && ev[0].index == 1);
    // Out of free roam the pending timer resets; idle skips the tick.
    Progress p2;
    FreeRoam fr2(data, p2);
    fr2.set_random([] { return 0.0; });
    CarState idle;
    idle.x = 5000, idle.z = 5000, idle.dt = 700;
    CHECK(fr2.step(idle).empty() && p2.barns[0].status == kBarnNotSpawned);  // stationary 700 s: skipped
    idle.speed = 15, idle.dt = 299;
    CHECK(fr2.step(idle).empty() && p2.barns[0].status == kBarnNotSpawned);
    idle.dt = 1;
    CHECK(fr2.step(idle).empty() && p2.barns[0].status == kBarnPending);
    idle.dt = 10;
    CHECK(fr2.step(idle).empty());
    CHECK(fr2.step(idle, BarnContext{false, false, false}).empty());  // a race: the timer resets
    idle.dt = 10;
    CHECK(fr2.step(idle).empty() && p2.barns[0].status == kBarnPending);
    idle.dt = 5;
    CHECK(fr2.step(idle).size() == 1 && p2.barns[0].status == kBarnRumour);
  }

  // A sign that is not placed is an error.
  Collectibles bad;
  CHECK(!parse_collectibles(R"(<A><Activity type="ActivityFlyers" name="flyer_404"/></A>)", "<A/>", kObjs, bad,
                            &error));

  // Synthetic Speed Traps & Speed Zones.
  {
    const char* kCamXml = R"(<ActivityManager>
      <Activity type="ActivitySpeedCamera" name="speed_camera_01" scoreboard_id="201"><SpeedThreshold minimumSpeed="30.0"/></Activity>
    </ActivityManager>)";
    const char* kAvgXml = R"(<ActivityManager>
      <Activity type="ActivityAverageSpeed" name="average_speed_01" scoreboard_id="101"><SpeedThreshold minimumSpeed="30.0"/></Activity>
    </ActivityManager>)";
    const char* kSpeedObjs = R"(<GameObjs>
      <Obj1 GameplayID="speed_camera_01_left"><Pos x="-10" y="0" z="50"/></Obj1>
      <Obj2 GameplayID="speed_camera_01_right"><Pos x="10" y="0" z="50"/></Obj2>
      <Obj3 GameplayID="average_speed_01_left1"><Pos x="-10" y="0" z="100"/></Obj3>
      <Obj4 GameplayID="average_speed_01_right1"><Pos x="10" y="0" z="100"/></Obj4>
      <Obj5 GameplayID="average_speed_01_left2"><Pos x="-10" y="0" z="300"/></Obj5>
      <Obj6 GameplayID="average_speed_01_right2"><Pos x="10" y="0" z="300"/></Obj6>
    </GameObjs>)";
    SpeedActivitiesData sdata;
    CHECK(parse_speed_activities(kCamXml, kAvgXml, kSpeedObjs, sdata, &error));
    CHECK(sdata.traps.size() == 1 && sdata.zones.size() == 1);
    SpeedActivityTracker st(sdata);
    // Cross speed trap at 50 m/s (~111.8 mph).
    auto ev1 = st.step(0.1, 1.0, 0, 48, 0, 52, 50.0);
    CHECK(ev1.size() == 1 && ev1[0].kind == SpeedActivityEvent::kTrapTriggered && ev1[0].is_new_pb && ev1[0].flash);
    CHECK(st.best_trap_mph("speed_camera_01") > 110.0);
    CHECK(st.hud(1.05).flash_alpha > 0.3f && st.hud(1.05).banner_active);
    // Enter speed zone gate 1 at t=2.0, drive 200 m in 4.0 s (~111.8 mph), exit gate 2 at t=6.0.
    auto ev2 = st.step(0.1, 2.0, 0, 98, 0, 102, 50.0);
    CHECK(ev2.size() == 1 && ev2[0].kind == SpeedActivityEvent::kZoneEntered);
    CHECK(st.hud(2.5).zone_active);
    for (int k = 1; k <= 38; ++k) st.step(0.1, 2.0 + k * 0.1, 0, 102 + (k - 1) * 5.0, 0, 102 + k * 5.0, 50.0);
    auto ev3 = st.step(0.1, 6.0, 0, 297, 0, 303, 50.0);
    CHECK(ev3.size() == 1 && ev3[0].kind == SpeedActivityEvent::kZoneCompleted && ev3[0].is_new_pb);
    CHECK(st.best_zone_mph("average_speed_01") > 100.0);

    // Session hot-reload clock reset: must clear active banner/flash/abort/cooldowns
    // so an earlier speed trap never replays when vehicle.time() restarts from 0 and
    // later crosses the old timestamp, while preserving personal bests.
    const double saved_trap_pb = st.best_trap_mph("speed_camera_01");
    const double saved_zone_pb = st.best_zone_mph("average_speed_01");
    auto ev_late = st.step(0.1, 40.0, 0, 48, 0, 52, 55.0);
    CHECK(ev_late.size() == 1 && ev_late[0].kind == SpeedActivityEvent::kTrapTriggered);
    CHECK(st.hud(41.0).banner_active && st.hud(40.1).flash_alpha > 0.1f);
    const double updated_trap_pb = st.best_trap_mph("speed_camera_01");
    CHECK(updated_trap_pb > saved_trap_pb);
    // Enter a zone too, then hot-reload the session clock back to 0.
    auto ev_enter = st.step(0.1, 42.0, 0, 98, 0, 102, 50.0);
    CHECK(ev_enter.size() == 1 && ev_enter[0].kind == SpeedActivityEvent::kZoneEntered);
    CHECK(st.hud(42.5).zone_active);
    st.reset_session_clock();
    // Neither t=0.5 nor the old banner window t=40.5..41.0 may replay the stale toast.
    CHECK(!st.hud(0.5).banner_active && !st.hud(0.5).zone_active && st.hud(0.5).flash_alpha == 0.0f);
    CHECK(!st.hud(40.5).banner_active && !st.hud(41.0).banner_active && st.hud(40.1).flash_alpha == 0.0f);
    // Personal bests and completed counts persist across the session reset.
    CHECK(st.best_trap_mph("speed_camera_01") == updated_trap_pb);
    CHECK(st.best_zone_mph("average_speed_01") == saved_zone_pb);
    CHECK(st.traps_completed() == 1 && st.zones_completed() == 1);
    // Cooldowns were cleared, so crossing the trap at t=1.0 in the new session fires normally.
    auto ev_new = st.step(0.1, 1.0, 0, 48, 0, 52, 45.0);
    CHECK(ev_new.size() == 1 && ev_new[0].kind == SpeedActivityEvent::kTrapTriggered && !ev_new[0].is_new_pb);
  }

  // Synthetic 54-skill tracker: MinSpeedToCancelFeats (cancel vs bank), TagAwardInterval,
  // MaxSkillsCarOrdinal, TwoWheels/TradingPaint/Landscaping/SideSwipe, and SkillChallenge sponsor payouts.
  {
    const char* kFame = R"(<Fame><Level rank="249" target="1000"/><Level rank="225" target="5000"/></Fame>)";
    const char* kFeats = R"(<RaceFeats WorldCollisionThreshold="12" CarCollisionThreshold="12" MinSpeedToCancelFeats="10" DelayAfterCancelling="3" TagAwardInterval="0.3">
      <Air><Grade LocString="IDS_Air1" Time="0.2" Fame="100"/></Air>
      <TwoWheels MinSpeed="45"><Grade LocString="IDS_TwoWheels1" Time="0.2" Fame="100"/></TwoWheels>
      <TradingPaint MinPlayerSpeedMPH="50" MaxDistance="3.2"><Grade LocString="IDS_TradingPaint" SpeedMPH="1" Fame="50"/></TradingPaint>
      <NearMiss MinPlayerSpeedMPH="50" MaxDistance="4.0"><Grade LocString="IDS_NearMiss1" SpeedMPH="50" Fame="200"/></NearMiss>
      <Destruction MaxTimeBetweenCollisions="4.0"><Grade LocString="IDS_Destruction1" NumChainedSmashables="1" Fame="100"/></Destruction>
      <Landscaping RetriggerDelay="1.0" objectType="Fence"><Grade LocString="IDS_Landscaping" Threshold="0" Fame="1000"/></Landscaping>
    </RaceFeats>)";
    const char* kSpon = R"(<SkillChallenges>
      <Challenge id="Fame" type="FameRankChallenge"><Rank levelRequired="225" credits="5000"/></Challenge>
      <Challenge id="Air" type="SkillChallenge"><Rank skill="Air" grade="4" number="2" credits="5000"/></Challenge>
      <Challenge id="AnySkill" type="SkillChallenge"><Rank skill="AnySkill" grade="0" number="1" credits="5000"/></Challenge>
    </SkillChallenges>)";
    SkillsData sd;
    CHECK(parse_skills_data(kFame, kFeats, kSpon, sd, &error));
    CHECK(sd.sponsor_challenges.size() == 3);
    SkillTracker tr(sd, 0.0, 0, 0, 0);
    // 1. Trigger Landscaping + Destruction -> TagAwardInterval queues second tag for 0.3 s later!
    SkillInput in;
    in.dt = 0.1;
    in.time = 1.0;
    in.speed_ms = 30.0;
    in.forward_speed_ms = 30.0;
    in.smashed_object = true;
    in.smashed_type = SmashObjectType::kFence;
    in.car_id = 1219;
    auto e1 = tr.step(in);
    CHECK(e1.size() == 1 && e1[0].kind == SkillEvent::kSkillAwarded);
    in.smashed_object = false;
    in.time = 1.4;  // >= 0.3 s later: queued Landscaping tag flushes (sponsor progress deferred until bank!)
    auto e2 = tr.step(in);
    bool got_landscaping = false, got_sponsor_unbanked = false;
    for (const auto& ev : e2) {
      if (ev.skill_id == "Landscaping") got_landscaping = true;
      if (ev.kind == SkillEvent::kSponsorChallenge) got_sponsor_unbanked = true;
    }
    CHECK(got_landscaping);
    CHECK(!got_sponsor_unbanked);
    // 2. Bank chain after timeout at speed -> commits deferred AnySkill sponsor tier & updates MaxSkillsCarOrdinal!
    in.time = 6.0;  // >= 4.5 s bank_timeout after t=1.4
    auto e3 = tr.step(in);
    bool got_banked = false, got_sponsor_banked = false;
    for (const auto& ev : e3) {
      if (ev.kind == SkillEvent::kChainBanked) got_banked = true;
      if (ev.kind == SkillEvent::kSponsorChallenge && ev.credits == 5000) got_sponsor_banked = true;
    }
    CHECK(got_banked);
    CHECK(got_sponsor_banked);
    CHECK(tr.max_skills_score() >= 1100 && tr.max_skills_car() == 1219);
    // 3. R3-CAM-05: Braking to a stop (< 10 mph for > 1 s) must NOT destroy the unbanked Skill Chain;
    //    the chain lets bank_timeout expire and banks cleanly!
    in.time = 7.0;
    in.car_impact_ms = 2.0;  // Trading Paint (below 12 m/s crash threshold)
    in.nearest_car_dist = 2.0;
    auto e4 = tr.step(in);
    CHECK(!e4.empty() && e4[0].skill_id == "TradingPaint");
    in.car_impact_ms = 0.0;
    in.nearest_car_dist = 100.0;
    in.speed_ms = 2.0;  // ~4.5 mph (< 10 mph)
    in.forward_speed_ms = 2.0;
    in.time = 8.5;
    auto e5 = tr.step(in);
    bool lost_on_brake = false;
    for (const auto& ev : e5)
      if (ev.kind == SkillEvent::kChainLost) lost_on_brake = true;
    CHECK(!lost_on_brake && tr.hud().active);
    in.time = 11.6;  // >= 4.5 s bank_timeout after t=7.0 -> banks cleanly while stopped!
    auto e6 = tr.step(in);
    bool banked_on_stop = false;
    for (const auto& ev : e6)
      if (ev.kind == SkillEvent::kChainBanked) banked_on_stop = true;
    CHECK(banked_on_stop && !tr.hud().active);
    // Hard collision (>= 12 m/s) still cancels an active chain!
    in.speed_ms = 30.0;
    in.forward_speed_ms = 30.0;
    in.time = 12.5;
    in.smashed_object = true;
    in.smashed_type = SmashObjectType::kGeneric;
    CHECK(!tr.step(in).empty());
    in.smashed_object = false;
    in.time = 13.0;
    in.world_impact_ms = 15.0;
    auto e7 = tr.step(in);
    CHECK(!e7.empty() && e7[0].kind == SkillEvent::kChainLost);
  }

  // Synthetic Festival AI & Head-to-Head point-to-point race.
  {
    traffic::NavMap nm;
    nm.keys = {"road_type", "traffic_density"};
    nm.values = {"a", "5"};
    for (uint32_t i = 0; i < 30; ++i) {
      traffic::NavNode nd;
      nd.id = i;
      nd.z = float(i * 100);
      nm.nodes.push_back(nd);
      nm.way_nodes.push_back(i);
    }
    nm.tags = {{0, 0}, {1, 1}};
    traffic::NavWay w;
    w.node_count = 30;
    w.tag_count = 2;
    nm.ways.push_back(w);
    traffic::Roads roads;
    roads.build(nm);
    traffic::TrafficConfig cfg;
    FestivalManager fm(nm, roads, cfg, 7);
    fm.set_driver_names({"Darius Flynt", "Ramona Cravache"});
    fm.set_roster({{1219, "NIS_SkylineGTRVSpecII_02", "", "A", 540, 1560.0}});
    fm.set_player_pi(500);
    const uint32_t fid = fm.spawn_ahead({0, 0, 100}, 0.0, 14.0);
    CHECK(fid > 0);
    // Match AI speed (~39.5 m/s) inside retail activation box (5..20 m ahead, |lat| <= 2.5 m).
    fm.step(0.05, 1.0, {2.2, 0, 100}, 0.0, 38.0);
    CHECK(fm.hud(1.0).prompt_visible && fm.hud(1.0).prompt_driver == "Darius Flynt" &&
          fm.hud(1.0).prompt_wager == h2h_reward_credits(500, 540));
    // Verify Festival AI velocity/RPM telemetry & player collision impulse.
    CHECK(!fm.cars().empty() && fm.cars()[0].rpm > 900.0);
    const auto fpos = fm.cars()[0].position;
    const auto col = fm.resolve_player_collision({fpos.x + 0.8, fpos.y, fpos.z - 0.5}, {-12.0, 0.0, 20.0}, 1500.0, 1.0 / 120.0);
    CHECK(col.hit && col.impulse_magnitude > 100.0);
    CHECK(fm.start_head_to_head({2.2, 0, 100}, 0.0, 1.0, fid));
    CHECK(fm.h2h_active() && fm.h2h_route().size() >= 10);
    const auto fin = fm.h2h_finish_pos();
    HeadToHeadResult hres;
    // Outside 10 m finish radius (EndPointTriggerDiameter = 20 m): must NOT finish yet.
    CHECK(!fm.step(0.05, 4.9, {fin[0], fin[1], fin[2] - 15.0}, 0.0, 40.0, &hres));
    // Inside 10 m finish radius: completes and pays PI-differential reward.
    CHECK(fm.step(0.05, 5.0, {fin[0], fin[1], fin[2] - 6.0}, 0.0, 40.0, &hres));
    CHECK(hres.won && hres.payout_credits == h2h_reward_credits(500, 540));
  }

  // R2-4 parity: Retail H2H PI-differential reward, trigger box gates + hysteresis,
  // one-way freeway routing (2500-5000 m), and uncapped hypercar top speed.
  {
    // 1) PI-differential payout: base 5000, +20/PI vs higher opponent, -10/PI vs lower opponent.
    CHECK(h2h_reward_credits(500, 500) == 5000);
    CHECK(h2h_reward_credits(500, 650) == 8000);
    CHECK(h2h_reward_credits(650, 500) == 3500);

    // 2) Trigger box gates: distance [5,20], |lat|<=2.5, heading_dot>0.85, |dv|<=6.71, target>=11.18.
    traffic::NavMap nm;
    nm.keys = {"road_type", "traffic_density", "oneway"};
    nm.values = {"freeway", "5", "true"};
    // Two parallel one-way freeway carriageways (4000 m long):
    //   x=0: northbound z=0 -> 4000 (oneway=true)
    //   x=30: southbound z=4000 -> 0 (oneway=true)
    // Connected at z=0 and z=4000.
    for (uint32_t i = 0; i <= 40; ++i) {
      traffic::NavNode nd;
      nd.id = i;
      nd.x = 0.0f;
      nd.z = float(i * 100);
      nm.nodes.push_back(nd);
    }
    for (uint32_t i = 0; i <= 40; ++i) {
      traffic::NavNode nd;
      nd.id = 41 + i;
      nd.x = 30.0f;
      nd.z = float((40 - i) * 100);
      nm.nodes.push_back(nd);
    }
    nm.tags = {{0, 0}, {1, 1}, {2, 2}};  // freeway, density 5, oneway=true
    traffic::NavWay w_nb;
    w_nb.node_first = 0;
    w_nb.node_count = 41;
    w_nb.tag_first = 0;
    w_nb.tag_count = 3;
    for (uint32_t i = 0; i <= 40; ++i) nm.way_nodes.push_back(i);
    traffic::NavWay w_sb;
    w_sb.node_first = uint32_t(nm.way_nodes.size());
    w_sb.node_count = 41;
    w_sb.tag_first = 0;
    w_sb.tag_count = 3;
    for (uint32_t i = 0; i <= 40; ++i) nm.way_nodes.push_back(41 + i);
    nm.ways = {w_nb, w_sb};
    traffic::Roads roads;
    roads.build(nm);
    traffic::TrafficConfig cfg;
    cfg.initial_festival_cars = 0;
    cfg.min_festival = 1;
    cfg.max_festival = 1;

    {
      // WORLD-08: the festival population target follows the local density
      // (SPEC-TRAFFIC, Population target): min(D_s, cap(h)), D_s the festival
      // list's d per 100 m of road within the spawn-out distance.
      traffic::TrafficConfig dc = cfg;
      dc.min_festival = 2, dc.max_festival = 7;
      traffic::DensityEntry e;
      e.id = 5, e.min = 0.1, e.max = 0.3;
      dc.festival[5] = e;
      FestivalManager fd(nm, roads, dc, 7);
      fd.set_roster({{1006, "FER_FXX_05", "Hyper Rival", "R1", 950, 1155.0}});
      fd.set_hour(12.0);
      fd.step(0.01, 1.0, {0, 0, 2000}, 0.0, 20.0);
      const double v = traffic::time_factor(12.0, dc);
      double want = 0;
      for (const auto& [id, m] : roads.traffic_length_within(0, 2000, dc.spawn_out))
        if (id == 5) want += traffic::density_at(e, v) * m / 100;
      std::printf("festival density at noon: D_s %.2f, target %.2f (cap %.1f)\n", fd.local_density(), fd.target(),
                  2 + 5 * v);
      CHECK(want > 0.5 && std::fabs(fd.local_density() - want) < 1e-9);
      CHECK(std::fabs(fd.target() - std::min(want, 2 + 5 * v)) < 1e-9);
      // Way ids the festival list lacks have no festival cars.
      traffic::TrafficConfig none = dc;
      none.festival.clear();
      none.festival[9] = e;
      FestivalManager fn(nm, roads, none, 7);
      fn.set_roster({{1006, "FER_FXX_05", "Hyper Rival", "R1", 950, 1155.0}});
      fn.step(0.01, 1.0, {0, 0, 2000}, 0.0, 20.0);
      CHECK(fn.target() == 0);
    }
    FestivalManager fm(nm, roads, cfg, 99);
    fm.set_player_pi(500);
    fm.set_roster({{1006, "FER_FXX_05", "Hyper Rival", "R1", 950, 1155.0}});
    const uint32_t cid = fm.spawn_ahead({0, 0, 200}, 0.0, 12.0);
    CHECK(cid > 0 && fm.cars().size() == 1);
    const double ai_spd = fm.cars()[0].speed;
    // Wrong heading (facing south, pi rad): must NOT trigger.
    fm.step(0.01, 1.0, {fm.cars()[0].position.x, 0, fm.cars()[0].position.z - 12.0}, 3.14159, ai_spd);
    CHECK(!fm.hud(1.0).prompt_visible);
    // Lateral offset > 2.5 m (4.0 m to the side): must NOT trigger.
    fm.step(0.01, 1.01, {fm.cars()[0].position.x - 4.0, 0, fm.cars()[0].position.z - 12.0}, 0.0, ai_spd);
    CHECK(!fm.hud(1.01).prompt_visible);
    // Speed difference > 6.71 m/s (15 mph): must NOT trigger.
    fm.step(0.01, 1.02, {fm.cars()[0].position.x, 0, fm.cars()[0].position.z - 12.0}, 0.0, ai_spd - 12.0);
    CHECK(!fm.hud(1.02).prompt_visible);
    // Too close (< 5 m): must NOT trigger.
    fm.step(0.01, 1.03, {fm.cars()[0].position.x, 0, fm.cars()[0].position.z - 3.0}, 0.0, ai_spd);
    CHECK(!fm.hud(1.03).prompt_visible);
    // Too far (> 20 m): must NOT trigger.
    fm.step(0.01, 1.04, {fm.cars()[0].position.x, 0, fm.cars()[0].position.z - 23.0}, 0.0, ai_spd);
    CHECK(!fm.hud(1.04).prompt_visible);
    // Inside retail box (12 m ahead, aligned, matched speed): MUST trigger!
    fm.step(0.01, 1.05, {fm.cars()[0].position.x, 0, fm.cars()[0].position.z - 12.0}, 0.0, ai_spd);
    CHECK(fm.hud(1.05).prompt_visible);
    // Hysteresis once locked: holds briefly at 22.5 m, releases at 28 m.
    fm.step(0.01, 1.06, {fm.cars()[0].position.x, 0, fm.cars()[0].position.z - 22.5}, 0.0, ai_spd);
    CHECK(fm.hud(1.06).prompt_visible);
    fm.step(0.01, 1.07, {fm.cars()[0].position.x, 0, fm.cars()[0].position.z - 28.0}, 0.0, ai_spd);
    CHECK(!fm.hud(1.07).prompt_visible);

    // 3) One-way freeway H2H routing: starting on northbound freeway must route north (2500-5000 m,
    //    never backwards down the one-way carriageway) and hypercar accelerates past old 58 m/s cap!
    const traffic::P3 start_p = {2.2, 0, 200};
    CHECK(fm.start_head_to_head(start_p, 0.0, 2.0, cid));
    const auto& r2d = fm.h2h_route();
    CHECK(r2d.size() >= 25);
    const double route_len = fm.hud(2.0).route_length_m;
    CHECK(route_len >= 2500.0 && route_len <= 5000.0);
    // Every consecutive point along the one-way northbound freeway progresses +Z.
    bool strictly_legal_north = true;
    for (size_t i = 1; i < r2d.size(); ++i) {
      if (r2d[i][1] < r2d[i - 1][1] - 1.0) strictly_legal_north = false;
    }
    CHECK(strictly_legal_north);
    // Step H2H for 6 seconds: PI 950 lightweight hypercar accelerates past old 58 m/s cap!
    for (int k = 1; k <= 120; ++k) {
      fm.step(0.05, 2.0 + k * 0.05, {2.2, 0, 200 + k * 2.5}, 0.0, 50.0);
    }
    CHECK(!fm.cars().empty() && fm.cars()[0].speed > 65.0);
  }

  // Festival AI smoothness across a 90-degree junction: bounded yaw rate & lateral accel,
  // no instant heading snaps or rail teleports, and braking before the sharp turn.
  {
    traffic::NavMap nm;
    nm.keys = {"road_type", "traffic_density"};
    nm.values = {"a", "5"};
    // 90-degree corner: (0,0) -> (0,120) -> (120,120)
    const float pts[7][2] = {{0, 0}, {0, 40}, {0, 80}, {0, 120}, {40, 120}, {80, 120}, {120, 120}};
    for (uint32_t i = 0; i < 7; ++i) {
      traffic::NavNode nd;
      nd.id = i;
      nd.x = pts[i][0];
      nd.z = pts[i][1];
      nm.nodes.push_back(nd);
    }
    nm.way_nodes = {0, 1, 2, 3, 3, 4, 5, 6};
    nm.tags = {{0, 0}, {1, 1}, {0, 0}, {1, 1}};
    traffic::NavWay w0, w1;
    w0.node_first = 0, w0.node_count = 4, w0.tag_first = 0, w0.tag_count = 2;
    w1.node_first = 4, w1.node_count = 4, w1.tag_first = 2, w1.tag_count = 2;
    nm.ways = {w0, w1};
    traffic::Roads roads;
    roads.build(nm);
    traffic::TrafficConfig cfg;
    cfg.initial_festival_cars = 0;
    cfg.min_festival = 1;
    cfg.max_festival = 1;
    FestivalManager fm(nm, roads, cfg, 17);
    fm.set_roster({{1219, "NIS_SkylineGTRVSpecII_02", "Smooth Driver", "A", 540}});
    const uint32_t cid = fm.spawn_ahead({0, 0, 10}, 0.0, 20.0);
    CHECK(cid > 0 && fm.cars().size() == 1);
    const double dt = 1.0 / 60.0;
    double prev_h = fm.cars()[0].heading;
    traffic::P3 prev_p = fm.cars()[0].position;
    double max_yaw_rate = 0.0, max_lat_acc = 0.0, max_step_jump = 0.0;
    double min_speed = 1e9, init_speed = fm.cars()[0].speed;
    bool saw_braking = false, rounded_corner = false;
    for (int k = 1; k <= 600; ++k) {
      // Keep the player well off the road so the AI car has an unobstructed corner.
      fm.step(dt, k * dt, {60, 0, 60}, 0.0, 0.0);
      if (fm.cars().empty()) break;
      const auto& c = fm.cars()[0];
      double dh = c.heading - prev_h;
      while (dh > 3.14159265358979323846) dh -= 2.0 * 3.14159265358979323846;
      while (dh < -3.14159265358979323846) dh += 2.0 * 3.14159265358979323846;
      const double yr = std::fabs(dh) / dt;
      max_yaw_rate = std::max(max_yaw_rate, yr);
      max_lat_acc = std::max(max_lat_acc, yr * c.speed);
      max_step_jump = std::max(max_step_jump, std::hypot(c.position.x - prev_p.x, c.position.z - prev_p.z));
      min_speed = std::min(min_speed, c.speed);
      saw_braking |= c.braking;
      if (c.position.x > 30.0 && std::fabs(c.position.z - 117.8) < 4.0) rounded_corner = true;
      prev_h = c.heading;
      prev_p = c.position;
    }
    std::printf("festival corner: init %.1f -> min %.1f m/s, max yaw %.2f rad/s, max lat %.1f m/s^2, jump %.2f m, "
                "braked %d, rounded %d\n",
                init_speed, min_speed, max_yaw_rate, max_lat_acc, max_step_jump, int(saw_braking), int(rounded_corner));
    CHECK(max_yaw_rate < 2.2);
    CHECK(max_lat_acc < 16.0);
    CHECK(max_step_jump < 1.0);
    CHECK(saw_braking && min_speed < init_speed - 8.0);
    CHECK(rounded_corner);
  }

  // Festival AI inter-car separation: two Festival cars on the same road must never fuse/overlap.
  {
    traffic::NavMap nm;
    nm.keys = {"road_type", "traffic_density"};
    nm.values = {"a", "5"};
    for (uint32_t i = 0; i < 20; ++i) {
      traffic::NavNode nd;
      nd.id = i;
      nd.z = float(i * 40);
      nm.nodes.push_back(nd);
      nm.way_nodes.push_back(i);
    }
    nm.tags = {{0, 0}, {1, 1}};
    traffic::NavWay w;
    w.node_count = 20;
    w.tag_count = 2;
    nm.ways.push_back(w);
    traffic::Roads roads;
    roads.build(nm);
    traffic::TrafficConfig cfg;
    cfg.initial_festival_cars = 0;
    cfg.min_festival = 2;
    cfg.max_festival = 2;
    FestivalManager fm(nm, roads, cfg, 23);
    fm.set_roster({{1001, "CAR_SLOW", "Terry Adams", "D", 247}, {1002, "CAR_FAST", "Michael Allen", "B", 439}});
    // Spawn two cars 12 m apart on the same straight road, trailing car faster than lead car.
    const uint32_t id1 = fm.spawn_ahead({0, 0, 50}, 0.0, 26.0);
    const uint32_t id2 = fm.spawn_ahead({0, 0, 50}, 0.0, 14.0);
    CHECK(id1 > 0 && id2 > 0 && fm.cars().size() == 2);
    double min_dist = 1e9;
    const double dt = 1.0 / 60.0;
    for (int k = 1; k <= 360; ++k) {
      fm.step(dt, k * dt, {0, 0, 50 + k * dt * 20.0}, 0.0, 20.0);
      if (fm.cars().size() < 2) break;
      const auto& a = fm.cars()[0];
      const auto& b = fm.cars()[1];
      min_dist = std::min(min_dist, std::hypot(a.position.x - b.position.x, a.position.z - b.position.z));
    }
    std::printf("festival separation: min center dist %.2f m\n", min_dist);
    CHECK(min_dist >= 2.1);
  }

  // R3-PHY-11: Festival AI collision spin/yaw response, suspension roll/pitch reaction,
  // off-road recovery steering, and 3.0 s post-crash ghosting un-clip + cruise resume.
  {
    traffic::NavMap nm;
    nm.keys = {"road_type", "traffic_density"};
    nm.values = {"a", "5"};
    for (uint32_t i = 0; i < 40; ++i) {
      traffic::NavNode nd;
      nd.id = i;
      nd.z = float(i * 40);
      nm.nodes.push_back(nd);
      nm.way_nodes.push_back(i);
    }
    nm.tags = {{0, 0}, {1, 1}};
    traffic::NavWay w;
    w.node_count = 40;
    w.tag_count = 2;
    nm.ways.push_back(w);
    traffic::Roads roads;
    roads.build(nm);
    traffic::TrafficConfig cfg;
    cfg.initial_festival_cars = 0;
    cfg.min_festival = 1;
    cfg.max_festival = 1;
    FestivalManager fm(nm, roads, cfg, 37);
    fm.set_roster({{1219, "NIS_SkylineGTRVSpecII_02", "Crash Rival", "A", 540, 1480.0}});
    const uint32_t fid = fm.spawn_ahead({0, 0, 50}, 0.0, 20.0);
    CHECK(fid > 0 && fm.cars().size() == 1);
    const double dt = 1.0 / 60.0;
    fm.step(dt, dt, {40.0, 0.0, 70.0}, 0.0, 0.0);

    // High-speed off-center corner impact imparts realistic spin, roll, off-road recovery, and 3.0 s ghosting.
    const auto p0 = fm.cars()[0].position;
    const auto col1 = fm.resolve_player_collision({p0.x + 1.0, p0.y, p0.z + 1.1}, {-22.0, 0.0, 15.0}, 1500.0, dt);
    CHECK(col1.hit && col1.impulse_magnitude > 5000.0);
    CHECK(std::fabs(fm.cars()[0].spin_yaw_rate) > 1.0);
    CHECK(std::fabs(fm.cars()[0].yaw_rate) > 1.0);
    CHECK(std::fabs(fm.cars()[0].roll) > 0.05);
    CHECK(fm.cars()[0].off_road_recovery > 0.5);
    CHECK(fm.cars()[0].ghosted() && near(fm.cars()[0].ghost_timer, FestivalManager::kCollisionGhostDuration, 0.05));

    // While ghosted, overlapping player contact is suppressed so wedged cars cleanly un-clip.
    const auto p1 = fm.cars()[0].position;
    const auto col_ghost = fm.resolve_player_collision({p1.x + 0.4, p1.y, p1.z}, {-10.0, 0.0, 15.0}, 1500.0, dt);
    CHECK(!col_ghost.hit);

    // Step for 3.5 s: ghosting expires, spin damps out, car steers smoothly back to lane and resumes cruising.
    for (int k = 2; k <= 212; ++k) {
      const double pz = fm.cars().empty() ? 100.0 : fm.cars()[0].position.z;
      fm.step(dt, k * dt, {45.0, 0.0, pz}, 0.0, 0.0);
    }
    CHECK(!fm.cars().empty());
    if (!fm.cars().empty()) {
      const auto& c = fm.cars()[0];
      std::printf("festival ghost recovery: ghost=%.2f spin=%.3f lat=%.2f spd=%.1f\n", c.ghost_timer, c.spin_yaw_rate,
                  c.lateral, c.speed);
      CHECK(!c.ghosted());
      CHECK(std::fabs(c.spin_yaw_rate) < 0.05);
      CHECK(std::fabs(c.lateral - 2.2) < 1.0);
      CHECK(c.speed > 12.0);
    }
  }

  // The player's disc.
  if (const char* root = std::getenv("SHIFT_GAME_ROOT")) {
    Collectibles disc;
    error.clear();
    CHECK(load_collectibles(root, disc, &error));
    if (!error.empty()) std::printf("%s\n", error.c_str());
    std::printf("disc: %zu signs, %zu barn finds\n", disc.signs.size(), disc.barns.size());
    CHECK(disc.signs.size() == 100 && disc.barns.size() == 9);
    // Disc check: 54/54 HorizonFeats & 14/14 SponsorshipChallenges.
    SkillsData disc_skills;
    CHECK(load_skills_data(root, disc_skills, &error));
    std::printf("disc skills: %zu defs, %zu sponsor challenges\n", disc_skills.skill_def_count(),
                disc_skills.sponsor_challenges.size());
    CHECK(disc_skills.skill_def_count() == 54);
    CHECK(disc_skills.sponsor_challenges.size() == 15);
    CHECK(near(disc_skills.tag_award_interval, 0.3, 0.01));
    // Disc check: Speed Traps & Speed Zones.
    SpeedActivitiesData disc_speed;
    CHECK(load_speed_activities(root, disc_speed, &error));
    std::printf("disc speed activities: %zu traps, %zu zones\n", disc_speed.traps.size(), disc_speed.zones.size());
    CHECK(disc_speed.traps.size() == 22 && disc_speed.zones.size() == 9);
    if (!disc.signs.empty()) {
      const auto& s = disc.signs[0];
      CHECK(s.id == "flyer_001" && near(s.at.x, 1410.35, 0.01) && near(s.at.z, 3126.53, 0.01));
    }
    if (disc.barns.size() == 9) {
      const int cars[9] = {1219, 1040, 1277, 1105, 1386, 1154, 1301, 251, 1155};
      std::set<std::string> doors;
      for (int i = 0; i < 9; ++i) {
        const Barn& b = disc.barns[size_t(i)];
        CHECK(b.car_id == cars[i] && near(b.radius, 10) && near(b.max_mph, 50));
        CHECK(b.hint_rx >= 286 && b.hint_rx <= 600 && near(b.hint_rx, b.hint_rz));
        doors.insert({b.open_id, b.closed_id, b.open_coll_id, b.closed_coll_id});
      }
      CHECK(doors.size() == 36);
      CHECK(disc.barns[0].trigger == "barnfind_bug_eb110bf_92" && near(disc.barns[0].at.x, 1423.378, 0.01));
      // The rumour reveal: each barn's map camera animation and VO line.
      const char* vo[9] = {"1215", "1040", "1277", "1105", "1386", "1154", "1301", "251", "1155"};
      for (int i = 0; i < 9; ++i) {
        const Barn& b = disc.barns[size_t(i)];
        CHECK(b.map_animation == "barn_find_region" + std::to_string(i + 1) + "_unlock");
        CHECK(b.map_vo == std::string("BarnFind") + vo[i] + "AppearsOnMap");
      }
      // SpawnInfo (1-in-N odds over miles of odometer) and DistanceDriven 5/10/10 mi.
      const double min_p[9] = {10, 5, 5, 25, 200, 250, 100, 10, 150};
      const double max_mi[9] = {200, 100, 100, 250, 1500, 2250, 500, 200, 1000};
      for (int i = 0; i < 9; ++i) {
        const Barn& b = disc.barns[size_t(i)];
        CHECK(b.spawn_weighting == 100 && near(b.spawn_min_probability, min_p[i]) && near(b.spawn_max_probability, 1));
        CHECK(near(b.spawn_min_distance_m, 0) && near(b.spawn_max_distance_m, max_mi[i] * 1609.344, 0.01));
        CHECK(near(b.restore_min_m, 8046.72, 0.01) && near(b.restore_max_m, 16093.44, 0.01) &&
              near(b.restore_threshold_m, 16093.44, 0.01));
      }
    }
  } else {
    std::printf("SHIFT_GAME_ROOT not set: the disc checks skipped\n");
  }

  if (failures) return 1;
  // P0-4: SkillTracker rewind snapshot preserves live chain score, multiplier & timer across crash/rewind.
  {
    SkillsData sd;
    sd.bank_timeout = 3.0;
    sd.tag_award_interval = 0.0;
    SkillTracker st(sd, 0.0, 0);
    st.award_named("Drift", 2, 10.0);
    st.award_named("NearMiss", 2, 10.5);
    st.award_named("Pass", 2, 11.0);
    const auto h_before = st.hud();
    CHECK(h_before.active && h_before.chain_points > 0 && h_before.multiplier_f > 1.15f);
    CHECK(st.chain_remaining_s(11.5) > 2.0);
    const auto snap = st.capture_rewind();
    // Simulate wreck cancelling chain.
    st.clear_active_chain();
    CHECK(!st.hud().active && st.hud().chain_points == 0);
    // Rewind restores the intact skill chain!
    st.restore_rewind(snap);
    const auto h_after = st.hud();
    CHECK(h_after.active && h_after.chain_points == h_before.chain_points && h_after.multiplier_f == h_before.multiplier_f);
    CHECK(st.chain_remaining_s(11.5) > 2.0);
  }
  // Phase 1 + R3-CAM-10 parity: Sponsor challenge active tier checks sc.tiers[claimed_tiers]
  // AND defers increments until chain banks (wiping on cancel_chain).
  {
    SkillsData sd;
    sd.bank_timeout = 3.0;
    sd.tag_award_interval = 0.0;
    SponsorChallenge sc;
    sc.id = "test_sponsor";
    sc.type = "SkillChallenge";
    sc.tiers = {{"Drift", 4, 1, 1000}, {"Drift", 1, 2, 5000}};  // tier 0: any grade x1; tier 1: Awesome (grade 3+) x2
    sd.sponsor_challenges.push_back(sc);
    SkillTracker st(sd, 0.0, 0);
    auto bank_at = [&](double t) {
      SkillInput bi;
      bi.dt = 0.1;
      bi.time = t;
      bi.speed_ms = 20.0;
      bi.forward_speed_ms = 20.0;
      return st.step(bi);
    };
    st.award_named("Drift", 1, 1.0);  // Unbanked -> must NOT commit sponsor progress yet!
    CHECK(st.sponsor_progress().find("test_sponsor") == st.sponsor_progress().end() ||
          st.sponsor_progress().at("test_sponsor").claimed_tiers == 0);
    // Hard collision cancels chain and wipes deferred sponsor feat!
    {
      SkillInput crash;
      crash.dt = 0.1;
      crash.time = 1.5;
      crash.speed_ms = 25.0;
      crash.forward_speed_ms = 25.0;
      crash.world_impact_ms = 20.0;
      st.step(crash);
    }
    CHECK(st.sponsor_progress().find("test_sponsor") == st.sponsor_progress().end() ||
          st.sponsor_progress().at("test_sponsor").count == 0);
    // Now award & bank cleanly (after 3.0 s cancel lockout -> t=6.0):
    st.award_named("Drift", 1, 6.0);
    bank_at(10.0);
    CHECK(st.sponsor_progress().at("test_sponsor").claimed_tiers == 1);
    st.award_named("Drift", 1, 11.0);  // Standard -> must NOT advance tier 1 (requires Awesome)
    bank_at(15.0);
    CHECK(st.sponsor_progress().at("test_sponsor").claimed_tiers == 1);
    CHECK(st.sponsor_progress().at("test_sponsor").count == 1);
    st.award_named("Drift", 3, 16.0);  // Awesome -> increments count to 2 and claims tier 1 on bank!
    bank_at(20.0);
    CHECK(st.sponsor_progress().at("test_sponsor").claimed_tiers == 2);
  }
  // R2-5 parity: Fame.xml dialogueEvent & radioFestivalUpdate propagate onto FameLevel and kRankUp SkillEvent.
  {
    const char* kFame = R"(<Fame>
      <Level rank="249" target="1000" dialogueEvent="FameDialogue249"/>
      <Level rank="175" target="2000" radioFestivalUpdate="RadioUpdate175"/>
    </Fame>)";
    const char* kFeats = R"(<RaceFeats WorldCollisionThreshold="12" CarCollisionThreshold="12" MinSpeedToCancelFeats="10" DelayAfterCancelling="3">
      <Air><Grade LocString="IDS_Air4" Time="0.2" Fame="2500"/></Air>
    </RaceFeats>)";
    const char* kSpon = R"(<SkillChallenges/>)";
    SkillsData sd;
    std::string ferr;
    CHECK(parse_skills_data(kFame, kFeats, kSpon, sd, &ferr));
    CHECK(sd.find_fame_level(249) != nullptr && sd.find_fame_level(249)->dialogue_event == "FameDialogue249");
    CHECK(sd.find_fame_level(175) != nullptr && sd.find_fame_level(175)->radio_festival_update == "RadioUpdate175");

    SkillTracker tr(sd, 0.0, 0, 0, 0);
    tr.award_named("Air", 4, 1.0);
    SkillInput bank_in;
    bank_in.dt = 0.1;
    bank_in.time = 6.0;  // >= 4.5 s bank_timeout after the award
    bank_in.speed_ms = 25.0;
    bank_in.forward_speed_ms = 25.0;
    bool got_rank_meta = false;
    for (const auto& ev : tr.step(bank_in)) {
      if (ev.kind == SkillEvent::kRankUp && ev.rank == 175 &&
          ev.dialogue_event == "FameDialogue249" && ev.radio_festival_update == "RadioUpdate175") {
        got_rank_meta = true;
      }
    }
    CHECK(got_rank_meta);
  }
  // PT-21: oriented ~4.1 m wide discount sign OBB, slow angled wing contact (bug_021),
  // and high-speed swept anti-tunneling check.
  {
    const char* kFlyersWide = R"(<ActivityManager>
      <Activity type="ActivityFlyers" name="flyer_037" />
      <Activity type="ActivityFlyers" name="flyer_038" />
    </ActivityManager>)";
    const char* kObjsWide = R"(<?xml version="1.0" ?>
    <GameObjs>
      <Obj0 GameplayID="flyer_037">
        <Pos x="-3399.242" y="7.986" z="-1347.967"/>
        <Orientation>
          <XAxis x="0.497470" y="0.0" z="0.867481"/>
          <YAxis x="0.0" y="1.0" z="0.0"/>
          <ZAxis x="-0.867481" y="0.0" z="0.497470"/>
        </Orientation>
      </Obj0>
      <Obj1 GameplayID="flyer_038">
        <Pos x="0.0" y="0.0" z="100.0"/>
        <Orientation>
          <XAxis x="1.0" y="0.0" z="0.0"/>
          <YAxis x="0.0" y="1.0" z="0.0"/>
          <ZAxis x="0.0" y="0.0" z="1.0"/>
        </Orientation>
      </Obj1>
    </GameObjs>)";
    Collectibles wdata;
    std::string werr;
    CHECK(parse_collectibles(kFlyersWide, "<ActivityManager/>", kObjsWide, wdata, &werr));
    Progress wprog;
    FreeRoam wfr(wdata, wprog);

    // 1. Exact bug_021 car pose (5.10 km/h = 1.42 m/s clipped into flyer_037) MUST smash & hide it!
    CarState c21;
    c21.x = -3398.418;
    c21.y = 7.988;
    c21.z = -1348.457;
    c21.forward_x = std::sin(2.3896);
    c21.forward_z = std::cos(2.3896);
    c21.half_width = 0.85;
    c21.half_length = 2.05;
    c21.speed = 5.10 / 3.6;
    c21.moved = c21.speed / 120.0;
    auto ev21 = wfr.step(c21);
    CHECK(ev21.size() == 1 && ev21[0].kind == Event::kSignSmashed && ev21[0].index == 0);
    CHECK(wfr.signs_found() == 1 && wfr.hidden()->count("flyer_037"));

    // 2. Miss 3.5 m off the side of flyer_038 (outside car_hw + 2.05 m board half-width).
    CarState c38;
    c38.x = 3.5;
    c38.y = 0.2;
    c38.z = 100.0;
    c38.forward_x = 0.0;
    c38.forward_z = 1.0;
    c38.half_width = 0.9;
    c38.half_length = 2.1;
    c38.speed = 20.0;
    c38.moved = 0.2;
    CHECK(wfr.step(c38).empty());

    // 3. Wing hit at x = 2.2 m (car right edge clips billboard wing) while sweeping past z = 100 at high speed.
    c38.x = 2.2;
    c38.z = 103.5;  // already 3.5 m past the sign center, but moved 4.5 m this step -> swept OBB catches it!
    c38.speed = 80.0;
    c38.moved = 4.5;
    auto ev38 = wfr.step(c38);
    CHECK(ev38.size() == 1 && ev38[0].kind == Event::kSignSmashed && ev38[0].index == 1);
    CHECK(wfr.signs_found() == 2 && wfr.hidden()->count("flyer_038"));
  }

  // Round 3 parity: R3-CAM-04 (add_popularity rank-up/sponsor), R3-CAM-10 (1.0x..5.0x 0.5x multiplier + Donut XML),
  // R3-CAM-11 (Speed Zone off-road fail, Rewind cancel & corridor distance), R3-CAM-09 (Barn progression gating),
  // R3-CAM-21 (H2H wager alignment & difficulty scaling).
  {
    // 1. R3-CAM-04: add_popularity() triggers rank-up, Fame Sponsor CR payouts, and Showcase dialogue/radio metadata.
    const char* kFame = R"(<Fame>
      <Level rank="249" target="1000" dialogueEvent="Rank249Dlg"/>
      <Level rank="225" target="5000" radioFestivalUpdate="Rank225Radio"/>
    </Fame>)";
    const char* kFeats = R"(<RaceFeats WorldCollisionThreshold="12" CarCollisionThreshold="12" MinSpeedToCancelFeats="10" DelayAfterCancelling="3">
      <Donut RetriggerDelay="1.0">
        <Grade LocString="IDS_Donut4" Distance="45" Fame="600"/>
        <Grade LocString="IDS_Donut3" Distance="30" Fame="350"/>
        <Grade LocString="IDS_Donut2" Distance="18" Fame="200"/>
        <Grade LocString="IDS_Donut1" Distance="8" Fame="120"/>
      </Donut>
    </RaceFeats>)";
    const char* kSpon = R"(<SkillChallenges>
      <Challenge id="Fame" type="FameRankChallenge"><Rank levelRequired="225" credits="5000"/></Challenge>
    </SkillChallenges>)";
    SkillsData sd;
    std::string serr;
    CHECK(parse_skills_data(kFame, kFeats, kSpon, sd, &serr));
    SkillTracker tr(sd, 0.0, 0);
    // Explicit-time add_popularity immediately evaluates & returns rank-up + Fame Sponsor payout.
    auto pop_evs = tr.add_popularity(5000.0, 2.0);
    bool saw_rank = false, saw_sponsor = false;
    for (const auto& ev : pop_evs) {
      if (ev.kind == SkillEvent::kRankUp && ev.rank == 225 && ev.dialogue_event == "Rank249Dlg" &&
          ev.radio_festival_update == "Rank225Radio")
        saw_rank = true;
      if (ev.kind == SkillEvent::kFameSponsor && ev.credits == 5000) saw_sponsor = true;
    }
    CHECK(saw_rank && saw_sponsor);
    CHECK(tr.popularity_rank() == 225 && tr.fame_rewards_claimed() == 1);
    // 1-arg add_popularity queues events for the next step() call so fire-and-forget callers receive them.
    SkillTracker tr2(sd, 0.0, 0);
    tr2.add_popularity(5000.0);
    CHECK(tr2.popularity_rank() == 225 && tr2.fame_rewards_claimed() == 1);
    SkillInput ping;
    ping.dt = 0.1;
    ping.time = 3.0;
    ping.speed_ms = 20.0;
    ping.forward_speed_ms = 20.0;
    auto step_evs = tr2.step(ping);
    bool step_rank = false, step_spon = false;
    for (const auto& ev : step_evs) {
      if (ev.kind == SkillEvent::kRankUp && ev.rank == 225) step_rank = true;
      if (ev.kind == SkillEvent::kFameSponsor && ev.credits == 5000) step_spon = true;
    }
    CHECK(step_rank && step_spon);

    // 2. The decoded chain multiplier: (min(n, 41) + 9) * 0.1 for n single skills; combinations and
    // specials do not count. 5 skills show the oracle frame's X1.4.
    SkillTracker tm(sd, 0.0, 0);
    CHECK(near(tm.chain_multiplier(), 1.0f));
    CHECK(near(freeroam::chain_multiplier_for(0), 1.0f) && near(freeroam::chain_multiplier_for(1), 1.0f));
    CHECK(near(freeroam::chain_multiplier_for(2), 1.1f, 1e-6) && near(freeroam::chain_multiplier_for(5), 1.4f, 1e-6));
    CHECK(near(freeroam::chain_multiplier_for(41), 5.0f, 1e-6) && near(freeroam::chain_multiplier_for(90), 5.0f, 1e-6));
    for (int i = 1; i <= 45; ++i) {
      tm.award_named(i % 2 ? "Drift" : "NearMiss", 1, 1.0 + i * 0.01);
      const float expected = float(std::min(i, 41) + 9) * 0.1f;
      CHECK(near(tm.chain_multiplier(), expected, 1e-5));
      CHECK(near(tm.hud().multiplier_f, expected, 1e-5));
    }
    CHECK(near(tm.chain_multiplier(), 5.0f, 1e-5));
    // A combination or special adds points but not to the multiplier's count.
    SkillTracker tc(sd, 0.0, 0);
    tc.award_named("Drift", 1, 1.0);
    tc.award_named("Air", 1, 1.1);
    tc.award_named("DareDevil", 1, 1.2);
    tc.award_named("Landscaping", 1, 1.3);
    CHECK(near(tc.chain_multiplier(), 1.1f, 1e-6) && tc.hud().skill_count == 4);
    // The bank truncates float(points) * multiplier (no rounding up).
    CHECK(freeroam::banked_chain_points(583, 1.4f) == 816);
    CHECK(freeroam::banked_chain_points(999, 1.1f) == 1098);
    CHECK(freeroam::banked_chain_points(0, 3.0f) == 0);

    // Donut XML grades: ~20 m distance spin (>= 18 m, < 30 m) awards grade 2 with XML Fame=200 (not hardcoded 150!).
    SkillTracker td(sd, 0.0, 0);
    SkillInput di;
    di.dt = 0.1;
    di.wheels_on_ground = 4;
    di.wheel_spin = 15.0;
    di.speed_ms = 5.0;  // ~11.2 mph <= 25 mph; 40 steps * 0.5 m = 20 m distance
    di.forward_speed_ms = 5.0;
    di.yaw_rate = 2.0;  // 40 steps * 0.2 rad = 8.0 rad >= 5.2 rad
    for (int k = 0; k < 40; ++k) {
      di.time = 1.0 + k * 0.1;
      td.step(di);
    }
    di.time = 5.1;
    di.yaw_rate = 0.0;
    di.wheel_spin = 0.0;
    auto dev = td.step(di);
    CHECK(!dev.empty() && dev[0].skill_id == "Donut" && dev[0].grade == 2 && dev[0].points == 200);

    // 3. R3-CAM-11: Speed Zone off-road fail (>1.5 s), Rewind cancel_active(), and authored corridor distance.
    const char* kAvgXml = R"(<ActivityManager>
      <Activity type="ActivityAverageSpeed" name="average_speed_01" scoreboard_id="101"><SpeedThreshold minimumSpeed="30.0"/></Activity>
    </ActivityManager>)";
    const char* kSpObjs = R"(<GameObjs>
      <Obj1 GameplayID="average_speed_01_left1"><Pos x="-10" y="0" z="100"/></Obj1>
      <Obj2 GameplayID="average_speed_01_right1"><Pos x="10" y="0" z="100"/></Obj2>
      <Obj3 GameplayID="average_speed_01_left2"><Pos x="-10" y="0" z="300"/></Obj3>
      <Obj4 GameplayID="average_speed_01_right2"><Pos x="10" y="0" z="300"/></Obj4>
    </GameObjs>)";
    SpeedActivitiesData sdata;
    CHECK(parse_speed_activities("<ActivityManager/>", kAvgXml, kSpObjs, sdata, &serr));
    CHECK(near(sdata.zones[0].corridor_distance_m, 200.0, 1e-3));
    SpeedActivityTracker szt(sdata);
    // Enter gate 1, then cancel_active() (simulating Rewind):
    CHECK(szt.step(0.1, 1.0, 0, 98, 0, 102, 40.0).size() == 1);
    CHECK(szt.zone_active());
    szt.cancel_active();
    CHECK(!szt.zone_active() && !szt.hud(1.1).zone_active);
    // A rewind's cancel is not an abort (no ABORT slide).
    CHECK(!szt.hud(1.1).abort_active);
    // Re-enter gate 1, go off-road for > 1.5 s -> fails cleanly with kZoneCancelled + failed_off_road!
    CHECK(szt.step(0.1, 2.0, 0, 98, 0, 102, 40.0).size() == 1);
    szt.set_off_road(true);
    bool offroad_failed = false;
    for (int k = 1; k <= 17; ++k) {
      auto evs = szt.step(0.1, 2.0 + k * 0.1, 0, 102 + (k - 1) * 4.0, 0, 102 + k * 4.0, 40.0);
      for (const auto& e : evs) {
        if (e.kind == SpeedActivityEvent::kZoneCancelled && e.failed_off_road) offroad_failed = true;
      }
    }
    CHECK(offroad_failed && !szt.zone_active());
    // The failed run is reported for the HUD's ABORT, with its live average.
    {
      const auto ah = szt.hud(3.7);
      CHECK(ah.abort_active && !ah.abort_title.empty() && ah.abort_speed_mph > 0.0 && ah.abort_age_s >= 0.0 &&
            ah.abort_age_s < 0.3);
      CHECK(!szt.hud(6.0).abort_active);
    }
    szt.set_off_road(false);
    // Zig-zagging inside the 200 m corridor over 4.0 s must NOT inflate average speed above ~111.8 mph!
    CHECK(szt.step(0.1, 10.0, 0, 98, 0, 102, 50.0).size() == 1);
    double prev_x = 0.0, prev_z = 102.0;
    for (int k = 1; k <= 39; ++k) {
      const double nx = (k % 2 == 0) ? 8.0 : -8.0;  // aggressive zig-zag
      const double nz = 102.0 + k * 5.0;
      szt.step(0.1, 10.0 + k * 0.1, prev_x, prev_z, nx, nz, 50.0);
      prev_x = nx;
      prev_z = nz;
    }
    auto zev = szt.step(0.1, 14.0, prev_x, prev_z, 0.0, 303.0, 50.0);
    CHECK(zev.size() == 1 && zev[0].kind == SpeedActivityEvent::kZoneCompleted);
    // True corridor average is 200 m / 4.0 s = 50 m/s = 111.85 mph (not ~350+ mph from zig-zagging!).
    CHECK(zev[0].speed_mph > 105.0 && zev[0].speed_mph < 120.0);

    // 5. R3-CAM-21: h2h_wager_credits 1:1 alignment & difficulty scaling.
    CHECK(h2h_wager_credits(500, 650) == h2h_reward_credits(500, 650));
    CHECK(h2h_wager_credits(700, 500, 1.25) == h2h_reward_credits(700, 500, 1.25));
    CHECK(h2h_wager_credits(500, 500, 1.5) == 7500);
  }
  if (failures) return 1;
  std::printf("freeroam_tests: all passed\n");
  return 0;
}
