// Career progression, skills/popularity, difficulty/mileage rewards, and save
// round-trip tests (synthetic always, plus real disc catalog when SHIFT_GAME_ROOT is set).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "data/event_restrictions.h"
#include "drive/save.h"
#include "freeroam/skills.h"
#include "race/career.h"
#include "race/event.h"
#include "race/rewards.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

void test_save_roundtrip() {
  drive::DriveSave s;
  s.car = "VW_Corrado_95";
  s.position = {-890.5, -8.25, -305.0};
  s.heading = 1.57;
  s.career_started = true;
  s.owned_garage = true;
  s.credits = 18500;
  s.total_winnings = 24000;
  s.wristband_points = 310;
  s.collected_wristband = 1;
  s.popularity_points = 45200.5;
  s.max_skills_score = 12500;
  s.max_skills_car = 309;
  s.fame_rewards_claimed = 2;
  s.difficulty = 2;

  const std::string text = drive::save_to_text(s);
  drive::DriveSave out;
  std::string err;
  CHECK(drive::save_from_text(text, out, &err));
  CHECK(out.career_started == true);
  CHECK(out.collected_wristband == 1);
  CHECK(std::fabs(out.popularity_points - 45200.5) < 0.01);
  CHECK(out.max_skills_score == 12500);
  CHECK(out.max_skills_car == 309);
  CHECK(out.fame_rewards_claimed == 2);
  CHECK(out.difficulty == 2);

  // Legacy save without new keys loads with clean defaults.
  const std::string legacy = "shift-drive-save 1\ncar NIS_SkylineGTRVSpecII_02\nposition 1 2 3\nheading 0.5\ncredits 5000 5000\nwristband 800\n";
  drive::DriveSave leg;
  CHECK(drive::save_from_text(legacy, leg, &err));
  CHECK(leg.collected_wristband == -1);
  CHECK(leg.popularity_points == 0.0);
  CHECK(leg.difficulty == 1);
}

void test_rewards_difficulty_mileage() {
  CHECK(race::difficulty_bonus_percent(0) == 0);
  CHECK(race::difficulty_bonus_percent(1) == 5);
  CHECK(race::difficulty_bonus_percent(2) == 15);
  CHECK(race::difficulty_bonus_percent(3) == 25);
  // PostRaceOptions MileageCredits (offline career modes): 5,000 Cr over 50 km, steps of 100.
  race::MileageCredits offline;
  offline.total_distance_m = 50000.0;
  offline.reward = 5000.0;
  offline.increment = 100;
  CHECK(race::mileage_bonus_credits(4000.0, offline) == 400);
  CHECK(race::mileage_bonus_credits(4900.0, offline) == 400);  // FR22's ~4.9 km: MILEAGE BONUS 400 Cr
  CHECK(race::mileage_bonus_credits(4999.0, offline) == 400);  // floored, not rounded
  CHECK(race::mileage_bonus_credits(5000.0, offline) == 500);
  CHECK(race::mileage_bonus_credits(99.0, offline) == 0);
  CHECK(race::mileage_bonus_credits(80000.0, offline) == 5000);  // capped at the reward
  CHECK(race::mileage_bonus_credits(4900.0, race::MileageCredits{}) == 0);  // no data, no bonus
  race::MileageCredits rivals;
  rivals.total_distance_m = 50000.0;
  rivals.reward = 20000.0;
  rivals.increment = 400;
  CHECK(race::mileage_bonus_credits(4900.0, rivals) == 1600);
  CHECK(race::mileage_bonus_credits(50000.0, rivals) == 20000);

  race::MileageCredits parsed;
  std::string perr;
  CHECK(race::parse_mileage_credits(
      "<PostRaceOptions>\n  <MileageXp totalDistance=\"10000.0\" reward=\"250.0\" increment=\"50\"/>\n"
      "  <MileageCredits totalDistance=\"50000.0\" reward=\"20000.0\" increment=\"400\"/>\n</PostRaceOptions>",
      parsed, &perr));
  CHECK(parsed.total_distance_m == 50000.0 && parsed.reward == 20000.0 && parsed.increment == 400);
  CHECK(!race::parse_mileage_credits("<PostRaceOptions/>", parsed, &perr));
  CHECK(!race::parse_mileage_credits("<MileageCredits totalDistance=\"50000\" reward=\"x\" increment=\"1\"/>", parsed,
                                     &perr));
  CHECK(race::post_race_mode(2, false) == race::PostRaceMode::kFestivalRace);
  CHECK(race::post_race_mode(4, false) == race::PostRaceMode::kStreetRace);
  CHECK(race::post_race_mode(11, false) == race::PostRaceMode::kNemesisRace);
  CHECK(race::post_race_mode(12, false) == race::PostRaceMode::kHeadline);
  CHECK(race::post_race_mode(7, false) == race::PostRaceMode::kExhibition);
  CHECK(race::post_race_mode(2, true) == race::PostRaceMode::kRivals);
  CHECK(race::post_race_options_member(race::PostRaceMode::kRivals) ==
        "PostRaceOptions/PostRaceOptions_Offline_Rivals.xml");

  race::WristbandTables wb;
  wb.points[0][1] = 100;
  wb.points[0][2] = 60;
  wb.levels = {{0, "Yellow"}, {220, "Green"}};

  race::RaceBonusInputs with_mileage;
  with_mileage.mileage = offline;
  race::CareerRecord rec;
  const auto a1 =
      race::award_race(rec, 26, 100, 2, 1, 0, 4000.0, 8, 1, true, 120.0, 55.0, 1460, wb, 30, 4000.0, with_mileage);
  CHECK(a1.base_credits == 4000);
  CHECK(a1.mileage_bonus == 400);
  CHECK(a1.difficulty_pct == 30);
  CHECK(a1.difficulty_bonus == 1320);
  CHECK(a1.credits == 5720);
  CHECK(a1.wristband_points == 100);
  CHECK(a1.prize_car == 1460);
  CHECK(rec.credits == 5720);
  CHECK(rec.prize_cars.size() == 1);

  // Winning again pays credits, 0 extra wristband pts, and does not re-award prize car.
  const auto a2 = race::award_race(rec, 26, 100, 2, 1, 0, 4000.0, 8, 1, true, 118.0, 56.0, 1460, wb, 0, 0.0);
  CHECK(a2.wristband_points == 0);
  CHECK(a2.prize_car == 0);
  CHECK(rec.prize_cars.size() == 1);
}

