// Vulkan Hardware Ray Tracing & Path Tracing Compute Pipeline Host (RT-2, RT-3, RT-4):
// Manages inline VK_KHR_ray_query Hybrid RT (Reflections, Sun/Headlight Soft Shadows, RTAO),
// Full Real-Time Path Tracing (ReSTIR DI + Multi-Bounce GI + Automotive Clearcoat/Glass Optics),
// Built-In Zero-Dependency SVGF Spatiotemporal Denoiser (Temporal + A-Trous Wavelet),
// and Convergent Progressive Multi-SPP Photo Mode Path Tracing.
#pragma once

#include <volk.h>

#include <cstdint>
#include <string>

#include "renderer.h"
#include "rt_logic.h"

namespace carview {

class RtScene;
class Showroom;

class RtPipeline {
 public:
  explicit RtPipeline(Renderer& r);
  RtPipeline(const RtPipeline&) = delete;
  RtPipeline& operator=(const RtPipeline&) = delete;
  ~RtPipeline();

  bool init(VkDescriptorSetLayout world_bindless_layout, VkSampler world_sampler, VkImageView fallback_white,
            std::string& error);
  void shutdown();
  bool ready() const { return ready_; }

  // Resets SVGF temporal history and Photo Mode progressive accumulation counter.
  void reset_history() {
    history_valid_ = false;
    photo_accum_count_ = 0;
  }
  uint32_t photo_accum_samples() const { return photo_accum_count_; }

  // Records Hybrid RT, Real-Time Path Tracing, or Photo Mode PT into `cmd`.
  // `hdr_img` must be in VK_IMAGE_LAYOUT_GENERAL; `depth_view` in DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  // `motion_view` in SHADER_READ_ONLY_OPTIMAL.
  void record(VkCommandBuffer cmd, const Showroom& host, const RtScene& scene, VkImage hdr_img,
              VkImageView hdr_view, VkImageView depth_view, VkImageView motion_view, uint32_t width,
              uint32_t height);

 private:
  struct RtFrameUboGpu {
    float view_proj[16];
    float inv_view_proj[16];
    float prev_view_proj[16];
    float eye_frame[4];
    float sun_dir_elev[4];
    float sun_color_night[4];
    float sky_color_exp[4];
    float ground_color_revz[4];
    float fog_color_start[4];
    float fog_params[4];
    float env_basis_x[4];
    float env_basis_y[4];
    float env_basis_z[4];
    float rt_params[4];
    float paint_two_tone[4];
    float paint_finish[4];
  };
  static_assert(sizeof(RtFrameUboGpu) == 400);

  struct RtReservoirGpu {
    uint32_t light_idx;
    float weight_sum;
    float W;
    uint32_t M;
  };
  static_assert(sizeof(RtReservoirGpu) == 16);

  bool ensure_targets(uint32_t width, uint32_t height, VkCommandBuffer cmd);
  void destroy_targets();
  bool create_compute_pipeline(const uint32_t* spv, size_t spv_bytes, VkPipeline& out, std::string& error);

  Renderer& r_;
  bool ready_ = false;
  uint32_t width_ = 0, height_ = 0;
  uint32_t frame_index_ = 0;
  uint32_t hist_idx_ = 0;
  bool history_valid_ = false;
  uint32_t photo_accum_count_ = 0;
  float last_vp_[16] = {};
  bool have_last_vp_ = false;

  VkDescriptorSetLayout rt_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout fallback_bindless_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool fallback_bindless_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet fallback_bindless_set_ = VK_NULL_HANDLE;
  VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE;

  VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet desc_sets_[Renderer::kFramesInFlight] = {};
  VkSampler linear_sampler_ = VK_NULL_HANDLE;
  VkSampler nearest_sampler_ = VK_NULL_HANDLE;

  Renderer::Buffer ubo_buf_[Renderer::kFramesInFlight];
  void* ubo_mapped_[Renderer::kFramesInFlight] = {};

  VkPipeline refl_pipe_ = VK_NULL_HANDLE;
  VkPipeline shad_pipe_ = VK_NULL_HANDLE;
  VkPipeline ao_pipe_ = VK_NULL_HANDLE;
  VkPipeline pt_pipe_ = VK_NULL_HANDLE;
  VkPipeline svgf_temp_pipe_ = VK_NULL_HANDLE;
  VkPipeline svgf_atrous_pipe_ = VK_NULL_HANDLE;
  VkPipeline comp_pipe_ = VK_NULL_HANDLE;

  // Resolution-dependent working targets (allocated only when RT != kOff).
  Renderer::Image gbuf_norm_;
  Renderer::Image gbuf_albedo_;
  Renderer::Image refl_img_;
  Renderer::Image shad_ao_img_;
  Renderer::Image pt_img_;
  Renderer::Image hist_img_[2];
  Renderer::Image svgf_tmp_;
  Renderer::Image moments_img_[2];
  Renderer::Buffer reservoirs_[2];
};

}  // namespace carview
