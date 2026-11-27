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
  uint32_t vb_slot = 0;         // Bindless RT geometry buffer slot (Set 2 binding 0) for vertices
  uint32_t vb_off = 0;          // Base vertex index offset within vb_slot
  uint32_t ib_slot = 0;         // Bindless RT geometry buffer slot (Set 2 binding 0) for uint32 indices
  uint32_t ib_off = 0;          // Base uint32 index offset within ib_slot
  uint32_t material_class = 0;  // Car: 0..10; World: 100=opaque, 101=road, 102=water, 103=foliage, 104=emissive
  uint32_t flags = 0;           // bit0=car_vtx, bit1=metallic, bit2=clearcoat, bit3=glass, bit4=emissive, bit5=water, bit6=road, bit7=bindless_tex
  uint32_t tex_albedo = 0;      // WorldGpu bindless slot (or 0)
  uint32_t tex_aux = 0;         // WorldGpu lightmap/layer_b slot (or car paint finish)
  float tint[4] = {1, 1, 1, 0.5f};       // rgb linear base/paint color, a = perceptual roughness
  float uv0_xform[4] = {0, 1, 0, 1};     // offset_u, scale_u, offset_v, scale_v (world: tiling_a.xy, tiling_b.zw)
  uint32_t extra_tex[4] = {0, 0, 0, 0};  // x=tex_splat, y=tex_layer_c, z=tiling_c/decal_c4 half2x16, w=tex_ao
};
static_assert(sizeof(RtGeomInfoGpu) == 80);

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
constexpr uint32_t kRtGeomFlagSplat = 1u << 12;
constexpr uint32_t kRtGeomFlagLayerC = 1u << 13;
constexpr uint32_t kRtGeomFlagDecal = 1u << 14;
constexpr uint32_t kRtGeomFlagVertexFade = 1u << 15;
constexpr uint32_t kRtGeomFlagDecalNoise = 1u << 16;
constexpr uint32_t kRtGeomFlagAoMap = 1u << 17;

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

  static constexpr uint32_t kMaxRtGeomBuffers = 256;

  VkAccelerationStructureKHR tlas() const { return slots_[active_slot_].tlas; }
  VkBuffer instance_info_buffer() const { return slots_[active_slot_].inst_info_buf.buffer; }
  VkBuffer geom_info_buffer() const { return slots_[active_slot_].geom_info_buf.buffer; }
  VkBuffer light_buffer() const { return slots_[active_slot_].light_buf.buffer; }
  const std::vector<VkBuffer>& geom_buffers() const { return slots_[active_slot_].geom_buffers; }
  VkBuffer fallback_geom_buffer() const { return seq_ib_.buffer; }
  uint32_t light_count() const { return stats_.light_count; }
  uint32_t instance_count() const { return stats_.tlas_instances; }
  const RtSceneStats& stats() const { return stats_; }

 private:
  struct WorldGeomTexRecords {
    uint32_t albedo = 0xFFFFFFFFu;
    uint32_t aux = 0xFFFFFFFFu;
    uint32_t splat = 0xFFFFFFFFu;
    uint32_t layer_c = 0xFFFFFFFFu;
    uint32_t ao = 0xFFFFFFFFu;
    bool pending = false;
  };

  struct BlasHandle {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    Renderer::Buffer buffer;
    VkDeviceAddress address = 0;
    uint64_t uncompacted_bytes = 0;
    uint64_t bytes = 0;
    bool compacted = false;
    bool compaction_pending = false;
    std::vector<RtGeomInfoGpu> geoms;
    std::vector<WorldGeomTexRecords> tex_refs;
    VkBuffer vb_buf = VK_NULL_HANDLE;
    VkBuffer ib_buf = VK_NULL_HANDLE;
    bool any_tex_pending = false;
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
    // Per-geometry LampChannel for body.geoms so live braking/headlight/DRL state
    // updates car lamp emission every frame (matching raster evaluate_lamp_emission).
    std::vector<uint8_t> body_lamp_channels;
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

    // Per-frame-slot dynamic actor vertex buffer & BLASes (seated driver, Spectators.zip
    // festival crowds, and Airborne Showcase aircraft) — zero allocation churn across frames.
    Renderer::Buffer dyn_vb;
    void* dyn_vb_mapped = nullptr;
    uint32_t dyn_vb_cap = 0;
    BlasHandle driver_blas;
    BlasHandle spectator_blas;
    BlasHandle showcase_blas;
    // Per-frame-slot deduplicated bindless geometry storage buffers (Set 2 binding 0).
    std::vector<VkBuffer> geom_buffers;
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
  void update_dynamic_actor_blases(VkCommandBuffer cmd, const Showroom& host, FrameSlot& slot);
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
  uint64_t last_world_blas_gen_ = ~0ull;
  size_t last_world_draw_count_ = 0;
  VkBuffer last_world_vb_ = VK_NULL_HANDLE;
  VkBuffer last_world_ib_ = VK_NULL_HANDLE;
  bool last_world_blases_complete_ = false;

  // Dynamic Colorado tree cross-quad BLAS (rebuilt when world_trees_ generation changes
  // or when pending tree atlas textures finish uploading to WorldGpu).
  uint64_t tree_world_gen_ = ~0ull;
  bool tree_textures_pending_ = false;
  size_t tree_tex_count_ = 0;
  Renderer::Buffer tree_vb_, tree_ib_;
  BlasHandle tree_blas_;
  double tree_origin_x_ = 0.0, tree_origin_z_ = 0.0;

  // Cached modular car BLASes (keyed by Showroom pointer).
  std::unordered_map<const Showroom*, CarBlasSet> car_blases_;

  // Shared static sequential index buffer (0..kMaxDynActorVerts-1) for dynamic actor BLASes.
  static constexpr uint32_t kMaxDynActorVerts = 131072;
  Renderer::Buffer seq_ib_;
  VkDeviceAddress seq_ib_bda_ = 0;
  VkImageView registered_spectator_view_ = VK_NULL_HANDLE;
  VkImageView registered_showcase_view_ = VK_NULL_HANDLE;

  FrameSlot slots_[Renderer::kFramesInFlight];
  RtSceneStats stats_{};
};

}  // namespace carview