// RACE-05: the award of a finished race is held until the finish banner's
// Continue; a Restart (or Quit) from the banner pays nothing.
void test_rewards_paid_after_banner() {
  race::WristbandTables wb;
  wb.points[0][1] = 100;
  wb.levels = {{0, "Yellow"}, {220, "Green"}};
  race::RaceBonusInputs ex;
  ex.mileage = {50000.0, 5000.0, 100};
  race::RaceAwardCommit c;
  c.event_id = 26, c.race_id = 100, c.career_type = 2, c.place = 1, c.finished = true;
  c.race_time = 120.0, c.top_speed = 55.0;

  race::CareerRecord rec;
  rec.credits = 1000;
  const race::CareerRecord before = rec;
  auto compute = [&] {
    return race::compute_race_award(rec, 26, 2, 1, 0, 4000.0, 8, 1, true, 1460, wb, 0, 4900.0, ex);
  };
  const race::RaceAward a = compute();
  CHECK(a.credits == 4400 && a.wristband_points == 100 && a.prize_car == 1460 && a.first_completion);
  // Computing changes nothing.
  CHECK(rec.credits == before.credits && rec.event_places.empty() && rec.prize_cars.empty() && rec.num_races == 0);

  // Finish, then Restart from the banner: dropped, nothing paid.
  race::PendingRaceAward pending;
  pending.hold(a, c);
  CHECK(pending.held() && pending.award() && pending.award()->credits == 4400);
  pending.drop();
  CHECK(!pending.held() && !pending.award());
  // A dropped award is not settled: the campaign must not advance past the
  // race (a qualifier restarted from the banner stays the qualifier).
  CHECK(!pending.committed());
  CHECK(pending.commit(rec) == nullptr);
  CHECK(!pending.committed());
  CHECK(rec.credits == 1000 && rec.wristband_points == 0 && rec.event_places.empty() && rec.prize_cars.empty() &&
        rec.num_races == 0 && rec.total_winnings == 0);

  // The restarted race finishes and the player continues: paid once.
  pending.hold(compute(), c);
  CHECK(!pending.committed());
  const race::RaceAward* paid = pending.commit(rec);
  CHECK(paid && paid->credits == 4400 && pending.committed());
  CHECK(rec.credits == 5400 && rec.total_winnings == 4400 && rec.wristband_points == 100 && rec.num_races == 1 &&
        rec.num_victories == 1 && rec.event_places[26] == 1 && rec.race_places[100] == 1 &&
        rec.prize_cars.size() == 1);
  CHECK(pending.commit(rec) == nullptr);  // a second Continue pays nothing more
  CHECK(rec.credits == 5400 && rec.num_races == 1);

  // compute + commit is award_race.
  race::CareerRecord r2;
  r2.credits = 1000;
  const auto a2 = race::award_race(r2, 26, 100, 2, 1, 0, 4000.0, 8, 1, true, 120.0, 55.0, 1460, wb, 0, 4900.0, ex);
  CHECK(a2.credits == 4400 && r2.credits == 5400 && r2.num_races == 1 && r2.prize_cars.size() == 1 &&
        r2.wristband_points == 100 && r2.time_in_festival == 120.0);
}

