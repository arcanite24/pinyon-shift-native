// The first-time career state machine, stepped headless through every stage
// by a fake host that plays cutscenes and movies for a fixed time, closes
// screens, drives the player to each destination and finishes the heats.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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

struct FakeHost {
  double cutscene_left = 0, movie_left = 0, screen_left = 0;
  bool hold = false;
  std::array<double, 3> pos{};
  std::vector<std::string> cutscenes, movies, dj, vo, fades, achievements;
  std::vector<int64_t> events;
  int64_t money = 0;
  bool wristband = false, intro_done = false, autoshow_restricted = false, satnav_hint = false;
  std::string car;
  bool time_locked = false;
  double tod = 0;
  int64_t pending_event = 0;

  void apply(const race::FtcCommands& c) {
    if (!c.play_cutscene.empty()) {
      cutscenes.push_back(c.play_cutscene);
      // The disc lengths of the two with timed DJ lines; others 2 s.
      cutscene_left = c.play_cutscene == "Opening_Cutscene" ? 39.0 : c.play_cutscene == "NEW_Diner_Cutscene_1" ? 6.7 : 2.0;
      hold = c.hold_cutscene;
      // A held cutscene runs until its voice-over (5 s here) ends.
      if (hold) cutscene_left = 5.0;
    }
    if (c.stop_cutscene) cutscene_left = 0;
    if (!c.play_movie.empty()) {
      movies.push_back(c.play_movie);
      movie_left = 1.0;
    }
    for (const auto& f : c.fades) fades.push_back(std::to_string(int(f.kind)));
    if (c.swap_car) car = c.car_media;
    if (c.place_car) pos = c.pos;
    if (c.set_time_of_day) tod = c.time_of_day_s;
    if (c.set_time_locked) time_locked = c.time_locked;
    dj.insert(dj.end(), c.dj_lines.begin(), c.dj_lines.end());
    vo.insert(vo.end(), c.vo.begin(), c.vo.end());
    if (c.open_map || c.open_autoshow) screen_left = 0.5;
    if (c.open_autoshow) autoshow_restricted = c.autoshow_restricted;
    if (c.show_satnav_hint) satnav_hint = true;
    if (c.launch_event_id) {
      events.push_back(c.launch_event_id);
      pending_event = c.launch_event_id;
    }
    money += c.give_money;
    if (!c.achievement.empty()) achievements.push_back(c.achievement);
    if (c.collect_yellow_wristband) wristband = true;
    if (c.intro_completed) intro_done = true;
  }
};

