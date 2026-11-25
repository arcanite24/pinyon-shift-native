#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "drive/save.h"
#include "fh1data/stringtables.h"
#include "freeroam/fast_travel.h"
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

    // PR stunts pay nothing (the title only raises the fast-travel discount).
    check(sp1->reward_credits == 0 && sp1->reward_popularity == 0, "early Outpost Speed Stunt pays nothing");
    check(sp9->reward_credits == 0 && sp9->reward_popularity == 0, "late Outpost Speed Stunt pays nothing");
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

  // The fast-travel price over the outpost's completed stunts (disc tuning).
  {
    const freeroam::FastTravelTuning ft;
    freeroam::OutpostProgress p2;
    check(p2.stunts_at(cat, "GasStation_01") == 3, "outpost 1 has 3 PR stunts");
    check(p2.fast_travel_cost(cat, "GasStation_01", ft) == 10000, "0/3 stunts: 10,000 CR");
    p2.complete_stunt("speedstunt_01");
    check(p2.fast_travel_cost(cat, "GasStation_01", ft) == 7500, "1/3 stunts: 7,500 CR");
    p2.complete_stunt("prstunt_01");
    check(p2.fast_travel_cost(cat, "GasStation_01", ft) == 5000, "2/3 stunts: 5,000 CR");
    p2.complete_stunt("photoshoot_01");
    check(p2.fast_travel_cost(cat, "GasStation_01", ft) == 0, "3/3 stunts: free");
    check(p2.fast_travel_cost(cat, "NoSuchOutpost", ft) == -1, "unknown outpost: -1");
    // The title's formula on other tuning: trunc of the discount, never below 0.
    freeroam::FastTravelTuning odd;
    odd.total_cost = 999;
    odd.discount = {0.0, 0.333, 1.5, 0.75};
    check(freeroam::fast_travel_cost(odd, 1, 3) == 999 - 332, "trunc(999 * 0.333) off");
    check(freeroam::fast_travel_cost(odd, 2, 3) == 0, "a discount above 1 is free, not negative");
    check(freeroam::fast_travel_cost(odd, 3, 3) == 0, "all stunts done: free");
    check(freeroam::fast_travel_cost(odd, 3, 4) == 250, "3 of 4 stunts: Discount_3");
  }

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
  // R3-CAM-12: Skill Stunt requires car inside stunt zone + BANKED points only;
  // Photo Op requires BOTH player car and landmark inside camera view frustum.
  {
    freeroam::PrStunt skill_stunt;
    skill_stunt.id = "prstunt_r3";
    skill_stunt.kind = freeroam::PrStuntKind::kSkill;
    skill_stunt.target_pos = {500.0, 0.0, -200.0};
    skill_stunt.target_points = 10000;
    skill_stunt.time_limit_s = 120.0;

    freeroam::PrStuntSession s;
    s.start(skill_stunt, 1000);
    // 1. Banking 15,000 pts OUTSIDE stunt zone (1500 m away) must NOT count!
    freeroam::PrStuntUpdate u_out{};
    u_out.car_x = 2000.0;
    u_out.car_z = -200.0;
    u_out.banked_skill_points = 16000;
    u_out.dt = 1.0;
    check(s.update(u_out) == freeroam::PrStuntState::kActive && s.earned_skill_points() == 0,
          "R3-CAM-12: Skill Stunt ignores banked points earned outside target zone");
    // 2. Unbanked combo of 12,000 pts INSIDE stunt zone must NOT complete stunt!
    freeroam::PrStuntUpdate u_unbanked{};
    u_unbanked.car_x = 500.0;
    u_unbanked.car_z = -200.0;
    u_unbanked.total_skill_points = 28000;
    u_unbanked.banked_skill_points = 16000;
    u_unbanked.unbanked_skill_points = 12000;
    u_unbanked.dt = 1.0;
    check(s.update(u_unbanked) == freeroam::PrStuntState::kActive && s.earned_skill_points() == 0,
          "R3-CAM-12: Skill Stunt ignores unbanked combo points");
    // 3. Banking 10,500 pts INSIDE stunt zone succeeds!
    freeroam::PrStuntUpdate u_banked{};
    u_banked.car_x = 500.0;
    u_banked.car_z = -200.0;
    u_banked.banked_skill_points = 26500;
    u_banked.dt = 1.0;
    check(s.update(u_banked) == freeroam::PrStuntState::kSucceeded && s.earned_skill_points() == 10500,
          "R3-CAM-12: Skill Stunt succeeds when banking target points inside stunt zone");

    // Photo Op frustum validation:
    freeroam::PrStunt photo_stunt;
    photo_stunt.id = "photoshoot_r3";
    photo_stunt.kind = freeroam::PrStuntKind::kPhoto;
    photo_stunt.target_pos = {800.0, 0.0, -400.0};
    photo_stunt.photo_radius = 50.0;
    photo_stunt.time_limit_s = 120.0;
    s.start(photo_stunt);
    // Camera inside zone looking AWAY (-Z) -> car & landmark behind camera -> MUST reject!
    freeroam::PrStuntUpdate p_away{};
    p_away.car_x = 800.0;
    p_away.car_y = 0.0;
    p_away.car_z = -410.0;
    p_away.photo_pressed = true;
    p_away.dt = 0.1;
    p_away.has_camera = true;
    p_away.camera_pos = {800.0, 2.0, -420.0};
    p_away.camera_forward = {0.0, 0.0, -1.0};  // facing away from both car (-410) and landmark (-400)
    p_away.camera_right = {-1.0, 0.0, 0.0};
    p_away.camera_up = {0.0, 1.0, 0.0};
    check(s.update(p_away) == freeroam::PrStuntState::kActive && !s.photo_framed(),
          "R3-CAM-12: Photo Op rejects shot when car & landmark are behind camera");
    // Camera positioned between car and landmark looking toward landmark -> car is behind camera -> MUST reject!
    freeroam::PrStuntUpdate p_no_car = p_away;
    p_no_car.camera_pos = {800.0, 2.0, -405.0};
    p_no_car.camera_forward = {0.0, 0.0, 1.0};
    p_no_car.camera_right = {1.0, 0.0, 0.0};
    check(s.update(p_no_car) == freeroam::PrStuntState::kActive && !s.photo_framed(),
          "R3-CAM-12: Photo Op rejects shot when player car is not in camera frustum");
    // Camera at z=-420 looking +Z -> both car (-410) and landmark (-400) inside frustum -> SUCCEEDS!
    freeroam::PrStuntUpdate p_good = p_no_car;
    p_good.camera_pos = {800.0, 2.0, -420.0};
    check(s.update(p_good) == freeroam::PrStuntState::kSucceeded && s.photo_framed(),
          "R3-CAM-12: Photo Op succeeds when both car and landmark project inside camera frustum");
  }
}

