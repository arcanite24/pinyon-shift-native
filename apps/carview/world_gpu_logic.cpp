#include "world_gpu_logic.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace carview::gpuworld {

void ArenaAllocator::reset(uint32_t capacity) {
  capacity_ = capacity;
  used_ = 0;
  free_.clear();
  if (capacity) free_[0] = capacity;
}

uint32_t ArenaAllocator::allocate(uint32_t units) {
  if (units == 0) return 0;
  for (auto it = free_.begin(); it != free_.end(); ++it) {
    if (it->second < units) continue;
    const uint32_t offset = it->first;
    const uint32_t left = it->second - units;
    free_.erase(it);
    if (left) free_[offset + units] = left;
    used_ += units;
    return offset;
  }
  return kNoSpace;
}

void ArenaAllocator::free(uint32_t offset, uint32_t units) {
  if (units == 0) return;
  used_ -= std::min(used_, units);
  auto next = free_.lower_bound(offset);
  if (next != free_.end() && offset + units == next->first) {  // merge with the range after
    units += next->second;
    next = free_.erase(next);
  }
  if (next != free_.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == offset) {  // and the one before
      prev->second += units;
      return;
    }
  }
  free_[offset] = units;
}

uint32_t ArenaAllocator::largest_free() const {
  uint32_t m = 0;
  for (const auto& [o, n] : free_) m = std::max(m, n);
  return m;
}

void ResidentSet::reset(uint32_t vertex_capacity, uint32_t index_capacity) {
  map_.clear();
  vertices_.reset(vertex_capacity);
  indices_.reset(index_capacity);
  evictions_ = 0;
}

ResidentDraw* ResidentSet::acquire(uint64_t key, uint64_t generation) {
  auto it = map_.find(key);
  if (it == map_.end()) return nullptr;
  if (!it->second.valid && it->second.refs == 0) {  // a placement whose copy never ran
    vertices_.free(it->second.vertex_base, it->second.vertex_count);
    indices_.free(it->second.index_base, it->second.index_count);
    map_.erase(it);
    return nullptr;
  }
  ++it->second.refs;
  it->second.last_used = generation;
  return &it->second;
}

void ResidentSet::mark_valid(uint64_t key) {
  auto it = map_.find(key);
  if (it != map_.end()) it->second.valid = true;
}

const ResidentDraw* ResidentSet::find(uint64_t key) const {
  auto it = map_.find(key);
  return it == map_.end() ? nullptr : &it->second;
}

bool ResidentSet::evict_oldest(uint64_t frames_done, std::vector<uint64_t>* evicted) {
  auto best = map_.end();
  for (auto it = map_.begin(); it != map_.end(); ++it) {
    if (it->second.refs != 0 || it->second.free_tag > frames_done) continue;
    if (best == map_.end() || it->second.last_used < best->second.last_used) best = it;
  }
  if (best == map_.end()) return false;
  vertices_.free(best->second.vertex_base, best->second.vertex_count);
  indices_.free(best->second.index_base, best->second.index_count);
  if (evicted) evicted->push_back(best->first);
  map_.erase(best);
  ++evictions_;
  return true;
}

ResidentDraw* ResidentSet::place(uint64_t key, uint32_t vertex_count, uint32_t index_count, double origin_x,
                                 double origin_z, uint64_t generation, uint64_t frames_done,
                                 std::vector<uint64_t>* evicted) {
  if (ResidentDraw* have = acquire(key, generation)) return have;  // already here (same key twice in a world)
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
  ResidentDraw d;
  d.vertex_base = v, d.vertex_count = vertex_count, d.index_base = i, d.index_count = index_count;
  d.origin_x = origin_x, d.origin_z = origin_z, d.refs = 1, d.last_used = generation;
  return &(map_[key] = d);
}

