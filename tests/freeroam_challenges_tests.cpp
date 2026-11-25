// Free-roam (co-op) sponsor challenges (PROG-11; freeroam/freeroam_challenges.h)
// on synthetic XML; with --corpus, the disc's FreeRoamChallenges.xml and
// FreeRoamChallenges.str (SHIFT_GAME_ROOT; exit 77 without it, failure when
// it is set but the data does not load).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "drive/save.h"
#include "freeroam/freeroam_challenges.h"

namespace {

int failures = 0;

#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++failures;                                              \
    }                                                          \
  } while (0)

using freeroam::CarField;
using freeroam::ChallengeStepInput;
using freeroam::FreeRoamChallengeTracker;
using freeroam::FreeRoamChallengeType;

// The shipped file's shapes, cut down (one of each, plus a retired one).
const char* kXml = R"(<FreeRoamChallenges>
    <!--
    EXAMPLE RESTRICTIONS
        <Restriction type="SpeedRestriction"
                        min_speed="20"              // Min speed in mph
                        max_speed="150" />
    -->
    <Challenge type="DistanceDrivenFreeRoamChallenge" id="DriveFar"
               sponsorName="IDS_DriveFar_Sponsor_Name" sponsorLogo="Game:\Media\UI\Textures\Horizon\SponsorLogoTile\KrashMax.tga" challengeName="IDS_DriveFar_Challenge_Name" challengeDesc="IDS_DriveFar_Challenge_Desc" >
		<Reward credits="5000000" />
		<Options distance_to_drive="1000" />
		<Restrictions>
        </Restrictions>
	</Challenge>
    <Challenge type="DistanceDrivenFreeRoamChallenge" id="SlowFords"
               sponsorName="a" sponsorLogo="b" challengeName="c" challengeDesc="d" >
		<Reward credits="25000" />
		<Options distance_to_drive="100" />
		<Restrictions>
            <Restriction type="SpeedRestriction" max_speed="60" />
            <Restriction type="CarRestriction" MakeId="14" YearMax="1979" BaseCostMin="" />
        </Restrictions>
	</Challenge>
    <Challenge type="DriveFromAToBFreeRoamChallenge" id="NightTrip"
	           sponsorName="a" sponsorLogo="b" challengeName="c" challengeDesc="d" >
        <Reward credits="200000" />
		<Options start_location.x="-2358" start_location.y="19" start_location.z="-4981"
                 start_radius="20"
                 end_location.x="-4404" end_location.y="-9" end_location.z="1410"
                 end_radius="200"
                 min_players="2"
                 bidirectional="false" />
		<Restrictions>
            <Restriction type="TimeOfDayRestriction" start_hour="19" start_minute="10" end_hour="7" end_minute="50" />
            <Restriction type="CarRestriction" YearMax="1979"/>
        </Restrictions>
    </Challenge>
    <Challenge type="SpeedCameraFreeRoamChallenge" id="Trap"
	           sponsorName="a" sponsorLogo="b" challengeName="c" challengeDesc="d" >
        <Reward credits="50000" />
        <Options time_limit="10" num_cameras_to_flash="2" cameras_must_be_unique="false">
            <Camera camera_id="speed_camera_27" />
        </Options>
        <Restrictions>
            <Restriction type="SpeedRestriction" min_speed="150" />
        </Restrictions>
    </Challenge>
    <Challenge type="SkillsFreeRoamChallenge" id="NearMissChallenge"
	           sponsorName="a" sponsorLogo="b" challengeName="c" challengeDesc="d" >
        <Reward credits="25000" />
        <Options target_feat_count="3" feat_id="NearMiss" />
        <Restrictions>
        </Restrictions>
    </Challenge>
    <!-- Old distance challenges below

    <Challenge type="DistanceDrivenFreeRoamChallenge" id="Drive75MilesFords"
               sponsorName="a" sponsorLogo="b" challengeName="c" challengeDesc="d" >
		<Reward credits="25000" />
		<Options distance_to_drive="120701" />
		<Restrictions>
            <Restriction type="CarRestriction" MakeId="14" />
        </Restrictions>
	</Challenge>
    -->
