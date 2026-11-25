// In-gameplay time rewind (Mulligan): 8-slot rolling keyframe buffer,
// 20 Hz pose stream for -1.0x reverse scrub, first-keyframe ignore rule,
// rewind menu loop (Resume / Rewind Further / Cancel / View Replay),
// post-resume settle delay, and audio mixer/DSP/SFX state hooks.
//
// Spec: SPEC-REWIND (docs/specs/REWIND.md, sections 1-7, Rates and timing).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "drive/cameras.h"
#include "drive/vehicle.h"

namespace drive {

// SPEC-REWIND, Rates and timing.
struct RewindConfig {
  static constexpr int kMaxKeyframes = 8;
  static constexpr int kIdleSlot = 8;

  double first_keyframe_delay_s = 10.0;   // TimeAfterRaceStartForFirstKeyframe
  double keyframe_interval_s = 2.5;       // TimeBetweenMulliganKeyframes
  double first_keyframe_ignore_s = 1.0;   // TimeDiffToIgnoreTheFirstKeyframe
  double stream_record_hz = 20.0;         // ReplayRecordKeyframeFrequency (interim 20 Hz)
  double reverse_playback_rate = -1.0;    // reverse scrub rate (-1.0x)
  double resume_settle_s = 1.0;           // post-resume settle delay

  // First-career tutorial hint pacing (GlobalRegistry.xml).
  double hint_cooldown_s = 30.0;          // FirstTimeCareer/RewindCoolDown
  double hint_timeout_s = 5.0;            // FirstTimeCareer/RewindMessageTimout
  int hint_max_shows = 10;                // FirstTimeCareer/TimesToShowRewind
  double tutorial_text_limit_s = 3.0;     // TutorialMessages/RewindTextTimerLimit
};

// SPEC-REWIND section 6: AudioMixerSnapshots.xml (<Rewind group="Gameplay">).
struct RewindMixerTargets {
  double focus_car = 0.175;
  double opp_car = 0.175;
  double game_music = 0.25;
  double muted_channels = 0.0;  // Transmission, Turbo, Tire, Collision, Ambient*, etc.
  double game_ui = 1.0;
  double vo = 1.0;
  double sat_nav = 1.0;
  double dj_modifier = 1.0;
  double fade_in_s = 0.2;
  double return_fade_s = 0.1;
};

// SPEC-REWIND section 6: GlobalDSPConfig.xml (<RewindDSPConfig>).
struct RewindDspConfig {
  bool bypass = false;
  double gain = 0.300000;
  double bandwidth = 3.359995;
  double center_hz = 3772.0;
};

// Pullable audio state driven by RewindBuffer for the mixer, DSP, and vehicle
// audio components (SPEC-REWIND section 6).
struct RewindAudioState {
  RewindMixerTargets mixer{};
  RewindDspConfig dsp{};
  bool snapshot_active = false;
  bool dsp_active = false;
  double blend = 0.0;             // 0 = normal mix, 1 = full rewind ducking
  double playback_speed = 1.0;    // -1.0 scrubbing, 0.0 paused, 1.0 normal
  int vehicle_audio_state = 1;    // 104 enter, 10 scrubbing, 0 paused, 1 exit/normal
  bool tape_start_cue = false;    // UIInGame/UI/Mulligan "TapeStart"

  double focus_car_gain() const { return 1.0 + (mixer.focus_car - 1.0) * blend; }
  double opp_car_gain() const { return 1.0 + (mixer.opp_car - 1.0) * blend; }
  double game_music_gain() const { return 1.0 + (mixer.game_music - 1.0) * blend; }
  double effects_gain() const { return 1.0 + (mixer.muted_channels - 1.0) * blend; }
};

// Camera state captured with each keyframe.
struct RewindCameraState {
  std::optional<DriveCamera> rig;
  CameraView view{};
  bool has_view = false;
  double look_angle = 0.0;
  bool front_view = false;
};

// Auxiliary per-keyframe VFX, rumble, camera, audio, and world-stunt state
// (all defaulted so existing callers keep compiling unchanged).
struct RewindExtraState {
  std::array<float, 4> rotor_heat{0.0f, 0.0f, 0.0f, 0.0f};
  int prev_rumble_gear = 1;
  int cam_state = 0;
  int prev_audio_checkpoint = -1;
  int prev_audio_lap = 0;
  std::vector<uint8_t> speed_trap_state;
  std::vector<uint8_t> pr_stunt_state;
  std::vector<uint8_t> signs_state;
};

// One keyframe slot: whole drive::Vehicle copy (all physics state) plus camera
// state, clocks, skill-chain state, and optional extended gameplay state.
struct RewindKeyframe {
  uint64_t snapshot_id = 0;  // unique monotonic id tying external world snapshots to this slot
  double replay_time = 0.0;
  double race_clock = 0.0;
  Vehicle vehicle;
  RewindCameraState camera{};
  uint64_t skill_chain_score = 0;
  int skill_chain_mult = 1;
  double skill_chain_timer = 0.0;
  std::array<float, 4> rotor_heat{0.0f, 0.0f, 0.0f, 0.0f};
  int prev_rumble_gear = 1;
  int cam_state = 0;
  int prev_audio_checkpoint = -1;
  int prev_audio_lap = 0;
  std::vector<uint8_t> speed_trap_state;
  std::vector<uint8_t> pr_stunt_state;
  std::vector<uint8_t> signs_state;

