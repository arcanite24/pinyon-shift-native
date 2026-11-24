#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "drive/save.h"
#include "fh1data/stringtables.h"
#include "freeroam/outposts.h"
#include "freeroam/speed_zones.h"

namespace {

int failures = 0;

void check(bool ok, const char* msg) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++failures;
  }
}

void test_synthetic_outposts_and_stunts() {
  const char* gas_xml = R"(
<ActivityManager>
  <Activity type="ActivityGasStation" name="GasStation_01">
    <StateGroup id="gasStation_01">
      <State id="place_car_for_cutscene_enter_activity">
        <Behaviour id="CPlaceCarAtObject" object_name="OUTPOST_001_NODE" />
      </State>
    </StateGroup>
    <TriggerZone object="GASSTATION_001" radius="50" prompt="IDS_GasStationName_01" triggerZoneRadius="150"/>
    <Missions>
      <Mission name="speedstunt_01" />
      <Mission name="photoshoot_01" />
      <Mission name="prstunt_01" />
    </Missions>
  </Activity>
</ActivityManager>
)";

  const char* speed_xml = R"(
<ActivityManager>
  <Activity type="ActivitySpeedStunt" name="speedstunt_01">
    <StateGroup id="speedstunt01">
      <State id="load_mission_specific_route">
        <Behaviour id="CLoadRoute"><Attribute id="route_id" value="226" /></Behaviour>
      </State>
    </StateGroup>
    <SpeedTrap name="speed_camera_01" nameDesc="IDS_SpeedCamera_201"/>
    <Speed easy="150" medium="150" hard="150" />
    <Time easy="60" medium="60" hard="60" />
    <Car carID="1451" carColour="1" />
  </Activity>
</ActivityManager>
)";

  const char* pr_xml = R"(
<ActivityManager>
  <Activity type="ActivityPRStunt" name="prstunt_01">
    <StateGroup id="prstunt01">
      <State id="set_prstunt_satnav_dest">
        <Behaviour id="CSetSatNavDestinationPos" x="500" y="10" z="-200" />
      </State>
    </StateGroup>
    <Popularity easy="10000" medium="10000" hard="10000"/>
    <TimeLimit easy="90" medium="90" hard="90"/>
    <Car carID="1500" carColour="1" />
  </Activity>
</ActivityManager>
)";

  const char* photo_xml = R"(
<ActivityManager>
  <Activity type="ActivityPhotoShoot" name="photoshoot_01">
    <StateGroup id="photoshoot01">
      <State id="load_mission_specific_route">
        <Behaviour id="CLoadRoute"><Attribute id="route_id" value="227" /></Behaviour>
      </State>
    </StateGroup>
    <PhotoZone node_name="mission_photo_end_01" radius="50"/>
    <Car carID="1032" carColour="1" />
  </Activity>
</ActivityManager>
)";

  const char* objs_xml = R"(
<GameObjs>
  <Object GameplayID="GASSTATION_001"><Pos x="100" y="0" z="100"/></Object>
  <Object GameplayID="GASSTATION_001_DISCOVERY"><Pos x="110" y="0" z="100"/></Object>
  <Object GameplayID="OUTPOST_001_NODE"><Pos x="120" y="0" z="105"/></Object>
  <Object GameplayID="speed_camera_01_left"><Pos x="290" y="0" z="0"/></Object>
  <Object GameplayID="speed_camera_01_right"><Pos x="310" y="0" z="0"/></Object>
</GameObjs>
)";

  std::map<int, std::string> routes;
  routes[227] = R"(
<TrackRoute>
  <NamedTransforms>
    <NamedTransform name='mission_photo_end_01'>
      <Transform pos.x='800' pos.y='12' pos.z='-400' facing.x='1' facing.y='0' facing.z='0'/>
    </NamedTransform>
  </NamedTransforms>
