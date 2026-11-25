#include "race/first_time_career.h"

#include <algorithm>
#include <cmath>

namespace race {
namespace {

struct StageName {
  FtcStage stage;
  const char* name;
};
constexpr StageName kNames[] = {
    {FtcStage::kNotStarted, "not_started"},
    {FtcStage::kControls, "controls"},
    {FtcStage::kOpeningCutscene, "opening_cutscene"},
    {FtcStage::kInitialDrive, "initial_drive"},
    {FtcStage::kDinerCutscenes, "diner_cutscenes"},
    {FtcStage::kRaceToFestival, "race_to_festival"},
    {FtcStage::kFestivalArrive, "festival_arrive"},
    {FtcStage::kFestivalArriveMovie, "festival_arrive_movie"},
    {FtcStage::kWorkshopMap, "workshop_map"},
    {FtcStage::kRadioUnlock, "radio_unlock"},
    {FtcStage::kHeats, "heats"},
    {FtcStage::kTravelToRaceCentral, "travel_to_race_central"},
    {FtcStage::kRaceCentralMovie, "race_central_movie"},
    {FtcStage::kWristbandIntro, "wristband_intro"},
    {FtcStage::kRewards, "rewards"},
    {FtcStage::kFirstAutoshow, "first_autoshow"},
    {FtcStage::kSatNavTutorial, "satnav_tutorial"},
    {FtcStage::kComplete, "complete"},
};

double dist_xz(const std::array<double, 3>& a, const std::array<double, 3>& b) {
  return std::hypot(a[0] - b[0], a[2] - b[2]);
}

FtcFade fade(FtcFadeKind kind, double duration = 0.0, bool s = true) { return FtcFade{kind, duration, s}; }

// The controller loading screen: it stays up while the opening loads; the
// host's load is not modelled, so it leaves on confirm or after this long
// (interim, Q65).
constexpr double kControlsScreenTime = 6.0;
constexpr double kOutroHold = 3.0;      // wait_3_sec after Festival_Race_Outro starts
constexpr double kFadeOut2 = 2.0;       // fade_out_2_sec
constexpr double kFadeOutHalf = 0.5;    // fade_out_half_sec
constexpr double kOpeningFadeIn = 2.0;  // play_intro_camera: CFadeIn 2 s, S curve

}  // namespace

const char* ftc_stage_name(FtcStage stage) {
  for (const auto& n : kNames)
    if (n.stage == stage) return n.name;
  return "not_started";
}

bool parse_ftc_stage(std::string_view name, FtcStage& out) {
  for (const auto& n : kNames)
    if (name == n.name) {
      out = n.stage;
      return true;
    }
  return false;
}

int FirstTimeCareer::tickets_for_remaining(double remaining_fraction) {
  for (const auto& t : kTickets)
    if (remaining_fraction >= t.remaining_fraction) return t.tickets;
  return kTickets.back().tickets;
}

void FirstTimeCareer::at(double t, std::string dj) { timed_.push_back({t, std::move(dj)}); }

void FirstTimeCareer::pump_timed(FtcCommands& out) {
  for (auto it = timed_.begin(); it != timed_.end();) {
    if (stage_t_ >= it->t) {
      out.dj_lines.push_back(std::move(it->dj));
      it = timed_.erase(it);
    } else {
      ++it;
    }
  }
}

FtcCommands FirstTimeCareer::start() {
  controls_ = true;
  ai_ = false;
  hud_ = false;
  credits_ = false;
  map_locked_ = false;
  tutor_.clear();
  instruction_.clear();
  return enter(FtcStage::kControls);
}

FtcCommands FirstTimeCareer::resume(FtcStage stage) {
  switch (stage) {
    case FtcStage::kNotStarted:
    case FtcStage::kComplete: stage_ = stage; return {};
    // The opening restarts as a whole: the drive begins inside the cutscene.
    case FtcStage::kControls:
    case FtcStage::kOpeningCutscene:
    case FtcStage::kInitialDrive: return enter(FtcStage::kOpeningCutscene);
    case FtcStage::kFestivalArrive: return enter(FtcStage::kFestivalArriveMovie);
    case FtcStage::kRewards: return enter(FtcStage::kRewards);
    default: return enter(stage);
  }
}

FtcCommands FirstTimeCareer::enter(FtcStage next) {
  stage_ = next;
  stage_t_ = 0.0;
  step_ = 0;
  step_t_ = 0.0;
  timed_.clear();
  instruction_.clear();
  FtcCommands c;
  c.save_requested = next != FtcStage::kControls;
  auto show_instruction = [&](const char* id) {
    instruction_ = id;
    instruction_t_ = 0.0;
  };
  switch (next) {
    case FtcStage::kNotStarted:
    case FtcStage::kControls:
      controls_ = false;
      hud_ = false;
      c.save_requested = false;
      break;

    case FtcStage::kOpeningCutscene:
      // intro_flow: radio playlist, the time locked at 09:15, the Viper on
      // route 170 under AI control, controls off, no traffic, then a cut to
      // black and Opening_Cutscene fading in over 2 s.
      c.radio_playlist = {"R1_Everyday", "R1_ThePower"};
      c.set_radio_switching = true;
      c.radio_switching = false;
      c.set_time_of_day = true;
      c.time_of_day_s = kIntroTimeOfDay;
      c.set_time_locked = true;
      c.time_locked = true;
      c.load_route = kInitialDriveRoute;
      c.swap_car = true;
      c.car_media = kViperMedia;
      c.car_id = kViperCarId;
      c.place_car = true;
      c.pos = kViperStart;
      c.heading_deg = kViperHeading;
      c.set_destination = true;
      c.destination_label = "IDS_HudInstruction_Race_To_Festival";
      c.destination_xz = {kStartPosition[0], kStartPosition[2]};
      c.set_civilian_traffic = true;
      c.civilian_traffic = false;
      c.mix_snapshots = {"OpeningCutscene"};
      c.fades = {fade(FtcFadeKind::kBlackOut), fade(FtcFadeKind::kFadeIn, kOpeningFadeIn)};
      c.play_cutscene = "Opening_Cutscene";
      controls_ = false;
      ai_ = true;
      hud_ = false;
      tutor_.clear();
      at(12.5, "IntroCutscene");
      break;

    case FtcStage::kInitialDrive:
      // play_intro_camera done: tutor on, human takeover, traffic back,
      // the camera back on the player, a white flash and the HUD.
      c.fades = {fade(FtcFadeKind::kWhiteFlash)};
      c.set_civilian_traffic = true;
      c.civilian_traffic = true;
      c.mix_snapshots = {"FreeRoam"};
      controls_ = true;
      ai_ = false;
      hud_ = true;
      tutor_ = "IDS_Accelerate_Message";
      throttle_held_ = 0.0;
      break;

    case FtcStage::kDinerCutscenes:
      // Finish_Initial_Drive + SetupFestivalDrive: black out, the diner
      // cutscenes with the DJ's E3 intro and the opening credits.
      c.fades = {fade(FtcFadeKind::kBlackOut)};
      c.mix_snapshots = {"DJSpeakingSnapshot"};
      c.radio_playlist = {"R1_ThePower"};
      c.play_cutscene = "NEW_Diner_Cutscene_1";
      c.set_civilian_traffic = true;
      c.civilian_traffic = false;
      controls_ = false;
      ai_ = true;
      hud_ = false;
      tutor_.clear();
      at(1.5, "E3Intro");
      break;

    case FtcStage::kRaceToFestival:
      // The white flash out of New_Diner_Cutscene_3, the Corrado at the diner.
      c.fades = {fade(FtcFadeKind::kWhiteFlash)};
      c.swap_car = true;
      c.car_media = kCorradoMedia;
      c.car_id = kCorradoCarId;
      c.place_car = true;
      c.pos = kDinerPlace;
      c.heading_deg = kDinerHeading;
      c.set_civilian_traffic = true;
      c.civilian_traffic = true;
      c.set_destination = true;
      c.destination_label = "IDS_HudInstruction_Race_To_Festival";
      c.destination_xz = {kEndRacePosition[0], kEndRacePosition[2]};
      controls_ = true;
      ai_ = false;
      hud_ = true;
      credits_ = false;
      show_instruction("IDS_HudInstruction_Race_To_Festival");
      race_total_ = std::max(1.0, dist_xz(kDinerPlace, kEndRacePosition));
      tickets_ = kTickets.front().tickets;
      at(30.0, "TicketGiveawayReminder1");
      at(60.0, "TicketGiveawayReminder2");
      break;

    case FtcStage::kFestivalArrive:
      // FestivalArrive: controls off, the AI drives into the festival under
      // Festival_Race_Outro, then a 2 s fade to black.
      c.play_cutscene = "Festival_Race_Outro";
      controls_ = false;
      ai_ = true;
      hud_ = false;
      break;

    case FtcStage::kFestivalArriveMovie:
      c.fades = {fade(FtcFadeKind::kBlackOut)};
      c.stop_cutscene = true;
      c.load_route = 0;
      c.play_movie = "FMV_01";
      controls_ = false;
      ai_ = false;
      hud_ = false;
      break;

    case FtcStage::kWorkshopMap:
      // FestivalArriveCutscene: placed at the workshop, the map shown with
      // fast travel locked.
      c.place_car = true;
      c.pos = kWorkshopPlace;
      c.heading_deg = kWorkshopHeading;
      c.set_civilian_traffic = true;
      c.civilian_traffic = false;
      c.open_map = true;
      map_locked_ = true;
      controls_ = false;
      hud_ = false;
      break;

    case FtcStage::kRadioUnlock:
      c.fades = {fade(FtcFadeKind::kBlackOut)};
      c.repair_car = true;
      c.set_time_locked = true;
      c.time_locked = true;
      c.set_time_of_day = true;
      c.time_of_day_s = kInitialTimeOfDay;
      c.mix_snapshots = {"FreeRoam", "VOPlaying"};
      c.play_cutscene = "Radio_Unlock";
      c.vo = {"RadioFixedByDak"};
      c.set_radio_switching = true;
      c.radio_switching = true;
      map_locked_ = false;
      controls_ = false;
      hud_ = false;
      break;

    case FtcStage::kHeats:
      // Interim (Q65): the heats follow Radio_Unlock; the flow files place
      // them in code. Both All Comers heats run in order.
      c.set_civilian_traffic = true;
      c.civilian_traffic = true;
      c.launch_event_id = kAllComerRace1;
      heat_ = 0;
      controls_ = true;
      hud_ = true;
      show_instruction("IDS_HudInstruction_Do_All_Comers_Heat");
      break;

    case FtcStage::kTravelToRaceCentral:
      c.set_destination = true;
      c.destination_label = "IDS_HudInstruction_Travel_To_RaceCentral";
      c.destination_xz = {kRaceCentral[0], kRaceCentral[2]};
      controls_ = true;
      hud_ = true;
      show_instruction("IDS_HudInstruction_Travel_To_RaceCentral");
      break;

    case FtcStage::kRaceCentralMovie:
      // RaceCentralArrive: a half-second fade out, 08:11, FMV_02.
      c.fades = {fade(FtcFadeKind::kFadeOut, kFadeOutHalf)};
      c.set_time_of_day = true;
      c.time_of_day_s = kRaceCentralTimeOfDay;
      controls_ = false;
      hud_ = false;
      break;

    case FtcStage::kWristbandIntro:
      c.repair_car = true;
      c.set_civilian_traffic = true;
      c.civilian_traffic = false;
      c.mix_snapshots = {"VOPlaying"};
      c.play_cutscene = "Wristband_Intro";
      c.hold_cutscene = true;  // held until RaceCentralWristbandIntro ends
      controls_ = false;
      hud_ = false;
      break;

    case FtcStage::kRewards:
      c.fades = {fade(FtcFadeKind::kBlackOut)};
      c.achievement = "ACHIEVEMENT_HORIZON_ROOKIE";
      c.dj_lines = {"festival_wristband"};
      c.give_money = kStartingMoney;
      c.collect_yellow_wristband = true;
      c.intro_completed = true;
      controls_ = false;
      hud_ = false;
      break;

    case FtcStage::kFirstAutoshow:
      c.open_map = true;  // map_flow_with_race_central_vo
      map_locked_ = true;
      controls_ = false;
      hud_ = false;
      break;

    case FtcStage::kSatNavTutorial:
      c.show_satnav_hint = true;
      c.set_destination = true;
      c.destination_label = kRecaroRush;
      c.set_time_locked = true;
      c.time_locked = false;
      c.set_civilian_traffic = true;
      c.civilian_traffic = true;
      controls_ = true;
      hud_ = true;
      map_locked_ = false;
      break;

    case FtcStage::kComplete:
      c.enter_free_roam = true;
      controls_ = true;
      ai_ = false;
      hud_ = true;
      tutor_.clear();
      break;
  }
  return c;
}

FtcCommands FirstTimeCareer::update(const FtcInput& in) {
  FtcCommands c;
  if (!active()) return c;
  const double dt = std::clamp(in.dt, 0.0, 0.25);
  stage_t_ += dt;
  step_t_ += dt;
  if (!instruction_.empty() && (instruction_t_ += dt) >= kLargeHudInstructionDisplayTime &&
      stage_ != FtcStage::kRaceToFestival)
    instruction_.clear();
  pump_timed(c);
  auto next_step = [&]() {
    ++step_;
    step_t_ = 0.0;
  };
  // The host starts what the previous update asked for before this one, so a
  // wait on a cutscene / movie / screen begins one update after the request.
  auto waited = [&](bool busy) { return step_t_ > 0.0 && !busy; };
  auto go = [&](FtcStage next) {
    FtcCommands n = enter(next);
    n.dj_lines.insert(n.dj_lines.begin(), c.dj_lines.begin(), c.dj_lines.end());
    return n;
  };

  switch (stage_) {
    case FtcStage::kControls:
      if ((in.confirm && stage_t_ > 0.5) || stage_t_ >= kControlsScreenTime) return go(FtcStage::kOpeningCutscene);
      break;

    case FtcStage::kOpeningCutscene:
      if (waited(in.cutscene_active)) return go(FtcStage::kInitialDrive);
      break;

    case FtcStage::kInitialDrive:
      // Tutor (interim, Q62): the accelerate prompt stays until the throttle
      // has been held for AccelerateTextTimerLimit seconds.
      if (!tutor_.empty()) {
        if (in.throttle > 0.5) throttle_held_ += dt;
        if (throttle_held_ >= kAccelerateTextTimerLimit) tutor_.clear();
      }
      // Interim (Q65): the drive ends at route 170's end or at the diner.
      if (in.route_finished || dist_xz(in.player_pos, kStartPosition) < kArriveRadius)
        return go(FtcStage::kDinerCutscenes);
      break;

    case FtcStage::kDinerCutscenes:
      if (step_ == 0 && waited(in.cutscene_active)) {
        c.play_cutscene = "New_Diner_Cutscene_2";
        credits_ = true;  // ShowOpeningCredits (interim presentation, Q64)
        next_step();
      } else if (step_ == 1 && waited(in.cutscene_active)) {
        c.play_cutscene = "New_Diner_Cutscene_3";
        credits_ = false;
        next_step();
      } else if (step_ == 2 && waited(in.cutscene_active)) {
        return go(FtcStage::kRaceToFestival);
      }
      break;

    case FtcStage::kRaceToFestival: {
      // Interim (Q61): the counter follows the straight-line distance left
      // to the festival through First50TicketsDistances; it never reaches
      // zero, so the race cannot be lost.
      const double left = dist_xz(in.player_pos, kEndRacePosition);
      const int t = tickets_for_remaining(std::clamp(left / race_total_, 0.0, 1.0));
      while (tickets_ > t) {
        --tickets_;
        c.dj_lines.push_back("FestivalTicketsDepleting" + std::to_string(tickets_));
      }
      if (left < kArriveRadius) return go(FtcStage::kFestivalArrive);
      break;
    }

    case FtcStage::kFestivalArrive:
      if (step_ == 0 && step_t_ >= kOutroHold) {
        c.fades = {fade(FtcFadeKind::kFadeOut, kFadeOut2)};
        next_step();
      } else if (step_ == 1 && step_t_ >= kFadeOut2) {
        return go(FtcStage::kFestivalArriveMovie);
      }
      break;

    case FtcStage::kFestivalArriveMovie:
      if (waited(in.movie_active)) return go(FtcStage::kWorkshopMap);
      break;

    case FtcStage::kWorkshopMap:
      if (waited(in.screen_open)) return go(FtcStage::kRadioUnlock);
      break;

    case FtcStage::kRadioUnlock:
      if (waited(in.cutscene_active)) {
        FtcCommands n = go(FtcStage::kHeats);
        n.dj_lines.push_back("CycleRadioLogos");
        return n;
      }
      break;

    case FtcStage::kHeats:
      if (in.race_finished) {
        if (in.race_event_id == kAllComerRace1 && heat_ == 0) {
          heat_ = 1;
          c.launch_event_id = kAllComerRace2;
        } else if (in.race_event_id == kAllComerFinal) {
          if (in.race_place >= 1 && in.race_place <= kHeatCutoffPlace) return go(FtcStage::kTravelToRaceCentral);
          c.launch_event_id = kAllComerFinal;  // retry the final heat
        }
      }
      break;

    case FtcStage::kTravelToRaceCentral:
      if (dist_xz(in.player_pos, kRaceCentral) < kArriveRadius) return go(FtcStage::kRaceCentralMovie);
      break;

    case FtcStage::kRaceCentralMovie:
      if (step_ == 0 && step_t_ >= kFadeOutHalf) {
        c.play_movie = "FMV_02";
        next_step();
      } else if (step_ == 1 && waited(in.movie_active)) {
        return go(FtcStage::kWristbandIntro);
      }
      break;

    case FtcStage::kWristbandIntro:
      if (waited(in.cutscene_active)) return go(FtcStage::kRewards);
      break;

    case FtcStage::kRewards:
      if (stage_t_ >= kLargeHudInstructionDisplayTime) return go(FtcStage::kFirstAutoshow);
      break;

    case FtcStage::kFirstAutoshow:
      if (step_ == 0 && waited(in.screen_open)) {
        c.place_car = true;
        c.pos = kWorkshopPlace;
        c.heading_deg = kWorkshopHeading;
        c.open_autoshow = true;
        c.autoshow_restricted = true;
        map_locked_ = false;
        next_step();
      } else if (step_ == 1 && waited(in.screen_open)) {
        return go(FtcStage::kSatNavTutorial);
      }
      break;

    case FtcStage::kSatNavTutorial:
      if (stage_t_ >= kLargeHudInstructionDisplayTime) return go(FtcStage::kComplete);
      break;

    case FtcStage::kNotStarted:
    case FtcStage::kComplete: break;
  }
  return c;
}

}  // namespace race