void test_career_progression_rules() {
  race::CareerCatalog cat;
  cat.wristbands = {
      {0, 0, "", "Yellow", 0, 0},
      {1, 220, "", "Green", 0, 424},
      {2, 750, "", "Blue", 0, 432},
  };
  cat.hubs = {{1, "", 310, {57}, true, {0, 0, 0}, 24.0}};

  race::CareerEvent fest_y;
  fest_y.event_id = 26, fest_y.horizon_id = "FR02", fest_y.level = 0, fest_y.style = 1, fest_y.unlock_points_req = 0;
  fest_y.target_class = 4;

  race::CareerEvent fest_g;
  fest_g.event_id = 30, fest_g.horizon_id = "FR10", fest_g.level = 1, fest_g.style = 1, fest_g.unlock_points_req = 220;
  fest_g.target_class = 3;
  // Require FWD drivetrain in bucket 0.
  shift::data::RestrictionSet rset;
  rset.drive_type = shift::data::kDriveFwd;
  fest_g.restrictions.buckets[0] = rset;

  race::CareerEvent showcase;
  showcase.event_id = 101, showcase.horizon_id = "EXHIBITION_1", showcase.style = 3, showcase.popularity_rank_req = 225;
  showcase.player_car_id = 404;

  race::CareerEvent street;
  street.event_id = 57, street.horizon_id = "STREET_1", street.style = 2, street.hub_id = 1, street.unlock_points_req = 310;

  race::CareerEvent nemesis_prereq;
  nemesis_prereq.event_id = 167, nemesis_prereq.horizon_id = "NEM_PRE", nemesis_prereq.level = 0, nemesis_prereq.style = 5;

  race::CareerEvent nemesis;
  nemesis.event_id = 169, nemesis.horizon_id = "NEM_FINAL", nemesis.level = 0, nemesis.style = 5, nemesis.prereq_event_id = 167;

  cat.events = {fest_y, fest_g, showcase, street, nemesis_prereq, nemesis};

  race::CarSpec corrado;
  corrado.car_id = 309, corrado.media_name = "VW_Corrado_95", corrado.stock_class = 2;
  corrado.props.drive_type = shift::data::kDriveFwd;
  race::CarSpec skyline;
  skyline.car_id = 1219, skyline.media_name = "NIS_Skyline", skyline.stock_class = 5;
  skyline.props.drive_type = shift::data::kDriveAwd;
  cat.cars[309] = corrado;
  cat.cars[1219] = skyline;

  race::CareerRecord rec;
  rec.wristband_points = 0;
  int collected = 0;

  CHECK(cat.earned_wristband(0) == 0);
  CHECK(cat.pending_wristband(0, collected) == -1);
  CHECK(cat.is_event_unlocked(fest_y, rec, collected, 250));
  CHECK(!cat.is_event_unlocked(fest_g, rec, collected, 250));
  CHECK(!cat.is_event_unlocked(showcase, rec, collected, 250));
  CHECK(!cat.is_event_unlocked(street, rec, collected, 250));
  CHECK(!cat.is_event_unlocked(nemesis, rec, collected, 250));

  // Earn 220 pts -> Green wristband is PENDING at Race Central, Green events still wait for collection!
  rec.wristband_points = 220;
  CHECK(cat.earned_wristband(220) == 1);
  CHECK(cat.pending_wristband(220, collected) == 1);
  CHECK(!cat.is_event_unlocked(fest_g, rec, collected, 250));

  // Collect Green wristband at Race Central -> Green events unlock!
  collected = 1;
  CHECK(cat.pending_wristband(220, collected) == -1);
  CHECK(cat.is_event_unlocked(fest_g, rec, collected, 250));

  // Popularity rank 225 unlocks showcase; 310 pts unlocks Street Hub 1; winning 167 unlocks 169.
  CHECK(cat.is_event_unlocked(showcase, rec, collected, 225));
  rec.wristband_points = 310;
  CHECK(cat.is_event_unlocked(street, rec, collected, 225));
  rec.event_places[167] = 1;
  CHECK(cat.is_event_unlocked(nemesis, rec, collected, 225));

  // P0-K1: STREET_PLNS_007 (Event ID 92) has prereq_event_id = 91 which does not exist in
  // catalog.events; missing prereq must be treated as satisfied so Event 92 is never deadlocked.
  race::CareerEvent street92;
  street92.event_id = 92;
  street92.horizon_id = "STREET_PLNS_007";
  street92.style = 2;
  street92.hub_id = 1;
  street92.unlock_points_req = 310;
  street92.prereq_event_id = 91;  // absent from cat.events
  cat.events.push_back(street92);
  CHECK(cat.find_event_by_id(91) == nullptr);
  CHECK(cat.is_event_unlocked(street92, rec, collected, 225));

  // Existing prereq (Event 92 IS in cat.events) still gates a dependent event until won.
  race::CareerEvent street93 = street92;
  street93.event_id = 93;
  street93.horizon_id = "STREET_PLNS_008";
  street93.prereq_event_id = 92;
  cat.events.push_back(street93);
  CHECK(!cat.is_event_unlocked(street93, rec, collected, 225));
  rec.event_places[92] = 1;
  CHECK(cat.is_event_unlocked(street93, rec, collected, 225));

  // Eligibility: Corrado (D, FWD) passes fest_g (C, FWD); Skyline (A, AWD) fails class; Showcase uses loaner!
  const std::vector<std::string> classes = {"F", "E", "D", "C", "B", "A", "S"};
  CHECK(cat.check_eligibility(fest_g, 309, 2, classes).eligible);
  CHECK(!cat.check_eligibility(fest_g, 1219, 5, classes).eligible);
  CHECK(!cat.check_eligibility(fest_g, 1219, 3, classes).eligible);  // AWD fails FWD restriction
  const auto sh_elig = cat.check_eligibility(showcase, 1219, 5, classes);
  CHECK(sh_elig.eligible && sh_elig.uses_loaner && sh_elig.loaner_car_id == 404);
}

