// Async Compute SPD Hi-Z Pyramid, 2-Phase Hi-Z Occlusion Culling,
// Async Compute Post-FX (Karis Bloom, 256-Bin Log-Luminance Auto-Exposure,
// McGuire Motion-Blur Tile Max), and Pre-Tonemap Linear HDR TAA-U.
//
// Runs natively across Vulkan 1.3, DirectX 12 (SM 6.6), and Metal 3 inside rhi::RenderGraph.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "rhi/meshlet.h"
#include "rhi/render_graph.h"
#include "rhi/rhi.h"

namespace shift::rhi {

struct PostFxConfig {
  uint32_t width = 0;
  uint32_t height = 0;
  float bloom_threshold = 1.0f;
  float bloom_intensity = 0.25f;
  float bloom_strength = 0.08f;
  float taa_blend_alpha = 0.12f;
  float exposure_min_log_lum = -8.0f;
  float exposure_max_log_lum = 4.0f;
  float exposure_adapt_speed = 2.5f;
  float exposure_target_key = 0.18f;
  float manual_exposure = 0.0f;  // > 0 overrides auto-exposure
  bool is_reverse_z = true;
};

struct PostFxOutputs {
  RGTextureHandle hiz_pyramid{};
  RGTextureHandle bloom_tex{};
  RGTextureHandle tile_max_vel{};
  RGTextureHandle taa_history{};
  RGTextureHandle ldr_output{};
  RGBufferHandle exposure_buf{};
};

struct ExposureTelemetry {
  float adapted_exposure = 1.0f;
  float avg_luminance = 0.0f;
  uint32_t total_pixels = 0;
};

struct TwoPhaseCullStats {
  uint32_t phase1_visible = 0;
  uint32_t frustum_culled = 0;
  uint32_t cone_culled = 0;
  uint32_t tested = 0;
  uint32_t hiz_occluded_p1 = 0;
  uint32_t phase2_recovered = 0;
  uint32_t total_visible = 0;
  uint32_t hiz_culled_final = 0;
};

class ComputePostFxPipeline {
 public:
  explicit ComputePostFxPipeline(Device& device);
  ~ComputePostFxPipeline();

  ComputePostFxPipeline(const ComputePostFxPipeline&) = delete;
  ComputePostFxPipeline& operator=(const ComputePostFxPipeline&) = delete;

  bool init(uint32_t max_width, uint32_t max_height, std::string& error);
  void shutdown();
  bool is_ready() const noexcept { return ready_; }

  void reset_history() noexcept { history_valid_ = false; }

  // Adds SPD Hi-Z (4 mips in 1 dispatch) + Async Compute Auto-Exposure Histogram +
  // Karis Bloom + McGuire Motion-Blur Tile Max + Pre-Tonemap Linear HDR TAA-U to `graph`.
  PostFxOutputs add_frame_passes(RenderGraph& graph,
                                 RGTextureHandle hdr_rg, TextureViewHandle hdr_view,
                                 RGTextureHandle depth_rg, TextureViewHandle depth_view,
                                 RGTextureHandle vel_rg, TextureViewHandle vel_view,
                                 const PostFxConfig& cfg, float dt_seconds,
                                 std::string& error);

  // Tests 2-phase Hi-Z occlusion culling against the current SPD Hi-Z pyramid.
  bool run_two_phase_cull_test(const float view_proj[16], const float camera_pos[3],
                               std::span<const MeshletInstanceGpu> instances,
                               std::span<const MeshletGpu> meshlets,
                               TextureHandle prev_hiz_tex, TextureViewHandle prev_hiz_view,
                               TextureHandle curr_hiz_tex, TextureViewHandle curr_hiz_view,
                               uint32_t hiz_w, uint32_t hiz_h, uint32_t hiz_mips,
                               TwoPhaseCullStats& out_stats, std::string& error);

  bool readback_exposure(ExposureTelemetry& out, std::string& error);

  TextureHandle hiz_texture() const noexcept { return hiz_tex_; }
  TextureViewHandle hiz_full_view() const noexcept { return hiz_full_view_; }
  TextureHandle ldr_texture() const noexcept { return ldr_tex_; }
  TextureHandle history_texture() const noexcept { return hist_tex_[hist_idx_ ^ 1u]; }
  TextureHandle tile_max_texture() const noexcept { return tile_vel_tex_; }
  uint32_t hiz_width() const noexcept { return hiz_w_; }
  uint32_t hiz_height() const noexcept { return hiz_h_; }
  uint32_t hiz_mip_count() const noexcept { return hiz_mips_; }

 private:
  Device& device_;
  bool ready_ = false;
  uint32_t max_w_ = 0, max_h_ = 0;
  uint32_t hiz_w_ = 0, hiz_h_ = 0, hiz_mips_ = 4;
  uint32_t bloom_w0_ = 0, bloom_h0_ = 0, bloom_w1_ = 0, bloom_h1_ = 0;
  uint32_t tile_w_ = 0, tile_h_ = 0;
  uint32_t hist_idx_ = 0;
  bool history_valid_ = false;

  BindGroupLayoutHandle hiz_bgl_{}, cull_hiz_bgl_{}, bloom_bgl_{}, exp_bgl_{}, mblur_bgl_{}, taa_bgl_{};
  PipelineLayoutHandle hiz_pl_{}, cull_hiz_pl_{}, bloom_pl_{}, exp_pl_{}, mblur_pl_{}, taa_pl_{};
  PipelineHandle hiz_pipe_{}, cull_hiz_pipe_{}, bloom_pipe_{}, exp_pipe_{}, mblur_pipe_{}, taa_pipe_{};

  TextureHandle hiz_tex_{};
  TextureViewHandle hiz_full_view_{};
  TextureViewHandle hiz_mip_views_[4]{};

  TextureHandle bloom_tex_[2]{};
  TextureViewHandle bloom_views_[2]{};

  TextureHandle tile_vel_tex_{};
  TextureViewHandle tile_vel_view_{};

  TextureHandle hist_tex_[2]{};
  TextureViewHandle hist_views_[2]{};

  TextureHandle ldr_tex_{};
  TextureViewHandle ldr_view_{};

  BufferHandle hist_buf_{};
  BufferHandle exp_buf_{};
  BufferHandle exp_rb_{};
};

}  // namespace shift::rhi
