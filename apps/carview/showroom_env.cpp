#include "showroom_internal.h"

#include "env_prefilter_frag_spv.h"
#include "post_vert_spv.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace carview {

void Showroom::bind_dynamic_env(VkImageView cube_view, VkSampler sampler) {
  if (!r_.device_ || !frame_set_ || !cube_view || !sampler || bound_dyn_env_view_ == cube_view) return;
  // Allocate a fresh frame descriptor set from pool_ so we NEVER mutate an in-flight set
  // or call vkDeviceWaitIdle() on traffic/race car spawn!
  VkDescriptorSet fresh = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &frame_layout_;
  if (vkAllocateDescriptorSets(r_.device_, &ai, &fresh) == VK_SUCCESS && fresh) {
    VkDescriptorBufferInfo buffer{frame_buffer_.buffer, 0, sizeof(ShowroomFrame)};
    VkDescriptorImageInfo cube{sampler, cube_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo static_cube{sampler_, environment_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo map{r_.shadow_sampler_, r_.shadow_map_.view,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo map_raw{r_.shadow_raw_sampler_, r_.shadow_map_.view,
                                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cookie{sampler_, headlight_cookie_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo hl_buf{headlights_ubo_.buffer, 0, sizeof(HeadlightUbo)};
    const VkImageView mirror_view = (dyn_env_ready_ && mirror_tex_.view) ? mirror_tex_.view : gpu_textures_[1].view;
    VkDescriptorImageInfo mir{sampler_, mirror_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo inst_buf{guest_ ? empty_inst_ssbo_.buffer : car_inst_buf_.buf.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo dirt_p{sampler_, shared_dirt_pattern_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo dirt_m{sampler_, shared_dirt_mask_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo gcrack{sampler_, shared_glass_crack_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[12]{};
    for (int i = 0; i < 12; ++i) {
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet = fresh;
      writes[i].dstBinding = uint32_t(i);
      writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, writes[0].pBufferInfo = &buffer;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[1].pImageInfo = &cube;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[2].pImageInfo = &map;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[3].pImageInfo = &cookie;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, writes[4].pBufferInfo = &hl_buf;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[5].pImageInfo = &mir;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, writes[6].pBufferInfo = &inst_buf;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[7].pImageInfo = &static_cube;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[8].pImageInfo = &dirt_p;
    writes[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[9].pImageInfo = &dirt_m;
    writes[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[10].pImageInfo = &gcrack;
    writes[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[11].pImageInfo = &map_raw;
    vkUpdateDescriptorSets(r_.device_, 12, writes, 0, nullptr);
    frame_set_ = fresh;
    bound_dyn_env_view_ = cube_view;
    return;
  }
  bound_dyn_env_view_ = cube_view;
}


bool Showroom::init_dynamic_reflections(std::string& error) {
  if (guest_ || !r_.device_) return true;
  if (dyn_env_ready_) return true;

  // RGBA16F cubemap (256^2x9 high, 128^2x8 low-power) for live drive reflections (car + LAKE water).
  const VkFormat hdr_fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
  const uint32_t env_sz = dyn_env_size_;
  const uint32_t env_mips = std::min(dyn_env_mips_, kDynEnvMips);
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = hdr_fmt;
  ci.extent = {env_sz, env_sz, 1};
  ci.mipLevels = env_mips;
  ci.arrayLayers = 6;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  VmaAllocationInfo ainfo{};
  VK_TRY(vmaCreateImage(r_.allocator_, &ci, &alloc, &dyn_env_.image, &dyn_env_.allocation, &ainfo),
         "dynamic env cubemap");
  dyn_env_.bytes = ainfo.size;

  VkImageViewCreateInfo cv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  cv.image = dyn_env_.image;
  cv.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  cv.format = hdr_fmt;
  cv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, env_mips, 0, 6};
  VK_TRY(vkCreateImageView(r_.device_, &cv, nullptr, &dyn_env_.view), "dynamic env cube view");

  VkImageViewCreateInfo cv0{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  cv0.image = dyn_env_.image;
  cv0.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  cv0.format = hdr_fmt;
  cv0.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
  VK_TRY(vkCreateImageView(r_.device_, &cv0, nullptr, &dyn_env_mip0_cube_), "dynamic env mip0 cube view");

  for (uint32_t f = 0; f < 6; ++f) {
    VkImageViewCreateInfo fv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    fv.image = dyn_env_.image;
    fv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    fv.format = hdr_fmt;
    fv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f, 1};
    VK_TRY(vkCreateImageView(r_.device_, &fv, nullptr, &dyn_env_faces_[f]), "dynamic env face view");
    for (uint32_t m = 1; m < env_mips; ++m) {
      fv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, f, 1};
      VK_TRY(vkCreateImageView(r_.device_, &fv, nullptr, &dyn_env_mip_faces_[m][f]), "dynamic env mip face view");
    }
  }

  // Cross-face GGX cubemap prefilter pipeline (post.vert + env_prefilter.frag).
  {
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   nullptr};
    VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount = 1;
    dsl.pBindings = &b;
    VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &dsl, nullptr, &env_prefilter_set_layout_), "prefilter dsl");
    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &env_prefilter_set_layout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    VK_TRY(vkCreatePipelineLayout(r_.device_, &pl, nullptr, &env_prefilter_pipe_layout_), "prefilter pl");
    VkShaderModule pv = VK_NULL_HANDLE, pf = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(kPost_vert_spv);
    smi.pCode = kPost_vert_spv;
    VK_TRY(vkCreateShaderModule(r_.device_, &smi, nullptr, &pv), "prefilter vert");
    smi.codeSize = sizeof(kEnv_prefilter_frag_spv);
    smi.pCode = kEnv_prefilter_frag_spv;
    if (vkCreateShaderModule(r_.device_, &smi, nullptr, &pf) != VK_SUCCESS) {
      vkDestroyShaderModule(r_.device_, pv, nullptr);
      error = "prefilter frag";
      return false;
    }
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, pv, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, pf, "main",
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
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo bl{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bl.attachmentCount = 1;
    bl.pAttachments = &att;
    const VkDynamicState dyn_s[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_s;
    VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rend.colorAttachmentCount = 1;
    rend.pColorAttachmentFormats = &hdr_fmt;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &rend;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &bl;
    pci.pDynamicState = &dyn;
    pci.layout = env_prefilter_pipe_layout_;
    VkResult pr_res =
        vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &pci, nullptr, &env_prefilter_pipe_);
    vkDestroyShaderModule(r_.device_, pv, nullptr);
    vkDestroyShaderModule(r_.device_, pf, nullptr);
    VK_TRY(pr_res, "prefilter pipeline");
  }

  if (!r_.create_image(env_sz, env_sz, 1, r_.depth_format_, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT, dyn_env_depth_)) {
    error = "dynamic env depth";
    return false;
  }

  if (!r_.create_buffer(sizeof(ShowroomFrame),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, env_frame_buffer_) ||
      !r_.create_buffer(sizeof(SkyUniforms),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, env_sky_ubo_[0]) ||
      !r_.create_buffer(sizeof(SkyUniforms),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, env_sky_ubo_[1])) {
    error = "dynamic env buffers";
    return false;
  }

  // Clear cubemap to SHADER_READ_ONLY_OPTIMAL so descriptors are valid before first draw.
  // Cockpit mirror targets are allocated lazily on first cockpit camera activation.
  const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = dyn_env_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, env_mips, 0, 6};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    const VkClearColorValue clr{{0.08f, 0.10f, 0.14f, 1.0f}};
    vkCmdClearColorImage(cmd, dyn_env_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &b.subresourceRange);
    b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    VkImageMemoryBarrier2 db{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    db.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    db.dstAccessMask =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    db.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    db.image = dyn_env_depth_.image;
    db.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo ddep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    ddep.imageMemoryBarrierCount = 1;
    ddep.pImageMemoryBarriers = &db;
    vkCmdPipelineBarrier2(cmd, &ddep);
  });
  if (!ok) {
    error = "dynamic env clear";
    return false;
  }
  dyn_env_ready_ = true;
  if (mirror_enabled_) ensure_mirror_targets();
  return true;
}

bool Showroom::ensure_mirror_targets() {
  if (guest_ || !r_.device_) return false;
  if (mirror_tex_.view && mirror_depth_.view) return true;
  const VkFormat hdr_fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
  if (!r_.create_image(kMirrorWidth, kMirrorHeight, 1, hdr_fmt, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, mirror_tex_) ||
      !r_.create_image(kMirrorWidth, kMirrorHeight, 1, r_.depth_format_, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT, mirror_depth_)) {
    r_.destroy_image(mirror_tex_);
    r_.destroy_image(mirror_depth_);
    return false;
  }
  const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = mirror_tex_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    const VkClearColorValue clr{{0.08f, 0.10f, 0.14f, 1.0f}};
    vkCmdClearColorImage(cmd, mirror_tex_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &b.subresourceRange);
    b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    VkImageMemoryBarrier2 db{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    db.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    db.dstAccessMask =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    db.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    db.image = mirror_depth_.image;
    db.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo ddep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    ddep.imageMemoryBarrierCount = 1;
    ddep.pImageMemoryBarriers = &db;
    vkCmdPipelineBarrier2(cmd, &ddep);
  });
  if (!ok) {
    r_.destroy_image(mirror_tex_);
    r_.destroy_image(mirror_depth_);
    return false;
  }
  if (frame_set_ && sampler_) {
    r_.wait_idle();
    VkDescriptorImageInfo info{sampler_, mirror_tex_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = frame_set_;
    w.dstBinding = 5;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(r_.device_, 1, &w, 0, nullptr);
  }
  mirror_valid_ = false;
  return true;
}

void Showroom::record_offscreen_view(VkCommandBuffer cmd, VkImageView color_view, VkImageView depth_view,
                                     uint32_t w, uint32_t h, const ShowroomFrame& view_frame, bool include_cars,
                                     uint32_t vx, uint32_t vy, uint32_t vw, uint32_t vh, bool clear_target,
                                     uint32_t* draw_counter) {
  if (vw == 0) vw = w;
  if (vh == 0) vh = h;
  // Upload view UBO and sky UBOs outside render pass.
  update_buffer(cmd, env_frame_buffer_.buffer, view_frame);
  if (sky_ready_ && env_sky_pipe_base_ && env_sky_sets_[0]) {
    SkyUniforms u{};
    fill_sky_uniforms(view_frame, u);
    for (int m = 0; m < 2; ++m) {
      u.cam_fwd[3] = float(m);
      vkCmdUpdateBuffer(cmd, env_sky_ubo_[m].buffer, 0, sizeof(SkyUniforms), &u);
      VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
      b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.buffer = env_sky_ubo_[m].buffer;
      b.size = sizeof(SkyUniforms);
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.bufferMemoryBarrierCount = 1;
      dep.pBufferMemoryBarriers = &b;
      vkCmdPipelineBarrier2(cmd, &dep);
    }
  }

  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = color_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = clear_target ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color = {{0.05f, 0.08f, 0.12f, 1.0f}};
  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = depth_view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.clearValue.depthStencil = {r_.reverse_z() ? 0.0f : 1.0f, 0};
  VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rend.renderArea = {{int32_t(vx), int32_t(vy)}, {vw, vh}};
  rend.layerCount = 1;
  rend.colorAttachmentCount = 1;
  rend.pColorAttachments = &color;
  rend.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &rend);
  const VkViewport vp{float(vx), float(vy), float(vw), float(vh), 0.0f, 1.0f};
  const VkRect2D sc{{int32_t(vx), int32_t(vy)}, {vw, vh}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);

  // 1. Opaque AND alpha-tested (blend 0 + blend 1) world terrain, roads, mountains, trees,
  //    buildings, signs, banners, and barriers (drawn BEFORE sky for Early-Z rejection).
  if (world_textured_ && !world_draws_.empty() && env_world_opaque_ && env_frame_set_) {
    float clip[16];
    multiply(view_frame.view_projection, world_transform_, clip);
    float planes[4][4];
    for (int c = 0; c < 4; ++c) {
      const float x = clip[c * 4 + 0], y = clip[c * 4 + 1], wv = clip[c * 4 + 3];
      planes[0][c] = wv + x, planes[1][c] = wv - x, planes[2][c] = wv + y, planes[3][c] = wv - y;
    }
    auto outside = [&](const WorldGeometry::Draw& d) {
      for (const auto& p : planes) {
        const float x = p[0] >= 0 ? d.box_max[0] : d.box_min[0];
        const float y = p[1] >= 0 ? d.box_max[1] : d.box_min[1];
        const float z = p[2] >= 0 ? d.box_max[2] : d.box_min[2];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0) return true;
      }
      return false;
    };
    const auto eye_sc = [&]() -> std::array<float, 3> {
      const float e[3] = {view_frame.eye[0] - world_transform_[12], view_frame.eye[1] - world_transform_[13],
                          view_frame.eye[2] - world_transform_[14]};
      std::array<float, 3> out{};
      for (int c = 0; c < 3; ++c)
        out[size_t(c)] = world_transform_[c * 4 + 0] * e[0] + world_transform_[c * 4 + 1] * e[1] +
                         world_transform_[c * 4 + 2] * e[2];
      return out;
    }();
    const float max_dist = include_cars ? (low_memory_mode_ ? 200.0f : 320.0f) : 160.0f;
    const float max_dist2 = max_dist * max_dist;
    const float cutout_dist2 = include_cars ? (low_memory_mode_ ? 110.0f * 110.0f : 165.0f * 165.0f) : (75.0f * 75.0f);
    const float layer_dist2 = include_cars ? 120.0f * 120.0f : 80.0f * 80.0f;
    world_bind_buffers(cmd, false);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 1, &env_frame_set_, 0,
                            nullptr);
    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
    VkPipeline bound_pipe = VK_NULL_HANDLE;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    for (const auto& d : world_draws_) {
      // Include opaque (blend 0) AND H_DIFF*/TREE_* alpha-tested props/trees/banners (blend 1).
      if (!d.cull || d.blend > 1) continue;
      // Fast squared AABB distance cull before frustum planes (cuts ~3,400 draws down to ~150-250).
      float d2 = 0.0f;
      for (int k = 0; k < 3; ++k) {
        const float q = std::max({d.box_min[k] - eye_sc[size_t(k)], 0.0f, eye_sc[size_t(k)] - d.box_max[k]});
        d2 += q * q;
      }
      if (d2 > max_dist2) continue;
      if (d.blend == 1 && d2 > cutout_dist2) continue;
      if (outside(d)) continue;
      const uint32_t fl = uint32_t(d.push.params[1] + 0.5f);
      if ((fl & (2u | 32768u)) != 0u) continue;
      VkPipeline want_pipe = (d.blend == 1 && env_world_coverage_) ? env_world_coverage_ : env_world_opaque_;
      if (want_pipe != bound_pipe)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_pipe = want_pipe);

      VkDescriptorSet set = d.texture != kNoWorldTexture ? world_set(d.texture) : VK_NULL_HANDLE;
      uint32_t out_fl = fl;
      const bool has_extra = (d2 <= layer_dist2) &&
                             (d.texture_b != kNoWorldTexture || d.splat != kNoWorldTexture ||
                              d.ao_map != kNoWorldTexture || d.layer_c != kNoWorldTexture ||
                              d.light_map != kNoWorldTexture || d.spec_map != kNoWorldTexture);
      if ((set || d.texture == kNoWorldTexture) && has_extra) {
        VkDescriptorSet layered = world_layer_set(
            {d.texture, d.texture_b, d.splat, d.light_map, d.normal_map, d.ao_map, d.layer_c, d.spec_map});
        if (layered) {
          set = layered;
        } else {
          out_fl &= ~(4u | 8u | 4096u | 8192u | 16384u | 524288u);
        }
      } else if (!has_extra) {
        out_fl &= ~(4u | 8u | 2048u | 4096u | 8192u | 16384u | 524288u);
      }
      if (!set) {
        set = world_white_set_;
        out_fl &= ~1u;
      }
      if (set != bound_set)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 1, 1, &(bound_set = set),
                                0, nullptr);
      ShowroomPush push = d.push;
      // 1x offscreen target has no MSAA alpha-to-coverage: enable alpha test (flag 64) for blend==1.
      if (d.blend == 1) out_fl |= 64u;
      push.params[1] = float(out_fl);
      world_draw_model(d, world_transform_, push.model);
      std::memcpy(push.uv0, world_sun_, sizeof push.uv0);
      push.uv0[3] = 2.0f;
      std::memcpy(push.uv1, world_up_, sizeof push.uv1);
      vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
      if (draw_counter) ++(*draw_counter);
    }
  }
  // 2. Trailing AI / traffic cars in cockpit mirrors (when include_cars is set).
  if (include_cars && env_car_opaque_ && env_frame_set_) {
    for (const auto& in : instances_) {
      if (!in.car) continue;
      const float dx = in.model[12] - view_frame.eye[0];
      const float dy = in.model[13] - view_frame.eye[1];
      const float dz = in.model[14] - view_frame.eye[2];
      if (dx * dx + dy * dy + dz * dz > 220.0f * 220.0f) continue;
      in.car->record_instance_offscreen(cmd, env_car_opaque_, env_frame_set_, in.model,
                                        in.has_lamps ? &in.lamps : nullptr);
      if (draw_counter) *draw_counter += uint32_t(in.car->draw_count());
    }
  }
  // 3. Real-time sky AFTER opaque world & cars (Early-Z rejects occluded pixels).
  if (sky_ready_ && env_sky_pipe_base_ && env_sky_pipe_layers_ && env_sky_sets_[0]) {
    const int disc = tod_state_.is_moon ? (1 + tod_moon_phase_) : 0;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, env_sky_pipe_base_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipe_layout_, 0, 1, &env_sky_sets_[disc], 0,
                            nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, env_sky_pipe_layers_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipe_layout_, 0, 1, &env_sky_sets_[6 + disc], 0,
                            nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
  }
  vkCmdEndRendering(cmd);
}

void Showroom::record_env_and_mirror(VkCommandBuffer cmd) {
  env_draws_ = 0;
  mirror_draws_ = 0;
  if (!dyn_env_ready_ || !env_frame_set_) return;
  auto make_view_proj = [&](const std::array<float, 3>& eye, const std::array<float, 3>& right,
                            const std::array<float, 3>& up, const std::array<float, 3>& fwd,
                            float tan_x, float tan_y, float out[16]) {
    const float dx = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
    const float dy = -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]);
    const float dz = -(fwd[0] * eye[0] + fwd[1] * eye[1] + fwd[2] * eye[2]);
    const float near_p = 0.5f, far_p = 8000.0f;
    out[0] = right[0] / tan_x; out[4] = right[1] / tan_x; out[8]  = right[2] / tan_x; out[12] = dx / tan_x;
    out[1] = -up[0] / tan_y;   out[5] = -up[1] / tan_y;   out[9]  = -up[2] / tan_y;   out[13] = -dy / tan_y;
    if (r_.reverse_z()) {
      out[2] = 0.0f; out[6] = 0.0f; out[10] = 0.0f; out[14] = near_p;
    } else {
      const float k = far_p / (far_p - near_p);
      out[2] = fwd[0] * k; out[6] = fwd[1] * k; out[10] = fwd[2] * k; out[14] = dz * k - near_p * k;
    }
    out[3] = fwd[0]; out[7] = fwd[1]; out[11] = fwd[2]; out[15] = dz;
  };
  auto game_to_render = [&](const std::array<float, 3>& g) -> std::array<float, 3> {
    return {world_transform_[0] * g[0] + world_transform_[4] * g[1] + world_transform_[8] * g[2],
            world_transform_[1] * g[0] + world_transform_[5] * g[1] + world_transform_[9] * g[2],
            world_transform_[2] * g[0] + world_transform_[6] * g[1] + world_transform_[10] * g[2]};
  };

  // Vulkan cubemap face basis in game space (+X, -X, +Y, -Y, +Z, -Z).
  static constexpr std::array<float, 3> kFwd[6] = {
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  static constexpr std::array<float, 3> kRight[6] = {
      {0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
  static constexpr std::array<float, 3> kUp[6] = {
      {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};

  // Schedule dynamic cubemap face updates per preset.reflection_rate and stagger 3-band cockpit mirrors.
  const modern::ReflectionStep rstep = modern::schedule_reflections(
      world_frame_, reflection_rate_, dyn_env_initialized_, mirror_enabled_, mirror_valid_);
  const uint32_t env_sz = dyn_env_size_;
  const uint32_t env_mips = std::min(dyn_env_mips_, kDynEnvMips);
  // Probe center slightly above car roof in render space.
  const std::array<float, 3> cube_eye = {0.0f, 1.35f, 0.0f};
  // Pass A: Render updated face(s) into mip 0 and transition mip 0 to SHADER_READ_ONLY_OPTIMAL.
  if (rstep.render_cubemap) {
    for (uint32_t i = 0; i < rstep.face_count; ++i) {
      const uint32_t f = (rstep.first_face + i) % 6;
      VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = dyn_env_.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f, 1};
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &b;
      vkCmdPipelineBarrier2(cmd, &dep);

      ShowroomFrame vf = frame_;
      vf.shadow[0] = 0.0f;
      vf.shadow_far_bias[1] = vf.shadow_far_bias[2] = vf.shadow_far_bias[3] = 0.0f;
      vf.eye[0] = cube_eye[0];
      vf.eye[1] = cube_eye[1];
      vf.eye[2] = cube_eye[2];
      vf.eye[3] = 1.0f;
      make_view_proj(cube_eye, game_to_render(kRight[f]), game_to_render(kUp[f]), game_to_render(kFwd[f]),
                     1.0f, 1.0f, vf.view_projection);
      std::memcpy(vf.transparent_view_projection, vf.view_projection, sizeof vf.view_projection);

      record_offscreen_view(cmd, dyn_env_faces_[f], dyn_env_depth_.view, env_sz, env_sz, vf, false,
                            0u, 0u, 0u, 0u, true, &env_draws_);

      b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier2(cmd, &dep);
    }
    r_.mark(cmd, "env_cubemap");

    // Pass B: Cross-face GGX prefilter mips 1..env_mips-1 sampling mip 0 across all 6 faces
    // seamlessly (eliminating cube seams on rough/metallic car paint).
    if (env_prefilter_pipe_ && env_prefilter_set_) {
      for (uint32_t i = 0; i < rstep.face_count; ++i) {
        const uint32_t f = (rstep.first_face + i) % 6;
        uint32_t mip_w = env_sz;
        for (uint32_t mip = 1; mip < env_mips; ++mip) {
          mip_w = std::max(1u, mip_w / 2u);
          VkImageMemoryBarrier2 mb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
          mb.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
          mb.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
          mb.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
          mb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
          mb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          mb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          mb.image = dyn_env_.image;
          mb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, f, 1};
          VkDependencyInfo mdep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
          mdep.imageMemoryBarrierCount = 1;
          mdep.pImageMemoryBarriers = &mb;
          vkCmdPipelineBarrier2(cmd, &mdep);

          VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
          ca.imageView = dyn_env_mip_faces_[mip][f];
          ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
          ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
          VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
          ri.renderArea = {{0, 0}, {mip_w, mip_w}};
          ri.layerCount = 1;
          ri.colorAttachmentCount = 1;
          ri.pColorAttachments = &ca;
          vkCmdBeginRendering(cmd, &ri);
          const VkViewport vp{0, 0, float(mip_w), float(mip_w), 0.0f, 1.0f};
          const VkRect2D sc{{0, 0}, {mip_w, mip_w}};
          vkCmdSetViewport(cmd, 0, 1, &vp);
          vkCmdSetScissor(cmd, 0, 1, &sc);
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, env_prefilter_pipe_);
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, env_prefilter_pipe_layout_, 0, 1,
                                  &env_prefilter_set_, 0, nullptr);
          const float rough = float(mip) / float(env_mips - 1u);
          const float pp[4] = {float(f), rough, float(mip), 1.0f / float(mip_w)};
          vkCmdPushConstants(cmd, env_prefilter_pipe_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pp), pp);
          vkCmdDraw(cmd, 3, 1, 0, 0);
          vkCmdEndRendering(cmd);

          mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
          mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
          mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
          mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
          mb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          mb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          vkCmdPipelineBarrier2(cmd, &mdep);
        }
      }
    }
    r_.mark(cmd, "env_prefilter");
    dyn_env_initialized_ = true;
  }

  // Dedicated 3-band cockpit mirror pass (1024x256) ONLY when cockpit camera is active:
  // Band 0 [0..341): mirrorLeft, Band 1 [341..683): mirrorMiddle, Band 2 [683..1024): mirrorRight.
  // Staggers 1 band/frame after initial full 3-band populate (preserving untouched bands).
  if (rstep.render_mirror && mirror_tex_.view && mirror_depth_.view) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = mirror_valid_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = mirror_tex_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);

    struct MirrorBand {
      uint32_t x, w;
      std::array<float, 3> eye, fwd;
      float tx, ty;
    };
    const float sl = 1.0f / std::sqrt(0.20f * 0.20f + 0.04f * 0.04f + 0.98f * 0.98f);
    const MirrorBand bands[3] = {
        {0u, 341u, {-0.95f, 0.95f, 0.25f}, {-0.20f * sl, -0.04f * sl, 0.98f * sl}, 0.50f, 0.28f},
        {341u, 342u, {0.0f, 1.18f, 0.25f}, {0.0f, -0.02f, 0.9998f}, 0.58f, 0.22f},
        {683u, 341u, {0.95f, 0.95f, 0.25f}, {0.20f * sl, -0.04f * sl, 0.98f * sl}, 0.50f, 0.28f},
    };
    for (uint32_t bi = 0; bi < rstep.mirror_band_count; ++bi) {
      const auto& mb = bands[(rstep.mirror_band_begin + bi) % 3u];
      // right = cross(fwd, up=(0,1,0)) = (-fwd.z, 0, fwd.x)
      const std::array<float, 3> mright = {-mb.fwd[2], 0.0f, mb.fwd[0]};
      ShowroomFrame mf = frame_;
      mf.shadow[0] = 0.0f;
      mf.shadow_far_bias[1] = mf.shadow_far_bias[2] = mf.shadow_far_bias[3] = 0.0f;
      mf.eye[0] = mb.eye[0];
      mf.eye[1] = mb.eye[1];
      mf.eye[2] = mb.eye[2];
      mf.eye[3] = 1.0f;
      make_view_proj(mb.eye, mright, {0.0f, 1.0f, 0.0f}, mb.fwd, mb.tx, mb.ty, mf.view_projection);
      std::memcpy(mf.transparent_view_projection, mf.view_projection, sizeof mf.view_projection);
      record_offscreen_view(cmd, mirror_tex_.view, mirror_depth_.view, kMirrorWidth, kMirrorHeight, mf, true,
                            mb.x, 0u, mb.w, kMirrorHeight, true, &mirror_draws_);
    }

    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    mirror_valid_ = true;
    r_.mark(cmd, "cockpit_mirrors");
  } else if (!mirror_enabled_) {
    mirror_valid_ = false;
  }
}

}  // namespace carview
