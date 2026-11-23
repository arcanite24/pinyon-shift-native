// The 1000 Club evaluator (SPEC-V4-CONTENT, The 1000 Club):
// 1) Synthetic evaluation matrix covering every single condition category
//    (speed, timer, gear, in_air, is_drifting, distance, location, collided,
//    skills + grades + all 16 types, skill_points, num_skill_chains,
//    smashables typed/untyped/interval, speed_camera trap/zone/combined/unique/interval,
//    event_started/restarted/completed/id/hub/career_event_style,
//    encounter_started/completed, race_progress, save_progress persistence,
//    definition_hash stability, localized string formatting, ClubMedalStats).
// 2) With SHIFT_STATE_ROOT: verifies 1,145 / 1,145 challenges across all 218
//    cars parse as supported with 0 missing locations.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "drive/save.h"
#include "fh1data/archive.h"
#include "fh1data/stringtables.h"
#include "freeroam/club.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

const char* kLocs =
    "\xEF\xBB\xBF<NamedTransforms>\n"
    "  <NamedTransform name='here'><Transform pos.x='0' pos.y='0' pos.z='0' radius='10'/></NamedTransform>\n"
    "  <NamedTransform name='there'><Transform pos.x='1000' pos.y='0' pos.z='0' radius=\"20\"/>"
    "<Transform pos.x='0' pos.y='0' pos.z='900'/></NamedTransform>\n"
    "</NamedTransforms>\n";

const char* kFile = R"(<car_challenges xmlns:xs="pg">
  <!-- 0: held speed -->
  <display description="IDS_A" title="IDS_TOP_SPEED"/>
  <group type="and">
    <pass_when><timer is="more_equal" value="10"/><hud progress="true"/></pass_when>
    <fail_when><speed is ="less_equal" value="90"/></fail_when>
  </group>
  <!-- 1: clean road trip -->
  <display description="IDS_B" title="IDS_CLEAN_ROAD_TRIP"/>
  <a_to_b start_loc="here" end_loc="there">
    <fail_when><timer is="more_equal" value="60"/></fail_when>
    <fail_when><collided/></fail_when>
    <hud activate="true"/>
  </a_to_b>
  <!-- 2: long jump -->
  <display description="IDS_C" title="IDS_LONG_JUMP"/>
  <group type="then">
    <pass_when><distance is="more_equal" value="15"/></pass_when>
    <pass_when><distance is="more_equal" value="30"/></pass_when>
    <fail_unless><in_air/></fail_unless>
  </group>
  <!-- 3: near misses (skills + save_progress) -->
  <display description="IDS_D" title="IDS_NEAR_MISSES"/>
  <pass_when pass_count="5" save_progress="true" desc_param0="pass_count">
    <skills type="nearmiss" desc_param1="type"/>
  </pass_when>
</car_challenges>)";