// The outpost fee: TotalCost less Discount_<done>_Missions, nothing once all
// of the outpost's own missions are done; festival places free; events,
// hubs, speed traps, signs and barns not destinations.
void test_fast_travel_cost_rule() {
  freeroam::FastTravelTunables t;
  check(freeroam::outpost_fast_travel_cost(t, 3, 0) == 10000, "fast travel: 0/3 missions costs 10,000");
  check(freeroam::outpost_fast_travel_cost(t, 3, 1) == 7500, "fast travel: 1/3 missions costs 7,500");
  check(freeroam::outpost_fast_travel_cost(t, 3, 2) == 5000, "fast travel: 2/3 missions costs 5,000");
  check(freeroam::outpost_fast_travel_cost(t, 3, 3) == 0, "fast travel: 3/3 missions is free");
  check(freeroam::outpost_fast_travel_cost(t, 3, 5) == 0, "fast travel: more than all done is free");
  check(freeroam::outpost_fast_travel_cost(t, 0, 0) == 0, "fast travel: an outpost with no missions is free");
  // Four missions: the fourth discount (Discount_3) is reached before all done.
  check(freeroam::outpost_fast_travel_cost(t, 4, 3) == 0, "fast travel: Discount_3 = 1.0 makes 3/4 free");
  freeroam::FastTravelTunables code_defaults;
  code_defaults.total_cost = 1000;
  code_defaults.discount = {0.0f, 0.25f, 0.5f, 0.75f};
  check(freeroam::outpost_fast_travel_cost(code_defaults, 4, 3) == 250, "fast travel: Discount_3 0.75 of 1000");
  check(freeroam::outpost_discount_percent(3, 0) == 0 && freeroam::outpost_discount_percent(3, 1) == 25 &&
            freeroam::outpost_discount_percent(3, 2) == 50 && freeroam::outpost_discount_percent(3, 3) == 100,
        "fast travel: Discount: 0/25/50/100 %");

  const char* reg = R"(<GlobalRegistry>
  <Value id="Activities/Outposts/Discount_0_Missions" type="float" value="0.100000"/>
  <Value id="Activities/Outposts/Discount_1_Missions" type="float" value="0.200000"/>
  <Value id="Activities/Outposts/Discount_2_Missions" type="float" value="0.400000"/>
  <Value id="Activities/Outposts/Discount_3_Missions" type="float" value="0.800000"/>
  <Value id="Activities/Outposts/TotalCost" type="int" value="20000"/>
  <Value id="Activities/RoadDiscovery/UnvisitedDistanceThreshold" type="float" value="75.000000"/>
</GlobalRegistry>)";
  freeroam::FastTravelTunables parsed;
  check(freeroam::parse_fast_travel_tunables(reg, parsed), "fast travel: registry parses");
  check(parsed.total_cost == 20000, "fast travel: TotalCost read");
  check(std::fabs(parsed.discount[0] - 0.1f) < 1e-6f && std::fabs(parsed.discount[3] - 0.8f) < 1e-6f,
        "fast travel: Discount_n_Missions read");
  check(freeroam::outpost_fast_travel_cost(parsed, 3, 0) == 18000 && freeroam::outpost_fast_travel_cost(parsed, 3, 2) == 12000,
        "fast travel: cost follows the registry");

  using K = freeroam::FastTravelKind;
  check(freeroam::fast_travel_kind("gas_station") == K::kOutpost, "fast travel: outposts cost");
  for (const char* tag : {"racecentral", "workshop", "autoshow", "paintshop", "carclub", "dlccenter"})
    check(freeroam::fast_travel_kind(tag) == K::kFree, "fast travel: festival places are free");
  for (const char* tag : {"race", "exhibition", "streetrace", "nemesisrace", "speed_camera_discovered",
                          "average_speed_camera_completed", "flyer", "barnfind", "barnfind_collected"})
    check(freeroam::fast_travel_kind(tag) == K::kNone, "fast travel: events, hubs, traps, signs, barns are not");
  const auto q = freeroam::quote_fast_travel(t, "gas_station", 3, 1, 7499);
  check(q.eligible() && q.cost == 7500 && !q.affordable, "fast travel: 7,499 Cr cannot pay 7,500");
  check(freeroam::quote_fast_travel(t, "gas_station", 3, 1, 7500).affordable, "fast travel: 7,500 Cr can pay 7,500");
  check(freeroam::quote_fast_travel(t, "racecentral", 0, 0, 0).affordable, "fast travel: the festival is free at 0 Cr");
  check(!freeroam::quote_fast_travel(t, "race", 0, 0, 1000000).eligible(), "fast travel: events are not eligible");

  // Per outpost: only its own missions count.
  freeroam::OutpostCatalog cat;
  freeroam::Outpost a, b;
  a.id = "GasStation_01", a.index = 1, a.mission_ids = {"speedstunt_01", "photoshoot_01", "prstunt_01"};
  b.id = "GasStation_02", b.index = 2, b.mission_ids = {"speedstunt_02", "photoshoot_02", "prstunt_02"};
  cat.outposts = {a, b};
  freeroam::OutpostProgress prog;
  prog.complete_stunt("speedstunt_01");
  prog.complete_stunt("photoshoot_01");
  prog.complete_stunt("prstunt_02");
  const auto ma = freeroam::outpost_missions(cat, prog, "GasStation_01");
  const auto mb = freeroam::outpost_missions(cat, prog, "GasStation_02");
  check(ma.total == 3 && ma.done == 2 && freeroam::outpost_fast_travel_cost(t, ma.total, ma.done) == 5000,
        "fast travel: outpost 1 with 2/3 done costs 5,000");
  check(mb.total == 3 && mb.done == 1 && freeroam::outpost_fast_travel_cost(t, mb.total, mb.done) == 7500,
        "fast travel: outpost 2 counts only its own missions");
}

