// The first-time career state machine, stepped headless through every stage
// by a fake host that plays cutscenes and movies for a fixed time, closes
// screens, drives the player through each stage's trigger zones and finishes
// the heats and the showcase; and the driving tutor's prompt sequence.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "race/driving_tutor.h"
#include "race/first_time_career.h"

namespace {

int g_failures = 0;

void check(bool cond, const std::string& msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    ++g_failures;
  }
}

using race::FirstTimeCareer;
using race::FtcStage;

constexpr std::array<double, 3> kHeatsNode = {-3200.0, 5.0, -1460.0};
constexpr std::array<double, 3> kRedRockNode = {-5400.0, 70.0, -3200.0};
constexpr std::array<double, 3> kRaceCentralSite = {-830.0, -6.0, -250.0};

struct FakeHost {
  double cutscene_left = 0, movie_left = 0, screen_left = 0;
  bool hold = false;
  std::array<double, 3> pos{};
  std::vector<std::string> cutscenes, preloads, movies, dj, vo, fades, achievements, mixes;
  std::vector<int64_t> events, autoshow_cars;
  int64_t money = 0;
  int saves = 0;
  bool wristband = false, intro_done = false, autoshow_restricted = false, map_hint = false, tutor = false;
  std::string car;
  bool time_locked = false;
  double tod = 0;
  int64_t pending_event = 0;
  std::array<double, 2> dest{};

  void apply(const race::FtcCommands& c) {
    if (!c.play_cutscene.empty()) {
      cutscenes.push_back(c.play_cutscene);
      // The disc lengths of the two with timed DJ lines; others 2 s.
      cutscene_left = c.play_cutscene == "Opening_Cutscene" ? 39.0 : c.play_cutscene == "NEW_Diner_Cutscene_1" ? 6.7 : 2.0;
      hold = c.hold_cutscene;
      // A held cutscene runs until its voice-over (5 s here) ends.
      if (hold) cutscene_left = 5.0;
    }
    if (!c.preload_cutscene.empty()) preloads.push_back(c.preload_cutscene);
    if (c.stop_cutscene) cutscene_left = 0;
    if (!c.play_movie.empty()) {
      movies.push_back(c.play_movie);
      movie_left = 1.0;
    }
    for (const auto& f : c.fades) fades.push_back(std::to_string(int(f.kind)));
    mixes.insert(mixes.end(), c.mix_snapshots.begin(), c.mix_snapshots.end());
    if (c.swap_car) car = c.car_media;
    if (c.place_car) pos = c.pos;
    if (c.set_time_of_day) tod = c.time_of_day_s;
    if (c.set_time_locked) time_locked = c.time_locked;
    if (c.set_destination) dest = c.destination_xz;
    dj.insert(dj.end(), c.dj_lines.begin(), c.dj_lines.end());
    vo.insert(vo.end(), c.vo.begin(), c.vo.end());
    if (c.open_map || c.open_autoshow) screen_left = 0.5;
    if (c.open_autoshow) {
      autoshow_restricted = c.autoshow_restricted;
      autoshow_cars = c.autoshow_cars;
    }
    if (c.show_map_hint) map_hint = true;
    if (c.activate_tutor) tutor = true;
    if (c.launch_event_id) {
      events.push_back(c.launch_event_id);
      pending_event = c.launch_event_id;
    }
    if (c.save_requested) ++saves;
    money += c.give_money;
    if (!c.achievement.empty()) achievements.push_back(c.achievement);
    if (c.collect_yellow_wristband) wristband = true;
    if (c.intro_completed) intro_done = true;
  }
};

// Moves `p` toward `to` by at most `step` metres (straight line, all axes).
void move_toward(std::array<double, 3>& p, const std::array<double, 3>& to, double step) {
  const double d = std::hypot(to[0] - p[0], to[1] - p[1], to[2] - p[2]);
  if (d <= step) {
    p = to;
    return;
  }
  for (int k = 0; k < 3; ++k) p[k] += (to[k] - p[k]) * step / d;
}

