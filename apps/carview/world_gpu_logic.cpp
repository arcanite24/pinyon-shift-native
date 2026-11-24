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

}  // namespace carview::gpuworld
