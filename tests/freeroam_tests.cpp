// Free-roam collectibles (SPEC-FREEROAM-COLLECTIBLES): the spec's synthetic
// cases for the map projection and the upgrade discount, the step rules on
// a small made-up set, and, with SHIFT_GAME_ROOT, the counts and
// placements read from the player's disc.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

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
    <TriggerZone object="BARNFIND_A" name="BARNFIND_A" radius="10" maxMPH="50" />
    <UnlockCar id="9001" stringTableNameId="IDS_Barnfind_Car_A"/>
  </Activity>
  <Activity type="ActivityBarnFind" name="barnfind_02">
    <HintRegion x_offset="0" y_offset="0" angle="0" x_radius="300" y_radius="300" />
    <DoorInfo open_id="BF_B_OPEN" closed_id="BF_B_CLOSED" open_coll_id="BF_B_OPENC" closed_coll_id="BF_B_CLOSEDC" />
    <TriggerZone object="BARNFIND_B" name="BARNFIND_B" radius="10" maxMPH="50" />
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

  // No rumour before 50 km; then barn 01, then barn 02 50 km later.
  car.x = 5000, car.z = 5000, car.moved = 49999;
  CHECK(fr.step(car).empty());
  car.moved = 1;
  ev = fr.step(car);
  CHECK(ev.size() == 1 && ev[0].kind == Event::kRumourStarted && ev[0].index == 0);
  CHECK(progress.barns[0].status == kBarnRumour && near(progress.barns[0].rumour_at, 50000));
  car.moved = 0;
  // Discovery: within 10 m and below 50 mph (22.35 m/s).
  car.x = 1005, car.z = 1000, car.speed = 25;
  CHECK(fr.step(car).empty());
  car.speed = 20;
  ev = fr.step(car);
  CHECK(ev.size() == 1 && ev[0].kind == Event::kBarnFound && ev[0].index == 0);
  CHECK(progress.barns[0].status == kBarnFound && progress.garage.size() == 1 && progress.garage[0] == 9001);
  CHECK(fr.hidden()->count("bf_a_closed") && fr.hidden()->count("barnfind_a") && !fr.hidden()->count("bf_a_open"));
  // Barn 02 is not spawned: driving to it finds nothing.
  car.x = -1000, car.z = -1000, car.speed = 5;
  CHECK(fr.step(car).empty() && progress.barns[1].status == kBarnNotSpawned);
  car.moved = 50000;
  ev = fr.step(car);  // the rumour starts this step; discovery tests next step
  CHECK(ev.size() == 1 && ev[0].kind == Event::kRumourStarted && ev[0].index == 1);
  car.moved = 0;
  ev = fr.step(car);
  CHECK(ev.size() == 1 && ev[0].kind == Event::kBarnFound && ev[0].index == 1 && fr.barns_found() == 2);

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
    in.time = 1.4;  // >= 0.3 s later: queued Landscaping tag flushes & triggers AnySkill sponsor tier!
    auto e2 = tr.step(in);
    bool got_landscaping = false, got_sponsor = false;
    for (const auto& ev : e2) {
      if (ev.skill_id == "Landscaping") got_landscaping = true;
      if (ev.kind == SkillEvent::kSponsorChallenge && ev.credits == 5000) got_sponsor = true;
    }
    CHECK(got_landscaping);
    CHECK(got_sponsor);
    // 2. Bank chain after timeout at speed -> updates MaxSkillsCarOrdinal!
    in.time = 5.0;
    auto e3 = tr.step(in);
    CHECK(!e3.empty() && e3[0].kind == SkillEvent::kChainBanked);
    CHECK(tr.max_skills_score() >= 1100 && tr.max_skills_car() == 1219);
    // 3. MinSpeedToCancelFeats: dropping below 10 mph cancels active chain instead of banking!
    in.time = 6.0;
    in.car_impact_ms = 2.0;  // Trading Paint (below 12 m/s crash threshold)
    in.nearest_car_dist = 2.0;
    auto e4 = tr.step(in);
    CHECK(!e4.empty() && e4[0].skill_id == "TradingPaint");
    in.car_impact_ms = 0.0;
    in.nearest_car_dist = 100.0;
    in.speed_ms = 2.0;  // ~4.5 mph (< 10 mph)
    in.forward_speed_ms = 2.0;
    in.time = 7.5;
    auto e5 = tr.step(in);
    CHECK(!e5.empty() && e5[0].kind == SkillEvent::kChainLost);
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
    fm.set_roster({{1219, "NIS_SkylineGTRVSpecII_02", "", "A", 540}});
    const uint32_t fid = fm.spawn_ahead({0, 0, 100}, 0.0, 20.0);
    CHECK(fid > 0);
    fm.step(0.05, 1.0, {0, 0, 100}, 0.0, 20.0);
    CHECK(fm.hud(1.0).prompt_visible && fm.hud(1.0).prompt_driver == "Darius Flynt" &&
          fm.hud(1.0).prompt_wager == h2h_wager_credits(540, 0));
    // Verify Festival AI velocity/RPM telemetry & player collision impulse.
    CHECK(!fm.cars().empty() && fm.cars()[0].rpm > 900.0);
    const auto fpos = fm.cars()[0].position;
    const auto col = fm.resolve_player_collision({fpos.x + 0.8, fpos.y, fpos.z - 0.5}, {-12.0, 0.0, 20.0}, 1500.0, 1.0 / 120.0);
    CHECK(col.hit && col.impulse_magnitude > 100.0);
    CHECK(fm.start_head_to_head({0, 0, 100}, 0.0, 1.0, fid));
    CHECK(fm.h2h_active() && fm.h2h_route().size() >= 10);
    const auto fin = fm.h2h_finish_pos();
    HeadToHeadResult hres;
    CHECK(fm.step(0.05, 5.0, {fin[0], fin[1], fin[2]}, 0.0, 40.0, &hres));
    CHECK(hres.won && hres.payout_credits == h2h_wager_credits(540, 0));
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
    CHECK(h_before.active && h_before.chain_points > 0 && h_before.multiplier >= 2);
    CHECK(st.chain_remaining_s(11.5) > 2.0);
    const auto snap = st.capture_rewind();
    // Simulate wreck cancelling chain.
    st.clear_active_chain();
    CHECK(!st.hud().active && st.hud().chain_points == 0);
    // Rewind restores the intact skill chain!
    st.restore_rewind(snap);
    const auto h_after = st.hud();
    CHECK(h_after.active && h_after.chain_points == h_before.chain_points && h_after.multiplier == h_before.multiplier);
    CHECK(st.chain_remaining_s(11.5) > 2.0);
  }
  if (failures) return 1;
  std::printf("freeroam_tests: all passed\n");
  return 0;
}
