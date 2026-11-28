#include "rhi/clustered_lights.h"

#include <algorithm>
#include <cstring>

#include "rhi_modern_shaders_hlsl_msl.h"
#include "rhi_modern_shaders_spv.h"

namespace shift::rhi {

ClusteredLightSystem::ClusteredLightSystem(Device& device) : device_(device) {}

ClusteredLightSystem::~ClusteredLightSystem() {
  shutdown();
}

bool ClusteredLightSystem::init(std::string& error) {
  shutdown();

  BindGroupLayoutBinding bindings[4]{};
  bindings[0] = {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};
  bindings[1] = {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute};
  bindings[2] = {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute};
  bindings[3] = {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute};

  bgl_ = device_.create_bind_group_layout({bindings, false}, error);
  if (!bgl_) return false;

  pl_ = device_.create_pipeline_layout({std::span(&bgl_, 1), {}}, error);
  if (!pl_) return false;

  ComputePipelineDesc cpd{};
  cpd.layout = pl_;
  cpd.compute_shader = shaders::make_shader(
      device_.backend(), ShaderStage::Compute,
      std::span<const uint32_t>(shaders::kLightClusterCompSpv,
                                std::size(shaders::kLightClusterCompSpv)),
      shaders::kLightClusterCompHlsl, shaders::kLightClusterCompMsl);
  pipe_ = device_.create_compute_pipeline(cpd, error);
  if (!pipe_) return false;

  params_buf_ = device_.create_buffer(
      {sizeof(ClusterCullParamsGpu),
       BufferUsage::Uniform | BufferUsage::TransferDst,
       MemoryHint::CpuToGpu,
       true,
       "cluster_params"},
      error);
  light_stage_buf_ = device_.create_buffer(
      {static_cast<uint64_t>(kMaxClusterLights) * sizeof(ClusterLightGpu),
       BufferUsage::TransferSrc,
       MemoryHint::CpuToGpu,
       true,
       "cluster_lights_stage"},
      error);
  light_buf_ = device_.create_buffer(
      {static_cast<uint64_t>(kMaxClusterLights) * sizeof(ClusterLightGpu),
       BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc,
       MemoryHint::GpuOnly,
       false,
       "cluster_lights_gpu"},
      error);
  cell_buf_ = device_.create_buffer(
      {static_cast<uint64_t>(kClusterCount) * sizeof(ClusterCellGpu),
       BufferUsage::Storage | BufferUsage::TransferSrc,
       MemoryHint::GpuOnly,
       false,
       "cluster_cells_gpu"},
      error);
  index_buf_ = device_.create_buffer(
      {static_cast<uint64_t>(kMaxClusterLightIndices) * sizeof(uint32_t),
       BufferUsage::Storage | BufferUsage::TransferSrc,
       MemoryHint::GpuOnly,
       false,
       "cluster_indices_gpu"},
      error);
  if (!params_buf_ || !light_stage_buf_ || !light_buf_ || !cell_buf_ || !index_buf_) {
    shutdown();
    return false;
  }

  BindGroupEntry entries[4]{};
  entries[0] = {0, 0, DescriptorType::UniformBuffer, params_buf_};
  entries[1] = {1, 0, DescriptorType::StorageBuffer, light_buf_};
  entries[2] = {2, 0, DescriptorType::StorageBuffer, cell_buf_};
  entries[3] = {3, 0, DescriptorType::StorageBuffer, index_buf_};
  bg_ = device_.create_bind_group({bgl_, entries}, error);
  if (!bg_) {
    shutdown();
    return false;
  }

  ready_ = true;
  return true;
}

void ClusteredLightSystem::shutdown() {
  ready_ = false;
  if (bg_) { device_.destroy_bind_group(bg_); bg_ = {}; }
  if (index_buf_) { device_.destroy_buffer(index_buf_); index_buf_ = {}; }
  if (cell_buf_) { device_.destroy_buffer(cell_buf_); cell_buf_ = {}; }
  if (light_buf_) { device_.destroy_buffer(light_buf_); light_buf_ = {}; }
  if (light_stage_buf_) { device_.destroy_buffer(light_stage_buf_); light_stage_buf_ = {}; }
  if (params_buf_) { device_.destroy_buffer(params_buf_); params_buf_ = {}; }
  if (pipe_) { device_.destroy_pipeline(pipe_); pipe_ = {}; }
  if (pl_) { device_.destroy_pipeline_layout(pl_); pl_ = {}; }
  if (bgl_) { device_.destroy_bind_group_layout(bgl_); bgl_ = {}; }
}

ClusteredLightsView ClusteredLightSystem::add_cull_pass(
    RenderGraph& graph,
    const ClusterCullParamsGpu& params,
    std::span<const ClusterLightGpu> lights) {
  ClusteredLightsView out{};
  if (!ready_) return out;

  const uint32_t count = static_cast<uint32_t>(std::min<size_t>(lights.size(), kMaxClusterLights));
  ClusterCullParamsGpu u = params;
  u.light_count = count;
  u.grid_x = kClusterGridX;
  u.grid_y = kClusterGridY;
  u.grid_z = kClusterGridZ;
  u.max_lights_per_cluster = kMaxLightsPerCluster;

  if (void* p = device_.map_buffer(params_buf_)) {
    std::memcpy(p, &u, sizeof(u));
  }
  if (count > 0) {
    if (void* lp = device_.map_buffer(light_stage_buf_)) {
      std::memcpy(lp, lights.data(), static_cast<size_t>(count) * sizeof(ClusterLightGpu));
    }
  }

  RGBufferHandle rg_stage =
      graph.import_buffer("cluster_lights_stage", light_stage_buf_, ResourceState::CopySrc);
  out.light_buffer =
      graph.import_buffer("cluster_lights", light_buf_, ResourceState::Common);
  out.cell_buffer =
      graph.import_buffer("cluster_cells", cell_buf_, ResourceState::Common);
  out.index_buffer =
      graph.import_buffer("cluster_indices", index_buf_, ResourceState::Common);

  const uint64_t upload_bytes =
      std::max<uint64_t>(static_cast<uint64_t>(count) * sizeof(ClusterLightGpu),
                         sizeof(ClusterLightGpu));

  graph.add_pass(
      "ClusterLightUpload", QueueKind::Transfer,
      [&](RenderGraphBuilder& b) {
        b.read_buffer(rg_stage, ResourceState::CopySrc);
        b.write_buffer(out.light_buffer, ResourceState::CopyDst);
      },
      [this, upload_bytes](CommandList& cmd, const RenderGraphContext&) {
        cmd.copy_buffer(light_stage_buf_, 0, light_buf_, 0, upload_bytes);
      });

  graph.add_pass(
      "ClusterLightCull", QueueKind::AsyncCompute,
      [&](RenderGraphBuilder& b) {
        b.read_buffer(out.light_buffer, ResourceState::ShaderRead);
        b.write_buffer(out.cell_buffer, ResourceState::StorageReadWrite);
        b.write_buffer(out.index_buffer, ResourceState::StorageReadWrite);
      },
      [this](CommandList& cmd, const RenderGraphContext&) {
        cmd.bind_pipeline(pipe_);
        cmd.bind_group(0, bg_);
        const uint32_t gx = (kClusterGridX + 7u) / 8u;
        const uint32_t gy = (kClusterGridY + 3u) / 4u;
        const uint32_t gz = (kClusterGridZ + 1u) / 2u;
        cmd.dispatch(gx, gy, gz);
      });

  return out;
}

}  // namespace shift::rhi