</FreeRoamChallenges>
)";

freeroam::FreeRoamChallengesData parsed() {
  freeroam::FreeRoamChallengesData d;
  std::string error;
  const bool ok = freeroam::parse_free_roam_challenges(kXml, d, &error);
  if (!ok) std::printf("parse: %s\n", error.c_str());
  CHECK(ok);
  return d;
}

void test_parse() {
  const auto d = parsed();
  CHECK(d.challenges.size() == 6);
  CHECK(d.live_count() == 5);
  const auto* far = d.find("DriveFar");
  CHECK(far && far->type == FreeRoamChallengeType::kDistanceDriven && far->reward_credits == 5000000);
  CHECK(far && far->distance_to_drive_m == 1000 && far->restrictions.empty() && !far->retired);
  CHECK(far && far->challenge_name_key == "IDS_DriveFar_Challenge_Name" && far->challenge_name.empty());
  CHECK(far && far->logo_texture() == "SPONSORLOGOTILE/KRASHMAX");
  const auto* slow = d.find("SlowFords");
  CHECK(slow && slow->restrictions.size() == 2);
  if (slow && slow->restrictions.size() == 2) {
    const auto& sp = slow->restrictions[0];
    CHECK(sp.kind == freeroam::ChallengeRestriction::Kind::kSpeed && sp.min_mph == 0 && sp.max_mph == 60);
    const auto& car = slow->restrictions[1];
    CHECK(car.kind == freeroam::ChallengeRestriction::Kind::kCar);
    CHECK(car.car.size() == 2);  // MakeId and YearMax; the blank BaseCostMin is skipped
  }
  const auto* trip = d.find("NightTrip");
  CHECK(trip && trip->type == FreeRoamChallengeType::kDriveFromAToB && trip->min_players == 2 && !trip->bidirectional);
  CHECK(trip && trip->start_location[0] == -2358 && trip->end_location[2] == 1410 && trip->end_radius == 200);
  if (trip && !trip->restrictions.empty()) {
    CHECK(std::fabs(trip->restrictions[0].start_hour - (19 + 10 / 60.0)) < 1e-9);
    CHECK(std::fabs(trip->restrictions[0].end_hour - (7 + 50 / 60.0)) < 1e-9);
  }
  const auto* trap = d.find("Trap");
  CHECK(trap && trap->type == FreeRoamChallengeType::kSpeedCamera && trap->time_limit_s == 10);
  CHECK(trap && trap->num_cameras_to_flash == 2 && !trap->cameras_must_be_unique);
  CHECK(trap && trap->camera_ids.size() == 1 && trap->camera_ids[0] == "speed_camera_27");
  const auto* nm = d.find("NearMissChallenge");
  CHECK(nm && nm->type == FreeRoamChallengeType::kSkills && nm->feat_id == "NearMiss" && nm->target_feat_count == 3);
  const auto* old = d.find("Drive75MilesFords");
  CHECK(old && old->retired && old->reward_credits == 25000 && old->distance_to_drive_m == 120701);

  freeroam::FreeRoamChallengesData bad;
  std::string error;
  CHECK(!freeroam::parse_free_roam_challenges("<Other/>", bad, &error) && !error.empty());
  CHECK(!freeroam::parse_free_roam_challenges("<FreeRoamChallenges>", bad, &error));
}

void test_distance() {
  const auto d = parsed();
  FreeRoamChallengeTracker t(d);
  ChallengeStepInput in;
  in.distance_m = 400;
  in.speed_mph = 100;  // too fast for SlowFords, and its car is unknown
  CHECK(t.step(in).empty());
  CHECK(t.progress("DriveFar") == 400);
  CHECK(std::fabs(t.fraction("DriveFar") - 0.4) < 1e-12);
  CHECK(t.progress("SlowFords") == 0);
  in.distance_m = 599.5;
  CHECK(t.step(in).empty());
  in.distance_m = 0.5;
  const auto done = t.step(in);
  CHECK(done.size() == 1 && done[0].id == "DriveFar" && done[0].credits == 5000000 && done[0].def);
  CHECK(t.completed("DriveFar") && t.fraction("DriveFar") == 1);
  CHECK(t.step(in).empty());  // once only
  // The retired challenge (same distance shape) is never tracked.
  in.distance_m = 200000;
  for (const auto& c : t.step(in)) CHECK(c.id != "Drive75MilesFords");
  CHECK(t.progress("Drive75MilesFords") == 0 && !t.completed("Drive75MilesFords"));
  // A negative or non-finite step adds nothing.
  FreeRoamChallengeTracker u(d);
  in.distance_m = -5;
  u.step(in);
  in.distance_m = std::nan("");
  u.step(in);
  CHECK(u.progress("DriveFar") == 0);
}

