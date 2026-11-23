// Pure C++ Ray Tracing & Path Tracing capability negotiation, tier clamping,
// BVH visibility mask packing, content-keyed BLAS LRU cache, and scratch ring
// allocator. Zero Vulkan headers so every rule is unit-tested in CI.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace carview::rt {

// Active ray tracing / path tracing tier.
enum class RtMode : int {
  kOff = 0,         // Pure rasterization (0 overhead, 0 RT allocations)
  kHybrid = 1,      // Tier 1: VK_KHR_ray_query RT Reflections + Soft Shadows + RTAO
  kPathTraced = 2,  // Tier 2: Full Real-Time Multi-Bounce Path Tracing + ReSTIR DI + SVGF
  kPhotoModePt = 3, // Convergent progressive multi-SPP Photo Mode Path Tracing
};

const char* rt_mode_name(RtMode m);
const char* rt_mode_label(RtMode m);
RtMode parse_rt_mode(std::string_view name);

int clamp_rt_bounces(int bounces);
int clamp_rt_spp(int spp);
int clamp_photo_pt_spp(int spp);

// Hardware & driver capabilities probed at VkDevice creation.
struct RtDeviceCaps {
  bool compiled_in = false;
  bool buffer_device_address = false;
  bool acceleration_structure = false;
  bool ray_query = false;
  bool ray_tracing_pipeline = false;
  bool deferred_host_operations = false;
  bool rt_maintenance1 = false;
  bool opacity_micromap = false;
  bool invocation_reorder_nv = false;
  bool invocation_reorder_ext = false;
  bool scalar_block_layout = false;
  bool shader_int64 = false;
  bool async_compute_queue = false;
  bool is_moltenvk = false;
  bool is_turnip = false;
  bool is_mobile_tbdr = false;
  uint32_t max_ray_recursion_depth = 0;
  uint32_t shader_group_handle_size = 0;
  uint32_t shader_group_base_alignment = 0;
  uint32_t min_as_scratch_alignment = 256;
  uint64_t device_local_bytes = 0;

  bool supports_hybrid() const {
    return compiled_in && buffer_device_address && acceleration_structure && ray_query &&
           deferred_host_operations && scalar_block_layout && shader_int64;
  }
  bool supports_pathtracing() const {
    return supports_hybrid() && !is_moltenvk && !is_turnip && !is_mobile_tbdr;
  }
  bool supports_photo_pt() const {
    // Even mobile / MoltenVK / RDNA2 GPUs with ray_query can converge Photo Mode PT.
    return supports_hybrid();
  }
};

// Resolved runtime RT configuration after capability & platform clamping.
struct RtDecision {
  RtMode requested = RtMode::kOff;
  RtMode active_mode = RtMode::kOff;
  bool use_rt_pipeline = false;
  bool use_omm = false;
  bool use_ser = false;
  bool half_res_reflections = false;
  bool half_res_shadows = false;
  bool unavailable = false;
  std::string reason;
};

RtDecision resolve_rt_mode(RtMode requested, const RtDeviceCaps& caps);

// 8-bit TLAS instance visibility masks (VkAccelerationStructureInstanceKHR::mask).
constexpr uint8_t kMaskPlayerCar = 0x01u;
constexpr uint8_t kMaskTrafficCar = 0x02u;
constexpr uint8_t kMaskWorldOpaque = 0x04u;
constexpr uint8_t kMaskFoliageAlpha = 0x08u;
constexpr uint8_t kMaskGlass = 0x10u;
constexpr uint8_t kMaskEmissive = 0x20u;
constexpr uint8_t kMaskWater = 0x40u;
constexpr uint8_t kMaskCockpitInterior = 0x80u;

constexpr uint8_t kMaskShadowCasters =
    kMaskPlayerCar | kMaskTrafficCar | kMaskWorldOpaque | kMaskFoliageAlpha;
constexpr uint8_t kMaskReflectionHits =
    kMaskPlayerCar | kMaskTrafficCar | kMaskWorldOpaque | kMaskFoliageAlpha | kMaskGlass | kMaskEmissive | kMaskWater;
constexpr uint8_t kMaskPrimaryHits = 0xFFu;

