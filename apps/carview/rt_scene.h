// Vulkan Hardware Ray Tracing Scene Manager (RT-1):
// Content-keyed static Colorado world BLAS cache with deferred compaction,
// modular car body/wheel/glass/traffic BLASes, Colorado pine tree cross-quad
// BLAS, triple-buffered per-frame TLAS with 8-bit visibility masks, and
// unified GPU metadata + dynamic light SSBOs.
#pragma once

#include <volk.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "renderer.h"
#include "rt_logic.h"

namespace carview {

class Showroom;
class WorldGpu;

// Per-geometry (per-draw inside a BLAS) metadata read by RT/PT shaders via SSBO.
// Matches GLSL `struct RtGeomInfo` in `shaders/rt_common.glsl`.
struct RtGeomInfoGpu {
  uint64_t vertex_bda = 0;
  uint64_t index_bda = 0;
  uint32_t material_class = 0;  // Car: 0..10; World: 100=opaque, 101=road, 102=water, 103=foliage, 104=emissive
  uint32_t flags = 0;           // bit0=car_vtx, bit1=metallic, bit2=clearcoat, bit3=glass, bit4=emissive, bit5=water, bit6=road, bit7=bindless_tex
  uint32_t tex_albedo = 0;      // WorldGpu bindless slot (or 0)
  uint32_t tex_aux = 0;         // WorldGpu lightmap/layer_b slot (or car paint finish)
  float tint[4] = {1, 1, 1, 0.5f};       // rgb linear base/paint color, a = perceptual roughness
  float uv0_xform[4] = {0, 1, 0, 1};     // offset_u, scale_u, offset_v, scale_v
};
static_assert(sizeof(RtGeomInfoGpu) == 64);

// Per-TLAS-instance metadata indexed by unpack_instance_info_index(instanceCustomIndex).
// Matches GLSL `struct RtInstanceInfo` in `shaders/rt_common.glsl`.
struct RtInstanceInfoGpu {
  uint32_t geom_offset = 0;
  uint32_t geom_count = 0;
  uint32_t kind = 0;          // InstanceKind
  uint32_t car_inst_idx = 0;  // 0 = player car, 1+ = traffic/AI car
  float model_row0[4] = {1, 0, 0, 0};
  float model_row1[4] = {0, 1, 0, 0};
  float model_row2[4] = {0, 0, 1, 0};
};
static_assert(sizeof(RtInstanceInfoGpu) == 64);

// Dynamic scene light for RT Headlight Shadows and ReSTIR DI.
// Matches GLSL `struct RtLightGpu` in `shaders/rt_common.glsl`.
struct RtLightGpu {
  float pos_type[4] = {0, 0, 0, 0};      // xyz world pos (or dir for type 0), w: 0=sun, 1=headlight spot, 2=taillight/brake, 3=world glow
  float dir_range[4] = {0, -1, 0, 100};  // xyz unit dir, w = range metres
  float color_power[4] = {1, 1, 1, 1};   // rgb linear color, w = cone outer cos (or intensity)
  float extra[4] = {0.9f, 0.1f, 0, 0};   // x = cone inner cos, y = emitter radius m, z = owner car id, w = flags
};
static_assert(sizeof(RtLightGpu) == 64);

struct RtSceneStats {
  uint32_t world_blas_count = 0;
  uint32_t world_blas_compacted = 0;
  uint32_t car_blas_count = 0;
  uint32_t tlas_instances = 0;
  uint32_t geom_count = 0;
  uint32_t light_count = 0;
  uint64_t blas_bytes = 0;
  uint64_t blas_saved_bytes = 0;
};

// Geometry flag bits shared with GLSL `rt_common.glsl`.
constexpr uint32_t kRtGeomFlagCarVertex = 1u << 0;
constexpr uint32_t kRtGeomFlagMetallic = 1u << 1;
constexpr uint32_t kRtGeomFlagClearcoat = 1u << 2;
constexpr uint32_t kRtGeomFlagGlass = 1u << 3;
constexpr uint32_t kRtGeomFlagEmissive = 1u << 4;
constexpr uint32_t kRtGeomFlagWater = 1u << 5;
constexpr uint32_t kRtGeomFlagRoad = 1u << 6;
constexpr uint32_t kRtGeomFlagBindlessTex = 1u << 7;
constexpr uint32_t kRtGeomFlagCutout = 1u << 8;
constexpr uint32_t kRtGeomFlagChrome = 1u << 9;
constexpr uint32_t kRtGeomFlagVertexBlend = 1u << 10;
constexpr uint32_t kRtGeomFlagTreeCross = 1u << 11;

class RtScene {
 public:
  using RetireFn = std::function<void(std::function<void()>)>;

  explicit RtScene(Renderer& r);
  RtScene(const RtScene&) = delete;
  RtScene& operator=(const RtScene&) = delete;
  ~RtScene();

  bool init(RetireFn retire_fn, std::string& error);
  void shutdown();
  bool ready() const { return ready_; }

  // Invalidates cached BLASes for a specific car (e.g. on car hot-swap).
  void invalidate_car(const Showroom* car);
  // Invalidates all cached world BLASes (e.g. on world reset).
  void invalidate_world();

  // Records pending static world BLAS builds, deferred compaction, car BLAS builds,
  // and this frame slot's TLAS rebuild + metadata SSBO updates into `cmd`.
  // Safe no-op if `!ready_`.
  void record_update(VkCommandBuffer cmd, const Showroom& host);

