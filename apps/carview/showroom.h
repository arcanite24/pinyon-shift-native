// carview showroom: draws one baked car (`carbake` output) with our own car
// shader: the default paint, livery masks, lamps, interior, wheels placed at
// the wheel-centre rows with rims, tyres, rotors and callipers, glass last,
// and our own lighting approximation (a procedural studio environment, a key
// and a fill light).
//
// Spec: FORMAT-CARS (Materials, Paint, Wheels, Brakes, Lights, glass),
// AUTOSHOW_REFERENCE (what the showroom draws).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <list>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace carview {

// O(1) LRU resident draw manager with O(log F) best-fit coalescing free-block
// allocators for the GPU-driven world's vertex and index arenas.
// Eliminates O(N) linear eviction scans and full-arena vkDeviceWaitIdle() resets.
class WorldArenaLru {
 public:
  static constexpr uint32_t kNoSpace = 0xFFFFFFFFu;

  class FreeList {
   public:
    void reset(uint32_t capacity) {
      capacity_ = capacity;
      used_ = 0;
      by_offset_.clear();
      by_size_.clear();
      if (capacity > 0) {
        by_offset_[0] = capacity;
        by_size_.emplace(capacity, 0u);
      }
    }

    uint32_t allocate(uint32_t units) {
      if (units == 0) return 0;
      auto sit = by_size_.lower_bound({units, 0u});
      if (sit == by_size_.end()) return kNoSpace;
      const uint32_t sz = sit->first;
      const uint32_t off = sit->second;
      by_size_.erase(sit);
      by_offset_.erase(off);
      const uint32_t left = sz - units;
      if (left > 0) {
        by_offset_[off + units] = left;
        by_size_.emplace(left, off + units);
      }
      used_ += units;
      return off;
    }

    void free(uint32_t offset, uint32_t units) {
      if (units == 0) return;
      used_ -= std::min(used_, units);
      auto next = by_offset_.lower_bound(offset);
      if (next != by_offset_.end() && offset + units == next->first) {
        by_size_.erase({next->second, next->first});
        units += next->second;
        next = by_offset_.erase(next);
      }
      if (next != by_offset_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
          by_size_.erase({prev->second, prev->first});
          offset = prev->first;
          units += prev->second;
          by_offset_.erase(prev);
        }
      }
      by_offset_[offset] = units;
      by_size_.emplace(units, offset);
    }

    uint32_t capacity() const { return capacity_; }
    uint32_t used() const { return used_; }
    uint32_t largest_free() const { return by_size_.empty() ? 0u : by_size_.rbegin()->first; }
    size_t free_ranges() const { return by_offset_.size(); }

   private:
    uint32_t capacity_ = 0;
    uint32_t used_ = 0;
    std::map<uint32_t, uint32_t> by_offset_;
    std::set<std::pair<uint32_t, uint32_t>> by_size_;
  };

  struct Entry {
    uint32_t vertex_base = 0, vertex_count = 0;
    uint32_t index_base = 0, index_count = 0;
    double origin_x = 0.0, origin_z = 0.0;
    uint32_t refs = 0;
    bool valid = false;
    uint64_t last_used = 0;
    uint64_t free_tag = 0;
    bool in_lru = false;
    std::list<uint64_t>::iterator lru_it{};
  };

  WorldArenaLru(uint32_t vertex_capacity = 0, uint32_t index_capacity = 0) { reset(vertex_capacity, index_capacity); }

  void reset(uint32_t vertex_capacity, uint32_t index_capacity) {
    map_.clear();
    unref_lru_.clear();
    vertices_.reset(vertex_capacity);
    indices_.reset(index_capacity);
    evictions_ = 0;
    has_no_evict_frame_ = false;
    no_evict_frame_ = 0;
  }

  Entry* acquire(uint64_t key, uint64_t generation) {
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    Entry& e = it->second;
    if (!e.valid && e.refs == 0) {
      if (e.in_lru) unref_lru_.erase(e.lru_it);
      vertices_.free(e.vertex_base, e.vertex_count);
      indices_.free(e.index_base, e.index_count);
      map_.erase(it);
      return nullptr;
    }
    if (e.refs == 0 && e.in_lru) {
      unref_lru_.erase(e.lru_it);
      e.in_lru = false;
    }
    ++e.refs;
    e.last_used = generation;
    return &e;
  }

  void mark_valid(uint64_t key) {
    auto it = map_.find(key);
    if (it != map_.end()) it->second.valid = true;
  }

  // O(1) amortized eviction from the tail of the unreferenced LRU list.
  bool evict_oldest(uint64_t frames_done, std::vector<uint64_t>* evicted = nullptr) {
    if (unref_lru_.empty()) return false;
    if (has_no_evict_frame_ && frames_done <= no_evict_frame_) return false;
    for (auto it = unref_lru_.end(); it != unref_lru_.begin();) {
      --it;
      const uint64_t k = *it;
      auto mit = map_.find(k);
      if (mit == map_.end()) {
        it = unref_lru_.erase(it);
        continue;
      }
      Entry& e = mit->second;
      if (e.refs != 0 || e.free_tag > frames_done) continue;
      vertices_.free(e.vertex_base, e.vertex_count);
      indices_.free(e.index_base, e.index_count);
      unref_lru_.erase(it);
      map_.erase(mit);
      if (evicted) evicted->push_back(k);
      ++evictions_;
      return true;
    }
    has_no_evict_frame_ = true;
    no_evict_frame_ = frames_done;
    return false;
  }

  Entry* place(uint64_t key, uint32_t vertex_count, uint32_t index_count, double origin_x, double origin_z,
               uint64_t generation, uint64_t frames_done = ~0ull, std::vector<uint64_t>* evicted = nullptr) {
    if (Entry* have = acquire(key, generation)) return have;
    uint32_t v = vertices_.allocate(vertex_count);
    uint32_t i = indices_.allocate(index_count);
    while ((v == kNoSpace || i == kNoSpace) && evict_oldest(frames_done, evicted)) {
      if (v == kNoSpace) v = vertices_.allocate(vertex_count);
      if (i == kNoSpace) i = indices_.allocate(index_count);
    }
    if (v == kNoSpace || i == kNoSpace) {
      if (v != kNoSpace) vertices_.free(v, vertex_count);
      if (i != kNoSpace) indices_.free(i, index_count);
      return nullptr;
    }
    Entry e{};
    e.vertex_base = v;
    e.vertex_count = vertex_count;
    e.index_base = i;
    e.index_count = index_count;
    e.origin_x = origin_x;
    e.origin_z = origin_z;
    e.refs = 1;
    e.valid = false;
    e.last_used = generation;
    e.free_tag = 0;
    e.in_lru = false;
    auto [ins, ok] = map_.emplace(key, e);
    return &ins->second;
  }

  void release(uint64_t key, uint64_t tag = 0) {
    auto it = map_.find(key);
    if (it == map_.end() || it->second.refs == 0) return;
    Entry& e = it->second;
    --e.refs;
    e.free_tag = std::max(e.free_tag, tag);
    if (e.refs == 0) {
      if (!e.valid) {
        // Placement was rolled back before its GPU copy ran: free immediately.
        vertices_.free(e.vertex_base, e.vertex_count);
        indices_.free(e.index_base, e.index_count);
        map_.erase(it);
        return;
      }
      if (!e.in_lru) {
        unref_lru_.push_front(key);
        e.lru_it = unref_lru_.begin();
        e.in_lru = true;
      }
      if (has_no_evict_frame_ && e.free_tag <= no_evict_frame_) has_no_evict_frame_ = false;
    }
  }

  void evict_unreferenced(uint64_t frames_done = ~0ull, std::vector<uint64_t>* evicted = nullptr) {
    while (evict_oldest(frames_done, evicted)) {
    }
  }

  bool contains(uint64_t key) const { return map_.count(key) != 0; }
  const Entry* find(uint64_t key) const {
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
  }
  size_t count() const { return map_.size(); }
  size_t unreferenced_count() const { return unref_lru_.size(); }
  bool has_inflight_unreferenced(uint64_t frames_done) const {
    for (uint64_t k : unref_lru_) {
      auto it = map_.find(k);
      if (it != map_.end() && it->second.refs == 0 && it->second.free_tag > frames_done) return true;
    }
    return false;
  }
  size_t referenced() const { return map_.size() >= unref_lru_.size() ? (map_.size() - unref_lru_.size()) : 0; }
  uint64_t vertex_used() const { return vertices_.used(); }
  uint64_t index_used() const { return indices_.used(); }
  uint32_t vertex_capacity() const { return vertices_.capacity(); }
  uint32_t index_capacity() const { return indices_.capacity(); }
  uint32_t vertex_largest_free() const { return vertices_.largest_free(); }
  uint32_t index_largest_free() const { return indices_.largest_free(); }
  size_t vertex_free_ranges() const { return vertices_.free_ranges(); }
  size_t index_free_ranges() const { return indices_.free_ranges(); }
  uint64_t evictions() const { return evictions_; }

 private:
  std::unordered_map<uint64_t, Entry> map_;
  std::list<uint64_t> unref_lru_;
  FreeList vertices_, indices_;
  uint64_t evictions_ = 0;
  bool has_no_evict_frame_ = false;
  uint64_t no_evict_frame_ = 0;
};

// Pure-C++ helpers shared by Showroom and unit tests (no Vulkan dependency).

// R3-REN-09: Cockpit & Bumper/Hood virtual rearview mirror band scheduler.
// Band 0 = mirrorLeft, Band 1 = mirrorMiddle (center), Band 2 = mirrorRight.
// Center mirror updates EVERY frame (alternating L/R side mirrors on even/odd frames
// in cockpit camera, and center-only every frame in Bumper/Hood virtual mirror mode).
struct MirrorBandSchedule {
  uint32_t bands[3] = {0, 0, 0};
  uint32_t count = 0;
};

inline MirrorBandSchedule schedule_cockpit_mirror_bands(uint64_t frame_index, bool cockpit_camera,
                                                        bool virtual_mirror_active, bool mirror_valid) {
  MirrorBandSchedule s{};
  if (cockpit_camera) {
    if (!mirror_valid) {
      s.bands[0] = 0u;
      s.bands[1] = 1u;
      s.bands[2] = 2u;
      s.count = 3u;
    } else {
      // Center rearview mirror EVERY frame; alternate left (0) and right (2) side mirrors.
      s.bands[0] = 1u;
      s.bands[1] = (frame_index & 1ull) == 0ull ? 0u : 2u;
      s.count = 2u;
    }
  } else if (virtual_mirror_active) {
    // Bumper/Hood virtual rearview mirror renders only the center band every frame.
    s.bands[0] = 1u;
    s.count = 1u;
  }
  return s;
}

// R3-REN-09: Detects forward-facing Bumper or Hood driving cameras in car model space
// (-Z forward, +Z rear) so the virtual rearview mirror activates automatically without
// appearing in Cockpit, Follow chase cameras, LookBack, Photo Mode, or Showroom views.
inline bool is_bumper_or_hood_camera(bool world_lit, bool cockpit_camera, bool draw_car, bool have_bounds,
                                     const std::array<float, 3>& bmin, const std::array<float, 3>& bmax,
                                     const float eye[3], const float vp[16]) {
  if (!world_lit || cockpit_camera || !have_bounds) return false;
  // Camera forward Z in car model space is row 3 col 2 (vp[11]); forward is -Z (< -0.35).
  if (vp[11] > -0.35f) return false;
  const float half_w = std::max(0.95f, 0.5f * (bmax[0] - bmin[0]) + 0.15f);
  if (std::fabs(eye[0]) > half_w) return false;
  if (eye[1] < bmin[1] - 0.15f || eye[1] > bmax[1] + 0.55f) return false;
  if (!draw_car) {
    // Bumper camera: hidden car, eye near front nose (z <= +0.25 m, forward of cabin rear).
    return eye[2] >= bmin[2] - 0.85f && eye[2] <= 0.25f;
  }
  // Hood camera: car drawn, eye sits above hood/cowl strictly inside front/mid footprint
  // (well ahead of FollowLow/FollowHigh chase cameras at eye.z > +3.5 m).
  return eye[2] >= bmin[2] - 0.25f && eye[2] <= std::min(0.25f, bmax[2] - 0.35f);
}

// R3-REN-19: Splits aggregate brake rotor thermal glow [0..1] into front/rear axle channels
// using brake energy bias (default 70% front / 30% rear so front discs visibly glow hotter).
inline std::array<float, 2> split_rotor_heat_bias(float heat, float front_bias = 0.70f) {
  const float h = std::clamp(heat, 0.0f, 1.0f);
  const float fb = std::clamp(front_bias, 0.10f, 0.90f);
  const float rb = 1.0f - fb;
  const float peak = std::max(fb, rb);
  return {std::clamp(h * (fb / peak), 0.0f, 1.0f), std::clamp(h * (rb / peak), 0.0f, 1.0f)};
}