void test_fast_travel_corpus() {
  const char* root = std::getenv("SHIFT_GAME_ROOT");
  if (!root || !*root) return;
  freeroam::FastTravelTunables t;
  std::string err;
  check(freeroam::load_fast_travel_tunables(root, t, &err), "fast travel: GlobalRegistry.xml loads");
  check(t.total_cost == 10000, "fast travel: retail TotalCost is 10,000");
  check(t.discount[0] == 0.0f && t.discount[1] == 0.25f && t.discount[2] == 0.5f && t.discount[3] == 1.0f,
        "fast travel: retail discounts 0, 0.25, 0.5, 1.0");
  freeroam::OutpostCatalog cat;
  if (freeroam::load_outposts(root, cat, &err)) {
    freeroam::OutpostProgress none;
    for (const auto& o : cat.outposts) {
      const auto m = freeroam::outpost_missions(cat, none, o.id);
      check(m.total == 3 && freeroam::outpost_fast_travel_cost(t, m.total, m.done) == 10000,
            "fast travel: every retail outpost starts at 10,000 Cr");
    }
  }
  std::printf("Fast travel tunables: TotalCost %lld, discounts %.2f %.2f %.2f %.2f\n", (long long)t.total_cost,
              t.discount[0], t.discount[1], t.discount[2], t.discount[3]);
}

}  // namespace

int main() {
  test_fast_travel_cost_rule();
  test_fast_travel_corpus();
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
