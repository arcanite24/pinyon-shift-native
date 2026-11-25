// FH1's first-time career: the scripted opening a new player goes through
// before free roam, from the controls screen to the HORIZON Heats and the
// first wristband. The order and the per-stage steps follow the original's
// flow files (game_free_roam_flow.xml intro_flow, first_time_career.xml) and
// tunables (gametunablesettings FirstTimeCareer.xml, First50TicketsDistances,
// the GlobalRegistry FirstTimeCareer keys). Headless: the host feeds inputs
// (time, player position, cutscene / movie / race / screen completion) and
// applies the one-shot commands each update returns; the persistent
// presentation state (controls, HUD, AI driving, instructions) is read back
// through the accessors.
//
// Stages whose rule the flow files do not state are marked interim here and
// in docs/SPEC_QUESTIONS.md (Q62 race-to-festival scoring, Q63 tutor
// prompts, Q64 first Autoshow restriction, Q65 opening credits, Q66 stage
// order around the Heats and the stage triggers).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace race {

enum class FtcStage : uint8_t {
  kNotStarted = 0,
  kControls,             // the controller loading screen
  kOpeningCutscene,      // Opening_Cutscene over the AI-driven Viper (route 170)
  kInitialDrive,         // white flash, human takeover, "hold RT" tutor prompt
  kDinerCutscenes,       // NEW_Diner_Cutscene_1, New_Diner_Cutscene_2 (credits), New_Diner_Cutscene_3
  kRaceToFestival,       // the Corrado on open roads, the DJ's ticket countdown
  kFestivalArrive,       // Festival_Race_Outro, fade to black
  kFestivalArriveMovie,  // FMV_01
  kWorkshopMap,          // placed at the workshop, the locked map
  kRadioUnlock,          // Dak fixes the radio (Radio_Unlock)
  kHeats,                // the All Comers heats (events 76, 77)
  kTravelToRaceCentral,  // drive to Race Central
  kRaceCentralMovie,     // FMV_02
  kWristbandIntro,       // Wristband_Intro until its voice-over ends
  kRewards,              // Horizon Rookie, 40,000 CR, the yellow wristband
  kFirstAutoshow,        // the map with Race Central VO, then the restricted Autoshow
  kSatNavTutorial,       // the sat-nav hint and Recaro Rush as the destination
  kComplete,
};

const char* ftc_stage_name(FtcStage stage);
bool parse_ftc_stage(std::string_view name, FtcStage& out);

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
  bool set_time_locked = false;
  bool time_locked = false;
  bool set_civilian_traffic = false;
  bool civilian_traffic = false;

  // Audio.
  std::vector<std::string> radio_playlist;  // set_radio_playlist, in order
  std::vector<std::string> dj_lines;        // RadioDJ requests due now
  std::vector<std::string> vo;              // voice-over triggers
  std::vector<std::string> mix_snapshots;
  bool set_radio_switching = false;
  bool radio_switching = false;

  // Screens and rewards.
  bool open_map = false;         // the map, locked to viewing (no fast travel)
  bool open_autoshow = false;
  bool autoshow_restricted = false;  // CSetRestrictionsForFirstAutoshow
  bool show_satnav_hint = false;
  bool set_destination = false;
  std::string destination_label;
  std::array<double, 2> destination_xz{};
  int64_t launch_event_id = 0;
  int64_t give_money = 0;
  std::string achievement;
  bool collect_yellow_wristband = false;
  bool intro_completed = false;  // introSequenceCompleted / CSetIsFristTimeCareer false
  bool enter_free_roam = false;
  bool save_requested = false;
};

struct FtcInput {
  double dt = 0.0;
  std::array<double, 3> player_pos{};
  bool cutscene_active = false;  // the host's cutscene player is running
  bool movie_active = false;
  double throttle = 0.0;  // 0..1
  bool confirm = false;   // A / Enter pressed this frame
  bool screen_open = false;  // the map / Autoshow the flow opened is still open
  bool race_finished = false;  // edge: the race the flow launched just finished
  int64_t race_event_id = 0;
  int race_place = 0;  // 1-based
  bool route_finished = false;  // the initial drive's route reached its end
};

struct FtcTicketThreshold {
  double remaining_fraction;
  int tickets;
};