// R3-REN-10: Independent per-corner headlight beam health/flicker multiplier.
// Matches drive::CarDamageState::kLampBreakThreshold (0.45) when corner damage is active.
inline float eval_headlight_corner_damage(float corner_dmg, float legacy_front_dmg, bool has_corner_dmg,
                                          float time_sec, float phase_offset = 0.0f) {
  if (has_corner_dmg) {
    if (corner_dmg >= 0.45f) return 0.0f;
    if (corner_dmg > 0.24f) {
      const float t = time_sec + phase_offset;
      const float flicker = 0.5f + 0.5f * std::sin(t * 27.0f + std::sin(t * 7.0f) * 3.0f);
      const float alive = std::clamp(1.0f - (corner_dmg - 0.24f) / 0.21f, 0.12f, 1.0f);
      return alive * (0.48f + 0.52f * flicker);
    }
    return 1.0f;
  }
  if (legacy_front_dmg >= 0.88f) return 0.0f;
  if (legacy_front_dmg > 0.58f) {
    const float t = time_sec + phase_offset;
    const float flicker = 0.5f + 0.5f * std::sin(t * 27.0f + std::sin(t * 7.0f) * 3.0f);
    const float alive = std::clamp(1.0f - (legacy_front_dmg - 0.58f) / 0.30f, 0.08f, 1.0f);
    return alive * (0.45f + 0.55f * flicker);
  }
  return 1.0f;
}

// R3-REN-10: Splits carbin front headlamp positions (body frame: -X left, +X right, +Z front)
// into distinct Left and Right projector origins, mirroring or offsetting +/-0.62 m as fallback.
struct DualHeadlightOrigins {
  std::array<float, 3> left{-0.62f, 0.70f, 1.95f};
  std::array<float, 3> right{0.62f, 0.70f, 1.95f};
  bool has_left_pts = false;
  bool has_right_pts = false;
};

inline DualHeadlightOrigins resolve_dual_headlight_origins(const float* pts_xyz, const uint8_t* pts_mask, size_t count,
                                                           const std::array<float, 3>& fallback_origin) {
  DualHeadlightOrigins out{};
  double sum_l[3] = {0.0, 0.0, 0.0};
  double sum_r[3] = {0.0, 0.0, 0.0};
  int cnt_l = 0, cnt_r = 0;
  for (size_t i = 0; i < count; ++i) {
    if ((pts_mask[i] & 0x01u) == 0u) continue;
    const float px = pts_xyz[i * 3 + 0];
    const float py = pts_xyz[i * 3 + 1];
    const float pz = pts_xyz[i * 3 + 2];
    if (pz < 0.5f) continue;
    if (px < -0.05f) {
      sum_l[0] += px;
      sum_l[1] += py;
      sum_l[2] += pz;
      ++cnt_l;
    } else if (px > 0.05f) {
      sum_r[0] += px;
      sum_r[1] += py;
      sum_r[2] += pz;
      ++cnt_r;
    }
  }
  if (cnt_l > 0) {
    out.left = {float(sum_l[0] / cnt_l), float(sum_l[1] / cnt_l), float(sum_l[2] / cnt_l)};
    out.has_left_pts = true;
  }
  if (cnt_r > 0) {
    out.right = {float(sum_r[0] / cnt_r), float(sum_r[1] / cnt_r), float(sum_r[2] / cnt_r)};
    out.has_right_pts = true;
  }
  if (out.has_left_pts && !out.has_right_pts) {
    out.right = {-out.left[0], out.left[1], out.left[2]};
  } else if (!out.has_left_pts && out.has_right_pts) {
    out.left = {-out.right[0], out.right[1], out.right[2]};
  } else if (!out.has_left_pts && !out.has_right_pts) {
    out.left = {fallback_origin[0] - 0.62f, fallback_origin[1], fallback_origin[2]};
    out.right = {fallback_origin[0] + 0.62f, fallback_origin[1], fallback_origin[2]};
  }
  return out;
}

// R3-REN-17: Reference resolution-scale and aspect-ratio math matching post.frag and stars.vert.
struct PostScaleParams {
  float res_scale = 1.0f;   // render_height / 720.0 (clamped [0.5, 6.0])
  float star_scale = 1.0f;  // viewport_height / 720.0 (clamped [0.75, 6.0])
  float aspect = 16.0f / 9.0f;
};

inline PostScaleParams compute_post_scale(uint32_t width, uint32_t height) {
  PostScaleParams p{};
  const float w = float(std::max(1u, width));
  const float h = float(std::max(1u, height));
  p.res_scale = std::clamp(h / 720.0f, 0.5f, 6.0f);
  p.star_scale = std::clamp(h / 720.0f, 0.75f, 6.0f);
  p.aspect = std::clamp(w / h, 0.5f, 5.0f);
  return p;
}

}  // namespace carview

#ifndef CARVIEW_ARENA_LRU_ONLY

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#include <volk.h>

#include "cars/bake_format.h"
#include "cars/shader_settings.h"
#include "cars/visual_setup.h"
#include "drive/damage.h"
#include "drive/lamps.h"
#include "renderer.h"
#include "rt_pipeline.h"
#include "rt_scene.h"
#include "world/smashables.h"
#include "world/tod.h"
#include "world/vfx.h"
#include "world/world.h"
#include "world/world_anim.h"
#include "world_gpu.h"
#include "world_texture_lru.h"