</TrackRoute>
)";

  freeroam::OutpostCatalog cat;
  std::string err;
  check(freeroam::parse_outposts(gas_xml, speed_xml, pr_xml, photo_xml, objs_xml, routes, nullptr, nullptr, nullptr,
                                 cat, &err),
        "parse_outposts succeeds on synthetic data");
  check(cat.outposts.size() == 1, "1 synthetic outpost parsed");
  check(cat.stunts.size() == 3, "3 synthetic PR stunts parsed");
  check(cat.stunts_for_outpost("GasStation_01").size() == 3, "3 stunts linked to GasStation_01");

  // Outpost discovery check.
  freeroam::OutpostProgress prog;
  check(prog.check_discovery(cat, 400.0, 400.0) == -1, "far position does not discover outpost");
  check(prog.check_discovery(cat, 150.0, 100.0) == 0, "entering discovery zone unlocks outpost");
  check(prog.is_outpost_discovered("GasStation_01"), "GasStation_01 marked discovered");
  check(prog.check_discovery(cat, 150.0, 100.0) == -1, "already discovered outpost does not re-fire");

  // Speed stunt: crossing gate slowly does not pass; crossing at >=150 mph passes.
  const auto* sp = cat.find_stunt("speedstunt_01");
  check(sp != nullptr && sp->has_gate && std::fabs(sp->target_pos.x - 300.0) < 1e-3, "speed trap gate midpoint");
  freeroam::PrStuntSession sess;
  sess.start(*sp);
  check(sess.update({300.0, 5.0, 300.0, -5.0, 120.0, 0, false, 1.0}) == freeroam::PrStuntState::kActive,
        "crossing trap below target speed stays active");
  check(sess.update({300.0, 5.0, 300.0, -5.0, 155.0, 0, false, 1.0}) == freeroam::PrStuntState::kSucceeded,
        "crossing trap above target speed succeeds");

  // Timeout failure check.
  sess.start(*sp);
  check(sess.update({0.0, 0.0, 0.0, 0.0, 50.0, 0, false, 65.0}) == freeroam::PrStuntState::kFailed,
        "expired speed stunt fails");

  // Skill stunt check.
  const auto* sk = cat.find_stunt("prstunt_01");
  check(sk != nullptr && sk->target_points == 10000, "skill stunt target parsed");
  sess.start(*sk, 25000);
  check(sess.update({500.0, -200.0, 500.0, -200.0, 40.0, 30000, false, 1.0}) == freeroam::PrStuntState::kActive,
        "halfway skill points stays active");
  check(sess.earned_skill_points() == 5000, "earned skill delta tracked from baseline");
  check(sess.update({500.0, -200.0, 500.0, -200.0, 40.0, 35500, false, 1.0}) == freeroam::PrStuntState::kSucceeded,
        "reaching target skill points succeeds");

  // Photo Op check.
  const auto* ph = cat.find_stunt("photoshoot_01");
  check(ph != nullptr && std::fabs(ph->target_pos.x - 800.0) < 1e-3, "photo zone parsed from route XML");
  sess.start(*ph);
  check(sess.update({0.0, 0.0, 0.0, 0.0, 0.0, 0, true, 1.0}) == freeroam::PrStuntState::kActive,
        "photo press outside zone does not complete");
  check(sess.update({805.0, -395.0, 805.0, -395.0, 5.0, 0, true, 0.1}) == freeroam::PrStuntState::kSucceeded,
        "photo press inside zone succeeds");

  // DriveSave round-trip for discovered Outposts & completed PR Stunts.
  prog.complete_stunt("speedstunt_01");
  prog.complete_stunt("photoshoot_01");
  const auto tmp = std::filesystem::temp_directory_path() / "shift_outpost_test_save.txt";
  drive::DriveSave s;
  s.car = "che_corvettegs_10";
  s.discovered_outposts.assign(prog.discovered_outposts.begin(), prog.discovered_outposts.end());
  s.completed_pr_stunts.assign(prog.completed_pr_stunts.begin(), prog.completed_pr_stunts.end());
  check(drive::write_save(tmp, s, &err), "write_save succeeds");
  drive::DriveSave loaded;
  check(drive::read_save(tmp, loaded, &err), "read_save succeeds");
  const std::set<std::string> got_op(loaded.discovered_outposts.begin(), loaded.discovered_outposts.end());
  const std::set<std::string> got_st(loaded.completed_pr_stunts.begin(), loaded.completed_pr_stunts.end());
  check(got_op.count("GasStation_01") == 1, "discovered outpost persisted");
  check(got_st.count("speedstunt_01") == 1 && got_st.count("photoshoot_01") == 1,
        "completed PR stunts persisted");
  std::error_code ec;
  std::filesystem::remove(tmp, ec);
}

