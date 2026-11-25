// Plain Vulkan-free helpers shared by carview --drive and unit tests.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "audio/engine_sound.h"
#include "drive/cameras.h"
#include "drive/vehicle.h"

namespace carview {

// Maps DriveCamera::mode() to EngineSound's CMT Listener/Basic view
// (SPEC-ENGINE-SOUND, SPEC-CAMERAS).
inline audio::View engine_audio_view(drive::CameraMode mode) {
  switch (mode) {
    case drive::CameraMode::kCockpit: return audio::View::kCockpit;
    case drive::CameraMode::kHood:
    case drive::CameraMode::kBumper: return audio::View::kFront;
    case drive::CameraMode::kFollowLow:
    case drive::CameraMode::kFollowHigh: return audio::View::kFollow;
  }
  return audio::View::kFollow;
}

// Per-rig velocity history for CameraPhysics g-force inputs, resettable on
// teleport, car reset, pause transition, or drive loop restart.
struct RigAccelState {
  drive::Vec3 last_vel{};
  bool have_last = false;

  void reset() {
    last_vel = {};
    have_last = false;
  }

  void sample(const drive::Vec3& vel, const drive::Quat& orientation, double dt, double& g_longitudinal,
              double& g_lateral) {
    g_longitudinal = 0.0;
    g_lateral = 0.0;
    if (!std::isfinite(vel.x) || !std::isfinite(vel.y) || !std::isfinite(vel.z)) {
      reset();
      return;
    }
    if (have_last && dt > 1e-4 && dt < 0.25) {
      const drive::Vec3 a{(vel.x - last_vel.x) / dt, (vel.y - last_vel.y) / dt, (vel.z - last_vel.z) / dt};
      const drive::Vec3 fwd = drive::rotate(orientation, {0, 0, 1});
      const drive::Vec3 right = drive::rotate(orientation, {1, 0, 0});
      g_longitudinal = (a.x * fwd.x + a.y * fwd.y + a.z * fwd.z) / 9.81;
      g_lateral = (a.x * right.x + a.y * right.y + a.z * right.z) / 9.81;
    }
    last_vel = vel;
    have_last = true;
  }
};

struct TrafficPose {
  drive::Vec3 pos{};
  drive::Quat rot{};
};

// Interpolates a traffic car pose between two snapshots, snapping only on a
// true wrap/respawn jump (> 250 m, i.e. dot(d, d) > 62500.0). Non-finite alpha
// falls back to t = 0.0 (start-of-step pose `a`).
inline TrafficPose blend_traffic_pose(const TrafficPose& a, const TrafficPose& b, double alpha) {
  const double t = std::isfinite(alpha) ? std::clamp(alpha, 0.0, 1.0) : 0.0;
  const drive::Vec3 d = b.pos - a.pos;
  if (t >= 1.0 || drive::dot(d, d) > 62500.0) return b;
  TrafficPose out;
  out.pos = a.pos + d * t;
  const double s =
      (a.rot.x * b.rot.x + a.rot.y * b.rot.y + a.rot.z * b.rot.z + a.rot.w * b.rot.w) < 0.0 ? -1.0 : 1.0;
  drive::Quat q{a.rot.x + (s * b.rot.x - a.rot.x) * t, a.rot.y + (s * b.rot.y - a.rot.y) * t,
                a.rot.z + (s * b.rot.z - a.rot.z) * t, a.rot.w + (s * b.rot.w - a.rot.w) * t};
  const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  out.rot = n > 0.0 ? drive::Quat{q.x / n, q.y / n, q.z / n, q.w / n} : b.rot;
  return out;
}

// Smooth cubic Hermite position interpolation with overshoot guard (falls back
// to linear lerp when dt <= 0 or tangents exceed 3x chord length).
inline drive::Vec3 hermite_vec3(const drive::Vec3& p0, const drive::Vec3& v0, const drive::Vec3& p1,
                                const drive::Vec3& v1, double dt, double alpha) {
  const double t = std::isfinite(alpha) ? std::clamp(alpha, 0.0, 1.0) : 0.0;
  const drive::Vec3 d = p1 - p0;
  if (!(dt > 1e-5) || !std::isfinite(dt)) return p0 + d * t;
  const double chord2 = drive::dot(d, d);
  const double m0_2 = drive::dot(v0, v0) * dt * dt;
  const double m1_2 = drive::dot(v1, v1) * dt * dt;
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

// Velocity-aware cubic Hermite traffic pose interpolation across keyframe spans.
inline TrafficPose blend_traffic_pose_hermite(const TrafficPose& a, const drive::Vec3& vel_a, const TrafficPose& b,
                                              const drive::Vec3& vel_b, double dt, double alpha) {
  const double t = std::isfinite(alpha) ? std::clamp(alpha, 0.0, 1.0) : 0.0;
  const drive::Vec3 d = b.pos - a.pos;
  if (t >= 1.0 || drive::dot(d, d) > 62500.0) return b;
  if (t <= 0.0) return a;
  TrafficPose out = blend_traffic_pose(a, b, t);
  out.pos = hermite_vec3(a.pos, vel_a, b.pos, vel_b, dt, t);
  return out;
}

// Velocity-aware cubic Hermite Vehicle::Pose interpolation across keyframe spans
// (250 m respawn guard, shortest-arc orientation, smooth wheel steer & spin).
inline drive::Vehicle::Pose blend_vehicle_pose_hermite(const drive::Vehicle::Pose& a, const drive::Vehicle::Pose& b,
                                                       double dt, double alpha) {
  const double t = std::isfinite(alpha) ? std::clamp(alpha, 0.0, 1.0) : 0.0;
  const drive::Vec3 d = b.pos - a.pos;
  if (t >= 1.0 || drive::dot(d, d) > 62500.0) return b;
  if (t <= 0.0) return a;
  drive::Vehicle::Pose out = b;
  out.pos = hermite_vec3(a.pos, a.vel, b.pos, b.vel, dt, t);
  out.vel = a.vel + (b.vel - a.vel) * t;
  out.ang = a.ang + (b.ang - a.ang) * t;
  const double s =
      (a.rot.x * b.rot.x + a.rot.y * b.rot.y + a.rot.z * b.rot.z + a.rot.w * b.rot.w) < 0.0 ? -1.0 : 1.0;
  drive::Quat q{a.rot.x + (s * b.rot.x - a.rot.x) * t, a.rot.y + (s * b.rot.y - a.rot.y) * t,
                a.rot.z + (s * b.rot.z - a.rot.z) * t, a.rot.w + (s * b.rot.w - a.rot.w) * t};
  const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  out.rot = n > 0.0 ? drive::Quat{q.x / n, q.y / n, q.z / n, q.w / n} : b.rot;
  for (size_t i = 0; i < 4; ++i) {
    out.wheels[i].centre = a.wheels[i].centre + (b.wheels[i].centre - a.wheels[i].centre) * t;
    out.wheels[i].compression = a.wheels[i].compression + (b.wheels[i].compression - a.wheels[i].compression) * t;
    out.wheels[i].steer = a.wheels[i].steer + (b.wheels[i].steer - a.wheels[i].steer) * t;
    out.wheels[i].spin = a.wheels[i].spin + (b.wheels[i].spin - a.wheels[i].spin) * t;
    out.wheels[i].omega = a.wheels[i].omega + (b.wheels[i].omega - a.wheels[i].omega) * t;
  }
  return out;
}

// Compact binary serialization helpers for RewindExtraState payloads
// (SpeedActivityTracker records, Outpost/PR stunt state, DiscountSigns set).
namespace rewind_codec {

inline void push_u32(std::vector<uint8_t>& dst, uint32_t v) {
  for (int i = 0; i < 4; ++i) dst.push_back(uint8_t((v >> (i * 8)) & 0xFFu));
}
inline bool read_u32(const std::vector<uint8_t>& src, size_t& off, uint32_t& out) {
  if (off + 4 > src.size()) return false;
  out = uint32_t(src[off]) | (uint32_t(src[off + 1]) << 8) | (uint32_t(src[off + 2]) << 16) |
        (uint32_t(src[off + 3]) << 24);
  off += 4;
  return true;
}
inline void push_f64(std::vector<uint8_t>& dst, double v) {
  uint8_t b[8];
  std::memcpy(b, &v, 8);
  dst.insert(dst.end(), b, b + 8);
}
inline bool read_f64(const std::vector<uint8_t>& src, size_t& off, double& out) {
  if (off + 8 > src.size()) return false;
  std::memcpy(&out, src.data() + off, 8);
  off += 8;
  return true;
}
inline void push_str(std::vector<uint8_t>& dst, const std::string& s) {
  push_u32(dst, uint32_t(s.size()));
  dst.insert(dst.end(), s.begin(), s.end());
}
inline bool read_str(const std::vector<uint8_t>& src, size_t& off, std::string& out) {
  uint32_t len = 0;
  if (!read_u32(src, off, len) || off + len > src.size()) return false;
  out.assign(reinterpret_cast<const char*>(src.data() + off), len);
  off += len;
  return true;
}

inline std::vector<uint8_t> encode_string_set(const std::set<std::string>& items) {
  std::vector<uint8_t> out;
  push_u32(out, uint32_t(items.size()));
  for (const auto& s : items) push_str(out, s);
  return out;
}
inline bool decode_string_set(const std::vector<uint8_t>& bytes, std::set<std::string>& out) {
  out.clear();
  size_t off = 0;
  uint32_t count = 0;
  if (!read_u32(bytes, off, count)) return false;
  for (uint32_t i = 0; i < count; ++i) {
    std::string s;
    if (!read_str(bytes, off, s)) return false;
    out.insert(std::move(s));
  }
  return true;
}

inline std::vector<uint8_t> encode_speed_records(const std::vector<std::pair<std::string, double>>& traps,
                                                 const std::vector<std::pair<std::string, double>>& zones) {
  std::vector<uint8_t> out;
  push_u32(out, uint32_t(traps.size()));
  for (const auto& [k, v] : traps) {
    push_str(out, k);
    push_f64(out, v);
  }
  push_u32(out, uint32_t(zones.size()));
  for (const auto& [k, v] : zones) {
    push_str(out, k);
    push_f64(out, v);
  }
  return out;
}
inline bool decode_speed_records(const std::vector<uint8_t>& bytes,
                                 std::vector<std::pair<std::string, double>>& traps,
                                 std::vector<std::pair<std::string, double>>& zones) {
  traps.clear();
  zones.clear();
  size_t off = 0;
  uint32_t nt = 0;
  if (!read_u32(bytes, off, nt)) return false;
  traps.reserve(nt);
  for (uint32_t i = 0; i < nt; ++i) {
    std::string k;
    double v = 0.0;
    if (!read_str(bytes, off, k) || !read_f64(bytes, off, v)) return false;
    traps.emplace_back(std::move(k), v);
  }
  uint32_t nz = 0;
  if (!read_u32(bytes, off, nz)) return false;
  zones.reserve(nz);
  for (uint32_t i = 0; i < nz; ++i) {
    std::string k;
    double v = 0.0;
    if (!read_str(bytes, off, k) || !read_f64(bytes, off, v)) return false;
    zones.emplace_back(std::move(k), v);
  }
  return true;
}

inline std::vector<uint8_t> encode_pr_stunt_state(const std::set<std::string>& discovered_outposts,
                                                  const std::set<std::string>& completed_pr_stunts,
                                                  const std::string& active_stunt_id) {
  std::vector<uint8_t> out;
  push_u32(out, uint32_t(discovered_outposts.size()));
  for (const auto& s : discovered_outposts) push_str(out, s);
  push_u32(out, uint32_t(completed_pr_stunts.size()));
  for (const auto& s : completed_pr_stunts) push_str(out, s);
  push_str(out, active_stunt_id);
  return out;
}
inline bool decode_pr_stunt_state(const std::vector<uint8_t>& bytes, std::set<std::string>& discovered_outposts,
                                  std::set<std::string>& completed_pr_stunts, std::string& active_stunt_id) {
  discovered_outposts.clear();
  completed_pr_stunts.clear();
  active_stunt_id.clear();
  size_t off = 0;
  uint32_t nd = 0;
  if (!read_u32(bytes, off, nd)) return false;
  for (uint32_t i = 0; i < nd; ++i) {
    std::string s;
    if (!read_str(bytes, off, s)) return false;
    discovered_outposts.insert(std::move(s));
  }
  uint32_t nc = 0;
  if (!read_u32(bytes, off, nc)) return false;
  for (uint32_t i = 0; i < nc; ++i) {
    std::string s;
    if (!read_str(bytes, off, s)) return false;
    completed_pr_stunts.insert(std::move(s));
  }
  return read_str(bytes, off, active_stunt_id);
}

}  // namespace rewind_codec

// Re-syncs the car picker / garage browser index after baked_cars or pick_ids
// are populated or rebuilt (e.g., when garage_picker() shrinks baked_cars on
// first Barn Find discovery or when PauseMenu::kGarage opens).
inline size_t sync_picker_index(const std::vector<std::string>& baked_cars,
                                const std::vector<int64_t>& pick_ids,
                                int64_t selected_garage_id,
                                const std::string& active_car_lower) {
  if (baked_cars.empty()) return 0;
  if (!pick_ids.empty() && pick_ids.size() == baked_cars.size() && selected_garage_id > 0) {
    for (size_t k = 0; k < pick_ids.size(); ++k)
      if (pick_ids[k] == selected_garage_id) return k;
  }
  for (size_t k = 0; k < baked_cars.size(); ++k)
    if (baked_cars[k] == active_car_lower) return k;
  return 0;
}

// Parses "--size WxH" arguments strictly (rejecting zero dimensions or trailing junk).
inline bool parse_size_arg(const char* arg, uint32_t& width, uint32_t& height) {
  if (!arg || !*arg) return false;
  uint32_t w = 0, h = 0;
  char extra = 0;
  if (std::sscanf(arg, "%ux%u%c", &w, &h, &extra) != 2 || w == 0 || h == 0) return false;
  width = w;
  height = h;
  return true;
}

}  // namespace carview