void test_skills_and_popularity() {
  const char* fame_xml = R"(
    <Fame>
      <Level rank="249" target="1000"/>
      <Level rank="225" target="1700"/>
      <Level rank="200" target="6000"/>
    </Fame>)";
  const char* feats_xml = R"(
    <RaceFeats WorldCollisionThreshold="12" CarCollisionThreshold="12" MinSpeedToCancelFeats="10" DelayAfterCancelling="3">
      <Air RetriggerDelay="0"><Grade LocString="IDS_Air4" Time="0.8" Fame="500"/><Grade LocString="IDS_Air1" Time="0.2" Fame="100"/></Air>
      <Drift RetriggerDelay="0" MinAngle="15" MaxAngle="110" MinSpeed="25"><Grade LocString="IDS_Drift4" Distance="100" Fame="1000"/><Grade LocString="IDS_Drift1" Distance="10" Fame="100"/></Drift>
      <NearMiss RetriggerDelay="0" MinSpeed="50" MaxDistance="4"><Grade LocString="IDS_NM1" Speed="50" Fame="100"/></NearMiss>
      <Pass RetriggerDelay="0"><Grade LocString="IDS_Pass1" SpeedDiff="0" Fame="100"/></Pass>
      <Destruction><Grade LocString="IDS_D1" Objects="1" Fame="100"/></Destruction>
    </RaceFeats>)";
  const char* sponsors_xml = R"(
    <SkillChallenges>
      <Challenge id="Fame" type="FameRankChallenge">
        <Rank levelRequired="225" credits="5000"/>
        <Rank levelRequired="200" credits="10000"/>
      </Challenge>
    </SkillChallenges>)";

  freeroam::SkillsData data;
  std::string err;
  CHECK(freeroam::parse_skills_data(fame_xml, feats_xml, sponsors_xml, data, &err));
  CHECK(data.rank_for_points(0) == 250);
  CHECK(data.rank_for_points(999) == 250);
  CHECK(data.rank_for_points(1000) == 249);
  CHECK(data.rank_for_points(2500) == 225);
  CHECK(data.fame_tiers.size() == 2);

  freeroam::SkillTracker tr(data, 0.0, 0, 0);
  double t = 0.0;
  const double dt = 0.1;

  // 1. Trigger an Air skill (0.9 s -> Grade 4 = 500 pts).
  for (int i = 0; i < 9; ++i) {
    freeroam::SkillInput in;
    in.dt = dt, in.time = (t += dt), in.speed_ms = 30.0, in.forward_speed_ms = 30.0, in.wheels_on_ground = 0;
    tr.step(in);
  }
  {
    freeroam::SkillInput land;
    land.dt = dt, land.time = (t += dt), land.speed_ms = 30.0, land.forward_speed_ms = 30.0, land.wheels_on_ground = 4;
    const auto evs = tr.step(land);
    CHECK(!evs.empty() && evs[0].kind == freeroam::SkillEvent::kSkillAwarded && evs[0].points == 500);
  }
  // 2. Trigger a Near Miss right after -> Stuntman combo (+1000 pts). Two single skills (the combo does
  // not count) -> X1.1.
  {
    freeroam::SkillInput nm;
    nm.dt = dt, nm.time = (t += dt), nm.speed_ms = 30.0, nm.forward_speed_ms = 30.0, nm.wheels_on_ground = 4;
    nm.nearest_car_dist = 2.5;
    const auto evs = tr.step(nm);
    CHECK(evs.size() >= 2);  // NearMiss + Stuntman combo
    CHECK(tr.hud().multiplier == 1 && std::fabs(tr.hud().multiplier_f - 1.1f) < 1e-5f);
    CHECK(tr.hud().chain_points == 1600);
  }
  // 3. Drive cleanly for 3.2 s -> banks trunc(1600 * 1.1) = 1760 pts -> crosses rank 249 AND 225 -> 5,000 Cr.
  bool banked = false, rank_up = false, sponsor = false;
  int64_t sponsor_cr = 0;
  for (int i = 0; i < 35; ++i) {
    freeroam::SkillInput in;
    in.dt = dt, in.time = (t += dt), in.speed_ms = 25.0, in.forward_speed_ms = 25.0, in.wheels_on_ground = 4;
    for (const auto& ev : tr.step(in)) {
      if (ev.kind == freeroam::SkillEvent::kChainBanked) banked = true;
      if (ev.kind == freeroam::SkillEvent::kRankUp) rank_up = true;
      if (ev.kind == freeroam::SkillEvent::kFameSponsor) sponsor = true, sponsor_cr += ev.credits;
    }
  }
  CHECK(banked);
  CHECK(rank_up);
  CHECK(sponsor && sponsor_cr == 5000);
  CHECK(tr.popularity_rank() == 225);
  CHECK(tr.max_skills_score() == 1760);

  // 4. Hard crash cancels active chain and locks out for 3 s.
  {
    freeroam::SkillInput sm;
    sm.dt = dt, sm.time = (t += dt), sm.speed_ms = 25.0, sm.forward_speed_ms = 25.0, sm.smashed_object = true;
    tr.step(sm);
    CHECK(tr.hud().active);
    freeroam::SkillInput hit;
    hit.dt = dt, hit.time = (t += dt), hit.speed_ms = 10.0, hit.world_impact_ms = 15.0;
    const auto evs = tr.step(hit);
    CHECK(!evs.empty() && evs[0].kind == freeroam::SkillEvent::kChainLost);
    CHECK(!tr.hud().active);
    // Immediate smash during 3 s lockout is ignored.
    sm.time = (t += dt);
    tr.step(sm);
    CHECK(!tr.hud().active);
  }
}