void test_order() {
  FirstTimeCareer ftc;
  ftc.set_event_trigger(FirstTimeCareer::kAllComerFinal, kHeatsNode, FirstTimeCareer::kHeatsSignUpRadius);
  ftc.set_event_trigger(FirstTimeCareer::kMustangRace, kRedRockNode, 25.0);
  ftc.set_race_central(kRaceCentralSite, 24.0);
  FakeHost host;
  host.apply(ftc.start());
  check(ftc.stage() == FtcStage::kControls && ftc.show_controls_screen(), "starts on the controls screen");
  check(host.car == "VIP_Viper_13" && host.pos == FirstTimeCareer::kViperStart, "the Viper placed under the controls screen");
  std::vector<FtcStage> seen = {ftc.stage()};
  bool saw_credits = false, saw_tickets = false, ai_in_opening = false, saw_large = false;
  int min_tickets = 99;
  size_t ticket_zone = 0;
  const double dt = 0.1;
  int final_attempts = 0;
  for (int frame = 0; frame < 40000 && !ftc.complete(); ++frame) {
    race::FtcInput in;
    in.dt = dt;
    in.cutscene_active = host.cutscene_left > 0;
    in.movie_active = host.movie_left > 0;
    in.screen_open = host.screen_left > 0;
    in.throttle = 1.0;
    // Drive: move the player through the stage's zones.
    switch (ftc.stage()) {
      case FtcStage::kInitialDrive:
        // Past the approach zone, then into the diner's arrival sphere.
        move_toward(host.pos, ftc.stage_time() < 2.0 ? FirstTimeCareer::kDinerApproach[0].pos
                                                     : FirstTimeCareer::kDinerArrive[0].pos,
                    400.0);
        break;
      case FtcStage::kRaceToFestival: {
        // Through every ticket zone, the free-tickets trigger, the last
        // stretch and into the festival.
        const auto& tz = FirstTimeCareer::kTicketTriggers;
        std::array<double, 3> to = ticket_zone < tz.size() ? tz[ticket_zone].pos
                                   : ticket_zone == tz.size() ? FirstTimeCareer::kFreeTicketsTrigger.pos
                                   : ticket_zone == tz.size() + 1 ? FirstTimeCareer::kFestivalApproach.pos
                                                                  : FirstTimeCareer::kEndRacePosition;
        move_toward(host.pos, to, 300.0);
        if (host.pos == to) ++ticket_zone;
        break;
      }
      case FtcStage::kHeats: move_toward(host.pos, kHeatsNode, 500.0); break;
      case FtcStage::kShowcase: move_toward(host.pos, kRedRockNode, 500.0); break;
      case FtcStage::kTravelToRaceCentral: move_toward(host.pos, kRaceCentralSite, 1000.0); break;
      default: break;
    }
    if (host.pending_event) {
      in.race_finished = true;
      in.race_event_id = host.pending_event;
      // Miss the cut in the final once, then make it.
      if (host.pending_event == FirstTimeCareer::kAllComerFinal) ++final_attempts;
      in.race_place = (host.pending_event == FirstTimeCareer::kAllComerFinal && final_attempts == 1) ? 6 : 2;
      host.pending_event = 0;
      // The lost final: the player drives off and comes back to sign up again.
      if (in.race_place > 3) host.pos = {kHeatsNode[0] + 100.0, kHeatsNode[1], kHeatsNode[2]};
    }
    in.player_pos = host.pos;
    if (ftc.stage() == FtcStage::kOpeningCutscene && ftc.ai_driving() && !ftc.controls_enabled()) ai_in_opening = true;
    host.apply(ftc.update(in));
    if (ftc.stage() != seen.back()) seen.push_back(ftc.stage());
    if (ftc.show_opening_credits()) saw_credits = true;
    if (!ftc.large_instruction_1().empty()) saw_large = true;
    if (ftc.tickets_remaining() >= 0) {
      saw_tickets = true;
      min_tickets = std::min(min_tickets, ftc.tickets_remaining());
    }
    if (host.cutscene_left > 0) host.cutscene_left -= dt;
    if (host.movie_left > 0) host.movie_left -= dt;
    if (host.screen_left > 0) host.screen_left -= dt;
  }
  const std::vector<FtcStage> want = {
      FtcStage::kControls,           FtcStage::kOpeningCutscene, FtcStage::kInitialDrive,
      FtcStage::kDinerCutscenes,     FtcStage::kRaceToFestival,  FtcStage::kFestivalArrive,
      FtcStage::kFestivalArriveMovie, FtcStage::kWorkshopMap,    FtcStage::kRadioUnlock,
      FtcStage::kHeats,              FtcStage::kShowcase,        FtcStage::kTravelToRaceCentral,
      FtcStage::kRaceCentralMovie,   FtcStage::kWristbandIntro,  FtcStage::kRewards,
      FtcStage::kFirstAutoshow,      FtcStage::kSatNavTutorial,  FtcStage::kComplete,
  };
  check(seen == want, "stages visited in the original's order");
  if (seen != want) {
    for (size_t i = 0; i < seen.size(); ++i) std::printf("  %2zu %s\n", i, race::ftc_stage_name(seen[i]));
    for (auto e : host.events) std::printf("  event %lld\n", (long long)e);
    std::printf("  pos %.1f %.1f %.1f\n", host.pos[0], host.pos[1], host.pos[2]);
  }
  const std::vector<std::string> want_cs = {"Opening_Cutscene",     "NEW_Diner_Cutscene_1", "New_Diner_Cutscene_2",
                                            "New_Diner_Cutscene_3", "Festival_Race_Outro",  "Radio_Unlock",
                                            "Wristband_Intro"};
  check(host.cutscenes == want_cs, "cutscenes in order");
  auto has = [](const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); };
  check(has(host.preloads, "Opening_Cutscene") && has(host.preloads, "NEW_Diner_Cutscene_1") &&
            has(host.preloads, "Wristband_Intro"),
        "cutscene areas preloaded before they play");
  check(host.movies == std::vector<std::string>({"FMV_01", "FMV_02"}), "FMV_01 at the festival, FMV_02 at Race Central");
  check(host.events == std::vector<int64_t>({77, 77, 101}), "the heats final (retried after a miss), then the showcase");
  check(host.money == 40000 && host.wristband && host.intro_done, "40,000 CR, the yellow wristband, intro complete");
  check(host.achievements == std::vector<std::string>({"ACHIEVEMENT_WELCOME_TO_HORIZON", "ACHIEVEMENT_HORIZON_ROOKIE"}),
        "Welcome to Horizon, then Horizon Rookie");
  check(host.autoshow_restricted && host.autoshow_cars == std::vector<int64_t>({1323, 1009}),
        "the first Autoshow lists the WRX STi and the Evo X");
  check(host.map_hint && host.tutor, "the tutor at the hand-over, the map hint at the end");
  check(host.car == "VW_Corrado_95", "the Corrado after the diner");
  check(!host.time_locked, "time unlocked at the end");
  check(ai_in_opening, "the AI drives the Viper under the opening");
  check(saw_credits && saw_tickets && saw_large, "opening titles, places counter, large instructions");
  // Six ticket zones from 10.
  check(min_tickets == 4, "each ticket zone takes one place: 10 - 6 = 4");
  check(has(host.dj, "IntroCutscene") && has(host.dj, "E3Intro") && has(host.dj, "FestivalTicketsDepleting9") &&
            has(host.dj, "FestivalTicketsDepleting4") && !has(host.dj, "FestivalTicketsDepleting3"),
        "DJ lines: intro, E3, the places countdown 9..4");
  check(!has(host.dj, "TicketGiveawayReminder1"), "no giveaway reminder while places are left");
  check(has(host.vo, "RadioFixedByDak"), "Dak's radio VO");
  check(has(host.mixes, "OpeningCutscene") && has(host.mixes, "VOPlayingInstant"), "mixer snapshots");
  check(host.saves >= 5, "save checkpoints");
  check(ftc.controls_enabled() && ftc.hud_visible() && !ftc.ai_driving(), "free roam handed back");
}