void ResidentSet::release(uint64_t key, uint64_t tag) {
  auto it = map_.find(key);
  if (it == map_.end() || it->second.refs == 0) return;
  --it->second.refs;
  it->second.free_tag = std::max(it->second.free_tag, tag);
}

size_t ResidentSet::referenced() const {
  size_t n = 0;
  for (const auto& [k, d] : map_) n += d.refs != 0;
  return n;
}

void ResidentSet::evict_unreferenced(uint64_t frames_done, std::vector<uint64_t>* evicted) {
  while (evict_oldest(frames_done, evicted)) {
  }
}

uint64_t hash_mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  return h;
}

uint64_t hash_bytes(uint64_t h, const void* data, size_t bytes) {
  const auto* p = static_cast<const unsigned char*>(data);
  size_t i = 0;
  for (; i + 8 <= bytes; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = hash_mix(h, w);
  }
  uint64_t tail = 0;
  for (size_t k = 0; i + k < bytes; ++k) tail |= uint64_t(p[i + k]) << (8 * k);
  return hash_mix(h, tail ^ uint64_t(bytes));
}

bool draw_visible(const DrawGpu& d, const CullView& view) {
  if (view.frustum) {
    for (const auto& pl : view.planes) {
      // The box's corner furthest along the plane's normal.
      const float x = pl[0] >= 0 ? d.box_max[0] : d.box_min[0];
      const float y = pl[1] >= 0 ? d.box_max[1] : d.box_min[1];
      const float z = pl[2] >= 0 ? d.box_max[2] : d.box_min[2];
      if (pl[0] * x + pl[1] * y + pl[2] * z + pl[3] < 0) return false;
    }
  }
  const uint32_t flags = uint32_t(std::lround(d.params[1]));
  if (view.lod_reach > 0.0f && (flags & kFlagLodNear) != 0) {
    float d2 = 0;
    for (int i = 0; i < 3; ++i) {
      const float v = std::max({d.box_min[i] - view.eye[i], 0.0f, view.eye[i] - d.box_max[i]});
      d2 += v * v;
    }
    if (d2 > view.lod_reach * view.lod_reach) return false;
  }
  return true;
}

IndirectBatches build_indirect(const std::vector<DrawGpu>& table, const CullView& view, uint32_t max_per_batch) {
  IndirectBatches out;
  for (uint32_t i = 0; i < table.size(); ++i) {
    const DrawGpu& d = table[i];
    if (!draw_visible(d, view)) continue;
    auto& list = out.commands[std::min(d.draw[3], uint32_t(kBatchCount - 1))];
    if (list.size() >= max_per_batch) continue;  // the shader drops what does not fit
    int32_t vo;
    std::memcpy(&vo, &d.draw[2], sizeof vo);
    list.push_back({d.draw[0], 1u, d.draw[1], vo, i});
  }
  return out;
}

