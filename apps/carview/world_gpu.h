// The GPU-driven world (Slice 11): the scenery's draws live in persistent
// vertex and index arenas, their textures in one bindless array, and a
// compute pass culls the table of draws + hardware-instanced tree sprites
// against frustum, LOD band, and reprojected Hi-Z depth into indirect draws.
#pragma once

#include <volk.h>

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "renderer.h"
#include "world_gpu_logic.h"

namespace carview {

class WorldGpu {
 public:
  static constexpr uint32_t kMaxTextures = 4096;
  static constexpr uint32_t kMaxPerBatch = 8192;  // indirect commands per batch
  static constexpr uint32_t kMaxTrees = 131072;   // max hardware-instanced tree sprites
  static constexpr uint32_t kHizWidth = 512;
  static constexpr uint32_t kHizHeight = 256;
  static constexpr uint32_t kHizMips = 10;        // 512x256 down to 1x1
  static constexpr uint32_t kDefaultWhite = 0, kDefaultBlack = 1, kDefaultFlatNormal = 2;

  explicit WorldGpu(Renderer& r) : r_(r) {}
  WorldGpu(const WorldGpu&) = delete;
  WorldGpu& operator=(const WorldGpu&) = delete;
  ~WorldGpu();

  struct Setup {
    VkDescriptorSetLayout frame_layout = VK_NULL_HANDLE;  // set 0, as the world's other pipelines use
    VkSampler sampler = VK_NULL_HANDLE;                   // the world's texture sampler
    VkImageView white = VK_NULL_HANDLE, black = VK_NULL_HANDLE, flat_normal = VK_NULL_HANDLE;
    // The arenas' size in vertices (48 bytes each) and indices.
    uint32_t vertex_capacity = 0, index_capacity = 0;
    // Retire: run once the frames submitted so far are done (Showroom::retire).
    std::function<void(std::function<void()>)> retire;
  };
  bool init(const Setup& setup, std::string& error);
  bool ready() const { return ready_; }

  // ---- the arenas (device-local, one buffer each) ----
  VkBuffer vertex_buffer() const { return vertices_.buffer; }
  VkBuffer index_buffer() const { return indices_.buffer; }
  gpuworld::ResidentSet& resident() { return resident_; }
  struct Copy {
    VkDeviceSize src, dst, size;
  };
  void record_copies(VkCommandBuffer cmd, VkBuffer staged_vertices, const std::vector<Copy>& vertex_copies,
                     VkBuffer staged_indices, const std::vector<Copy>& index_copies);

  // ---- bindless textures ----
  uint32_t texture_slot(uint32_t record) const;
  bool has_texture(uint32_t record) const;
  uint32_t add_texture(uint32_t record, VkImageView view);
  void remove_texture(uint32_t record);
  size_t texture_count() const { return texture_slots_.size(); }

  // ---- the table of draws and hardware-instanced tree sprites ----
  bool set_table(const std::vector<gpuworld::DrawGpu>& rows, std::string& error,
                 const std::vector<gpuworld::TreeInstanceGpu>& trees = {});
  uint32_t table_rows() const { return generation_.rows; }
  uint32_t tree_count() const { return generation_.trees_count; }

  // ---- per frame ----
  void record_cull(VkCommandBuffer cmd, const gpuworld::CullView& view);
  struct DrawParams {
    VkDescriptorSet frame_set = VK_NULL_HANDLE;
    float model[16];
    float sun[3];
    float up[4];       // xyz, w exposure
    float swap = 2.0f; // the incoming draws' fade progress (2: none)
    float bias_sign = 1.0f;
    float decal_bias_constant = 64.0f, decal_bias_slope = 1.0f;
    bool depth_prepass = false;
  };
  void record_z_prepass(VkCommandBuffer cmd, const DrawParams& p);
  void record_draw(VkCommandBuffer cmd, const DrawParams& p);
  void record_hiz(VkCommandBuffer cmd, VkImageView depth_view, uint32_t depth_w, uint32_t depth_h, bool reverse_z);
  void invalidate_hiz() { hiz_valid_ = false; }
  bool hiz_valid() const { return hiz_valid_; }

  uint32_t last_tested() const { return last_tested_; }
  uint32_t last_drawn() const { return last_drawn_; }
  uint32_t last_hiz_culled() const { return last_hiz_culled_; }
  uint32_t last_trees_drawn() const { return last_trees_drawn_; }
  uint32_t resident_draws() const { return uint32_t(resident_.count()); }
  VkImageView hiz_image_view() const { return hiz_valid_ ? hiz_image_.view : VK_NULL_HANDLE; }
  VkDeviceSize arena_bytes() const;

 private:
  bool create_pipelines(std::string& error);
  bool create_buffers(std::string& error);
  bool create_hiz(std::string& error);

  struct Generation {
    Renderer::Buffer table;
    Renderer::Buffer trees;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t rows = 0;
    uint32_t trees_count = 0;
  };
  void destroy_generation(Generation& g);
  struct SlotBuffers {
    Renderer::Buffer commands, counts, stats, cull_ubo;
    Renderer::Buffer tree_vis, tree_cmd, tree_stats;
    void* stats_mapped = nullptr;
    void* tree_stats_mapped = nullptr;
    VkDescriptorPool pool = VK_NULL_HANDLE;      // reset each frame (cull + hiz sets)
    VkDescriptorSet vis_set = VK_NULL_HANDLE;    // set 3 (tree visible_indices)
    bool used = false;
  };

  Renderer& r_;
  Setup setup_;
  bool ready_ = false;
  Renderer::Buffer vertices_, indices_, empty_ssbo_;
  gpuworld::ResidentSet resident_;
  VkDescriptorSetLayout bindless_layout_ = VK_NULL_HANDLE, table_layout_ = VK_NULL_HANDLE,
                        vis_layout_ = VK_NULL_HANDLE, cull_layout_ = VK_NULL_HANDLE,
                        hiz_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool bindless_pool_ = VK_NULL_HANDLE, table_pool_ = VK_NULL_HANDLE, vis_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;
  std::unordered_map<uint32_t, uint32_t> texture_slots_;
  std::vector<uint32_t> free_slots_;
  uint32_t next_slot_ = 3;
  VkPipelineLayout layout_ = VK_NULL_HANDLE, cull_pipeline_layout_ = VK_NULL_HANDLE,
                   hiz_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline opaque_ = VK_NULL_HANDLE, opaque_after_z_ = VK_NULL_HANDLE, z_prepass_ = VK_NULL_HANDLE,
             coverage_ = VK_NULL_HANDLE, tree_coverage_ = VK_NULL_HANDLE,
             cull_ = VK_NULL_HANDLE, tree_cull_ = VK_NULL_HANDLE, hiz_gen_ = VK_NULL_HANDLE;
  Renderer::Image hiz_image_;
  std::array<VkImageView, kHizMips> hiz_mip_views_{};
  VkSampler hiz_sampler_ = VK_NULL_HANDLE;
  bool hiz_valid_ = false;
  Generation generation_;
  SlotBuffers slots_[Renderer::kFramesInFlight];
  uint32_t last_tested_ = 0, last_drawn_ = 0, last_hiz_culled_ = 0, last_trees_drawn_ = 0;
  uint32_t recorded_slot_ = 0;
  mutable std::mutex mutex_;
};

}  // namespace carview