void test_colorado_corpus() {
  const char* root = std::getenv("SHIFT_GAME_ROOT");
  if (!root || !*root) return;
  freeroam::OutpostCatalog cat;
  std::string err;
  check(freeroam::load_outposts(root, cat, &err), "load_outposts succeeds on Colorado game root");
  check(cat.outposts.size() == 10, "Colorado has all 10 regional Horizon Outposts");
  check(cat.stunts.size() == 30, "Colorado has all 30 Outpost PR Stunts");
  int speed_n = 0, skill_n = 0, photo_n = 0;
  for (const auto& s : cat.stunts) {
    if (s.kind == freeroam::PrStuntKind::kSpeed) ++speed_n;
    else if (s.kind == freeroam::PrStuntKind::kSkill) ++skill_n;
    else if (s.kind == freeroam::PrStuntKind::kPhoto) ++photo_n;
    check(!s.outpost_id.empty(), "every PR stunt linked to an Outpost");
    check(std::hypot(s.target_pos.x, s.target_pos.z) > 10.0, "every PR stunt has real world target");
  }
  check(speed_n == 10 && skill_n == 10 && photo_n == 10, "10 Speed + 10 Skill + 10 Photo stunts");
  for (const auto& o : cat.outposts) {
    check(!o.display_name.empty(), "Outpost has localized display name");
    check(cat.stunts_for_outpost(o.id).size() == 3, "every Outpost has 3 PR Stunts");
  }
  std::printf("Colorado Outposts check: %zu outposts (e.g. '%s'), %zu PR stunts (%d speed, %d skill, %d photo)\n",
              cat.outposts.size(), cat.outposts.empty() ? "" : cat.outposts[0].display_name.c_str(),
              cat.stunts.size(), speed_n, skill_n, photo_n);
}

void test_r2_5_outpost_tiers_fast_travel_and_speed_camera_names() {
  const char* gas_xml = R"(
<ActivityManager>
  <Activity type="ActivityGasStation" name="GasStation_01">
    <TriggerZone object="GASSTATION_001" radius="50" prompt="IDS_GasStationName_01" triggerZoneRadius="150"/>
    <Missions>
      <Mission name="speedstunt_01" />
      <Mission name="photoshoot_01" />
      <Mission name="prstunt_01" />
    </Missions>
  </Activity>
  <Activity type="ActivityGasStation" name="GasStation_09">
    <TriggerZone object="GASSTATION_009" radius="50" prompt="IDS_GasStationName_09" triggerZoneRadius="150"/>
    <Missions>
      <Mission name="speedstunt_09" />
    </Missions>
  </Activity>
</ActivityManager>
)";
  // Distinct retail easy/medium/hard values on speedstunt_01; missing medium/hard on speedstunt_09 -> fallback to easy.
  const char* speed_xml = R"(
<ActivityManager>
  <Activity type="ActivitySpeedStunt" name="speedstunt_01">
    <SpeedTrap name="speed_camera_01" nameDesc="IDS_SpeedCamera_201"/>
    <Speed easy="130" medium="145" hard="162" />
    <Time easy="90" medium="75" hard="60" />
  </Activity>
  <Activity type="ActivitySpeedStunt" name="speedstunt_09">
    <SpeedTrap name="speed_camera_01" nameDesc="IDS_SpeedCamera_201"/>
    <Speed easy="175" />
    <Time easy="55" />
  </Activity>
</ActivityManager>
)";
  const char* pr_xml = R"(
<ActivityManager>
  <Activity type="ActivityPRStunt" name="prstunt_01">
    <Popularity easy="10000" medium="15000" hard="22500"/>
    <TimeLimit easy="120" medium="100" hard="85"/>
  </Activity>
</ActivityManager>
)";
  const char* photo_xml = R"(
<ActivityManager>
  <Activity type="ActivityPhotoShoot" name="photoshoot_01">
    <PhotoZone node_name="mission_photo_end_01" radius="50"/>
  </Activity>
</ActivityManager>
)";
  const char* objs_xml = R"(