void test_opening_commands() {
  FirstTimeCareer ftc;
  auto c = ftc.start();
  check(c.swap_car && c.car_media == "VIP_Viper_13" && c.load_route == 170 && c.place_car, "Viper and route 170 under the controls screen");
  check(c.set_time_of_day && c.time_of_day_s == 33300.0 && c.time_locked, "time locked at 09:15");
  check(c.radio_playlist == std::vector<std::string>({"R1_Everyday", "R1_ThePower"}), "radio playlist");
  check(c.preload_cutscene == "Opening_Cutscene", "the opening's area preloaded");
  check(c.set_destination && c.destination_xz[0] == FirstTimeCareer::kInitialDriveDestination[0], "sat-nav to the diner");
  race::FtcInput in;
  in.dt = 0.1;
  // The controls screen does not leave on a button; it leaves once loaded.
  c = ftc.update(in);
  check(ftc.stage() == FtcStage::kControls, "controls screen up for its first update");
  c = ftc.update(in);
  check(ftc.stage() == FtcStage::kOpeningCutscene, "controls screen leaves when the opening has loaded");
  check(c.play_cutscene == "Opening_Cutscene" && c.play_movie.empty(), "Opening_Cutscene, no FMV");
  check(c.fades.size() == 2 && c.fades[0].kind == race::FtcFadeKind::kBlackOut &&
            c.fades[1].kind == race::FtcFadeKind::kFadeIn && c.fades[1].duration == 2.0,
        "black out, then a 2 s fade in");
  in.cutscene_active = false;
  c = ftc.update(in);
  check(ftc.stage() == FtcStage::kInitialDrive && c.fades.size() == 1 && c.fades[0].kind == race::FtcFadeKind::kWhiteFlash,
        "white flash at the hand-over");
  check(c.activate_tutor && ftc.controls_enabled() && !ftc.ai_driving(), "tutor on, controls handed over");
  // The old 40 m around StartPosition no longer ends the drive.
  in.player_pos = FirstTimeCareer::kStartPosition;
  ftc.update(in);
  check(ftc.stage() == FtcStage::kInitialDrive, "the diner's arrival spheres, not StartPosition, end the drive");
  in.player_pos = FirstTimeCareer::kDinerArrive[1].pos;
  ftc.update(in);
  check(ftc.stage() == FtcStage::kDinerCutscenes, "the diner's arrival sphere ends the drive");
}