void test_restrictions() {
  const auto d = parsed();
  // TimeOfDay wrapping midnight: [19:10, 07:50).
  const auto& tod = d.find("NightTrip")->restrictions[0];
  freeroam::ChallengeCar any;
  CHECK(tod.holds(19 + 10 / 60.0, 0, any));
  CHECK(tod.holds(23.5, 0, any) && tod.holds(0, 0, any) && tod.holds(7.8, 0, any));
  CHECK(!tod.holds(7 + 50 / 60.0, 0, any) && !tod.holds(12, 0, any) && !tod.holds(19.1, 0, any));
  freeroam::ChallengeRestriction day = tod;
  std::swap(day.start_hour, day.end_hour);  // [07:50, 19:10)
  CHECK(day.holds(12, 0, any) && day.holds(7 + 50 / 60.0, 0, any) && !day.holds(19 + 10 / 60.0, 0, any));
  CHECK(!day.holds(3, 0, any));
  // Speed: >= min, < max.
  freeroam::ChallengeRestriction sp;
  sp.kind = freeroam::ChallengeRestriction::Kind::kSpeed;
  sp.min_mph = 150;
  CHECK(sp.holds(0, 150, any) && sp.holds(0, 220, any) && !sp.holds(0, 149.9, any));
  sp.min_mph = 0;
  sp.max_mph = 60;
  CHECK(sp.holds(0, 0, any) && sp.holds(0, 59.9, any) && !sp.holds(0, 60, any));

  // SlowFords: a pre-1980 Ford under 60 mph.
  FreeRoamChallengeTracker t(d);
  ChallengeStepInput in;
  in.distance_m = 30;
  in.speed_mph = 40;
  in.car[CarField::kMake] = 14;
  in.car[CarField::kYear] = 1965;
  t.step(in);
  CHECK(t.progress("SlowFords") == 30);
  in.car[CarField::kYear] = 1980;  // too new
  t.step(in);
  in.car[CarField::kYear] = 1979;
  in.car[CarField::kMake] = 13;  // not a Ford
  t.step(in);
  in.car[CarField::kMake] = -1;  // unknown make
  t.step(in);
  in.car[CarField::kMake] = 14;
  in.speed_mph = 60;  // too fast
  t.step(in);
  CHECK(t.progress("SlowFords") == 30);
  in.speed_mph = 59;
  in.distance_m = 70;
  const auto done = t.step(in);
  bool slow_done = false;
  for (const auto& c : done) slow_done |= c.id == "SlowFords" && c.credits == 25000;
  CHECK(slow_done);

  // AllowUpgrades and inclusive bounds (BaseCostMax="9999": under 10,000 CR).
  freeroam::FreeRoamChallengesData cd;
  CHECK(freeroam::parse_free_roam_challenges(
      R"(<FreeRoamChallenges><Challenge type="DistanceDrivenFreeRoamChallenge" id="Cheap"><Reward credits="1"/>
         <Options distance_to_drive="10"/><Restrictions>
         <Restriction type="CarRestriction" BaseCostMax="9999" PIMin="100" PIMax="300" AllowUpgrades="false"/>
         </Restrictions></Challenge></FreeRoamChallenges>)",
      cd));
  const auto& car = cd.challenges.at(0).restrictions.at(0);
  freeroam::ChallengeCar c;
  c[CarField::kBaseCost] = 9999;
  c[CarField::kPi] = 300;
  CHECK(car.holds(0, 0, c));
  c[CarField::kBaseCost] = 10000;
  CHECK(!car.holds(0, 0, c));
  c[CarField::kBaseCost] = 0;
  c[CarField::kPi] = 99;
  CHECK(!car.holds(0, 0, c));
  c[CarField::kPi] = 100;
  CHECK(car.holds(0, 0, c));
  c.upgraded = true;
  CHECK(!car.holds(0, 0, c));
}

