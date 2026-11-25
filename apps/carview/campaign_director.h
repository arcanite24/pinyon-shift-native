// Campaign Director for carview: bridges race::CampaignFlow with FmvPlayer,
// AudioHost (VO + Radio DJ + automatic music/engine ducking), DriveSave
// persistence, hold-to-skip input, and smooth cubic-bezier in-engine cinematic
// camera splines for Wristband Ceremonies, Boss Showdowns, Showcases, Barn
// Finds, Pre-Race Grid Flybys, Post-Race Finish Cameras, and the Horizon
// Festival Finale & Credits.
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include "cutscene_runtime.h"
#include "drive/cameras.h"
#include "drive/save.h"
#include "fmv_player.h"
#include "race/campaign_flow.h"
#include "race/first_time_career.h"

namespace carview {

class AudioHost;

// Evaluates a 3D cubic Bezier curve at parameter t in [0, 1].
std::array<double, 3> eval_cubic_bezier(const std::array<double, 3>& p0, const std::array<double, 3>& p1,
                                        const std::array<double, 3>& p2, const std::array<double, 3>& p3, double t);

class CampaignDirector {
 public:
  void configure(const std::filesystem::path& game_root, std::string_view language, bool skip_intro_movies);

  // Syncs campaign progression from/to DriveSave cleanly.
  void load_from_save(const drive::DriveSave& save);
  void save_to_save(drive::DriveSave& save) const;

  // ---- First-time career (the original's scripted opening) ----
  // A new career runs race::FirstTimeCareer instead of the onboarding stages
  // of CampaignFlow; those stay only to read old saves.
  struct FtcFrameInput {
    std::array<double, 3> player_pos{};
    double throttle = 0.0;
    bool screen_open = false;  // the map / Autoshow the flow opened
    bool loading = false;      // a loading screen or a world gather is up: the flow waits
  };
  void set_ftc_frame_input(const FtcFrameInput& in) { ftc_in_ = in; }
  bool ftc_active() const { return ftc_.active(); }
  const race::FirstTimeCareer& ftc() const { return ftc_; }
  // The flow's commands the host applies beyond CampaignAction (time of day,
  // traffic, money, achievement, map, sat-nav hint, repair), oldest first.
  std::vector<race::FtcCommands> take_ftc_commands();
  // A cutscene's PlacePlayerCar since the last call (engine heading, m/s).
  bool take_place_car(std::array<double, 3>& pos, double& heading_rad, double& speed_mps);
  CutsceneRuntime& cutscenes() { return cutscenes_; }
  const CutsceneRuntime& cutscenes() const { return cutscenes_; }
  // Capture entry point (`--cutscene <name> --cutscene-time <t>`): plays the
  // cutscene frozen at t, with the place-car events up to t applied.
  bool start_cutscene_capture(std::string_view name, double t);
  // When a cut frames a spot far from the player car (a camera node), where
  // to put the car so the world streams around the camera: behind the eye,
  // out of frame. Once per cut.
  bool take_stream_anchor(const std::array<double, 3>& car_pos, double car_heading_rad, std::array<double, 3>& pos,
                          double& heading_rad);
  // The black / white overlay of the cutscene fades, drawn over everything.
  cutscene::Overlay screen_overlay() const { return cutscenes_.overlay(); }

  // Starts the authentic Minute-0 New Career flow.
  race::CampaignAction start_new_career(FmvPlayer& fmv, AudioHost* audio,
                                        std::array<double, 3> festival_gate = {-1360.0, 2.0, -210.0});

  // Deterministic CLI stage jump (`--campaign-stage <stage>`).
  bool apply_cli_stage(std::string_view cli_stage, FmvPlayer& fmv, AudioHost* audio, race::CampaignAction& out_action);

  // Per-frame tick: updates CampaignFlow, dispatches VO/radio cues & ducking,
  // advances hold-to-skip, and launches FMVs when requested.
  race::CampaignAction update(double dt, std::array<double, 2> player_xz, FmvPlayer& fmv, AudioHost* audio,
                              bool race_finished, bool holding_skip);

  // Instant advance / skip (e.g. pressing Enter/A/Tab on a cinematic or tally).
  race::CampaignAction advance_or_skip(FmvPlayer& fmv, AudioHost* audio);
  race::CampaignAction skip_onboarding_all(FmvPlayer& fmv, AudioHost* audio);

  // Gameplay milestone entry points.
  race::CampaignAction on_enter_event(const race::CareerEvent& ev, FmvPlayer& fmv, AudioHost* audio);
  race::CampaignAction on_race_finished(const race::CareerEvent* ev, const race::RaceAward& award, int place,
                                        int total_cars, int old_pop_rank, int new_pop_rank, int64_t balance_before,
                                        int64_t balance_after, int wb_points_after, int collected_wristband,
                                        const std::string& prize_car_name, const race::CareerCatalog* catalog,
                                        FmvPlayer& fmv, AudioHost* audio,
                                        const std::vector<race::RaceFinishStanding>& standings = {});
  bool on_viper_convoy_pass(int passed_delta, AudioHost* audio);
  race::CampaignAction on_wristband_collected(int wristband_level, const std::string& bonus_car_name,
                                              int newly_unlocked_events, FmvPlayer& fmv, AudioHost* audio);
  race::CampaignAction on_barn_find(bool discovered, const std::string& barn_id, const std::string& car_name,
                                    AudioHost* audio);
  race::CampaignAction on_barn_restored(const std::string& barn_id, const std::string& car_name, AudioHost* audio);
  void on_popularity_rank_changed(int old_rank, int new_rank, AudioHost* audio);

  // Computes an in-engine cubic-bezier cinematic camera around `car_pos` / `car_heading`
  // when a campaign cinematic is active. Returns true when `out_cam` should override the
  // normal driving camera.
  bool compute_camera(const std::array<double, 3>& car_pos, double car_heading, drive::CameraView& out_cam) const;

  const race::CampaignFlow& flow() const { return flow_; }
  race::CampaignFlow& flow() { return flow_; }
  float skip_hold_progress() const { return skip_hold_progress_; }
  bool is_cinematic_camera_active() const;
  bool is_blocking_gameplay() const;

 private:
  void dispatch_action_media(const race::CampaignAction& act, FmvPlayer& fmv, AudioHost* audio);
  void pump_audio(AudioHost* audio);
  race::CampaignAction apply_ftc(const race::FtcCommands& c, FmvPlayer& fmv, AudioHost* audio);
  race::CampaignAction update_ftc(double dt, FmvPlayer& fmv, AudioHost* audio);
  void handle_cutscene_events(const std::vector<const cutscene::Event*>& events, AudioHost* audio);

  race::FirstTimeCareer ftc_;
  FtcFrameInput ftc_in_;
  bool ftc_resume_pending_ = false;
  race::FtcStage ftc_resume_stage_ = race::FtcStage::kNotStarted;
  bool ftc_confirm_ = false;
  bool ftc_race_finished_ = false;
  int64_t ftc_race_event_ = 0;
  int ftc_race_place_ = 0;
  std::vector<race::FtcCommands> ftc_pending_;
  CutsceneRuntime cutscenes_;
  int anchor_cam_ = -1;
  const cutscene::Cutscene* anchor_cs_ = nullptr;
  bool place_pending_ = false;
  std::array<double, 3> place_pos_{};
  double place_heading_ = 0.0, place_speed_ = 0.0;

  race::CampaignFlow flow_;
  std::filesystem::path game_root_;
  std::string language_ = "EN";
  bool skip_intro_movies_ = false;
  float skip_hold_progress_ = 0.0f;
};

}  // namespace carview
