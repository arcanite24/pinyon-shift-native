// Playable FH1 Opening Career Sequence (Slice 9):
//   Boot Splash FMVs (honouring Skip Intro Movies & Start in Single Player)
//   -> FMV_01 Opening Montage
//   -> SRT Viper GTS (VIP_Viper_13) highway drive to the Horizon Festival
//      with Alice / Scott Tyler radio immediate beats & distance countdown
//   -> VW Corrado VR6 (VW_Corrado_95) qualification race (FESTIVAL_SECOND, route 16)
//   -> Alice / Dak / Scott festival intro beats + WristbandGet ceremony FMV
//   -> Autoshow hand-off & starter car/credits grant.
// Cleanly supports per-stage skip & fast-forward for players and tests.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "fmv_player.h"
#include "ui.h"

namespace carview {

// The boot splashes the original plays before Press Start, in its order
// (BOOT-01): every recorded boot opens T10_MS_Combined, Dolby_Corona_Intro,
// then PressStart.wmv, and nothing else.
inline constexpr std::array<FmvMovieId, 2> kBootSplashes = {FmvMovieId::kT10MsCombined, FmvMovieId::kDolbyCorona};
// The title's attract movie (BOOT-06): after kAttractIdleSeconds idle on
// Press Start the original replays forza_intro.wmv, then reloads the
// press-start scene.
inline constexpr FmvMovieId kAttractMovie = FmvMovieId::kForzaIntro;
inline constexpr double kAttractIdleSeconds = 53.0;
// The legal disclaimer before the first splash (BOOT-07, Q276): the
// P_STA_Disclaimer texture (Textures.zip P_Map/P_STA/<language>/) full
// screen on black. Its time is the capture's (about 3 s; interim).
inline constexpr double kDisclaimerSeconds = 3.0;
// The disclaimer's Textures.zip key for a language ("EN" when the language
// has no copy of its own).
std::string disclaimer_texture(std::string_view language);

enum class StoryStage {
  kInactive = 0,
  kBootSplash,
  kOpeningFmv,
  kViperDrive,
  kCorradoQualify,
  kFestivalIntroBeats,
  kAutoshowHandoff,
  kCompleted,
};

struct StoryBeat {
  std::string speaker;      // "ALICE HART", "SCOTT TYLER", "DAK"
  std::string radio_event;  // RadioSystem.xml DJImmediateDialogue gameplayEvent (optional)
  std::string vo_trigger;   // DialogueScript_*.xml Trigger id (optional)
  std::string caption;      // Localized/English subtitle caption
  double trigger_dist_m = -1.0;  // >= 0: fires when distance <= trigger_dist_m
  double trigger_time_s = 0.0;   // fallback timer in stage
  double duration_s = 6.0;
};

struct StoryAction {
  bool swap_car = false;
  std::string car_media;
  int car_id = 0;
  bool teleport = false;
  std::array<double, 3> pos{};
  double heading = 0.0;
  bool set_gps_target = false;
  std::array<double, 2> gps_xz{};
  bool launch_qualify_race = false;
  int qualify_route_id = 16;  // FESTIVAL_SECOND qualification route
  bool play_fmv = false;
  FmvMovieId fmv_id = FmvMovieId::kFmv01Opening;
  std::string radio_immediate_event;
  std::string vo_trigger;
  bool open_autoshow = false;
  bool grant_starter = false;
  bool enter_title_screen = false;
  bool intro_finished = false;
  std::string onboarding_banner;
};

class StoryIntro {
 public:
  // Canonical starter/opening vehicles from Data_Car.
  static constexpr const char* kViperMedia = "VIP_Viper_13";
  static constexpr int kViperCarId = 1544;
  static constexpr const char* kCorradoMedia = "VW_Corrado_95";
  static constexpr int kCorradoCarId = 309;
  static constexpr int kQualifyRouteId = 16;  // FESTIVAL_SECOND qualification route
  static constexpr int kConvoyTarget = 5;     // Festival-bound convoy cars to overtake on Viper drive
  static constexpr double kFestivalArrivalRadiusM = 120.0;

  // Starts boot splash sequence (respects `skip_intro_movies` and
  // `start_in_single_player`). Returns actions to apply immediately.
  StoryAction begin_boot(const std::filesystem::path& game_root, bool skip_intro_movies,
                         bool start_in_single_player, bool needs_new_career_intro);