const char* kMatrixXml = R"(<car_challenges>
  <!-- 0: graded skill (Awesome or Ultimate burnout, plus typo max_grad ignored) -->
  <display title="IDS_AWESOME_BURNOUT" description="IDS_Skill_Perform1{0}Skill"/>
  <pass_when>
    <skills type="burnout" min_grade="1" max_grade="0" max_grad="0" desc_param0="type"/>
  </pass_when>

  <!-- 1: single skill chain: 2000 pts + 2 near misses without ending chain -->
  <display title="IDS_SKILL_CHAIN" description="IDS_ChainDesc"/>
  <group type="and">
    <pass_when><skill_points is="more_equal" value="2000" desc_param0="value"/></pass_when>
    <pass_when pass_count="2"><skills type="nearmiss"/></pass_when>
    <fail_when><num_skill_chains is="more_equal" value="1"/></fail_when>
  </group>

  <!-- 2: drift through named speed trap -->
  <display title="IDS_SPEED_TRAP_DRIFT" description="IDS_SpeedTrap_Speed{0}Trap{1}"/>
  <group type="and">
    <pass_when>
      <speed_camera type="trap" name="rt._29_speed_trap_230" min_speed="90" desc_param0="min_speed" desc_param1="name"/>
    </pass_when>
    <fail_unless><is_drifting/></fail_unless>
  </group>

  <!-- 3: speed zone between 100 and 110 mph -->
  <display title="IDS_SPEED_ZONE" description="IDS_SpeedZone_SpeedZoneBetween{0}and{1}on{2}"/>
  <pass_when>
    <speed_camera type="zone" name="i-76_speed_zone_101" min_speed="100" max_speed="110"
                  desc_param0="min_speed" desc_param1="max_speed" desc_param2="name"/>
  </pass_when>

  <!-- 4: combined 3 unique speed traps >= 300 mph -->
  <display title="IDS_SPEED_COMBINED" description="IDS_SpeedTrap_CombinedTraps{0}Speed{1}"/>
  <pass_when pass_count="3" desc_param0="pass_count">
    <speed_camera type="trap" min_speed="300" unique="true" desc_param1="min_speed"/>
  </pass_when>

  <!-- 5: 3 unique speed traps within 120 seconds -->
  <display title="IDS_SPEED_TRAPS" description="IDS_SpeedTrap_{0}NumInSeconds{1}"/>
  <pass_when pass_count="3" pass_interval="120" desc_param0="pass_count" desc_param1="pass_interval">
    <speed_camera type="trap" unique="true"/>
  </pass_when>

  <!-- 6: smash 3 fences within 10 s -->
  <display title="IDS_SMASH_FENCES" description="IDS_SmashFences"/>
  <pass_when pass_count="3" pass_interval="10">
    <smashables type="fence"/>
  </pass_when>

  <!-- 7: smash any 2 objects (save_progress) -->
  <display title="IDS_SMASH_ANY" description="IDS_SmashAny"/>
  <pass_when pass_count="2" save_progress="true">
    <smashables/>
  </pass_when>

  <!-- 8: street race at Carson hub, finish top 3, fail if career_event_style becomes none -->
  <display title="IDS_STREET_RACE_CARSON" description="IDS_StreetRaceCarson3rd"/>
  <group type="then">
    <pass_when><event_hub id="carson"/></pass_when>
    <pass_when><event_started/></pass_when>
    <group type="and">
      <pass_when><event_completed min_finish_place="3"/></pass_when>
      <fail_when><career_event_style id="none"/></fail_when>
    </group>
  </group>

  <!-- 9: wait 5s at start of event 209 (race_progress < 0), then top 3, fail on restart -->
  <display title="IDS_WAIT_WIN_EVENT" description="IDS_Wait{0}And3rdBetterEventID{1}" hint0="IDS_KettleHills"/>
  <group type="then">
    <pass_when><event_id is="equal_to" value="209" desc_param1="name"/></pass_when>
    <pass_when><event_started/></pass_when>
    <group type="and">
      <pass_when><timer is="more_equal" value="5" desc_param0="value"/></pass_when>
      <fail_when><race_progress is="more_equal" value="0" mode="furthest"/></fail_when>
    </group>
    <pass_when><event_completed min_finish_place="3"/></pass_when>
    <fail_when><event_restarted/></fail_when>
  </group>

  <!-- 10: clean head-to-head encounter win -->
  <display title="IDS_CLEAN_ENCOUNTER" description="IDS_RaceEncounterNoCrash"/>
  <group type="then">
    <pass_when><encounter_started/></pass_when>
    <group type="and">
      <pass_when><encounter_completed min_finish_place="1"/></pass_when>
      <fail_when><collided/></fail_when>
    </group>
  </group>
</car_challenges>)";

void put_utf16_entry(fh1::StringTable& t, const char* id, const char* ascii) {
  const uint16_t k = fh1::string_hash(id);
  std::u16string u;
  for (const char* p = ascii; *p; ++p) u.push_back(char16_t(uint8_t(*p)));
  auto it = std::lower_bound(t.keys.begin(), t.keys.end(), k);
  const size_t idx = size_t(it - t.keys.begin());
  if (it != t.keys.end() && *it == k) {
    t.text[idx] = std::move(u);
  } else {
    t.keys.insert(it, k);
    t.text.insert(t.text.begin() + std::ptrdiff_t(idx), std::move(u));
  }
}

}  // namespace

