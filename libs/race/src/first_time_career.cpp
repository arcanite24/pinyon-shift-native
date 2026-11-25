#include "race/first_time_career.h"

#include <algorithm>
#include <cmath>

namespace race {
namespace {

struct StageName {
  FtcStage stage;
  const char* name;
  int title_stage;
};
constexpr StageName kNames[] = {
    {FtcStage::kNotStarted, "not_started", 0},
    {FtcStage::kControls, "controls", 0},
    {FtcStage::kOpeningCutscene, "opening_cutscene", 0},
    {FtcStage::kInitialDrive, "initial_drive", 0},
    {FtcStage::kDinerCutscenes, "diner_cutscenes", 1},
    {FtcStage::kRaceToFestival, "race_to_festival", 3},
    {FtcStage::kFestivalArrive, "festival_arrive", 4},
    {FtcStage::kFestivalArriveMovie, "festival_arrive_movie", 4},
    {FtcStage::kWorkshopMap, "workshop_map", 4},
    {FtcStage::kRadioUnlock, "radio_unlock", 4},
    {FtcStage::kHeats, "heats", 7},
    {FtcStage::kShowcase, "showcase", 12},
    {FtcStage::kTravelToRaceCentral, "travel_to_race_central", 8},
    {FtcStage::kRaceCentralMovie, "race_central_movie", 9},
    {FtcStage::kWristbandIntro, "wristband_intro", 9},
    {FtcStage::kRewards, "rewards", 9},
    {FtcStage::kFirstAutoshow, "first_autoshow", 9},
    {FtcStage::kSatNavTutorial, "satnav_tutorial", 9},
    {FtcStage::kComplete, "complete", 8},
};

double dist_xz(const std::array<double, 3>& a, const std::array<double, 3>& b) {
  return std::hypot(a[0] - b[0], a[2] - b[2]);
}
// The activity's trigger spheres (radius in metres, full 3D distance).
bool inside(const FtcZone& z, const std::array<double, 3>& p) {
  return std::hypot(p[0] - z.pos[0], p[1] - z.pos[1], p[2] - z.pos[2]) <= z.radius;
}

FtcFade fade(FtcFadeKind kind, double duration = 0.0, bool s = true) { return FtcFade{kind, duration, s}; }

constexpr double kOutroHold = 3.0;      // wait_3_sec after Festival_Race_Outro starts
constexpr double kFadeOut2 = 2.0;       // fade_out_2_sec
constexpr double kFadeOutHalf = 0.5;    // fade_out_half_sec
constexpr double kOpeningFadeIn = 2.0;  // play_intro_camera: CFadeIn 2 s, S curve
// Interim (Q62): when the race's sat-nav moves on to its next waypoint.
constexpr double kWaypointRadius = 20.0;
// The initial drive's next sat-nav leg starts here, short of the sat-nav's
// own arrival distance (interim).
constexpr double kDriveLegRadius = 60.0;
// The event sign-up sphere re-arms once the player has left it by this much.
constexpr double kRearmMargin = 10.0;
// A launched event that has not started racing this long is taken as declined.
constexpr double kLaunchGrace = 10.0;

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

int ftc_title_stage(FtcStage stage) {
  for (const auto& n : kNames)
    if (n.stage == stage) return n.title_stage;
  return 0;
}

FtcTodRequest ftc_tod_request(double now_s, double target_s, double duration_s) {
  FtcTodRequest r;
  r.target_s = target_s < now_s ? target_s + 86400.0 : target_s;
  r.rate = duration_s > 0.0 ? (r.target_s - now_s) / duration_s : 0.0;
  return r;
}

void FirstTimeCareer::set_event_trigger(int64_t event_id, const std::array<double, 3>& pos, double radius) {
  for (auto& t : triggers_)
    if (t.id == event_id) {
      t.pos = pos;
      t.radius = radius;
      return;
    }
  triggers_.push_back({event_id, pos, radius});
}

void FirstTimeCareer::set_race_central(const std::array<double, 3>& pos, double radius) {
  have_race_central_ = true;
  race_central_ = pos;
  race_central_radius_ = radius > 0 ? radius : kRaceCentralRadius;
}

bool FirstTimeCareer::event_trigger(int64_t id, const std::array<double, 3>& p, double& radius,
                                    double& distance) const {
  for (const auto& t : triggers_)
    if (t.id == id) {
      radius = t.radius;
      distance = dist_xz(p, t.pos);
      return true;
    }
  return false;
}

FtcStage FirstTimeCareer::checkpoint() const {
  switch (stage_) {
    case FtcStage::kNotStarted:
    case FtcStage::kControls:
    case FtcStage::kOpeningCutscene:
    case FtcStage::kInitialDrive: return FtcStage::kOpeningCutscene;
    // Save 1 at the diner.
    case FtcStage::kDinerCutscenes:
    case FtcStage::kRaceToFestival: return FtcStage::kDinerCutscenes;
    // Save 7 on arriving at the festival: a reload waits for the heats.
    case FtcStage::kFestivalArrive:
    case FtcStage::kFestivalArriveMovie:
    case FtcStage::kWorkshopMap:
    case FtcStage::kRadioUnlock:
    case FtcStage::kHeats: return FtcStage::kHeats;
    case FtcStage::kShowcase: return FtcStage::kShowcase;
    // Save 8 after the showcase; Race Central's stage 9 is never saved.
    case FtcStage::kTravelToRaceCentral:
    case FtcStage::kRaceCentralMovie:
    case FtcStage::kWristbandIntro: return FtcStage::kTravelToRaceCentral;
    // Past the rewards the career is over (CSetIsFristTimeCareer false).
    case FtcStage::kRewards:
    case FtcStage::kFirstAutoshow:
    case FtcStage::kSatNavTutorial:
    case FtcStage::kComplete: return FtcStage::kComplete;
  }
  return FtcStage::kOpeningCutscene;
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

void FirstTimeCareer::set_instruction(int title_stage) {
  // The first-time-career HUD instruction picks its strings by the title's
  // stage value: the persistent line, and the large two-line one.
  instruction_.clear();
  large1_.clear();
  large2_.clear();
  large_t_ = 0.0;
  switch (title_stage) {
    case 2: instruction_ = "IDS_HudInstruction_Travel_To_Festival"; break;
    case 3:
      instruction_ = "IDS_HudInstruction_Race_To_Festival";
      large1_ = "IDS_HudMsg_RaceTo";
      large2_ = "IDS_HudMsg_TheFestival";
      break;
    case 5: instruction_ = "IDS_HudInstruction_Travel_To_Workshop"; break;
    case 7:
      instruction_ = "IDS_HudInstruction_Do_All_Comers_Final";
      large1_ = "IDS_HudMsg_EnterThe";
      large2_ = "IDS_HudMsg_HorizonHeats";
      break;
    case 8:
      instruction_ = "IDS_HudInstruction_Travel_To_RaceCentral";
      large1_ = "IDS_HudMsg_DriveTo";
      large2_ = "IDS_HudMsg_RaceCentral";
      break;
    case 12:
      instruction_ = "IDS_HudInstruction_Go_To_Red_Rock";
      large1_ = "IDS_HudMsg_EnterThe";
      large2_ = "IDS_HudMsg_ShowcaseEvent";
      break;
    default: break;
  }
}

void FirstTimeCareer::route_to_next_waypoint(FtcCommands& c) {
  static constexpr std::array<std::array<double, 3>, 3> kRoute = {kHalfWayToTollBooth, kTollBoothPosition,
                                                                   kBeyondTollBoothPosition};
  const auto& p = kRoute[size_t(std::clamp(waypoint_, 0, 2))];
  c.set_destination = true;
  c.destination_label = "IDS_HudInstruction_Race_To_Festival";
  c.destination_xz = {p[0], p[2]};
}

void FirstTimeCareer::route_initial_drive(FtcCommands& c) {
  // The engine's sat-nav takes one destination: the route goes leg by leg
  // through CSetDestForInitialDrive's points (interim: the original routes
  // through them at once).
  const auto& p = kInitialDrivePoints[size_t(std::clamp(drive_leg_, 0, 2))];
  c.set_destination = true;
  c.destination_label = "IDS_HudInstruction_Travel_To_Festival";
  c.destination_xz = {p[0], p[2]};
}

FtcCommands FirstTimeCareer::start() {
  controls_ = true;
  ai_ = false;
  hud_ = false;
  credits_ = false;
  map_locked_ = false;
  in_tod_zone_ = 0;
  return enter(FtcStage::kControls);
}

FtcCommands FirstTimeCareer::resume(FtcStage stage) {
  switch (stage) {
    case FtcStage::kNotStarted:
    case FtcStage::kComplete: stage_ = stage; return {};
    // No save before the diner: the opening restarts as a whole.
    case FtcStage::kControls:
    case FtcStage::kOpeningCutscene:
    case FtcStage::kInitialDrive: return start();
    default: {
      // The festival arrival resumes at its movie.
      const FtcStage at = stage == FtcStage::kFestivalArrive ? FtcStage::kFestivalArriveMovie : stage;
      FtcCommands c = enter(at);
      // The diner's cutscenes start from the diner.
      if (stage == FtcStage::kDinerCutscenes) {
        c.place_car = true;
        c.pos = kStartPosition;
        c.heading_deg = kStartHeading;
      }
      // Before Dak fixes it the radio stays as the opening set it; after, the
      // station's own.
      const bool broken = at < FtcStage::kHeats;
      c.set_radio_dj = true;
      c.radio_dj = !broken;
      c.set_silent_idents = true;
      c.silent_idents = broken;
      c.set_radio_switching = true;
      c.radio_switching = !broken || at == FtcStage::kRadioUnlock;
      return c;
    }
  }
}

void FirstTimeCareer::opening_setup(FtcCommands& c) {
  // intro_flow, under the controller loading screen: radio playlist, the
  // time locked at 09:15, route 170, the Viper placed and given to the AI,
  // the sat-nav to the diner, no traffic.
  c.set_radio_dj = true;
  c.radio_dj = false;
  c.radio_playlist = {"R1_Everyday", "R1_ThePower"};
  c.set_radio_switching = true;
  c.radio_switching = false;
  c.set_silent_idents = true;
  c.silent_idents = true;
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
  drive_leg_ = 0;
  route_initial_drive(c);
  c.set_civilian_traffic = true;
  c.civilian_traffic = false;
  c.preload_cutscene = "Opening_Cutscene";
}

FtcCommands FirstTimeCareer::enter(FtcStage next) {
  stage_ = next;
  stage_t_ = 0.0;
  step_ = 0;
  step_t_ = 0.0;
  timed_.clear();
  set_instruction(-1);
  in_trigger_ = false;
  awaiting_race_ = false;
  FtcCommands c;
  switch (next) {
    case FtcStage::kNotStarted: break;

    case FtcStage::kControls:
      // The controller loading screen stays up while the opening loads.
      opening_setup(c);
      controls_ = false;
      ai_ = true;
      hud_ = false;
      break;

    case FtcStage::kOpeningCutscene:
      // A cut to black, then Opening_Cutscene fading in over 2 s with the
      // opening mix; the DJ's intro line 12.5 s in.
      c.mix_snapshots = {"OpeningCutscene"};
      c.fades = {fade(FtcFadeKind::kBlackOut), fade(FtcFadeKind::kFadeIn, kOpeningFadeIn)};
      c.play_cutscene = "Opening_Cutscene";
      controls_ = false;
      ai_ = true;
      hud_ = false;
      at(12.5, "IntroCutscene");
      break;

    case FtcStage::kInitialDrive:
      // play_intro_camera done: the tutor on, human takeover, traffic back,
      // the camera back on the player, a white flash and the HUD.
      c.fades = {fade(FtcFadeKind::kWhiteFlash)};
      c.set_civilian_traffic = true;
      c.civilian_traffic = true;
      c.mix_snapshots = {"FreeRoam"};
      c.activate_tutor = true;
      controls_ = true;
      ai_ = false;
      hud_ = true;
      diner_preloaded_ = false;
      break;

    case FtcStage::kDinerCutscenes:
      // Finish_Initial_Drive + SetupFestivalDrive: black out, the diner
      // cutscenes with the DJ's E3 intro and the opening titles. Save 1.
      c.save_requested = true;
      c.fades = {fade(FtcFadeKind::kBlackOut)};
      c.mix_snapshots = {"RadioDJSpeaking"};
      c.radio_playlist = {"R1_ThePower"};
      c.play_cutscene = "NEW_Diner_Cutscene_1";
      c.set_civilian_traffic = true;
      c.civilian_traffic = false;
      controls_ = false;
      ai_ = true;
      hud_ = false;
      at(1.5, "E3Intro");
      break;

    case FtcStage::kRaceToFestival:
      // The white flash out of New_Diner_Cutscene_3, the Corrado at the
      // diner, the HUD (the opening titles close with it) and the counter.
      c.fades = {fade(FtcFadeKind::kWhiteFlash)};
      c.swap_car = true;
      c.car_media = kCorradoMedia;
      c.car_id = kCorradoCarId;
      c.place_car = true;
      c.pos = kDinerPlace;
      c.heading_deg = kDinerHeading;
      c.set_civilian_traffic = true;
      c.civilian_traffic = true;
      c.mix_snapshots = {"FreeRoam"};
      controls_ = true;
      ai_ = false;
      hud_ = true;
      credits_ = false;
      set_instruction(3);
      tickets_ = kTicketsStart;
      zones_hit_ = 0;
      dj_counted_ = 0;
      festival_approach_ = false;
      reminders_due_ = false;
      waypoint_ = 0;
      route_to_next_waypoint(c);
      break;

    case FtcStage::kFestivalArrive:
      // FestivalArrive: controls off, the AI drives into the festival under
      // Festival_Race_Outro, then a 2 s fade to black. Save 7.
      c.save_requested = true;
      c.fades = {fade(FtcFadeKind::kWhiteFlash)};
      c.play_cutscene = "Festival_Race_Outro";
      // The AI drives on along route 170 to its last waypoint, in the festival.
      if (!route_waypoints_.empty()) {
        c.set_destination = true;
        c.destination_label = "IDS_HudInstruction_Race_To_Festival";
        c.destination_xz = {route_waypoints_.back()[0], route_waypoints_.back()[2]};
      }
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
      c.mix_snapshots = {"Loading"};
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
      c.mix_snapshots = {"FreeRoam"};
      c.preload_cutscene = "Radio_Unlock";
      c.play_cutscene = "Radio_Unlock";
      c.vo = {"RadioFixedByDak"};
      c.set_radio_switching = true;
      c.radio_switching = true;
      map_locked_ = false;
      controls_ = false;
      hud_ = false;
      break;

    case FtcStage::kHeats:
      // Stage 7: free roam with the heats as the destination; the final
      // starts at its sign-up sphere.
      c.set_civilian_traffic = true;
      c.civilian_traffic = true;
      c.set_time_locked = true;
      c.time_locked = false;
      controls_ = true;
      ai_ = false;
      hud_ = true;
      set_instruction(7);
      for (const auto& t : triggers_)
        if (t.id == kAllComerFinal) {
          c.set_destination = true;
          c.destination_label = "IDS_HudMsg_HorizonHeats";
          c.destination_xz = {t.pos[0], t.pos[2]};
        }
      break;

    case FtcStage::kShowcase:
      // Stages 10 and 12: placed by the heats, 15:48, the Red Rock showcase
      // as the destination. Save 12.
      c.save_requested = true;
      c.place_car = true;
      c.pos = kHeatsFinalPlace;
      c.heading_deg = kHeatsFinalHeading;
      c.set_time_of_day = true;
      c.time_of_day_s = kHeatsFinalTimeOfDay;
      controls_ = true;
      ai_ = false;
      hud_ = true;
      set_instruction(12);
      for (const auto& t : triggers_)
        if (t.id == kMustangRace) {
          c.set_destination = true;
          c.destination_label = "IDS_HudMsg_ShowcaseEvent";
          c.destination_xz = {t.pos[0], t.pos[2]};
        }
      break;

    case FtcStage::kTravelToRaceCentral:
      // Stage 8, save 8.
      c.save_requested = true;
      c.set_destination = true;
      c.destination_label = "IDS_HudMsg_RaceCentral";
      c.destination_xz = {race_central_[0], race_central_[2]};
      controls_ = true;
      ai_ = false;
      hud_ = true;
      set_instruction(8);
      break;

    case FtcStage::kRaceCentralMovie:
      // RaceCentralArrive: a half-second fade out, 08:11, FMV_02.
      c.fades = {fade(FtcFadeKind::kFadeOut, kFadeOutHalf)};
      c.set_time_of_day = true;
      c.time_of_day_s = kRaceCentralTimeOfDay;
      c.preload_cutscene = "Wristband_Intro";
      controls_ = false;
      hud_ = false;
      break;

    case FtcStage::kWristbandIntro:
      c.repair_car = true;
      c.set_civilian_traffic = true;
      c.civilian_traffic = false;
      c.mix_snapshots = {"VOPlayingInstant"};
      c.play_cutscene = "Wristband_Intro";
      c.hold_cutscene = true;  // held until RaceCentralWristbandIntro ends
      controls_ = false;
      hud_ = false;
      break;

    case FtcStage::kRewards:
      c.fades = {fade(FtcFadeKind::kBlackOut)};
      c.mix_snapshots = {"FreeRoam"};
      c.achievement = "ACHIEVEMENT_HORIZON_ROOKIE";
      c.dj_lines = {"festival_wristband"};
      c.give_money = kStartingMoney;
      c.collect_yellow_wristband = true;
      c.intro_completed = true;
      c.save_requested = true;
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
      // Without Kinect the sat-nav popup gives way to the map input hint;
      // Recaro Rush becomes the destination and the clock runs again.
      c.show_map_hint = true;
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
      c.save_requested = true;
      controls_ = true;
      ai_ = false;
      hud_ = true;
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
  if (credits_) credits_t_ += dt;
  if (!large1_.empty() && (large_t_ += dt) >= kLargeHudInstructionDisplayTime) {
    large1_.clear();
    large2_.clear();
  }
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
    if (c.request_time_of_day && !n.set_time_of_day) {
      n.request_time_of_day = true;
      n.tod_request_target_s = c.tod_request_target_s;
      n.tod_request_duration_s = c.tod_request_duration_s;
    }
    return n;
  };
  // An event the activity waits on: its result place moves the career on.
  auto event_result = [&](int64_t id) {
    return in.race_finished && in.race_event_id == id && in.race_place >= 1 && in.race_place <= kCutoffPlace;
  };
  // Drive to an event's sign-up sphere, then start it (each new entry; the
  // host's own event marker can start it too).
  auto sign_up = [&](int64_t id) {
    if (in.racing) {
      raced_ = true;
      return;
    }
    double radius = 0, d = 0;
    const bool have_trigger = event_trigger(id, in.player_pos, radius, d);
    if (awaiting_race_) {
      // A launched event that was declined or quit (it ran, or never
      // started, and reported no result): the sphere re-arms once the
      // player is out of it.
      const bool gave_up = raced_ || step_t_ - launch_t_ > kLaunchGrace;
      if (have_trigger && gave_up && d > radius + kRearmMargin) {
        awaiting_race_ = false;
        in_trigger_ = false;
      }
      return;
    }
    auto launch = [&]() {
      c.launch_event_id = id;
      awaiting_race_ = true;
      raced_ = false;
      launch_t_ = step_t_;
    };
    if (!have_trigger) {
      // No trigger known: launch at once (interim).
      launch();
      return;
    }
    if (d <= radius && !in_trigger_) launch();
    if (d <= radius) in_trigger_ = true;
    else if (d > radius + kRearmMargin) in_trigger_ = false;
  };

  // The time-of-day zones, on entry, up to the festival arrival.
  for (size_t i = 0; i < kTodZones.size(); ++i) {
    const bool now_in = inside(kTodZones[i].zone, in.player_pos);
    const bool was_in = (in_tod_zone_ >> i) & 1u;
    if (now_in) in_tod_zone_ |= 1u << i;
    else in_tod_zone_ &= ~(1u << i);
    if (now_in && !was_in && ftc_title_stage(stage_) <= kTodZonesLastStage) {
      c.request_time_of_day = true;
      c.tod_request_target_s = kTodZones[i].time_s;
      c.tod_request_duration_s = kTodRampTime;
    }
  }

  if ((stage_ == FtcStage::kOpeningCutscene || stage_ == FtcStage::kInitialDrive) && drive_leg_ < 2 &&
      dist_xz(in.player_pos, kInitialDrivePoints[size_t(drive_leg_)]) < kDriveLegRadius) {
    ++drive_leg_;
    route_initial_drive(c);
  }

  switch (stage_) {
    case FtcStage::kControls:
      // The screen leaves once the opening has loaded (the host does not
      // update the career while its world is loading).
      if (step_t_ > 0.0 && stage_t_ > dt) return go(FtcStage::kOpeningCutscene);
      break;

    case FtcStage::kOpeningCutscene:
      if (waited(in.cutscene_active)) return go(FtcStage::kInitialDrive);
      break;

    case FtcStage::kInitialDrive:
      // Approaching the diner: the traffic goes and the diner cutscene loads.
      if (!diner_preloaded_ && std::any_of(kDinerApproach.begin(), kDinerApproach.end(),
                                           [&](const FtcZone& z) { return inside(z, in.player_pos); })) {
        diner_preloaded_ = true;
        c.set_civilian_traffic = true;
        c.civilian_traffic = false;
        c.preload_cutscene = "NEW_Diner_Cutscene_1";
      }
      if (std::any_of(kDinerArrive.begin(), kDinerArrive.end(), [&](const FtcZone& z) { return inside(z, in.player_pos); }))
        return go(FtcStage::kDinerCutscenes);
      break;

    case FtcStage::kDinerCutscenes:
      if (step_ == 0 && waited(in.cutscene_active)) {
        c.play_cutscene = "New_Diner_Cutscene_2";
        credits_ = true;  // ShowOpeningCredits: H_OPENING_TITLES opens and stays
        credits_t_ = 0.0;
        next_step();
      } else if (step_ == 1 && waited(in.cutscene_active)) {
        c.play_cutscene = "New_Diner_Cutscene_3";
        next_step();
      } else if (step_ == 2 && waited(in.cutscene_active)) {
        return go(FtcStage::kRaceToFestival);
      }
      break;

    case FtcStage::kRaceToFestival: {
      // The heats places counter: each ticket zone the player enters takes a
      // place (never under 1); the DJ counts down while the last stretch is
      // ahead; at 1 the giveaway reminders follow.
      for (size_t i = 0; i < kTicketTriggers.size(); ++i) {
        if ((zones_hit_ & (1u << i)) || !inside(kTicketTriggers[i], in.player_pos)) continue;
        zones_hit_ |= 1u << i;
        tickets_ = std::max(kTicketsMin, tickets_ - 1);
        if (!festival_approach_ && tickets_ >= 1 && tickets_ <= 9 && !(dj_counted_ & (1u << tickets_))) {
          dj_counted_ |= 1u << tickets_;
          c.dj_lines.push_back("FestivalTicketsDepleting" + std::to_string(tickets_));
        }
      }
      if (tickets_ <= kTicketsMin && !reminders_due_) {
        reminders_due_ = true;
        at(stage_t_ + kReminderDelay1, "TicketGiveawayReminder1");
        at(stage_t_ + kReminderDelay2, "TicketGiveawayReminder2");
      }
      if (!festival_approach_ && inside(kFestivalApproach, in.player_pos)) {
        festival_approach_ = true;
        c.stop_dj = true;
      }
      // The sat-nav: HalfWay, then (past the free-tickets trigger) the toll
      // booth and beyond it.
      if (waypoint_ == 0 && inside(kFreeTicketsTrigger, in.player_pos)) {
        waypoint_ = 1;
        route_to_next_waypoint(c);
      } else if (waypoint_ == 1 && dist_xz(in.player_pos, kTollBoothPosition) < kWaypointRadius) {
        waypoint_ = 2;
        route_to_next_waypoint(c);
      }
      if (std::any_of(kFestivalArriveZones.begin(), kFestivalArriveZones.end(),
                      [&](const FtcZone& z) { return inside(z, in.player_pos); }))
        return go(FtcStage::kFestivalArrive);
      break;
    }

    case FtcStage::kFestivalArrive:
      if (step_ == 0 && step_t_ >= kOutroHold) {
        c.mix_snapshots = {"FadeOut"};
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
        // The festival arrival flow ends: stage 6, then 7.
        FtcCommands n = go(FtcStage::kHeats);
        n.dj_lines.push_back("CycleRadioLogos");
        // The HUD back, then the DJ on and the idents audible.
        n.set_radio_dj = true;
        n.radio_dj = true;
        n.set_silent_idents = true;
        n.silent_idents = false;
        n.achievement = "ACHIEVEMENT_WELCOME_TO_HORIZON";
        return n;
      }
      break;

    case FtcStage::kHeats:
      if (in.race_finished) awaiting_race_ = false;
      if (event_result(kAllComerFinal)) return go(FtcStage::kShowcase);
      sign_up(kAllComerFinal);
      break;

    case FtcStage::kShowcase:
      if (in.race_finished) awaiting_race_ = false;
      if (event_result(kMustangRace)) {
        FtcCommands n = go(FtcStage::kTravelToRaceCentral);
        n.place_car = true;
        n.pos = kPlaneRacePlace;
        n.heading_deg = kPlaneRaceHeading;
        return n;
      }
      sign_up(kMustangRace);
      break;

    case FtcStage::kTravelToRaceCentral:
      if (dist_xz(in.player_pos, race_central_) < race_central_radius_) return go(FtcStage::kRaceCentralMovie);
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
        c.autoshow_cars.assign(kFirstAutoshowCars.begin(), kFirstAutoshowCars.end());
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