void test_skills_reset_session_clock() {
  const char* fame_xml = R"(
    <Fame>
      <Level rank="249" target="100"/>
    </Fame>)";
  const char* feats_xml = R"(
    <RaceFeats WorldCollisionThreshold="12" CarCollisionThreshold="12" MinSpeedToCancelFeats="10" DelayAfterCancelling="3">
      <Drift RetriggerDelay="5" MinAngle="15" MaxAngle="110" MinSpeed="25"><Grade LocString="IDS_Drift1" Distance="10" Fame="100"/></Drift>
      <EDrift RetriggerDelay="5" MinAngle="15" MaxAngle="110" MinSpeed="25"><Grade LocString="IDS_EDrift1" Distance="10" Fame="150"/></EDrift>
      <NearMiss RetriggerDelay="5" MinSpeed="50" MaxDistance="4"><Grade LocString="IDS_NM1" Speed="50" Fame="100"/></NearMiss>
      <Destruction><Grade LocString="IDS_D1" Objects="1" Fame="100"/></Destruction>
    </RaceFeats>)";
  const char* sponsors_xml = R"(<SkillChallenges/>)";

  freeroam::SkillsData data;
  std::string err;
  CHECK(freeroam::parse_skills_data(fame_xml, feats_xml, sponsors_xml, data, &err));

  freeroam::SkillTracker tr(data, 0.0, 0, 0);
  // Trigger Near Miss (sets near_miss_cd_ >= 65.0) and crash (sets lockout_until_ = 63.0) at t = 60.0 s.
  {
    freeroam::SkillInput nm;
    nm.dt = 0.1, nm.time = 60.0, nm.speed_ms = 30.0, nm.forward_speed_ms = 30.0, nm.nearest_car_dist = 2.5;
    const auto evs = tr.step(nm);
    CHECK(!evs.empty() && evs[0].kind == freeroam::SkillEvent::kSkillAwarded);
    CHECK(tr.hud().active);

    freeroam::SkillInput hit;
    hit.dt = 0.1, hit.time = 60.1, hit.speed_ms = 10.0, hit.world_impact_ms = 15.0;
    tr.step(hit);
    CHECK(!tr.hud().active);
  }

  // Reloading resets vehicle.time() back to 0: reset_session_clock() must clear lockouts & cooldowns.
  tr.reset_session_clock();
  CHECK(!tr.hud().active);
  CHECK(tr.hud().chain_points == 0);
  CHECK(tr.hud().banked_total == 0);
  CHECK(tr.hud().lost_at < -1e8);
  CHECK(tr.hud().last_skill.empty());

  // At t = 0.1 s, Near Miss awards immediately and banks cleanly after 3.2 s.
  {
    freeroam::SkillInput nm;
    nm.dt = 0.1, nm.time = 0.1, nm.speed_ms = 30.0, nm.forward_speed_ms = 30.0, nm.nearest_car_dist = 2.5;
    const auto evs = tr.step(nm);
    CHECK(!evs.empty() && evs[0].kind == freeroam::SkillEvent::kSkillAwarded);
    CHECK(tr.hud().active && tr.hud().chain_points == 100);
  }
  bool banked = false;
  for (int i = 1; i <= 35; ++i) {
    freeroam::SkillInput in;
    in.dt = 0.1, in.time = 0.1 + 0.1 * i, in.speed_ms = 25.0, in.forward_speed_ms = 25.0;
    for (const auto& ev : tr.step(in))
      if (ev.kind == freeroam::SkillEvent::kChainBanked) banked = true;
  }
  CHECK(banked);
  CHECK(tr.popularity_points() >= 100.0);

  // EDrift requires >= 1.0 s handbrake (0.5 s yields DRIFT, 1.1 s yields E-DRIFT).
  tr.reset_session_clock();
  auto run_drift = [&](double hb_time, double start_t) -> std::string {
    std::string label;
    double tt = start_t;
    for (int i = 0; i < 12; ++i) {
      freeroam::SkillInput dr;
      dr.dt = 0.1, dr.time = (tt += 0.1), dr.forward_speed_ms = 20.0, dr.lateral_speed_ms = 10.0;
      dr.speed_ms = std::hypot(20.0, 10.0);
      dr.handbrake = (i * 0.1) < hb_time;
      tr.step(dr);
    }
    freeroam::SkillInput end;
    end.dt = 0.1, end.time = (tt += 0.1), end.forward_speed_ms = 25.0, end.speed_ms = 25.0;
    for (const auto& ev : tr.step(end))
      if (ev.kind == freeroam::SkillEvent::kSkillAwarded) label = ev.label;
    return label;
  };
  CHECK(run_drift(0.5, 1.0).find("E-DRIFT") == std::string::npos);
  tr.reset_session_clock();
  CHECK(run_drift(1.1, 1.0).find("E-DRIFT") != std::string::npos);
}

