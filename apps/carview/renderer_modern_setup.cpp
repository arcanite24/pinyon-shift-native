#include "renderer_internal.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "fxaa_frag_spv.h"
#include "hdr_output_frag_spv.h"
#include "inspector_preview_frag_spv.h"
#include "mblur_tile_frag_spv.h"
#include "motion_vectors_frag_spv.h"
#include "post_vert_spv.h"
#include "ssao_frag_spv.h"
#include "sscs_frag_spv.h"
#include "ssr_frag_spv.h"
#include "temporal_upscale_frag_spv.h"
#include "upscale_frag_spv.h"
#include "volumetric_frag_spv.h"

namespace carview {

void Renderer::apply_vrs(VkCommandBuffer cmd) const {
  if (!vrs_supported_ || !vkCmdSetFragmentShadingRateKHR) return;
  const VkExtent2D sz = graphics_.vrs ? VkExtent2D{2, 2} : VkExtent2D{1, 1};
  const VkFragmentShadingRateCombinerOpKHR ops[2] = {
      VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
  vkCmdSetFragmentShadingRateKHR(cmd, &sz, ops);
}

void Renderer::set_graphics_settings(const modern::GraphicsSettings& g) {
  const modern::GraphicsSettings clamped = modern::clamp_graphics(g);
  const bool need_targets =
      clamped.scale != graphics_.scale || clamped.base_540p != graphics_.base_540p ||
      clamped.ssao != graphics_.ssao || clamped.contact_shadows != graphics_.contact_shadows ||
      clamped.ssr != graphics_.ssr || clamped.volumetric_fog != graphics_.volumetric_fog ||
      clamped.velocity_blur != graphics_.velocity_blur || clamped.hdr != graphics_.hdr ||
      clamped.hdr_compare != graphics_.hdr_compare ||
      clamped.aa != graphics_.aa || clamped.upscaler != graphics_.upscaler ||
      clamped.rt_mode != graphics_.rt_mode;
  const bool hdr_changed = clamped.hdr != graphics_.hdr;
  graphics_ = clamped;
  lut_strength_ = graphics_.lut_strength / 100.0f;
  set_vsync(graphics_.vsync);
  if (need_targets) targets_dirty_ = true;
  if (hdr_changed && options_.window) swapchain_dirty_ = true;
}

void Renderer::set_camera_projection(float near_plane, float far_plane, float proj00, float proj11) {
  cam_near_ = std::max(near_plane, 0.01f);
  cam_far_ = std::max(far_plane, cam_near_ + 1.0f);
  cam_proj00_ = proj00;
  cam_proj11_ = proj11;
}

void Renderer::set_camera_vp(const float vp[16], float jitter_uv_x, float jitter_uv_y) {
  if (!vp) return;
  std::memcpy(cam_vp_, vp, sizeof(cam_vp_));
  cam_jitter_uv_x_ = jitter_uv_x;
  cam_jitter_uv_y_ = jitter_uv_y;
  cam_vp_set_ = true;
}

void Renderer::set_inspector_preview(bool active, InspectorView view) {
  if (active != inspector_active_ || (active && view != inspector_view_)) targets_dirty_ = true;
  inspector_active_ = active;
  inspector_view_ = view;
  if (!active) inspector_has_rgba_ = false;
}

void Renderer::set_hiz_preview_view(VkImageView view) {
  if (view == hiz_preview_view_) return;
  hiz_preview_view_ = view;
  if (!modern_ready_ || !inspector_sets_[5] || !post_depth_sampler_ || !post_sampler_ || !depth_resolved_.view) return;
  if (options_.window && device_) wait_idle();
  const VkDescriptorImageInfo info =
      hiz_preview_view_ ? VkDescriptorImageInfo{post_sampler_, hiz_preview_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                        : VkDescriptorImageInfo{post_depth_sampler_, depth_resolved_.view,
                                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
  const VkDescriptorImageInfo hiz_nearest =
      hiz_preview_view_ ? VkDescriptorImageInfo{post_depth_sampler_, hiz_preview_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                        : VkDescriptorImageInfo{post_depth_sampler_, depth_resolved_.view,
                                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet writes[3]{};
  uint32_t count = 0;
  writes[count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[count].dstSet = inspector_sets_[5];
  writes[count].dstBinding = 0;
  writes[count].descriptorCount = 1;
  writes[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[count].pImageInfo = &info;
  ++count;
  for (int i = 0; i < 2; ++i) {
    if (!ssr_sets_[i]) continue;
    writes[count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[count].dstSet = ssr_sets_[i];
    writes[count].dstBinding = 2;
    writes[count].descriptorCount = 1;
    writes[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[count].pImageInfo = &hiz_nearest;
    ++count;
  }
  vkUpdateDescriptorSets(device_, count, writes, 0, nullptr);
}

bool Renderer::take_inspector_preview(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
  if (!inspector_has_rgba_ || inspector_rgba_.empty()) return false;
  rgba = inspector_rgba_;
  width = kInspectorPreviewW;
  height = kInspectorPreviewH;
  inspector_has_rgba_ = false;
  return true;
}

bool Renderer::create_modern_passes(std::string& error) {
  if (modern_ready_) return true;
  if (!shadow_map_.view && !ensure_shadow_map(error)) return false;
  // 1x1 neutral fallback textures:
  //   ao_white_:     RGBA8 {1.0, 0.5, 0.5, 1.0} (full visibility + neutral +Z view bent normal)
  //   vel_zero_:     RGBA16F {0, 0, 0, 1000} (zero motion vector, 0 reactive, far depth)
  //   ssr_zero_:     RGBA16F {0, 0, 0, 0} (no screen-space reflection)
  //   vol_identity_: RGBA16F {0, 0, 0, 1} (zero in-scatter, 1.0 transmittance)
  if (!ao_white_.image) {
    if (!create_image(1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                      ao_white_) ||
        !create_image(1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                      vel_zero_) ||
        !create_image(1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                      ssr_zero_) ||
        !create_image(1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                      vol_identity_)) {
      error = "modern fallback targets";
      return false;
    }
    const bool ok = submit_once([&](VkCommandBuffer cmd) {
      const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      const VkClearColorValue neutral_ao{{1.0f, 0.5f, 0.5f, 1.0f}};
      const VkClearColorValue zero_vel{{0.0f, 0.0f, 0.0f, 1000.0f}};
      const VkClearColorValue zero_ssr{{0.0f, 0.0f, 0.0f, 0.0f}};
      const VkClearColorValue ident_vol{{0.0f, 0.0f, 0.0f, 1.0f}};
      auto init_clear = [&](const Image& img, const VkClearColorValue& cv) {
        image_barrier(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rng);
        image_barrier(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      };
      init_clear(ao_white_, neutral_ao);
      init_clear(vel_zero_, zero_vel);
      init_clear(ssr_zero_, zero_ssr);
      init_clear(vol_identity_, ident_vol);
    });
    if (!ok) {
      error = "modern fallback clear";
      return false;
    }
  }
  if (!vol_ubo_.buffer &&
      !create_buffer(sizeof(VolumetricState), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vol_ubo_)) {
    error = "volumetric UBO";
    return false;
  }
  const VkDescriptorSetLayoutBinding ssao_bindings[2] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo ssao_set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  ssao_set_info.bindingCount = 2;
  ssao_set_info.pBindings = ssao_bindings;
  VK_TRY(vkCreateDescriptorSetLayout(device_, &ssao_set_info, nullptr, &ssao_set_layout_), "ssao set layout");

  const VkDescriptorSetLayoutBinding tex_binding{
      0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo tex_set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  tex_set_info.bindingCount = 1;
  tex_set_info.pBindings = &tex_binding;
  VK_TRY(vkCreateDescriptorSetLayout(device_, &tex_set_info, nullptr, &single_tex_set_layout_),
         "single tex set layout");

  const VkDescriptorSetLayoutBinding temporal_bindings[3] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo temp_set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  temp_set_info.bindingCount = 3;
  temp_set_info.pBindings = temporal_bindings;
  VK_TRY(vkCreateDescriptorSetLayout(device_, &temp_set_info, nullptr, &temporal_set_layout_), "temporal set layout");

  const VkDescriptorSetLayoutBinding ssr_bindings[4] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo ssr_set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  ssr_set_info.bindingCount = 4;
  ssr_set_info.pBindings = ssr_bindings;
  VK_TRY(vkCreateDescriptorSetLayout(device_, &ssr_set_info, nullptr, &ssr_set_layout_), "ssr set layout");

  const VkDescriptorSetLayoutBinding vol_bindings[4] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo vol_set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  vol_set_info.bindingCount = 4;
  vol_set_info.pBindings = vol_bindings;
  VK_TRY(vkCreateDescriptorSetLayout(device_, &vol_set_info, nullptr, &vol_set_layout_), "volumetric set layout");

  auto make_layout = [&](VkDescriptorSetLayout set_layout, uint32_t push_bytes, VkPipelineLayout& out,
                         const char* what) -> bool {
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_bytes};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 1;
    info.pSetLayouts = &set_layout;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    VK_TRY(vkCreatePipelineLayout(device_, &info, nullptr, &out), what);
    return true;
  };
  if (!make_layout(ssao_set_layout_, 48, ssao_layout_, "ssao layout") ||
      !make_layout(single_tex_set_layout_, 48, sscs_layout_, "sscs layout") ||
      !make_layout(ssr_set_layout_, 64, ssr_layout_, "ssr layout") ||
      !make_layout(vol_set_layout_, 32, vol_layout_, "volumetric layout") ||
      !make_layout(single_tex_set_layout_, 16, mblur_tile_layout_, "mblur tile layout") ||
      !make_layout(single_tex_set_layout_, 16, fxaa_layout_, "fxaa layout") ||
      !make_layout(single_tex_set_layout_, 48, upscale_layout_, "upscale layout") ||
      !make_layout(single_tex_set_layout_, 48, hdr_layout_, "hdr layout") ||
      !make_layout(single_tex_set_layout_, 80, motion_layout_, "motion layout") ||
      !make_layout(temporal_set_layout_, 48, temporal_layout_, "temporal layout") ||
      !make_layout(single_tex_set_layout_, 16, inspector_layout_, "inspector layout"))
    return false;

  const VkDescriptorPoolSize pool_sizes[2] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 96},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 40;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = pool_sizes;
  VK_TRY(vkCreateDescriptorPool(device_, &pool_info, nullptr, &modern_pool_), "modern descriptor pool");

  VkDescriptorSetLayout ssao_layouts[3] = {ssao_set_layout_, ssao_set_layout_, ssao_set_layout_};
  VkDescriptorSetAllocateInfo salloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  salloc.descriptorPool = modern_pool_;
  salloc.descriptorSetCount = 3;
  salloc.pSetLayouts = ssao_layouts;
  VK_TRY(vkAllocateDescriptorSets(device_, &salloc, ssao_sets_), "ssao descriptor sets");

  VkDescriptorSetLayout ssr_layouts[2] = {ssr_set_layout_, ssr_set_layout_};
  VkDescriptorSetAllocateInfo ssr_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ssr_alloc.descriptorPool = modern_pool_;
  ssr_alloc.descriptorSetCount = 2;
  ssr_alloc.pSetLayouts = ssr_layouts;
  VK_TRY(vkAllocateDescriptorSets(device_, &ssr_alloc, ssr_sets_), "ssr descriptor sets");

  VkDescriptorSetLayout vol_layouts[2] = {vol_set_layout_, vol_set_layout_};
  VkDescriptorSetAllocateInfo vol_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  vol_alloc.descriptorPool = modern_pool_;
  vol_alloc.descriptorSetCount = 2;
  vol_alloc.pSetLayouts = vol_layouts;
  VK_TRY(vkAllocateDescriptorSets(device_, &vol_alloc, vol_sets_), "volumetric descriptor sets");

  VkDescriptorSetLayout tex_layouts[19];
  for (int i = 0; i < 19; ++i) tex_layouts[i] = single_tex_set_layout_;
  VkDescriptorSet tex_sets[19] = {};
  VkDescriptorSetAllocateInfo talloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  talloc.descriptorPool = modern_pool_;
  talloc.descriptorSetCount = 19;
  talloc.pSetLayouts = tex_layouts;
  VK_TRY(vkAllocateDescriptorSets(device_, &talloc, tex_sets), "modern tex descriptor sets");
  fxaa_set_scene_ = tex_sets[0];
  fxaa_set_standin_ = tex_sets[1];
  upscale_set_scene_ = tex_sets[2];
  upscale_set_fxaa_ = tex_sets[3];
  upscale_set_tmp_ = tex_sets[4];
  hdr_set_standin_ = tex_sets[5];
  hdr_set_fxaa_ = tex_sets[6];
  motion_set_ = tex_sets[7];
  upscale_set_hist_[0] = tex_sets[8];
  upscale_set_hist_[1] = tex_sets[9];
  for (int i = 0; i < 6; ++i) inspector_sets_[i] = tex_sets[10 + i];
  sscs_set_ = tex_sets[16];
  mblur_tile_set_ = tex_sets[17];
  mblur_neighbor_set_ = tex_sets[18];

  VkDescriptorSetLayout temp_layouts[4] = {temporal_set_layout_, temporal_set_layout_, temporal_set_layout_,
                                           temporal_set_layout_};
  VkDescriptorSetAllocateInfo tmp_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  tmp_alloc.descriptorPool = modern_pool_;
  tmp_alloc.descriptorSetCount = 4;
  tmp_alloc.pSetLayouts = temp_layouts;
  VK_TRY(vkAllocateDescriptorSets(device_, &tmp_alloc, temporal_sets_), "temporal descriptor sets");

  VkShaderModule shared_vert = VK_NULL_HANDLE;
  {
    VkShaderModuleCreateInfo vinfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    vinfo.codeSize = sizeof(kPost_vert_spv);
    vinfo.pCode = kPost_vert_spv;
    VK_TRY(vkCreateShaderModule(device_, &vinfo, nullptr, &shared_vert), "modern vert");
  }

  std::vector<std::function<VkResult(VkPipelineCache)>> tasks;
  tasks.reserve(12);
  auto queue_fs_pipe = [&](const uint32_t* frag_code, size_t frag_bytes, VkPipelineLayout layout, VkFormat fmt,
                           VkPipeline& out) {
    VkPipeline* dst = &out;
    tasks.emplace_back([this, shared_vert, frag_code, frag_bytes, layout, fmt, dst](VkPipelineCache cache) -> VkResult {
      VkShaderModuleCreateInfo finfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      finfo.codeSize = frag_bytes;
      finfo.pCode = frag_code;
      VkShaderModule frag = VK_NULL_HANDLE;
      VkResult res = vkCreateShaderModule(device_, &finfo, nullptr, &frag);
      if (res != VK_SUCCESS) return res;
      const VkPipelineShaderStageCreateInfo stages[2] = {
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, shared_vert,
           "main", nullptr},
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main",
           nullptr}};
      VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
      ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
      vp.viewportCount = vp.scissorCount = 1;
      VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
      rs.polygonMode = VK_POLYGON_MODE_FILL;
      rs.cullMode = VK_CULL_MODE_NONE;
      rs.lineWidth = 1.0f;
      VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      VkPipelineColorBlendAttachmentState att{};
      att.colorWriteMask =
          VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
      VkPipelineColorBlendStateCreateInfo bl{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      bl.attachmentCount = 1;
      bl.pAttachments = &att;
      const VkDynamicState dstates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
      dy.dynamicStateCount = 2;
      dy.pDynamicStates = dstates;
      const VkFormat color_fmt = fmt;
      VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
      rend.colorAttachmentCount = 1;
      rend.pColorAttachmentFormats = &color_fmt;
      VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      pi.pNext = &rend;
      pi.stageCount = 2;
      pi.pStages = stages;
      pi.pVertexInputState = &vi;
      pi.pInputAssemblyState = &ia;
      pi.pViewportState = &vp;
      pi.pRasterizationState = &rs;
      pi.pMultisampleState = &ms;
      pi.pDepthStencilState = &ds;
      pi.pColorBlendState = &bl;
      pi.pDynamicState = &dy;
      pi.layout = layout;
      res = vkCreateGraphicsPipelines(device_, cache, 1, &pi, nullptr, dst);
      vkDestroyShaderModule(device_, frag, nullptr);
      return res;
    });
  };
  queue_fs_pipe(kSsao_frag_spv, sizeof(kSsao_frag_spv), ssao_layout_, VK_FORMAT_R8G8B8A8_UNORM, ssao_pipe_);
  queue_fs_pipe(kSscs_frag_spv, sizeof(kSscs_frag_spv), sscs_layout_, VK_FORMAT_R8_UNORM, sscs_pipe_);
  queue_fs_pipe(kSsr_frag_spv, sizeof(kSsr_frag_spv), ssr_layout_, VK_FORMAT_R16G16B16A16_SFLOAT, ssr_pipe_);
  queue_fs_pipe(kVolumetric_frag_spv, sizeof(kVolumetric_frag_spv), vol_layout_, VK_FORMAT_R16G16B16A16_SFLOAT,
                vol_pipe_);
  queue_fs_pipe(kMblur_tile_frag_spv, sizeof(kMblur_tile_frag_spv), mblur_tile_layout_, VK_FORMAT_R16G16_SFLOAT,
                mblur_tile_pipe_);
  queue_fs_pipe(kFxaa_frag_spv, sizeof(kFxaa_frag_spv), fxaa_layout_, VK_FORMAT_R8G8B8A8_UNORM, fxaa_pipe_);
  queue_fs_pipe(kUpscale_frag_spv, sizeof(kUpscale_frag_spv), upscale_layout_, VK_FORMAT_R8G8B8A8_UNORM, upscale_pipe_);
  queue_fs_pipe(kMotion_vectors_frag_spv, sizeof(kMotion_vectors_frag_spv), motion_layout_,
                VK_FORMAT_R16G16B16A16_SFLOAT, motion_pipe_);
  queue_fs_pipe(kTemporal_upscale_frag_spv, sizeof(kTemporal_upscale_frag_spv), temporal_layout_,
                VK_FORMAT_R8G8B8A8_UNORM, temporal_pipe_);
  queue_fs_pipe(kInspector_preview_frag_spv, sizeof(kInspector_preview_frag_spv), inspector_layout_,
                VK_FORMAT_R8G8B8A8_UNORM, inspector_pipe_);
  queue_fs_pipe(kHdr_output_frag_spv, sizeof(kHdr_output_frag_spv), hdr_layout_, VK_FORMAT_R8G8B8A8_UNORM,
                hdr_pipe_sdr_);
  if (color_format_ != VK_FORMAT_R8G8B8A8_UNORM) {
    queue_fs_pipe(kHdr_output_frag_spv, sizeof(kHdr_output_frag_spv), hdr_layout_, color_format_, hdr_pipe_swap_);
  }

  const bool pipes_ok = compile_pipelines_parallel(tasks, error);
  vkDestroyShaderModule(device_, shared_vert, nullptr);
  if (!pipes_ok) return false;
  if (color_format_ == VK_FORMAT_R8G8B8A8_UNORM) {
    hdr_pipe_swap_ = hdr_pipe_sdr_;
  }
  hdr_swap_fmt_ = color_format_;
  modern_ready_ = true;
  return true;
}

void Renderer::destroy_modern_passes() {
  if (!device_) return;
  if (hdr_pipe_swap_ && hdr_pipe_swap_ != hdr_pipe_sdr_) vkDestroyPipeline(device_, hdr_pipe_swap_, nullptr);
  hdr_pipe_swap_ = VK_NULL_HANDLE;
  hdr_swap_fmt_ = VK_FORMAT_UNDEFINED;
  if (hdr_pipe_sdr_) vkDestroyPipeline(device_, hdr_pipe_sdr_, nullptr), hdr_pipe_sdr_ = VK_NULL_HANDLE;
  if (inspector_pipe_) vkDestroyPipeline(device_, inspector_pipe_, nullptr), inspector_pipe_ = VK_NULL_HANDLE;
  if (temporal_pipe_) vkDestroyPipeline(device_, temporal_pipe_, nullptr), temporal_pipe_ = VK_NULL_HANDLE;
  if (motion_pipe_) vkDestroyPipeline(device_, motion_pipe_, nullptr), motion_pipe_ = VK_NULL_HANDLE;
  if (mblur_tile_pipe_) vkDestroyPipeline(device_, mblur_tile_pipe_, nullptr), mblur_tile_pipe_ = VK_NULL_HANDLE;
  if (upscale_pipe_) vkDestroyPipeline(device_, upscale_pipe_, nullptr), upscale_pipe_ = VK_NULL_HANDLE;
  if (fxaa_pipe_) vkDestroyPipeline(device_, fxaa_pipe_, nullptr), fxaa_pipe_ = VK_NULL_HANDLE;
  if (vol_pipe_) vkDestroyPipeline(device_, vol_pipe_, nullptr), vol_pipe_ = VK_NULL_HANDLE;
  if (ssr_pipe_) vkDestroyPipeline(device_, ssr_pipe_, nullptr), ssr_pipe_ = VK_NULL_HANDLE;
  if (sscs_pipe_) vkDestroyPipeline(device_, sscs_pipe_, nullptr), sscs_pipe_ = VK_NULL_HANDLE;
  if (ssao_pipe_) vkDestroyPipeline(device_, ssao_pipe_, nullptr), ssao_pipe_ = VK_NULL_HANDLE;
  if (inspector_layout_) vkDestroyPipelineLayout(device_, inspector_layout_, nullptr), inspector_layout_ = VK_NULL_HANDLE;
  if (temporal_layout_) vkDestroyPipelineLayout(device_, temporal_layout_, nullptr), temporal_layout_ = VK_NULL_HANDLE;
  if (motion_layout_) vkDestroyPipelineLayout(device_, motion_layout_, nullptr), motion_layout_ = VK_NULL_HANDLE;
  if (mblur_tile_layout_) vkDestroyPipelineLayout(device_, mblur_tile_layout_, nullptr), mblur_tile_layout_ = VK_NULL_HANDLE;
  if (hdr_layout_) vkDestroyPipelineLayout(device_, hdr_layout_, nullptr), hdr_layout_ = VK_NULL_HANDLE;
  if (upscale_layout_) vkDestroyPipelineLayout(device_, upscale_layout_, nullptr), upscale_layout_ = VK_NULL_HANDLE;
  if (fxaa_layout_) vkDestroyPipelineLayout(device_, fxaa_layout_, nullptr), fxaa_layout_ = VK_NULL_HANDLE;
  if (vol_layout_) vkDestroyPipelineLayout(device_, vol_layout_, nullptr), vol_layout_ = VK_NULL_HANDLE;
  if (ssr_layout_) vkDestroyPipelineLayout(device_, ssr_layout_, nullptr), ssr_layout_ = VK_NULL_HANDLE;
  if (sscs_layout_) vkDestroyPipelineLayout(device_, sscs_layout_, nullptr), sscs_layout_ = VK_NULL_HANDLE;
  if (ssao_layout_) vkDestroyPipelineLayout(device_, ssao_layout_, nullptr), ssao_layout_ = VK_NULL_HANDLE;
  if (modern_pool_) vkDestroyDescriptorPool(device_, modern_pool_, nullptr), modern_pool_ = VK_NULL_HANDLE;
  if (vol_set_layout_)
    vkDestroyDescriptorSetLayout(device_, vol_set_layout_, nullptr), vol_set_layout_ = VK_NULL_HANDLE;
  if (ssr_set_layout_)
    vkDestroyDescriptorSetLayout(device_, ssr_set_layout_, nullptr), ssr_set_layout_ = VK_NULL_HANDLE;
  if (temporal_set_layout_)
    vkDestroyDescriptorSetLayout(device_, temporal_set_layout_, nullptr), temporal_set_layout_ = VK_NULL_HANDLE;
  if (single_tex_set_layout_)
    vkDestroyDescriptorSetLayout(device_, single_tex_set_layout_, nullptr), single_tex_set_layout_ = VK_NULL_HANDLE;
  if (ssao_set_layout_)
    vkDestroyDescriptorSetLayout(device_, ssao_set_layout_, nullptr), ssao_set_layout_ = VK_NULL_HANDLE;
  destroy_buffer(vol_ubo_);
  destroy_image(inspector_thumb_);
  destroy_image(vol_identity_);
  destroy_image(ssr_zero_);
  destroy_image(vel_zero_);
  destroy_image(ao_white_);
  modern_ready_ = false;
}

void Renderer::update_modern_descriptors() {
  if (!modern_ready_ || !depth_resolved_.view) return;
  const VkImageView fb = ao_white_.view;
  const VkImageView vel_fb = vel_zero_.view ? vel_zero_.view : fb;
  auto vv = [&](const Image& img) { return img.view ? img.view : fb; };
  auto vvel = [&](const Image& img) { return img.view ? img.view : vel_fb; };
  const VkImageView ao_view =
      (graphics_.ssao != modern::SsaoQuality::kOff && ao_blur_.view) ? ao_blur_.view : ao_white_.view;
  const VkImageView shadow_view = shadow_map_.view ? shadow_map_.view : depth_resolved_.view;
  VkDescriptorImageInfo ssao_imgs[6] = {
      {post_sampler_, ao_white_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(ao_raw_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(ao_tmp_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
  };
  VkDescriptorImageInfo tex_imgs[19] = {
      {post_sampler_, vv(scene_sdr_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(stand_in_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(scene_sdr_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(scene_fxaa_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(upscale_tmp_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(stand_in_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(stand_in_fxaa_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(temporal_hist_[0]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(temporal_hist_[1]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(scene_sdr_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_sampler_, ao_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, shadow_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_sampler_, vvel(motion_vec_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      hiz_preview_view_
          ? VkDescriptorImageInfo{post_sampler_, hiz_preview_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
          : VkDescriptorImageInfo{post_depth_sampler_, depth_resolved_.view,
                                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_sampler_, vvel(motion_vec_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vvel(mblur_tile_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
  };
  VkDescriptorSet tex_sets[19] = {fxaa_set_scene_,      fxaa_set_standin_,  upscale_set_scene_,   upscale_set_fxaa_,
                                  upscale_set_tmp_,     hdr_set_standin_,   hdr_set_fxaa_,        motion_set_,
                                  upscale_set_hist_[0], upscale_set_hist_[1], inspector_sets_[0], inspector_sets_[1],
                                  inspector_sets_[2],   inspector_sets_[3], inspector_sets_[4],   inspector_sets_[5],
                                  sscs_set_,            mblur_tile_set_,    mblur_neighbor_set_};
  VkDescriptorImageInfo temp_imgs[12] = {
      {post_sampler_, vv(scene_sdr_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(temporal_hist_[1]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vvel(motion_vec_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(scene_sdr_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(temporal_hist_[0]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vvel(motion_vec_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(scene_fxaa_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(temporal_hist_[1]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vvel(motion_vec_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(scene_fxaa_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vv(temporal_hist_[0]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, vvel(motion_vec_), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
  };
  VkWriteDescriptorSet writes[37]{};
  for (int i = 0; i < 6; ++i) {
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = ssao_sets_[i / 2];
    writes[i].dstBinding = uint32_t(i % 2);
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &ssao_imgs[i];
  }
  for (int i = 0; i < 19; ++i) {
    writes[6 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[6 + i].dstSet = tex_sets[i];
    writes[6 + i].dstBinding = 0;
    writes[6 + i].descriptorCount = 1;
    writes[6 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[6 + i].pImageInfo = &tex_imgs[i];
  }
  for (int i = 0; i < 12; ++i) {
    writes[25 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[25 + i].dstSet = temporal_sets_[i / 3];
    writes[25 + i].dstBinding = uint32_t(i % 3);
    writes[25 + i].descriptorCount = 1;
    writes[25 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[25 + i].pImageInfo = &temp_imgs[i];
  }
  vkUpdateDescriptorSets(device_, 37, writes, 0, nullptr);

  // Phase B SSR & Volumetric Scattering descriptor sets.
  const VkImageView hdr_view = hdr_.view ? hdr_.view : fb;
  const VkImageView ssr_fb = ssr_zero_.view ? ssr_zero_.view : fb;
  const VkImageView vol_fb = vol_identity_.view ? vol_identity_.view : fb;
  const VkDescriptorImageInfo hiz_nearest =
      hiz_preview_view_
          ? VkDescriptorImageInfo{post_depth_sampler_, hiz_preview_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
          : VkDescriptorImageInfo{post_depth_sampler_, depth_resolved_.view,
                                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
  VkDescriptorImageInfo ssr_imgs[8] = {
      {post_sampler_, hdr_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      hiz_nearest,
      {post_sampler_, ssr_fb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_sampler_, hdr_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      hiz_nearest,
      {post_sampler_, ssr_raw_.view ? ssr_raw_.view : ssr_fb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
  };
  const VkImageView vol_sh_view = (shadow_map_.view && shadow_sampler_) ? shadow_map_.view : depth_resolved_.view;
  const VkSampler vol_sh_samp = shadow_sampler_ ? shadow_sampler_ : post_depth_sampler_;
  VkDescriptorImageInfo vol_imgs[6] = {
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {vol_sh_samp, vol_sh_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_sampler_, vol_fb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {vol_sh_samp, vol_sh_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {post_sampler_, vol_raw_.view ? vol_raw_.view : vol_fb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
  };
  VkDescriptorBufferInfo vol_buf_info{vol_ubo_.buffer, 0, sizeof(VolumetricState)};
  VkWriteDescriptorSet b_writes[16]{};
  for (int i = 0; i < 8; ++i) {
    b_writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    b_writes[i].dstSet = ssr_sets_[i / 4];
    b_writes[i].dstBinding = uint32_t(i % 4);
    b_writes[i].descriptorCount = 1;
    b_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b_writes[i].pImageInfo = &ssr_imgs[i];
  }
  for (int s = 0; s < 2; ++s) {
    for (int b = 0; b < 3; ++b) {
      const int idx = 8 + s * 4 + b;
      b_writes[idx] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      b_writes[idx].dstSet = vol_sets_[s];
      b_writes[idx].dstBinding = uint32_t(b);
      b_writes[idx].descriptorCount = 1;
      b_writes[idx].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      b_writes[idx].pImageInfo = &vol_imgs[s * 3 + b];
    }
    const int uidx = 8 + s * 4 + 3;
    b_writes[uidx] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    b_writes[uidx].dstSet = vol_sets_[s];
    b_writes[uidx].dstBinding = 3;
    b_writes[uidx].descriptorCount = 1;
    b_writes[uidx].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b_writes[uidx].pBufferInfo = &vol_buf_info;
  }
  vkUpdateDescriptorSets(device_, 16, b_writes, 0, nullptr);
}


}  // namespace carview