namespace carview {

// One wheel's pose for set_wheel_poses, in the body frame (FORMAT-CARS:
// +X right, +Y up, +Z forward): the hub centre, the steer angle about y
// (positive towards +x) and the rolling angle about x (positive rolling
// forward).
struct WheelPose {
  float centre[3] = {0, 0, 0};
  float steer = 0, spin = 0;
  float omega = 0;  // angular speed rad/s (drives blur_rim / chrome_blur_rim blend)
};

// The frame's uniform block (showroom.vert / showroom.frag, set 0 binding 0).
struct ShowroomFrame {
  float view_projection[16];  // car space to Vulkan clip, column-major
  float eye[4];               // camera position in car space
  float key_light[4];         // towards the light, intensity
  float fill_light[4];
  float options[4];  // exposure, environment intensity, livery overlay, metallic paint's metal share
  float tint[4];     // light colour (linear rgb) for diffuse light: environment and both lights
  float reflect_tint[4];  // colour of environment reflections (linear rgb)
  float transparent_view_projection[16];  // glass and lamp covers; usually view_projection
  float stripe[4];  // the factory stripe colour (linear rgb); set by the showroom
  // The drive's sun shadow (world.frag; set by the showroom each frame):
  // the frame's space to the map's clip, then on, texel, strength, taps,
  // and the normal offset in metres.
  float shadow_matrix[16];
  float shadow[4];
  float shadow_bias[4];
  // The world's distance cross-fade (world.frag; ours): the prop LOD
  // distance and its band, and where the props fade out at the gather's
  // edge (start, end), in metres from the eye. Band 0: off.
  float world_fade[4];
  // The drive's light (showroom.frag; set by set_world_light's state each
  // frame): xyz the world's up in the car frame, w 1 to light the car by
  // the world's sun and sky rather than the studio.
  float drive[4];
  // Dynamic 24-hour Time-of-Day lighting and fog (SPEC-SKY-AND-TOD).
  // sun_color[3] < 0 falls back to legacy fixed midday constants.
  float sun_color[4] = {0.9695f, 0.8358f, 0.5840f, -1.0f};
  float sky_color[4] = {0.1687f, 0.2229f, 0.2746f, 0.0f};
  float ground_color[4] = {0.1788f, 0.1729f, 0.1024f, 0.0f};
  float fog_color[4] = {0.0910f, 0.1374f, 0.1446f, 975.0f};
  float fog_scatter[4] = {0.2233f, 0.2232f, 0.2232f, 5.0f};
  float fog_params[4] = {0.00045f, 200.0f, 4000.0f, 0.9f};
  float car_light_top[4] = {0.1687f, 0.2229f, 0.2746f, 1.0f};
  float car_light_bot[4] = {0.1788f, 0.1729f, 0.1024f, 150.0f};
  // Multi-cascade sun shadow far cascade, dynamic cubemap basis, and mirror flags.
  float shadow_matrix_far[16] = {};
  float shadow_far_bias[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float env_basis_x[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  float env_basis_y[4] = {0.0f, 1.0f, 0.0f, 0.0f};
  float env_basis_z[4] = {0.0f, 0.0f, 1.0f, 0.0f};
  // Paint Shop finishes, two-tone pearlescent, window tint, cosmetic damage & dirt.
  float paint_two_tone[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float paint_finish[4] = {0.0f, 0.25f, 0.0f, 0.0f};
  float damage_dirt[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float damage_zones[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  // Car-specific direct sun color scaled by IBLDirectScaleCar (0.6238 vs env 1.30).
  float car_sun_color[4] = {0.464f, 0.400f, 0.280f, -1.0f};
  // Phase B SOTA: 3-cascade stabilized D32 CSM mid cascade, PCSS mode & automotive detail flags.
  float shadow_matrix_mid[16] = {};
  float shadow_mid_bias[4] = {0.0f, 0.0f, 0.0f, 2.0f};  // x mid bias, y 3-cascade (>0.5), z pcss (0/1/2), w cascades (2/3)
  float phase_b_params[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // x car_detail (>0.5), y rotor_glow [0..1], z frame_jitter, w ssr_active
  // World prop AmbientColour from TimeOfDayA.xml (SKY_AND_TOD §2.4).
  float ambient_color[4] = {0.1717f, 0.2079f, 0.2229f, 1.0f};
  // Live cockpit digital MFD telemetry (x=speed_norm, y=tach_norm, z=fuel_norm, w=boost_norm).
  float cockpit_mfd[4] = {0.0f, 0.14f, 0.85f, 0.0f};
  // Extended cockpit MFD & gauge lighting: x=gear (-1=R,0=N,1..8), y=speed_display, z=flags, w=headlight_amt.
  float cockpit_mfd_ext[4] = {0.0f, 0.0f, 1.0f, 0.0f};
  // 4-corner impact damage for independent FL/FR headlight and RL/RR taillight breakage.
  float damage_corners[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  // Full retail ShaderFinishParams (Shared Normal/Metallic/Matte + per-car ColorShaderSettings1..17.xml).
  float paint_shader_a[4] = {1.55f, 0.90f, 1.0f, 0.0f};   // fresnel_index, fresnel_scalar, spec_cube_scalar, spec_amount
  float paint_shader_b[4] = {3.0f, 12.0f, 2.0f, 4.0f};    // spec_amount2, spec_power2, spec_amount3, spec_power3
  float paint_shader_c[4] = {0.0f, 0.0f, 1.0f, 5.0f};     // metal_color.rgb, metal_power
  float paint_shader_d[4] = {0.0f, 0.0f, 75.0f, 3.0f};    // metal_bias, metal_scale, spec_power, gauge_emissive_scale
  // Per-region paint overrides (0=hood, 1=wing, 2=mirrors): secondary color/power and finish/scale/bias/active.
  float region_two_tone[3][4] = {{0, 0, 0, 4.0f}, {0, 0, 0, 4.0f}, {0, 0, 0, 4.0f}};
  float region_finish[3][4] = {{0, 0.25f, 0, 0}, {0, 0.25f, 0, 0}, {0, 0.25f, 0, 0}};
  // Per-car RimShaderSettings.xml + dirt/crush feature flags.
  float rim_shader[4] = {1.55f, 0.90f, 1.0f, 0.0f};       // fresnel_index, fresnel_scalar, cube_scale, has_override
  float rim_tint_dirt[4] = {1.0f, 1.0f, 1.0f, 0.0f};      // rim_tint.rgb, bit0=car_dirt_mask bit1=car_crush_weights
  // R3-REN-10 / R3-REN-19: Dynamic night streetlamp/tunnel/brake/backfire point lights on cars
  // + split front/rear brake rotor thermal glow channels.
  float dyn_light_pos[6][4] = {};                          // xyz render-space pos, w radius (m)
  float dyn_light_col[6][4] = {};                          // rgb linear HDR color * intensity, w spec_scale
  float dyn_light_meta[4] = {0.0f, 0.0f, 0.0f, 0.0f};      // x active count (0..6), y rotor_front, z rotor_rear, w unused
};
static_assert(sizeof(ShowroomFrame) == 1264);

// Additional per-draw flags in ShowroomPush::params.y (exact float32 bits 0..23).
inline constexpr uint32_t kFlagBlurRim = 4u;
inline constexpr uint32_t kFlagCarbonTex = 4194304u;
inline constexpr uint32_t kFlagHasBlurMesh = 8388608u;

// Per-draw push constants.
struct ShowroomPush {
  float model[16];
  float uv0[4];
  float uv1[4];
  float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float params[4];  // material class, flags, tyre rim radius, tyre outer radius
};
static_assert(sizeof(ShowroomPush) == 128);

// A world texture ready for upload: the untiled levels of a texture record
// (WORLD-MATERIALS), sampled as sRGB when its format word says gamma.
struct WorldTextureData {
  uint32_t record = 0;
  bool srgb = false;
  cars::bake::Texture texture;
};
constexpr uint32_t kNoWorldTexture = 0xFFFFFFFFu;

// Scenery drawn around the car (carview --drive): triangles in groups of
// one flat colour each, with positions relative to an origin the caller
// folds into set_world_transform (floats stay precise far from zero). With
// materials (the textured world) the triangles also carry per corner a
// normal, the surface and lightmap UVs and the vertex colour, and per
// triangle a material.
// The decals' depth bias (ours): in float ulps of the primitive's depth and
// times its depth slope.
constexpr float kDecalBiasConstant = 64.0f, kDecalBiasSlope = 1.0f;

struct WorldTriangles {
  std::vector<std::array<float, 3>> positions;  // three per triangle
  std::vector<uint8_t> group;                   // per triangle: index into colours
  std::vector<std::array<float, 3>> colours;    // linear rgb per group
  struct Material {
    uint32_t texture = kNoWorldTexture;  // texture record of slot 0
    uint8_t blend = 0;                   // world::Blend: opaque, coverage, alpha
    uint8_t far = 0;                     // world::MeshMaterial::far: pushed back in depth
    uint8_t decal = 0;                   // world::decal_effect: drawn with a depth bias
    uint8_t layers = 0;                  // world::Layers: one, vertex blend, splat
    uint32_t layer_b = kNoWorldTexture;  // Blend_B
    uint32_t splat = kNoWorldTexture;    // Splat
    uint32_t light_map = kNoWorldTexture;   // Light_Map (night emissive spill)
    uint32_t normal_map = kNoWorldTexture;  // NormalMap (DXN/BC5)
    uint32_t ao_map = kNoWorldTexture;      // AO / AOMap
    uint32_t layer_c = kNoWorldTexture;     // Blend_C
    uint32_t spec_map = kNoWorldTexture;    // SpecularMap
    std::array<float, 4> tiling{1, 1, 1, 1};  // Blend_A's UV tiling in xy, Blend_B's (or decal noise's) in zw
    std::array<float, 2> tiling_c{1, 1};      // Blend_C's UV tiling
    std::array<float, 2> decal_noise_c4{3.5f, 8.0f};  // H_VBLND_DECAL pixel constant 4 xy
    float emissive_force = 0.0f;
    uint8_t is_emissive_map = 0;
    uint8_t is_water = 0;
    uint8_t anim_flag = 0;               // world::MeshMaterial::anim_flag (1=flag cloth, 2=countdown LED, 3=flipbook LED)
    uint8_t is_grass = 0;                // world::MeshMaterial::is_grass (_0x00005007)
    uint8_t is_road = 0;                 // world::MeshMaterial::is_road (H_ROAD*)
    uint8_t fade = 0;                    // world::MeshMaterial::fade: the distance cross-fade class
    uint8_t interior = 0;                // an H_INT_* effect (the hub rooms; params.w packs c0.z, c2.x)
  };
  std::vector<std::array<float, 3>> normals;
  std::vector<std::array<float, 2>> uv0, uv1, uv2;
  std::vector<std::array<uint8_t, 4>> vertex_colours;
  std::vector<uint32_t> material;  // per triangle: index into materials
  std::vector<std::array<float, 3>> instance;  // per triangle: its prop's origin (world::Mesh::instance)
  std::vector<Material> materials;
  // Textures not uploaded before, to upload with this scenery.
  std::vector<std::shared_ptr<const WorldTextureData>> textures;
};

// The textured world's vertex (world.vert).
struct WorldVertex {
  float position[3];
  int8_t normal[4];
  float uv0[2], uv1[2];
  uint8_t colour[4];
  // A prop's origin less the corner, in decimetres (the level is picked
  // per instance in world.vert); zero elsewhere.
  int16_t centre[4];
  // TEXCOORD2 (lightmap / AO UV) stored as IEEE-754 float16 pair, keeping the
  // stride at 48 bytes (16-byte aligned for vertex fetch).
  uint16_t uv2[2] = {0, 0};
};
static_assert(sizeof(WorldVertex) == 48);

// WorldTriangles as draw data: one vertex per corner with the face normal,
// indices, and one draw per colour. Plain CPU work with no Vulkan, so a
// streaming worker can build it (build_world) and the render thread only
// uploads it.
struct WorldGeometry {
  struct Draw {
    uint32_t first_index = 0, index_count = 0;
    uint32_t hidden_index_count = 0;  // saved index_count while masked by smashables (restorable on Rewind)
    ShowroomPush push{};
    uint32_t texture = kNoWorldTexture;  // textured: the texture record drawn
    uint32_t texture_b = kNoWorldTexture, splat = kNoWorldTexture;  // terrain layers / decal noise
    uint32_t light_map = kNoWorldTexture, normal_map = kNoWorldTexture;
    uint32_t ao_map = kNoWorldTexture, layer_c = kNoWorldTexture;
    uint32_t spec_map = kNoWorldTexture;
    uint8_t blend = 0;
    uint8_t decal = 0;  // drawn with the decal depth bias
    // The draw's box in the scenery's frame (tree sprites padded by their
    // corners' reach), tested against the view each frame; cull false:
    // always drawn (the sky, the flat world).
    bool cull = false;
    float box_min[3] = {0, 0, 0}, box_max[3] = {0, 0, 0};
    // The GPU-driven world (world_gpu.h): a resident draw lives in the
    // arenas (first_index and vertex_offset then address them, its indices
    // relative to its own vertices), found again across regathers by `key`.
    // `shift` (m, x and z) is what its positions need added to be relative
    // to the scenery being drawn; `fade_in` marks a draw new to the scenery,
    // dithered in with the swap; `gpu` that the culling pass draws it.
    bool resident = false, fade_in = false, gpu = false;
    int32_t vertex_offset = 0;
    uint64_t key = 0;
    uint32_t vertex_begin = 0, vertex_count = 0;  // its vertices in the staged buffer
    float shift[2] = {0, 0};
    // Per-draw resolved descriptor cache invalidated by Showroom::world_sets_gen_.
    mutable VkDescriptorSet cached_set = VK_NULL_HANDLE;
    mutable uint64_t cached_set_gen = 0;
    mutable uint8_t cached_layer_fallback = 0;
  };
  bool textured = false;
  std::vector<WorldVertex> textured_vertices;  // textured: in place of vertices
  std::vector<std::shared_ptr<const WorldTextureData>> textures;
  std::vector<cars::bake::Vertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<Draw> draws;
  size_t triangles = 0;
  // Maximum vertex-to-prop-origin radius across all fade==1/2 draws (m).
  float near_prop_reach = 0.0f;
};
WorldGeometry build_world(const WorldTriangles& world);

struct ShowroomOptions {
  size_t colour = 0;            // index into the car's colours (by Sequence)
  int64_t match_rgb = -1;       // when set (r x 65536 + g x 256 + b): the colour with that sRGB
  // When set: the colour with that Combo_Colors Sequence (a garage row's
  // DefaultManufacturerColorIndex, SPEC-AUTOSHOW-PAINT); none: the lowest.
  int64_t match_sequence = -1;
  bool livery_overlay = false;  // tint the livery mask magenta on the paint
  bool all_variants = false;    // draw every part variant and upgrade part
  // The Data_Car row shown (0: the baked row). Rows sharing a model can
  // differ in stock parts and factory livery (FORMAT-CARS, Stock parts).
  int64_t car_id = 0;
  // Optional game media root (for on-demand aftermarket rims, per-car tyres,
  // and upgraded brake rotors/calipers from media/brakes/).
  std::filesystem::path game_root;
  // Optional fitted visual upgrades (body kits, wing, hood, skirts, cage,
  // exhaust, aftermarket rim, upgraded brakes, ride height, track offsets,
  // tyre widths/rim sizes, camber).
  cars::VisualSetup visual;
  // Prefer LOD1-5 exterior meshes and omit interior/cockpit meshes (mobile traffic & AI opponents).
  bool prefer_lod1 = false;
  // Minimum LOD index when prefer_lod1 is set (0/1 = LOD1 with LOD2/3 far_draws_, 2 = LOD2 directly).
  int32_t prefer_min_lod = 0;
};

class Showroom {
 public:
  explicit Showroom(Renderer& renderer) : r_(renderer) {}
  Showroom(const Showroom&) = delete;
  Showroom& operator=(const Showroom&) = delete;
  ~Showroom();

  // Reads <bake>/cars/<car>.shiftbake, its wheel and tyre bakes and
  // <bake>/shared.shiftbake, and builds the draw list. CPU only.
  // When bake_dir is empty or a .shiftbake file is missing, decodes directly
  // from the game disc in memory via the shared thread-safe disc cache.
  bool load(const std::filesystem::path& bake_dir, const std::string& car,
            const ShowroomOptions& options, std::string& error);
  // Configures the shared in-memory disc loader used when .shiftbake files are
  // absent (zero-bake direct-from-disc mode). Thread-safe.
  static void set_disc_root(const std::filesystem::path& game_root,
                            const std::filesystem::path& state_root = {});
  // Discovers all cars available directly from the game disc / DLC packages
  // (Data_Car.Id -> MediaName and sorted distinct lower-case media names).
  static bool discover_disc_cars(const std::filesystem::path& game_root,
                                 const std::filesystem::path& state_root,
                                 std::map<int64_t, std::string>& out_ids,
                                 std::vector<std::string>& out_media,
                                 std::string* error = nullptr);
  // Creates the pipelines and uploads geometry and textures; the renderer
  // must be initialised. Installs itself as the renderer's scene.
  bool upload(std::string& error);
  // Hot-swaps the player car's bake, geometry, textures, material sets and
  // bounds in-place while keeping Renderer, pipelines, WorldGpu, world
  // textures/draws, sky, VFX, carlights and dynamic reflections alive.
  bool reload_car(const std::filesystem::path& bake_dir, const std::string& car,
                  const ShowroomOptions& options, std::string& error);
  void set_reflection_rate(float rate) { reflection_rate_ = std::clamp(rate, 0.0f, 1.0f); }
  float reflection_rate() const { return reflection_rate_; }
  void set_low_memory_mode(bool on) { low_memory_mode_ = on; }
  void set_virtual_mirror_enabled(bool enabled);
  bool virtual_mirror_enabled() const { return virtual_mirror_enabled_; }
  bool virtual_mirror_active() const { return virtual_mirror_active_; }
  bool mirror_targets_allocated() const { return mirror_tex_.view != VK_NULL_HANDLE; }
  // The caller's camera and lighting; the stripe colour, paint finish, and damage state are the car's own.
  void set_frame(const ShowroomFrame& frame) {
    frame_ = frame;
    for (int i = 0; i < 4; ++i) {
      frame_.stripe[i] = stripe_[i];
      frame_.world_fade[i] = world_fade_[i];
      frame_.paint_two_tone[i] = paint_two_tone_[i];
      frame_.paint_finish[i] = paint_finish_[i];
      frame_.damage_dirt[i] = damage_dirt_[i];
      frame_.damage_zones[i] = damage_zones_[i];
      frame_.cockpit_mfd[i] = cockpit_mfd_[i];
      frame_.cockpit_mfd_ext[i] = cockpit_mfd_ext_[i];
      frame_.damage_corners[i] = damage_corners_[i];
      frame_.paint_shader_a[i] = paint_shader_a_[i];
      frame_.paint_shader_b[i] = paint_shader_b_[i];
      frame_.paint_shader_c[i] = paint_shader_c_[i];
      frame_.paint_shader_d[i] = paint_shader_d_[i];
      frame_.rim_shader[i] = rim_shader_[i];
      frame_.rim_tint_dirt[i] = rim_tint_dirt_[i];
    }
    frame_.dyn_light_meta[1] = rotor_glow_front_;
    frame_.dyn_light_meta[2] = rotor_glow_rear_;
    for (int r = 0; r < 3; ++r) {
      for (int i = 0; i < 4; ++i) {
        frame_.region_two_tone[r][i] = region_two_tone_[r][i];
        frame_.region_finish[r][i] = region_finish_[r][i];
      }
    }
    if (custom_livery_active_) frame_.options[2] = 2.0f;
    sync_motion_scene();
    update_mirror_mode();
  }
  // Moves the wheels, rotors and callipers (LF, RF, LR, RR). Until it is
  // called they stand at the car's rest centres, as the showroom shows them.
  void set_wheel_poses(const WheelPose poses[4]);
  // Replaces the scenery (after upload); the GPU is idled first. Empty
  // clears it. The car-coverage mask never includes it.
  bool set_world(const WorldTriangles& world, std::string& error);
  bool set_world(WorldGeometry geometry, std::string& error);
  // Scenery staged for upload: its draws, with the vertices and indices
  // already in host-visible buffers. stage_world only creates and fills new
  // objects, so a streaming worker may call it while this thread renders;
  // begin_world then starts the GPU copy into device memory without
  // waiting for it, and the scenery in use stays drawn until poll_world
  // finds the copy done and swaps (set_world(StagedWorld) does both,
  // waiting). Staging buffers are freed when the last copy of the
  // StagedWorld goes (before the Showroom).
  struct StagedWorld {
    std::vector<WorldGeometry::Draw> draws;
    size_t triangles = 0;
    bool textured = false;
    std::vector<std::shared_ptr<const WorldTextureData>> textures;
    std::shared_ptr<Renderer::Buffer> vertices, indices;
    // The textures' levels packed in one host-visible buffer by the worker
    // (ours: uploaded in the scenery's fenced copy, not one wait each):
    // per texture, its offset and level count (0: upload it apart).
    std::shared_ptr<Renderer::Buffer> texels;
    std::vector<std::pair<VkDeviceSize, uint32_t>> texel_ranges;
    std::vector<uint32_t> texel_skips;
    // Their images, made by the worker too (per texture; empty where it
    // was already uploaded or failed, which begin_world then handles).
    // The ones begin_world does not take go with the StagedWorld.
    std::shared_ptr<std::vector<Renderer::Image>> images;
    std::shared_ptr<const void> pin_guard;
    double origin_x = 0, origin_z = 0;  // what the positions are relative to
    bool arena = false;                 // staged for the resident arenas (indices relative to each draw)
    struct TreeRaw {
      float base[3];
      float half_width;
      float up[3];
      float height;
      float uv[4];
      uint32_t rgba8;
      uint32_t texture;
    };
    std::vector<TreeRaw> trees;
    using PropSpan = gpuworld::PropSpan;
    // Per-prop-draw smashable instance spans extracted off-thread in stage_world() from cached RAM.
    std::vector<std::pair<uint64_t, std::vector<PropSpan>>> prop_spans;
    float near_prop_reach = 0.0f;
    // Draw keys freshly allocated and copied into the GPU arena in place_world_arena() (intact indices).
    std::unordered_set<uint64_t> uploaded_keys;
  };
  // `origin_x/z`: what the geometry's positions are relative to, for the content keys of the GPU-driven world.
  bool stage_world(WorldGeometry geometry, StagedWorld& out, std::string& error, double origin_x = 0,
                   double origin_z = 0);
  bool begin_world(StagedWorld staged, std::string& error);
  // Swaps in a begun world once its copy is done (or waits, with `wait`).
  // `swapped`: whether it did.
  bool poll_world(bool wait, bool* swapped);
  bool has_pending_world() const { return pending_world_ != nullptr; }
  bool set_world(StagedWorld staged, std::string& error);
  // AOT shader/pipeline pre-compilation helpers (zero first-frame lazy compile).
  bool init_minimal_for_precompile();
  bool precompile_rt_pipelines(std::string& error);
  bool prewarm_rt_targets(uint32_t width, uint32_t height);
  // Per-frame ring-buffered async upload budget (default 2.5 ms, 0 = unbounded).
  void set_upload_budget_ms(double ms) { upload_budget_ms_ = ms; }
  double upload_budget_ms() const { return upload_budget_ms_; }
  void queue_deferred_world_texture(std::shared_ptr<const WorldTextureData> tex);
  size_t deferred_upload_queue_size() const;
  // The origin of the scenery being drawn (its StagedWorld's).
  double world_origin_x() const { return world_origin_[0]; }
  double world_origin_z() const { return world_origin_[1]; }
  // The scenery's positions to the car's model frame (column-major).
  void set_world_transform(const float transform[16]);
  // Dynamic unlit alpha-blended triangles in the scenery's frame (relative to
  // world_origin_x/z), e.g. the 3D driving/braking line ribbon. Empty clears it.
  void set_world_overlay(std::span<const WorldVertex> triangles);
  size_t world_triangle_count() const { return world_triangles_; }
  // The world draws of the last recorded frame: recorded and culled, and
  // the indices drawn.
  struct WorldDrawStats {
    size_t drawn = 0, culled = 0, indices = 0;
    // Draws whose texture did not upload (drawn flat with the white set) or
    // whose layer set did not (slot 0 alone).
    size_t white = 0, layer_fallback = 0;
  };
  WorldDrawStats world_draw_stats() const { return world_stats_; }
  uint32_t gpu_resident_draws() const {
    if (arena_lru_.vertex_capacity() > 0 && !arena_use_rt_resident_) return uint32_t(arena_lru_.count());
    return gpu_world_ ? gpu_world_->resident_draws() : 0u;
  }
  uint32_t gpu_hiz_culled() const { return gpu_world_ ? gpu_world_->last_hiz_culled() : 0u; }
  uint32_t gpu_trees_drawn() const { return gpu_world_ ? gpu_world_->last_trees_drawn() : 0u; }
  double pipeline_compile_ms() const { return pipeline_compile_ms_; }
  uint32_t last_env_draws() const { return env_draws_; }
  uint32_t last_mirror_draws() const { return mirror_draws_; }
  uint32_t last_car_draws() const { return car_draws_; }
  uint32_t last_particle_sprites() const { return particle_alpha_count_ + particle_add_count_; }
  uint32_t last_skid_tris() const { return (skid_asphalt_count_ + skid_rut_count_) / 3u; }
  uint64_t world_staging_bytes() const { return world_staging_bytes_.load(std::memory_order_relaxed); }
  VkImageView hiz_image_view() const { return gpu_world_ ? gpu_world_->hiz_image_view() : VK_NULL_HANDLE; }
  // Frustum culling of the world's draws (on by default).
  void set_world_culling(bool on) { world_cull_ = on; }
  const ShowroomFrame& frame() const { return frame_; }
  const float* world_transform() const { return world_transform_; }
  // The props' distance cross-fade (ShowroomFrame::world_fade; band 0 off)
  // and how long a swapped-in scenery takes to dither in over the one it
  // replaces (0: swapped at once). Both ours.
  void set_world_fade(float lod, float band, float edge_start, float edge_end) {
    world_fade_[0] = lod, world_fade_[1] = band, world_fade_[2] = edge_start, world_fade_[3] = edge_end;
    for (int i = 0; i < 4; ++i) frame_.world_fade[i] = world_fade_[i];
  }
  void set_world_swap_fade(double seconds) { swap_fade_seconds_ = seconds; }
  // The colour the scene clears to, as stored in the 8-bit target (0-1).
  void set_background(float r, float g, float b);
  // The textured world's light, in the frame's space: towards the sun, up,
  // and an exposure (WORLD-MATERIALS' sunDir is in the game frame).
  void set_world_light(const float sun[3], const float up[3], float exposure);
  // Real-time sky from media/realtimesky.zip (atmosphere, Stars.bin, sun/moon, clouds).
  bool upload_sky(const world::RealTimeSkyAssets& assets, std::string& error);
  bool has_sky() const { return sky_ready_; }
  // Leaves the sky out of the frame (the background colour shows instead):
  // the hub rooms are interiors with no time of day (MENU-02).
  void set_sky_hidden(bool hidden) { sky_hidden_ = hidden; }
  // Updates dynamic 24-hour Time-of-Day lighting, fog, and sky parameters.
  void set_tod(const world::TodState& tod, int moon_phase, float real_time_sec, float eye_game_y);
  // The state set_tod last took, for a frame that draws another place's
  // light (a hub room's) until the next set_tod.
  world::TodState& tod_state() { return tod_state_; }
  // Updates wind animation parameters (from TrackSettings.xml) for waving flags & grass.
  void set_wind(float speed, float amplitude) { wind_speed_ = speed, wind_amplitude_ = amplitude; }
  // The direction the sun shadow is cast from (towards the light, the
  // frame's space); null: the world light's sun.
  void set_shadow_direction(const float* towards_light) {
    shadow_direction_set_ = towards_light != nullptr;
    if (towards_light)
      for (int k = 0; k < 3; ++k) shadow_direction_[k] = towards_light[k];
  }
  // Whether the car is drawn (the world reference frame leaves it out).
  void set_draw_car(bool draw) { draw_car_ = draw; }
  void set_driver_visible(bool visible) { draw_driver_ = visible; }
  // Paint (SPEC-AUTOSHOW-PAINT, Manufacturer colors): the car's colour
  // rows, the drawn one, and a live preview that retints the paint draws
  // (metallic flag and stripe too; a row's own atlas needs the car rebuilt).
  const std::vector<cars::bake::Colour>& colours() const { return car_.car.colours; }
  size_t colour_index() const { return options_.colour; }
  void preview_colour(size_t index);
  // Paint Shop finishes, per-region colours (body, hood, wing, mirrors,
  // aftermarket non-chrome rims, brake calipers) & window tint.
  void set_paint_spec(const cars::CarPaintSpec& paint);
  // Uploads a composited 1024x1024 RGBA8 custom vinyl livery texture onto GPU
  // and binds it onto all car paint material descriptor sets.
  bool set_custom_livery(const uint8_t* rgba, uint32_t width, uint32_t height, std::string& error);
  // Applies cosmetic impact deformation, scratches, cracked glass, and body/tyre dirt.
  void set_damage_state(const drive::CarDamageState& dmg);
  // Returns the real baked car-model-space centroid and half-extents for a detachable part
  // (0=front bumper, 1=rear bumper, 2=left mirror, 3=right mirror, 4=rear wing).
  bool detached_part_bounds(int part_kind, std::array<float, 3>& out_center,
                            std::array<float, 3>& out_half_extents) const;
  // Applies a world-simulated car-model-space rigid transform to a detached part's actual
  // baked 3D mesh while it tumbles away after impact; once inactive, the part hides cleanly.
  void set_detached_part_transform(int part_kind, bool active, const float xform[16]);
  void clear_detached_part_transforms();
  // A prop's reach from its origin (m): the box test of LOD00 cells allows
  // an instance whose origin is inside the band but whose corners are not.
  static constexpr float kPropReach = 40;
  // Full vehicle lamp state (CAR_LAMPS.md): headlights, DRLs, tail, brake,
  // reverse, indicators, side lights, numplate, and cockpit gauge backlights.
  void set_braking(float glow);
  // Sets thermal brake rotor glow [0..1] independently of instantaneous brake pedal lamps.
  // Single-argument overload splits heat with ~70/30 front brake bias.
  void set_rotor_heat(float heat);
  void set_rotor_heat(float front_heat, float rear_heat);
  float rotor_glow_front() const { return rotor_glow_front_; }
  float rotor_glow_rear() const { return rotor_glow_rear_; }
  void set_lamp_state(const drive::CarLampState& state);
  void set_lamp_vectors(const drive::LampShaderVectors& vectors);
  // Loads kind-5 .carbin lamp points and per-car ShaderSettings.xml from disc.
  void load_car_lamps(const std::filesystem::path& game_root, const std::string& car);
  // Loads media/carlights/headlight_glow_1..7.xds + headlight_beam_dip.xds.
  bool upload_carlights(const std::filesystem::path& media_dir, std::string& error);
  // Updates world streetlamp / festival / outpost glow sprites for the active scenery.
  void set_world_glows(std::vector<world::WorldGlow> glows) { world_glows_ = std::move(glows); }
  // Optional showroom static cubemap (media/tracks/uiautoshow/staticcarcubemap.xpr).
  // Call before upload() to replace the procedural studio cubemap.
  void set_environment_xpr(std::filesystem::path xpr_path) { env_xpr_path_ = std::move(xpr_path); }
  // Living cockpit animation (steering wheel rotation, RPM/speed/fuel/boost/power
  // gauge needles, and cockpit mirror render pass enable).
  void set_cockpit_anim(const drive::CockpitAnimState& anim, bool cockpit_camera);
  // Real cockpit MFD telemetry, warning indicators, and driver gear-shift/handbrake animations.
  struct CockpitTelemetry {
    int gear = 0;                    // -1=R, 0=N, 1..8
    float speed_mps = 0.0f;          // live vehicle forward speed (m/s)
    float dial_max_speed_mps = 80.0f;
    bool metric_units = true;        // true=km/h, false=mph
    bool abs_active = false;
    bool tcs_active = false;
    bool handbrake_active = false;
    bool high_beam = false;
    bool turn_left = false;
    bool turn_right = false;
    bool shifting = false;
    float dt = 1.0f / 60.0f;
  };
  void set_cockpit_telemetry(const CockpitTelemetry& telemetry);
  const drive::CockpitGaugeCalibration& gauge_calibration() const { return gauge_calibration_; }
  const cars::CarShaderOverrides& car_shader_overrides() const { return car_shader_overrides_; }
  // CameraOverrides CockpitWheelPositionOffset{X,Y,Z} (body frame, m) and
  // CockpitWheelRotationOffset (deg) applied to the drawn steering wheel.
  void set_cockpit_wheel_offsets(std::array<float, 3> pos_offset_m, float rot_offset_deg);
  // Animates 3D active aero spoilers, pop-up headlights, and severely damaged loose bumper/mirror rattle.
  void set_animated_car_parts(float aero_deploy, float popup_open, float time_sec, float dt = 1.0f / 60.0f);
  // True when this car carries animated active aero (rear wing/flaps or active engine vents).
  bool has_active_aero() const {
    for (const auto& d : draws_) {
      if (d.anim_part == 1 || d.anim_part == 7) return true;
    }
    return false;
  }
  // Pop-up headlight pod openness [0..1] (always 1.0f on cars without retractable pods).
  float popup_open() const {
    for (const auto& d : draws_) {
      if (d.anim_part == 2 || d.anim_part == 9 || d.anim_part == 10) {
        return popup_open_smooth_ >= 0.95f ? 1.0f : popup_open_smooth_;
      }
    }
    return 1.0f;
  }
  // Rear-most body-frame exhaust tip exit positions (+X right, +Y up, +Z forward, -Z rear)
  // derived from the car's actual baked/disc exhaust part geometry.
  std::vector<std::array<float, 3>> exhaust_tips_local() const {
    const float cp = std::cos(car_.car.pitch), sp = std::sin(car_.car.pitch);
    const float lift = car_.car.lift;
    auto to_body = [&](const float m[16], const float p[3]) -> std::array<float, 3> {
      const float mx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
      const float my = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
      const float mz = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
      return {mx, cp * my + sp * mz + lift, sp * my - cp * mz};
    };

    std::vector<std::array<float, 3>> raw;
    for (const auto& d : draws_) {
      if (d.wheel >= 0 || d.cockpit_role >= 0 || d.index_count == 0) continue;
      if (d.part_name.find("exhaust") == std::string::npos) continue;
      if (size_t(d.first_index) + size_t(d.index_count) > indices_.size()) continue;

      std::vector<std::array<float, 3>> pts;
      pts.reserve(std::min<size_t>(d.index_count, 512));
      float min_z = 1e9f;
      for (uint32_t i = 0; i < d.index_count; ++i) {
        const int64_t vi = int64_t(d.vertex_offset) + int64_t(indices_[size_t(d.first_index) + size_t(i)]);
        if (vi < 0 || size_t(vi) >= vertices_.size()) continue;
        const auto bp = to_body(d.rest_model, vertices_[size_t(vi)].position.data());
        min_z = std::min(min_z, bp[2]);
        pts.push_back(bp);
      }
      if (pts.empty() || min_z > 1e8f) continue;

      const float z_band = min_z + 0.08f;
      float min_x = 1e9f, max_x = -1e9f;
      double sum_x = 0.0, sum_y = 0.0;
      size_t rim_n = 0;
      for (const auto& p : pts) {
        if (p[2] > z_band) continue;
        min_x = std::min(min_x, p[0]);
        max_x = std::max(max_x, p[0]);
        sum_x += double(p[0]);
        sum_y += double(p[1]);
        ++rim_n;
      }
      if (rim_n == 0) continue;

      if (min_x < -0.18f && max_x > 0.18f && (max_x - min_x) > 0.45f) {
        double lx = 0.0, ly = 0.0, rx = 0.0, ry = 0.0;
        float lz = 1e9f, rz = 1e9f;
        size_t ln = 0, rn = 0;
        for (const auto& p : pts) {
          if (p[2] > z_band) continue;
          if (p[0] < 0.0f) {
            lx += double(p[0]);
            ly += double(p[1]);
            lz = std::min(lz, p[2]);
            ++ln;
          } else {
            rx += double(p[0]);
            ry += double(p[1]);
            rz = std::min(rz, p[2]);
            ++rn;
          }
        }
        if (ln > 0) raw.push_back({float(lx / double(ln)), float(ly / double(ln)), lz - 0.04f});
        if (rn > 0) raw.push_back({float(rx / double(rn)), float(ry / double(rn)), rz - 0.04f});
      } else {
        raw.push_back({float(sum_x / double(rim_n)), float(sum_y / double(rim_n)), min_z - 0.04f});
      }
    }

    // Merge co-located exhaust subsections (e.g. inner/outer chrome rings of the same tailpipe).
    struct Cluster {
      double sx = 0.0, sy = 0.0;
      float min_z = 1e9f;
      int n = 0;
    };
    std::vector<Cluster> clusters;
    for (const auto& p : raw) {
      bool merged = false;
      for (auto& c : clusters) {
        const float cx = float(c.sx / double(c.n));
        const float cy = float(c.sy / double(c.n));
        const float dx = p[0] - cx, dy = p[1] - cy, dz = p[2] - c.min_z;
        if (dx * dx + dy * dy + dz * dz < 0.24f * 0.24f) {
          c.sx += double(p[0]);
          c.sy += double(p[1]);
          c.min_z = std::min(c.min_z, p[2]);
          ++c.n;
          merged = true;
          break;
        }
      }
      if (!merged) clusters.push_back({double(p[0]), double(p[1]), p[2], 1});
    }

    std::vector<std::array<float, 3>> out;
    out.reserve(clusters.size());
    for (const auto& c : clusters) {
      out.push_back({float(c.sx / double(c.n)), float(c.sy / double(c.n)), c.min_z});
    }
    std::sort(out.begin(), out.end(),
              [](const std::array<float, 3>& a, const std::array<float, 3>& b) { return a[0] < b[0]; });
    if (out.size() > 4) out.resize(4);
    return out;
  }
  bool has_cockpit_mesh() const { return cockpit_rigs_[0].valid; }
  // Batched/instanced soft particle atlas & skidmark pipelines (SPEC-VFX-AND-PARTICLES).
  bool init_vfx(const world::ParticleLibrary* lib, std::string& error);
  void set_particles(const std::vector<world::ParticleSprite>& alpha,
                     const std::vector<world::ParticleSprite>& additive,
                     const std::vector<std::string>& used_textures, float near_plane, bool cockpit_camera);
  void set_skidmarks(const std::vector<world::SkidVertex>& asphalt_tris,
                     const std::vector<world::SkidVertex>& rut_tris);
  // Dynamic lit 3D smashable debris mesh (game frame, rendered via world_opaque_).
  void set_debris_mesh(const std::vector<world::DebrisVertex>& tris);
  // Optional terrain height sampler for grounding dynamic Spectators.zip festival crowds.
  void set_world_ground(const drive::Ground* ground) { world_ground_ = ground; }
  // Textured, smoothly shaded 3D Airborne Showcase aircraft mesh (P0-6).
  struct ShowcaseVertex {
    std::array<float, 3> pos{};
    std::array<float, 3> normal{0, 1, 0};
    std::array<float, 2> uv{0, 0};
    std::array<uint8_t, 4> rgba{255, 255, 255, 255};
  };
  bool load_showcase_texture(const std::filesystem::path& game_root, int aircraft_kind, std::string& error);
  void set_showcase_mesh(const std::vector<ShowcaseVertex>& tris);
  bool has_showcase_texture() const { return showcase_tex_set_ != VK_NULL_HANDLE; }
  // Immediately hides intact static world prop draws overlapping a smashed prop's
  // sphere (game frame) without waiting for the next world regather.
  // `persistent = true` protects permanent hides (e.g. collected discount signs)
  // from `unhide_world_props_near()` and active-sphere cap pruning.
  size_t hide_world_props_near(const std::array<float, 3>& game_pos, float radius_m, bool persistent = false);
  // Restores previously hidden static world prop draws overlapping `game_pos`
  // (used when Rewind rolls back a smashed prop).
  size_t unhide_world_props_near(const std::array<float, 3>& game_pos, float radius_m);
  // True when a world draw's material flags & AABB extents match a smashable
  // prop (compact props <= 12 m or low-profile roadside fence batches <= 64 m).
  static bool is_smashable_prop_draw(uint32_t flags, uint8_t decal, float ex, float ey, float ez);

  // Other cars drawn with this one's camera and light (traffic): each is a
  // Showroom loaded and uploaded as a guest (set_guest before upload: no
  // scene of its own), drawn wherever an instance names it. An instance's
  // matrix takes the guest's model frame to this car's. A guest must be
  // taken out of the instances before it is destroyed.
  void set_guest() { guest_ = true; }
  void set_shared_dynamic_env(VkImageView cube_view, VkSampler sampler) {
    shared_dyn_env_view_ = cube_view;
    shared_dyn_env_sampler_ = sampler;
  }
  VkImageView dynamic_env_view() const { return dyn_env_ready_ ? dyn_env_.view : VK_NULL_HANDLE; }
  VkSampler dynamic_env_sampler() const { return dyn_env_ready_ ? sampler_ : VK_NULL_HANDLE; }
  void skip_wait_idle_on_destroy() { skip_wait_idle_on_destroy_ = true; }
  void retire_deferred(std::function<void()> fn) { retire(std::move(fn)); }
  struct Instance {
    Showroom* car = nullptr;
    float model[16];
    drive::LampShaderVectors lamps{};
    bool has_lamps = false;
    float fade_alpha = 1.0f;  // smooth traffic spawn/despawn fade [0..1]
    bool has_paint = false;   // per-instance linear body paint colour override
    float paint_rgb[3] = {1.0f, 1.0f, 1.0f};
  };
  void set_instances(std::vector<Instance> list);
  void add_instances(const std::vector<Instance>& more);
  void bind_dynamic_env(VkImageView cube_view, VkSampler sampler);
  size_t instance_count() const { return instances_.size(); }
  size_t world_texture_count() const;
  struct WorldTextureStats {
    size_t resident_count = 0;
    size_t retired_pending = 0;
    size_t descriptor_sets_allocated = 0;
    size_t descriptor_sets_free = 0;
    size_t descriptor_pools = 0;
    uint64_t resident_bytes = 0;
    uint64_t uploads = 0;
    uint64_t evictions = 0;
    uint64_t reuploads = 0;
  };
  void set_world_texture_budget(const WorldTextureBudget& budget);
  WorldTextureBudget world_texture_budget() const;
  void set_texture_cap(modern::TextureCap cap) { texture_cap_ = cap; }
  modern::TextureCap texture_cap() const { return texture_cap_; }
  // Main thread only: reads main-thread-owned retired/pool state alongside
  // world_textures_mutex_.
  WorldTextureStats world_texture_stats() const;
  bool has_world_texture(uint32_t record) const;
  bool ensure_world_texture(const std::shared_ptr<const WorldTextureData>& tex, std::string& error);
  bool reserve_world_texture(uint32_t record);
  void pin_prepare_records(const std::vector<uint32_t>& records);
  // Live texture reload (mods, texture_mods.h): uploads `texture` for a
  // resident world texture record and points every set that samples it at
  // the new image (after the GPU is idle), then frees the old one. False
  // (no change) when the record is not resident.
  bool replace_world_texture(uint32_t record, const cars::bake::Texture& texture, bool srgb, std::string& error);
  void release_prepare_pins(const std::vector<uint32_t>& records);
  // The world draws the last frame's shadow pass drew (0 without shadows).
  size_t shadow_draw_count() const { return shadow_drawn_; }
  // The traffic cars it drew.
  size_t shadow_car_count() const { return shadow_cars_; }

  // Renders the car-coverage mask (every car draw, no depth test, at the
  // colour pass's MSAA) at the current frame: 255 where any sample of the
  // pixel is covered, or with `fraction` the resolved share of covered
  // samples (0-255, 255 fully covered).
  bool render_mask(uint32_t width, uint32_t height, std::vector<uint8_t>& mask, std::string& error,
                   bool fraction = false);

  const cars::bake::CarInfo& car() const { return car_.car; }
  const ShowroomOptions& options() const { return options_; }
  const cars::ShaderSettingsSet& shader_settings() const { return shader_settings_; }
  const std::vector<cars::bake::LiveryPlane>& livery_planes() const { return car_.livery; }
  const std::array<float, 3>& bounds_min() const { return bounds_min_; }
  const std::array<float, 3>& bounds_max() const { return bounds_max_; }
  size_t draw_count() const { return draws_.size(); }
  size_t triangle_count() const { return indices_.size() / 3; }
  size_t vertex_count() const { return vertices_.size(); }
  size_t texture_count() const { return gpu_textures_.size(); }
  const std::vector<std::string>& notes() const { return notes_; }
  const std::vector<std::string>& drawn_parts() const { return drawn_parts_; }
  const std::vector<std::string>& skipped_parts() const { return skipped_parts_; }

 private:
  struct TextureRef {
    const cars::bake::Texture* texture = nullptr;
    std::string key;
  };
  struct Draw {
    uint32_t first_index = 0, index_count = 0;
    int32_t vertex_offset = 0;
    ShowroomPush push{};
    std::array<uint32_t, 5> textures{};  // slot 0, slot 1, livery, normal_map, damage_map (UINT32_MAX = fallback)
    uint32_t descriptor = 0;
    bool glass = false;
    bool is_cover = false;    // two-pass multiply+additive detail_glass_* lamp cover
    bool is_blur_rim = false; // retail blur_rim / blur_lip / chrome_blur_* spinning disc
    bool is_static_spoke = false;  // static 3D rim spokes cross-faded out when blur_rim is present
    bool detached = false;    // hidden when detached by heavy zone impact
    bool uses_atlas = false;  // originally sampled "@atlas" (for live factory livery swatch swap)
    bool atlas_lod0 = true;
    bool brake_lamp = false;  // legacy brake lamp flag
    drive::LampChannel lamp_channel = drive::LampChannel::kNone;
    bool stripe_able = false;  // an exterior paint draw a striped colour row tints (FORMAT-CARS, Paint)
    std::string part_name;     // lower-case part name (for per-region painting: hood, wing, mirror)
    std::array<float, 3> centre{};  // car space, for sorting glass
    std::array<float, 3> part_min{};
    std::array<float, 3> part_max{};
    std::array<uint32_t, 4> blur_descriptors{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    int wheel = -1;      // the corner a wheel, rotor or calliper draw follows
    bool spins = false;  // rims, tyres and rotors roll; callipers do not
    int8_t cockpit_role = -1;  // -1 none, 0 steering_wheel, 1 speed, 2 tach, 3 fuel, 4 boost, 5 power
    int8_t anim_part = 0;      // 0 none, 1 active_spoiler, 2 popup_lamp, 3 front_bumper, 4 rear_bumper, 5 mirror_l, 6 mirror_r, 7 engine_vent, 8 wing_link, 9 popup_up, 10 popup_dn, 11 hood, 12 fixed_wing
    float rest_model[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float local[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // about the hub, body frame
  };
  using GpuTexture = Renderer::Image;

  // The shown row's stock parts and livery flag (null: none baked).
  const cars::bake::StockRow* stock_ = nullptr;
  void add_model(const cars::bake::Model& model, const std::vector<std::string>& parts,
                 const float transform[16], const cars::bake::Bake& pack, int axle);
  uint32_t texture_index(const std::string& slot, const cars::bake::Bake& pack, bool lod0, bool optional = false);
  bool upload_texture(const cars::bake::Texture* texture, bool srgb, GpuTexture& out, std::string& error);
  struct TextureUploadItem {
    const cars::bake::Texture* texture = nullptr;
    bool srgb = true;
    GpuTexture* dst = nullptr;
    bool fallback_white = false;
  };
  bool upload_textures_batch(const std::vector<TextureUploadItem>& items, std::string& error);
  bool create_texture_image(const cars::bake::Texture& texture, bool srgb, uint32_t levels, GpuTexture& out,
                            std::string& error, uint32_t skip = 0);
  // Makes the image, view and set of a world texture staged in `texels`
  // and records its copy into `cmd` (the scenery's fenced upload).
  // `made`: its image when the worker made it (taken), else null.
  bool begin_world_texture(const WorldTextureData& t, VkBuffer texels, VkDeviceSize offset, uint32_t levels,
                           uint32_t skip, VkCommandBuffer cmd, Renderer::Image* made, std::string& error);
  // Destroying Vulkan objects off this thread (freeing a scenery's
  // buffers takes milliseconds): `reap` runs `f` on the reaper thread now;
  // `retire` once the frame in flight is done with what `f` frees
  // (release_retired, from poll_world, hands those over).
  void reap(std::function<void()> f);
  void retire(std::function<void()> f);
  void release_retired();
  void stop_reaper();
  bool upload_environment(std::string& error);
  bool create_pipelines(std::string& error);
  void record_draws(VkCommandBuffer cmd, VkPipeline opaque, VkPipeline glass, int pass_mode = 0);
  void record_z_prepass(VkCommandBuffer cmd);
  void record_all_instances(VkCommandBuffer cmd, bool depth_equal, int pass_mode = 0);
  void record_world(VkCommandBuffer cmd);
  void record_world_opaque(VkCommandBuffer cmd);
  void record_world_blended(VkCommandBuffer cmd);
  // The eye in a scenery's frame, and whether a cell of LOD00 copies lies
  // wholly past the LOD band (showroom.cpp).
  std::array<float, 3> eye_in_scenery(const float transform[16]) const;
  bool near_lod_beyond(const WorldGeometry::Draw& d, const std::array<float, 3>& eye) const;
  // One scenery's textured draws; swap: world.frag's dither (2 none, t in
  // 0-1 the incoming scenery, 3 + t the outgoing one). pass_mode: 0 all, 1 opaque/cutout, 2 blended.
  void record_world_textured(VkCommandBuffer cmd, const std::vector<WorldGeometry::Draw>& draws,
                             const float transform[16], float swap, bool outgoing, int pass_mode = 0);
  void drop_fading_world(bool now);
  // Uploads the textures not in the cache and gives each a material set.
  bool upload_world_textures(const std::vector<std::shared_ptr<const WorldTextureData>>& textures,
                             std::string& error);
  VkDescriptorSet world_set(uint32_t record) const;
  // The set binding eight uploaded world textures (albedo, layer_b, splat,
  // light_map, normal_map, ao_map, layer_c, spec_map; kNoWorldTexture uses fallbacks).
  VkDescriptorSet world_layer_set(const std::array<uint32_t, 8>& records);
  // Persistently mapped kFramesInFlight-sliced uniform buffer ring (zero vkCmdUpdateBuffer, zero barriers).
  struct MappedRing {
    Renderer::Buffer buf;
    void* mapped = nullptr;
    VkDeviceSize stride = 0;
    uint32_t slots = 0;
    bool ready() const { return buf.buffer != VK_NULL_HANDLE && mapped != nullptr && stride > 0; }
    VkDeviceSize offset(uint32_t idx) const { return VkDeviceSize(idx) * stride; }
    void* ptr(uint32_t idx) const { return static_cast<uint8_t*>(mapped) + offset(idx); }
  };
  bool create_ubo_ring(MappedRing& ring, VkDeviceSize elem_size, uint32_t slots, std::string& error);
  void destroy_ubo_ring(MappedRing& ring);
  void write_frame_ubo(MappedRing& ring, uint32_t slot, const ShowroomFrame& f) const;
  void write_frame_set(VkDescriptorSet dst, uint32_t slot, VkBuffer ubo_buf, VkDeviceSize ubo_off,
                       VkImageView cube_view, VkSampler cube_sampler, VkImageView cookie_view,
                       VkImageView mirror_view) const;
  void update_frame(VkCommandBuffer cmd);
  // The sun shadow (ours): place_shadow fits the map's box around the car
  // and fills frame_'s shadow fields and shadow_frame_ (the sun's view);
  // record_shadow draws the pass (host); record_shadow_car the car's
  // opaque draws, each model matrix first taken by `m` when given.
  bool shadows_on() const;
  void place_shadow();
  void record_shadow(VkCommandBuffer cmd);
  void record_shadow_car(VkCommandBuffer cmd, const float* m, VkDescriptorSet cascade_set = VK_NULL_HANDLE);
  // The opaque and glass draws once more, each model matrix first taken by
  // `m` (a guest's instance; glass unsorted).
  void record_instance(VkCommandBuffer cmd, const float m[16], const drive::LampShaderVectors* lamps = nullptr);
  void record_instance_offscreen(VkCommandBuffer cmd, VkPipeline car_opaque, VkDescriptorSet view_frame_set,
                                 const float m[16], const drive::LampShaderVectors* lamps = nullptr,
                                 VkPipeline car_glass = VK_NULL_HANDLE);
  void record_player_mirror_rear(VkCommandBuffer cmd, VkPipeline car_opaque, VkDescriptorSet view_frame_set,
                                 float min_rear_z);
  void sync_paint_shader_uniforms();
  void apply_damage_part_transforms(float time_sec);
  bool load_driver_assets();

  bool guest_ = false;
  bool skip_wait_idle_on_destroy_ = false;
  VkImageView shared_dyn_env_view_ = VK_NULL_HANDLE;
  VkSampler shared_dyn_env_sampler_ = VK_NULL_HANDLE;
  modern::TextureCap texture_cap_ = modern::TextureCap::kFull;
  std::vector<Instance> instances_;
  Renderer& r_;
  cars::bake::Bake car_, wheel_, tyre_, tyre_textures_, shared_;
  std::filesystem::path shared_bake_dir_;
  cars::ShaderSettingsSet shader_settings_;
  cars::CarShaderOverrides car_shader_overrides_;
  cars::ShaderFinishParams active_paint_params_;
  bool car_has_crush_weights_ = false;
  bool has_blur_rim_mesh_ = false;
  ShowroomOptions options_;
  ShowroomFrame frame_{};
  float stripe_[4] = {1, 1, 1, 1};
  float body_to_model_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  int pending_wheel_ = -1;  // what add_model records on the draws it makes
  bool pending_spins_ = false;
  float pending_local_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::vector<cars::bake::Vertex> vertices_;
  std::vector<uint32_t> indices_;
  std::vector<Draw> draws_;
  std::vector<Draw> far_draws_;  // optional coarser LOD2/3 index slices (shared vertex pool) for distant AI/shadows
  std::vector<TextureRef> textures_;  // 0 white, 1 black, then by first use
  std::map<std::string, uint32_t> texture_by_key_;
  std::map<std::array<uint32_t, 5>, uint32_t> descriptor_by_textures_;
  std::array<float, 3> bounds_min_{}, bounds_max_{};
  bool have_bounds_ = false;
  std::vector<std::string> notes_, drawn_parts_, skipped_parts_;

  // GPU.
  std::vector<GpuTexture> gpu_textures_;
  GpuTexture environment_;
  GpuTexture shared_dirt_pattern_, shared_dirt_mask_, shared_glass_crack_;
  GpuTexture car_dirt_mask_;
  bool has_car_dirt_mask_ = false;
  VkImageView bound_dyn_env_view_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  Renderer::Buffer vertex_buffer_, index_buffer_;
  MappedRing frame_buffer_;
  VkDescriptorSetLayout frame_layout_ = VK_NULL_HANDLE, material_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout world_material_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout world_pipeline_layout_ = VK_NULL_HANDLE;
  GpuTexture flat_normal_tex_;  // 1x1 (128, 128, 255, 255) fallback normal map
  VkPipeline opaque_ = VK_NULL_HANDLE, opaque_after_z_ = VK_NULL_HANDLE, opaque_cutout_ = VK_NULL_HANDLE,
             opaque_cutout_after_z_ = VK_NULL_HANDLE, opaque_fade_ = VK_NULL_HANDLE,
             car_z_prepass_ = VK_NULL_HANDLE, glass_ = VK_NULL_HANDLE, glass_mul_ = VK_NULL_HANDLE,
             glass_add_ = VK_NULL_HANDLE, mask_ = VK_NULL_HANDLE;
  VkPipelineLayout contact_shadow_pipe_layout_ = VK_NULL_HANDLE;
  VkPipeline contact_shadow_pipe_ = VK_NULL_HANDLE;
  void record_contact_shadows(VkCommandBuffer cmd);
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorPool material_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet frame_set_ = VK_NULL_HANDLE;
  // The sun shadow pass: 2-cascade (Near/Far) or 3-cascade (Near/Mid/Far) D32_SFLOAT stabilized CSM atlas.
  MappedRing shadow_frame_buffer_;
  MappedRing shadow_frame_mid_buffer_;
  MappedRing shadow_frame_far_buffer_;
  VkDescriptorSet shadow_sets_[Renderer::kFramesInFlight] = {};
  VkDescriptorSet shadow_sets_mid_[Renderer::kFramesInFlight] = {};
  VkDescriptorSet shadow_sets_far_[Renderer::kFramesInFlight] = {};
  VkDescriptorSet shadow_set_ = VK_NULL_HANDLE;
  VkDescriptorSet shadow_set_mid_ = VK_NULL_HANDLE;
  VkDescriptorSet shadow_set_far_ = VK_NULL_HANDLE;
  ShowroomFrame shadow_frame_{};
  ShowroomFrame shadow_frame_mid_{};
  ShowroomFrame shadow_frame_far_{};
  bool shadow_atlas_valid_ = false;
  uint64_t shadow_atlas_world_gen_ = 0;
  bool shadow_update_mid_ = true;
  bool shadow_update_far_ = true;
  float shadow_cached_mid_matrix_[16] = {};
  float shadow_cached_far_matrix_[16] = {};
  float shadow_cached_mid_bias_ = 0.0f;
  float shadow_cached_far_bias_ = 0.0f;
  float rotor_glow_ = 0.0f;
  float rotor_glow_front_ = 0.0f;
  float rotor_glow_rear_ = 0.0f;
  WheelPose wheel_poses_[4]{};
  bool have_wheel_poses_ = false;
  VkPipeline shadow_world_ = VK_NULL_HANDLE, shadow_cutout_ = VK_NULL_HANDLE, shadow_car_ = VK_NULL_HANDLE;
  size_t shadow_drawn_ = 0, shadow_cars_ = 0;
  float shadow_direction_[3] = {0, 1, 0};
  bool shadow_direction_set_ = false;
  std::vector<VkDescriptorSet> material_sets_;
  // Scenery.
  Renderer::Buffer world_vertices_, world_indices_;
  // Dynamic world-space overlay (3D driving/braking line ribbon).
  static constexpr size_t kMaxOverlayVertices = 1365;
  VkDeviceSize overlay_vb_offset_ = 0;
  std::vector<WorldVertex> overlay_cpu_;
  uint32_t overlay_count_ = 0;
  bool overlay_dirty_ = false;
  // The scenery being swapped out, drawn dithered under the new one until
  // the swap fade ends (ours).
  struct FadingWorld {
    Renderer::Buffer vertices, indices;
    std::vector<WorldGeometry::Draw> draws;
    bool arena = false;          // its draws are resident (their references are held until it goes)
    double origin[2] = {0, 0};
    float near_prop_reach = 0.0f;
  };
  float world_fade_[4] = {0, 0, 0, 0};  // set_world_fade's, kept over set_frame
  float world_near_prop_reach_ = kPropReach;
  float effective_near_prop_reach() const {
    const float base = std::max(kPropReach, world_near_prop_reach_);
    return fading_on_ ? std::max(base, fading_.near_prop_reach) : base;
  }
  bool fading_on_ = false;
  FadingWorld fading_;
  std::chrono::steady_clock::time_point fade_start_{};
  double swap_fade_seconds_ = 0;
  std::vector<WorldGeometry::Draw> world_draws_;
  std::vector<StagedWorld::TreeRaw> world_trees_;
  float prev_vp_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  bool have_prev_vp_ = false;
  bool hiz_enabled_ = true;
  // The GPU-driven world: arenas, bindless textures, culling (world_gpu.h).
  std::unique_ptr<WorldGpu> gpu_world_;
  WorldArenaLru arena_lru_;
  bool arena_use_rt_resident_ = false;
  bool arena_on() const;  // the arenas hold the scenery (else the monolithic buffers)
  uint64_t world_generation_ = 0;
  void sync_motion_scene() {
    if (guest_) return;
    Renderer::MotionSceneState ms{};
    ms.has_world_transform = world_lit_ || !world_draws_.empty();
    std::memcpy(ms.world_transform, world_transform_, sizeof(ms.world_transform));
    ms.world_origin[0] = world_origin_[0];
    ms.world_origin[1] = world_origin_[1];
    ms.has_player_car = draw_car_ && have_bounds_;
    if (ms.has_player_car) {
      // Pad car OBB tightly around wheels/mirrors while lifting bottom +1.5 cm above asphalt
      // so the road surface underneath stays outside the player-car OBB and blurs cleanly.
      const float lift_bot = (bounds_max_[1] - bounds_min_[1] > 0.2f) ? 0.015f : 0.0f;
      ms.player_bmin[0] = bounds_min_[0] - 0.06f;
      ms.player_bmin[1] = bounds_min_[1] + lift_bot;
      ms.player_bmin[2] = bounds_min_[2] - 0.05f;
      ms.player_bmax[0] = bounds_max_[0] + 0.06f;
      ms.player_bmax[1] = bounds_max_[1] + 0.06f;
      ms.player_bmax[2] = bounds_max_[2] + 0.05f;
    }
    if (!instances_.empty()) {
      struct Cand {
        float d2 = 0.0f;
        size_t idx = 0;
      };
      std::vector<Cand> cands;
      cands.reserve(instances_.size());
      for (size_t i = 0; i < instances_.size(); ++i) {
        const auto& in = instances_[i];
        if (!in.car || !in.car->have_bounds_ || in.fade_alpha <= 0.05f) continue;
        const float dx = in.model[12] - frame_.eye[0];
        const float dy = in.model[13] - frame_.eye[1];
        const float dz = in.model[14] - frame_.eye[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > 160.0f * 160.0f) continue;
        cands.push_back({d2, i});
      }
      if (cands.size() > Renderer::MotionSceneState::kMaxDynamicCars) {
        std::partial_sort(cands.begin(), cands.begin() + Renderer::MotionSceneState::kMaxDynamicCars, cands.end(),
                          [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
        cands.resize(Renderer::MotionSceneState::kMaxDynamicCars);
      }
      ms.dynamic_car_count = uint32_t(cands.size());
      for (uint32_t k = 0; k < ms.dynamic_car_count; ++k) {
        const auto& in = instances_[cands[k].idx];
        auto& dst = ms.dynamic_cars[k];
        dst.model_id = reinterpret_cast<uintptr_t>(in.car);
        std::memcpy(dst.model, in.model, sizeof(dst.model));
        const auto& bmin = in.car->bounds_min_;
        const auto& bmax = in.car->bounds_max_;
        const float lift_bot = (bmax[1] - bmin[1] > 0.2f) ? 0.015f : 0.0f;
        dst.bmin[0] = bmin[0] - 0.06f;
        dst.bmin[1] = bmin[1] + lift_bot;
        dst.bmin[2] = bmin[2] - 0.05f;
        dst.bmax[0] = bmax[0] + 0.06f;
        dst.bmax[1] = bmax[1] + 0.06f;
        dst.bmax[2] = bmax[2] + 0.05f;
      }
    }
    r_.set_motion_scene(ms);
  }
  void world_bind_buffers(VkCommandBuffer cmd, bool fading);
  void world_draw_model(const WorldGeometry::Draw& d, const float transform[16], float out[16]) const;
  void world_release_draws(std::vector<WorldGeometry::Draw>& draws);
  bool build_gpu_table(std::string& error);
  void record_world_cull(VkCommandBuffer cmd);
  bool place_world_arena(StagedWorld& st, std::vector<WorldGpu::Copy>& vertex_copies,
                         std::vector<WorldGpu::Copy>& index_copies, std::string& error,
                         bool* deferred_busy = nullptr, bool allow_blocking_reset = true);
  bool arena_lookup_draw(uint64_t key, uint32_t& out_index_base) const;
  bool arena_contains_draw(uint64_t key) const;
  void arena_mark_draw_valid(uint64_t key);
  void arena_release_draw(uint64_t key, uint64_t tag);
  void install_world_prop_spans(StagedWorld& st);
  size_t world_triangles_ = 0;
  size_t gpu_table_white_ = 0, gpu_table_layer_fallback_ = 0;
  std::vector<uint32_t> active_gpu_table_records_;
  std::unordered_set<uint32_t> gpu_table_missing_textures_;
  bool gpu_table_refresh_pending_ = false;
  WorldDrawStats world_stats_;
  bool world_cull_ = true;
  float world_transform_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  double world_origin_[2] = {0, 0};
  struct PendingWorld {
    Renderer::Buffer vertices, indices;
    StagedWorld staged;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool deferred_arena_place = false;
  };
  bool submit_pending_world_upload(PendingWorld& p, bool allow_blocking_reset, std::string& error);
  std::unique_ptr<PendingWorld> pending_world_;
  void drop_pending_world();
  // The textured world.
  bool world_textured_ = false;
  bool draw_car_ = true;
  bool draw_driver_ = true;
  VkPipeline world_opaque_ = VK_NULL_HANDLE, world_opaque_after_z_ = VK_NULL_HANDLE,
             world_opaque_dither_ = VK_NULL_HANDLE, world_z_prepass_ = VK_NULL_HANDLE,
             world_coverage_ = VK_NULL_HANDLE, world_alpha_ = VK_NULL_HANDLE, world_water_ = VK_NULL_HANDLE;
  float current_world_swap_ = 2.0f;
  std::map<uint32_t, GpuTexture> world_textures_;     // by texture record
  // Held for writes to world_textures_ and for the worker's reads.
  mutable std::mutex world_textures_mutex_;
  std::thread reaper_;
  std::mutex reap_mutex_;
  std::condition_variable reap_cv_;
  std::deque<std::function<void()>> reap_queue_;
  bool reap_stop_ = false;
  std::vector<std::pair<uint64_t, std::function<void()>>> retired_;  // main thread only
  std::map<uint32_t, VkDescriptorSet> world_sets_;   // by texture record
  // Sets for multi-texture world draws: bindings 0..7.
  std::map<std::array<uint32_t, 8>, VkDescriptorSet> world_layer_sets_;
  uint64_t world_sets_gen_ = 1;
  mutable std::vector<uint32_t> world_buckets_[3];
  mutable std::vector<uint32_t> world_touched_tex_;
  VkDescriptorSet world_white_set_ = VK_NULL_HANDLE;  // untextured materials
  std::vector<VkDescriptorPool> world_pools_;
  uint32_t world_pool_left_ = 0;
  float world_sun_[4] = {0.3848f, 0.8754f, 0.2926f, 1}, world_up_[4] = {0, 1, 0, 1};
  bool world_lit_ = false;  // set_world_light was called: light the car by the world's sun and sky
  // Dynamic 24-hour Time-of-Day & real-time sky (realtimesky.zip + Stars.bin).
  bool tod_active_ = false;
  world::TodState tod_state_{};
  int tod_moon_phase_ = 2;
  float tod_real_time_ = 0.0f;
  float tod_eye_game_y_ = 150.0f;
  bool sky_ready_ = false;
  bool sky_hidden_ = false;
  bool sky_has_textures_ = false;
  struct SkyUniforms {
    float cam_fwd[4];
    float cam_right[4];
    float cam_up[4];
    float sun_dir[4];
    float sun_obj_dir[4];
    float haze_dir[4];
    float atmo_top[4];
    float atmo_bottom[4];
    float atmo_haze[4];
    float atmo_haze_p[4];
    float sun_obj_col[4];
    float far_dense[4];
    float far_thin[4];
    float far_params[4];
    float far_back_dense[4];
    float far_back_thin[4];
    float far_scroll[4];
    float close_top_col[4];
    float close_bot_col[4];
    float close_back_col[4];
    float close_dense_col[4];
    float close_thin_col[4];
    float close_intens[4];
    float close_masks[4];
    float close_layers[4];
    float close_back_dense_col[4];
    float close_back_thin_col[4];
    float close_back_bot_col[4];
    float fog_color[4];
    float fog_scatter[4];
    float fog_sky[4];
  };
  static_assert(sizeof(SkyUniforms) == 31 * 16);
  MappedRing sky_ubo_;  // kFramesInFlight * 2 slots (mode 0/2, mode 1), dynamic UBO
  Renderer::Buffer star_vertices_;
  uint32_t star_count_ = 0;
  GpuTexture sky_sun_, sky_moons_[5], sky_dither_;
  GpuTexture sky_far_f_, sky_far_b_, sky_far_mask_;
  GpuTexture sky_close_ad_f_, sky_close_ad_b_, sky_close_am_f_, sky_close_am_b_, sky_close_tb_f_, sky_close_tb_b_;
  VkDescriptorSetLayout sky_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout sky_pipe_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout stars_pipe_layout_ = VK_NULL_HANDLE;
  VkPipeline sky_pipe_base_ = VK_NULL_HANDLE, sky_pipe_layers_ = VK_NULL_HANDLE, stars_pipe_ = VK_NULL_HANDLE;
  VkDescriptorPool sky_pool_ = VK_NULL_HANDLE;
  // 6 sets (sun + 5 moon phases) with dynamic UBO offset for (frame_slot, mode).
  VkDescriptorSet sky_sets_[6] = {};
  void record_sky_before(VkCommandBuffer cmd);
  void record_sky_draw(VkCommandBuffer cmd);
  void apply_tod_to_frame();
  // Car lamps, headlight road projection & additive glow sprites (CAR_LAMPS.md).
  struct HeadlightBeamGpu {
    float proj[16];
    float origin_range[4];
    float dir_power[4];
    float color[4];
  };
  struct HeadlightUbo {
    float count_night[4];
    HeadlightBeamGpu beams[16];
  };
  static_assert(sizeof(HeadlightUbo) == 16 + 16 * 112);
  struct GlowSpriteGpu {
    float pos_radius[4];
    float dir_inner[4];
    float color_outer[4];
    float extra[4];
  };
  static_assert(sizeof(GlowSpriteGpu) == 64);
  static constexpr uint32_t kMaxGlowSprites = 1024;
  std::vector<fh1::CarbinLampPoint> lamp_points_;
  drive::LampColorTable lamp_colors_;
  drive::LampShaderVectors lamp_vectors_{};
  bool lamp_state_active_ = false;
  std::vector<world::WorldGlow> world_glows_;
  GpuTexture headlight_cookie_;
  GpuTexture headlight_cookie_full_;
  VkDescriptorSet frame_sets_dip_[Renderer::kFramesInFlight] = {};
  VkDescriptorSet frame_sets_fullbeam_[Renderer::kFramesInFlight] = {};
  MappedRing headlights_ubo_;
  Renderer::VolumetricBeam vol_beams_[8]{};
  uint32_t vol_beam_count_ = 0;
  float vol_night_factor_ = 0.0f;
  GpuTexture glow_tex_[7];
  VkDescriptorSetLayout glow_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout glow_pipe_layout_ = VK_NULL_HANDLE;
  VkPipeline glow_pipe_ = VK_NULL_HANDLE;
  VkDescriptorPool glow_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet glow_set_ = VK_NULL_HANDLE;
  VkDeviceSize glow_vb_offset_ = 0;
  uint32_t glow_count_ = 0;
  bool carlights_ready_ = false;
  void record_lights_before(VkCommandBuffer cmd);
  void record_glows_draw(VkCommandBuffer cmd);
  void update_frame_cookie_descriptor();
  // Showroom XPR2 cubemap, live 256x256 dynamic drive reflection cubemap, and 1024x256 3-band mirror pass.
  std::filesystem::path env_xpr_path_;
  static constexpr uint32_t kDynEnvSize = 256;
  static constexpr uint32_t kDynEnvMips = 9;
  static constexpr uint32_t kMaxEnvViews = 10;
  uint32_t dyn_env_size_ = kDynEnvSize;
  uint32_t dyn_env_mips_ = kDynEnvMips;
  uint32_t env_view_cursor_ = 0;
  float reflection_rate_ = 1.0f;
  bool low_memory_mode_ = false;
  static constexpr uint32_t kMirrorWidth = 1024;
  static constexpr uint32_t kMirrorHeight = 256;
  GpuTexture dyn_env_;
  VkImageView dyn_env_faces_[6] = {};
  VkImageView dyn_env_mip0_cube_ = VK_NULL_HANDLE;
  VkImageView dyn_env_mip_faces_[kDynEnvMips][6] = {};
  GpuTexture dyn_env_depth_;
  GpuTexture mirror_tex_;
  GpuTexture mirror_depth_;
  MappedRing env_frame_buffer_;
  VkDescriptorSet env_frame_sets_[Renderer::kFramesInFlight][kMaxEnvViews] = {};
  VkDescriptorSet env_frame_set_ = VK_NULL_HANDLE;
  MappedRing env_sky_ubo_;
  VkDescriptorPool env_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet env_sky_sets_[6] = {};
  VkPipeline env_world_opaque_ = VK_NULL_HANDLE;
  VkPipeline env_world_coverage_ = VK_NULL_HANDLE;
  VkPipeline env_car_opaque_ = VK_NULL_HANDLE;
  VkPipeline env_sky_pipe_base_ = VK_NULL_HANDLE;
  VkPipeline env_sky_pipe_layers_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout env_prefilter_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout env_prefilter_pipe_layout_ = VK_NULL_HANDLE;
  VkPipeline env_prefilter_pipe_ = VK_NULL_HANDLE;
  VkDescriptorSet env_prefilter_set_ = VK_NULL_HANDLE;
  bool dyn_env_ready_ = false;
  bool dyn_env_initialized_ = false;
  bool dyn_env_bound_ = false;
  bool mirror_enabled_ = false;
  bool mirror_valid_ = false;
  bool virtual_mirror_enabled_ = true;
  bool virtual_mirror_active_ = false;
  void update_mirror_mode();
  static void bind_post_virtual_mirror(Renderer& r, VkImageView view);
  void sync_fullbeam_frame_sets(VkImageView cube_view, VkSampler cube_sampler);
  bool init_dynamic_reflections(std::string& error);
  bool ensure_mirror_targets();
  void record_env_and_mirror(VkCommandBuffer cmd);
  void record_offscreen_view(VkCommandBuffer cmd, VkImageView color_view, VkImageView depth_view,
                             uint32_t w, uint32_t h, const ShowroomFrame& view_frame, bool include_cars,
                             uint32_t vx = 0, uint32_t vy = 0, uint32_t vw = 0, uint32_t vh = 0,
                             bool clear_target = true, uint32_t* draw_counter = nullptr);
  void fill_sky_uniforms(const ShowroomFrame& view_frame, SkyUniforms& u) const;
  // Batched soft particles + terrain-conforming skidmarks (SPEC-VFX-AND-PARTICLES).
  struct ParticleGpu {
    float pos_sx[4];
    float vel_sy[4];
    float uv_rect[4];
    float color[4];
    float light[4];
    float extra[4];
  };
  static_assert(sizeof(ParticleGpu) == 96);
  struct SkidGpuVertex {
    float pos[3];
    float normal[3];
    float uv[2];
    uint8_t rgba[4];
  };
  static_assert(sizeof(SkidGpuVertex) == 36);
  static constexpr uint32_t kMaxParticleGpu = 2400;
  static constexpr uint32_t kMaxSkidVerts = 24000;
  static constexpr uint32_t kParticleAtlasGrid = 8;
  static constexpr uint32_t kParticleAtlasCell = 256;
  static constexpr uint32_t kParticleAtlasSize = kParticleAtlasGrid * kParticleAtlasCell;  // 2048
  bool vfx_ready_ = false;
  GpuTexture particle_atlas_;
  GpuTexture skid_asphalt_tex_;
  GpuTexture skid_rut_tex_;
  VkSampler particle_sampler_ = VK_NULL_HANDLE;
  VkSampler skid_sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout particle_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout particle_pipe_layout_ = VK_NULL_HANDLE;
  VkPipeline particle_alpha_pipe_ = VK_NULL_HANDLE;
  VkPipeline particle_add_pipe_ = VK_NULL_HANDLE;
  VkDescriptorPool particle_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet particle_set_ = VK_NULL_HANDLE;
  VkImageView particle_bound_depth_ = VK_NULL_HANDLE;
  // A vertex/instance buffer the CPU rewrites each frame, one region per
  // frame in flight: the setters write `cpu` (the newest content) and the
  // draw copies it into its slot's region when that region is out of date.
  struct DynBuffer {
    Renderer::Buffer buf;         // kFramesInFlight regions of `cap` bytes
    void* gpu = nullptr;          // its mapping
    std::vector<uint8_t> cpu;     // `cap` bytes
    VkDeviceSize cap = 0;
    uint64_t ver = 1;             // bumped when `cpu` changes
    uint64_t slot_ver[Renderer::kFramesInFlight] = {};
    bool ready() const { return gpu != nullptr && !cpu.empty(); }
    void* data() { return cpu.data(); }
  };
  bool create_dyn_buffer(DynBuffer& d, VkDeviceSize cap, std::string& error,
                         VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  void destroy_dyn_buffer(DynBuffer& d);
  // Copies the content into this frame's region if needed; returns the
  // region's byte offset to bind.
  VkDeviceSize dyn_sync(DynBuffer& d, VkDeviceSize used_bytes);
  static constexpr uint32_t kMaxCarInstances = 256;
  struct CarInstanceGpu {
    float model[16];
    float stripe[4];
    float lamp_rgb[30][4];
    float pad[4];
  };
  static_assert(sizeof(CarInstanceGpu) == 576);
  DynBuffer car_inst_buf_;
  DynBuffer glow_buf_;
  DynBuffer overlay_buf_;
  Renderer::Buffer empty_inst_ssbo_;
  DynBuffer particle_buf_;
  uint32_t particle_alpha_count_ = 0;
  uint32_t particle_add_count_ = 0;
  float particle_near_plane_ = 0.25f;
  bool particle_cockpit_ = false;
  std::unordered_map<std::string, uint16_t> atlas_tex_slot_;  // lower-case .xds -> atlas cell [0..63]
  VkDescriptorSetLayout skid_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout skid_pipe_layout_ = VK_NULL_HANDLE;
  VkPipeline skid_pipe_ = VK_NULL_HANDLE;
  VkDescriptorPool skid_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet skid_sets_[2] = {};
  DynBuffer skid_asphalt_buf_;
  DynBuffer skid_rut_buf_;
  uint32_t skid_asphalt_count_ = 0;
  uint32_t skid_rut_count_ = 0;
  static constexpr uint32_t kMaxDebrisVerts = 131072;  // smashable .rmb.bin debris + animated world props/grass
  struct DebrisBatch {
    uint32_t first_vertex = 0;
    uint32_t vertex_count = 0;
    uint32_t texture_record = kNoWorldTexture;
    uint8_t cutout = 0;
  };
  DynBuffer debris_buf_;
  uint32_t debris_count_ = 0;
  std::vector<DebrisBatch> debris_batches_;
  static constexpr uint32_t kMaxSpectatorVerts = 131072;  // authentic Spectators.zip 3D festival crowds
  world::AnimatedWorldSystem spectator_anim_;
  const drive::Ground* world_ground_ = nullptr;
  std::vector<world::SpectatorVertex> spectator_cpu_;
  GpuTexture spectator_atlas_;
  VkDescriptorSet spectator_set_ = VK_NULL_HANDLE;
  DynBuffer spectator_buf_;
  uint32_t spectator_count_ = 0;
  static constexpr uint32_t kMaxShowcaseVerts = 16384;  // textured 3D showcase aircraft/balloon/chopper
  DynBuffer showcase_buf_;
  uint32_t showcase_count_ = 0;
  GpuTexture showcase_tex_;
  VkDescriptorSet showcase_tex_set_ = VK_NULL_HANDLE;
  int showcase_tex_kind_ = -1;
  static constexpr uint32_t kMaxDriverVerts = 32768;   // animated 3D seated driver + steering hands (Driver.zip .skinbin or procedural)
  DynBuffer driver_buf_;
  uint32_t driver_count_ = 0;
  uint32_t driver_hands_count_ = 0;  // leading vertices for forearms + gloved hands (visible in cockpit view)
  float driver_wheel_rad_ = 0.0f;
  float driver_shift_blend_ = 0.0f;
  float driver_shift_timer_ = 0.0f;
  int driver_last_gear_ = 0;
  float driver_handbrake_blend_ = 0.0f;
  std::array<float, 3> driver_hands_offset_{0.0f, 0.0f, 0.0f};
  float driver_hands_scale_ = 1.0f;
  world::SpectatorMeshLod driver_body_mesh_;
  world::SpectatorMeshLod driver_hands_mesh_;
  world::SpectatorMeshLod driver_wristband_mesh_;
  GpuTexture driver_tex_;
  VkDescriptorSet driver_tex_set_ = VK_NULL_HANDLE;
  bool driver_assets_tried_ = false;
  bool driver_assets_loaded_ = false;
  float aero_deploy_smooth_ = 0.0f;
  float popup_open_smooth_ = 0.0f;
  bool cockpit_camera_active_ = false;
  float wind_speed_ = 1.0f;
  float wind_amplitude_ = 0.15f;
  void update_driver_mesh(float wheel_rad);
  void record_driver_draw(VkCommandBuffer cmd);
  void record_skidmarks_draw(VkCommandBuffer cmd);
  void record_debris_draw(VkCommandBuffer cmd);
  void record_showcase_draw(VkCommandBuffer cmd);
  void record_particles_draw(VkCommandBuffer cmd);
  // Living cockpit animation rigs (steering wheel + 5 analog gauge needles).
  struct CockpitRig {
    bool valid = false;
    std::array<float, 3> pivot{0, 0, 0};
    std::array<float, 3> axis{0, 0, 1};
    float ang_mid = 0.0f;
    float ang_max = 0.0f;
    float radius = 0.165f;  // actual steering wheel grip radius (m)
  };
  CockpitRig cockpit_rigs_[6]{};  // 0 steering_wheel, 1 speed, 2 tach, 3 fuel, 4 boost, 5 power
  drive::CockpitGaugeCalibration gauge_calibration_;
  std::array<std::array<float, 3>, 3> mirror_origins_{
      std::array<float, 3>{-0.95f, 0.95f, 0.25f},
      std::array<float, 3>{0.0f, 1.18f, 0.25f},
      std::array<float, 3>{0.95f, 0.95f, 0.25f},
  };
  std::array<bool, 3> mirror_origin_valid_{false, false, false};
  std::array<float, 3> cockpit_wheel_pos_offset_{0.0f, 0.0f, 0.0f};
  float cockpit_wheel_rot_offset_rad_ = 0.0f;
  std::array<float, 3> car_attribs_wheel_pos_{0.0f, 0.0f, 0.0f};
  float car_attribs_wheel_rot_rad_ = 0.0f;
  float cockpit_mfd_[4] = {0.0f, 0.14f, 0.85f, 0.0f};
  float cockpit_mfd_ext_[4] = {0.0f, 0.0f, 1.0f, 0.0f};
  bool allocate_world_set(VkDescriptorSet& out, std::string& error);
  bool create_world_single_set(VkImageView view, VkDescriptorSet& out, std::string& error);
  void evict_world_textures();
  WorldTextureLru world_lru_;
  bool world_budget_explicit_ = false;
  uint64_t world_frame_ = 0;
  std::unordered_map<uint32_t, size_t> prepare_pins_;
  std::vector<std::pair<uint64_t, std::vector<VkDescriptorSet>>> retired_sets_;  // main thread only
  std::vector<VkDescriptorSet> free_world_sets_;
  size_t world_sets_allocated_ = 0;
  // Paint Shop finish/two-tone/tint, custom 1024^2 livery, and cosmetic damage/dirt state.
  float paint_two_tone_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float paint_finish_[4] = {0.0f, 0.25f, 0.0f, 0.0f};
  float damage_dirt_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float damage_zones_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float damage_corners_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float paint_shader_a_[4] = {1.55f, 0.90f, 1.0f, 0.0f};
  float paint_shader_b_[4] = {3.0f, 12.0f, 2.0f, 4.0f};
  float paint_shader_c_[4] = {0.0f, 0.0f, 1.0f, 5.0f};
  float paint_shader_d_[4] = {0.0f, 0.0f, 75.0f, 3.0f};
  float region_two_tone_[3][4] = {{0, 0, 0, 4.0f}, {0, 0, 0, 4.0f}, {0, 0, 0, 4.0f}};
  float region_finish_[3][4] = {{0, 0.25f, 0, 0}, {0, 0.25f, 0, 0}, {0, 0.25f, 0, 0}};
  float rim_shader_[4] = {1.55f, 0.90f, 1.0f, 0.0f};
  float rim_tint_dirt_[4] = {1.0f, 1.0f, 1.0f, 0.0f};
  bool dmg_front_bumper_detached_ = false;
  bool dmg_rear_bumper_detached_ = false;
  bool dmg_left_mirror_detached_ = false;
  bool dmg_right_mirror_detached_ = false;
  bool dmg_wing_detached_ = false;
  bool detached_part_flying_[5] = {false, false, false, false, false};
  float detached_part_xform_[5][16]{};
  float dmg_hood_ajar_ = 0.0f;
  bool custom_livery_active_ = false;
  GpuTexture custom_livery_img_;
  // Benchmark telemetry counters.
  double pipeline_compile_ms_ = 0.0;
  uint32_t env_draws_ = 0;
  uint32_t mirror_draws_ = 0;
  uint32_t car_draws_ = 0;
  std::atomic<uint64_t> world_staging_bytes_{0};
  // Hardware Ray Tracing / Path Tracing scene manager & pipeline (eagerly pre-compiled at startup when RT != kOff).
  std::unique_ptr<RtScene> rt_scene_;
  std::unique_ptr<RtPipeline> rt_pipeline_;
  bool rt_init_failed_ = false;
  // Ring-buffered per-frame budgeted async texture uploader (zero submit_once / wait_idle stalls).
  struct UploadRingSlot {
    Renderer::Buffer staging;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
  };
  struct UploadRingGuard {
    Renderer* r = nullptr;
    UploadRingSlot slots[Renderer::kFramesInFlight]{};
    ~UploadRingGuard();
  };
  UploadRingGuard upload_ring_;
  double upload_budget_ms_ = 2.5;
  mutable std::mutex deferred_uploads_mutex_;
  std::deque<std::shared_ptr<const WorldTextureData>> deferred_world_textures_;
  void pump_deferred_uploads(VkCommandBuffer cmd);
  friend class RtScene;
  friend class RtPipeline;
};

// Internal virtual rearview mirror hook between Showroom (showroom_env.cpp) and Renderer (renderer_post.cpp).
namespace detail {
void register_virtual_mirror(const Renderer* r, VkImageView view);
void unregister_virtual_mirror(const Renderer* r);
void set_virtual_mirror_active(const Renderer* r, bool active);
}  // namespace detail

// Standalone AOT shader/pipeline pre-compilation entry point (`carview --precompile-shaders`).
int run_precompile_shaders(int argc, char** argv);

}  // namespace carview

#endif  // CARVIEW_ARENA_LRU_ONLY