void test_race_to_festival() {
  FirstTimeCareer ftc;
  auto c = ftc.resume(FtcStage::kRaceToFestival);
  check(ftc.tickets_remaining() == 10, "the counter starts at 10");
  check(c.set_destination && c.destination_xz[0] == FirstTimeCareer::kHalfWayToTollBooth[0], "sat-nav to HalfWay first");
  check(ftc.hud_instruction() == "IDS_HudInstruction_Race_To_Festival" && ftc.large_instruction_1() == "IDS_HudMsg_RaceTo",
        "race instruction and RACE TO / THE FESTIVAL");
  race::FtcInput in;
  in.dt = 0.1;
  // Re-entering the same zone takes no second place.
  in.player_pos = FirstTimeCareer::kTicketTriggers[0].pos;
  c = ftc.update(in);
  check(ftc.tickets_remaining() == 9 && c.dj_lines == std::vector<std::string>({"FestivalTicketsDepleting9"}), "zone 1: 9 left");
  c = ftc.update(in);
  check(ftc.tickets_remaining() == 9 && c.dj_lines.empty(), "same zone again: still 9");
  in.player_pos = FirstTimeCareer::kFreeTicketsTrigger.pos;
  c = ftc.update(in);
  check(c.set_destination && c.destination_xz[0] == FirstTimeCareer::kTollBoothPosition[0], "free tickets: route on to the toll booth");
  in.player_pos = FirstTimeCareer::kFestivalApproach.pos;
  c = ftc.update(in);
  check(c.stop_dj, "the last stretch cuts the DJ");
  in.player_pos = FirstTimeCareer::kTicketTriggers[5].pos;
  c = ftc.update(in);
  check(ftc.tickets_remaining() == 8 && c.dj_lines.empty(), "no countdown line on the last stretch");
  in.player_pos = {-945.9, -1.0, -647.2};
  ftc.update(in);
  check(ftc.stage() == FtcStage::kFestivalArrive, "the alternate festival zone also arrives");
}

