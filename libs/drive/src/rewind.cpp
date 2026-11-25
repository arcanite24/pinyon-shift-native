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

void store_extra(RewindKeyframe& kf, const RewindExtraState* extra) {
  if (!extra) return;
  kf.rotor_heat = extra->rotor_heat;
  kf.prev_rumble_gear = extra->prev_rumble_gear;
  kf.cam_state = extra->cam_state;
  kf.prev_audio_checkpoint = extra->prev_audio_checkpoint;
  kf.prev_audio_lap = extra->prev_audio_lap;
  kf.speed_trap_state = extra->speed_trap_state;
  kf.pr_stunt_state = extra->pr_stunt_state;
  kf.signs_state = extra->signs_state;
}

void load_extra(const RewindKeyframe& kf, RewindExtraState* out) {
  if (!out) return;
  out->rotor_heat = kf.rotor_heat;
  out->prev_rumble_gear = kf.prev_rumble_gear;
  out->cam_state = kf.cam_state;
  out->prev_audio_checkpoint = kf.prev_audio_checkpoint;
  out->prev_audio_lap = kf.prev_audio_lap;
  out->speed_trap_state = kf.speed_trap_state;
  out->pr_stunt_state = kf.pr_stunt_state;
  out->signs_state = kf.signs_state;
}

bool vehicle_grounded(const Vehicle& vehicle) {
  for (const auto& w : vehicle.wheels()) {
    if (w.contact) return true;
  }
  return false;
}

double lerp_angle_rad(double a, double b, double t) {
  constexpr double kPi = 3.14159265358979323846;
  double d = b - a;
  while (d > kPi) d -= 2.0 * kPi;
  while (d < -kPi) d += 2.0 * kPi;
  return a + d * t;
}

Quat blend_quat_shortest(const Quat& a, const Quat& b, double t) {
  const double s = (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) < 0.0 ? -1.0 : 1.0;
  Quat q{a.x + (s * b.x - a.x) * t, a.y + (s * b.y - a.y) * t,
         a.z + (s * b.z - a.z) * t, a.w + (s * b.w - a.w) * t};
  const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return n > 1e-12 ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : b;
}

Vec3 hermite_pos(const Vec3& p0, const Vec3& v0, const Vec3& p1, const Vec3& v1, double dt, double t) {
  const Vec3 d = p1 - p0;
  if (!(dt > 1e-5) || !std::isfinite(dt)) return p0 + d * t;
  const double chord2 = dot(d, d);
  const double m0_2 = dot(v0, v0) * dt * dt;
  const double m1_2 = dot(v1, v1) * dt * dt;
  // Use linear fallback when velocity is zero or unphysically large relative to chord (e.g. hard collision).
  if ((m0_2 < 1e-8 && m1_2 < 1e-8) || m0_2 > 9.0 * std::max(chord2, 1.0) || m1_2 > 9.0 * std::max(chord2, 1.0)) {
    return p0 + d * t;
  }
  const double t2 = t * t;
  const double t3 = t2 * t;
  const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
  const double h10 = t3 - 2.0 * t2 + t;
  const double h01 = -2.0 * t3 + 3.0 * t2;
  const double h11 = t3 - t2;
  return {
      h00 * p0.x + h10 * (v0.x * dt) + h01 * p1.x + h11 * (v1.x * dt),
      h00 * p0.y + h10 * (v0.y * dt) + h01 * p1.y + h11 * (v1.y * dt),
      h00 * p0.z + h10 * (v0.z * dt) + h01 * p1.z + h11 * (v1.z * dt),
  };
}