  VkAccelerationStructureKHR tlas() const { return slots_[active_slot_].tlas; }
  VkBuffer instance_info_buffer() const { return slots_[active_slot_].inst_info_buf.buffer; }
  VkBuffer geom_info_buffer() const { return slots_[active_slot_].geom_info_buf.buffer; }
  VkBuffer light_buffer() const { return slots_[active_slot_].light_buf.buffer; }
  uint32_t light_count() const { return stats_.light_count; }
  uint32_t instance_count() const { return stats_.tlas_instances; }
  const RtSceneStats& stats() const { return stats_; }

 private:
  struct BlasHandle {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    Renderer::Buffer buffer;
    VkDeviceAddress address = 0;
    uint64_t uncompacted_bytes = 0;
    uint64_t bytes = 0;
    bool compacted = false;
    bool compaction_pending = false;
    std::vector<RtGeomInfoGpu> geoms;
  };

  struct PendingCompaction {
    uint64_t world_key = 0;  // non-zero for static world BLAS
    uint32_t query_index = 0;
    uint64_t submit_tag = 0;
  };

  struct WheelSubBlas {
    BlasHandle blas;
    int draw_idx = -1;
  };

  struct CarBlasSet {
    VkBuffer vb_id = VK_NULL_HANDLE;
    VkBuffer ib_id = VK_NULL_HANDLE;
    BlasHandle body;
    BlasHandle glass;
    // One sub-BLAS per distinct (wheel, spins, local_matrix) transform so rims,
    // tyres, rotors, and calipers always sit at their exact per-draw transforms.
    std::vector<WheelSubBlas> wheel_parts;
    // Representative draw indices to fetch live body/glass transforms each frame.
    int body_draw = -1;
    int glass_draw = -1;
  };

  struct FrameSlot {
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    Renderer::Buffer tlas_buf;
    Renderer::Buffer tlas_scratch;
    Renderer::Buffer inst_vk_buf;    // VkAccelerationStructureInstanceKHR[]
    void* inst_vk_mapped = nullptr;
    uint32_t inst_vk_cap = 0;

    Renderer::Buffer inst_info_buf;  // RtInstanceInfoGpu[]
    void* inst_info_mapped = nullptr;
    uint32_t inst_info_cap = 0;

    Renderer::Buffer geom_info_buf;  // RtGeomInfoGpu[]
    void* geom_info_mapped = nullptr;
    uint32_t geom_info_cap = 0;

    Renderer::Buffer light_buf;      // RtLightGpu[]
    void* light_mapped = nullptr;
    uint32_t light_cap = 0;
  };

  struct WorldCellEntry {
    BlasHandle blas;
    double origin_x = 0.0;
    double origin_z = 0.0;
    uint8_t opacity_class = 0;  // 0=opaque, 1=cutout/foliage, 2=water
    uint64_t last_frame = 0;
  };

  bool build_blas_from_geoms(VkCommandBuffer cmd, const std::vector<VkAccelerationStructureGeometryKHR>& vk_geoms,
                             const std::vector<uint32_t>& prim_counts,
                             const std::vector<VkAccelerationStructureBuildRangeInfoKHR>& ranges,
                             bool allow_compaction, BlasHandle& out, uint32_t* out_query_index);
  bool ensure_car_blas(VkCommandBuffer cmd, const Showroom& car, CarBlasSet& out);
  void update_world_blases(VkCommandBuffer cmd, const Showroom& host);
  void update_tree_blas(VkCommandBuffer cmd, const Showroom& host);
  void process_compactions(VkCommandBuffer cmd);
  void destroy_blas(BlasHandle& b, bool defer);
  bool ensure_scratch(VkCommandBuffer cmd, VkDeviceSize needed, VkDeviceAddress& out_addr);
  bool ensure_slot_buffers(FrameSlot& s, uint32_t inst_n, uint32_t geom_n, uint32_t light_n);

  Renderer& r_;
  RetireFn retire_fn_;
  bool ready_ = false;
  uint32_t active_slot_ = 0;
  uint64_t frame_counter_ = 0;

  // Shared per-frame scratch buffer ring for BLAS builds (reused via barrier on burst frames).
  static constexpr VkDeviceSize kBlasScratchBytes = 24ull << 20;
  Renderer::Buffer blas_scratch_[Renderer::kFramesInFlight];
  VkDeviceAddress blas_scratch_bda_[Renderer::kFramesInFlight] = {};
  rt::ScratchRingAllocator scratch_alloc_;

  // Deferred BLAS compaction query pool.
  static constexpr uint32_t kMaxCompactionQueries = 1024;
  VkQueryPool compact_pool_ = VK_NULL_HANDLE;
  uint32_t next_query_ = 0;
  std::deque<PendingCompaction> pending_compactions_;

  // Content-keyed static world BLAS cache.
  rt::BlasCacheLru world_lru_{144ull << 20};
  std::unordered_map<uint64_t, WorldCellEntry> world_blases_;
  std::vector<uint64_t> active_world_keys_;

  // Dynamic Colorado tree cross-quad BLAS (rebuilt only when world_trees_ generation changes).
  uint64_t tree_world_gen_ = ~0ull;
  Renderer::Buffer tree_vb_, tree_ib_;
  BlasHandle tree_blas_;
  double tree_origin_x_ = 0.0, tree_origin_z_ = 0.0;

  // Cached modular car BLASes (keyed by Showroom pointer).
  std::unordered_map<const Showroom*, CarBlasSet> car_blases_;

  FrameSlot slots_[Renderer::kFramesInFlight];
  RtSceneStats stats_{};
};

}  // namespace carview
