// The GPU-driven world's CPU-side bookkeeping (Slice 11), plain C++ so it is
// unit-tested: a first-fit sub-allocator over the resident vertex and index
// arenas, the resident set of draws (per material and ground cell, found
// again by a content key so a regather uploads only what is new), the packed
// per-draw table the culling compute shader reads, and a CPU model of that
// shader's visibility rule and of the indirect commands it writes.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace carview::gpuworld {

constexpr uint32_t kNoSpace = 0xFFFFFFFFu;

// First-fit free-list over [0, capacity) units; adjacent free ranges merge.
class ArenaAllocator {
 public:
  explicit ArenaAllocator(uint32_t capacity = 0) { reset(capacity); }
  void reset(uint32_t capacity);
  // The offset of `units` free units, or kNoSpace.
  uint32_t allocate(uint32_t units);
  void free(uint32_t offset, uint32_t units);
  uint32_t capacity() const { return capacity_; }
  uint32_t used() const { return used_; }
  uint32_t largest_free() const;
  size_t free_ranges() const { return free_.size(); }

 private:
  uint32_t capacity_ = 0, used_ = 0;
  std::map<uint32_t, uint32_t> free_;  // offset -> units
};

// What a draw needs on the GPU, and where it lives once resident.
struct ResidentDraw {
  uint32_t vertex_base = 0, vertex_count = 0;  // in vertices of the vertex arena
  uint32_t index_base = 0, index_count = 0;    // in indices of the index arena
  double origin_x = 0, origin_z = 0;           // the frame its vertices are relative to
  uint32_t refs = 0;                           // worlds (current, pending, fading) using it
  bool valid = false;                          // its data is on the GPU (the copy that placed it finished)
  uint64_t last_used = 0;                      // generation of its last use, for eviction order
  uint64_t free_tag = 0;                       // frames submitted when its last reference went: reusable once those are done
};

// Residency by content key across regathers.
class ResidentSet {
 public:
  ResidentSet(uint32_t vertex_capacity = 0, uint32_t index_capacity = 0) { reset(vertex_capacity, index_capacity); }
  void reset(uint32_t vertex_capacity, uint32_t index_capacity);

  // A resident draw with this key (its reference count taken), or null. An
  // entry whose data never arrived (not valid, no references) is dropped.
  ResidentDraw* acquire(uint64_t key, uint64_t generation);
  // The copy that placed this draw is done.
  void mark_valid(uint64_t key);
  // Makes room for a new draw and returns it (one reference), evicting
  // unreferenced draws oldest first when the arenas are full (only those the
  // GPU is done with: free_tag <= frames_done); null when even that is not
  // enough. `evicted` collects the keys dropped.
  ResidentDraw* place(uint64_t key, uint32_t vertex_count, uint32_t index_count, double origin_x, double origin_z,
                      uint64_t generation, uint64_t frames_done = ~0ull, std::vector<uint64_t>* evicted = nullptr);
  // Drops a reference; `tag` is the frames submitted so far (a frame in
  // flight may still draw it).
  void release(uint64_t key, uint64_t tag = 0);
  bool contains(uint64_t key) const { return map_.count(key) != 0; }
  const ResidentDraw* find(uint64_t key) const;
  size_t count() const { return map_.size(); }
  size_t referenced() const;
  uint64_t vertex_used() const { return vertices_.used(); }
  uint64_t index_used() const { return indices_.used(); }
  uint32_t vertex_capacity() const { return vertices_.capacity(); }
  uint32_t index_capacity() const { return indices_.capacity(); }
  uint64_t evictions() const { return evictions_; }
  // Drops every unreferenced draw the GPU is done with.
  void evict_unreferenced(uint64_t frames_done = ~0ull, std::vector<uint64_t>* evicted = nullptr);

 private:
  bool evict_oldest(uint64_t frames_done, std::vector<uint64_t>* evicted);
  std::unordered_map<uint64_t, ResidentDraw> map_;
  ArenaAllocator vertices_, indices_;
  uint64_t evictions_ = 0;
};

// The key of a draw: its material, state and geometry, with positions made
// absolute (so the same draw found by a later gather, relative to another
// origin, has the same key).
uint64_t hash_mix(uint64_t h, uint64_t v);
uint64_t hash_bytes(uint64_t h, const void* data, size_t bytes);
inline int64_t quantize_mm(double metres) { return int64_t(metres * 1000.0 + (metres < 0 ? -0.5 : 0.5)); }

// ---- The per-draw table (matches shaders/world_gpu_table.glsl) ------------
// Solid opaque batches (0, 1) have zero shader discard -> Early-Z + Z-prepass eligible.
// Dithered opaque LOD batches (4, 5) run screen-door LOD discard separately so they never
// disable Early-Z on solid terrain, roads, and buildings.
enum Batch : uint32_t {
  kOpaque = 0,
  kOpaqueDecal = 1,
  kCutout = 2,
  kCutoutDecal = 3,
  kOpaqueDither = 4,
  kOpaqueDitherDecal = 5,
  kBatchCount = 6
};
// blend: 0 opaque, 1 coverage (cut-out), >= 2 blended (not in the table).
inline bool in_gpu_batches(uint8_t blend) { return blend <= 1; }
inline Batch batch_of(uint8_t blend, bool decal, bool dither = false) {
  if (blend == 1) return decal ? kCutoutDecal : kCutout;
  if (dither) return decal ? kOpaqueDitherDecal : kOpaqueDither;
  return decal ? kOpaqueDecal : kOpaque;
}