  explicit RewindKeyframe(const Vehicle& v) : vehicle(v) {}
};

// Compact high-frequency (20 Hz) world actor pose for smooth rewind scrubbing
// of Race AI rivals, Traffic, Festival AI, and Showcase aircraft without
// keyframe-interval chord-cutting or teleporting.
struct RewindActorPose {
  uint8_t kind = 0;       // 0 = race AI, 1 = traffic, 2 = festival AI, 3 = showcase
  uint32_t id = 0;        // car index (race AI), car id (traffic/festival), 0 (showcase)
  Vec3 pos{};
  Quat rot{0.0, 0.0, 0.0, 1.0};
  Vec3 vel{};
  float steer = 0.0f;      // wheel steer (race AI), heading rad (traffic/festival/showcase)
  float wheel_spin = 0.0f; // wheel spin rad (race AI), speed m/s (traffic/festival/showcase)
  float extra0 = 0.0f;     // pitch rad (traffic/festival/showcase)
  float extra1 = 0.0f;     // roll rad (showcase)
  float extra2 = 0.0f;     // rotor_angle rad (showcase)
};

// High-frequency pose sample for smooth -1.0x linear interpolation during scrub.
struct RewindStreamSample {
  double replay_time = 0.0;
  Vehicle::Pose pose{};
  CameraView camera_view{};
  bool has_camera_view = false;
  std::vector<RewindActorPose> actors;
};

enum class RewindState {
  kIdle,
  kRewinding,
  kPausedAtKeyframe,
  kSettling,
};

// 921_I_REWIND UI overlay states (SPEC-REWIND section 4).
enum class RewindUiState {
  kHidden,
  kRewinding,
  kLoopRewind,
  kStopRewind,
};

class RewindBuffer {
 public:
  explicit RewindBuffer(const RewindConfig& config = {});

  const RewindConfig& config() const { return cfg_; }

  // Profile & flow-graph controls (SPEC-REWIND State & sections 1-2).
  void set_enabled(bool on) { enabled_ = on; }
  bool enabled() const { return enabled_; }
  void set_disabled(bool lockout) { disabled_ = lockout; }
  bool disabled() const { return disabled_; }
  void set_rewind_on_back(bool on) { rewind_on_back_ = on; }
  bool rewind_on_back() const { return rewind_on_back_; }
  void set_hint_enabled(bool on) { hint_enabled_ = on; }
  bool hint_enabled() const { return hint_enabled_; }

  // Session start and CResetMulliganHistory (SPEC-REWIND section 1).
  void start_session(double start_replay_time = 0.0);
  void reset_history(double current_replay_time = 0.0);

  // Called during active driving to record 20 Hz stream poses and periodic
  // 2.5 s keyframes (skipped when capture_allowed is false, rolled over, or
  // all 4 wheels are off the ground).
  // Returns the newly created keyframe's monotonic snapshot_id, or 0 if no
  // keyframe was captured on this step.
  uint64_t record_step(double replay_time, double race_clock, const Vehicle& vehicle,
                       const RewindCameraState& camera, bool capture_allowed = true,
                       uint64_t skill_chain_score = 0, int skill_chain_mult = 1,
                       double skill_chain_timer = 0.0, const RewindExtraState* extra = nullptr);

  // Trigger eligibility (SPEC-REWIND section 2).
  bool can_trigger() const;

  // Enter rewind from active driving (SPEC-REWIND section 3).
  // Captures the trigger-point snapshot for Cancel, applies the first-keyframe
  // ignore rule (< 1.0 s when count > 2), and starts -1.0x reverse playback.
  bool trigger(double current_replay_time, double race_clock, const Vehicle& current_vehicle,
               const RewindCameraState& current_camera, uint64_t skill_chain_score = 0,
               int skill_chain_mult = 1, double skill_chain_timer = 0.0,
               const RewindExtraState* extra = nullptr);

  // Advance reverse playback, post-resume settle timer, and audio ducking ramp.
  void update(double dt);

  // Rewind menu actions (SPEC-REWIND section 4).
  bool rewind_further();
  bool resume(Vehicle& out_vehicle, RewindCameraState* out_camera = nullptr,
              double* out_replay_time = nullptr, double* out_race_clock = nullptr,
              uint64_t* out_skill_score = nullptr, int* out_skill_mult = nullptr,
              uint64_t* out_snapshot_id = nullptr, double* out_skill_timer = nullptr,
              RewindExtraState* out_extra = nullptr);
  bool cancel(Vehicle& out_vehicle, RewindCameraState* out_camera = nullptr,
              double* out_replay_time = nullptr, double* out_race_clock = nullptr,
              uint64_t* out_skill_score = nullptr, int* out_skill_mult = nullptr,
              uint64_t* out_snapshot_id = nullptr, double* out_skill_timer = nullptr,
              RewindExtraState* out_extra = nullptr);
  bool request_view_replay();
  bool consume_view_replay_request();

