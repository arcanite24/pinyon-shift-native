// Clustered Forward+ 3D Froxel Light Culling (16x9x24 = 3,456 froxels, up to 256
// dynamic point & spot lights) running on QueueKind::AsyncCompute via rhi::RenderGraph.
// Replaces legacy fixed 16-headlight + 6-point-light brute-force loops across
// Vulkan 1.3, DirectX 12, and Metal 3.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "rhi/render_graph.h"
#include "rhi/rhi.h"

namespace shift::rhi {

inline constexpr uint32_t kClusterGridX = 16u;
inline constexpr uint32_t kClusterGridY = 9u;
inline constexpr uint32_t kClusterGridZ = 24u;
inline constexpr uint32_t kClusterCount = kClusterGridX * kClusterGridY * kClusterGridZ;
inline constexpr uint32_t kMaxClusterLights = 256u;
inline constexpr uint32_t kMaxLightsPerCluster = 32u;
inline constexpr uint32_t kMaxClusterLightIndices = kClusterCount * kMaxLightsPerCluster;

enum class ClusterLightType : uint32_t {
  Point = 0,
  Spot = 1,
};

// 64-byte GPU light record (world-space pos/dir + attenuation & cone parameters).
struct alignas(16) ClusterLightGpu {
  float pos_radius[4] = {0.0f, 0.0f, 0.0f, 10.0f};       // xyz = world pos, w = radius
  float color_intensity[4] = {1.0f, 1.0f, 1.0f, 1.0f};   // rgb = linear color, w = intensity
  float dir_cos_outer[4] = {0.0f, 0.0f, -1.0f, -1.0f};   // xyz = spot dir, w = cos(outer_angle)
  float cos_inner = -1.0f;                                // cos(inner_angle)
  uint32_t type = 0;                                      // 0 = Point, 1 = Spot
  uint32_t pad[2] = {0, 0};
};
static_assert(sizeof(ClusterLightGpu) == 64, "ClusterLightGpu must be 64 bytes");

// 8-byte per-froxel cell header (offset into light_indices + active light count).
struct ClusterCellGpu {
  uint32_t offset = 0;
  uint32_t count = 0;
};
static_assert(sizeof(ClusterCellGpu) == 8, "ClusterCellGpu must be 8 bytes");

// 160-byte uniform block uploaded each frame for async-compute froxel culling.
struct alignas(16) ClusterCullParamsGpu {
  float view[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float inv_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float z_near = 0.1f;
  float z_far = 1000.0f;
  uint32_t light_count = 0;
  uint32_t grid_x = kClusterGridX;
  uint32_t grid_y = kClusterGridY;
  uint32_t grid_z = kClusterGridZ;
  uint32_t max_lights_per_cluster = kMaxLightsPerCluster;
  uint32_t pad = 0;
};
static_assert(sizeof(ClusterCullParamsGpu) == 160, "ClusterCullParamsGpu must be 160 bytes");

struct ClusteredLightsView {
  RGBufferHandle light_buffer;
  RGBufferHandle cell_buffer;
  RGBufferHandle index_buffer;
};

class ClusteredLightSystem {
 public:
  explicit ClusteredLightSystem(Device& device);
  ~ClusteredLightSystem();

  ClusteredLightSystem(const ClusteredLightSystem&) = delete;
  ClusteredLightSystem& operator=(const ClusteredLightSystem&) = delete;

  bool init(std::string& error);
  void shutdown();
  bool ready() const noexcept { return ready_; }

  // Uploads active lights + camera view/inv_proj and adds an AsyncCompute froxel
  // culling pass to `graph`. Returns RenderGraph handles for downstream shading passes.
  ClusteredLightsView add_cull_pass(RenderGraph& graph,
                                    const ClusterCullParamsGpu& params,
                                    std::span<const ClusterLightGpu> lights);

  BufferHandle light_buffer_handle() const noexcept { return light_buf_; }
  BufferHandle cell_buffer_handle() const noexcept { return cell_buf_; }
  BufferHandle index_buffer_handle() const noexcept { return index_buf_; }

 private:
  Device& device_;
  bool ready_ = false;

  BindGroupLayoutHandle bgl_{};
  PipelineLayoutHandle pl_{};
  PipelineHandle pipe_{};
  BindGroupHandle bg_{};

  BufferHandle params_buf_{};
  BufferHandle light_stage_buf_{};
  BufferHandle light_buf_{};
  BufferHandle cell_buf_{};
  BufferHandle index_buf_{};
};

}  // namespace shift::rhi