HizBox hiz_project_box(const float prev_vp[16], const float box_min[3], const float box_max[3], uint32_t hiz_w,
                       uint32_t hiz_h, bool reverse_z) {
  HizBox out{};
  if (!hiz_w || !hiz_h) return out;
  float u0 = 1.0f, v0 = 1.0f, u1 = 0.0f, v1 = 0.0f;
  float z_near = reverse_z ? 0.0f : 1.0f;
  for (int c = 0; c < 8; ++c) {
    const float x = (c & 1) ? box_max[0] : box_min[0];
    const float y = (c & 2) ? box_max[1] : box_min[1];
    const float z = (c & 4) ? box_max[2] : box_min[2];
    const float cx = prev_vp[0] * x + prev_vp[4] * y + prev_vp[8] * z + prev_vp[12];
    const float cy = prev_vp[1] * x + prev_vp[5] * y + prev_vp[9] * z + prev_vp[13];
    const float cz = prev_vp[2] * x + prev_vp[6] * y + prev_vp[10] * z + prev_vp[14];
    const float cw = prev_vp[3] * x + prev_vp[7] * y + prev_vp[11] * z + prev_vp[15];
    if (cw <= 0.05f) return out;
    const float u = (cx / cw) * 0.5f + 0.5f;
    const float v = (cy / cw) * 0.5f + 0.5f;
    const float ndc_z = std::clamp(cz / cw, 0.0f, 1.0f);
    u0 = std::min(u0, u);
    v0 = std::min(v0, v);
    u1 = std::max(u1, u);
    v1 = std::max(v1, v);
    z_near = reverse_z ? std::max(z_near, ndc_z) : std::min(z_near, ndc_z);
  }
  if (u1 <= 0.0f || v1 <= 0.0f || u0 >= 1.0f || v0 >= 1.0f) return out;
  u0 = std::clamp(u0, 0.0f, 1.0f);
  v0 = std::clamp(v0, 0.0f, 1.0f);
  u1 = std::clamp(u1, 0.0f, 1.0f);
  v1 = std::clamp(v1, 0.0f, 1.0f);
  const float span = std::max((u1 - u0) * float(hiz_w), (v1 - v0) * float(hiz_h));
  out.valid = true;
  out.uv_min[0] = u0, out.uv_min[1] = v0;
  out.uv_max[0] = u1, out.uv_max[1] = v1;
  out.z_near = z_near;
  out.lod = span > 1.0f ? std::ceil(std::log2(span)) : 0.0f;
  return out;
}

bool hiz_is_occluded(float z_near, float hiz_sample, bool reverse_z) {
  return reverse_z ? (z_near + 1e-4f < hiz_sample) : (z_near - 1e-4f > hiz_sample);
}

float hiz_reduce(float a, float b, float c, float d, bool reverse_z) {
  return reverse_z ? std::min({a, b, c, d}) : std::max({a, b, c, d});
}

bool prop_span_matches_origin(const PropSpan& sp, const std::array<float, 3>& game_pos, float radius_m) {
  // .pvsz / CollObjs prop origins match within centimetres; keep the origin radius tight
  // so neighboring fence panels 3-4 m away stay intact.
  const float r = std::clamp(radius_m * 0.72f, 1.15f, 2.35f);
  const float dx = sp.gx - game_pos[0];
  const float dy = sp.gy - game_pos[1];
  const float dz = sp.gz - game_pos[2];
  return (dx * dx + dz * dz <= r * r) && (std::fabs(dy) <= r + 1.6f);
}

bool prop_span_matches_aabb_fallback(const PropSpan& sp, const std::array<float, 3>& game_pos, float pad_m) {
  const float ex = sp.bmax[0] - sp.bmin[0];
  const float ey = sp.bmax[1] - sp.bmin[1];
  const float ez = sp.bmax[2] - sp.bmin[2];
  // Never hide giant buildings via fallback AABB overlap; allow tall slender poles (ey <= 24 m).
  if (std::max(ex, ez) > 14.0f || ey > 24.0f) return false;
  const float cx = std::clamp(game_pos[0], sp.bmin[0], sp.bmax[0]);
  const float cy = std::clamp(game_pos[1], sp.bmin[1], sp.bmax[1]);
  const float cz = std::clamp(game_pos[2], sp.bmin[2], sp.bmax[2]);
  const float dx = game_pos[0] - cx;
  const float dy = game_pos[1] - cy;
  const float dz = game_pos[2] - cz;
  return (dx * dx + dz * dz <= pad_m * pad_m) && (std::fabs(dy) <= pad_m + 1.5f);
}

bool prop_span_matches_same_instance(const PropSpan& sp, const std::array<float, 3>& origin) {
  const float dx = sp.gx - origin[0];
  const float dy = sp.gy - origin[1];
  const float dz = sp.gz - origin[2];
  return (dx * dx + dz * dz <= 0.35f * 0.35f) && (std::fabs(dy) <= 0.35f);
}

