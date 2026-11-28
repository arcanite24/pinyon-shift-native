#include "rhi/vis_buffer.h"

#include <algorithm>
#include <cstring>

#include "rhi_modern_shaders_hlsl_msl.h"
#include "rhi_modern_shaders_spv.h"

namespace shift::rhi {
namespace {

struct alignas(16) CullUboGpu {
  float view_proj[16];
  float frustum_planes[6][4];
  float camera_pos_pad[4];
  uint32_t work_item_count;
  uint32_t max_draws;
  uint32_t enable_cone_cull;
  uint32_t pad;
};
static_assert(sizeof(CullUboGpu) == 192);

struct alignas(16) RasterUboGpu {
  float view_proj[16];
  float prev_view_proj[16];
};
static_assert(sizeof(RasterUboGpu) == 128);

struct alignas(16) ClassifyUboGpu {
  uint32_t width;
  uint32_t height;
  uint32_t tiles_x;
  uint32_t tiles_y;
  uint32_t max_tiles_per_class;
  uint32_t material_class_count;
  uint32_t pad0;
  uint32_t pad1;
};
static_assert(sizeof(ClassifyUboGpu) == 32);

struct alignas(16) ShadeUboGpu {
  float view[16];
  float view_proj[16];
  float prev_view_proj[16];
  float camera_pos_znear[4];
  float sun_dir_zfar[4];
  float sun_color_intensity[4];
  uint32_t viewport_grid[4];
  uint32_t grid_z_pad[4];
};
static_assert(sizeof(ShadeUboGpu) == 272);

struct ShadePushConstants {
  uint32_t active_material_class = 0;
  uint32_t max_tiles_per_class = 0;
};

}  // namespace

VisBufferPipeline::VisBufferPipeline(Device& device) : device_(device), lights_(device) {}

VisBufferPipeline::~VisBufferPipeline() {
  shutdown();
}

bool VisBufferPipeline::init(uint32_t max_width, uint32_t max_height, std::string& error,
                             uint32_t max_draws) {
  shutdown();
  if (max_width == 0 || max_height == 0 || max_draws == 0) {
    error = "VisBufferPipeline::init: invalid dimensions or max_draws";
    return false;
  }

  max_width_ = max_width;
  max_height_ = max_height;
  max_draws_ = max_draws;
  const uint32_t tiles_x = (max_width_ + 7u) / 8u;
  const uint32_t tiles_y = (max_height_ + 7u) / 8u;
  max_tiles_ = std::max(tiles_x * tiles_y, 1u);

  if (!lights_.init(error)) return false;

  const BackendKind bk = device_.backend();

  // -------------------------------------------------------------------------
  // 1. Allocate fixed max-resolution output & intermediate textures (DRS-ready)
  // -------------------------------------------------------------------------
  auto make_tex = [&](const char* name, Format fmt, TextureUsage usage,
                      TextureHandle& th, TextureViewHandle& vh) -> bool {
    TextureDesc td{};
    td.format = fmt;
    td.width = max_width_;
    td.height = max_height_;
    td.usage = usage;
    td.memory = MemoryHint::Transient;
    td.debug_name = name;
    th = device_.create_texture(td, error);
    if (!th) return false;
    TextureViewDesc vd{};
    vd.texture = th;
    vd.format = fmt;
    vh = device_.create_texture_view(vd, error);
    return vh.valid();
  };

  if (!make_tex("vis_buffer", Format::Rg32Uint,
                TextureUsage::ColorAttachment | TextureUsage::Storage |
                    TextureUsage::Sampled | TextureUsage::TransferSrc,
                vis_tex_, vis_view_) ||
      !make_tex("vis_depth", Format::D32Sfloat,
                TextureUsage::DepthAttachment | TextureUsage::Sampled,
                depth_tex_, depth_view_) ||
      !make_tex("vis_hdr", Format::Rgba16Sfloat,
                TextureUsage::ColorAttachment | TextureUsage::Storage |
                    TextureUsage::Sampled | TextureUsage::TransferSrc,
                hdr_tex_, hdr_view_) ||
      !make_tex("vis_gbuf", Format::Rgba16Sfloat,
                TextureUsage::ColorAttachment | TextureUsage::Storage |
                    TextureUsage::Sampled | TextureUsage::TransferSrc,
                gbuf_tex_, gbuf_view_) ||
      !make_tex("vis_velocity", Format::Rg16Sfloat,
                TextureUsage::ColorAttachment | TextureUsage::Storage |
                    TextureUsage::Sampled | TextureUsage::TransferSrc,
                vel_tex_, vel_view_)) {
    shutdown();
    return false;
  }

  // -------------------------------------------------------------------------
  // 2. Allocate uniform, draw-id VB, indirect draw & tile buffers
  // -------------------------------------------------------------------------
  cull_ubo_ = device_.create_buffer(
      {sizeof(CullUboGpu), BufferUsage::Uniform, MemoryHint::CpuToGpu, true, "vis_cull_ubo"}, error);
  raster_ubo_ = device_.create_buffer(
      {sizeof(RasterUboGpu), BufferUsage::Uniform, MemoryHint::CpuToGpu, true, "vis_raster_ubo"}, error);
  classify_ubo_ = device_.create_buffer(
      {sizeof(ClassifyUboGpu), BufferUsage::Uniform, MemoryHint::CpuToGpu, true, "vis_classify_ubo"}, error);
  shade_ubo_ = device_.create_buffer(
      {sizeof(ShadeUboGpu), BufferUsage::Uniform, MemoryHint::CpuToGpu, true, "vis_shade_ubo"}, error);

  draw_cmd_buf_ = device_.create_buffer(
      {static_cast<uint64_t>(max_draws_) * sizeof(DrawIndexedIndirectCmd),
       BufferUsage::Storage | BufferUsage::Indirect | BufferUsage::TransferDst,
       MemoryHint::GpuOnly, false, "vis_draw_cmds"}, error);
  visible_draw_buf_ = device_.create_buffer(
      {static_cast<uint64_t>(max_draws_) * sizeof(VisibleMeshletDrawGpu),
       BufferUsage::Storage | BufferUsage::TransferDst,
       MemoryHint::GpuOnly, false, "vis_visible_draws"}, error);
  cull_stats_buf_ = device_.create_buffer(
      {16ull, BufferUsage::Storage | BufferUsage::Indirect | BufferUsage::TransferDst | BufferUsage::TransferSrc,
       MemoryHint::GpuOnly, false, "vis_cull_stats"}, error);

  const uint64_t tile_indirect_bytes = static_cast<uint64_t>(kVisMaterialClassCount) * 12ull;
  tile_indirect_buf_ = device_.create_buffer(
      {tile_indirect_bytes,
       BufferUsage::Storage | BufferUsage::Indirect | BufferUsage::TransferDst | BufferUsage::TransferSrc,
       MemoryHint::GpuOnly, false, "vis_tile_indirect"}, error);
  tile_list_buf_ = device_.create_buffer(
      {static_cast<uint64_t>(kVisMaterialClassCount) * max_tiles_ * sizeof(uint32_t),
       BufferUsage::Storage, MemoryHint::GpuOnly, false, "vis_tile_lists"}, error);
  telemetry_rb_buf_ = device_.create_buffer(
      {16ull + tile_indirect_bytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "vis_telemetry_rb"}, error);

  // Per-instance draw_id vertex buffer [0, 1, 2, ..., max_draws_ - 1] (portable across VK/DX12/Metal).
  const uint64_t draw_id_bytes = static_cast<uint64_t>(max_draws_) * sizeof(uint32_t);
  BufferHandle draw_id_stage = device_.create_buffer(
      {draw_id_bytes, BufferUsage::TransferSrc, MemoryHint::CpuToGpu, true, "vis_draw_id_stage"}, error);
  draw_id_vb_ = device_.create_buffer(
      {draw_id_bytes, BufferUsage::Vertex | BufferUsage::TransferDst, MemoryHint::GpuOnly, false, "vis_draw_id_vb"}, error);

  if (!cull_ubo_ || !raster_ubo_ || !classify_ubo_ || !shade_ubo_ ||
      !draw_cmd_buf_ || !visible_draw_buf_ || !cull_stats_buf_ ||
      !tile_indirect_buf_ || !tile_list_buf_ || !telemetry_rb_buf_ ||
      !draw_id_stage || !draw_id_vb_) {
    if (draw_id_stage) device_.destroy_buffer(draw_id_stage);
    shutdown();
    return false;
  }

  if (auto* ids = static_cast<uint32_t*>(device_.map_buffer(draw_id_stage))) {
    for (uint32_t i = 0; i < max_draws_; ++i) ids[i] = i;
  }
  const bool id_ok = device_.submit_once([&](CommandList& cmd) {
    cmd.copy_buffer(draw_id_stage, 0, draw_id_vb_, 0, draw_id_bytes);
    BufferBarrier bb{draw_id_vb_, ResourceState::CopyDst, ResourceState::VertexBuffer};
    cmd.barrier({{}, std::span(&bb, 1)});
  }, error);
  device_.destroy_buffer(draw_id_stage);
  if (!id_ok) {
    shutdown();
    return false;
  }

  // -------------------------------------------------------------------------
  // 3. Create BindGroupLayouts, PipelineLayouts & Pipelines
  // -------------------------------------------------------------------------
  // Pass A: VisCull
  BindGroupLayoutBinding cull_b[7]{
      {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute},
      {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
  };
  cull_bgl_ = device_.create_bind_group_layout({cull_b, false}, error);
  cull_pl_ = device_.create_pipeline_layout({std::span(&cull_bgl_, 1), {}}, error);
  if (!cull_bgl_ || !cull_pl_) { shutdown(); return false; }

  ComputePipelineDesc cull_cpd{};
  cull_cpd.layout = cull_pl_;
  cull_cpd.compute_shader = shaders::make_shader(
      bk, ShaderStage::Compute,
      std::span<const uint32_t>(shaders::kVisCullCompSpv, std::size(shaders::kVisCullCompSpv)),
      shaders::kVisCullCompHlsl, shaders::kVisCullCompMsl);
  cull_pipe_ = device_.create_compute_pipeline(cull_cpd, error);
  if (!cull_pipe_) { shutdown(); return false; }

  // Pass B: VisRaster (64-bit Rg32Uint + D32Sfloat Reverse-Z)
  BindGroupLayoutBinding raster_b[3]{
      {0, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex},
      {1, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex},
      {2, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex},
  };
  raster_bgl_ = device_.create_bind_group_layout({raster_b, false}, error);
  raster_pl_ = device_.create_pipeline_layout({std::span(&raster_bgl_, 1), {}}, error);
  if (!raster_bgl_ || !raster_pl_) { shutdown(); return false; }

  VertexBindingDesc vb_descs[2]{
      {0, sizeof(VisVertexGpu), false},
      {1, sizeof(uint32_t), true},
  };
  VertexAttributeDesc va_descs[2]{
      {0, 0, Format::Rgb32Sfloat, 0},
      {1, 1, Format::R32Uint, 0},
  };
  Format vis_fmt = Format::Rg32Uint;
  BlendState vis_blend = BlendState::opaque();

  GraphicsPipelineDesc gpd{};
  gpd.layout = raster_pl_;
  gpd.vertex_shader = shaders::make_shader(
      bk, ShaderStage::Vertex,
      std::span<const uint32_t>(shaders::kVisRasterVertSpv, std::size(shaders::kVisRasterVertSpv)),
      shaders::kVisRasterVertHlsl, shaders::kVisRasterVertMsl);
  gpd.fragment_shader = shaders::make_shader(
      bk, ShaderStage::Fragment,
      std::span<const uint32_t>(shaders::kVisRasterFragSpv, std::size(shaders::kVisRasterFragSpv)),
      shaders::kVisRasterFragHlsl, shaders::kVisRasterFragMsl);
  gpd.vertex_bindings = vb_descs;
  gpd.vertex_attributes = va_descs;
  gpd.color_formats = std::span(&vis_fmt, 1);
  gpd.color_blends = std::span(&vis_blend, 1);
  gpd.depth_format = Format::D32Sfloat;
  gpd.depth.test_enabled = true;
  gpd.depth.write_enabled = true;
  gpd.depth.compare_op = CompareOp::GreaterOrEqual;  // Reverse-Z
  // Vulkan clip.y flip in vis_raster.vert maps Y-up CCW to framebuffer CCW (negative area),
  // matching DirectX 12 and Metal FrontFace::CounterClockwise identically.
  gpd.raster.cull_mode = CullMode::Back;
  gpd.raster.front_face = FrontFace::CounterClockwise;
  raster_pipe_ = device_.create_graphics_pipeline(gpd, error);
  if (!raster_pipe_) { shutdown(); return false; }

  // Pass C: VisTileClassify
  BindGroupLayoutBinding classify_b[4]{
      {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute},
      {1, DescriptorType::StorageTexture, 1, ShaderStage::Compute},
      {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
  };
  classify_bgl_ = device_.create_bind_group_layout({classify_b, false}, error);
  classify_pl_ = device_.create_pipeline_layout({std::span(&classify_bgl_, 1), {}}, error);
  if (!classify_bgl_ || !classify_pl_) { shutdown(); return false; }

  ComputePipelineDesc cls_cpd{};
  cls_cpd.layout = classify_pl_;
  cls_cpd.compute_shader = shaders::make_shader(
      bk, ShaderStage::Compute,
      std::span<const uint32_t>(shaders::kVisClassifyCompSpv, std::size(shaders::kVisClassifyCompSpv)),
      shaders::kVisClassifyCompHlsl, shaders::kVisClassifyCompMsl);
  classify_pipe_ = device_.create_compute_pipeline(cls_cpd, error);
  if (!classify_pipe_) { shutdown(); return false; }

  BindGroupEntry classify_ents[4]{
      {0, 0, DescriptorType::UniformBuffer, classify_ubo_},
      {1, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, vis_view_},
      {2, 0, DescriptorType::StorageBuffer, tile_indirect_buf_},
      {3, 0, DescriptorType::StorageBuffer, tile_list_buf_},
  };
  classify_bg_ = device_.create_bind_group({classify_bgl_, classify_ents}, error);
  if (!classify_bg_) { shutdown(); return false; }

  // Pass D: VisMaterialShade (15 bindings + 8 B push constants)
  BindGroupLayoutBinding shade_b[15]{
      {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute},
      {1, DescriptorType::StorageTexture, 1, ShaderStage::Compute},
      {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {9, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {10, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {11, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
      {12, DescriptorType::StorageTexture, 1, ShaderStage::Compute},
      {13, DescriptorType::StorageTexture, 1, ShaderStage::Compute},
      {14, DescriptorType::StorageTexture, 1, ShaderStage::Compute},
  };
  shade_bgl_ = device_.create_bind_group_layout({shade_b, false}, error);
  PushConstantRange shade_pcr{ShaderStage::Compute, 0, sizeof(ShadePushConstants)};
  shade_pl_ = device_.create_pipeline_layout(
      {std::span(&shade_bgl_, 1), std::span(&shade_pcr, 1)}, error);
  if (!shade_bgl_ || !shade_pl_) { shutdown(); return false; }

  ComputePipelineDesc shd_cpd{};
  shd_cpd.layout = shade_pl_;
  shd_cpd.compute_shader = shaders::make_shader(
      bk, ShaderStage::Compute,
      std::span<const uint32_t>(shaders::kVisShadeCompSpv, std::size(shaders::kVisShadeCompSpv)),
      shaders::kVisShadeCompHlsl, shaders::kVisShadeCompMsl);
  shade_pipe_ = device_.create_compute_pipeline(shd_cpd, error);
  if (!shade_pipe_) { shutdown(); return false; }

  ready_ = true;
  return true;
}

void VisBufferPipeline::shutdown() {
  ready_ = false;
  auto destroy_bg = [&](BindGroupHandle& h) { if (h) { device_.destroy_bind_group(h); h = {}; } };
  auto destroy_buf = [&](BufferHandle& h) { if (h) { device_.destroy_buffer(h); h = {}; } };
  auto destroy_tv = [&](TextureViewHandle& h) { if (h) { device_.destroy_texture_view(h); h = {}; } };
  auto destroy_tex = [&](TextureHandle& h) { if (h) { device_.destroy_texture(h); h = {}; } };
  auto destroy_pipe = [&](PipelineHandle& h) { if (h) { device_.destroy_pipeline(h); h = {}; } };
  auto destroy_pl = [&](PipelineLayoutHandle& h) { if (h) { device_.destroy_pipeline_layout(h); h = {}; } };
  auto destroy_bgl = [&](BindGroupLayoutHandle& h) { if (h) { device_.destroy_bind_group_layout(h); h = {}; } };

  destroy_bg(shade_bg_);
  destroy_bg(classify_bg_);
  destroy_bg(raster_bg_);
  destroy_bg(cull_bg_);

  destroy_buf(work_item_buf_);
  destroy_buf(material_buf_);
  destroy_buf(instance_buf_);
  destroy_buf(meshlet_buf_);
  destroy_buf(index_buf_);
  destroy_buf(vertex_buf_);

  destroy_buf(telemetry_rb_buf_);
  destroy_buf(tile_list_buf_);
  destroy_buf(tile_indirect_buf_);
  destroy_buf(cull_stats_buf_);
  destroy_buf(visible_draw_buf_);
  destroy_buf(draw_cmd_buf_);
  destroy_buf(draw_id_vb_);
  destroy_buf(shade_ubo_);
  destroy_buf(classify_ubo_);
  destroy_buf(raster_ubo_);
  destroy_buf(cull_ubo_);

  destroy_tv(vel_view_);
  destroy_tv(gbuf_view_);
  destroy_tv(hdr_view_);
  destroy_tv(depth_view_);
  destroy_tv(vis_view_);
  destroy_tex(vel_tex_);
  destroy_tex(gbuf_tex_);
  destroy_tex(hdr_tex_);
  destroy_tex(depth_tex_);
  destroy_tex(vis_tex_);

  destroy_pipe(shade_pipe_);
  destroy_pipe(classify_pipe_);
  destroy_pipe(raster_pipe_);
  destroy_pipe(cull_pipe_);
  destroy_pl(shade_pl_);
  destroy_pl(classify_pl_);
  destroy_pl(raster_pl_);
  destroy_pl(cull_pl_);
  destroy_bgl(shade_bgl_);
  destroy_bgl(classify_bgl_);
  destroy_bgl(raster_bgl_);
  destroy_bgl(cull_bgl_);

  lights_.shutdown();
  work_item_count_ = 0;
}

bool VisBufferPipeline::upload_scene(const VisSceneUpload& scene, std::string& error) {
  if (!ready_) {
    error = "VisBufferPipeline::upload_scene: pipeline not initialized";
    return false;
  }
  if (scene.vertices.empty() || scene.indices.empty() || scene.meshlets.empty() ||
      scene.instances.empty() || scene.materials.empty()) {
    error = "VisBufferPipeline::upload_scene: empty scene arrays";
    return false;
  }

  device_.wait_idle();

  // Build flat (instance_id, meshlet_id) work items for vis_cull.comp.
  struct WorkItem { uint32_t inst_id, meshlet_id; };
  std::vector<WorkItem> items;
  for (uint32_t i = 0; i < static_cast<uint32_t>(scene.instances.size()); ++i) {
    const MeshletInstanceGpu& inst = scene.instances[i];
    for (uint32_t m = 0; m < inst.meshlet_count; ++m) {
      const uint32_t mid = inst.first_meshlet + m;
      if (mid < scene.meshlets.size()) {
        items.push_back({i, mid});
      }
    }
  }
  if (items.empty()) {
    error = "VisBufferPipeline::upload_scene: no valid instance meshlet work items";
    return false;
  }
  work_item_count_ = static_cast<uint32_t>(items.size());

  auto upload_buf = [&](BufferHandle& dst, const void* data, uint64_t bytes,
                        BufferUsage usage, const char* name) -> bool {
    if (dst) { device_.destroy_buffer(dst); dst = {}; }
    BufferHandle stage = device_.create_buffer(
        {bytes, BufferUsage::TransferSrc, MemoryHint::CpuToGpu, true, "vis_stage"}, error);
    dst = device_.create_buffer(
        {bytes, usage | BufferUsage::TransferDst, MemoryHint::GpuOnly, false, name}, error);
    if (!stage || !dst) {
      if (stage) device_.destroy_buffer(stage);
      return false;
    }
    if (void* p = device_.map_buffer(stage)) {
      std::memcpy(p, data, static_cast<size_t>(bytes));
    }
    const bool ok = device_.submit_once([&](CommandList& cmd) {
      cmd.copy_buffer(stage, 0, dst, 0, bytes);
      BufferBarrier bb{dst, ResourceState::CopyDst, ResourceState::Common};
      cmd.barrier({{}, std::span(&bb, 1)});
    }, error);
    device_.destroy_buffer(stage);
    return ok;
  };

  if (!upload_buf(vertex_buf_, scene.vertices.data(), scene.vertices.size_bytes(),
                  BufferUsage::Vertex | BufferUsage::Storage, "vis_vertices") ||
      !upload_buf(index_buf_, scene.indices.data(), scene.indices.size_bytes(),
                  BufferUsage::Index | BufferUsage::Storage, "vis_indices") ||
      !upload_buf(meshlet_buf_, scene.meshlets.data(), scene.meshlets.size_bytes(),
                  BufferUsage::Storage, "vis_meshlets") ||
      !upload_buf(instance_buf_, scene.instances.data(), scene.instances.size_bytes(),
                  BufferUsage::Storage, "vis_instances") ||
      !upload_buf(material_buf_, scene.materials.data(), scene.materials.size_bytes(),
                  BufferUsage::Storage, "vis_materials") ||
      !upload_buf(work_item_buf_, items.data(),
                  static_cast<uint64_t>(items.size()) * sizeof(WorkItem),
                  BufferUsage::Storage, "vis_work_items")) {
    return false;
  }

  // Rebuild bind groups referencing the uploaded scene buffers.
  if (cull_bg_) { device_.destroy_bind_group(cull_bg_); cull_bg_ = {}; }
  if (raster_bg_) { device_.destroy_bind_group(raster_bg_); raster_bg_ = {}; }
  if (shade_bg_) { device_.destroy_bind_group(shade_bg_); shade_bg_ = {}; }

  BindGroupEntry cull_ents[7]{
      {0, 0, DescriptorType::UniformBuffer, cull_ubo_},
      {1, 0, DescriptorType::StorageBuffer, instance_buf_},
      {2, 0, DescriptorType::StorageBuffer, meshlet_buf_},
      {3, 0, DescriptorType::StorageBuffer, work_item_buf_},
      {4, 0, DescriptorType::StorageBuffer, draw_cmd_buf_},
      {5, 0, DescriptorType::StorageBuffer, visible_draw_buf_},
      {6, 0, DescriptorType::StorageBuffer, cull_stats_buf_},
  };
  cull_bg_ = device_.create_bind_group({cull_bgl_, cull_ents}, error);

  BindGroupEntry raster_ents[3]{
      {0, 0, DescriptorType::UniformBuffer, raster_ubo_},
      {1, 0, DescriptorType::StorageBuffer, instance_buf_},
      {2, 0, DescriptorType::StorageBuffer, visible_draw_buf_},
  };
  raster_bg_ = device_.create_bind_group({raster_bgl_, raster_ents}, error);

  BindGroupEntry shade_ents[15]{
      {0, 0, DescriptorType::UniformBuffer, shade_ubo_},
      {1, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, vis_view_},
      {2, 0, DescriptorType::StorageBuffer, tile_list_buf_},
      {3, 0, DescriptorType::StorageBuffer, vertex_buf_},
      {4, 0, DescriptorType::StorageBuffer, index_buf_},
      {5, 0, DescriptorType::StorageBuffer, meshlet_buf_},
      {6, 0, DescriptorType::StorageBuffer, instance_buf_},
      {7, 0, DescriptorType::StorageBuffer, visible_draw_buf_},
      {8, 0, DescriptorType::StorageBuffer, material_buf_},
      {9, 0, DescriptorType::StorageBuffer, lights_.light_buffer_handle()},
      {10, 0, DescriptorType::StorageBuffer, lights_.cell_buffer_handle()},
      {11, 0, DescriptorType::StorageBuffer, lights_.index_buffer_handle()},
      {12, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, hdr_view_},
      {13, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, gbuf_view_},
      {14, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, vel_view_},
  };
  shade_bg_ = device_.create_bind_group({shade_bgl_, shade_ents}, error);

  return cull_bg_.valid() && raster_bg_.valid() && shade_bg_.valid();
}

VisFrameOutputs VisBufferPipeline::add_frame_passes(RenderGraph& graph,
                                                    const VisFrameView& view,
                                                    std::span<const ClusterLightGpu> lights,
                                                    std::string& error) {
  VisFrameOutputs out{};
  if (!ready_ || !cull_bg_ || !raster_bg_ || !shade_bg_) {
    error = "VisBufferPipeline::add_frame_passes: pipeline or scene not ready";
    return out;
  }

  const uint32_t rw = std::clamp(view.width > 0 ? view.width : max_width_, 1u, max_width_);
  const uint32_t rh = std::clamp(view.height > 0 ? view.height : max_height_, 1u, max_height_);
  const uint32_t tiles_x = (rw + 7u) / 8u;
  const uint32_t tiles_y = (rh + 7u) / 8u;
  graph.set_render_extent(rw, rh, max_width_, max_height_);

  // Upload frame UBOs (persistently mapped, zero alloc).
  CullUboGpu cu{};
  std::memcpy(cu.view_proj, view.view_proj, sizeof(cu.view_proj));
  extract_frustum_planes(view.view_proj, cu.frustum_planes);
  cu.camera_pos_pad[0] = view.camera_pos[0];
  cu.camera_pos_pad[1] = view.camera_pos[1];
  cu.camera_pos_pad[2] = view.camera_pos[2];
  cu.work_item_count = work_item_count_;
  cu.max_draws = max_draws_;
  cu.enable_cone_cull = view.enable_cone_cull ? 1u : 0u;
  if (void* p = device_.map_buffer(cull_ubo_)) std::memcpy(p, &cu, sizeof(cu));

  RasterUboGpu ru{};
  std::memcpy(ru.view_proj, view.view_proj, sizeof(ru.view_proj));
  std::memcpy(ru.prev_view_proj, view.prev_view_proj, sizeof(ru.prev_view_proj));
  if (void* p = device_.map_buffer(raster_ubo_)) std::memcpy(p, &ru, sizeof(ru));

  ClassifyUboGpu clu{};
  clu.width = rw;
  clu.height = rh;
  clu.tiles_x = tiles_x;
  clu.tiles_y = tiles_y;
  clu.max_tiles_per_class = max_tiles_;
  clu.material_class_count = kVisMaterialClassCount;
  if (void* p = device_.map_buffer(classify_ubo_)) std::memcpy(p, &clu, sizeof(clu));

  ShadeUboGpu su{};
  std::memcpy(su.view, view.view, sizeof(su.view));
  std::memcpy(su.view_proj, view.view_proj, sizeof(su.view_proj));
  std::memcpy(su.prev_view_proj, view.prev_view_proj, sizeof(su.prev_view_proj));
  su.camera_pos_znear[0] = view.camera_pos[0];
  su.camera_pos_znear[1] = view.camera_pos[1];
  su.camera_pos_znear[2] = view.camera_pos[2];
  su.camera_pos_znear[3] = view.z_near;
  su.sun_dir_zfar[0] = view.sun_dir[0];
  su.sun_dir_zfar[1] = view.sun_dir[1];
  su.sun_dir_zfar[2] = view.sun_dir[2];
  su.sun_dir_zfar[3] = view.z_far;
  su.sun_color_intensity[0] = view.sun_color[0];
  su.sun_color_intensity[1] = view.sun_color[1];
  su.sun_color_intensity[2] = view.sun_color[2];
  su.sun_color_intensity[3] = view.sun_intensity;
  su.viewport_grid[0] = rw;
  su.viewport_grid[1] = rh;
  su.viewport_grid[2] = kClusterGridX;
  su.viewport_grid[3] = kClusterGridY;
  su.grid_z_pad[0] = kClusterGridZ;
  if (void* p = device_.map_buffer(shade_ubo_)) std::memcpy(p, &su, sizeof(su));

  // 1. Clustered Forward+ light culling on Transfer + AsyncCompute.
  ClusterCullParamsGpu lcp{};
  std::memcpy(lcp.view, view.view, sizeof(lcp.view));
  std::memcpy(lcp.inv_proj, view.inv_proj, sizeof(lcp.inv_proj));
  lcp.z_near = view.z_near;
  lcp.z_far = view.z_far;
  ClusteredLightsView light_view = lights_.add_cull_pass(graph, lcp, lights);

  // Import VisBuffer persistent/DRS outputs into RenderGraph.
  out.vis_buffer = graph.import_texture(
      "vis_buffer", vis_tex_, vis_view_, ResourceState::Undefined, ResourceState::CopySrc, Format::Rg32Uint);
  out.depth_buffer = graph.import_texture(
      "vis_depth", depth_tex_, depth_view_, ResourceState::Undefined, ResourceState::DepthRead, Format::D32Sfloat);
  out.hdr_color = graph.import_texture(
      "vis_hdr", hdr_tex_, hdr_view_, ResourceState::Undefined, ResourceState::CopySrc, Format::Rgba16Sfloat);
  out.thin_gbuf = graph.import_texture(
      "vis_gbuf", gbuf_tex_, gbuf_view_, ResourceState::Undefined, ResourceState::CopySrc, Format::Rgba16Sfloat);
  out.velocity = graph.import_texture(
      "vis_velocity", vel_tex_, vel_view_, ResourceState::Undefined, ResourceState::CopySrc, Format::Rg16Sfloat);

  RGBufferHandle rg_draw_cmds = graph.import_buffer("vis_draw_cmds", draw_cmd_buf_, ResourceState::Common);
  RGBufferHandle rg_vis_draws = graph.import_buffer("vis_visible_draws", visible_draw_buf_, ResourceState::Common);
  out.cull_stats = graph.import_buffer("vis_cull_stats", cull_stats_buf_, ResourceState::Common);
  out.tile_indirect = graph.import_buffer("vis_tile_indirect", tile_indirect_buf_, ResourceState::Common);
  RGBufferHandle rg_tile_list = graph.import_buffer("vis_tile_lists", tile_list_buf_, ResourceState::Common);

  // 2. GPU Meshlet Cull on AsyncCompute.
  graph.add_pass(
      "VisCull", QueueKind::AsyncCompute,
      [&](RenderGraphBuilder& b) {
        b.write_buffer(rg_draw_cmds, ResourceState::StorageReadWrite);
        b.write_buffer(rg_vis_draws, ResourceState::StorageReadWrite);
        b.write_buffer(out.cull_stats, ResourceState::StorageReadWrite);
      },
      [this](CommandList& cmd, const RenderGraphContext&) {
        const uint32_t zeros[4] = {0, 0, 0, 0};
        BufferBarrier to_dst{cull_stats_buf_, ResourceState::StorageReadWrite, ResourceState::CopyDst};
        cmd.barrier({{}, std::span(&to_dst, 1)});
        cmd.update_buffer(cull_stats_buf_, 0, sizeof(zeros), zeros);
        BufferBarrier to_uav{cull_stats_buf_, ResourceState::CopyDst, ResourceState::StorageReadWrite};
        cmd.barrier({{}, std::span(&to_uav, 1)});

        cmd.bind_pipeline(cull_pipe_);
        cmd.bind_group(0, cull_bg_);
        cmd.dispatch((work_item_count_ + 63u) / 64u, 1, 1);
      });

  // 3. Visibility Buffer Raster + Sky/Output Clear on Graphics.
  const std::array<float, 4> sky = {
      view.sky_clear[0], view.sky_clear[1], view.sky_clear[2], view.sky_clear[3]};
  graph.add_pass(
      "VisRaster", QueueKind::Graphics,
      [&](RenderGraphBuilder& b) {
        b.read_buffer(rg_draw_cmds, ResourceState::IndirectArg);
        b.read_buffer(out.cull_stats, ResourceState::IndirectArg);
        b.read_buffer(rg_vis_draws, ResourceState::ShaderRead);
        b.write_texture(out.vis_buffer, ResourceState::RenderTarget);
        b.write_texture(out.depth_buffer, ResourceState::DepthWrite);
        b.write_texture(out.hdr_color, ResourceState::RenderTarget);
        b.write_texture(out.thin_gbuf, ResourceState::RenderTarget);
        b.write_texture(out.velocity, ResourceState::RenderTarget);
      },
      [this, rw, rh, sky](CommandList& cmd, const RenderGraphContext&) {
        // Clear HDR/G-Buffer/Velocity sky background via fast render pass clear.
        ColorAttachmentDesc clears[3]{};
        clears[0].view = hdr_view_;
        clears[0].load_op = LoadOp::Clear;
        clears[0].store_op = StoreOp::Store;
        std::memcpy(clears[0].clear_color, sky.data(), sizeof(float) * 4);
        clears[1].view = gbuf_view_;
        clears[1].load_op = LoadOp::Clear;
        clears[1].store_op = StoreOp::Store;
        clears[1].clear_color[0] = 0.5f;
        clears[1].clear_color[1] = 0.5f;
        clears[1].clear_color[2] = 1.0f;
        clears[1].clear_color[3] = 0.0f;
        clears[2].view = vel_view_;
        clears[2].load_op = LoadOp::Clear;
        clears[2].store_op = StoreOp::Store;
        clears[2].clear_color[3] = 0.0f;
        RenderPassDesc clear_rp{rw, rh, clears, nullptr, "VisOutputClear"};
        cmd.begin_render_pass(clear_rp);
        cmd.end_render_pass();

        // Rasterize GPU-culled meshlets into 64-bit VisBuf + Reverse-Z D32F.
        ColorAttachmentDesc vis_att{};
        vis_att.view = vis_view_;
        vis_att.load_op = LoadOp::Clear;
        vis_att.store_op = StoreOp::Store;
        vis_att.clear_color[0] = 0.0f;
        vis_att.clear_color[1] = 0.0f;
        vis_att.clear_color[2] = 0.0f;
        vis_att.clear_color[3] = 0.0f;

        DepthAttachmentDesc depth_att{};
        depth_att.view = depth_view_;
        depth_att.load_op = LoadOp::Clear;
        depth_att.store_op = StoreOp::Store;
        depth_att.clear_depth = 0.0f;  // Reverse-Z far = 0.0

        RenderPassDesc rp{rw, rh, std::span(&vis_att, 1), &depth_att, "VisRasterPass"};
        cmd.begin_render_pass(rp);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh), 0.0f, 1.0f);
        cmd.set_scissor(0, 0, rw, rh);
        cmd.bind_pipeline(raster_pipe_);
        cmd.bind_group(0, raster_bg_);
        BufferHandle vbs[2] = {vertex_buf_, draw_id_vb_};
        uint64_t vb_offs[2] = {0, 0};
        cmd.bind_vertex_buffers(0, vbs, vb_offs);
        cmd.bind_index_buffer(index_buf_, 0, IndexType::Uint32);
        cmd.draw_indexed_indirect_count(
            draw_cmd_buf_, 0, cull_stats_buf_, 0, max_draws_, sizeof(DrawIndexedIndirectCmd));
        cmd.end_render_pass();
      });

  // 4. 8x8 LDS Tile Material Classification on AsyncCompute.
  graph.add_pass(
      "VisTileClassify", QueueKind::AsyncCompute,
      [&](RenderGraphBuilder& b) {
        b.read_texture(out.vis_buffer, ResourceState::StorageReadWrite);
        b.write_buffer(out.tile_indirect, ResourceState::StorageReadWrite);
        b.write_buffer(rg_tile_list, ResourceState::StorageReadWrite);
      },
      [this, tiles_x, tiles_y](CommandList& cmd, const RenderGraphContext&) {
        uint32_t init_args[kVisMaterialClassCount * 3]{};
        for (uint32_t c = 0; c < kVisMaterialClassCount; ++c) {
          init_args[c * 3 + 0] = 0u;  // group_x = active tile count
          init_args[c * 3 + 1] = 1u;  // group_y = 1
          init_args[c * 3 + 2] = 1u;  // group_z = 1
        }
        BufferBarrier to_dst{tile_indirect_buf_, ResourceState::StorageReadWrite, ResourceState::CopyDst};
        cmd.barrier({{}, std::span(&to_dst, 1)});
        cmd.update_buffer(tile_indirect_buf_, 0, sizeof(init_args), init_args);
        BufferBarrier to_uav{tile_indirect_buf_, ResourceState::CopyDst, ResourceState::StorageReadWrite};
        cmd.barrier({{}, std::span(&to_uav, 1)});

        cmd.bind_pipeline(classify_pipe_);
        cmd.bind_group(0, classify_bg_);
        cmd.dispatch(tiles_x, tiles_y, 1);
      });

  // 5. Tile-Classified Compute Material Shading + Clustered Forward+ Lighting on AsyncCompute.
  graph.add_pass(
      "VisMaterialShade", QueueKind::AsyncCompute,
      [&](RenderGraphBuilder& b) {
        b.read_texture(out.vis_buffer, ResourceState::StorageReadWrite);
        b.read_buffer(out.tile_indirect, ResourceState::IndirectArg);
        b.read_buffer(rg_tile_list, ResourceState::ShaderRead);
        b.read_buffer(rg_vis_draws, ResourceState::ShaderRead);
        b.read_buffer(light_view.light_buffer, ResourceState::ShaderRead);
        b.read_buffer(light_view.cell_buffer, ResourceState::ShaderRead);
        b.read_buffer(light_view.index_buffer, ResourceState::ShaderRead);
        b.write_texture(out.hdr_color, ResourceState::StorageReadWrite);
        b.write_texture(out.thin_gbuf, ResourceState::StorageReadWrite);
        b.write_texture(out.velocity, ResourceState::StorageReadWrite);
      },
      [this](CommandList& cmd, const RenderGraphContext&) {
        cmd.bind_pipeline(shade_pipe_);
        cmd.bind_group(0, shade_bg_);
        for (uint32_t c = 0; c < kVisMaterialClassCount; ++c) {
          ShadePushConstants pc{c, max_tiles_};
          cmd.push_constants(ShaderStage::Compute, 0, sizeof(pc), &pc);
          cmd.dispatch_indirect(tile_indirect_buf_, static_cast<uint64_t>(c) * 12ull);
        }
      });

  return out;
}

bool VisBufferPipeline::readback_telemetry(VisTelemetry& out, std::string& error) {
  out = {};
  if (!ready_) return false;
  const uint64_t tile_bytes = static_cast<uint64_t>(kVisMaterialClassCount) * 12ull;
  const bool ok = device_.submit_once([&](CommandList& cmd) {
    BufferBarrier bbs[2] = {
        {cull_stats_buf_, ResourceState::IndirectArg, ResourceState::CopySrc},
        {tile_indirect_buf_, ResourceState::IndirectArg, ResourceState::CopySrc},
    };
    cmd.barrier({{}, bbs});
    cmd.copy_buffer(cull_stats_buf_, 0, telemetry_rb_buf_, 0, 16ull);
    cmd.copy_buffer(tile_indirect_buf_, 0, telemetry_rb_buf_, 16ull, tile_bytes);
    cmd.barrier({{}, {}, false, false, true});
  }, error);
  if (!ok) return false;

  auto* ptr = static_cast<const uint32_t*>(device_.map_buffer(telemetry_rb_buf_));
  if (!ptr) {
    error = "VisBufferPipeline::readback_telemetry: map_buffer failed";
    return false;
  }
  out.visible_meshlets = ptr[0];
  out.frustum_culled = ptr[1];
  out.cone_culled = ptr[2];
  out.tested_meshlets = ptr[3];
  for (uint32_t c = 0; c < kVisMaterialClassCount; ++c) {
    out.active_tiles_per_class[c] = ptr[4 + c * 3];
  }
  device_.unmap_buffer(telemetry_rb_buf_);
  return true;
}

}  // namespace shift::rhi
