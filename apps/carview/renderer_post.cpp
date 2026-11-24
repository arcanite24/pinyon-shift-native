#include "renderer_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "bloom_down_frag_spv.h"
#include "bloom_up_frag_spv.h"
#include "post_frag_spv.h"
#include "post_vert_spv.h"

namespace carview {

void Renderer::set_post_lut(std::span<const uint8_t> rgba, uint32_t size) {
  if (size < 2 || size > 65) return;
  const size_t want = size_t(size) * size * size * 4u;
  if (rgba.size() < want) return;
  lut_pending_.assign(rgba.begin(), rgba.begin() + ptrdiff_t(want));
  lut_pending_size_ = size;
  lut_dirty_ = true;
}

bool Renderer::ensure_lut_image(uint32_t size, std::string& error) {
  if (size < 2 || size > 65) return false;
  if (lut3d_.image && lut_image_size_ == size) return true;
  if (device_) wait_idle();
  destroy_image(lut3d_);
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_3D;
  info.format = VK_FORMAT_R8G8B8A8_UNORM;
  info.extent = {size, size, size};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  VmaAllocationInfo alloc_info{};
  VK_TRY(vmaCreateImage(allocator_, &info, &alloc, &lut3d_.image, &lut3d_.allocation, &alloc_info),
         "vmaCreateImage (3D LUT)");
  lut3d_.bytes = alloc_info.size;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = lut3d_.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_3D;
  view.format = VK_FORMAT_R8G8B8A8_UNORM;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VK_TRY(vkCreateImageView(device_, &view, nullptr, &lut3d_.view), "vkCreateImageView (3D LUT)");
  lut_image_size_ = size;
  if (!submit_once([&](VkCommandBuffer cmd) {
        image_barrier(cmd, lut3d_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      })) {
    error = "3D LUT layout transition";
    return false;
  }
  if (post_pool_ && post_sampler_) {
    std::vector<VkDescriptorImageInfo> imgs(kPostSetCount);
    std::vector<VkWriteDescriptorSet> writes(kPostSetCount);
    for (uint32_t s = 0; s < kPostSetCount; ++s) {
      imgs[s] = {post_sampler_, lut3d_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      writes[s] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[s].dstSet = post_sets_[s];
      writes[s].dstBinding = 2;
      writes[s].descriptorCount = 1;
      writes[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[s].pImageInfo = &imgs[s];
    }
    vkUpdateDescriptorSets(device_, kPostSetCount, writes.data(), 0, nullptr);
  }
  return true;
}

bool Renderer::create_post(std::string& error) {
  if (post_final_) return true;
  // Create auto-exposure targets (16x16 + 1x1 ping-pong) and 16x16x16 3D LUT once.
  const VkImageUsageFlags lum_usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!create_image(16, 16, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, lum_usage, VK_IMAGE_ASPECT_COLOR_BIT, lum_16_) ||
      !create_image(1, 1, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, lum_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                    lum_adapt_[0]) ||
      !create_image(1, 1, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, lum_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                    lum_adapt_[1])) {
    error = "post luminance targets";
    return false;
  }
  // Create 16x16x16 3D RGBA8 LUT image + host-visible staging buffer.
  {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_3D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {16, 16, 16};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    VmaAllocationInfo alloc_info{};
    VK_TRY(vmaCreateImage(allocator_, &info, &alloc, &lut3d_.image, &lut3d_.allocation, &alloc_info),
           "vmaCreateImage (3D LUT)");
    lut3d_.bytes = alloc_info.size;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = lut3d_.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_3D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_TRY(vkCreateImageView(device_, &view, nullptr, &lut3d_.view), "vkCreateImageView (3D LUT)");

    constexpr VkDeviceSize kLutBytes = 16 * 16 * 16 * 4;
    Buffer lut_staging;
    if (!create_buffer(kLutBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, lut_staging)) {
      error = "3D LUT staging buffer";
      return false;
    }
    void* lut_staging_mapped = nullptr;
    VK_TRY(vmaMapMemory(allocator_, lut_staging.allocation, &lut_staging_mapped), "map 3D LUT staging");
    // Fill identity ramp by default.
    auto* dst = static_cast<uint8_t*>(lut_staging_mapped);
    for (uint32_t b = 0; b < 16; ++b)
      for (uint32_t g = 0; g < 16; ++g)
        for (uint32_t r = 0; r < 16; ++r) {
          const size_t o = ((size_t(b) * 16 + g) * 16 + r) * 4;
          dst[o + 0] = uint8_t(std::lround(double(r) * 255.0 / 15.0));
          dst[o + 1] = uint8_t(std::lround(double(g) * 255.0 / 15.0));
          dst[o + 2] = uint8_t(std::lround(double(b) * 255.0 / 15.0));
          dst[o + 3] = 255;
        }
    const bool ok = submit_once([&](VkCommandBuffer cmd) {
      image_barrier(cmd, lut3d_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
      VkBufferImageCopy reg{};
      reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      reg.imageExtent = {16, 16, 16};
      vkCmdCopyBufferToImage(cmd, lut_staging.buffer, lut3d_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
      image_barrier(cmd, lut3d_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      // Clear lum_adapt_[0..1] to neutral initial luminance and transition to SHADER_READ_ONLY_OPTIMAL.
      const VkClearColorValue init_lum{{0.35f, 0.35f, 0.0f, 1.0f}};
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      for (int i = 0; i < 2; ++i) {
        image_barrier(cmd, lum_adapt_[i].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(cmd, lum_adapt_[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &init_lum, 1, &range);
        image_barrier(cmd, lum_adapt_[i].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      }
    });
    vmaUnmapMemory(allocator_, lut_staging.allocation);
    destroy_buffer(lut_staging);
    if (!ok) {
      error = "3D LUT initial upload";
      return false;
    }
    lum_reset_ = true;
  }

  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_TRY(vkCreateSampler(device_, &sampler, nullptr, &post_sampler_), "post sampler");
  VkSamplerCreateInfo dsampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  dsampler.magFilter = dsampler.minFilter = VK_FILTER_NEAREST;
  dsampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  dsampler.addressModeU = dsampler.addressModeV = dsampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  dsampler.maxLod = VK_LOD_CLAMP_NONE;
  VK_TRY(vkCreateSampler(device_, &dsampler, nullptr, &post_depth_sampler_), "post depth sampler");
  constexpr VkDeviceSize kOpticalUboBytes = 15 * 16;  // 15 vec4s = 240 bytes
  if (!create_buffer(kOpticalUboBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, post_optical_buf_)) {
    error = "post optical UBO";
    return false;
  }
  const VkDescriptorSetLayoutBinding bindings[12] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set.bindingCount = 12;
  set.pBindings = bindings;
  VK_TRY(vkCreateDescriptorSetLayout(device_, &set, nullptr, &post_set_layout_), "post set layout");
  VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &post_set_layout_;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &push;
  VK_TRY(vkCreatePipelineLayout(device_, &layout, nullptr, &post_layout_), "post pipeline layout");
  const VkDescriptorPoolSize pool_sizes[2] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kPostSetCount * 11},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kPostSetCount}};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = kPostSetCount;
  pool.poolSizeCount = 2;
  pool.pPoolSizes = pool_sizes;
  VK_TRY(vkCreateDescriptorPool(device_, &pool, nullptr, &post_pool_), "post descriptor pool");
  VkDescriptorSetLayout layouts[kPostSetCount];
  for (uint32_t i = 0; i < kPostSetCount; ++i) layouts[i] = post_set_layout_;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = post_pool_;
  alloc.descriptorSetCount = kPostSetCount;
  alloc.pSetLayouts = layouts;
  VK_TRY(vkAllocateDescriptorSets(device_, &alloc, post_sets_), "post descriptor sets");

  auto make_module = [&](const uint32_t* code, size_t bytes, VkShaderModule& module) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    return vkCreateShaderModule(device_, &info, nullptr, &module);
  };
  VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE, frag_bdown = VK_NULL_HANDLE, frag_bup = VK_NULL_HANDLE;
  VK_TRY(make_module(kPost_vert_spv, sizeof(kPost_vert_spv), vert), "post vertex shader");
  VK_TRY(make_module(kPost_frag_spv, sizeof(kPost_frag_spv), frag), "post fragment shader");
  VK_TRY(make_module(kBloom_down_frag_spv, sizeof(kBloom_down_frag_spv), frag_bdown), "bloom down shader");
  VK_TRY(make_module(kBloom_up_frag_spv, sizeof(kBloom_up_frag_spv), frag_bup), "bloom up shader");
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState attachment{};
  attachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;
  const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  auto build_post_pipe = [&](VkShaderModule fmod, VkFormat format, VkPipeline& dst) -> VkResult {
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fmod, "main",
         nullptr}};
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = post_layout_;
    return vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &dst);
  };
  VkResult result = build_post_pipe(frag, scene_format(), post_quarter_);
  if (result == VK_SUCCESS) result = build_post_pipe(frag_bdown, scene_format(), post_bloom_down_);
  if (result == VK_SUCCESS) result = build_post_pipe(frag_bup, scene_format(), post_bloom_up_);
  if (result == VK_SUCCESS) result = build_post_pipe(frag, color_format_, post_final_);
  if (result == VK_SUCCESS) result = build_post_pipe(frag, VK_FORMAT_R8G8B8A8_UNORM, post_final_sdr_);
  vkDestroyShaderModule(device_, vert, nullptr);
  vkDestroyShaderModule(device_, frag, nullptr);
  vkDestroyShaderModule(device_, frag_bdown, nullptr);
  vkDestroyShaderModule(device_, frag_bup, nullptr);
  VK_TRY(result, "post pipelines");
  return true;
}


void Renderer::record_post(VkCommandBuffer cmd, VkImageView final_view, VkPipeline pipe, float out_alpha) {
  const PostSettings& s = options_.post;
  // Update optical post-FX UBO (binding 5): in the frame's commands, behind
  // a barrier, so a frame still in flight keeps its own values.
  if (post_optical_buf_.buffer) {
    struct PostOpticalUbo {
      float flags_near[4];
      float mblur[4];
      float dof[4];
      float sun[4];
      float sun_color[4];
      float vig_top_col[4];
      float vig_top_geom[4];
      float vig_top_rot[4];
      float vig_bot_col[4];
      float vig_bot_geom[4];
      float vig_bot_rot[4];
      float texel_size[4];
      float modern_p[4];
      float sota_p[4];
      float phase_b[4];
    };
    static_assert(sizeof(PostOpticalUbo) == 240);
    const uint32_t oflags = (s.motion_blur ? 1u : 0u) | (s.dof ? 2u : 0u) | (s.god_rays ? 4u : 0u) |
                            (s.lens_flare ? 8u : 0u) | (options_.reverse_z ? 16u : 0u) |
                            (s.paired_vignette ? 32u : 0u);
    PostOpticalUbo u{};
    u.flags_near[0] = float(oflags);
    u.flags_near[1] = s.near_plane;
    u.flags_near[2] = s.contrast;
    u.flags_near[3] = s.brightness;
    u.mblur[0] = s.motion_blur_center[0];
    u.mblur[1] = s.motion_blur_center[1];
    u.mblur[2] = s.motion_blur_strength;
    u.mblur[3] = s.motion_blur_car_protect;
    u.dof[0] = s.dof_focal_distance;
    u.dof[1] = s.dof_focal_range;
    u.dof[2] = s.dof_max_blur;
    u.dof[3] = s.sepia;
    u.sun[0] = s.sun_screen_uv[0];
    u.sun[1] = s.sun_screen_uv[1];
    u.sun[2] = s.sun_visibility;
    u.sun[3] = s.god_ray_strength;
    u.sun_color[0] = s.sun_scatter_color[0];
    u.sun_color[1] = s.sun_scatter_color[1];
    u.sun_color[2] = s.sun_scatter_color[2];
    u.sun_color[3] = s.lens_flare_strength;
    u.vig_top_col[0] = s.vig_top_color[0];
    u.vig_top_col[1] = s.vig_top_color[1];
    u.vig_top_col[2] = s.vig_top_color[2];
    u.vig_top_col[3] = s.vig_top_opacity;
    u.vig_top_geom[0] = s.vig_top_center[0];
    u.vig_top_geom[1] = s.vig_top_center[1];
    u.vig_top_geom[2] = s.vig_top_radius[0];
    u.vig_top_geom[3] = s.vig_top_radius[1];
    u.vig_top_rot[0] = std::cos(s.vig_top_rot);
    u.vig_top_rot[1] = std::sin(s.vig_top_rot);
    u.vig_top_rot[2] = s.vig_top_feather;
    u.vig_bot_col[0] = s.vig_bot_color[0];
    u.vig_bot_col[1] = s.vig_bot_color[1];
    u.vig_bot_col[2] = s.vig_bot_color[2];
    u.vig_bot_col[3] = s.vig_bot_opacity;
    u.vig_bot_geom[0] = s.vig_bot_center[0];
    u.vig_bot_geom[1] = s.vig_bot_center[1];
    u.vig_bot_geom[2] = s.vig_bot_radius[0];
    u.vig_bot_geom[3] = s.vig_bot_radius[1];
    u.vig_bot_rot[0] = std::cos(s.vig_bot_rot);
    u.vig_bot_rot[1] = std::sin(s.vig_bot_rot);
    u.vig_bot_rot[2] = s.vig_bot_feather;
    u.texel_size[0] = render_width_ ? 1.0f / float(render_width_) : 1.0f / 1280.0f;
    u.texel_size[1] = render_height_ ? 1.0f / float(render_height_) : 1.0f / 720.0f;
    u.texel_size[2] = s.time_sec;
    u.texel_size[3] = float(frames_submitted_ & 63u) / 64.0f;
    const float ao_strength = graphics_.ssao == modern::SsaoQuality::kLow      ? 0.50f
                              : graphics_.ssao == modern::SsaoQuality::kMedium ? 0.72f
                              : graphics_.ssao == modern::SsaoQuality::kHigh   ? 0.88f
                              : graphics_.ssao == modern::SsaoQuality::kUltra  ? 0.96f
                                                                               : 0.0f;
    u.modern_p[0] = ao_strength;
    u.modern_p[1] = float(lut_image_size_);
    u.modern_p[2] = lut_strength_;
    u.modern_p[3] = out_alpha;
    const float sscs_strength = graphics_.contact_shadows == modern::ContactShadows::kLow     ? 0.70f
                                : graphics_.contact_shadows == modern::ContactShadows::kHigh  ? 0.85f
                                : graphics_.contact_shadows == modern::ContactShadows::kUltra ? 0.95f
                                                                                              : 0.0f;
    const bool vel_mblur_ready =
        (graphics_.velocity_blur != modern::VelocityBlur::kOff) && mblur_neighbor_.view && motion_vec_.view;
    u.sota_p[0] = float(int(graphics_.tonemapper));
    u.sota_p[1] = sscs_.view ? sscs_strength : 0.0f;
    u.sota_p[2] = vel_mblur_ready ? float(modern::velocity_blur_taps(graphics_.velocity_blur)) : 0.0f;
    u.sota_p[3] = (graphics_.ssao == modern::SsaoQuality::kHigh || graphics_.ssao == modern::SsaoQuality::kUltra)
                      ? 1.0f
                      : (graphics_.ssao != modern::SsaoQuality::kOff ? 0.55f : 0.0f);
    const bool ssr_on = (graphics_.ssr != modern::SsrQuality::kOff) && ssr_blur_.view;
    const bool vol_on = (graphics_.volumetric_fog != modern::VolumetricFog::kOff) && vol_blur_.view && vol_state_valid_;
    u.phase_b[0] = ssr_on ? (graphics_.ssr == modern::SsrQuality::kFull ? 0.90f : 0.80f) : 0.0f;
    u.phase_b[1] = vol_on ? 1.0f : 0.0f;
    u.phase_b[2] = vol_width_ ? 1.0f / float(vol_width_) : 0.0f;
    u.phase_b[3] = vol_height_ ? 1.0f / float(vol_height_) : 0.0f;
    VkBufferMemoryBarrier2 ub{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    ub.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    ub.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    ub.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    ub.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    ub.srcQueueFamilyIndex = ub.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ub.buffer = post_optical_buf_.buffer;
    ub.size = VK_WHOLE_SIZE;
    VkDependencyInfo udep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    udep.bufferMemoryBarrierCount = 1;
    udep.pBufferMemoryBarriers = &ub;
    vkCmdPipelineBarrier2(cmd, &udep);
    vkCmdUpdateBuffer(cmd, post_optical_buf_.buffer, 0, sizeof u, &u);
    ub.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    ub.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    ub.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    ub.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &udep);
  }
  // Upload updated N^3 3D LUT when dirty (zero pipeline stall).
  const size_t want_lut_bytes = size_t(lut_image_size_) * lut_image_size_ * lut_image_size_ * 4u;
  if (lut_dirty_ && lut_pending_size_ == lut_image_size_ && lut_pending_.size() == want_lut_bytes) {
    FrameSlot& fs = slot();
    if (fs.lut_staging.size < want_lut_bytes) {
      if (fs.lut_staging_mapped) vmaUnmapMemory(allocator_, fs.lut_staging.allocation);
      fs.lut_staging_mapped = nullptr;
      destroy_buffer(fs.lut_staging);
      if (create_buffer(want_lut_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, fs.lut_staging) &&
          vmaMapMemory(allocator_, fs.lut_staging.allocation, &fs.lut_staging_mapped) != VK_SUCCESS)
        fs.lut_staging_mapped = nullptr;
    }
  }
  if (lut_dirty_ && slot().lut_staging_mapped && lut_pending_size_ == lut_image_size_ &&
      lut_pending_.size() == want_lut_bytes) {
    lut_dirty_ = false;
    std::memcpy(slot().lut_staging_mapped, lut_pending_.data(), lut_pending_.size());
    image_barrier(cmd, lut3d_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy reg{};
    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    reg.imageExtent = {lut_image_size_, lut_image_size_, lut_image_size_};
    vkCmdCopyBufferToImage(cmd, slot().lut_staging.buffer, lut3d_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
    image_barrier(cmd, lut3d_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  }

  auto finite_or = [](float v, float fallback) -> float { return std::isfinite(v) ? v : fallback; };
  const float safe_exposure = std::clamp(finite_or(s.exposure, 0.8f), -6.0f, 6.0f);
  const float safe_white = std::clamp(finite_or(s.white, 4.0f), 0.25f, 64.0f);
  const float safe_bloom_cutoff = std::clamp(finite_or(s.bloom_cutoff, 1.0f), 0.05f, 32.0f);
  const float safe_bloom_scale = std::clamp(finite_or(s.bloom_scale, 0.25f), 0.0f, 4.0f);
  const float safe_adapt_alpha = std::clamp(finite_or(s.adapt_alpha, 0.05f), 0.005f, 1.0f);
  const uint32_t flags = (s.bloom ? 1u : 0u) | (s.lut_enabled ? 2u : 0u) | (s.auto_exposure ? 4u : 0u);
  struct {
    float mode[4], abc[4], def[4], desat[4], bloom[4], balance[4], exp_keys[4], exp_ev[4];
  } push = {{0, 0, 0, 0},
            {finite_or(s.hable[0], 0.22f), finite_or(s.hable[1], 0.30f), finite_or(s.hable[2], 0.10f), safe_white},
            {finite_or(s.hable[3], 0.20f), finite_or(s.hable[4], 0.01f), finite_or(s.hable[5], 0.30f), safe_exposure},
            {finite_or(s.luminance[0], 0.2125f), finite_or(s.luminance[1], 0.7154f), finite_or(s.luminance[2], 0.0721f),
             std::clamp(finite_or(s.desaturate, 0.0f), 0.0f, 1.0f)},
            {safe_bloom_cutoff, safe_bloom_scale, float(flags), std::max(finite_or(s.bloom_lower_grad, 0.0f), 0.0f)},
            {finite_or(s.balance[0], 1.0f), finite_or(s.balance[1], 1.0f), finite_or(s.balance[2], 1.0f),
             std::max(finite_or(s.bloom_upper_grad, 1.0f), 0.01f)},
            {finite_or(s.exp_keys[0], 0.5f), finite_or(s.exp_keys[1], 4.0f), finite_or(s.exp_keys[2], 16.0f),
             finite_or(s.exp_keys[3], 64.0f)},
            {finite_or(s.exp_ev[0], 1.0f), finite_or(s.exp_ev[1], 0.0f), finite_or(s.exp_ev[2], -0.8f),
             finite_or(s.exp_ev[3], -1.6f)}};
  static_assert(sizeof(push) == 128);
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  const auto read = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  auto to_read = [&](const Image& i) {
    image_barrier(cmd, i.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read);
  };
  auto to_write = [&](const Image& i) {
    image_barrier(cmd, i.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
  };
  auto pass = [&](VkImageView target, uint32_t w, uint32_t h, VkPipeline pipeline, VkDescriptorSet set, float mode,
                  float dx, float dy, float mw = 0.0f) {
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {w, h}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    VkViewport viewport{0, 0, float(w), float(h), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout_, 0, 1, &set, 0, nullptr);
    push.mode[0] = mode, push.mode[1] = dx, push.mode[2] = dy, push.mode[3] = mw;
    vkCmdPushConstants(cmd, post_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
  };

  // hdr_ was transitioned to SHADER_READ_ONLY_OPTIMAL in record_scene before SSR/Volumetrics.

  // 1. Adaptive auto-exposure: 16x16 centre-weighted reduction -> 1x1 temporal adaptation.
  if (s.auto_exposure) {
    to_write(lum_16_);
    pass(lum_16_.view, 16, 16, post_quarter_, post_sets_[0], 3, 0.25f / 16.0f, 0.25f / 16.0f);
    to_read(lum_16_);
    to_write(lum_adapt_[0]);
    const float alpha = lum_reset_ ? 1.0f : safe_adapt_alpha;
    lum_reset_ = false;
    pass(lum_adapt_[0].view, 1, 1, post_quarter_, post_sets_[1], 4, 0, 0, alpha);
    // Copy adapted 1x1 luminance into lum_adapt_[1] for next frame, leave lum_adapt_[0] readable.
    image_barrier(cmd, lum_adapt_[0].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color_stage, color_write, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT);
    image_barrier(cmd, lum_adapt_[1].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, read_stage, read, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy cp{};
    cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {1, 1, 1};
    vkCmdCopyImage(cmd, lum_adapt_[0].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, lum_adapt_[1].image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    image_barrier(cmd, lum_adapt_[0].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT, read_stage, read);
    image_barrier(cmd, lum_adapt_[1].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, read_stage, read);
  }

  // 2. Physically-Based 6-level Jimenez / Dual-Kawase bloom pyramid:
  //    Mip 0: 13-tap Karis-averaged anti-firefly prefilter -> Mips 1..5: 13-tap progressive downsample
  //    Mips 5->0: 9-tap 3x3 tent-filter progressive upsample combine (11 passes vs 19 Gaussian passes).
  if (s.bloom && bloom_a_[0].image && bloom_b_[0].image && post_bloom_down_ && post_bloom_up_) {
    // Downsample mip 0 (Karis average + soft-knee prefilter from full HDR scene).
    to_write(bloom_a_[0]);
    pass(bloom_a_[0].view, bloom_w_[0], bloom_h_[0], post_bloom_down_, post_sets_[2], 0.0f,
         1.0f / float(std::max(1u, render_width_)), 1.0f / float(std::max(1u, render_height_)), 0.45f);
    to_read(bloom_a_[0]);

    // Progressive 13-tap downsamples mips 1..5.
    for (uint32_t k = 1; k < kBloomLevels; ++k) {
      to_write(bloom_a_[k]);
      pass(bloom_a_[k].view, bloom_w_[k], bloom_h_[k], post_bloom_down_, post_sets_[2 + k], float(k),
           1.0f / float(std::max(1u, bloom_w_[k - 1])), 1.0f / float(std::max(1u, bloom_h_[k - 1])), 0.0f);
      to_read(bloom_a_[k]);
    }

    // Progressive 3x3 tent-filter upsample mips 5->4->3->2->1->0 into bloom_b_[4..0].
    // Map FH1's 5 bloom_weights onto our 6 mips cleanly (mip 5 extends the widest halation tail).
    float mip_w[kBloomLevels];
    float w_sum = 0.0f;
    for (uint32_t k = 0; k < kBloomLevels; ++k) {
      mip_w[k] = std::max(k < 5 ? s.bloom_weights[k] : s.bloom_weights[4] * 0.85f, 0.0f);
      w_sum += mip_w[k];
    }
    const float w_norm = w_sum > 1e-4f ? 1.0f / w_sum : (1.0f / float(kBloomLevels));
    constexpr float kTentRadius = 1.15f;
    for (int k = int(kBloomLevels) - 2; k >= 0; --k) {
      const uint32_t set_idx = 8u + uint32_t((int(kBloomLevels) - 2) - k);
      const float w_cur = mip_w[k] * w_norm;
      const float w_prev = (k == int(kBloomLevels) - 2) ? (mip_w[k + 1] * w_norm) : 1.0f;
      to_write(bloom_b_[k]);
      pass(bloom_b_[k].view, bloom_w_[k], bloom_h_[k], post_bloom_up_, post_sets_[set_idx], w_cur,
           kTentRadius / float(std::max(1u, bloom_w_[k + 1])), kTentRadius / float(std::max(1u, bloom_h_[k + 1])),
           w_prev);
      to_read(bloom_b_[k]);
    }
    mark(cmd, "bloom");
  } else if (bloom_b_[0].image) {
    // Explicitly clear bloom_b_[0] to black when bloom is skipped so uninitialized VRAM
    // can never leak into the composite pass as a white flash.
    const VkClearColorValue zero_col{{0.0f, 0.0f, 0.0f, 0.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    image_barrier(cmd, bloom_b_[0].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, read_stage, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(cmd, bloom_b_[0].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero_col, 1, &range);
    image_barrier(cmd, bloom_b_[0].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, read_stage, read);
  }

  // 3. Final composite (scene + McGuire MBlur + XeGTAO + SSCS + bloom -> exposure -> selected tonemapper -> LUT -> display).
  pass(final_view, render_width_, render_height_, pipe, post_sets_[13], 2, 0, 0, s.vignette_amount);
  mark(cmd, "post");
}

}  // namespace carview