struct DrawGpu {  // std430: eight vec4
  float tint[4];
  float params[4];     // x blend, y flags, z far level, w light gain
  uint32_t tex0[4];    // albedo, layer_b, splat, light_map (bindless slots)
  uint32_t tex1[4];    // normal_map, ao_map, layer_c, bit 0: fades in with the swap
  float box_min[4];    // xyz in the current scenery frame
  float box_max[4];
  uint32_t draw[4];    // index_count, first_index, vertex_offset (int bits), batch
  float shift[4];      // xyz: added to the vertices (origin difference), w unused
};
static_assert(sizeof(DrawGpu) == 128);

struct TreeInstanceGpu {  // std430: four vec4 (64 bytes per tree billboard)
  float base_hw[4];    // xyz base in scenery frame, w half_width
  float up_h[4];       // xyz up axis in scenery frame, w height
  float uv_rect[4];    // u0, v0, u1, v1
  uint32_t col_tex[4]; // x RGBA8 tint, y bindless tex slot, z unused, w unused
};
static_assert(sizeof(TreeInstanceGpu) == 64);

// What the culling shader sees of the view.
struct CullView {
  float planes[4][4];   // w+x, w-x, w+y, w-y of the scenery's clip, as normal.xyz, d
  float eye[3] = {0, 0, 0};  // in the scenery frame
  float lod_reach = 0;  // > 0: prop LOD00 copies wholly past it are not drawn
  bool frustum = true;
  float prev_vp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // scenery -> previous frame clip
  bool hiz = false;
  bool reverse_z = false;
  uint32_t hiz_width = 0, hiz_height = 0;
};
constexpr uint32_t kFlagSky = 2u;
constexpr uint32_t kFlagLodNear = 128u;

struct HizBox {
  bool valid = false;
  float uv_min[2]{0, 0}, uv_max[2]{1, 1};
  float z_near = 0;
  float lod = 0;
};
HizBox hiz_project_box(const float prev_vp[16], const float box_min[3], const float box_max[3], uint32_t hiz_w,
                       uint32_t hiz_h, bool reverse_z);
bool hiz_is_occluded(float z_near, float hiz_sample, bool reverse_z);
float hiz_reduce(float a, float b, float c, float d, bool reverse_z);

// The shader's rule, on the CPU.
bool draw_visible(const DrawGpu& d, const CullView& view);

struct IndirectCommand {  // VkDrawIndexedIndirectCommand
  uint32_t index_count, instance_count, first_index;
  int32_t vertex_offset;
  uint32_t first_instance;
};
static_assert(sizeof(IndirectCommand) == 20);

// The commands the shader writes per batch for these draws (order within a
// batch is the shader's atomic order; here, table order), and the counts.
struct IndirectBatches {
  std::array<std::vector<IndirectCommand>, kBatchCount> commands;
  uint32_t visible() const {
    uint32_t n = 0;
    for (const auto& c : commands) n += uint32_t(c.size());
    return n;
  }
};
IndirectBatches build_indirect(const std::vector<DrawGpu>& table, const CullView& view, uint32_t max_per_batch);

// ---- Per-prop-instance smashable index-masking tracker (pure C++, unit-tested) ----
struct PropSpan {
  float gx = 0.0f, gy = 0.0f, gz = 0.0f;  // prop placement origin (game frame)
  float bmin[3] = {0.0f, 0.0f, 0.0f};     // world-space AABB (game frame)
  float bmax[3] = {0.0f, 0.0f, 0.0f};
  uint32_t rel_index_offset = 0;          // offset within this draw's index range
  uint32_t index_count = 0;
  bool hidden = false;
  std::vector<uint32_t> saved_indices;    // original indices for lossless Rewind restore
};

struct HiddenSphereRec {
  std::array<float, 3> pos{};
  float radius = 2.0f;
  bool persistent = false;
  bool origin_matched = false;
  bool used_aabb_fallback = false;
};

bool prop_span_matches_origin(const PropSpan& sp, const std::array<float, 3>& game_pos, float radius_m);
bool prop_span_matches_aabb_fallback(const PropSpan& sp, const std::array<float, 3>& game_pos, float pad_m);
bool prop_span_matches_same_instance(const PropSpan& sp, const std::array<float, 3>& origin);

struct SmashHideTracker {
  std::unordered_map<uint64_t, std::vector<PropSpan>> by_draw_key;
  std::vector<HiddenSphereRec> active_spheres;

  void prune_non_resident(const std::function<bool(uint64_t)>& is_resident);

  void install_spans(std::vector<std::pair<uint64_t, std::vector<PropSpan>>>& prop_spans,
                     const std::unordered_set<uint64_t>& uploaded_keys,
                     const std::function<bool(uint64_t)>& is_resident,
                     const std::function<bool(uint64_t, uint32_t&)>& lookup_index_base,
                     const std::function<void(uint32_t, uint32_t)>& queue_zero);

  size_t hide_near(const std::array<float, 3>& game_pos, float radius_m, bool persistent,
                   const std::vector<uint64_t>& resident_draw_keys,
                   const std::function<bool(uint64_t, uint32_t&)>& lookup_index_base,
                   const std::function<void(uint32_t, uint32_t)>& queue_zero);

  size_t unhide_near(const std::array<float, 3>& game_pos, float radius_m,
                     const std::vector<uint64_t>& resident_draw_keys,
                     const std::function<bool(uint64_t, uint32_t&)>& lookup_index_base,
                     const std::function<void(uint32_t, const std::vector<uint32_t>&)>& queue_restore);

  bool box_overlaps_persistent_sphere(const float box_min[3], const float box_max[3],
                                      double origin_x, double origin_z) const;
};

}  // namespace carview::gpuworld