class FirstTimeCareer {
 public:
  // FirstTimeCareer.xml.
  static constexpr std::array<double, 3> kStartPosition = {1562.52, 117.73, -1901.47};
  static constexpr double kStartHeading = 180.0;
  static constexpr double kInitialTimeOfDay = 33840.0;  // 09:24
  static constexpr double kIntroTimeOfDay = 33300.0;    // 09:15 (intro_flow)
  static constexpr double kRaceCentralTimeOfDay = 29460.0;  // 08:11
  static constexpr int kRacePointsRequired = 6;
  static constexpr int64_t kAllComerRace1 = 76, kAllComerRace2 = 77, kAllComerFinal = 77;
  static constexpr int kHeatCutoffPlace = 3;  // interim (Q66)
  static constexpr std::array<double, 3> kEndRacePosition = {-715.03, 42.56, -1250.56};
  // intro_flow / first_time_career.xml placements.
  static constexpr std::array<double, 3> kViperStart = {-1361.97, 47.32, 1881.99};
  static constexpr double kViperHeading = 94.07;
  static constexpr int kInitialDriveRoute = 170;
  static constexpr std::array<double, 3> kDinerPlace = {1553.881226, 118.073807, -1934.264526};
  static constexpr double kDinerHeading = -120.859550;
  static constexpr std::array<double, 3> kWorkshopPlace = {-1027.26, -8.68, -145.57};
  static constexpr double kWorkshopHeading = 100.0;
  // Wristband_Intro's player placement, the stand-in for Race Central's trigger.
  static constexpr std::array<double, 3> kRaceCentral = {-832.17, -6.02, -254.29};
  static constexpr const char* kViperMedia = "VIP_Viper_13";
  static constexpr int64_t kViperCarId = 1544;
  static constexpr const char* kCorradoMedia = "VW_Corrado_95";
  static constexpr int64_t kCorradoCarId = 309;
  static constexpr int64_t kStartingMoney = 40000;  // CGivePlayerMoney
  static constexpr const char* kRecaroRush = "FR02";
  // Interim arrival radii (Q66).
  static constexpr double kArriveRadius = 40.0;
  // GlobalRegistry FirstTimeCareer/TutorialMessages.
  static constexpr double kAccelerateTextTimerLimit = 3.0;
  static constexpr double kLargeHudInstructionDisplayTime = 3.0;
  // First50TicketsDistances.
  static constexpr std::array<FtcTicketThreshold, 10> kTickets = {{
      {0.90, 10}, {0.80, 9}, {0.70, 8}, {0.60, 7}, {0.50, 6}, {0.40, 5}, {0.30, 4}, {0.20, 3}, {0.10, 2}, {0.00, 2}}};
  static int tickets_for_remaining(double remaining_fraction);

  // Starts (or restarts) the career at the controls screen.
  FtcCommands start();
  // Resumes at the start of `stage` (a save's stage); kNotStarted / kComplete are no-ops.
  FtcCommands resume(FtcStage stage);
  FtcCommands update(const FtcInput& in);

  FtcStage stage() const { return stage_; }
  double stage_time() const { return stage_t_; }
  bool active() const { return stage_ != FtcStage::kNotStarted && stage_ != FtcStage::kComplete; }
  bool complete() const { return stage_ == FtcStage::kComplete; }

  // Persistent presentation state.
  bool show_controls_screen() const { return stage_ == FtcStage::kControls; }
  bool controls_enabled() const { return controls_; }
  bool ai_driving() const { return ai_; }
  bool hud_visible() const { return hud_; }
  bool show_opening_credits() const { return credits_; }
  bool map_locked() const { return map_locked_; }
  // String ids (UI string table) of the large HUD instruction and the tutor
  // prompt on screen, empty when none.
  const std::string& hud_instruction() const { return instruction_; }
  const std::string& tutor_prompt() const { return tutor_; }
  // The race to the festival's ticket counter (-1 outside that stage).
  int tickets_remaining() const { return stage_ == FtcStage::kRaceToFestival ? tickets_ : -1; }

 private:
  FtcCommands enter(FtcStage next);
  void at(double t, std::string dj);  // a DJ line due `t` seconds into the stage
  void pump_timed(FtcCommands& out);

  FtcStage stage_ = FtcStage::kNotStarted;
  double stage_t_ = 0.0;
  int step_ = 0;
  double step_t_ = 0.0;
  bool controls_ = true, ai_ = false, hud_ = true, credits_ = false, map_locked_ = false;
  std::string instruction_, tutor_;
  double instruction_t_ = 0.0;
  double throttle_held_ = 0.0;
  int tickets_ = 10;
  double race_total_ = 1.0;
  int heat_ = 0;
  bool awaiting_race_ = false;  // a heat was launched and has not reported
  struct Timed {
    double t;
    std::string dj;
  };
  std::vector<Timed> timed_;
};

}  // namespace race
