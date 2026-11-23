#include "rt_logic.h"

#include <algorithm>
#include <cctype>

namespace carview::rt {

const char* rt_mode_name(RtMode m) {
  switch (m) {
    case RtMode::kHybrid: return "hybrid";
    case RtMode::kPathTraced: return "pt";
    case RtMode::kPhotoModePt: return "photo-pt";
    default: return "off";
  }
}

const char* rt_mode_label(RtMode m) {
  switch (m) {
    case RtMode::kHybrid: return "HYBRID RT";
    case RtMode::kPathTraced: return "PATH TRACED";
    case RtMode::kPhotoModePt: return "PHOTO MODE PT";
    default: return "OFF";
  }
}

RtMode parse_rt_mode(std::string_view name) {
  std::string lower;
  lower.reserve(name.size());
  for (char c : name) lower.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  if (lower == "hybrid" || lower == "hybrid-rt" || lower == "rt" || lower == "on" || lower == "1")
    return RtMode::kHybrid;
  if (lower == "pt" || lower == "pathtraced" || lower == "path-traced" || lower == "pathtrace" || lower == "2")
    return RtMode::kPathTraced;
  if (lower == "photo-pt" || lower == "photo" || lower == "photomode-pt" || lower == "3")
    return RtMode::kPhotoModePt;
  return RtMode::kOff;
}

int clamp_rt_bounces(int bounces) { return std::clamp(bounces, 1, 4); }
int clamp_rt_spp(int spp) { return std::clamp(spp, 1, 16); }
int clamp_photo_pt_spp(int spp) { return std::clamp(spp, 16, 256); }

RtDecision resolve_rt_mode(RtMode requested, const RtDeviceCaps& caps) {
  RtDecision d{};
  d.requested = requested;
  if (requested == RtMode::kOff) {
    d.active_mode = RtMode::kOff;
    d.reason = "RT disabled";
    return d;
  }
  if (!caps.compiled_in) {
    d.active_mode = RtMode::kOff;
    d.unavailable = true;
    d.reason = "Built with SHIFT_ENABLE_RAY_TRACING=OFF";
    return d;
  }
  if (!caps.supports_hybrid()) {
    d.active_mode = RtMode::kOff;
    d.unavailable = true;
    d.reason = "GPU lacks VK_KHR_acceleration_structure + VK_KHR_ray_query + BDA";
    return d;
  }

  d.use_omm = caps.opacity_micromap && !caps.is_moltenvk && !caps.is_turnip;
  d.use_ser = (caps.invocation_reorder_ext || caps.invocation_reorder_nv) && caps.ray_tracing_pipeline &&
              !caps.is_moltenvk && !caps.is_turnip;
  d.half_res_reflections = caps.is_moltenvk || caps.is_turnip || caps.is_mobile_tbdr;
  d.half_res_shadows = caps.is_turnip || caps.is_mobile_tbdr;

  if (requested == RtMode::kHybrid) {
    d.active_mode = RtMode::kHybrid;
    d.use_rt_pipeline = false;  // Tier 1 Hybrid RT always uses inline VK_KHR_ray_query compute
    d.reason = d.half_res_reflections ? "Hybrid RT (inline ray_query, half-res mobile/portable)"
                                      : "Hybrid RT (inline ray_query, full-res)";
    return d;
  }

  if (requested == RtMode::kPhotoModePt) {
    d.active_mode = RtMode::kPhotoModePt;
    d.use_rt_pipeline = false;  // Compute wavefront PT works universally across all ray_query GPUs
    d.half_res_reflections = false;
    d.half_res_shadows = false;
    d.reason = "Convergent Photo Mode Path Tracing";
    return d;
  }

  // requested == RtMode::kPathTraced
  if (!caps.supports_pathtracing()) {
    d.active_mode = RtMode::kHybrid;
    d.use_rt_pipeline = false;
    d.reason = "Real-time PT clamped to Hybrid RT on mobile/portable GPU";
    return d;
  }
  d.active_mode = RtMode::kPathTraced;
  d.use_rt_pipeline = false;  // Unified VK_KHR_ray_query wavefront compute PT (universal Vulkan 1.3)
  d.reason = d.use_ser ? "Real-Time Path Tracing (ReSTIR DI + SVGF, SER-capable GPU)"
                       : "Real-Time Path Tracing (ReSTIR DI + SVGF)";
  return d;
}

uint32_t pack_instance_custom_index(uint32_t info_index, InstanceKind kind) {
  const uint32_t idx = info_index & kInstanceInfoIndexMask;
  const uint32_t tag = (uint32_t(kind) & 0x0Fu) << 20u;
  return (tag | idx) & kMaxInstanceCustomIndex;
}

uint32_t unpack_instance_info_index(uint32_t custom_index) { return custom_index & kInstanceInfoIndexMask; }

InstanceKind unpack_instance_kind(uint32_t custom_index) {
  return static_cast<InstanceKind>((custom_index >> 20u) & 0x0Fu);
}

uint32_t pack_instance_custom_and_mask(uint32_t custom_index_24, uint8_t mask_8) {
  return (custom_index_24 & kMaxInstanceCustomIndex) | (uint32_t(mask_8) << 24u);
}

uint8_t unpack_instance_mask(uint32_t packed_word) { return uint8_t((packed_word >> 24u) & 0xFFu); }

uint64_t make_world_cell_key(int32_t cell_x, int32_t cell_z, uint8_t opacity_class, uint64_t content_hash) {
  auto mix = [](uint64_t h, uint64_t v) -> uint64_t {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
  };
  uint64_t h = 0x517CC1B727220A95ull;
  h = mix(h, uint64_t(uint32_t(cell_x)));
  h = mix(h, uint64_t(uint32_t(cell_z)));
  h = mix(h, uint64_t(opacity_class));
  h = mix(h, content_hash);
  return h ? h : 1ull;
}

const BlasCacheEntry* BlasCacheLru::find(uint64_t key) const {
  auto it = entries_.find(key);
  return it != entries_.end() ? &it->second : nullptr;
}

bool BlasCacheLru::acquire(uint64_t key, uint64_t generation) {
  auto it = entries_.find(key);
  if (it == entries_.end()) return false;
  it->second.refs += 1u;
  it->second.last_used_gen = generation;
  return true;
}

void BlasCacheLru::insert(uint64_t key, uint64_t bytes, uint64_t generation) {
  auto it = entries_.find(key);
  if (it != entries_.end()) {
    if (used_bytes_ >= it->second.bytes) used_bytes_ -= it->second.bytes;
    it->second.bytes = bytes;
    it->second.last_used_gen = generation;
    it->second.refs = std::max(it->second.refs, 1u);
    it->second.compacted = false;
    used_bytes_ += bytes;
    return;
  }
  BlasCacheEntry e{};
  e.key = key;
  e.bytes = bytes;
  e.last_used_gen = generation;
  e.refs = 1u;
  e.compacted = false;
  entries_.emplace(key, e);
  used_bytes_ += bytes;
}

bool BlasCacheLru::mark_compacted(uint64_t key, uint64_t compacted_bytes) {
  auto it = entries_.find(key);
  if (it == entries_.end()) return false;
  if (used_bytes_ >= it->second.bytes) used_bytes_ -= it->second.bytes;
  it->second.bytes = compacted_bytes;
  it->second.compacted = true;
  used_bytes_ += compacted_bytes;
  return true;
}

void BlasCacheLru::release(uint64_t key, uint64_t frame_tag) {
  auto it = entries_.find(key);
  if (it == entries_.end()) return;
  if (it->second.refs > 0u) it->second.refs -= 1u;
  if (it->second.refs == 0u) it->second.free_tag = frame_tag;
}

std::vector<uint64_t> BlasCacheLru::evict(uint64_t frames_done, bool force_all_unreferenced) {
  std::vector<uint64_t> evicted;
  if (!force_all_unreferenced && used_bytes_ <= budget_bytes_) return evicted;
  std::vector<BlasCacheEntry*> candidates;
  candidates.reserve(entries_.size());
  for (auto& [k, e] : entries_) {
    if (e.refs == 0u && e.free_tag <= frames_done) candidates.push_back(&e);
  }
  std::sort(candidates.begin(), candidates.end(), [](const BlasCacheEntry* a, const BlasCacheEntry* b) {
    if (a->last_used_gen != b->last_used_gen) return a->last_used_gen < b->last_used_gen;
    return a->key < b->key;
  });
  for (BlasCacheEntry* e : candidates) {
    if (!force_all_unreferenced && used_bytes_ <= budget_bytes_) break;
    const uint64_t k = e->key;
    const uint64_t b = e->bytes;
    if (used_bytes_ >= b) used_bytes_ -= b;
    else used_bytes_ = 0;
    entries_.erase(k);
    evicted.push_back(k);
    ++evictions_;
  }
  return evicted;
}

void BlasCacheLru::clear() {
  entries_.clear();
  used_bytes_ = 0;
}

uint64_t ScratchRingAllocator::allocate(uint64_t bytes) {
  if (bytes == 0 || capacity_ == 0) return UINT64_MAX;
  const uint64_t mask = uint64_t(alignment_) - 1ull;
  const uint64_t aligned = (cursor_ + mask) & ~mask;
  if (aligned > capacity_ || bytes > capacity_ - aligned) return UINT64_MAX;
  cursor_ = aligned + bytes;
  return aligned;
}

}  // namespace carview::rt