void test_order() {
  FirstTimeCareer ftc;
  FakeHost host;
  host.apply(ftc.start());
  check(ftc.stage() == FtcStage::kControls && ftc.show_controls_screen(), "starts on the controls screen");
  std::vector<FtcStage> seen = {ftc.stage()};
  bool saw_tutor = false, saw_credits = false, saw_tickets = false, ai_in_opening = false;
  int min_tickets = 99;
  const double dt = 0.1;
  for (int frame = 0; frame < 20000 && !ftc.complete(); ++frame) {
    race::FtcInput in;
    in.dt = dt;
    in.cutscene_active = host.cutscene_left > 0;
    in.movie_active = host.movie_left > 0;
    in.screen_open = host.screen_left > 0;
    in.confirm = ftc.stage() == FtcStage::kControls && ftc.stage_time() > 1.0;
    in.throttle = 1.0;
    // Drive: move the player to where the stage is heading.
    switch (ftc.stage()) {
      case FtcStage::kInitialDrive:
        if (ftc.stage_time() > 5.0) host.pos = FirstTimeCareer::kStartPosition;
        break;
      case FtcStage::kRaceToFestival: {
        // Move toward the festival over ~20 s.
        const auto& e = FirstTimeCareer::kEndRacePosition;
        for (int k = 0; k < 3; ++k) host.pos[k] += (e[k] - host.pos[k]) * 0.03;
        if (std::hypot(host.pos[0] - e[0], host.pos[2] - e[2]) < 60) host.pos = e;
        break;
      }
      case FtcStage::kTravelToRaceCentral: host.pos = FirstTimeCareer::kRaceCentral; break;
      default: break;
    }
    in.player_pos = host.pos;
    if (host.pending_event && ftc.stage() == FtcStage::kHeats) {
      in.race_finished = true;
      in.race_event_id = host.pending_event;
      // Lose the final heat once, then make the cut.
      const bool first_final = host.pending_event == FirstTimeCareer::kAllComerFinal &&
                               std::count(host.events.begin(), host.events.end(), FirstTimeCareer::kAllComerFinal) == 1;
      in.race_place = first_final ? 6 : 2;
      host.pending_event = 0;
    }
    if (ftc.stage() == FtcStage::kOpeningCutscene && ftc.ai_driving() && !ftc.controls_enabled()) ai_in_opening = true;
    host.apply(ftc.update(in));
    if (ftc.stage() != seen.back()) seen.push_back(ftc.stage());
    if (!ftc.tutor_prompt().empty()) saw_tutor = true;
    if (ftc.show_opening_credits()) saw_credits = true;
    if (ftc.tickets_remaining() >= 0) {
      saw_tickets = true;
      min_tickets = std::min(min_tickets, ftc.tickets_remaining());
    }
    // The fake host's clocks.
    if (host.cutscene_left > 0)
      host.cutscene_left -= dt;
    if (host.movie_left > 0) host.movie_left -= dt;
    if (host.screen_left > 0) host.screen_left -= dt;
  }
  const std::vector<FtcStage> want = {
      FtcStage::kControls,          FtcStage::kOpeningCutscene, FtcStage::kInitialDrive,
      FtcStage::kDinerCutscenes,    FtcStage::kRaceToFestival,  FtcStage::kFestivalArrive,
      FtcStage::kFestivalArriveMovie, FtcStage::kWorkshopMap,   FtcStage::kRadioUnlock,
      FtcStage::kHeats,             FtcStage::kTravelToRaceCentral, FtcStage::kRaceCentralMovie,
      FtcStage::kWristbandIntro,    FtcStage::kRewards,         FtcStage::kFirstAutoshow,
      FtcStage::kSatNavTutorial,    FtcStage::kComplete,
  };
  check(seen == want, "stages visited in the original's order");
  for (size_t i = 0; i < seen.size(); ++i) std::printf("  %2zu %s\n", i, race::ftc_stage_name(seen[i]));
  const std::vector<std::string> want_cs = {"Opening_Cutscene",     "NEW_Diner_Cutscene_1", "New_Diner_Cutscene_2",
                                            "New_Diner_Cutscene_3", "Festival_Race_Outro",  "Radio_Unlock",
                                            "Wristband_Intro"};
  check(host.cutscenes == want_cs, "cutscenes in order");
  check(host.movies == std::vector<std::string>({"FMV_01", "FMV_02"}), "FMV_01 at the festival, FMV_02 at Race Central");
  check(host.events == std::vector<int64_t>({76, 77, 77}), "both All Comers heats, the final retried after a miss");
  check(host.money == 40000 && host.wristband && host.intro_done, "40,000 CR, the yellow wristband, intro complete");
  check(host.achievements == std::vector<std::string>({"ACHIEVEMENT_HORIZON_ROOKIE"}), "Horizon Rookie");
  check(host.autoshow_restricted && host.satnav_hint, "restricted first Autoshow and the sat-nav hint");
  check(host.car == "VW_Corrado_95", "the Corrado after the diner");
  check(!host.time_locked, "time unlocked at the end");
  check(ai_in_opening, "the AI drives the Viper under the opening");
  check(saw_tutor && saw_credits && saw_tickets && min_tickets == 2, "tutor prompt, credits, ticket countdown to 2");
  auto has = [&](const std::vector<std::string>& v, const char* s) {
    return std::find(v.begin(), v.end(), s) != v.end();
  };
  check(has(host.dj, "IntroCutscene") && has(host.dj, "E3Intro") && has(host.dj, "FestivalTicketsDepleting2"),
        "DJ lines: intro, E3, ticket countdown");
  check(has(host.vo, "RadioFixedByDak"), "Dak's radio VO");
  check(ftc.controls_enabled() && ftc.hud_visible() && !ftc.ai_driving(), "free roam handed back");
}

