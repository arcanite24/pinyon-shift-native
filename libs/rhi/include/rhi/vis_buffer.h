// Complete Visibility Buffer + 8x8 Tile Material Classifier + Specialized
// Compute Material Shading + Clustered Forward+ Lighting pipeline built on
// libs/rhi + rhi::RenderGraph across Vulkan 1.3, DirectX 12, and Metal 3.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rhi/clustered_lights.h"
#include "rhi/meshlet.h"
#include "rhi/render_graph.h"
#include "rhi/rhi.h"

namespace shift::rhi {

struct VisSceneUpload {
  std::span<const VisVertexGpu> vertices;
  std::span<const uint32_t> indices;
  std::span<const MeshletGpu> meshlets;
  std::span<const MeshletInstanceGpu> instances;
  std::span<const VisMaterialGpu> materials;
};

struct VisFrameView {
  uint32_t width = 0;
  uint32_t height = 0;
  float view[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float inv_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float view_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float prev_view_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float camera_pos[3] = {0.0f, 0.0f, 0.0f};
  float z_near = 0.1f;
  float z_far = 500.0f;
  float sun_dir[3] = {0.4f, 0.8f, 0.4f};
  float sun_color[3] = {1.0f, 0.96f, 0.9f};
  float sun_intensity = 3.0f;
  float sky_clear[4] = {0.08f, 0.14f, 0.24f, 1.0f};
  bool enable_cone_cull = true;
};

struct VisFrameOutputs {
  RGTextureHandle vis_buffer;     // Rg32Uint 64-bit Visibility Buffer
  RGTextureHandle depth_buffer;   // D32Sfloat Reverse-Z depth
  RGTextureHandle hdr_color;      // Rgba16Sfloat linear HDR lit output
  RGTextureHandle thin_gbuf;      // Rgba16Sfloat (oct_normal.xy, roughness, metallic)
  RGTextureHandle velocity;       // Rg16Sfloat per-pixel screen motion vectors
  RGBufferHandle cull_stats;      // uint32[4]: visible, frustum_culled, cone_culled, tested
  RGBufferHandle tile_indirect;   // uint32[kVisMaterialClassCount * 3] dispatch counts
};

struct VisTelemetry {
  uint32_t tested_meshlets = 0;
  uint32_t visible_meshlets = 0;
  uint32_t frustum_culled = 0;
  uint32_t cone_culled = 0;
  std::array<uint32_t, kVisMaterialClassCount> active_tiles_per_class{};
};

class VisBufferPipeline {
 public:
  explicit VisBufferPipeline(Device& device);
  ~VisBufferPipeline();

  VisBufferPipeline(const VisBufferPipeline&) = delete;
  VisBufferPipeline& operator=(const VisBufferPipeline&) = delete;

  bool init(uint32_t max_width, uint32_t max_height, std::string& error,
            uint32_t max_draws = 65536u);
  void shutdown();
  bool ready() const noexcept { return ready_; }

  // Uploads static/dynamic scene meshlets, vertices, indices, instances & materials.
  bool upload_scene(const VisSceneUpload& scene, std::string& error);

  // Schedules the full multi-queue Visibility Buffer frame into `graph`:
  //   1. ClusterLightUpload [Transfer] + ClusterLightCull [AsyncCompute]
  //   2. VisCull [AsyncCompute] (frustum + normal-cone -> DrawIndexedIndirectCmds)
  //   3. VisRaster [Graphics] (draw_indexed_indirect_count -> Rg32Uint VisBuf + D32F + clear outputs)
  //   4. VisTileClassify [AsyncCompute] (8x8 LDS tile bitmask -> per-material IndirectDispatchArgs)
  //   5. VisMaterialShade [AsyncCompute] (dispatch_indirect per material class -> HDR + G-Buf + Velocity)
  VisFrameOutputs add_frame_passes(RenderGraph& graph,
                                   const VisFrameView& view,
                                   std::span<const ClusterLightGpu> lights,
                                   std::string& error);

  // Reads back GPU cull & tile classification telemetry after graph execution completes.
  bool readback_telemetry(VisTelemetry& out, std::string& error);

  // Direct handle accessors for persistent render outputs.
  TextureHandle hdr_texture() const noexcept { return hdr_tex_; }
  TextureHandle gbuf_texture() const noexcept { return gbuf_tex_; }
  TextureHandle velocity_texture() const noexcept { return vel_tex_; }
  TextureHandle vis_texture() const noexcept { return vis_tex_; }
  TextureHandle depth_texture() const noexcept { return depth_tex_; }
  TextureViewHandle hdr_view() const noexcept { return hdr_view_; }
  TextureViewHandle gbuf_view() const noexcept { return gbuf_view_; }
  TextureViewHandle velocity_view() const noexcept { return vel_view_; }
  TextureViewHandle vis_view() const noexcept { return vis_view_; }
  TextureViewHandle depth_view() const noexcept { return depth_view_; }

 private:
  Device& device_;
  ClusteredLightSystem lights_;
  bool ready_ = false;

  uint32_t max_width_ = 0;
  uint32_t max_height_ = 0;
  uint32_t max_tiles_ = 0;
  uint32_t max_draws_ = 65536u;
  uint32_t work_item_count_ = 0;

  // Pipelines & layouts
  BindGroupLayoutHandle cull_bgl_{}, raster_bgl_{}, classify_bgl_{}, shade_bgl_{};
  PipelineLayoutHandle cull_pl_{}, raster_pl_{}, classify_pl_{}, shade_pl_{};
  PipelineHandle cull_pipe_{}, raster_pipe_{}, classify_pipe_{}, shade_pipe_{};
  BindGroupHandle cull_bg_{}, raster_bg_{}, classify_bg_{}, shade_bg_{};

  // Output / intermediate textures (fixed max-resolution for stall-free DRS)
  TextureHandle vis_tex_{}, depth_tex_{}, hdr_tex_{}, gbuf_tex_{}, vel_tex_{};
  TextureViewHandle vis_view_{}, depth_view_{}, hdr_view_{}, gbuf_view_{}, vel_view_{};

  // Uniforms & GPU-driven indirect buffers
  BufferHandle cull_ubo_{}, raster_ubo_{}, classify_ubo_{}, shade_ubo_{};
  BufferHandle draw_id_vb_{};
  BufferHandle draw_cmd_buf_{}, visible_draw_buf_{}, cull_stats_buf_{};
  BufferHandle tile_indirect_buf_{}, tile_list_buf_{};
  BufferHandle telemetry_rb_buf_{};

  // Scene buffers (grown as needed in upload_scene)
  BufferHandle vertex_buf_{}, index_buf_{}, meshlet_buf_{}, instance_buf_{}, material_buf_{}, work_item_buf_{};
};

}  // namespace shift::rhi