void test_heats_and_showcase() {
  FirstTimeCareer ftc;
  // No sign-up trigger known: the event launches at once.
  auto c = ftc.resume(FtcStage::kHeats);
  check(ftc.hud_instruction() == "IDS_HudInstruction_Do_All_Comers_Final", "Head to the Horizon Heats");
  race::FtcInput in;
  in.dt = 0.1;
  c = ftc.update(in);
  check(c.launch_event_id == 77, "no trigger: the final launches");
  c = ftc.update(in);
  check(c.launch_event_id == 0, "no relaunch while the event runs");
  in.race_finished = true;
  in.race_event_id = 77;
  in.race_place = 4;
  c = ftc.update(in);
  check(ftc.stage() == FtcStage::kHeats && c.launch_event_id == 77, "4th misses the cut: the final again");
  in.race_place = 3;
  c = ftc.update(in);
  check(ftc.stage() == FtcStage::kShowcase && c.place_car && c.pos == FirstTimeCareer::kHeatsFinalPlace &&
            c.time_of_day_s == FirstTimeCareer::kHeatsFinalTimeOfDay,
        "3rd makes it: placed by the heats at 15:48, the showcase next");
  check(ftc.hud_instruction() == "IDS_HudInstruction_Go_To_Red_Rock", "Get to the Showcase event in Red Rock");
  in.race_finished = false;
  c = ftc.update(in);
  check(c.launch_event_id == 101, "the showcase launches");
  in.race_finished = true;
  in.race_event_id = 101;
  in.race_place = 1;
  c = ftc.update(in);
  check(ftc.stage() == FtcStage::kTravelToRaceCentral && c.place_car && c.pos == FirstTimeCareer::kPlaneRacePlace,
        "showcase won: placed by the plane race, Race Central next");
  check(ftc.hud_instruction() == "IDS_HudInstruction_Travel_To_RaceCentral", "Drive to Race Central");
  // With a sign-up trigger the final waits for the player to drive there.
  FirstTimeCareer f2;
  f2.set_event_trigger(77, kHeatsNode, FirstTimeCareer::kHeatsSignUpRadius);
  c = f2.resume(FtcStage::kHeats);
  check(c.set_destination && c.destination_xz[0] == kHeatsNode[0], "sat-nav to the heats");
  race::FtcInput i2;
  i2.dt = 0.1;
  i2.player_pos = {kHeatsNode[0] + 100.0, 0, kHeatsNode[2]};
  c = f2.update(i2);
  check(c.launch_event_id == 0, "away from the heats: nothing launches");
  i2.player_pos = {kHeatsNode[0] + 20.0, 0, kHeatsNode[2]};
  c = f2.update(i2);
  check(c.launch_event_id == 77, "inside 27 m: the final signs up");
}

