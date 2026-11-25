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

#include "drive/cameras.h"
#include "drive/save.h"
#include "fmv_player.h"
#include "race/campaign_flow.h"

namespace carview {

class AudioHost;

// Evaluates a 3D cubic Bezier curve at parameter t in [0, 1].
std::array<double, 3> eval_cubic_bezier(const std::array<double, 3>& p0, const std::array<double, 3>& p1,
                                        const std::array<double, 3>& p2, const std::array<double, 3>& p3, double t);

class CampaignDirector {
 public:
  void configure(const std::filesystem::path& game_root, std::string_view language, bool skip_intro_movies);

  // Syncs campaign progression from/to DriveSave cleanly. A save that came
  // from an original profile (`owned_garage`) gets the campaign mirror the
  // profile does not store rebuilt from its career first
  // (race::seed_imported_campaign, idempotent), on every load path:
  // `popularity_rank` is the rank of its popularity (0: unknown).
  void load_from_save(const drive::DriveSave& save, int popularity_rank = 0);
  void save_to_save(drive::DriveSave& save) const;

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
  bool is_blocking_gameplay() const { return flow_.is_blocking_gameplay(); }

 private:
  void dispatch_action_media(const race::CampaignAction& act, FmvPlayer& fmv, AudioHost* audio);
  void pump_audio(AudioHost* audio);

  race::CampaignFlow flow_;
  std::filesystem::path game_root_;
  std::string language_ = "EN";
  bool skip_intro_movies_ = false;
  float skip_hold_progress_ = 0.0f;
};

}  // namespace carview