bool prop_span_matches_colocated_compact(const PropSpan& sp, const std::array<float, 3>& game_pos) {
  const float ex = sp.bmax[0] - sp.bmin[0];
  const float ey = sp.bmax[1] - sp.bmin[1];
  const float ez = sp.bmax[2] - sp.bmin[2];
  // Restrict to sign/post-sized compact props so wide adjacent fence panels are never touched.
  if (std::max(ex, ez) > 4.5f || ey > 10.0f) return false;
  const float odx = sp.gx - game_pos[0];
  const float ody = sp.gy - game_pos[1];
  const float odz = sp.gz - game_pos[2];
  if (odx * odx + odz * odz > 2.0f * 2.0f || std::fabs(ody) > 2.5f) return false;
  return prop_span_matches_aabb_fallback(sp, game_pos, 0.25f);
}

void SmashHideTracker::prune_non_resident(const std::function<bool(uint64_t)>& is_resident) {
  if (!is_resident) return;
  for (auto it = by_draw_key.begin(); it != by_draw_key.end();) {
    if (!is_resident(it->first)) it = by_draw_key.erase(it);
    else ++it;
  }
}

void SmashHideTracker::install_spans(std::vector<std::pair<uint64_t, std::vector<PropSpan>>>& prop_spans,
                                     const std::unordered_set<uint64_t>& uploaded_keys,
                                     const std::function<bool(uint64_t)>& is_resident,
                                     const std::function<bool(uint64_t, uint32_t&)>& lookup_index_base,
                                     const std::function<void(uint32_t, uint32_t)>& queue_zero) {
  prune_non_resident(is_resident);

  std::vector<uint64_t> touched_keys;
  touched_keys.reserve(prop_spans.size());
  std::unordered_set<uint64_t> touched_seen;
  touched_seen.reserve(prop_spans.size() * 2 + 8);

  for (auto& [key, spans] : prop_spans) {
    if (spans.empty()) continue;
    const bool freshly_uploaded = uploaded_keys.count(key) != 0;
    auto it = by_draw_key.find(key);
    if (freshly_uploaded || it == by_draw_key.end()) {
      for (auto& sp : spans) sp.hidden = false;
      by_draw_key[key] = std::move(spans);
    }
    if (touched_seen.insert(key).second) touched_keys.push_back(key);
  }
  prop_spans.clear();

  if (active_spheres.empty() || touched_keys.empty()) return;

  for (auto& hs : active_spheres) {
    std::vector<std::array<float, 3>> matched_origins;
    bool origin_hit = false;

    for (uint64_t key : touched_keys) {
      auto it = by_draw_key.find(key);
      if (it == by_draw_key.end()) continue;
      uint32_t idx_base = 0;
      if (!lookup_index_base(key, idx_base)) continue;
      for (auto& sp : it->second) {
        if (!prop_span_matches_origin(sp, hs.pos, hs.radius)) continue;
        origin_hit = true;
        matched_origins.push_back({sp.gx, sp.gy, sp.gz});
        if (!sp.hidden) {
          sp.hidden = true;
          queue_zero(idx_base + sp.rel_index_offset, sp.index_count);
        }
      }
    }

    if (origin_hit) {
      hs.origin_matched = true;
      // Propagate to sibling submeshes/LODs sharing the exact same prop instance origin
      // (catches decimetre quantization edges across LOD00/LOD02/decal/blended draws).
      for (uint64_t key : touched_keys) {
        auto it = by_draw_key.find(key);
        if (it == by_draw_key.end()) continue;
        uint32_t idx_base = 0;
        if (!lookup_index_base(key, idx_base)) continue;
        for (auto& sp : it->second) {
          if (sp.hidden) continue;
          bool match = prop_span_matches_colocated_compact(sp, hs.pos);
          if (!match) {
            for (const auto& mo : matched_origins) {
              if (prop_span_matches_same_instance(sp, mo)) {
                match = true;
                break;
              }
            }
          }
          if (match) {
            sp.hidden = true;
            queue_zero(idx_base + sp.rel_index_offset, sp.index_count);
          }
        }
      }
    }

    if (hs.used_aabb_fallback || (!hs.origin_matched && !origin_hit)) {
      for (uint64_t key : touched_keys) {
        auto it = by_draw_key.find(key);
        if (it == by_draw_key.end()) continue;
        uint32_t idx_base = 0;
        if (!lookup_index_base(key, idx_base)) continue;
        for (auto& sp : it->second) {
          if (sp.hidden) continue;
          if (prop_span_matches_aabb_fallback(sp, hs.pos, 0.85f)) {
            sp.hidden = true;
            hs.used_aabb_fallback = true;
            queue_zero(idx_base + sp.rel_index_offset, sp.index_count);
          }
        }
      }
    }
  }
}

