// FH1's first-time career: the scripted opening a new player goes through
// before free roam, from the controls screen to the first wristband. The
// steps inside each stage follow the original's flow files
// (game_free_roam_flow.xml intro_flow, first_time_career.xml) and tunables
// (gametunablesettings FirstTimeCareer.xml, the GlobalRegistry
// FirstTimeCareer keys); the hand-offs between stages, the trigger zones, the
// ticket counter and the save checkpoints follow the original's
// first-time-career activity (read 2026-11-25; SPEC_QUESTIONS Q62, Q66).
// Headless: the host feeds inputs (time, player position, cutscene / movie /
// race / screen completion) and applies the one-shot commands each update
// returns; the persistent presentation state (controls, HUD, AI driving,
// instructions) is read back through the accessors.
//
// The activity's stages (the title's stage value in brackets): the opening
// and the initial drive [0], the diner [1], the race to the festival [2, 3],
// the festival arrival flow [4, 6], the Horizon Heats [7], the Red Rock
// showcase [10, 12], the drive to Race Central [8] and Race Central [9].
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace race {

enum class FtcStage : uint8_t {
  kNotStarted = 0,
  kControls,             // the controller loading screen while the opening loads
  kOpeningCutscene,      // Opening_Cutscene over the AI-driven Viper
  kInitialDrive,         // white flash, human takeover, the driving tutor
  kDinerCutscenes,       // NEW_Diner_Cutscene_1, New_Diner_Cutscene_2 (opening titles), New_Diner_Cutscene_3
  kRaceToFestival,       // the Corrado on open roads, the heats places counter
  kFestivalArrive,       // Festival_Race_Outro, fade to black
  kFestivalArriveMovie,  // FMV_01
  kWorkshopMap,          // placed at the workshop, the locked map
  kRadioUnlock,          // Dak fixes the radio (Radio_Unlock)
  kHeats,                // drive to the Horizon Heats and finish the final (event 77) in the top 3
  kShowcase,             // the Red Rock showcase (event 101, Mustang against the plane)
  kTravelToRaceCentral,  // drive to Race Central
  kRaceCentralMovie,     // FMV_02
  kWristbandIntro,       // Wristband_Intro until its voice-over ends
  kRewards,              // Horizon Rookie, 40,000 CR, the yellow wristband
  kFirstAutoshow,        // the map with Race Central VO, then the restricted Autoshow
  kSatNavTutorial,       // the map input hint and Recaro Rush as the destination
  kComplete,
};

const char* ftc_stage_name(FtcStage stage);
bool parse_ftc_stage(std::string_view name, FtcStage& out);
// The title's CFirstTimeCareerState stage value while the engine is in `stage`.
int ftc_title_stage(FtcStage stage);

// H_OPENING_TITLES' cards in order: Microsoft, Turn 10, Playground, the
// Horizon logo. The scene's PLAY slide (33 s from ShowOpeningCredits) keys
// each card's opacity curve four times (Q164; read from the curves,
// docs/UI_SCENE_ANIMATION.md). The alpha rises between the first two keys,
// holds, and falls between the last two: the curves' values are all 0, so
// the ramp is inferred (Q186). Returns 0 outside a card's keys and for an
// unknown card.
inline constexpr int kOpeningTitleCards = 4;
inline constexpr double kOpeningTitlesLength = 33.0;
float opening_title_alpha(int card, double t);
// The same ramp over any four key times (seconds), e.g. the scene's own.
float opening_title_alpha(const std::array<double, 4>& keys, double t);

// The presentation states the flow scripts (state_definitions.xml).
enum class FtcFadeKind : uint8_t {
  kBlackOut,    // CBlackOut: cut to black
  kWhiteFlash,  // CWhiteFlash: a white frame fading out
  kFadeIn,      // CFadeIn: black to clear
  kFadeOut,     // CFadeOut: clear to black
};
struct FtcFade {
  FtcFadeKind kind = FtcFadeKind::kBlackOut;
  double duration = 0.0;
  bool s_curve = true;
};

// One-shot commands for the host.
struct FtcCommands {
  // Cutscenes (in-engine, not skippable) and movies (skippable with confirm).
  std::string play_cutscene;  // by name; empty = none
  bool hold_cutscene = false;  // hold-and-loop until released (voice-over exit)
  bool stop_cutscene = false;
  // Stream the world around a cutscene's places before it plays (the
  // activity's GetRidOfTrafficAndLoadCutscene, CPrepareCutscene).
  std::string preload_cutscene;
  std::string play_movie;  // "FMV_01" / "FMV_02"
  std::vector<FtcFade> fades;

  // The player's car.
  bool swap_car = false;
  std::string car_media;
  int64_t car_id = 0;
  bool place_car = false;
  std::array<double, 3> pos{};
  double heading_deg = 0.0;  // FH1 heading: forward (sin h, 0, cos h)
  bool repair_car = false;
  int load_route = -1;  // LoadRoute id (0 clears)

  // World.
  bool set_time_of_day = false;
  double time_of_day_s = 0.0;  // seconds since midnight
  // A time-of-day request: the clock runs forward to `tod_request_target_s`
  // (seconds since midnight; passing midnight when it is behind the clock)
  // over `tod_request_duration_s` real seconds, then holds there
  // (ftc_tod_request gives the target and rate).
  bool request_time_of_day = false;
  double tod_request_target_s = 0.0;
  double tod_request_duration_s = 0.0;
  bool set_time_locked = false;
  bool time_locked = false;
  bool set_civilian_traffic = false;
  bool civilian_traffic = false;

  // Audio.
  std::vector<std::string> radio_playlist;  // set_radio_playlist, in order
  std::vector<std::string> dj_lines;        // RadioDJ requests due now
  bool stop_dj = false;                     // cut the DJ line playing
  std::vector<std::string> vo;              // voice-over triggers
  std::vector<std::string> mix_snapshots;   // AudioMixerSnapshots.xml names, applied in order
  bool set_radio_switching = false;
  bool radio_switching = false;
  // Audio.disable/enable_radio_dj and Audio.enable/disable_radio_silent_idents:
  // until Dak fixes the radio the station plays no DJ links and no idents.
  bool set_radio_dj = false;
  bool radio_dj = false;
  bool set_silent_idents = false;
  bool silent_idents = false;

  // Screens and rewards.
  bool activate_tutor = false;   // CActivateDrivingTutor
  bool open_map = false;         // the map, locked to viewing (no fast travel)
  bool open_autoshow = false;
  // CSetRestrictionsForFirstAutoshow: the Autoshow lists only these cars,
  // previews the first, hides Back and skips the "keep shopping?" prompt.
  bool autoshow_restricted = false;
  std::vector<int64_t> autoshow_cars;
  bool show_map_hint = false;    // the map input hint ("PRESS <Back> TO OPEN THE MAP")
  bool set_destination = false;
  std::string destination_label;
  std::array<double, 2> destination_xz{};
  int64_t launch_event_id = 0;
  int64_t give_money = 0;
  std::string achievement;
  bool collect_yellow_wristband = false;
  bool intro_completed = false;  // introSequenceCompleted / CSetIsFristTimeCareer false
  bool enter_free_roam = false;
  bool save_requested = false;   // a save checkpoint (the activity's save stages 1, 7, 12, 8)
};

struct FtcInput {
  double dt = 0.0;
  std::array<double, 3> player_pos{};
  bool cutscene_active = false;  // the host's cutscene player is running
  bool movie_active = false;
  double throttle = 0.0;  // 0..1
  bool confirm = false;   // A / Enter pressed this frame
  bool screen_open = false;  // the map / Autoshow the flow opened is still open
  bool race_finished = false;  // edge: a race the player was in just finished
  int64_t race_event_id = 0;
  int race_place = 0;  // 1-based
  bool racing = false;  // the player is in a race (the heats triggers wait)
};

// A sphere trigger (the activity's CTriggerZone, shape 1).
struct FtcZone {
  std::array<double, 3> pos;
  double radius;
};

// The time-of-day request a zone makes at `now_s`: the target moved past
// midnight when it is behind the clock (+86,400 s), and the rate in game
// seconds per real second that reaches it in `duration_s`.
struct FtcTodRequest {
  double target_s = 0.0;
  double rate = 0.0;
};
FtcTodRequest ftc_tod_request(double now_s, double target_s, double duration_s);

class FirstTimeCareer {
 public:
  // FirstTimeCareer.xml.
  static constexpr std::array<double, 3> kStartPosition = {1562.52, 117.73, -1901.47};
  static constexpr double kStartHeading = 180.0;
  static constexpr double kInitialTimeOfDay = 33840.0;  // 09:24
  static constexpr double kIntroTimeOfDay = 33300.0;    // 09:15 (intro_flow)
  static constexpr double kRaceCentralTimeOfDay = 29460.0;  // 08:11
  static constexpr int64_t kAllComerFinal = 77;  // AllComerFinal (FESTIVAL_SECOND); Race1ID 76 has no event
  static constexpr int64_t kMustangRace = 101;   // MustangRace (PLANE_RACE_001, the Red Rock showcase)
  // The activity moves on when the event's result place is under 4.
  static constexpr int kCutoffPlace = 3;
  static constexpr std::array<double, 3> kHalfWayToTollBooth = {225.43, 116.76, -3323.21};
  static constexpr std::array<double, 3> kTollBoothPosition = {-800.06, 30.05, -1063.63};
  static constexpr std::array<double, 3> kBeyondTollBoothPosition = {-915.37, 13.75, -885.44};
  static constexpr std::array<double, 3> kEndRacePosition = {-715.03, 42.56, -1250.56};
  static constexpr FtcZone kFreeTicketsTrigger = {{207.42, 116.10, -3332.04}, 10.0};
  // TicketTriggers: each one the player enters takes a place off the counter.
  static constexpr std::array<FtcZone, 6> kTicketTriggers = {{
      {{1389.10, 113.91, -2221.42}, 10.0},
      {{1252.56, 124.64, -2572.83}, 10.0},
      {{803.55, 121.71, -3115.07}, 10.0},
      {{-240.76, 103.61, -3348.91}, 15.0},
      {{-602.16, 88.36, -2973.12}, 10.0},
      {{-778.72, 73.57, -2466.95}, 10.0},
  }};
  // The activity's own zones.
  static constexpr std::array<FtcZone, 2> kDinerArrive = {{{{1709.0, 117.8, -1718.0}, 10.0}, {{1486.0, 115.0, -2024.0}, 10.0}}};
  static constexpr std::array<FtcZone, 2> kDinerApproach = {{{{1890.0, 118.6, -1503.0}, 20.0}, {{938.0, 119.7, -2927.0}, 20.0}}};
  static constexpr std::array<FtcZone, 2> kFestivalArriveZones = {{{kEndRacePosition, 20.0}, {{-945.9, -1.0, -647.2}, 20.0}}};
  static constexpr FtcZone kFestivalApproach = {{-701.4, 46.19, -1501.0}, 20.0};
  static constexpr double kHeatsSignUpRadius = 27.0;  // the FESTIVAL_SECOND_NODE race activation
  // The activity's two time-of-day zones (20 m spheres): entering one while
  // the title's stage value is 4 or less runs the clock forward to its time
  // over kTodRampTime. The one on the race to the festival goes to 18:00,
  // the one on the initial drive (past the second sat-nav point) to 14:30.
  struct TodZone {
    FtcZone zone;
    double time_s;
  };
  static constexpr std::array<TodZone, 2> kTodZones = {{
      {{{-116.0, 107.0, -3377.0}, 20.0}, 64800.0},
      {{{2284.42, 109.87, 398.12}, 20.0}, 52200.0},
  }};
  static constexpr double kTodRampTime = 45.0;
  static constexpr int kTodZonesLastStage = 4;
  // Where the activity puts the player after each event.
  static constexpr std::array<double, 3> kHeatsFinalPlace = {-3196.8, 4.18, -1465.5};
  static constexpr double kHeatsFinalHeading = -117.78;
  static constexpr double kHeatsFinalTimeOfDay = 56880.0;  // 15:48
  static constexpr std::array<double, 3> kPlaneRacePlace = {-5423.0, 74.1, -3181.0};
  static constexpr double kPlaneRaceHeading = -6.77;
  // PopularitySkillsFlow (game_free_roam_flow.xml), back in free roam after
  // the showcase: 18:40 (SetTimeOfDayPostPlaneRace), Skills_Unlock over the
  // RedRockSkillsIntro voice-over. Skills score from then on.
  static constexpr double kPostPlaneRaceTimeOfDay = 67200.0;
  static constexpr const char* kSkillsCutscene = "Skills_Unlock";
  static constexpr const char* kSkillsVo = "RedRockSkillsIntro";
  // intro_flow / first_time_career.xml placements.
  static constexpr std::array<double, 3> kViperStart = {-1361.97, 47.32, 1881.99};
  static constexpr double kViperHeading = 94.07;
  static constexpr int kInitialDriveRoute = 170;
  // CSetDestForInitialDrive's three points: the sat-nav goes through two
  // to the last, near the diner. In route order (interim, Q66): from the
  // opening's road CO-20 passes (1552, 1436) about 490 m before
  // (1881, 1077); the other order sends the car there and back.
  static constexpr std::array<std::array<double, 3>, 3> kInitialDrivePoints = {{
      {1552.28, 168.99, 1435.91},
      {1881.06, 153.76, 1076.59},
      {1576.62, 119.08, -1839.37},
  }};
  static constexpr std::array<double, 3> kInitialDriveDestination = kInitialDrivePoints[2];
  static constexpr std::array<double, 3> kDinerPlace = {1553.881226, 118.073807, -1934.264526};
  static constexpr double kDinerHeading = -120.859550;
  static constexpr std::array<double, 3> kWorkshopPlace = {-1027.26, -8.68, -145.57};
  static constexpr double kWorkshopHeading = 100.0;
  // Wristband_Intro's player placement: Race Central when the host gives no site.
  static constexpr std::array<double, 3> kRaceCentral = {-832.17, -6.02, -254.29};
  static constexpr const char* kViperMedia = "VIP_Viper_13";
  static constexpr int64_t kViperCarId = 1544;
  // The opening Viper's paint: yellow (0xFDCF03) with a black stripe,
  // Combo_Colors sequence 7 (read from the original's opening; Q271).
  static constexpr int64_t kViperColourSequence = 7;
  static constexpr const char* kCorradoMedia = "VW_Corrado_95";
  static constexpr int64_t kCorradoCarId = 309;
  // The Corrado's paint: red (0xDC001F), Combo_Colors sequence 2 (the
  // original's diner and race to the festival; Q271).
  static constexpr int64_t kCorradoColourSequence = 2;
  static constexpr int64_t kStartingMoney = 40000;  // CGivePlayerMoney
  // A new profile's credits: the profile schema's default
  // (media/profileschema/ForzaProfile.sch, Credits = 500). The career pays
  // nothing before the wristband bonus.
  static constexpr int64_t kNewProfileCredits = 500;
  // The first Autoshow's car list (Subaru WRX STi 2011, Mitsubishi Lancer Evo X GSR 2008).
  static constexpr std::array<int64_t, 2> kFirstAutoshowCars = {1323, 1009};
  static constexpr const char* kRecaroRush = "FR02";
  // Interim (Q66): Race Central's arrival radius when the host gives no site.
  static constexpr double kRaceCentralRadius = 40.0;
  // The heats places counter (CHudStatusTicketCountdown): 10, never under 1.
  static constexpr int kTicketsStart = 10, kTicketsMin = 1;
  static constexpr double kReminderDelay1 = 30.0, kReminderDelay2 = 60.0;
  // GlobalRegistry UI/FirstTimeCareer.
  static constexpr double kLargeHudInstructionDisplayTime = 3.0;

  // The host's world: where events 77 / 101 sign up and where Race Central
  // is (the career catalogue's triggers). Without them the events launch at
  // once and Race Central uses kRaceCentral / kRaceCentralRadius.
  void set_event_trigger(int64_t event_id, const std::array<double, 3>& pos, double radius);
  // The sign-up place set for `event_id`; false when none was set.
  bool event_trigger(int64_t event_id, std::array<double, 3>& pos) const {
    for (const auto& t : triggers_)
      if (t.id == event_id) return pos = t.pos, true;
    return false;
  }
  void set_race_central(const std::array<double, 3>& pos, double radius);
  // Route 170's waypoints (route_waypoint_NN in order). The route is the
  // race to the festival's: its grid is at the diner and its waypoints run
  // through the toll booth into the festival. When the AI takes the car at
  // the festival it drives on to the route's last waypoint.
  void set_route_waypoints(std::vector<std::array<double, 3>> waypoints) { route_waypoints_ = std::move(waypoints); }

  // Starts (or restarts) the career at the controls screen.
  FtcCommands start();
  // Resumes at the start of `stage` (a save's stage); kNotStarted / kComplete are no-ops.
  FtcCommands resume(FtcStage stage);
  FtcCommands update(const FtcInput& in);

  FtcStage stage() const { return stage_; }
  double stage_time() const { return stage_t_; }
  bool active() const { return stage_ != FtcStage::kNotStarted && stage_ != FtcStage::kComplete; }
  bool complete() const { return stage_ == FtcStage::kComplete; }
  // The stage a save made now resumes at: the activity's last save checkpoint
  // (none before the diner, so the opening replays).
  FtcStage checkpoint() const;

  // Persistent presentation state.
  bool show_controls_screen() const { return stage_ == FtcStage::kControls; }
  bool controls_enabled() const { return controls_; }
  bool ai_driving() const { return ai_; }
  bool hud_visible() const { return hud_; }
  // H_OPENING_TITLES: from New_Diner_Cutscene_2 until the HUD comes back.
  bool show_opening_credits() const { return credits_; }
  double opening_credits_time() const { return credits_ ? credits_t_ : 0.0; }
  bool map_locked() const { return map_locked_; }
  // Driving skills score: not in the career's opening, from the end of
  // PopularitySkillsFlow (after the showcase) on, and outside the career.
  bool skills_unlocked() const { return !active() || (stage_ > FtcStage::kShowcase && !skills_cutscene_); }
  // String ids (InGame): the stage's persistent HUD instruction, and the
  // large two-line instruction shown for LargeHudInstructionDisplayTime as a
  // stage starts; empty when none.
  const std::string& hud_instruction() const { return instruction_; }
  const std::string& large_instruction_1() const { return large1_; }
  const std::string& large_instruction_2() const { return large2_; }
  // The heats places counter (-1 when not shown).
  int tickets_remaining() const { return stage_ == FtcStage::kRaceToFestival ? tickets_ : -1; }

 private:
  FtcCommands enter(FtcStage next);
  void opening_setup(FtcCommands& c);
  void at(double t, std::string dj);  // a DJ line due `t` seconds into the stage
  void pump_timed(FtcCommands& out);
  void set_instruction(int title_stage);
  void route_to_next_waypoint(FtcCommands& c);
  void route_initial_drive(FtcCommands& c);
  // The event's sign-up sphere and the player's distance to it; false when unknown.
  bool event_trigger(int64_t id, const std::array<double, 3>& p, double& radius, double& distance) const;

  FtcStage stage_ = FtcStage::kNotStarted;
  double stage_t_ = 0.0;
  int step_ = 0;
  double step_t_ = 0.0;
  bool controls_ = true, ai_ = false, hud_ = true, credits_ = false, map_locked_ = false;
  double credits_t_ = 0.0;
  std::string instruction_, large1_, large2_;
  double large_t_ = 0.0;
  int tickets_ = kTicketsStart;
  uint32_t zones_hit_ = 0;      // ticket zones already taken (bit per zone)
  uint32_t in_tod_zone_ = 0;    // inside a time-of-day zone (bit per zone; requests on entry)
  uint32_t dj_counted_ = 0;     // FestivalTicketsDepleting<n> already played (bit n)
  bool festival_approach_ = false;  // the player passed the last stretch's zone
  bool reminders_due_ = false;
  bool diner_preloaded_ = false;
  bool skills_cutscene_ = false;  // Skills_Unlock playing (PopularitySkillsFlow)
  int waypoint_ = 0;            // the race's sat-nav route: HalfWay, TollBooth, Beyond
  int drive_leg_ = 0;           // the initial drive's sat-nav point (kInitialDrivePoints)
  bool in_trigger_ = false;     // inside the event's sign-up sphere (edge)
  bool awaiting_race_ = false;  // an event was launched and has not reported
  bool raced_ = false;          // ...and the player has been in it since
  double launch_t_ = 0.0;
  struct Trigger {
    int64_t id;
    std::array<double, 3> pos;
    double radius;
  };
  std::vector<Trigger> triggers_;
  std::vector<std::array<double, 3>> route_waypoints_;
  bool have_race_central_ = false;
  std::array<double, 3> race_central_ = kRaceCentral;
  double race_central_radius_ = kRaceCentralRadius;
  struct Timed {
    double t;
    std::string dj;
  };
  std::vector<Timed> timed_;
};

}  // namespace race