void test_skills() {
  const auto d = parsed();
  FreeRoamChallengeTracker t(d);
  ChallengeStepInput in;
  in.skills = {"Drift", "nearmiss", "TradingPaint"};
  CHECK(t.step(in).empty());
  CHECK(t.progress("NearMissChallenge") == 1);
  in.skills = {"NearMiss"};
  CHECK(t.step(in).empty());
  in.skills = {"NearMiss", "NearMiss"};
  const auto done = t.step(in);
  CHECK(done.size() == 1 && done[0].id == "NearMissChallenge" && done[0].credits == 25000);
  CHECK(t.progress("NearMissChallenge") == 4 && t.completed("NearMissChallenge"));
  CHECK(t.step(in).empty());
}

void test_speed_camera() {
  const auto d = parsed();
  // Solo: one player can never make the two flashes the challenge needs.
  FreeRoamChallengeTracker solo(d);
  ChallengeStepInput in;
  for (int i = 0; i < 60; ++i) {
    in.time_s = i * 2.0;
    in.flashes = {{"speed_camera_27", 180, 0}};
    CHECK(solo.step(in).empty());
  }
  CHECK(!solo.completed("Trap"));

  // Two players within 10 s.
  FreeRoamChallengeTracker t(d);
  in.time_s = 100;
  in.flashes = {{"SPEED_CAMERA_27", 170, 0}};
  CHECK(t.step(in).empty());
  in.time_s = 108;
  in.flashes = {{"speed_camera_27", 160, 3}};
  auto done = t.step(in);
  CHECK(done.size() == 1 && done[0].id == "Trap" && done[0].credits == 50000);

  // Too far apart, a slow local flash, another camera, a session player alone.
  FreeRoamChallengeTracker late(d);
  in.time_s = 100;
  in.flashes = {{"speed_camera_27", 170, 0}};
  late.step(in);
  in.time_s = 110.5;
  in.flashes = {{"speed_camera_27", 170, 2}};
  CHECK(late.step(in).empty());
  FreeRoamChallengeTracker slow(d);
  in.time_s = 0;
  in.flashes = {{"speed_camera_27", 149, 0}, {"speed_camera_27", 170, 1}};
  CHECK(slow.step(in).empty());
  in.flashes = {{"speed_camera_01", 170, 0}, {"speed_camera_27", 170, 1}};
  CHECK(slow.step(in).empty());
  in.flashes = {{"speed_camera_27", 170, 1}, {"speed_camera_27", 170, 2}};
  CHECK(slow.step(in).empty());  // the local player must be among them
  in.flashes = {{"speed_camera_27", 151, 0}};
  CHECK(slow.step(in).size() == 1);
}

void test_a_to_b() {
  const auto d = parsed();
  FreeRoamChallengeTracker t(d);
  ChallengeStepInput in;
  in.tod_hour = 23;
  in.car[CarField::kYear] = 1970;
  in.distance_m = 50000;
  in.speed_mph = 50;
  for (int i = 0; i < 10; ++i) {
    for (const auto& c : t.step(in)) CHECK(c.id != "NightTrip");
  }
  CHECK(!t.completed("NightTrip") && t.progress("NightTrip") == 0);
}

void test_active() {
  const auto d = parsed();
  FreeRoamChallengeTracker t(d);
  t.set_active("NearMissChallenge");
  ChallengeStepInput in;
  in.distance_m = 5000;
  in.skills = {"NearMiss"};
  CHECK(t.step(in).empty());
  CHECK(t.progress("DriveFar") == 0 && t.progress("NearMissChallenge") == 1);
}

