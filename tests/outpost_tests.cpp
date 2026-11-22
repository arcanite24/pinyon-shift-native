#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "drive/save.h"
#include "freeroam/outposts.h"

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

}  // namespace

int main() {
  test_synthetic_outposts_and_stunts();
  test_colorado_corpus();
  if (failures > 0) {
    std::fprintf(stderr, "outpost_tests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("outpost_tests: PASS\n");
  return 0;
}