  // Monotonic snapshot ids for external world-state snapshots synchronized to keyframes.
  uint64_t latest_snapshot_id() const { return keyframes_.empty() ? 0u : keyframes_.back().snapshot_id; }
  uint64_t cancel_snapshot_id() const { return cancel_snapshot_ ? cancel_snapshot_->snapshot_id : 0u; }
  uint64_t selected_snapshot_id() const {
    return (selected_slot_ >= 0 && selected_slot_ < int(keyframes_.size()))
               ? keyframes_[size_t(selected_slot_)].snapshot_id
               : 0u;
  }
  std::vector<uint64_t> active_snapshot_ids() const;

  // State queries.
  RewindState state() const { return state_; }
  RewindUiState ui_state() const { return ui_state_; }
  bool active() const { return state_ != RewindState::kIdle; }
  bool is_rewinding() const { return state_ == RewindState::kRewinding; }
  bool is_paused_at_keyframe() const { return state_ == RewindState::kPausedAtKeyframe; }
  bool is_settling() const { return state_ == RewindState::kSettling; }

  int keyframe_count() const { return int(keyframes_.size()); }
  const RewindKeyframe& keyframe(int index) const { return keyframes_[size_t(index)]; }
  int target_slot() const { return target_slot_; }
  int selected_slot() const { return selected_slot_; }
  bool at_oldest_slot() const { return selected_slot_ <= 0; }
  double playback_time() const { return playback_time_; }
  double next_capture_time() const { return next_capture_time_; }
  double settle_timer() const { return settle_timer_; }
  size_t stream_sample_count() const { return stream_.size(); }

  // Clean-lap invalidation (SPEC-REWIND section 5).
  bool lap_uncertified() const { return lap_uncertified_; }
  void clear_lap_uncertified() { lap_uncertified_ = false; }

  // Audio state & one-shot TapeStart cue.
  const RewindAudioState& audio() const { return audio_; }
  bool consume_tape_start_cue();

  // True when the next record_step(replay_time, ...) call will append a 20 Hz stream sample.
  bool will_record_stream(double replay_time) const;
  // Attaches compact world-actor poses to the most recently recorded stream sample.
  void attach_latest_stream_actors(std::vector<RewindActorPose> actors);

  // Interpolated vehicle pose, camera view, and world actor poses at current playback_time_.
  Vehicle::Pose sampled_pose() const;
  bool sampled_camera(CameraView& out) const;
  bool sampled_actor_poses(std::vector<RewindActorPose>& out) const;

  // Adjacent keyframe/cancel snapshot bracket for smooth whole-world visual rewind scrubbing
  // (AI rivals, traffic, Festival AI, and smashables).
  struct RewindBracket {
    uint64_t snap_a = 0u;
    uint64_t snap_b = 0u;
    double alpha = 0.0;  // 0 at snap_a, 1 at snap_b
    double span_s = 0.0; // time delta (snap_b - snap_a) in seconds
  };
  bool sampled_snapshot_bracket(RewindBracket& out) const;

  // First-career tutorial hint cooldown (SPEC-REWIND State & section 2).
  bool request_hint(double now_s);
  bool hint_visible(double now_s) const;
  int hints_remaining() const { return hints_remaining_; }

 private:
  void push_keyframe(RewindKeyframe&& kf);
  void prune_stream();

  RewindConfig cfg_{};
  bool enabled_ = true;
  bool disabled_ = false;
  bool rewind_on_back_ = false;
  bool hint_enabled_ = true;

  std::vector<RewindKeyframe> keyframes_;
  std::vector<RewindStreamSample> stream_;
  std::optional<RewindKeyframe> cancel_snapshot_;
  uint64_t next_snapshot_id_ = 1;

  double next_capture_time_ = 10.0;
  double next_stream_time_ = 0.0;
  bool stream_started_ = false;

  RewindState state_ = RewindState::kIdle;
  RewindUiState ui_state_ = RewindUiState::kHidden;
  int target_slot_ = RewindConfig::kIdleSlot;
  int selected_slot_ = RewindConfig::kIdleSlot;
  double playback_time_ = 0.0;
  double settle_timer_ = 0.0;
  bool lap_uncertified_ = false;
  bool view_replay_requested_ = false;

  RewindAudioState audio_{};
  RewindExtraState last_extra_{};
  bool has_last_extra_ = false;

  // Tutorial hint state.
  int hints_remaining_ = 10;
  double hint_cooldown_until_ = -1e9;
  double hint_visible_until_ = -1e9;
};

}  // namespace drive