size_t SmashHideTracker::hide_near(const std::array<float, 3>& game_pos, float radius_m, bool persistent,
                                   const std::vector<uint64_t>& resident_draw_keys,
                                   const std::function<bool(uint64_t, uint32_t&)>& lookup_index_base,
                                   const std::function<void(uint32_t, uint32_t)>& queue_zero) {
  if (radius_m <= 0.0f) return 0;
  const float r = std::max(0.5f, radius_m);
  active_spheres.push_back({game_pos, r, persistent, false, false});
  if (active_spheres.size() > 2048) {
    // Only prune oldest non-persistent spheres so broken discount signs are never evicted.
    size_t to_remove = 512;
    active_spheres.erase(
        std::remove_if(active_spheres.begin(), active_spheres.end(),
                       [&](const HiddenSphereRec& hs) {
                         if (hs.persistent || to_remove == 0) return false;
                         --to_remove;
                         return true;
                       }),
        active_spheres.end());
  }
  HiddenSphereRec& rec = active_spheres.back();

  if (resident_draw_keys.empty()) return 0;

  std::vector<uint64_t> unique_keys;
  unique_keys.reserve(resident_draw_keys.size());
  std::unordered_set<uint64_t> visited;
  visited.reserve(resident_draw_keys.size() * 2 + 8);
  for (uint64_t key : resident_draw_keys) {
    if (visited.insert(key).second) unique_keys.push_back(key);
  }

  size_t hidden_spans = 0;
  std::vector<std::array<float, 3>> matched_origins;
  bool any_origin_match = false;

  for (uint64_t key : unique_keys) {
    auto it = by_draw_key.find(key);
    if (it == by_draw_key.end()) continue;
    uint32_t idx_base = 0;
    if (!lookup_index_base(key, idx_base)) continue;
    for (auto& sp : it->second) {
      if (!prop_span_matches_origin(sp, game_pos, r)) continue;
      any_origin_match = true;
      matched_origins.push_back({sp.gx, sp.gy, sp.gz});
      if (!sp.hidden) {
        sp.hidden = true;
        queue_zero(idx_base + sp.rel_index_offset, sp.index_count);
        ++hidden_spans;
      }
    }
  }

  if (any_origin_match) {
    rec.origin_matched = true;
    for (uint64_t key : unique_keys) {
      auto it = by_draw_key.find(key);
      if (it == by_draw_key.end()) continue;
      uint32_t idx_base = 0;
      if (!lookup_index_base(key, idx_base)) continue;
      for (auto& sp : it->second) {
        if (sp.hidden) continue;
        bool match = prop_span_matches_colocated_compact(sp, game_pos);
        if (!match) {
          for (const auto& mo : matched_origins) {
            if (prop_span_matches_same_instance(sp, mo)) {
              match = true;
              break;
            }
          }
        }
        if (match) {
          sp.hidden = true;
          queue_zero(idx_base + sp.rel_index_offset, sp.index_count);
          ++hidden_spans;
        }
      }
    }
  } else {
    for (uint64_t key : unique_keys) {
      auto it = by_draw_key.find(key);
      if (it == by_draw_key.end()) continue;
      uint32_t idx_base = 0;
      if (!lookup_index_base(key, idx_base)) continue;
      for (auto& sp : it->second) {
        if (sp.hidden) continue;
        if (prop_span_matches_aabb_fallback(sp, game_pos, 0.85f)) {
          sp.hidden = true;
          rec.used_aabb_fallback = true;
          queue_zero(idx_base + sp.rel_index_offset, sp.index_count);
          ++hidden_spans;
        }
      }
    }
  }
  return hidden_spans;
}