void test_save() {
  const auto d = parsed();
  FreeRoamChallengeTracker t(d);
  ChallengeStepInput in;
  in.distance_m = 1000;
  in.skills = {"NearMiss", "NearMiss"};
  t.step(in);
  CHECK(t.completed("DriveFar") && t.progress("NearMissChallenge") == 2);

  // Through the drive save's text.
  drive::DriveSave s;
  s.car = "X";
  for (const auto& r : t.save()) s.freeroam_challenges.push_back({r.id, r.progress, r.completed});
  CHECK(s.freeroam_challenges.size() == 2);
  const std::string text = drive::save_to_text(s);
  CHECK(text.find("freeroam_challenge DriveFar 1000.000000 1\n") != std::string::npos);
  CHECK(text.find("freeroam_challenge NearMissChallenge 2.000000 0\n") != std::string::npos);
  drive::DriveSave back;
  std::string error;
  CHECK(drive::save_from_text(text, back, &error));
  CHECK(back.freeroam_challenges.size() == 2);
  std::vector<freeroam::ChallengeRecord> recs;
  for (const auto& fc : back.freeroam_challenges) recs.push_back({fc.id, fc.progress, fc.completed});
  FreeRoamChallengeTracker u(d);
  u.load(recs);
  CHECK(u.completed("DriveFar") && !u.completed("NearMissChallenge") && u.progress("NearMissChallenge") == 2);
  in.distance_m = 1000;
  in.skills = {"NearMiss"};
  const auto done = u.step(in);
  CHECK(done.size() == 1 && done[0].id == "NearMissChallenge");  // DriveFar is not paid twice

  // The big distance keeps its precision.
  drive::DriveSave big;
  big.car = "X";
  big.freeroam_challenges.push_back({"Drive8000Miles", 12874751.75, false});
  drive::DriveSave big_back;
  CHECK(drive::save_from_text(drive::save_to_text(big), big_back, &error));
  CHECK(big_back.freeroam_challenges.size() == 1 && big_back.freeroam_challenges[0].progress == 12874751.75);

  // An old save (no lines) still loads; bad lines are skipped; unknown ids are dropped on load.
  drive::DriveSave old;
  CHECK(drive::save_from_text("shift-drive-save 1\ncar X\nposition 1 2 3\nheading 0.5\nsponsor Air 3 1\n", old, &error));
  CHECK(old.freeroam_challenges.empty() && old.sponsors.size() == 1);
  drive::DriveSave junk;
  CHECK(drive::save_from_text(
      "shift-drive-save 1\ncar X\nposition 1 2 3\nheading 0.5\nfreeroam_challenge A -1 0\nfreeroam_challenge B\n"
      "freeroam_challenge C nan 1\n",
      junk, &error));
  CHECK(junk.freeroam_challenges.empty());
  FreeRoamChallengeTracker w(d);
  w.load({{"Gone", 5, true}, {"DriveFar", 10, false}});
  CHECK(w.completed_count() == 0 && w.progress("DriveFar") == 10 && w.save().size() == 1);
}