void test_real_disc_catalog() {
  const char* root = std::getenv("SHIFT_GAME_ROOT");
  if (!root || !*root) return;
  race::CareerCatalog cat;
  std::string err;
  CHECK(race::load_career_catalog(root, cat, &err));
  CHECK(cat.wristbands.size() == 7);
  CHECK(cat.hubs.size() == 3);
  CHECK(cat.sites.size() == 4);
  CHECK(cat.events.size() >= 90);
  const auto* fr02 = cat.find_event("FR02");
  const auto* fsec = cat.find_event("FESTIVAL_SECOND");
  CHECK(fr02 != nullptr && fr02->has_trigger);
  CHECK(fr02 != nullptr && fr02->has_tod_start && fr02->tod_start_sec > 0.0);
  CHECK(fsec != nullptr && fsec->has_trigger);
  {
    race::EventData ev_data;
    CHECK(race::load_event(root, "FR02", race::Difficulty::kMed, ev_data, &err));
    CHECK(ev_data.has_tod_start && ev_data.tod_start_sec > 0.0);
  }
  CHECK(!cat.make_names.empty() && !cat.country_names.empty() && !cat.engine_placement_names.empty());
  const std::vector<std::string> classes = {"F", "E", "D", "C", "B", "A", "S", "R3", "R2", "R1", "U"};
  // Starter Corrado VR6 (Data_Car 309, Class D = 2, FWD) is eligible for opening FESTIVAL_SECOND;
  // FR02 requires AWD (ineligible in FWD Corrado, eligible in AWD car of class <= B);
  // Showcase event 101 (Rank 250) is unlocked at start and supplies a loaner car!
  if (fsec) CHECK(cat.check_eligibility(*fsec, race::kStarterCarId, 2, classes).eligible);
  if (fr02) {
    CHECK(!cat.check_eligibility(*fr02, race::kStarterCarId, 2, classes).eligible);
    CHECK(cat.check_eligibility(*fr02, 1219, 4, classes).eligible);
  }
  const auto* sc101 = cat.find_event_by_id(101);
  CHECK(sc101 != nullptr);
  if (sc101) {
    race::CareerRecord empty_rec;
    CHECK(cat.is_event_unlocked(*sc101, empty_rec, 0, 250));
    CHECK(cat.check_eligibility(*sc101, race::kStarterCarId, 2, classes).uses_loaner);
  }

  freeroam::SkillsData sk;
  CHECK(freeroam::load_skills_data(root, sk, &err));
  CHECK(sk.fame_levels.size() == 249);
  CHECK(sk.rank_for_points(0) == 250);
  CHECK(sk.rank_for_points(15000) == 249);
  CHECK(sk.rank_for_points(2042782) == 1);
  CHECK(sk.fame_tiers.size() == 10);
  std::printf("career_tests: real disc catalog verified (%zu events, %zu sites, %zu fame levels)\n",
              cat.events.size(), cat.sites.size(), sk.fame_levels.size());

  if (const char* state = std::getenv("SHIFT_STATE_ROOT");
      state && *state && std::filesystem::exists(std::filesystem::path(state) / "title-update-v4" / "media.zip")) {
    race::CareerCatalog merged_cat;
    CHECK(race::load_career_catalog(root, state, nullptr, merged_cat, &err));
    CHECK(merged_cat.cars.size() >= 250);
    for (const auto& ev : merged_cat.events) CHECK(ev.career_type != 9);
    std::printf("career_tests: merged catalog verified (%zu cars, %zu Colorado events)\n", merged_cat.cars.size(),
                merged_cat.events.size());
  }
}