size_t SmashHideTracker::unhide_near(const std::array<float, 3>& game_pos, float radius_m,
                                     const std::vector<uint64_t>& resident_draw_keys,
                                     const std::function<bool(uint64_t, uint32_t&)>& lookup_index_base,
                                     const std::function<void(uint32_t, const std::vector<uint32_t>&)>& queue_restore) {
  if (radius_m <= 0.0f) return 0;
  const float r = std::max(0.5f, radius_m);

  // Never remove persistent spheres (e.g. smashed discount signs).
  active_spheres.erase(
      std::remove_if(active_spheres.begin(), active_spheres.end(),
                     [&](const HiddenSphereRec& hs) {
                       if (hs.persistent) return false;
                       const float dx = hs.pos[0] - game_pos[0];
                       const float dy = hs.pos[1] - game_pos[1];
                       const float dz = hs.pos[2] - game_pos[2];
                       return (dx * dx + dz * dz <= r * r) && (std::fabs(dy) <= r + 1.6f);
                     }),
      active_spheres.end());

  if (resident_draw_keys.empty()) return 0;

  std::unordered_set<uint64_t> visited;
  visited.reserve(resident_draw_keys.size() * 2 + 8);
  size_t restored_spans = 0;

  for (uint64_t key : resident_draw_keys) {
    if (!visited.insert(key).second) continue;
    auto it = by_draw_key.find(key);
    if (it == by_draw_key.end()) continue;
    uint32_t idx_base = 0;
    if (!lookup_index_base(key, idx_base)) continue;
    for (auto& sp : it->second) {
      if (!sp.hidden) continue;
      if (!prop_span_matches_origin(sp, game_pos, r) && !prop_span_matches_aabb_fallback(sp, game_pos, r)) {
        continue;
      }
      // Never restore a span that still matches any remaining active sphere
      // (such as a persistent discount sign or an adjacent still-broken prop).
      bool still_hidden = false;
      for (const auto& hs : active_spheres) {
        if (prop_span_matches_origin(sp, hs.pos, hs.radius) ||
            ((hs.used_aabb_fallback || hs.persistent) && prop_span_matches_aabb_fallback(sp, hs.pos, 0.85f))) {
          still_hidden = true;
          break;
        }
      }
      if (still_hidden) continue;
      sp.hidden = false;
      queue_restore(idx_base + sp.rel_index_offset, sp.saved_indices);
      ++restored_spans;
    }
  }
  return restored_spans;
}

bool SmashHideTracker::box_overlaps_persistent_sphere(const float box_min[3], const float box_max[3],
                                                      double origin_x, double origin_z) const {
  for (const auto& hs : active_spheres) {
    if (!hs.persistent) continue;
    const float sx = float(double(hs.pos[0]) - origin_x);
    const float sy = hs.pos[1];
    const float sz = float(double(hs.pos[2]) - origin_z);
    const float cx = std::clamp(sx, box_min[0], box_max[0]);
    const float cy = std::clamp(sy, box_min[1], box_max[1]);
    const float cz = std::clamp(sz, box_min[2], box_max[2]);
    const float dx = sx - cx, dy = sy - cy, dz = sz - cz;
    if (dx * dx + dz * dz <= hs.radius * hs.radius && std::fabs(dy) <= hs.radius + 1.5f) return true;
  }
  return false;
}

}  // namespace carview::gpuworld