void test_tutor() {
  race::DrivingTutor t;
  race::TutorInput in;
  in.dt = 0.1;
  t.update(in);
  check(t.message() == race::TutorMessage::kNone, "inactive until the hand-over");
  t.activate();
  t.update(in);
  check(t.message() == race::TutorMessage::kAccelerate, "accelerate first");
  t.update(in);
  check(t.message() == race::TutorMessage::kAccelerate, "until the throttle is pressed");
  in.throttle = 1.0;
  t.update(in);
  check(t.message() == race::TutorMessage::kNone, "the throttle clears it");
  for (int i = 0; i < 99; ++i) t.update(in);
  check(t.message() == race::TutorMessage::kNone, "brake hint waits 10 s");
  t.update(in);
  t.update(in);
  check(t.message() == race::TutorMessage::kBrake, "then the brake hint");
  for (int i = 0; i < 51; ++i) t.update(in);
  check(t.message() == race::TutorMessage::kNone && t.state() == 4, "brake hint for 5 s, then idle");
  // A crash with rewind available: the rewind hint.
  in.impact = true;
  in.rewind_available = true;
  t.update(in);
  check(t.message() == race::TutorMessage::kRewind, "a crash shows the rewind hint");
  in.impact = false;
  for (int i = 0; i < 51; ++i) t.update(in);
  check(t.message() == race::TutorMessage::kNone, "rewind hint for 5 s");
  // Another crash inside the 30 s cooldown: nothing.
  in.impact = true;
  t.update(in);
  check(t.message() == race::TutorMessage::kNone, "rewind cooldown");
  in.impact = false;
  // Fast into a turn: the e-brake hint, then (first time) the camera hint.
  in.speed_mps = 25.0;
  in.turn_ahead = true;
  in.turn_distance_m = 80.0;
  t.update(in);
  check(t.message() == race::TutorMessage::kEBrake, "fast into a turn: e-brake hint");
  in.turn_ahead = false;
  for (int i = 0; i < 66; ++i) t.update(in);
  check(t.message() == race::TutorMessage::kCamera, "after the e-brake hint, the camera hint once");
  for (int i = 0; i < 51; ++i) t.update(in);
  check(t.message() == race::TutorMessage::kNone && t.state() == 4, "camera hint for 5 s");
  // Using rewind ends its hints.
  in.rewind_pressed = true;
  t.update(in);
  in.rewind_pressed = false;
  for (int i = 0; i < 400; ++i) t.update(in);
  in.impact = true;
  t.update(in);
  check(t.message() == race::TutorMessage::kNone, "no rewind hint once the player has rewound");
  const auto h = race::tutor_hint(race::TutorMessage::kAccelerate);
  check(std::string(h.left_id) == "IDS_InputHint_Accelerate_L" && std::string(h.right_id) == "IDS_InputHint_Accelerate_R",
        "hint strings from HelpButtons");
}

void test_stage_names_and_resume() {
  for (int s = 0; s <= int(FtcStage::kComplete); ++s) {
    FtcStage back{};
    check(race::parse_ftc_stage(race::ftc_stage_name(FtcStage(s)), back) && back == FtcStage(s), "stage names round-trip");
  }
  FirstTimeCareer ftc;
  auto c = ftc.resume(FtcStage::kInitialDrive);
  check(ftc.stage() == FtcStage::kControls && c.car_media == "VIP_Viper_13", "resuming the drive replays the opening");
  check(ftc.checkpoint() == FtcStage::kOpeningCutscene, "no checkpoint before the diner");
  c = ftc.resume(FtcStage::kRaceToFestival);
  check(ftc.checkpoint() == FtcStage::kDinerCutscenes, "the race resumes at the diner (save 1)");
  c = ftc.resume(FtcStage::kRadioUnlock);
  check(ftc.checkpoint() == FtcStage::kHeats, "the festival arrival resumes at the heats (save 7)");
  c = ftc.resume(FtcStage::kWristbandIntro);
  check(ftc.checkpoint() == FtcStage::kTravelToRaceCentral, "Race Central resumes on the drive there (save 8)");
  c = ftc.resume(FtcStage::kDinerCutscenes);
  check(c.place_car && c.pos == FirstTimeCareer::kStartPosition && c.play_cutscene == "NEW_Diner_Cutscene_1",
        "the diner resumes at the diner");
  check(race::ftc_title_stage(FtcStage::kHeats) == 7 && race::ftc_title_stage(FtcStage::kShowcase) == 12 &&
            race::ftc_title_stage(FtcStage::kTravelToRaceCentral) == 8,
        "title stage values");
}

}  // namespace

int main() {
  test_order();
  test_opening_commands();
  test_race_to_festival();
  test_heats_and_showcase();
  test_tutor();
  test_stage_names_and_resume();
  if (g_failures) {
    std::fprintf(stderr, "first_time_career_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("first_time_career_tests: ok\n");
  return 0;
}