RewindActorPose blend_actor_pose(const RewindActorPose& a, const RewindActorPose& b, double dt, double t) {
  const Vec3 d = b.pos - a.pos;
  // True wrap / respawn guard (> 250 m displacement snaps cleanly).
  if (t >= 1.0 || dot(d, d) > 62500.0) return b;
  if (t <= 0.0) return a;
  RewindActorPose out = b;
  out.pos = hermite_pos(a.pos, a.vel, b.pos, b.vel, dt, t);
  out.rot = blend_quat_shortest(a.rot, b.rot, t);
  out.vel = {a.vel.x + (b.vel.x - a.vel.x) * t, a.vel.y + (b.vel.y - a.vel.y) * t,
             a.vel.z + (b.vel.z - a.vel.z) * t};
  if (a.kind == 0) {
    // Race AI: steer is wheel steer angle; wheel_spin is continuous spin angle.
    out.steer = float(double(a.steer) + (double(b.steer) - double(a.steer)) * t);
    out.wheel_spin = float(double(a.wheel_spin) + (double(b.wheel_spin) - double(a.wheel_spin)) * t);
  } else {
    // Traffic / Festival AI / Showcase: steer carries heading rad, wheel_spin carries speed m/s.
    out.steer = float(lerp_angle_rad(double(a.steer), double(b.steer), t));
    out.wheel_spin = float(double(a.wheel_spin) + (double(b.wheel_spin) - double(a.wheel_spin)) * t);
  }
  out.extra0 = float(double(a.extra0) + (double(b.extra0) - double(a.extra0)) * t);
  out.extra1 = float(double(a.extra1) + (double(b.extra1) - double(a.extra1)) * t);
  out.extra2 = float(double(a.extra2) + (double(b.extra2) - double(a.extra2)) * t);
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
  last_extra_ = {};
  has_last_extra_ = false;
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

uint64_t RewindBuffer::record_step(double replay_time, double race_clock, const Vehicle& vehicle,
                                   const RewindCameraState& camera, bool capture_allowed,
                                   uint64_t skill_chain_score, int skill_chain_mult,
                                   double skill_chain_timer, const RewindExtraState* extra) {
  if (state_ != RewindState::kIdle) return 0u;

  if (extra) {
    last_extra_ = *extra;
    has_last_extra_ = true;
  }

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
    stream_.push_back(std::move(s));
    stream_started_ = true;
    next_stream_time_ = replay_time + stream_dt;
  }

  // Never capture an unrecoverable mid-air or rolled-over resume anchor.
  const bool valid_anchor = capture_allowed && vehicle_grounded(vehicle) && !vehicle.is_rolled_over();
  uint64_t captured_id = 0u;
  if (valid_anchor && replay_time >= next_capture_time_ - 1e-9) {
    RewindKeyframe kf(vehicle);
    captured_id = next_snapshot_id_++;
    kf.snapshot_id = captured_id;
    kf.replay_time = replay_time;
    kf.race_clock = race_clock;
    kf.camera = camera;
    kf.skill_chain_score = skill_chain_score;
    kf.skill_chain_mult = skill_chain_mult;
    kf.skill_chain_timer = skill_chain_timer;
    store_extra(kf, extra ? extra : (has_last_extra_ ? &last_extra_ : nullptr));
    push_keyframe(std::move(kf));
    next_capture_time_ = replay_time + cfg_.keyframe_interval_s;
    prune_stream();
  } else if (stream_.size() > 1024) {
    prune_stream();
  }
  return captured_id;
}

bool RewindBuffer::can_trigger() const {
  return enabled_ && !disabled_ && state_ == RewindState::kIdle && !keyframes_.empty();
}