int main() {
  using namespace freeroam::club;
  Locations locs;
  std::string error;
  CHECK(parse_named_transforms(kLocs, locs, &error));
  if (!error.empty()) std::printf("club: %s\n", error.c_str());
  CHECK(locs.size() == 2 && locs["there"].points.size() == 2 && near(locs["there"].radius[1], kDefaultRadius));
  CHECK(locs["there"].inside(1015, 5) && !locs["there"].inside(1030, 0) && locs["there"].inside(3, 895));

  std::vector<Challenge> cs;
  CHECK(parse_challenges(kFile, cs, &error));
  if (!error.empty()) std::printf("club: %s\n", error.c_str());
  CHECK(cs.size() == 4);
  if (cs.size() != 4) return 1;
  CHECK(cs[0].supported && cs[1].supported && cs[2].supported && cs[3].supported);
  CHECK(cs[0].label() == "top speed");
  CHECK(cs[0].definition_hash != 0 && cs[0].definition_hash != cs[1].definition_hash);
  const double dt = 1.0 / 120;

  // Held speed: 95 mph, a dip to 80 at 5 s restarts it; 10 s more passes.
  {
    Run run(cs[0], locs);
    Signal s;
    double passed_at = -1;
    for (int k = 0; k < 120 * 20 && passed_at < 0; ++k) {
      s.time = k * dt;
      s.speed_mph = std::fabs(s.time - 5) < 0.05 ? 80 : 95;
      if (run.step(s)) passed_at = s.time;
    }
    CHECK(passed_at > 15.0 && passed_at < 15.2);
  }
  // Clean A-to-B trip: waits for A; crash fails once; clean drive passes.
  {
    Run run(cs[1], locs);
    Signal s;
    bool passed = false;
    double x = -50;
    int crashes = 0;
    for (int k = 0; k < 120 * 200 && !passed; ++k) {
      s.time = k * dt;
      x += 40 * dt;
      if (x > 1100) x = -50;
      s.x = x;
      s.collided = crashes == 0 && x > 500 && x < 501;
      crashes += s.collided;
      passed = run.step(s);
    }
    CHECK(passed && run.failures() == 1 && crashes == 1);
  }
  // Long jump sequence.
  {
    Run run(cs[2], locs);
    Signal s;
    auto jump = [&](double length, double from) {
      for (int k = 0;; ++k) {
        s.time = from + k * dt;
        s.in_air = k > 0 && k * dt * 25 < length;
        s.jump_m = s.in_air ? k * dt * 25 : 0;
        if (run.step(s)) return true;
        if (k > 0 && !s.in_air) return false;
      }
    };
    CHECK(!jump(20, 0));
    CHECK(run.failures() == 1);
    CHECK(jump(35, 10));
  }
  // Near misses with save_progress export/import + DriveSave text round-trip.
  {
    Run run(cs[3], locs);
    CHECK(run.has_save_progress());
    Signal s;
    s.time = 1.0;
    s.skills_awarded = {{"nearmiss", 3}, {"nearmiss", 2}};  // 2 on the same step!
    CHECK(!run.step(s));
    const auto saved_counts = run.export_save_progress();
    CHECK(saved_counts.size() == 1 && saved_counts[0] == 2);

    drive::DriveSave ds;
    ds.car = "SUB_ImprezaWRX_05";
    drive::SavedClubCar sc;
    sc.car_id = 1234;
    sc.challenges = {{cs[3].definition_hash, false}};
    sc.progress = {saved_counts};
    ds.club.push_back(sc);
    const std::string text = drive::save_to_text(ds);
    drive::DriveSave parsed;
    std::string save_err;
    CHECK(drive::save_from_text(text, parsed, &save_err));
    CHECK(parsed.club.size() == 1 && parsed.club[0].progress.size() == 1 &&
          parsed.club[0].progress[0].size() == 1 && parsed.club[0].progress[0][0] == 2);

    // Restore into a fresh Run and finish the remaining 3 near misses.
    Run run2(cs[3], locs);
    run2.import_save_progress(parsed.club[0].progress[0]);
    for (int i = 0; i < 3; ++i) {
      Signal s2;
      s2.time = 2.0 + i;
      s2.skills_awarded = {{"nearmiss", 1}};
      const bool done = run2.step(s2);
      CHECK(done == (i == 2));
    }
    CHECK(run2.passed());
  }

  // --- Full synthetic condition matrix ---
  std::vector<Challenge> mx;
  CHECK(parse_challenges(kMatrixXml, mx, &error));
  if (!error.empty()) std::printf("club matrix: %s\n", error.c_str());
  CHECK(mx.size() == 11);
  for (const auto& c : mx) CHECK(c.supported);

  // [0] Graded skill: Great (2) burnout rejected; Awesome (1) accepted.
  {
    CHECK(skill_tracker_grade_to_club_grade(4) == 0);
    CHECK(skill_tracker_grade_to_club_grade(3) == 1);
    CHECK(skill_tracker_grade_to_club_grade(2) == 2);
    CHECK(skill_tracker_grade_to_club_grade(1) == 3);
    Run r(mx[0], locs);
    Signal s;
    s.time = 1.0;
    s.skills_awarded = {{"burnout", 2}};  // Great: too low
    CHECK(!r.step(s));
    s.time = 2.0;
    s.skills_awarded = {{"drift", 0}};  // wrong skill
    CHECK(!r.step(s));
    s.time = 3.0;
    s.skills_awarded = {{"burnout", 1}};  // Awesome: passes!
    CHECK(r.step(s));
  }

  // [1] Skill chain: 1 near miss + chain ends resets; then 2 near misses + 2000 pts in 1 chain passes.
  {
    Run r(mx[1], locs);
    Signal s;
    s.time = 1.0;
    s.skills_awarded = {{"nearmiss", 3}};
    s.skill_points = 500;
    CHECK(!r.step(s));
    // Chain ends -> fails and resets!
    s.time = 2.0;
    s.skills_awarded.clear();
    s.skill_points = 0;
    s.skill_chain_completed = true;
    CHECK(!r.step(s));
    CHECK(r.failures() == 1);
    // New chain: 2 near misses + 2100 pts -> pass!
    s.skill_chain_completed = false;
    s.time = 3.0;
    s.skills_awarded = {{"nearmiss", 3}};
    s.skill_points = 1200;
    CHECK(!r.step(s));
    s.time = 4.0;
    s.skills_awarded = {{"nearmiss", 2}};
    s.skill_points = 2100;
    CHECK(r.step(s));
  }

  // [2] Drift through named speed trap 230 >= 90 mph.
  {
    Run r(mx[2], locs);
    Signal s;
    s.time = 1.0;
    s.is_drifting = false;
    s.speed_cameras = {{false, 230, "speed_camera_30", 105.0}};
    CHECK(!r.step(s));  // not drifting!
    s.time = 2.0;
    s.is_drifting = true;
    s.speed_cameras = {{false, 215, "speed_camera_15", 105.0}};
    CHECK(!r.step(s));  // wrong trap!
    s.time = 3.0;
    s.is_drifting = true;
    s.speed_cameras = {{false, 230, "speed_camera_30", 95.0}};
    CHECK(r.step(s));  // drifting + right trap + >=90 mph!
  }

  // [3] Speed zone between 100 and 110 mph on scoreboard 101.
  {
    Run r(mx[3], locs);
    Signal s;
    s.time = 1.0;
    s.speed_cameras = {{true, 101, "average_speed_01", 115.0}};  // too fast
    CHECK(!r.step(s));
    s.time = 2.0;
    s.speed_cameras = {{true, 101, "average_speed_01", 104.5}};  // in [100, 110]
    CHECK(r.step(s));
  }

  // [4] Combined 3 unique speed traps >= 300 mph.
  {
    Run r(mx[4], locs);
    Signal s;
    s.time = 1.0;
    s.speed_cameras = {{false, 201, "", 120.0}};
    CHECK(!r.step(s));
    // Repeat same trap 201 -> does NOT count as a second unique trap!
    s.time = 2.0;
    s.speed_cameras = {{false, 201, "", 130.0}};
    CHECK(!r.step(s));
    s.time = 3.0;
    s.speed_cameras = {{false, 202, "", 80.0}};
    CHECK(!r.step(s));
    // 3rd unique trap brings sum to 130 + 80 + 70 = 280 (< 300): still running.
    s.time = 4.0;
    s.speed_cameras = {{false, 203, "", 70.0}};
    CHECK(!r.step(s));
    // 4th unique trap at 110 replaces lowest (70): 130 + 80 + 110 = 320 >= 300 -> PASS!
    s.time = 5.0;
    s.speed_cameras = {{false, 204, "", 110.0}};
    CHECK(r.step(s));
  }

  // [5] 3 unique speed traps within 120 s.
  {
    Run r(mx[5], locs);
    Signal s;
    s.time = 10.0;
    s.speed_cameras = {{false, 201, "", 60.0}};
    CHECK(!r.step(s));
    s.time = 100.0;
    s.speed_cameras = {{false, 202, "", 60.0}};
    CHECK(!r.step(s));
    // At t=150, t=10 expired (>120s ago), so only 2 unique traps in window.
    s.time = 150.0;
    s.speed_cameras = {{false, 203, "", 60.0}};
    CHECK(!r.step(s));
    // At t=160, 202 (t=100), 203 (t=150), 204 (t=160) -> 3 unique within 120 s -> PASS!
    s.time = 160.0;
    s.speed_cameras = {{false, 204, "", 60.0}};
    CHECK(r.step(s));
  }

  // [6] & [7] Smashables (typed + interval, untyped, and classify_smashable_type).
  {
    CHECK(classify_smashable_type("co_clrd_fencef", -1) == "fence");
    CHECK(classify_smashable_type("flyer_012", 1) == "sign");
    CHECK(classify_smashable_type("co_lamp_pole_01", -1) == "pole");
    CHECK(classify_smashable_type("co_picnic_table", -1) == "picnic");
    CHECK(classify_smashable_type("co_traffic_cone", -1) == "cone");
    CHECK(classify_smashable_type("co_flower_pot", -1) == "flower");
    CHECK(classify_smashable_type("co_trash_bin", -1) == "bin");
    CHECK(classify_smashable_type("co_camera_tripod", -1) == "camera");

    Run r_fence(mx[6], locs);
    Signal s;
    s.time = 1.0;
    s.smashables_broken = {"cone", "fence"};
    CHECK(!r_fence.step(s));
    s.time = 15.0;  // t=1 expired (>10s)
    s.smashables_broken = {"fence", "fence"};
    CHECK(!r_fence.step(s));
    s.time = 18.0;
    s.smashables_broken = {"fence"};
    CHECK(r_fence.step(s));

    Run r_any(mx[7], locs);
    Signal s2;
    s2.time = 1.0;
    s2.smashables_broken = {"generic"};
    CHECK(!r_any.step(s2));
    s2.time = 2.0;
    s2.smashables_broken = {"sign"};
    CHECK(r_any.step(s2));
  }

  // [8] Street race at Carson hub, top 3 finish, fail if career_event_style becomes none.
  {
    Run r(mx[8], locs);
    Signal s;
    s.time = 1.0;
    s.event_hub = "carson";
    s.career_event_style = "street";
    s.event_started = true;
    CHECK(!r.step(s));
    // Finish 2nd while in street race -> PASS!
    s.time = 30.0;
    s.event_started = false;
    s.event_completed = true;
    s.event_finish_place = 2;
    CHECK(r.step(s));
  }

  // [9] Wait 5 s at start of event 209 (race_progress < 0), then finish top 3, fail on restart.
  {
    Run r(mx[9], locs);
    Signal s;
    s.time = 0.0;
    s.event_id = 209;
    s.event_started = true;
    s.race_progress = -0.01;
    CHECK(!r.step(s));
    // Rolling at t=2s fails the wait!
    s.event_started = false;
    s.time = 2.0;
    s.race_progress = 0.05;
    CHECK(!r.step(s));
    CHECK(r.failures() == 1);
    // Restart properly: sit still for 5.1s, then finish 3rd.
    s.time = 10.0;
    s.event_started = true;
    s.race_progress = -0.01;
    CHECK(!r.step(s));
    s.event_started = false;
    s.time = 15.2;
    s.race_progress = -0.01;
    CHECK(!r.step(s));  // 5s hold passed! Now waiting for event_completed
    s.time = 100.0;
    s.race_progress = 1.0;
    s.event_completed = true;
    s.event_finish_place = 3;
    CHECK(r.step(s));
  }

  // [10] Clean head-to-head encounter win.
  {
    Run r(mx[10], locs);
    Signal s;
    s.time = 1.0;
    s.encounter_started = true;
    CHECK(!r.step(s));
    s.encounter_started = false;
    s.time = 10.0;
    s.collided = true;  // crash fails!
    CHECK(!r.step(s));
    CHECK(r.failures() == 1);
    s.collided = false;
    s.time = 20.0;
    s.encounter_started = true;
    CHECK(!r.step(s));
    s.encounter_started = false;
    s.time = 50.0;
    s.encounter_completed = true;
    s.encounter_finish_place = 1;
    CHECK(r.step(s));
  }

  // Localized string-table formatting ({0}, {1}, {2}) + ClubMedalStats.
  {
    fh1::StringTables st;
    fh1::StringTable titles, descs, transforms;
    put_utf16_entry(titles, "IDS_SPEED_ZONE", "SPEED ZONE");
    put_utf16_entry(descs, "IDS_SpeedZone_SpeedZoneBetween{0}and{1}on{2}",
                    "Average between {0} and {1} mph in the {2}");
    put_utf16_entry(transforms, "i-76_speed_zone_101", "I-76 Speed Zone");
    st.put("carchallengetitles", std::move(titles));
    st.put("carchallengedescriptions", std::move(descs));
    st.put("carchallengetransforms", std::move(transforms));
    CHECK(mx[3].format_title(&st) == "SPEED ZONE");
    CHECK(mx[3].format_description(&st) == "Average between 100 and 110 mph in the I-76 Speed Zone");

    std::map<int64_t, std::string> ev_names{{209, "Kettle Hills Circuit"}};
    fh1::StringTable descs2;
    put_utf16_entry(descs2, "IDS_Wait{0}And3rdBetterEventID{1}",
                    "Wait {0} seconds at the start and finish top 3 in {1}");
    st.put("carchallengedescriptions", std::move(descs2));
    CHECK(mx[9].format_description(&st, &ev_names) ==
          "Wait 5 seconds at the start and finish top 3 in Kettle Hills Circuit");

    // Verify raw internal OUTPOST_* / GASSTATION_* transform tokens clean up into display names (Phase 4).
    const char* kOutpostXml = R"(<car_challenges>
      <display title="IDS_OUTPOST_RUN" description="IDS_DriveInto{0}"/>
      <pass_when><location name="OUTPOST_REDROCK_NODE" desc_param0="name"/></pass_when>
    </car_challenges>)";
    std::vector<Challenge> oc;
    std::string oerr;
    CHECK(parse_challenges(kOutpostXml, oc, &oerr) && oc.size() == 1);
    if (oc.size() == 1) {
      fh1::StringTable d3;
      put_utf16_entry(d3, "IDS_DriveInto{0}", "Drive into {0}");
      st.put("carchallengedescriptions", std::move(d3));
      const std::string formatted = oc[0].format_description(&st);
      CHECK(formatted.find("OUTPOST_") == std::string::npos);
      CHECK(formatted.find("Red Rock") != std::string::npos);
      const std::string fallback = oc[0].summary();
      CHECK(fallback.find("OUTPOST_") == std::string::npos);
    }

    std::map<int64_t, CarMeta> meta{
        {1, {1, "Ford", "USA", 35000}},       {2, {2, "Ford", "USA", 45000}},
        {3, {3, "Ford", "USA", 55000}},       {4, {4, "Ford", "USA", 65000}},
        {5, {5, "Ford", "USA", 75000}},       {6, {6, "Aston Martin", "UK", 1200000}},
    };
    const auto stats = compute_medal_stats({{1, 2}, {2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 3}}, meta);
    CHECK(stats.total_medals == 9 && stats.cars_with_medal == 6 && stats.ford_cars == 5 &&
          stats.british_cars == 1 && stats.million_cr_cars == 1);
    CHECK(stats.ach_first_medal && stats.ach_multi_cars && stats.ach_ford && stats.ach_million_cr);
    CHECK(stats.achievements_unlocked == 4);
  }

  // Full real v4 1000 Club corpus: ALL 1,145 / 1,145 challenges across 218 cars supported!
  if (const char* state = std::getenv("SHIFT_STATE_ROOT")) {
    ClubData club;
    error.clear();
    CHECK(load_club(state, club, &error));
    if (!error.empty()) std::printf("club: %s\n", error.c_str());
    std::printf("club: package %s, %zu cars, %zu challenges, %zu measured, %zu locations\n",
                club.package.c_str(), club.cars.size(), club.challenges(), club.measured(), club.locations.size());
    CHECK(club.cars.size() == 218 && club.challenges() == 1145 && club.measured() == 1145);
    CHECK(club.locations.size() == 30);
    // Every location across all 1,145 challenges resolves and no challenge reports "not measured".
    size_t missing = 0;
    for (const auto& [id, list] : club.cars)
      for (const auto& c : list)
        if (!c.supported || Run(c, club.locations).status(Signal{}).rfind("not measured", 0) == 0) ++missing;
    CHECK(missing == 0);

    // Load v4 EN carchallenge* string tables and verify formatting resolves cleanly!
    std::string zerr;
    auto zip = fh1::Archive::open(std::filesystem::path(state) / "title-update-v4" / "media.zip", &zerr);
    if (zip) {
      fh1::StringTables st;
      for (const char* tbl : {"carchallengetitles", "carchallengedescriptions", "carchallengehints",
                              "carchallengetransforms", "carchallenges"}) {
        auto r = zip->read(std::string("stringtables/en/") + tbl + ".str");
        if (r.ok()) {
          fh1::StringTable t;
          std::string perr;
          if (fh1::parse_string_table(r.data, t, &perr)) st.put(tbl, std::move(t));
        }
      }
      CHECK(st.size() >= 4);
      size_t unresolved_placeholders = 0;
      size_t printed_samples = 0;
      for (const auto& [id, list] : club.cars) {
        for (const auto& c : list) {
          const std::string desc = c.format_description(&st);
          if (printed_samples < 5 && desc.find("mph") != std::string::npos) {
            std::printf("club sample: %s -> %s\n", c.format_title(&st).c_str(), desc.c_str());
            ++printed_samples;
          }
          if (desc.find('{') != std::string::npos) {
            ++unresolved_placeholders;
            if (unresolved_placeholders <= 10) {
              std::printf("club: unresolved placeholder in car %lld %s (%s): %s\n", static_cast<long long>(id),
                          c.title.c_str(), c.description.c_str(), desc.c_str());
            }
          }
        }
      }
      std::printf("club: localized string check across 1,145 challenges: %zu unresolved placeholders\n",
                  unresolved_placeholders);
      CHECK(unresolved_placeholders == 0);
    }
  }

  if (failures) return 1;
  std::printf("club_tests: all passed\n");
  return 0;
}
