#include "rhi/post_fx_compute.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "rhi_modern_shaders_hlsl_msl.h"
#include "rhi_modern_shaders_spv.h"

namespace shift::rhi {

namespace {

struct HizSpdPush {
  uint32_t src_width, src_height, num_mips, is_reverse_z;
};
struct BloomPush {
  uint32_t dst_width, dst_height, src_width, src_height, mode;
  float threshold, intensity, pad;
};
struct ExposurePush {
  uint32_t width, height, mode;
  float min_log_lum, inv_log_range, dt, adapt_speed, target_key;
};
struct MblurTilePush {
  uint32_t width, height, tiles_x, tiles_y;
};
struct TaaPush {
  uint32_t width, height, history_valid;
  float blend_alpha, bloom_strength, manual_exposure;
  uint32_t bloom_width, bloom_height;
};
struct alignas(16) HizCullUbo {
  float view_proj[16];
  float frustum_planes[6][4];
  float camera_pos_znear[4];
  uint32_t work_item_count;
  uint32_t max_draws;
  uint32_t enable_cone_cull;
  uint32_t enable_hiz_cull;
  uint32_t phase;
  uint32_t hiz_width;
  uint32_t hiz_height;
  uint32_t hiz_mip_count;
};

void extract_frustum_planes_hiz(const float m[16], float planes[6][4]) {
  auto row = [&](int r, float out[4]) {
    out[0] = m[0 * 4 + r];
    out[1] = m[1 * 4 + r];
    out[2] = m[2 * 4 + r];
    out[3] = m[3 * 4 + r];
  };
  float r0[4], r1[4], r2[4], r3[4];
  row(0, r0); row(1, r1); row(2, r2); row(3, r3);
  auto norm = [](float p[4]) {
    const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (len > 1e-6f) {
      const float inv = 1.0f / len;
      for (int i = 0; i < 4; ++i) p[i] *= inv;
    }
  };
  for (int i = 0; i < 4; ++i) {
    planes[0][i] = r3[i] + r0[i];
    planes[1][i] = r3[i] - r0[i];
    planes[2][i] = r3[i] + r1[i];
    planes[3][i] = r3[i] - r1[i];
    planes[4][i] = r2[i];
    planes[5][i] = r3[i] - r2[i];
  }
  for (int p = 0; p < 6; ++p) norm(planes[p]);
}

std::shared_ptr<BindGroupHandle> make_bg_guard(Device& dev, BindGroupHandle bg) {
  return std::shared_ptr<BindGroupHandle>(
      new BindGroupHandle(bg),
      [&dev](BindGroupHandle* h) {
        if (*h) dev.destroy_bind_group(*h);
        delete h;
      });
}

}  // namespace

ComputePostFxPipeline::ComputePostFxPipeline(Device& device) : device_(device) {}

ComputePostFxPipeline::~ComputePostFxPipeline() { shutdown(); }

bool ComputePostFxPipeline::init(uint32_t max_width, uint32_t max_height, std::string& error) {
  shutdown();
  if (max_width == 0 || max_height == 0) {
    error = "ComputePostFxPipeline::init: invalid dimensions";
    return false;
  }
  max_w_ = max_width;
  max_h_ = max_height;
  hiz_w_ = std::max(max_w_ >> 1u, 1u);
  hiz_h_ = std::max(max_h_ >> 1u, 1u);
  uint32_t max_mips = 1;
  for (uint32_t d = std::max(hiz_w_, hiz_h_); d > 1u && max_mips < 4u; d >>= 1u) ++max_mips;
  hiz_mips_ = max_mips;

  bloom_w0_ = std::max(max_w_ >> 1u, 1u);
  bloom_h0_ = std::max(max_h_ >> 1u, 1u);
  bloom_w1_ = std::max(bloom_w0_ >> 1u, 1u);
  bloom_h1_ = std::max(bloom_h0_ >> 1u, 1u);
  tile_w_ = (max_w_ + 15u) / 16u;
  tile_h_ = (max_h_ + 15u) / 16u;

  const BackendKind bk = device_.backend();

  // 1. SPD Hi-Z pipeline.
  {
    BindGroupLayoutBinding b[5]{
        {0, DescriptorType::SampledTexture, 1, ShaderStage::Compute, false, true},
        {1, DescriptorType::StorageTexture, 1, ShaderStage::Compute, false, false},
        {2, DescriptorType::StorageTexture, 1, ShaderStage::Compute, false, false},
        {3, DescriptorType::StorageTexture, 1, ShaderStage::Compute, false, false},
        {4, DescriptorType::StorageTexture, 1, ShaderStage::Compute, false, false},
    };
    hiz_bgl_ = device_.create_bind_group_layout({b}, error);
    if (!hiz_bgl_) return false;
    PushConstantRange pc{ShaderStage::Compute, 0, sizeof(HizSpdPush)};
    hiz_pl_ = device_.create_pipeline_layout({std::span(&hiz_bgl_, 1), std::span(&pc, 1)}, error);
    if (!hiz_pl_) return false;
    hiz_pipe_ = device_.create_compute_pipeline(
        {hiz_pl_, shaders::make_shader(bk, ShaderStage::Compute, shaders::kHizSpdCompSpv,
                                       shaders::kHizSpdCompHlsl, shaders::kHizSpdCompMsl)}, error);
    if (!hiz_pipe_) return false;
  }

  // 2. 2-Phase Hi-Z Cull pipeline.
  {
    BindGroupLayoutBinding b[9]{
        {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, false, true},
        {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, false, true},
        {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, false, true},
        {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, false, true},
        {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, false, false},
        {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, false, false},
        {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, false, false},
        {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, false, false},
        {8, DescriptorType::SampledTexture, 1, ShaderStage::Compute, false, true},
    };
    cull_hiz_bgl_ = device_.create_bind_group_layout({b}, error);
    if (!cull_hiz_bgl_) return false;
    cull_hiz_pl_ = device_.create_pipeline_layout({std::span(&cull_hiz_bgl_, 1), {}}, error);
    if (!cull_hiz_pl_) return false;
    cull_hiz_pipe_ = device_.create_compute_pipeline(
        {cull_hiz_pl_, shaders::make_shader(bk, ShaderStage::Compute, shaders::kVisCullHizCompSpv,
                                            shaders::kVisCullHizCompHlsl, shaders::kVisCullHizCompMsl)}, error);
    if (!cull_hiz_pipe_) return false;
  }

  // 3. Bloom SPD pipeline.
  {
    BindGroupLayoutBinding b[2]{
        {0, DescriptorType::SampledTexture, 1, ShaderStage::Compute, false, true},
        {1, DescriptorType::StorageTexture, 1, ShaderStage::Compute, false, false},
    };
    bloom_bgl_ = device_.create_bind_group_layout({b}, error);
    if (!bloom_bgl_) return false;
    PushConstantRange pc{ShaderStage::Compute, 0, sizeof(BloomPush)};
    bloom_pl_ = device_.create_pipeline_layout({std::span(&bloom_bgl_, 1), std::span(&pc, 1)}, error);
    if (!bloom_pl_) return false;
    bloom_pipe_ = device_.create_compute_pipeline(
        {bloom_pl_, shaders::make_shader(bk, ShaderStage::Compute, shaders::kPostBloomSpdCompSpv,
                                         shaders::kPostBloomSpdCompHlsl, shaders::kPostBloomSpdCompMsl)}, error);
    if (!bloom_pipe_) return false;
  }

  // 4. Exposure Histogram pipeline.
  {
    BindGroupLayoutBinding b[3]{
        {0, DescriptorType::SampledTexture, 1, ShaderStage::Compute, false, true},
        {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, false, false},
        {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, false, false},
    };
    exp_bgl_ = device_.create_bind_group_layout({b}, error);
    if (!exp_bgl_) return false;
    PushConstantRange pc{ShaderStage::Compute, 0, sizeof(ExposurePush)};
    exp_pl_ = device_.create_pipeline_layout({std::span(&exp_bgl_, 1), std::span(&pc, 1)}, error);
    if (!exp_pl_) return false;
    exp_pipe_ = device_.create_compute_pipeline(
        {exp_pl_, shaders::make_shader(bk, ShaderStage::Compute, shaders::kPostExposureHistCompSpv,
                                       shaders::kPostExposureHistCompHlsl, shaders::kPostExposureHistCompMsl)}, error);
    if (!exp_pipe_) return false;
  }

  // 5. McGuire Motion Blur Tile Max pipeline.
  {
    BindGroupLayoutBinding b[2]{
        {0, DescriptorType::SampledTexture, 1, ShaderStage::Compute, false, true},
        {1, DescriptorType::StorageTexture, 1, ShaderStage::Compute, false, false},
    };
    mblur_bgl_ = device_.create_bind_group_layout({b}, error);
    if (!mblur_bgl_) return false;
    PushConstantRange pc{ShaderStage::Compute, 0, sizeof(MblurTilePush)};
    mblur_pl_ = device_.create_pipeline_layout({std::span(&mblur_bgl_, 1), std::span(&pc, 1)}, error);
    if (!mblur_pl_) return false;
    mblur_pipe_ = device_.create_compute_pipeline(
        {mblur_pl_, shaders::make_shader(bk, ShaderStage::Compute, shaders::kPostMblurTileCompSpv,
                                         shaders::kPostMblurTileCompHlsl, shaders::kPostMblurTileCompMsl)}, error);
    if (!mblur_pipe_) return false;
  }

  // 6. Pre-Tonemap Linear HDR TAA-U + Tonemap pipeline.
  {
    BindGroupLayoutBinding b[8]{
        {0, DescriptorType::SampledTexture, 1, ShaderStage::Compute, false, true},
        {1, DescriptorType::SampledTexture, 1, ShaderStage::Compute, false, true},
        {2, DescriptorType::SampledTexture, 1, ShaderStage::Compute, false, true},
        {3, DescriptorType::SampledTexture, 1, ShaderStage::Compute, false, true},
        {4, DescriptorType::SampledTexture, 1, ShaderStage::Compute, false, true},
        {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, false, true},
        {6, DescriptorType::StorageTexture, 1, ShaderStage::Compute, false, false},
        {7, DescriptorType::StorageTexture, 1, ShaderStage::Compute, false, false},
    };
    taa_bgl_ = device_.create_bind_group_layout({b}, error);
    if (!taa_bgl_) return false;
    PushConstantRange pc{ShaderStage::Compute, 0, sizeof(TaaPush)};
    taa_pl_ = device_.create_pipeline_layout({std::span(&taa_bgl_, 1), std::span(&pc, 1)}, error);
    if (!taa_pl_) return false;
    taa_pipe_ = device_.create_compute_pipeline(
        {taa_pl_, shaders::make_shader(bk, ShaderStage::Compute, shaders::kPostTaaHdrCompSpv,
                                       shaders::kPostTaaHdrCompHlsl, shaders::kPostTaaHdrCompMsl)}, error);
    if (!taa_pipe_) return false;
  }

  // Allocate persistent targets & exposure buffers.
  const TextureUsage st_rw = TextureUsage::Sampled | TextureUsage::Storage | TextureUsage::TransferSrc;
  hiz_tex_ = device_.create_texture(
      {TextureType::Tex2D, Format::R32Sfloat, hiz_w_, hiz_h_, 1, hiz_mips_, 1, 1,
       st_rw, MemoryHint::GpuOnly, "post_hiz"}, error);
  if (!hiz_tex_) return false;
  hiz_full_view_ = device_.create_texture_view(
      {hiz_tex_, TextureType::Tex2D, Format::R32Sfloat, 0, hiz_mips_, 0, 1}, error);
  for (uint32_t m = 0; m < 4; ++m) {
    const uint32_t eff_m = std::min(m, hiz_mips_ - 1u);
    hiz_mip_views_[m] = device_.create_texture_view(
        {hiz_tex_, TextureType::Tex2D, Format::R32Sfloat, eff_m, 1, 0, 1}, error);
    if (!hiz_mip_views_[m]) return false;
  }

  bloom_tex_[0] = device_.create_texture(
      {TextureType::Tex2D, Format::Rgba16Sfloat, bloom_w0_, bloom_h0_, 1, 1, 1, 1,
       st_rw, MemoryHint::GpuOnly, "post_bloom0"}, error);
  bloom_tex_[1] = device_.create_texture(
      {TextureType::Tex2D, Format::Rgba16Sfloat, bloom_w1_, bloom_h1_, 1, 1, 1, 1,
       st_rw, MemoryHint::GpuOnly, "post_bloom1"}, error);
  tile_vel_tex_ = device_.create_texture(
      {TextureType::Tex2D, Format::Rg16Sfloat, tile_w_, tile_h_, 1, 1, 1, 1,
       st_rw, MemoryHint::GpuOnly, "post_mblur_tile"}, error);
  hist_tex_[0] = device_.create_texture(
      {TextureType::Tex2D, Format::Rgba16Sfloat, max_w_, max_h_, 1, 1, 1, 1,
       st_rw, MemoryHint::GpuOnly, "post_taa_hist0"}, error);
  hist_tex_[1] = device_.create_texture(
      {TextureType::Tex2D, Format::Rgba16Sfloat, max_w_, max_h_, 1, 1, 1, 1,
       st_rw, MemoryHint::GpuOnly, "post_taa_hist1"}, error);
  ldr_tex_ = device_.create_texture(
      {TextureType::Tex2D, Format::Rgba8Unorm, max_w_, max_h_, 1, 1, 1, 1,
       st_rw, MemoryHint::GpuOnly, "post_ldr_out"}, error);
  if (!bloom_tex_[0] || !bloom_tex_[1] || !tile_vel_tex_ || !hist_tex_[0] || !hist_tex_[1] || !ldr_tex_) {
    return false;
  }

  bloom_views_[0] = device_.create_texture_view({bloom_tex_[0], TextureType::Tex2D, Format::Rgba16Sfloat, 0, 1, 0, 1}, error);
  bloom_views_[1] = device_.create_texture_view({bloom_tex_[1], TextureType::Tex2D, Format::Rgba16Sfloat, 0, 1, 0, 1}, error);
  tile_vel_view_ = device_.create_texture_view({tile_vel_tex_, TextureType::Tex2D, Format::Rg16Sfloat, 0, 1, 0, 1}, error);
  hist_views_[0] = device_.create_texture_view({hist_tex_[0], TextureType::Tex2D, Format::Rgba16Sfloat, 0, 1, 0, 1}, error);
  hist_views_[1] = device_.create_texture_view({hist_tex_[1], TextureType::Tex2D, Format::Rgba16Sfloat, 0, 1, 0, 1}, error);
  ldr_view_ = device_.create_texture_view({ldr_tex_, TextureType::Tex2D, Format::Rgba8Unorm, 0, 1, 0, 1}, error);

  hist_buf_ = device_.create_buffer(
      {256ull * sizeof(uint32_t), BufferUsage::Storage | BufferUsage::TransferDst, MemoryHint::GpuOnly, false, "post_exp_hist"}, error);
  exp_buf_ = device_.create_buffer(
      {16ull, BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc, MemoryHint::GpuOnly, false, "post_exp_buf"}, error);
  exp_rb_ = device_.create_buffer(
      {16ull, BufferUsage::TransferDst, MemoryHint::GpuToCpu, true, "post_exp_rb"}, error);
  if (!hist_buf_ || !exp_buf_ || !exp_rb_) return false;

  // Zero histogram & initialize adapted exposure = 1.0f.
  const bool init_ok = device_.submit_once([&](CommandList& cmd) {
    cmd.fill_buffer(hist_buf_, 0, 256ull * sizeof(uint32_t), 0u);
    const float init_exp[4] = {1.0f, 0.18f, 0.0f, 0.0f};
    cmd.update_buffer(exp_buf_, 0, sizeof(init_exp), init_exp);
    BufferBarrier bbs[2] = {
        {hist_buf_, ResourceState::CopyDst, ResourceState::Common},
        {exp_buf_, ResourceState::CopyDst, ResourceState::Common},
    };
    cmd.barrier({{}, bbs});
  }, error);
  if (!init_ok) return false;

  ready_ = true;
  return true;
}

void ComputePostFxPipeline::shutdown() {
  ready_ = false;
  auto dt = [&](TextureHandle& h) { if (h) { device_.destroy_texture(h); h = {}; } };
  auto dtv = [&](TextureViewHandle& h) { if (h) { device_.destroy_texture_view(h); h = {}; } };
  auto db = [&](BufferHandle& h) { if (h) { device_.destroy_buffer(h); h = {}; } };
  auto dp = [&](PipelineHandle& h) { if (h) { device_.destroy_pipeline(h); h = {}; } };
  auto dpl = [&](PipelineLayoutHandle& h) { if (h) { device_.destroy_pipeline_layout(h); h = {}; } };
  auto dbgl = [&](BindGroupLayoutHandle& h) { if (h) { device_.destroy_bind_group_layout(h); h = {}; } };

  db(exp_rb_); db(exp_buf_); db(hist_buf_);
  dtv(ldr_view_); dtv(hist_views_[1]); dtv(hist_views_[0]); dtv(tile_vel_view_);
  dtv(bloom_views_[1]); dtv(bloom_views_[0]);
  for (int m = 0; m < 4; ++m) dtv(hiz_mip_views_[m]);
  dtv(hiz_full_view_);
  dt(ldr_tex_); dt(hist_tex_[1]); dt(hist_tex_[0]); dt(tile_vel_tex_);
  dt(bloom_tex_[1]); dt(bloom_tex_[0]); dt(hiz_tex_);

  dp(taa_pipe_); dp(mblur_pipe_); dp(exp_pipe_); dp(bloom_pipe_); dp(cull_hiz_pipe_); dp(hiz_pipe_);
  dpl(taa_pl_); dpl(mblur_pl_); dpl(exp_pl_); dpl(bloom_pl_); dpl(cull_hiz_pl_); dpl(hiz_pl_);
  dbgl(taa_bgl_); dbgl(mblur_bgl_); dbgl(exp_bgl_); dbgl(bloom_bgl_); dbgl(cull_hiz_bgl_); dbgl(hiz_bgl_);
}

PostFxOutputs ComputePostFxPipeline::add_frame_passes(RenderGraph& graph,
                                                      RGTextureHandle hdr_rg, TextureViewHandle hdr_view,
                                                      RGTextureHandle depth_rg, TextureViewHandle depth_view,
                                                      RGTextureHandle vel_rg, TextureViewHandle vel_view,
                                                      const PostFxConfig& cfg, float dt_seconds,
                                                      std::string& error) {
  PostFxOutputs out{};
  if (!ready_) {
    error = "ComputePostFxPipeline::add_frame_passes: not initialized";
    return out;
  }

  const uint32_t rw = std::clamp(cfg.width ? cfg.width : max_w_, 1u, max_w_);
  const uint32_t rh = std::clamp(cfg.height ? cfg.height : max_h_, 1u, max_h_);
  const uint32_t prev_hi = hist_idx_ ^ 1u;
  const uint32_t curr_hi = hist_idx_;

  out.hiz_pyramid = graph.import_texture("post_hiz", hiz_tex_, hiz_full_view_, ResourceState::Undefined, ResourceState::CopySrc, Format::R32Sfloat);
  RGTextureHandle rg_bloom0 = graph.import_texture("post_bloom0", bloom_tex_[0], bloom_views_[0], ResourceState::Undefined, ResourceState::ShaderRead, Format::Rgba16Sfloat);
  RGTextureHandle rg_bloom1 = graph.import_texture("post_bloom1", bloom_tex_[1], bloom_views_[1], ResourceState::Undefined, ResourceState::ShaderRead, Format::Rgba16Sfloat);
  out.bloom_tex = rg_bloom0;
  out.tile_max_vel = graph.import_texture("post_mblur_tile", tile_vel_tex_, tile_vel_view_, ResourceState::Undefined, ResourceState::CopySrc, Format::Rg16Sfloat);
  RGTextureHandle rg_prev_hist = graph.import_texture("post_taa_prev", hist_tex_[prev_hi], hist_views_[prev_hi],
                                                      history_valid_ ? ResourceState::Common : ResourceState::Undefined,
                                                      ResourceState::Common, Format::Rgba16Sfloat);
  out.taa_history = graph.import_texture("post_taa_curr", hist_tex_[curr_hi], hist_views_[curr_hi], ResourceState::Undefined, ResourceState::Common, Format::Rgba16Sfloat);
  out.ldr_output = graph.import_texture("post_ldr_out", ldr_tex_, ldr_view_, ResourceState::Undefined, ResourceState::CopySrc, Format::Rgba8Unorm);

  RGBufferHandle rg_hist_buf = graph.import_buffer("post_exp_hist", hist_buf_, ResourceState::Common);
  out.exposure_buf = graph.import_buffer("post_exp_buf", exp_buf_, ResourceState::Common);

  // Build transient bind groups held alive through graph execution.
  BindGroupEntry hiz_ents[5]{
      {0, 0, DescriptorType::SampledTexture, {}, 0, UINT64_MAX, depth_view},
      {1, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, hiz_mip_views_[0]},
      {2, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, hiz_mip_views_[1]},
      {3, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, hiz_mip_views_[2]},
      {4, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, hiz_mip_views_[3]},
  };
  auto hiz_bg = make_bg_guard(device_, device_.create_bind_group({hiz_bgl_, hiz_ents}, error));

  BindGroupEntry exp_ents[3]{
      {0, 0, DescriptorType::SampledTexture, {}, 0, UINT64_MAX, hdr_view},
      {1, 0, DescriptorType::StorageBuffer, hist_buf_},
      {2, 0, DescriptorType::StorageBuffer, exp_buf_},
  };
  auto exp_bg = make_bg_guard(device_, device_.create_bind_group({exp_bgl_, exp_ents}, error));

  BindGroupEntry bdown0_ents[2]{
      {0, 0, DescriptorType::SampledTexture, {}, 0, UINT64_MAX, hdr_view},
      {1, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, bloom_views_[0]},
  };
  auto bdown0_bg = make_bg_guard(device_, device_.create_bind_group({bloom_bgl_, bdown0_ents}, error));

  BindGroupEntry bdown1_ents[2]{
      {0, 0, DescriptorType::SampledTexture, {}, 0, UINT64_MAX, bloom_views_[0]},
      {1, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, bloom_views_[1]},
  };
  auto bdown1_bg = make_bg_guard(device_, device_.create_bind_group({bloom_bgl_, bdown1_ents}, error));

  BindGroupEntry bup0_ents[2]{
      {0, 0, DescriptorType::SampledTexture, {}, 0, UINT64_MAX, bloom_views_[1]},
      {1, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, bloom_views_[0]},
  };
  auto bup0_bg = make_bg_guard(device_, device_.create_bind_group({bloom_bgl_, bup0_ents}, error));

  BindGroupEntry mblur_ents[2]{
      {0, 0, DescriptorType::SampledTexture, {}, 0, UINT64_MAX, vel_view},
      {1, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, tile_vel_view_},
  };
  auto mblur_bg = make_bg_guard(device_, device_.create_bind_group({mblur_bgl_, mblur_ents}, error));

  BindGroupEntry taa_ents[8]{
      {0, 0, DescriptorType::SampledTexture, {}, 0, UINT64_MAX, hdr_view},
      {1, 0, DescriptorType::SampledTexture, {}, 0, UINT64_MAX, hist_views_[prev_hi]},
      {2, 0, DescriptorType::SampledTexture, {}, 0, UINT64_MAX, vel_view},
      {3, 0, DescriptorType::SampledTexture, {}, 0, UINT64_MAX, depth_view},
      {4, 0, DescriptorType::SampledTexture, {}, 0, UINT64_MAX, bloom_views_[0]},
      {5, 0, DescriptorType::StorageBuffer, exp_buf_},
      {6, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, hist_views_[curr_hi]},
      {7, 0, DescriptorType::StorageTexture, {}, 0, UINT64_MAX, ldr_view_},
  };
  auto taa_bg = make_bg_guard(device_, device_.create_bind_group({taa_bgl_, taa_ents}, error));

  if (!*hiz_bg || !*exp_bg || !*bdown0_bg || !*bdown1_bg || !*bup0_bg || !*mblur_bg || !*taa_bg) {
    return {};
  }

  // 1. Single-Pass LDS SPD Hi-Z Pyramid on AsyncCompute.
  graph.add_pass(
      "PostHizSpd", QueueKind::AsyncCompute,
      [depth_rg, hiz = out.hiz_pyramid](RenderGraphBuilder& b) {
        b.read_texture(depth_rg, ResourceState::ShaderRead);
        b.write_texture(hiz, ResourceState::StorageReadWrite);
      },
      [this, rw, rh, revz = cfg.is_reverse_z, hiz_bg](CommandList& cmd, const RenderGraphContext&) {
        HizSpdPush pc{rw, rh, hiz_mips_, revz ? 1u : 0u};
        cmd.bind_pipeline(hiz_pipe_);
        cmd.bind_group(0, *hiz_bg);
        cmd.push_constants(ShaderStage::Compute, 0, sizeof(pc), &pc);
        const uint32_t gx = (std::max(rw >> 1u, 1u) + 7u) / 8u;
        const uint32_t gy = (std::max(rh >> 1u, 1u) + 7u) / 8u;
        cmd.dispatch(gx, gy, 1u);
      });

  // 2. Async Compute 256-Bin Log-Luminance Histogram + Adaptation.
  graph.add_pass(
      "PostExposureHist", QueueKind::AsyncCompute,
      [hdr_rg, rg_hist_buf, exp_rg = out.exposure_buf](RenderGraphBuilder& b) {
        b.read_texture(hdr_rg, ResourceState::ShaderRead);
        b.write_buffer(rg_hist_buf, ResourceState::StorageReadWrite);
        b.write_buffer(exp_rg, ResourceState::StorageReadWrite);
      },
      [this, rw, rh, cfg, dt_seconds, exp_bg](CommandList& cmd, const RenderGraphContext&) {
        const float range = std::max(cfg.exposure_max_log_lum - cfg.exposure_min_log_lum, 1.0f);
        ExposurePush pc{rw, rh, 0u, cfg.exposure_min_log_lum, 1.0f / range,
                        std::clamp(dt_seconds, 0.001f, 1.0f), cfg.exposure_adapt_speed, cfg.exposure_target_key};
        cmd.bind_pipeline(exp_pipe_);
        cmd.bind_group(0, *exp_bg);
        cmd.push_constants(ShaderStage::Compute, 0, sizeof(pc), &pc);
        cmd.dispatch((rw + 15u) / 16u, (rh + 15u) / 16u, 1u);

        cmd.barrier({{}, {}, true, false, false});
        pc.mode = 1u;
        cmd.push_constants(ShaderStage::Compute, 0, sizeof(pc), &pc);
        cmd.dispatch(1u, 1u, 1u);
      });

  // 3. Async Compute Karis Bloom Downsample (mip0 -> mip1) + Tent Upsample (mip1 -> mip0).
  const uint32_t bw0 = std::clamp(std::max(rw >> 1u, 1u), 1u, bloom_w0_);
  const uint32_t bh0 = std::clamp(std::max(rh >> 1u, 1u), 1u, bloom_h0_);
  const uint32_t bw1 = std::clamp(std::max(bw0 >> 1u, 1u), 1u, bloom_w1_);
  const uint32_t bh1 = std::clamp(std::max(bh0 >> 1u, 1u), 1u, bloom_h1_);

  graph.add_pass(
      "PostBloomDown0", QueueKind::AsyncCompute,
      [hdr_rg, rg_bloom0](RenderGraphBuilder& b) {
        b.read_texture(hdr_rg, ResourceState::ShaderRead);
        b.write_texture(rg_bloom0, ResourceState::StorageReadWrite);
      },
      [this, rw, rh, bw0, bh0, cfg, bdown0_bg](CommandList& cmd, const RenderGraphContext&) {
        BloomPush pc{bw0, bh0, rw, rh, 0u, cfg.bloom_threshold, cfg.bloom_intensity, 0.0f};
        cmd.bind_pipeline(bloom_pipe_);
        cmd.bind_group(0, *bdown0_bg);
        cmd.push_constants(ShaderStage::Compute, 0, sizeof(pc), &pc);
        cmd.dispatch((bw0 + 7u) / 8u, (bh0 + 7u) / 8u, 1u);
      });

  graph.add_pass(
      "PostBloomDown1", QueueKind::AsyncCompute,
      [rg_bloom0, rg_bloom1](RenderGraphBuilder& b) {
        b.read_texture(rg_bloom0, ResourceState::ShaderRead);
        b.write_texture(rg_bloom1, ResourceState::StorageReadWrite);
      },
      [this, bw0, bh0, bw1, bh1, cfg, bdown1_bg](CommandList& cmd, const RenderGraphContext&) {
        BloomPush pc{bw1, bh1, bw0, bh0, 0u, 0.0f, cfg.bloom_intensity, 0.0f};
        cmd.bind_pipeline(bloom_pipe_);
        cmd.bind_group(0, *bdown1_bg);
        cmd.push_constants(ShaderStage::Compute, 0, sizeof(pc), &pc);
        cmd.dispatch((bw1 + 7u) / 8u, (bh1 + 7u) / 8u, 1u);
      });

  graph.add_pass(
      "PostBloomUp0", QueueKind::AsyncCompute,
      [rg_bloom1, rg_bloom0](RenderGraphBuilder& b) {
        b.read_texture(rg_bloom1, ResourceState::ShaderRead);
        b.write_texture(rg_bloom0, ResourceState::StorageReadWrite);
      },
      [this, bw0, bh0, bw1, bh1, cfg, bup0_bg](CommandList& cmd, const RenderGraphContext&) {
        BloomPush pc{bw0, bh0, bw1, bh1, 1u, 0.0f, cfg.bloom_intensity, 0.0f};
        cmd.bind_pipeline(bloom_pipe_);
        cmd.bind_group(0, *bup0_bg);
        cmd.push_constants(ShaderStage::Compute, 0, sizeof(pc), &pc);
        cmd.dispatch((bw0 + 7u) / 8u, (bh0 + 7u) / 8u, 1u);
      });

  // 4. Async Compute McGuire Motion-Blur Tile-Max Velocity.
  graph.add_pass(
      "PostMblurTile", QueueKind::AsyncCompute,
      [vel_rg, tm = out.tile_max_vel](RenderGraphBuilder& b) {
        b.read_texture(vel_rg, ResourceState::ShaderRead);
        b.write_texture(tm, ResourceState::StorageReadWrite);
      },
      [this, rw, rh, mblur_bg](CommandList& cmd, const RenderGraphContext&) {
        const uint32_t tx = (rw + 15u) / 16u;
        const uint32_t ty = (rh + 15u) / 16u;
        MblurTilePush pc{rw, rh, tx, ty};
        cmd.bind_pipeline(mblur_pipe_);
        cmd.bind_group(0, *mblur_bg);
        cmd.push_constants(ShaderStage::Compute, 0, sizeof(pc), &pc);
        cmd.dispatch(tx, ty, 1u);
      });

  // 5. Pre-Tonemap Linear HDR TAA-U + Exposure/Bloom Composite + ACES Tonemap.
  const bool hv = history_valid_;
  graph.add_pass(
      "PostTaaHdr", QueueKind::AsyncCompute,
      [hdr_rg, rg_prev_hist, vel_rg, depth_rg, rg_bloom0, exp_rg = out.exposure_buf,
       hist_out = out.taa_history, ldr_out = out.ldr_output](RenderGraphBuilder& b) {
        b.read_texture(hdr_rg, ResourceState::ShaderRead);
        b.read_texture(rg_prev_hist, ResourceState::ShaderRead);
        b.read_texture(vel_rg, ResourceState::ShaderRead);
        b.read_texture(depth_rg, ResourceState::ShaderRead);
        b.read_texture(rg_bloom0, ResourceState::ShaderRead);
        b.read_buffer(exp_rg, ResourceState::ShaderRead);
        b.write_texture(hist_out, ResourceState::StorageReadWrite);
        b.write_texture(ldr_out, ResourceState::StorageReadWrite);
      },
      [this, rw, rh, bw0, bh0, hv, cfg, taa_bg](CommandList& cmd, const RenderGraphContext&) {
        TaaPush pc{rw, rh, hv ? 1u : 0u, cfg.taa_blend_alpha, cfg.bloom_strength, cfg.manual_exposure, bw0, bh0};
        cmd.bind_pipeline(taa_pipe_);
        cmd.bind_group(0, *taa_bg);
        cmd.push_constants(ShaderStage::Compute, 0, sizeof(pc), &pc);
        cmd.dispatch((rw + 7u) / 8u, (rh + 7u) / 8u, 1u);
      });

  history_valid_ = true;
  hist_idx_ ^= 1u;
  return out;
}

bool ComputePostFxPipeline::run_two_phase_cull_test(const float view_proj[16], const float camera_pos[3],
                                                    std::span<const MeshletInstanceGpu> instances,
                                                    std::span<const MeshletGpu> meshlets,
                                                    TextureHandle prev_hiz_tex, TextureViewHandle prev_hiz_view,
                                                    TextureHandle curr_hiz_tex, TextureViewHandle curr_hiz_view,
                                                    uint32_t hiz_w, uint32_t hiz_h, uint32_t hiz_mips,
                                                    TwoPhaseCullStats& out_stats, std::string& error) {
  out_stats = {};
  if (!ready_ || instances.empty() || meshlets.empty()) {
    error = "run_two_phase_cull_test: not ready or empty inputs";
    return false;
  }

  struct WorkItem { uint32_t inst_id, meshlet_id; };
  std::vector<WorkItem> items;
  for (uint32_t i = 0; i < static_cast<uint32_t>(instances.size()); ++i) {
    for (uint32_t m = 0; m < instances[i].meshlet_count; ++m) {
      items.push_back({i, instances[i].first_meshlet + m});
    }
  }
  const uint32_t max_draws = std::max<uint32_t>(static_cast<uint32_t>(items.size()), 16u);

  BufferHandle ubo0 = device_.create_buffer({sizeof(HizCullUbo), BufferUsage::Uniform, MemoryHint::CpuToGpu, true, "hiz_ubo0"}, error);
  BufferHandle ubo1 = device_.create_buffer({sizeof(HizCullUbo), BufferUsage::Uniform, MemoryHint::CpuToGpu, true, "hiz_ubo1"}, error);
  BufferHandle inst_b = device_.create_buffer({instances.size_bytes(), BufferUsage::Storage | BufferUsage::TransferDst, MemoryHint::GpuOnly, false, "hiz_inst"}, error);
  BufferHandle mesh_b = device_.create_buffer({meshlets.size_bytes(), BufferUsage::Storage | BufferUsage::TransferDst, MemoryHint::GpuOnly, false, "hiz_mesh"}, error);
  BufferHandle item_b = device_.create_buffer({items.size() * sizeof(WorkItem), BufferUsage::Storage | BufferUsage::TransferDst, MemoryHint::GpuOnly, false, "hiz_items"}, error);
  BufferHandle draw_b = device_.create_buffer({max_draws * sizeof(DrawIndexedIndirectCmd), BufferUsage::Storage, MemoryHint::GpuOnly, false, "hiz_draws"}, error);
  BufferHandle vdraw_b = device_.create_buffer({max_draws * sizeof(VisibleMeshletDrawGpu), BufferUsage::Storage, MemoryHint::GpuOnly, false, "hiz_vdraws"}, error);
  BufferHandle occ_b = device_.create_buffer({max_draws * sizeof(WorkItem), BufferUsage::Storage, MemoryHint::GpuOnly, false, "hiz_occ"}, error);
  BufferHandle stat_b = device_.create_buffer({32ull, BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc, MemoryHint::GpuOnly, false, "hiz_stats"}, error);
  BufferHandle stat_rb = device_.create_buffer({32ull, BufferUsage::TransferDst, MemoryHint::GpuToCpu, true, "hiz_stats_rb"}, error);
  BindGroupHandle bg0{}, bg1{};

  auto cleanup = [&]() {
    if (bg1) device_.destroy_bind_group(bg1);
    if (bg0) device_.destroy_bind_group(bg0);
    for (BufferHandle b : {stat_rb, stat_b, occ_b, vdraw_b, draw_b, item_b, mesh_b, inst_b, ubo1, ubo0}) {
      if (b) device_.destroy_buffer(b);
    }
  };
  if (!ubo0 || !ubo1 || !inst_b || !mesh_b || !item_b || !draw_b || !vdraw_b || !occ_b || !stat_b || !stat_rb) {
    cleanup();
    return false;
  }

  HizCullUbo u{};
  std::memcpy(u.view_proj, view_proj, sizeof(u.view_proj));
  extract_frustum_planes_hiz(view_proj, u.frustum_planes);
  u.camera_pos_znear[0] = camera_pos[0];
  u.camera_pos_znear[1] = camera_pos[1];
  u.camera_pos_znear[2] = camera_pos[2];
  u.camera_pos_znear[3] = 0.1f;
  u.work_item_count = static_cast<uint32_t>(items.size());
  u.max_draws = max_draws;
  u.enable_cone_cull = 1u;
  u.enable_hiz_cull = 1u;
  u.phase = 0u;
  u.hiz_width = hiz_w;
  u.hiz_height = hiz_h;
  u.hiz_mip_count = hiz_mips;
  if (void* p = device_.map_buffer(ubo0)) std::memcpy(p, &u, sizeof(u));
  u.phase = 1u;
  if (void* p = device_.map_buffer(ubo1)) std::memcpy(p, &u, sizeof(u));

  BindGroupEntry e0[9]{
      {0, 0, DescriptorType::UniformBuffer, ubo0},
      {1, 0, DescriptorType::StorageBuffer, inst_b},
      {2, 0, DescriptorType::StorageBuffer, mesh_b},
      {3, 0, DescriptorType::StorageBuffer, item_b},
      {4, 0, DescriptorType::StorageBuffer, draw_b},
      {5, 0, DescriptorType::StorageBuffer, vdraw_b},
      {6, 0, DescriptorType::StorageBuffer, occ_b},
      {7, 0, DescriptorType::StorageBuffer, stat_b},
      {8, 0, DescriptorType::SampledTexture, {}, 0, UINT64_MAX, prev_hiz_view},
  };
  BindGroupEntry e1[9] = {e0[0], e0[1], e0[2], e0[3], e0[4], e0[5], e0[6], e0[7], e0[8]};
  e1[0].buffer = ubo1;
  e1[8].texture_view = curr_hiz_view;
  bg0 = device_.create_bind_group({cull_hiz_bgl_, e0}, error);
  bg1 = device_.create_bind_group({cull_hiz_bgl_, e1}, error);
  if (!bg0 || !bg1) { cleanup(); return false; }

  const bool ok = device_.submit_once([&](CommandList& cmd) {
    cmd.update_buffer(inst_b, 0, instances.size_bytes(), instances.data());
    cmd.update_buffer(mesh_b, 0, meshlets.size_bytes(), meshlets.data());
    cmd.update_buffer(item_b, 0, items.size() * sizeof(WorkItem), items.data());
    cmd.fill_buffer(stat_b, 0, 32ull, 0u);
    BufferBarrier to_comp[4] = {
        {inst_b, ResourceState::CopyDst, ResourceState::ShaderRead},
        {mesh_b, ResourceState::CopyDst, ResourceState::ShaderRead},
        {item_b, ResourceState::CopyDst, ResourceState::ShaderRead},
        {stat_b, ResourceState::CopyDst, ResourceState::StorageReadWrite},
    };
    TextureBarrier hiz_read[2]{};
    uint32_t hiz_bar_count = 0;
    hiz_read[hiz_bar_count].texture = prev_hiz_tex;
    hiz_read[hiz_bar_count].before = ResourceState::Common;
    hiz_read[hiz_bar_count].after = ResourceState::ShaderRead;
    hiz_read[hiz_bar_count].mip_count = hiz_mips;
    ++hiz_bar_count;
    if (curr_hiz_tex != prev_hiz_tex) {
      hiz_read[hiz_bar_count].texture = curr_hiz_tex;
      hiz_read[hiz_bar_count].before = ResourceState::Common;
      hiz_read[hiz_bar_count].after = ResourceState::ShaderRead;
      hiz_read[hiz_bar_count].mip_count = hiz_mips;
      ++hiz_bar_count;
    }
    cmd.barrier({std::span(hiz_read, hiz_bar_count), to_comp});

    // Phase 1: cull against prev-frame Hi-Z.
    cmd.bind_pipeline(cull_hiz_pipe_);
    cmd.bind_group(0, bg0);
    cmd.dispatch((static_cast<uint32_t>(items.size()) + 63u) / 64u, 1u, 1u);

    cmd.barrier({{}, {}, true, false, false});

    // Phase 2: re-test ONLY Phase-1 occluded candidates against current-frame Hi-Z.
    cmd.bind_group(0, bg1);
    cmd.dispatch((max_draws + 63u) / 64u, 1u, 1u);

    TextureBarrier hiz_back[2]{};
    for (uint32_t bi = 0; bi < hiz_bar_count; ++bi) {
      hiz_back[bi] = hiz_read[bi];
      hiz_back[bi].before = ResourceState::ShaderRead;
      hiz_back[bi].after = ResourceState::Common;
    }
    BufferBarrier rb_bar{stat_b, ResourceState::StorageReadWrite, ResourceState::CopySrc};
    cmd.barrier({std::span(hiz_back, hiz_bar_count), std::span(&rb_bar, 1)});
    cmd.copy_buffer(stat_b, 0, stat_rb, 0, 32ull);
    cmd.barrier({{}, {}, false, false, true});
  }, error);

  if (ok) {
    if (const auto* ptr = static_cast<const uint32_t*>(device_.map_buffer(stat_rb))) {
      out_stats.phase1_visible = ptr[0];
      out_stats.frustum_culled = ptr[1];
      out_stats.cone_culled = ptr[2];
      out_stats.tested = ptr[3];
      out_stats.hiz_occluded_p1 = ptr[4];
      out_stats.phase2_recovered = ptr[5];
      out_stats.total_visible = ptr[6];
      out_stats.hiz_culled_final = ptr[7];
      device_.unmap_buffer(stat_rb);
    }
  }
  cleanup();
  return ok;
}

bool ComputePostFxPipeline::readback_exposure(ExposureTelemetry& out, std::string& error) {
  out = {};
  if (!ready_) return false;
  const bool ok = device_.submit_once([&](CommandList& cmd) {
    BufferBarrier bb{exp_buf_, ResourceState::Common, ResourceState::CopySrc};
    cmd.barrier({{}, std::span(&bb, 1)});
    cmd.copy_buffer(exp_buf_, 0, exp_rb_, 0, 16ull);
    BufferBarrier back{exp_buf_, ResourceState::CopySrc, ResourceState::Common};
    cmd.barrier({{}, std::span(&back, 1), false, false, true});
  }, error);
  if (!ok) return false;
  if (const auto* p = static_cast<const uint8_t*>(device_.map_buffer(exp_rb_))) {
    std::memcpy(&out.adapted_exposure, p + 0, 4);
    std::memcpy(&out.avg_luminance, p + 4, 4);
    std::memcpy(&out.total_pixels, p + 8, 4);
    device_.unmap_buffer(exp_rb_);
    return true;
  }
  return false;
}

}  // namespace shift::rhi