<GameObjs>
  <Object GameplayID="GASSTATION_001"><Pos x="100" y="0" z="100"/></Object>
  <Object GameplayID="GASSTATION_009"><Pos x="900" y="0" z="900"/></Object>
  <Object GameplayID="speed_camera_01_left"><Pos x="290" y="0" z="0"/></Object>
  <Object GameplayID="speed_camera_01_right"><Pos x="310" y="0" z="0"/></Object>
</GameObjs>
)";

  freeroam::OutpostCatalog cat;
  std::string err;
  check(freeroam::parse_outposts(gas_xml, speed_xml, pr_xml, photo_xml, objs_xml, {}, nullptr, nullptr, nullptr, cat,
                                 &err),
        "R2-5 parse_outposts succeeds");

  // 1. Retail easy/medium/hard XML attributes parsed directly (no fabricated multipliers).
  const auto* sp1 = cat.find_stunt("speedstunt_01");
  const auto* sp9 = cat.find_stunt("speedstunt_09");
  const auto* pr1 = cat.find_stunt("prstunt_01");
  check(sp1 != nullptr && pr1 != nullptr && sp9 != nullptr, "all R2-5 stunts found");
  if (sp1 && pr1 && sp9) {
    freeroam::PrStuntSession sess;
    sess.start(*sp1, 0, 0);
    check(std::fabs(sess.stunt().target_mph - 130.0) < 1e-6 && std::fabs(sess.time_remaining_s() - 90.0) < 1e-6,
          "SpeedStunt Easy uses XML easy attributes");
    sess.start(*sp1, 0, 1);
    check(std::fabs(sess.stunt().target_mph - 145.0) < 1e-6 && std::fabs(sess.time_remaining_s() - 75.0) < 1e-6,
          "SpeedStunt Medium uses exact XML medium attributes");
    sess.start(*sp1, 0, 2);
    check(std::fabs(sess.stunt().target_mph - 162.0) < 1e-6 && std::fabs(sess.time_remaining_s() - 60.0) < 1e-6,
          "SpeedStunt Hard uses exact XML hard attributes");

    // Fallback to easy when medium/hard are omitted in XML.
    sess.start(*sp9, 0, 2);
    check(std::fabs(sess.stunt().target_mph - 175.0) < 1e-6 && std::fabs(sess.time_remaining_s() - 55.0) < 1e-6,
          "SpeedStunt falls back to easy when medium/hard absent");

    // Skill stunt XML tiers.
    sess.start(*pr1, 0, 1);
    check(sess.stunt().target_points == 15000 && std::fabs(sess.time_remaining_s() - 100.0) < 1e-6,
          "Skill PRStunt Medium uses exact XML medium attributes");
    sess.start(*pr1, 0, 2);
    check(sess.stunt().target_points == 22500 && std::fabs(sess.time_remaining_s() - 85.0) < 1e-6,
          "Skill PRStunt Hard uses exact XML hard attributes");

    // Retail tiered rewards scale cleanly from early (Outpost 1) to late (Outpost 9).
    check(sp1->reward_credits == 5000 && sp1->reward_popularity == 5000, "early Outpost Speed Stunt retail tier");
    check(sp9->reward_credits == 15000 && sp9->reward_popularity == 15000, "late Outpost Speed Stunt retail tier");
  }

  // 2. Free fast travel unlocks strictly once all 3 PR Stunts at an Outpost are completed.
  freeroam::OutpostProgress prog;
  check(!prog.is_outpost_fast_travel_free(1), "fast travel NOT free at 0/3 stunts");
  prog.complete_stunt("speedstunt_01");
  check(!prog.is_outpost_fast_travel_free(1), "fast travel NOT free at 1/3 stunts");
  prog.complete_stunt("prstunt_01");
  check(!prog.is_outpost_fast_travel_free(1), "fast travel NOT free at 2/3 stunts");
  check(!prog.is_outpost_fast_travel_free(cat, 1), "catalog fast travel NOT free at 2/3 stunts");
  prog.complete_stunt("photoshoot_01");
  check(prog.is_outpost_fast_travel_free(1), "standalone is_outpost_fast_travel_free(1) true at 3/3 stunts");
  check(prog.is_outpost_fast_travel_free(cat, 1), "catalog is_outpost_fast_travel_free(cat, 1) true at 3/3 stunts");
  check(prog.is_outpost_fast_travel_free(cat, "GasStation_01"),
        "catalog string-id is_outpost_fast_travel_free true at 3/3 stunts");
  check(!prog.is_outpost_fast_travel_free(2), "other outposts remain paid");

  // 3. Localized retail Speed Trap & Speed Zone names from List_SpeedCameras.str.
  const char* cam_xml = R"(<ActivityManager>
    <Activity type="ActivitySpeedCamera" name="speed_camera_01" scoreboard_id="201"><SpeedThreshold minimumSpeed="30.0"/></Activity>
  </ActivityManager>)";
  const char* avg_xml = R"(<ActivityManager>
    <Activity type="ActivityAverageSpeed" name="average_speed_05" scoreboard_id="105"><SpeedThreshold minimumSpeed="30.0"/></Activity>
  </ActivityManager>)";
  const char* sp_objs = R"(<GameObjs>
    <Obj1 GameplayID="speed_camera_01_left"><Pos x="-10" y="0" z="50"/></Obj1>
    <Obj2 GameplayID="speed_camera_01_right"><Pos x="10" y="0" z="50"/></Obj2>
    <Obj3 GameplayID="average_speed_05_left1"><Pos x="-10" y="0" z="100"/></Obj3>
    <Obj4 GameplayID="average_speed_05_right1"><Pos x="10" y="0" z="100"/></Obj4>
    <Obj5 GameplayID="average_speed_05_left2"><Pos x="-10" y="0" z="300"/></Obj5>
    <Obj6 GameplayID="average_speed_05_right2"><Pos x="10" y="0" z="300"/></Obj6>
  </GameObjs>)";

  fh1::StringTable cam_str;
  auto put_str = [&](const char* key, const char* val) {
    const uint16_t h = fh1::string_hash(key);
    std::u16string u;
    for (const char* p = val; *p; ++p) u.push_back(char16_t(uint8_t(*p)));
    auto it = std::lower_bound(cam_str.keys.begin(), cam_str.keys.end(), h);
    const size_t idx = size_t(it - cam_str.keys.begin());
    cam_str.keys.insert(it, h);
    cam_str.text.insert(cam_str.text.begin() + std::ptrdiff_t(idx), std::move(u));
  };
  put_str("IDS_SpeedCamera_201", "Carson Highway Speed Trap");
  put_str("IDS_SpeedCamera_105", "Red Rock Canyon Speed Zone");

  freeroam::SpeedActivitiesData sdata;
  check(freeroam::parse_speed_activities(cam_xml, avg_xml, sp_objs, sdata, &err, &cam_str),
        "parse_speed_activities with List_SpeedCameras.str succeeds");
  check(sdata.traps.size() == 1 && sdata.traps[0].display_name == "Carson Highway Speed Trap",
        "Speed Trap resolves localized retail name");
  check(sdata.zones.size() == 1 && sdata.zones[0].display_name == "Red Rock Canyon Speed Zone",
        "Speed Zone resolves localized retail name");

  freeroam::SpeedActivityTracker tracker(sdata);
  const auto ev1 = tracker.step(0.1, 1.0, 0, 48, 0, 52, 50.0);
  check(ev1.size() == 1 && ev1[0].label == "Carson Highway Speed Trap",
        "Speed Trap trigger event uses localized retail label");
  check(tracker.hud(1.1).banner_title == "Carson Highway Speed Trap",
        "Speed Trap HUD banner uses localized retail name");
  const auto ev2 = tracker.step(0.1, 2.0, 0, 98, 0, 102, 50.0);
  check(ev2.size() == 1 && ev2[0].label == "Red Rock Canyon Speed Zone",
        "Speed Zone entry event uses localized retail label");
  check(tracker.hud(2.1).active_zone_id == "Red Rock Canyon Speed Zone",
        "Speed Zone active HUD uses localized retail name");
}

}  // namespace

int main() {
  test_synthetic_outposts_and_stunts();
  test_r2_5_outpost_tiers_fast_travel_and_speed_camera_names();
  test_colorado_corpus();
  if (failures > 0) {
    std::fprintf(stderr, "outpost_tests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("outpost_tests: PASS\n");
  return 0;
}