void test_r2_5_detailed_restrictions_and_fame_events() {
  race::CareerCatalog cat;
  cat.make_names[10] = "Ferrari";
  cat.country_names[5] = "Japan";
  cat.region_names[2] = "Europe";
  cat.body_family_names[3] = "Coupe";
  cat.model_family_names[7] = "Skyline";
  cat.engine_placement_names[2] = "Mid";

  race::CarSpec car;
  car.car_id = 309;
  car.media_name = "VW_Corrado_95";
  car.stock_class = 2;
  car.base_cost = 12000;
  car.engine_placement = 1;  // Front
  car.cylinder_count = 6;
  car.props.year = 1995;
  car.props.value = 12000;
  car.props.make = 1;
  car.props.country = 3;
  car.props.region = 3;
  car.props.body_family = 1;
  car.props.model_family = 1;
  car.props.drive_type = shift::data::kDriveFwd;
  car.props.rally_upgraded = false;
  cat.cars[309] = car;

  race::CarSpec rally_car = car;
  rally_car.car_id = 1626;
  rally_car.props.rally_upgraded = true;
  cat.cars[1626] = rally_car;

  const std::vector<std::string> classes = {"F", "E", "D", "C", "B", "A", "S"};
  auto check_reason = [&](const race::CareerEvent& ev, const char* must_contain) {
    const auto res = cat.check_eligibility(ev, 309, 2, classes);
    CHECK(!res.eligible);
    CHECK(res.reason != "CAR DOES NOT MEET EVENT RESTRICTIONS");
    CHECK(res.reason.find(must_contain) != std::string::npos);
  };

  // Make restriction
  {
    race::CareerEvent ev;
    ev.restrictions.buckets[0].make = 10;
    check_reason(ev, "FERRARI");
  }
  // Country restriction
  {
    race::CareerEvent ev;
    ev.restrictions.buckets[0].country = 5;
    check_reason(ev, "JAPAN");
  }
  // Region restriction
  {
    race::CareerEvent ev;
    ev.restrictions.buckets[0].region = 2;
    check_reason(ev, "EUROPE");
  }
  // Body Family restriction
  {
    race::CareerEvent ev;
    ev.restrictions.buckets[0].body_family = 3;
    check_reason(ev, "COUPE");
  }
  // Model Family restriction
  {
    race::CareerEvent ev;
    ev.restrictions.buckets[0].model_family = 7;
    check_reason(ev, "SKYLINE");
  }
  // Engine Placement restriction
  {
    race::CareerEvent ev;
    ev.required_engine_placement = 2;
    check_reason(ev, "MID-ENGINE");
  }
  // Cylinder Count restriction
  {
    race::CareerEvent ev;
    ev.required_cylinders = 10;
    check_reason(ev, "10-CYLINDER");
  }
  // Min Value restriction
  {
    race::CareerEvent ev;
    ev.restrictions.buckets[0].value_min = 100000;
    check_reason(ev, "100000");
  }
  // Rally Upgrade restriction (fails stock car, passes via CarSpec.props.rally_upgraded OR car_rally_upgraded override)
  {
    race::CareerEvent ev;
    ev.restrictions.buckets[0].rally_upgraded = true;
    check_reason(ev, "RALLY");
    CHECK(cat.check_eligibility(ev, 1626, 2, classes).eligible);
    CHECK(cat.check_eligibility(ev, 309, 2, classes, false, true).eligible);
  }
  // Multi-restriction event reports the ACTUAL failing field (not an already-passing drivetrain!).
  {
    race::CareerEvent ev;
    ev.restrictions.buckets[0].drive_type = shift::data::kDriveFwd;  // passes (car is FWD)
    ev.restrictions.buckets[0].value_min = 50000;                    // fails (car is 12,000 CR)
    check_reason(ev, "50000");
  }

  // Fame.xml dialogueEvent & radioFestivalUpdate attributes parsed and surfaced on RankUp event.
  const char* fame_xml = R"(
    <Fame>
      <Level rank="249" target="1000" dialogueEvent="FameRank249"/>
      <Level rank="175" target="2000" dialogueEvent="FameRank175" radioFestivalUpdate="RadioPop175"/>
    </Fame>)";
  const char* feats_xml = R"(
    <RaceFeats WorldCollisionThreshold="12" CarCollisionThreshold="12" MinSpeedToCancelFeats="10" DelayAfterCancelling="3">
      <Air RetriggerDelay="0"><Grade LocString="IDS_Air4" Time="0.2" Fame="2500"/></Air>
    </RaceFeats>)";
  const char* sponsors_xml = R"(<SkillChallenges/>)";
  freeroam::SkillsData sd;
  std::string err;
  CHECK(freeroam::parse_skills_data(fame_xml, feats_xml, sponsors_xml, sd, &err));
  const auto* lv249 = sd.find_fame_level(249);
  const auto* lv175 = sd.find_fame_level(175);
  CHECK(lv249 != nullptr && lv249->dialogue_event == "FameRank249" && lv249->radio_festival_update.empty());
  CHECK(lv175 != nullptr && lv175->dialogue_event == "FameRank175" && lv175->radio_festival_update == "RadioPop175");
}