int corpus() {
  const char* root = std::getenv("SHIFT_GAME_ROOT");
  if (!root) {
    std::printf("freeroam_challenges corpus: SHIFT_GAME_ROOT not set, skipped\n");
    return 77;
  }
  freeroam::FreeRoamChallengesData d;
  std::string error;
  if (!freeroam::load_free_roam_challenges(root, d, &error)) {
    std::printf("FAIL: FreeRoamChallenges.xml did not load from %s: %s\n", root, error.c_str());
    return 1;
  }
  // The authored set (live and retired) and the live one.
  std::map<FreeRoamChallengeType, int> all, live;
  std::map<int64_t, int> rewards_all, rewards_live;
  for (const auto& c : d.challenges) {
    ++all[c.type];
    ++rewards_all[c.reward_credits];
    if (c.retired) continue;
    ++live[c.type];
    ++rewards_live[c.reward_credits];
    CHECK(!c.challenge_name.empty() && !c.sponsor_name.empty() && !c.description.empty());
  }
  std::printf("free-roam challenges: %zu authored, %zu live\n", d.challenges.size(), d.live_count());
  CHECK(d.challenges.size() == 43);
  CHECK(all[FreeRoamChallengeType::kDistanceDriven] == 20 && all[FreeRoamChallengeType::kDriveFromAToB] == 14);
  CHECK(all[FreeRoamChallengeType::kSkills] == 3 && all[FreeRoamChallengeType::kSpeedCamera] == 6);
  CHECK(all[FreeRoamChallengeType::kUnknown] == 0);
  const std::map<int64_t, int> want_all = {{5000000, 1}, {1000000, 3}, {250000, 3},
                                           {200000, 5},  {50000, 9},   {25000, 22}};
  CHECK(rewards_all == want_all);
  CHECK(d.live_count() == 24);
  CHECK(live[FreeRoamChallengeType::kDistanceDriven] == 1 && live[FreeRoamChallengeType::kDriveFromAToB] == 14);
  CHECK(live[FreeRoamChallengeType::kSkills] == 3 && live[FreeRoamChallengeType::kSpeedCamera] == 6);
  const std::map<int64_t, int> want_live = {{5000000, 1}, {1000000, 3}, {250000, 3},
                                            {200000, 5},  {50000, 9},   {25000, 3}};
  CHECK(rewards_live == want_live);

  const auto* big = d.find("Drive8000Miles");
  CHECK(big && !big->retired && big->reward_credits == 5000000 && big->distance_to_drive_m == 12874752);
  CHECK(big && big->challenge_name == "ENDURANCE CHALLENGE" && big->sponsor_name == "KRASH MAX");
  CHECK(big && big->logo_texture() == "SPONSORLOGOTILE/KRASHMAX");
  int a_to_b_multi = 0, camera_multi = 0;
  for (const auto& c : d.challenges) {
    if (c.type == FreeRoamChallengeType::kDriveFromAToB && c.min_players >= 2) ++a_to_b_multi;
    if (c.type == FreeRoamChallengeType::kSpeedCamera && c.num_cameras_to_flash >= 2 && c.camera_ids.size() == 1)
      ++camera_multi;
    if (c.retired) CHECK(c.type == FreeRoamChallengeType::kDistanceDriven && c.reward_credits == 25000);
  }
  CHECK(a_to_b_multi == 14 && camera_multi == 6);
  const auto* night = d.find("RedFootRanchToCarsonWarehousesNighttime");
  CHECK(night && night->restrictions.size() == 2 && night->reward_credits == 200000);

  // Solo play: the endurance and skills challenges complete; the co-op trips
  // and speed cameras never do.
  FreeRoamChallengeTracker t(d);
  ChallengeStepInput in;
  in.speed_mph = 200;
  int64_t paid = 0;
  for (int hour = 0; hour < 24; ++hour) {
    in.tod_hour = hour + 0.5;
    in.time_s = hour * 3600.0;
    in.distance_m = 600000;
    in.skills = {"NearMiss", "TradingPaint", "Draft", "NearMiss", "TradingPaint", "Draft"};
    in.flashes.clear();
    for (const auto& c : d.challenges)
      for (const auto& cam : c.camera_ids) in.flashes.push_back({cam, 240, 0});
    for (const auto& c : t.step(in)) paid += c.credits;
  }
  for (int i = 0; i < 60; ++i) {
    in.skills = {"NearMiss", "TradingPaint", "Draft"};
    for (const auto& c : t.step(in)) paid += c.credits;
  }
  CHECK(t.completed("Drive8000Miles") && t.completed("NearMissChallenge") && t.completed("TradingPaintChallenge") &&
        t.completed("DraftingChallenge"));
  CHECK(t.completed_count() == 4);
  CHECK(paid == 5000000 + 3 * 25000);
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--corpus") == 0) {
    const int r = corpus();
    if (r == 0) std::printf("freeroam_challenges corpus: ok\n");
    return r;
  }
  test_parse();
  test_distance();
  test_restrictions();
  test_skills();
  test_speed_camera();
  test_a_to_b();
  test_active();
  test_save();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("freeroam_challenges_tests: ok\n");
  return 0;
}