// 4-bit instance kind tag stored in upper bits [20..23] of 24-bit instanceCustomIndex.
enum class InstanceKind : uint8_t {
  kWorld = 0,
  kPlayerCar = 1,
  kTrafficCar = 2,
  kFoliage = 3,
  kGlass = 4,
};

constexpr uint32_t kInstanceInfoIndexMask = 0x000FFFFFu;
constexpr uint32_t kMaxInstanceCustomIndex = 0x00FFFFFFu;

uint32_t pack_instance_custom_index(uint32_t info_index, InstanceKind kind);
uint32_t unpack_instance_info_index(uint32_t custom_index);
InstanceKind unpack_instance_kind(uint32_t custom_index);

// Packs Vulkan's combined 32-bit word: bits [0..23] = instanceCustomIndex, bits [24..31] = mask.
uint32_t pack_instance_custom_and_mask(uint32_t custom_index_24, uint8_t mask_8);
uint8_t unpack_instance_mask(uint32_t packed_word);

// Spatial cell key (128 m x 128 m quantized XZ + opacity class) for static Colorado BLAS grouping.
uint64_t make_world_cell_key(int32_t cell_x, int32_t cell_z, uint8_t opacity_class, uint64_t content_hash);

// Content-keyed LRU cache bookkeeping for static world BLASes (pure C++, unit-tested).
struct BlasCacheEntry {
  uint64_t key = 0;
  uint64_t bytes = 0;
  uint64_t last_used_gen = 0;
  uint64_t free_tag = 0;  // frames_submitted when last dereferenced
  uint32_t refs = 0;
  bool compacted = false;
};

class BlasCacheLru {
 public:
  explicit BlasCacheLru(uint64_t budget_bytes = 128ull << 20) : budget_bytes_(budget_bytes) {}

  void set_budget(uint64_t budget_bytes) { budget_bytes_ = budget_bytes; }
  uint64_t budget_bytes() const { return budget_bytes_; }
  uint64_t used_bytes() const { return used_bytes_; }
  size_t entry_count() const { return entries_.size(); }
  uint64_t eviction_count() const { return evictions_; }

  bool contains(uint64_t key) const { return entries_.count(key) != 0; }
  const BlasCacheEntry* find(uint64_t key) const;

  // Marks key referenced for `generation`. Returns true if present.
  bool acquire(uint64_t key, uint64_t generation);
  // Inserts a newly built BLAS with refs = 1.
  void insert(uint64_t key, uint64_t bytes, uint64_t generation);
  // Updates stored byte footprint after deferred BLAS compaction completes.
  bool mark_compacted(uint64_t key, uint64_t compacted_bytes);
  // Releases one reference; records `frame_tag` when refs drops to 0.
  void release(uint64_t key, uint64_t frame_tag);

  // Evicts oldest unreferenced entries whose `free_tag <= frames_done` until
  // `used_bytes_ <= budget_bytes_` (or `force_all_unreferenced` is true).
  // Returns the keys evicted so the Vulkan owner can retire/destroy their AS handles.
  std::vector<uint64_t> evict(uint64_t frames_done, bool force_all_unreferenced = false);
  void clear();

 private:
  uint64_t budget_bytes_ = 128ull << 20;
  uint64_t used_bytes_ = 0;
  uint64_t evictions_ = 0;
  std::unordered_map<uint64_t, BlasCacheEntry> entries_;
};

// Aligned linear scratch sub-allocator for per-frame BLAS/TLAS builds.
class ScratchRingAllocator {
 public:
  ScratchRingAllocator(uint64_t capacity_bytes = 0, uint32_t alignment = 256)
      : capacity_(capacity_bytes), alignment_(alignment > 0 ? alignment : 256) {}

  void reset_capacity(uint64_t capacity_bytes, uint32_t alignment) {
    capacity_ = capacity_bytes;
    alignment_ = alignment > 0 ? alignment : 256;
    cursor_ = 0;
  }
  void begin_frame() { cursor_ = 0; }
  uint64_t capacity() const { return capacity_; }
  uint64_t used() const { return cursor_; }
  uint32_t alignment() const { return alignment_; }

  // Returns aligned byte offset or UINT64_MAX if insufficient space remains.
  uint64_t allocate(uint64_t bytes);

 private:
  uint64_t capacity_ = 0;
  uint32_t alignment_ = 256;
  uint64_t cursor_ = 0;
};

}  // namespace carview::rt
