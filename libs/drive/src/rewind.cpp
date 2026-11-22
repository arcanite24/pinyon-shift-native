#include "drive/rewind.h"

#include <algorithm>
#include <cmath>

namespace drive {

namespace {

std::array<double, 3> lerp3(const std::array<double, 3>& a, const std::array<double, 3>& b, double t) {
  return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

std::array<double, 3> normalize3(const std::array<double, 3>& v, const std::array<double, 3>& fallback) {
  const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (len <= 1e-12) return fallback;
  return {v[0] / len, v[1] / len, v[2] / len};
}

CameraView blend_camera_view(const CameraView& a, const CameraView& b, double t) {
  CameraView out = t < 0.5 ? a : b;
  out.eye = lerp3(a.eye, b.eye, t);
  out.forward = normalize3(lerp3(a.forward, b.forward, t), b.forward);
  out.up = normalize3(lerp3(a.up, b.up, t), b.up);
  out.fov_degrees = a.fov_degrees + (b.fov_degrees - a.fov_degrees) * t;
  out.near_clip = a.near_clip + (b.near_clip - a.near_clip) * t;
  return out;
}

}  // namespace

RewindBuffer::RewindBuffer(const RewindConfig& config)
    : cfg_(config),
      next_capture_time_(config.first_keyframe_delay_s),
      hints_remaining_(config.hint_max_shows) {
  keyframes_.reserve(size_t(RewindConfig::kMaxKeyframes));
}

void RewindBuffer::start_session(double start_replay_time) {
  reset_history(start_replay_time);
  lap_uncertified_ = false;
  hints_remaining_ = cfg_.hint_max_shows;
  hint_cooldown_until_ = -1e9;
  hint_visible_until_ = -1e9;
}

void RewindBuffer::reset_history(double current_replay_time) {
  keyframes_.clear();
  stream_.clear();
  cancel_snapshot_.reset();
  next_capture_time_ = current_replay_time + cfg_.first_keyframe_delay_s;
  next_stream_time_ = current_replay_time;
  stream_started_ = false;
  state_ = RewindState::kIdle;
  ui_state_ = RewindUiState::kHidden;
  target_slot_ = RewindConfig::kIdleSlot;
  selected_slot_ = RewindConfig::kIdleSlot;
  playback_time_ = current_replay_time;
  settle_timer_ = 0.0;
  view_replay_requested_ = false;
  audio_.snapshot_active = false;
  audio_.dsp_active = false;
  audio_.blend = 0.0;
  audio_.playback_speed = 1.0;
  audio_.vehicle_audio_state = 1;
  audio_.tape_start_cue = false;
}

void RewindBuffer::push_keyframe(RewindKeyframe&& kf) {
  if (int(keyframes_.size()) >= RewindConfig::kMaxKeyframes) {
    keyframes_.erase(keyframes_.begin());
  }
  keyframes_.push_back(std::move(kf));
}

void RewindBuffer::prune_stream() {
  if (stream_.empty()) return;
  double cutoff = -1e18;
  if (!keyframes_.empty()) {
    cutoff = keyframes_.front().replay_time - 0.5;
  } else {
    const double max_window = RewindConfig::kMaxKeyframes * cfg_.keyframe_interval_s + 5.0;
    cutoff = stream_.back().replay_time - max_window;
  }
  auto it = std::lower_bound(stream_.begin(), stream_.end(), cutoff,
                             [](const RewindStreamSample& s, double t) { return s.replay_time < t; });
  if (it != stream_.begin()) {
    stream_.erase(stream_.begin(), it);
  }
}

void RewindBuffer::record_step(double replay_time, double race_clock, const Vehicle& vehicle,
                               const RewindCameraState& camera, bool capture_allowed,
                               uint64_t skill_chain_score, int skill_chain_mult) {
  if (state_ != RewindState::kIdle) return;

  const double stream_dt = cfg_.stream_record_hz > 0.0 ? 1.0 / cfg_.stream_record_hz : 0.05;
  if (!stream_started_ || replay_time >= next_stream_time_ - 1e-9) {
    RewindStreamSample s;
    s.replay_time = replay_time;
    s.pose = vehicle.pose();
    s.camera_view = camera.view;
    s.has_camera_view = camera.has_view;
    if (!stream_.empty() && replay_time < stream_.back().replay_time) {
      stream_.clear();
    }
    stream_.push_back(s);
    stream_started_ = true;
    next_stream_time_ = replay_time + stream_dt;
  }

  if (capture_allowed && replay_time >= next_capture_time_ - 1e-9) {
    RewindKeyframe kf(vehicle);
    kf.replay_time = replay_time;
    kf.race_clock = race_clock;
    kf.camera = camera;
    kf.skill_chain_score = skill_chain_score;
    kf.skill_chain_mult = skill_chain_mult;
    push_keyframe(std::move(kf));
    next_capture_time_ = replay_time + cfg_.keyframe_interval_s;
    prune_stream();
  } else if (stream_.size() > 1024) {
    prune_stream();
  }
}

bool RewindBuffer::can_trigger() const {
  return enabled_ && !disabled_ && state_ == RewindState::kIdle && !keyframes_.empty();
}

bool RewindBuffer::trigger(double current_replay_time, double race_clock, const Vehicle& current_vehicle,
                           const RewindCameraState& current_camera, uint64_t skill_chain_score,
                           int skill_chain_mult) {
  if (!can_trigger()) return false;

  // Capture end-state snapshot so Cancel can return to the exact trigger point.
  cancel_snapshot_.emplace(current_vehicle);
  cancel_snapshot_->replay_time = current_replay_time;
  cancel_snapshot_->race_clock = race_clock;
  cancel_snapshot_->camera = current_camera;
  cancel_snapshot_->skill_chain_score = skill_chain_score;
  cancel_snapshot_->skill_chain_mult = skill_chain_mult;

  // Append exact trigger-point stream sample so reverse scrub starts seamlessly.
  if (stream_.empty() || current_replay_time > stream_.back().replay_time + 1e-6) {
    RewindStreamSample s;
    s.replay_time = current_replay_time;
    s.pose = current_vehicle.pose();
    s.camera_view = current_camera.view;
    s.has_camera_view = current_camera.has_view;
    stream_.push_back(s);
  }

  // First-Keyframe Ignore rule (SPEC-REWIND section 3):
  // If count > 2 and (current_replay_time - latest_slot_time) < 1.0 s, step 2
  // keyframes back (latest - 1) instead of 1 (latest).
  const int count = int(keyframes_.size());
  const int latest = count - 1;
  int target = latest;
  if (count > 2 && (current_replay_time - keyframes_[size_t(latest)].replay_time) < cfg_.first_keyframe_ignore_s) {
    target = latest - 1;
  }

  target_slot_ = target;
  selected_slot_ = target;
  playback_time_ = current_replay_time;
  settle_timer_ = 0.0;
  view_replay_requested_ = false;
  state_ = RewindState::kRewinding;
  ui_state_ = RewindUiState::kRewinding;

  audio_.snapshot_active = true;
  audio_.dsp_active = true;
  audio_.tape_start_cue = true;
  audio_.playback_speed = cfg_.reverse_playback_rate;
  audio_.vehicle_audio_state = 104;
  return true;
}

void RewindBuffer::update(double dt) {
  if (dt < 0.0) dt = 0.0;

  // Ramp mixer snapshot ducking (0.2 s fade-in, 0.1 s return-fade).
  if (audio_.snapshot_active) {
    if (audio_.mixer.fade_in_s > 0.0) {
      audio_.blend = std::min(1.0, audio_.blend + dt / audio_.mixer.fade_in_s);
    } else {
      audio_.blend = 1.0;
    }
  } else {
    if (audio_.mixer.return_fade_s > 0.0) {
      audio_.blend = std::max(0.0, audio_.blend - dt / audio_.mixer.return_fade_s);
    } else {
      audio_.blend = 0.0;
    }
  }

  if (state_ == RewindState::kRewinding) {
    if (target_slot_ < 0 || target_slot_ >= int(keyframes_.size())) {
      state_ = RewindState::kIdle;
      ui_state_ = RewindUiState::kHidden;
      target_slot_ = RewindConfig::kIdleSlot;
      audio_.snapshot_active = false;
      audio_.dsp_active = false;
      audio_.playback_speed = 1.0;
      audio_.vehicle_audio_state = 1;
      return;
    }
    audio_.vehicle_audio_state = 10;
    const double target_time = keyframes_[size_t(target_slot_)].replay_time;
    playback_time_ += cfg_.reverse_playback_rate * dt;
    if (playback_time_ <= target_time) {
      playback_time_ = target_time;
      selected_slot_ = target_slot_;
      target_slot_ = RewindConfig::kIdleSlot;
      state_ = RewindState::kPausedAtKeyframe;
      ui_state_ = RewindUiState::kStopRewind;
      audio_.playback_speed = 0.0;
      audio_.vehicle_audio_state = 0;
    }
  } else if (state_ == RewindState::kSettling) {
    settle_timer_ += dt;
    if (settle_timer_ >= cfg_.resume_settle_s) {
      settle_timer_ = cfg_.resume_settle_s;
      state_ = RewindState::kIdle;
      ui_state_ = RewindUiState::kHidden;
      audio_.snapshot_active = false;
      audio_.dsp_active = false;
      audio_.playback_speed = 1.0;
      audio_.vehicle_audio_state = 1;
    }
  }
}

bool RewindBuffer::rewind_further() {
  if (state_ != RewindState::kPausedAtKeyframe && state_ != RewindState::kRewinding) return false;
  if (selected_slot_ <= 0 || keyframes_.empty()) return false;

  target_slot_ = selected_slot_ - 1;
  selected_slot_ = target_slot_;
  state_ = RewindState::kRewinding;
  ui_state_ = RewindUiState::kLoopRewind;
  audio_.playback_speed = cfg_.reverse_playback_rate;
  audio_.tape_start_cue = true;
  audio_.vehicle_audio_state = 10;
  return true;
}

bool RewindBuffer::resume(Vehicle& out_vehicle, RewindCameraState* out_camera, double* out_replay_time,
                          double* out_race_clock, uint64_t* out_skill_score, int* out_skill_mult) {
  if (state_ != RewindState::kPausedAtKeyframe) return false;
  if (selected_slot_ < 0 || selected_slot_ >= int(keyframes_.size())) return false;

  const RewindKeyframe& kf = keyframes_[size_t(selected_slot_)];
  out_vehicle = kf.vehicle;
  if (out_camera) *out_camera = kf.camera;
  if (out_replay_time) *out_replay_time = kf.replay_time;
  if (out_race_clock) *out_race_clock = kf.race_clock;
  if (out_skill_score) *out_skill_score = kf.skill_chain_score;
  if (out_skill_mult) *out_skill_mult = kf.skill_chain_mult;

  // Truncate future keyframes and stream samples after the restored keyframe.
  const double restored_time = kf.replay_time;
  if (size_t(selected_slot_ + 1) < keyframes_.size()) {
    keyframes_.erase(keyframes_.begin() + (selected_slot_ + 1), keyframes_.end());
  }
  auto it = std::upper_bound(stream_.begin(), stream_.end(), restored_time,
                             [](double t, const RewindStreamSample& s) { return t < s.replay_time; });
  stream_.erase(it, stream_.end());
  next_stream_time_ = restored_time + (cfg_.stream_record_hz > 0.0 ? 1.0 / cfg_.stream_record_hz : 0.05);
  next_capture_time_ = restored_time + cfg_.keyframe_interval_s;

  cancel_snapshot_.reset();
  lap_uncertified_ = true;
  target_slot_ = RewindConfig::kIdleSlot;
  settle_timer_ = 0.0;
  state_ = RewindState::kSettling;
  audio_.playback_speed = 1.0;
  return true;
}

bool RewindBuffer::cancel(Vehicle& out_vehicle, RewindCameraState* out_camera, double* out_replay_time,
                          double* out_race_clock, uint64_t* out_skill_score, int* out_skill_mult) {
  if (state_ != RewindState::kRewinding && state_ != RewindState::kPausedAtKeyframe) return false;
  if (!cancel_snapshot_.has_value()) return false;

  const RewindKeyframe& kf = *cancel_snapshot_;
  out_vehicle = kf.vehicle;
  if (out_camera) *out_camera = kf.camera;
  if (out_replay_time) *out_replay_time = kf.replay_time;
  if (out_race_clock) *out_race_clock = kf.race_clock;
  if (out_skill_score) *out_skill_score = kf.skill_chain_score;
  if (out_skill_mult) *out_skill_mult = kf.skill_chain_mult;

  playback_time_ = kf.replay_time;
  cancel_snapshot_.reset();
  target_slot_ = RewindConfig::kIdleSlot;
  selected_slot_ = RewindConfig::kIdleSlot;
  settle_timer_ = 0.0;
  state_ = RewindState::kIdle;
  ui_state_ = RewindUiState::kHidden;
  audio_.snapshot_active = false;
  audio_.dsp_active = false;
  audio_.playback_speed = 1.0;
  audio_.vehicle_audio_state = 1;
  return true;
}

bool RewindBuffer::request_view_replay() {
  if (state_ != RewindState::kPausedAtKeyframe) return false;
  view_replay_requested_ = true;
  return true;
}

bool RewindBuffer::consume_view_replay_request() {
  const bool v = view_replay_requested_;
  view_replay_requested_ = false;
  return v;
}

bool RewindBuffer::consume_tape_start_cue() {
  const bool v = audio_.tape_start_cue;
  audio_.tape_start_cue = false;
  return v;
}

Vehicle::Pose RewindBuffer::sampled_pose() const {
  if (!stream_.empty()) {
    if (playback_time_ <= stream_.front().replay_time) return stream_.front().pose;
    if (playback_time_ >= stream_.back().replay_time) return stream_.back().pose;
    auto it = std::lower_bound(stream_.begin(), stream_.end(), playback_time_,
                               [](const RewindStreamSample& s, double t) { return s.replay_time < t; });
    if (it == stream_.begin()) return it->pose;
    const auto& b = *it;
    const auto& a = *(it - 1);
    const double span = b.replay_time - a.replay_time;
    const double alpha = span > 1e-9 ? std::clamp((playback_time_ - a.replay_time) / span, 0.0, 1.0) : 1.0;
    return Vehicle::blend(a.pose, b.pose, alpha);
  }
  if (selected_slot_ >= 0 && selected_slot_ < int(keyframes_.size())) {
    return keyframes_[size_t(selected_slot_)].vehicle.pose();
  }
  if (!keyframes_.empty()) return keyframes_.back().vehicle.pose();
  return {};
}

bool RewindBuffer::sampled_camera(CameraView& out) const {
  if (!stream_.empty()) {
    if (playback_time_ <= stream_.front().replay_time) {
      if (!stream_.front().has_camera_view) return false;
      out = stream_.front().camera_view;
      return true;
    }
    if (playback_time_ >= stream_.back().replay_time) {
      if (!stream_.back().has_camera_view) return false;
      out = stream_.back().camera_view;
      return true;
    }
    auto it = std::lower_bound(stream_.begin(), stream_.end(), playback_time_,
                               [](const RewindStreamSample& s, double t) { return s.replay_time < t; });
    if (it == stream_.begin()) {
      if (!it->has_camera_view) return false;
      out = it->camera_view;
      return true;
    }
    const auto& b = *it;
    const auto& a = *(it - 1);
    if (a.has_camera_view && b.has_camera_view) {
      const double span = b.replay_time - a.replay_time;
      const double alpha = span > 1e-9 ? std::clamp((playback_time_ - a.replay_time) / span, 0.0, 1.0) : 1.0;
      out = blend_camera_view(a.camera_view, b.camera_view, alpha);
      return true;
    }
    if (b.has_camera_view) {
      out = b.camera_view;
      return true;
    }
    if (a.has_camera_view) {
      out = a.camera_view;
      return true;
    }
  }
  if (selected_slot_ >= 0 && selected_slot_ < int(keyframes_.size()) &&
      keyframes_[size_t(selected_slot_)].camera.has_view) {
    out = keyframes_[size_t(selected_slot_)].camera.view;
    return true;
  }
  return false;
}

bool RewindBuffer::request_hint(double now_s) {
  if (!enabled_ || !hint_enabled_ || disabled_ || hints_remaining_ <= 0) return false;
  if (now_s < hint_cooldown_until_) return false;
  --hints_remaining_;
  hint_visible_until_ = now_s + cfg_.hint_timeout_s;
  hint_cooldown_until_ = now_s + cfg_.hint_cooldown_s;
  return true;
}

bool RewindBuffer::hint_visible(double now_s) const {
  return enabled_ && hint_enabled_ && !disabled_ && now_s < hint_visible_until_;
}

}  // namespace drive