void test_r3_e1_street_hubs_and_buyins() {
  // R3-CAM-13: Progressive 3-hub tier unlocking + CR entry buy-ins.
  race::CareerCatalog cat;
  cat.hubs = {
      {1, "HUB_1", 310, {57, 80}, true, {0, 0, 0}, 24.0},
      {2, "HUB_2", 1700, {38, 53}, true, {100, 0, 0}, 24.0},
      {3, "HUB_3", 4500, {83, 97}, true, {200, 0, 0}, 24.0},
  };

  auto make_street = [](int64_t id, const char* hid, int hub, int pts, double cash) {
    race::CareerEvent ev;
    ev.event_id = id;
    ev.horizon_id = hid;
    ev.style = 2;
    ev.career_type = 4;
    ev.hub_id = hub;
    ev.unlock_points_req = pts;
    ev.cash_prize = cash;
    return ev;
  };
  cat.events = {
      make_street(57, "STREET_1A", 1, 310, 24000.0),
      make_street(80, "STREET_1B", 1, 310, 22000.0),
      make_street(38, "STREET_2A", 2, 1700, 35000.0),
      make_street(53, "STREET_2B", 2, 1700, 36000.0),
      make_street(83, "STREET_3A", 3, 4500, 50000.0),
      make_street(97, "STREET_3B", 3, 4500, 100000.0),
  };

  race::CareerRecord rec;
  rec.wristband_points = 300;
  CHECK(!cat.is_hub_unlocked(cat.hubs[0], rec, 0));  // below 310 pts

  rec.wristband_points = 5000;
  CHECK(!cat.is_hub_unlocked(cat.hubs[0], rec, -1));  // pre-Yellow Wristband locked
  CHECK(cat.is_hub_unlocked(cat.hubs[0], rec, 0));    // Hub 1 unlocks!
  CHECK(!cat.is_hub_unlocked(cat.hubs[1], rec, 0));   // Hub 2 STILL locked until Hub 1 completed!
  CHECK(!cat.is_hub_unlocked(cat.hubs[2], rec, 0));   // Hub 3 STILL locked until Hub 2 completed!
  CHECK(!cat.is_event_unlocked(cat.events[2], rec, 0, 250));
  CHECK(cat.hub_unlock_requirement(1) == 0);
  CHECK(cat.hub_unlock_requirement(2) == 2);
  CHECK(cat.hub_unlock_requirement(3) == 2);

  // Complete 1 Hub 1 race -> Hub 2 still requires 1 more Hub 1 race.
  rec.event_places[57] = 1;
  CHECK(cat.hub_events_completed(1, rec) == 1);
  CHECK(!cat.is_hub_unlocked(cat.hubs[1], rec, 0));

  // Complete 2nd Hub 1 race -> Hub 2 unlocks! Hub 3 still locked!
  rec.event_places[80] = 2;
  CHECK(cat.hub_events_completed(1, rec) == 2);
  CHECK(cat.is_hub_unlocked(cat.hubs[1], rec, 0));
  CHECK(cat.is_event_unlocked(cat.events[2], rec, 0, 250));
  CHECK(!cat.is_hub_unlocked(cat.hubs[2], rec, 0));

  // Hub 2 also requires its points threshold (1700).
  rec.wristband_points = 1200;
  CHECK(!cat.is_hub_unlocked(cat.hubs[1], rec, 0));
  rec.wristband_points = 5000;
  CHECK(cat.is_hub_unlocked(cat.hubs[1], rec, 0));

  // Complete both Hub 2 races -> Hub 3 unlocks!
  rec.event_places[38] = 1;
  rec.event_places[53] = 1;
  CHECK(cat.is_hub_unlocked(cat.hubs[2], rec, 0));
  CHECK(cat.is_event_unlocked(cat.events[4], rec, 0, 250));

  // CR entry buy-in fee validation & atomic deduction.
  race::CareerEvent fest;
  fest.style = 1;
  fest.cash_prize = 20000.0;
  CHECK(race::CareerCatalog::street_race_buy_in(fest) == 0);
  rec.credits = 0;
  int64_t fee = -1;
  CHECK(race::CareerCatalog::charge_street_race_buy_in(fest, rec, &fee) && fee == 0);

  // Hub 1 (24,000 CR prize -> 2,500 CR buy-in), Hub 2 (35,000 -> 3,500), Hub 3 (100,000 -> 10,000).
  CHECK(race::CareerCatalog::street_race_buy_in(cat.events[0]) == 2500);
  CHECK(race::CareerCatalog::street_race_buy_in(cat.events[2]) == 3500);
  CHECK(race::CareerCatalog::street_race_buy_in(cat.events[5]) == 10000);

  // Insufficient CR refuses entry and leaves balance untouched.
  rec.credits = 2000;
  std::string why;
  CHECK(!race::CareerCatalog::charge_street_race_buy_in(cat.events[0], rec, &fee, &why));
  CHECK(fee == 2500);
  CHECK(rec.credits == 2000);
  CHECK(why.find("2,500") != std::string::npos);

  // Sufficient CR deducts exact buy-in.
  rec.credits = 5000;
  why.clear();
  CHECK(race::CareerCatalog::charge_street_race_buy_in(cat.events[0], rec, &fee, &why));
  CHECK(fee == 2500 && rec.credits == 2500);
}

}  // namespace

// --corpus: the mileage rules from the player's gametunablesettings.zip
// (RACE-06). Exit 77 without SHIFT_GAME_ROOT.
int post_race_options_corpus() {
  const char* root = std::getenv("SHIFT_GAME_ROOT");
  if (!root || !*root) {
    std::printf("career_tests --corpus: SHIFT_GAME_ROOT not set, skipped\n");
    return 77;
  }
  race::PostRaceOptions o;
  std::string err;
  CHECK(race::load_post_race_options(root, o, &err));
  if (!err.empty()) std::printf("post race options: %s\n", err.c_str());
  for (size_t i = 0; i < size_t(race::PostRaceMode::kCount); ++i) {
    const auto mode = race::PostRaceMode(i);
    const auto& m = o.for_mode(mode);
    std::printf("%s: totalDistance %.1f reward %.1f increment %lld\n", race::post_race_options_member(mode).c_str(),
                m.total_distance_m, m.reward, (long long)m.increment);
    CHECK(m.total_distance_m == 50000.0);
    if (mode == race::PostRaceMode::kRivals) CHECK(m.reward == 20000.0 && m.increment == 400);
    else CHECK(m.reward == 5000.0 && m.increment == 100);
  }
  // The measured cash reward of FR22 (about 4.9 km driven): MILEAGE BONUS 400 Cr.
  const auto& fest = o.for_mode(race::post_race_mode(2, false));
  CHECK(race::mileage_bonus_credits(4900.0, fest) == 400);
  CHECK(race::mileage_bonus_credits(4900.0, o.for_mode(race::post_race_mode(2, true))) == 1600);
  std::printf("career_tests --corpus: %d failures\n", failures);
  return failures ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--corpus") return post_race_options_corpus();
  test_save_roundtrip();
  test_rewards_difficulty_mileage();
  test_rewards_paid_after_banner();
  test_career_progression_rules();
  test_skills_and_popularity();
  test_skills_reset_session_clock();
  test_r2_5_detailed_restrictions_and_fame_events();
  test_r3_e1_street_hubs_and_buyins();
  test_real_disc_catalog();
  std::printf("career_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}