void test_opening_commands() {
  FirstTimeCareer ftc;
  ftc.start();
  race::FtcInput in;
  in.dt = 0.1;
  in.confirm = true;
  race::FtcCommands c;
  for (int i = 0; i < 20 && ftc.stage() == FtcStage::kControls; ++i) c = ftc.update(in);
  check(ftc.stage() == FtcStage::kOpeningCutscene, "confirm leaves the controls screen");
  check(c.play_cutscene == "Opening_Cutscene" && c.swap_car && c.car_media == "VIP_Viper_13" && c.load_route == 170,
        "opening: Viper on route 170 under Opening_Cutscene");
  check(c.set_time_of_day && c.time_of_day_s == 33300.0 && c.time_locked, "time locked at 09:15");
  check(c.radio_playlist == std::vector<std::string>({"R1_Everyday", "R1_ThePower"}), "radio playlist");
  check(c.fades.size() == 2 && c.fades[0].kind == race::FtcFadeKind::kBlackOut &&
            c.fades[1].kind == race::FtcFadeKind::kFadeIn && c.fades[1].duration == 2.0,
        "black out, then a 2 s fade in");
  check(c.play_movie.empty(), "no FMV at career start");
  // Hand-over: a white flash and the accelerate prompt.
  in.confirm = false;
  in.cutscene_active = false;
  c = ftc.update(in);
  check(ftc.stage() == FtcStage::kInitialDrive && c.fades.size() == 1 && c.fades[0].kind == race::FtcFadeKind::kWhiteFlash,
        "white flash at the hand-over");
  check(ftc.tutor_prompt() == "IDS_Accelerate_Message" && ftc.controls_enabled(), "hold RT prompt, controls on");
  in.throttle = 1.0;
  for (int i = 0; i < 35; ++i) ftc.update(in);
  check(ftc.tutor_prompt().empty(), "prompt clears after 3 s of throttle");
}

void test_heat_edges() {
  FirstTimeCareer ftc;
  ftc.resume(FtcStage::kHeats);
  race::FtcInput in;
  in.dt = 0.1;
  in.race_finished = true;
  in.race_event_id = FirstTimeCareer::kAllComerRace1;
  in.race_place = 1;
  auto c = ftc.update(in);
  check(c.launch_event_id == FirstTimeCareer::kAllComerRace2, "first heat done: the final");
  in.race_event_id = FirstTimeCareer::kAllComerFinal;
  in.race_place = 8;
  c = ftc.update(in);
  check(c.launch_event_id == FirstTimeCareer::kAllComerFinal, "missed cut: retry");
  // A held race_finished flag does not relaunch every frame once reported.
  in.race_finished = false;
  c = ftc.update(in);
  check(c.launch_event_id == 0 && ftc.stage() == FtcStage::kHeats, "no relaunch without a new finish");
}

void test_stage_names_and_resume() {
  for (int s = 0; s <= int(FtcStage::kComplete); ++s) {
    FtcStage back{};
    check(race::parse_ftc_stage(race::ftc_stage_name(FtcStage(s)), back) && back == FtcStage(s), "stage names round-trip");
  }
  FirstTimeCareer ftc;
  auto c = ftc.resume(FtcStage::kInitialDrive);
  check(ftc.stage() == FtcStage::kOpeningCutscene && c.play_cutscene == "Opening_Cutscene", "resuming the drive replays the opening");
  c = ftc.resume(FtcStage::kHeats);
  check(ftc.stage() == FtcStage::kHeats && c.launch_event_id == 76, "resume at the heats relaunches the first heat");
  check(FirstTimeCareer::tickets_for_remaining(1.0) == 10 && FirstTimeCareer::tickets_for_remaining(0.55) == 6 &&
            FirstTimeCareer::tickets_for_remaining(0.0) == 2,
        "ticket table");
}

}  // namespace

int main() {
  test_order();
  test_opening_commands();
  test_heat_edges();
  test_stage_names_and_resume();
  if (g_failures) {
    std::fprintf(stderr, "first_time_career_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("first_time_career_tests: ok\n");
  return 0;
}
