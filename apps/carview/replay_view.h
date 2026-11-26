// Gameplay Replay Viewer & HUD (Slice 10): timeline scrubber, play/pause,
// slow-motion (0.25x/0.5x), rewind/fast-forward, 6 cameras (chase, track-side,
// orbit, bumper, wheel, car-relative), telemetry HUD toggle, and persistent
// replay file save/load browser.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "drive/cameras.h"
#include "drive/replay.h"
#include "ui.h"

namespace carview {

enum class ReplayCameraMode : int {
  kChase = 0,
  kTrackSide,
  kOrbit,
  kBumper,
  kWheel,
  kCarRelative,
  kCockpit,
  kHood,
  kCount
};

enum class ReplayTelemetryPage : int {
  kGeneral = 0,
  kFriction,
  kSuspension,
  kDamage,
  kEngine,
  kCount
};

const char* replay_camera_name(ReplayCameraMode m);
const char* replay_telemetry_page_name(ReplayTelemetryPage p);

// Unit-aware telemetry formatters respecting metric (KM/H, BAR, kW, Nm, °C) vs imperial (MPH, PSI, HP, LB-FT, °F).
std::string format_replay_speed(float speed_mps, bool metric_units);
std::string format_replay_boost(float boost_psi, bool has_fi, bool metric_units);
std::string format_replay_power(float power_kw, bool metric_units);
std::string format_replay_torque(float torque_nm, bool metric_units);
std::string format_replay_temp(float temp_c, bool metric_units);

class ReplayViewer {
 public:
  // Optional `_&<hash>` string-table resolver (e.g. `carview::game_text`) for localized Replay UI labels.
  static void set_text_resolver(std::function<std::string(const std::string&)> fn);
  // Optional car display/media -> (class_name, pi) resolver for Replay Browser class badges.
  static void set_car_class_resolver(std::function<bool(const std::string&, std::string&, int&)> fn);

  void open(drive::ReplayRecording rec, bool start_playing = true);
  void close();
  bool active() const { return active_; }

  const drive::ReplayRecording& recording() const { return rec_; }
  double time() const { return t_; }
  double duration() const { return rec_.duration(); }
  double progress() const { return rec_.duration() > 1e-6 ? t_ / rec_.duration() : 0.0; }
  bool paused() const { return paused_; }
  double speed() const { return speed_; }
  ReplayCameraMode camera_mode() const { return cam_mode_; }
  uint32_t focus_slot() const { return focus_slot_; }
  bool telemetry_visible() const { return telemetry_; }
  ReplayTelemetryPage telemetry_page() const { return telemetry_page_; }
  bool repeat() const { return repeat_loop_; }
  bool ui_hidden() const { return ui_hidden_; }
  bool metric_units() const { return metric_units_; }
  void set_metric_units(bool metric) { metric_units_ = metric; }

  // Playback transport controls.
  void toggle_pause();
  void set_paused(bool p) { paused_ = p; }
  void set_speed(double s);
  void step_speed(int dir);  // steps through {-4, -2, -1, 0.25, 0.5, 1, 2, 4}
  void set_slow_motion();    // toggles 0.5x -> 0.25x -> 1.0x
  void trigger_rewind();     // -1x -> -2x -> -4x
  void trigger_fast_forward();  // 2x -> 4x -> 1x
  void seek(double t_s);
  void scrub(double delta_s);
  void step_frame(int frames);  // pauses and steps by frames / sample_hz

  void cycle_camera(int dir = +1);
  void set_camera_mode(ReplayCameraMode m);
  void cycle_focus_car(int dir = +1);
  void toggle_telemetry() { telemetry_ = !telemetry_; }
  void set_telemetry(bool on) { telemetry_ = on; }
  void cycle_telemetry_page(int dir = +1);
  void set_telemetry_page(ReplayTelemetryPage p) { telemetry_page_ = p; }
  void toggle_repeat() { repeat_loop_ = !repeat_loop_; }
  void set_repeat(bool on) { repeat_loop_ = on; }
  void toggle_ui_hidden() { ui_hidden_ = !ui_hidden_; }

  // Advances playback clock, updates sampled cars, and steps camera state.
  void update(double dt);
  const std::vector<drive::ReplayCarSample>& current_cars() const { return current_cars_; }
  const drive::ReplayCarSample* focused_car() const;

  // Computes the world-space camera view for the active ReplayCameraMode.
  drive::CameraView compute_camera(
      double dt, float orbit_yaw_in = 0.0f, float orbit_pitch_in = 0.0f, float zoom_in = 0.0f,
      const std::function<bool(double x, double y, double z, double& ground_y)>& ground_below = {});

  // Persistent save/load helpers.
  bool save_to_dir(const std::filesystem::path& replays_dir, std::filesystem::path& out_path,
                   std::string* error = nullptr);
  void open_browser(const std::filesystem::path& replays_dir);
  void close_browser() {
    browser_open_ = false;
    browser_delete_confirm_idx_ = -1;
  }
  bool browser_open() const { return browser_open_; }
  void browser_move(int delta);
  void browser_select(int idx);
  void seek_fraction(double frac);
  bool browser_load_selected(std::string* error = nullptr);
  // Two-step confirmed delete for the selected `.shrep` replay file (X / Del).
  bool browser_delete_confirm_active() const {
    return browser_delete_confirm_idx_ >= 0 && browser_delete_confirm_idx_ == browser_selected_;
  }
  void browser_cancel_delete() { browser_delete_confirm_idx_ = -1; }
  bool browser_request_or_confirm_delete(std::string* error = nullptr);
  bool browser_delete_selected(bool confirmed = true, std::string* error = nullptr);
  const std::vector<drive::ReplayFileEntry>& browser_entries() const { return browser_entries_; }
  int browser_selected() const { return browser_selected_; }

  void set_notice(const std::string& msg, double seconds = 3.0);

  // Renders the Replay Viewer HUD (timeline scrubber, transport controls,
  // camera badge, telemetry panel, or saved replay browser) into `out`.
  void build_hud(ui::List& out, float w, float h) const;

 private:
  void refresh_samples();
  void reset_trackside_anchor(const drive::ReplayCarSample& focus,
                              const std::function<bool(double x, double y, double z, double& ground_y)>& ground_below);

  bool active_ = false;
  drive::ReplayRecording rec_{};
  std::vector<drive::ReplayCarSample> current_cars_;
  double t_ = 0.0;
  bool paused_ = false;
  double speed_ = 1.0;
  ReplayCameraMode cam_mode_ = ReplayCameraMode::kTrackSide;
  uint32_t focus_slot_ = 0;
  bool telemetry_ = true;
  ReplayTelemetryPage telemetry_page_ = ReplayTelemetryPage::kGeneral;
  bool repeat_loop_ = true;
  bool ui_hidden_ = false;
  bool metric_units_ = false;  // miles by default (Q277)

  // Orbit & track-side camera state.
  double orbit_yaw_ = 0.55;
  double orbit_pitch_ = -0.18;
  double orbit_dist_ = 5.8;
  bool trackside_valid_ = false;
  std::array<double, 3> trackside_eye_{0, 2.0, 0};
  int trackside_side_ = 1;

  // Saved replays browser overlay.
  bool browser_open_ = false;
  std::filesystem::path browser_dir_;
  std::vector<drive::ReplayFileEntry> browser_entries_;
  int browser_selected_ = 0;
  int browser_delete_confirm_idx_ = -1;

  std::string notice_;
  double notice_timer_ = 0.0;
};

}  // namespace carview