  // Starts a new career from the opening FMV -> Viper drive -> Corrado qualify.
  StoryAction begin_new_career(const std::filesystem::path& game_root, bool skip_intro_movies,
                               std::array<double, 3> festival_gate = {-1360.0, 2.0, -210.0});

  // Advances the active story stage by `dt` seconds given the player's current
  // XZ position and whether the active FMV or qualification race has finished.
  StoryAction update(double dt, std::array<double, 2> player_xz, bool fmv_active, bool race_finished);

  // Advances past the current beat / FMV / stage cleanly.
  StoryAction skip_stage();
  // Skips the entire intro straight to Autoshow hand-off / free roam.
  StoryAction skip_all();
  // The radio and VO beats a stage plays (empty for stages without beats).
  static std::vector<StoryBeat> beats_for(StoryStage stage);
  // Advances directly to a target stage (for tests and headless capture).
  StoryAction advance_to(StoryStage stage);
  void reset() {
    stage_ = StoryStage::kInactive;
    splash_queue_.clear();
    disclaimer_left_ = 0.0;
    beats_.clear();
    active_speaker_.clear();
    active_caption_.clear();
    caption_timer_ = 0.0;
  }

  StoryStage stage() const { return stage_; }
  // The boot's legal disclaimer is up (before the first splash).
  bool disclaimer_active() const { return stage_ == StoryStage::kBootSplash && disclaimer_left_ > 0.0; }
  bool active() const { return stage_ != StoryStage::kInactive && stage_ != StoryStage::kCompleted; }
  bool completed() const { return stage_ == StoryStage::kCompleted; }
  double stage_time() const { return stage_time_; }
  double distance_to_festival() const { return dist_to_festival_; }
  void on_convoy_pass(int delta = 1) {
    if (stage_ == StoryStage::kViperDrive && delta > 0)
      convoy_passed_ = std::min(kConvoyTarget, convoy_passed_ + delta);
  }
  int convoy_passed() const { return convoy_passed_; }
  int convoy_target() const { return kConvoyTarget; }
  const std::string& active_speaker() const { return active_speaker_; }
  const std::string& active_caption() const { return active_caption_; }
  const std::string& objective_title() const { return objective_title_; }
  const std::string& objective_detail() const { return objective_detail_; }
  int splash_index() const { return splash_idx_; }
  const std::vector<FmvMovieId>& splash_queue() const { return splash_queue_; }
  int beat_index() const { return beat_idx_; }

  static const char* stage_name(StoryStage s);

  // Draws the Viper/Corrado/Festival story objective HUD + VO caption bar.
  void set_metric_units(bool metric) { metric_units_ = metric; }
  bool metric_units() const { return metric_units_; }
  void draw_hud(ui::List& l, float screen_w, float screen_h) const { draw_hud(l, screen_w, screen_h, metric_units_); }
  void draw_hud(ui::List& l, float screen_w, float screen_h, bool metric_units) const;

 private:
  StoryAction enter_stage(StoryStage next);
  void populate_beats_for(StoryStage s);

  StoryStage stage_ = StoryStage::kInactive;
  std::filesystem::path game_root_;
  bool skip_movies_ = false;
  bool start_sp_ = false;
  bool pending_career_intro_ = false;

  // Verified Colorado highway asphalt start (COLLISION.md measured free-roam spawn).
  std::array<double, 3> viper_spawn_ = {2002.96, 143.38, 960.00};
  double viper_heading_ = 2.20073;
  std::array<double, 3> festival_gate_ = {-1360.0, 2.0, -210.0};
  double dist_to_festival_ = 3560.0;

  std::vector<FmvMovieId> splash_queue_;
  int splash_idx_ = 0;
  double disclaimer_left_ = 0.0;

  std::vector<StoryBeat> beats_;
  int beat_idx_ = 0;
  double stage_time_ = 0.0;
  double caption_timer_ = 0.0;
  std::string active_speaker_;
  std::string active_caption_;
  std::string objective_title_;
  std::string objective_detail_;
  bool qualify_launched_ = false;
  bool ceremony_fmv_triggered_ = false;
  int convoy_passed_ = 0;
  bool metric_units_ = true;
};

}  // namespace carview