bool RewindBuffer::trigger(double current_replay_time, double race_clock, const Vehicle& current_vehicle,
                           const RewindCameraState& current_camera, uint64_t skill_chain_score,
                           int skill_chain_mult, double skill_chain_timer, const RewindExtraState* extra) {
  if (!can_trigger()) return false;

  // Capture end-state snapshot so Cancel can return to the exact trigger point.
  cancel_snapshot_.emplace(current_vehicle);
  cancel_snapshot_->snapshot_id = next_snapshot_id_++;
  cancel_snapshot_->replay_time = current_replay_time;
  cancel_snapshot_->race_clock = race_clock;
  cancel_snapshot_->camera = current_camera;
  cancel_snapshot_->skill_chain_score = skill_chain_score;
  cancel_snapshot_->skill_chain_mult = skill_chain_mult;
  cancel_snapshot_->skill_chain_timer = skill_chain_timer;
  store_extra(*cancel_snapshot_, extra ? extra : (has_last_extra_ ? &last_extra_ : nullptr));

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
                          double* out_race_clock, uint64_t* out_skill_score, int* out_skill_mult,
                          uint64_t* out_snapshot_id, double* out_skill_timer, RewindExtraState* out_extra) {
  if (state_ != RewindState::kPausedAtKeyframe) return false;
  if (selected_slot_ < 0 || selected_slot_ >= int(keyframes_.size())) return false;

  const RewindKeyframe& kf = keyframes_[size_t(selected_slot_)];
  out_vehicle = kf.vehicle;
  if (out_camera) *out_camera = kf.camera;
  if (out_replay_time) *out_replay_time = kf.replay_time;
  if (out_race_clock) *out_race_clock = kf.race_clock;
  if (out_skill_score) *out_skill_score = kf.skill_chain_score;
  if (out_skill_mult) *out_skill_mult = kf.skill_chain_mult;
  if (out_snapshot_id) *out_snapshot_id = kf.snapshot_id;
  if (out_skill_timer) *out_skill_timer = kf.skill_chain_timer;
  load_extra(kf, out_extra);

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
                          double* out_race_clock, uint64_t* out_skill_score, int* out_skill_mult,
                          uint64_t* out_snapshot_id, double* out_skill_timer, RewindExtraState* out_extra) {
  if (state_ != RewindState::kRewinding && state_ != RewindState::kPausedAtKeyframe) return false;
  if (!cancel_snapshot_.has_value()) return false;

  const RewindKeyframe& kf = *cancel_snapshot_;
  out_vehicle = kf.vehicle;
  if (out_camera) *out_camera = kf.camera;
  if (out_replay_time) *out_replay_time = kf.replay_time;
  if (out_race_clock) *out_race_clock = kf.race_clock;
  if (out_skill_score) *out_skill_score = kf.skill_chain_score;
  if (out_skill_mult) *out_skill_mult = kf.skill_chain_mult;
  if (out_snapshot_id) *out_snapshot_id = kf.snapshot_id;
  if (out_skill_timer) *out_skill_timer = kf.skill_chain_timer;
  load_extra(kf, out_extra);

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

std::vector<uint64_t> RewindBuffer::active_snapshot_ids() const {
  std::vector<uint64_t> ids;
  ids.reserve(keyframes_.size() + (cancel_snapshot_ ? 1u : 0u));
  for (const auto& kf : keyframes_) ids.push_back(kf.snapshot_id);
  if (cancel_snapshot_) ids.push_back(cancel_snapshot_->snapshot_id);
  return ids;
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

bool RewindBuffer::will_record_stream(double replay_time) const {
  if (state_ != RewindState::kIdle) return false;
  return !stream_started_ || replay_time >= next_stream_time_ - 1e-9;
}

void RewindBuffer::attach_latest_stream_actors(std::vector<RewindActorPose> actors) {
  if (stream_.empty()) return;
  stream_.back().actors = std::move(actors);
}

bool RewindBuffer::sampled_actor_poses(std::vector<RewindActorPose>& out) const {
  out.clear();
  if (stream_.empty()) return false;
  // Find the nearest bracketing stream samples that carry actor pose snapshots.
  auto it = std::lower_bound(stream_.begin(), stream_.end(), playback_time_,
                             [](const RewindStreamSample& s, double t) { return s.replay_time < t; });
  const RewindStreamSample* a = nullptr;
  const RewindStreamSample* b = nullptr;
  for (auto fwd = it; fwd != stream_.end(); ++fwd) {
    if (!fwd->actors.empty()) {
      b = &(*fwd);
      break;
    }
  }
  if (it != stream_.begin()) {
    for (auto rev = it;; --rev) {
      if (rev != stream_.end() && rev != it && !rev->actors.empty()) {
        a = &(*rev);
        break;
      }
      if (rev == stream_.begin()) break;
    }
  }
  if (!a && !b) return false;
  if (!a) {
    out = b->actors;
    return true;
  }
  if (!b || a == b) {
    out = a->actors;
    return true;
  }
  const double span = b->replay_time - a->replay_time;
  const double alpha = span > 1e-9 ? std::clamp((playback_time_ - a->replay_time) / span, 0.0, 1.0) : 1.0;
  out.reserve(std::max(a->actors.size(), b->actors.size()));
  for (const auto& ab : b->actors) {
    const RewindActorPose* match_a = nullptr;
    for (const auto& aa : a->actors) {
      if (aa.kind == ab.kind && aa.id == ab.id) {
        match_a = &aa;
        break;
      }
    }
    if (match_a) {
      out.push_back(blend_actor_pose(*match_a, ab, span, alpha));
    } else if (alpha >= 0.5) {
      out.push_back(ab);
    }
  }
  if (alpha < 0.5) {
    for (const auto& aa : a->actors) {
      bool in_b = false;
      for (const auto& ab : b->actors) {
        if (ab.kind == aa.kind && ab.id == aa.id) {
          in_b = true;
          break;
        }
      }
      if (!in_b) out.push_back(aa);
    }
  }
  return !out.empty();
}

bool RewindBuffer::sampled_snapshot_bracket(RewindBracket& out) const {
  out = {};
  struct Node {
    double t = 0.0;
    uint64_t id = 0u;
  };
  std::vector<Node> nodes;
  nodes.reserve(keyframes_.size() + 1);
  for (const auto& kf : keyframes_) {
    if (kf.snapshot_id != 0u) nodes.push_back({kf.replay_time, kf.snapshot_id});
  }
  if (cancel_snapshot_ && cancel_snapshot_->snapshot_id != 0u) {
    if (nodes.empty() || cancel_snapshot_->replay_time >= nodes.back().t - 1e-6) {
      nodes.push_back({cancel_snapshot_->replay_time, cancel_snapshot_->snapshot_id});
    }
  }
  if (nodes.empty()) return false;
  if (playback_time_ <= nodes.front().t) {
    out = {nodes.front().id, nodes.front().id, 0.0, 0.0};
    return true;
  }
  if (playback_time_ >= nodes.back().t) {
    out = {nodes.back().id, nodes.back().id, 1.0, 0.0};
    return true;
  }
  for (size_t i = 1; i < nodes.size(); ++i) {
    if (playback_time_ <= nodes[i].t) {
      const Node& a = nodes[i - 1];
      const Node& b = nodes[i];
      const double span = b.t - a.t;
      const double alpha = span > 1e-9 ? std::clamp((playback_time_ - a.t) / span, 0.0, 1.0) : 1.0;
      out = {a.id, b.id, alpha, span};
      return true;
    }
  }
  out = {nodes.back().id, nodes.back().id, 1.0, 0.0};
  return true;
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
