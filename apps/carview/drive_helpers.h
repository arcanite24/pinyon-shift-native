// Plain Vulkan-free helpers shared by carview --drive and unit tests.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
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

// Interpolates a traffic car pose between two fixed-step snapshots, snapping
// if the displacement exceeds 5 m (matching Vehicle::blend). Non-finite alpha
// falls back to t = 0.0 (start-of-step pose `a`).
inline TrafficPose blend_traffic_pose(const TrafficPose& a, const TrafficPose& b, double alpha) {
  const double t = std::isfinite(alpha) ? std::clamp(alpha, 0.0, 1.0) : 0.0;
  const drive::Vec3 d = b.pos - a.pos;
  if (t >= 1.0 || drive::dot(d, d) > 25.0) return b;
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
