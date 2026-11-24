#include "renderer_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ui_frag_spv.h"
#include "ui_vert_spv.h"

namespace carview {

void Renderer::set_ui(std::span<const ui::Instance> items) { ui_items_.assign(items.begin(), items.end()); }

void Renderer::ui_atlas_put(uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::span<const uint8_t> rgba) {
  const uint32_t atlas_sz = options_.ui_atlas_size ? options_.ui_atlas_size : kUiAtlas;
  if (!w || !h || x + w > atlas_sz || y + h > atlas_sz || rgba.size() < size_t(w) * h * 4) return;
  ui_puts_.push_back({x, y, w, h, std::vector<uint8_t>(rgba.begin(), rgba.begin() + ptrdiff_t(size_t(w) * h * 4))});
}

bool Renderer::create_ui(std::string& error) {
  if (!ui_font_.buffer) {
    const auto font = ui::font_segments();
    if (!upload_buffer(font.data(), font.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ui_font_)) {
      error = "UI font buffer";
      return false;
    }
    const uint32_t atlas_sz = options_.ui_atlas_size ? options_.ui_atlas_size : kUiAtlas;
    if (!create_image(atlas_sz, atlas_sz, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, ui_atlas_)) {
      error = "UI atlas";
      return false;
    }
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_TRY(vkCreateSampler(device_, &sampler, nullptr, &ui_sampler_), "UI sampler");
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount = 2;
    set.pBindings = bindings;
    VK_TRY(vkCreateDescriptorSetLayout(device_, &set, nullptr, &ui_set_layout_), "UI set layout");
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &ui_set_layout_;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    VK_TRY(vkCreatePipelineLayout(device_, &layout, nullptr, &ui_layout_), "UI pipeline layout");
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                           {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    VK_TRY(vkCreateDescriptorPool(device_, &pool, nullptr, &ui_pool_), "UI descriptor pool");
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = ui_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &ui_set_layout_;
    VK_TRY(vkAllocateDescriptorSets(device_, &alloc, &ui_set_), "UI descriptor set");
    VkDescriptorBufferInfo font_info{ui_font_.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo atlas_info{ui_sampler_, ui_atlas_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    writes[0].dstSet = ui_set_, writes[0].dstBinding = 0, writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, writes[0].pBufferInfo = &font_info;
    writes[1].dstSet = ui_set_, writes[1].dstBinding = 1, writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, writes[1].pImageInfo = &atlas_info;
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    // The atlas starts transparent, in its sampled layout.
    if (!submit_once([&](VkCommandBuffer cmd) {
          image_barrier(cmd, ui_atlas_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
          VkClearColorValue clear{};
          VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          vkCmdClearColorImage(cmd, ui_atlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
          image_barrier(cmd, ui_atlas_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        })) {
      error = "UI atlas clear";
      return false;
    }
  }
  if (ui_pipeline_sdr_ && ui_pipeline_sdr_ != ui_pipeline_) vkDestroyPipeline(device_, ui_pipeline_sdr_, nullptr);
  ui_pipeline_sdr_ = VK_NULL_HANDLE;
  if (ui_pipeline_) vkDestroyPipeline(device_, ui_pipeline_, nullptr), ui_pipeline_ = VK_NULL_HANDLE;

  auto make_module = [&](const uint32_t* code, size_t bytes, VkShaderModule& module) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    return vkCreateShaderModule(device_, &info, nullptr, &module);
  };
  VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
  VK_TRY(make_module(kUi_vert_spv, sizeof(kUi_vert_spv), vert), "UI vertex shader");
  VK_TRY(make_module(kUi_frag_spv, sizeof(kUi_frag_spv), frag), "UI fragment shader");
  const VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main",
       nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main",
       nullptr}};
  const VkVertexInputBindingDescription binding{0, sizeof(ui::Instance), VK_VERTEX_INPUT_RATE_INSTANCE};
  const VkVertexInputAttributeDescription attributes[6] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
                                                           {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
                                                           {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
                                                           {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 48},
                                                           {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 64},
                                                           {5, 0, VK_FORMAT_R32G32B32A32_UINT, 80}};
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &binding;
  vertex_input.vertexAttributeDescriptionCount = 6;
  vertex_input.pVertexAttributeDescriptions = attributes;
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
  attachment.blendEnable = VK_TRUE;  // premultiplied
  attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  attachment.colorBlendOp = VK_BLEND_OP_ADD;
  attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  attachment.alphaBlendOp = VK_BLEND_OP_ADD;
  attachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;
  const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  ui_format_ = color_format_;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &ui_format_;
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
  info.layout = ui_layout_;
  VkResult result = vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &ui_pipeline_);
  if (result == VK_SUCCESS) {
    if (ui_format_ == VK_FORMAT_R8G8B8A8_UNORM) {
      ui_pipeline_sdr_ = ui_pipeline_;
    } else {
      const VkFormat sdr_fmt = VK_FORMAT_R8G8B8A8_UNORM;
      rendering.pColorAttachmentFormats = &sdr_fmt;
      result = vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &ui_pipeline_sdr_);
    }
  }
  vkDestroyShaderModule(device_, vert, nullptr);
  vkDestroyShaderModule(device_, frag, nullptr);
  VK_TRY(result, "UI pipeline");
  return true;
}

void Renderer::record_ui(VkCommandBuffer cmd, VkImage final_image, VkImageView final_view, VkImageLayout& layout,
                         VkPipelineStageFlags2& last_stage, VkAccessFlags2& last_access, VkPipeline pipeline,
                         uint32_t width, uint32_t height) {
  if (ui_items_.empty() && ui_puts_.empty()) return;
  if (!ui_ready_) {
    if (ui_failed_) return;
    std::string e;
    if (!create_ui(e)) {
      std::fprintf(stderr, "carview: UI layer off: %s\n", e.c_str());
      ui_failed_ = true;
      return;
    }
    ui_ready_ = true;
  }
  if (!pipeline) pipeline = (ui_format_ == VK_FORMAT_R8G8B8A8_UNORM) ? ui_pipeline_sdr_ : ui_pipeline_;
  // Atlas uploads: staged, copied, back to sampled.
  if (!ui_puts_.empty()) {
    VkDeviceSize bytes = 0;
    for (const auto& p : ui_puts_) bytes += (VkDeviceSize(p.rgba.size()) + 15) & ~VkDeviceSize(15);
    FrameSlot& fs = slot();
    if (fs.ui_staging.size < bytes) {
      // This slot's last frame is done (its fence): the buffer is free to replace.
      if (fs.ui_staging_mapped) vmaUnmapMemory(allocator_, fs.ui_staging.allocation);
      fs.ui_staging_mapped = nullptr;
      destroy_buffer(fs.ui_staging);
      if (create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, fs.ui_staging) &&
          vmaMapMemory(allocator_, fs.ui_staging.allocation, &fs.ui_staging_mapped) != VK_SUCCESS)
        fs.ui_staging_mapped = nullptr;
    }
    if (fs.ui_staging_mapped) {
      std::vector<VkBufferImageCopy> regions;
      regions.reserve(ui_puts_.size());
      VkDeviceSize at = 0;
      for (const auto& p : ui_puts_) {
        std::memcpy(static_cast<uint8_t*>(fs.ui_staging_mapped) + at, p.rgba.data(), p.rgba.size());
        VkBufferImageCopy r{};
        r.bufferOffset = at;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageOffset = {int32_t(p.x), int32_t(p.y), 0};
        r.imageExtent = {p.w, p.h, 1};
        regions.push_back(r);
        at += (VkDeviceSize(p.rgba.size()) + 15) & ~VkDeviceSize(15);
      }
      image_barrier(cmd, ui_atlas_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
      vkCmdCopyBufferToImage(cmd, fs.ui_staging.buffer, ui_atlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             uint32_t(regions.size()), regions.data());
      image_barrier(cmd, ui_atlas_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      ui_puts_.clear();
    }
  } else {
    // Release oversized transient UI staging buffers (> 8 MiB; keeps 1280x720 FMV staging hot)
    // once this slot's fence has passed.
    FrameSlot& fs = slot();
    if (fs.ui_staging.size > (8ULL << 20)) {
      if (fs.ui_staging_mapped) vmaUnmapMemory(allocator_, fs.ui_staging.allocation);
      fs.ui_staging_mapped = nullptr;
      destroy_buffer(fs.ui_staging);
    }
  }
  if (ui_items_.empty() || !pipeline) return;
  const VkDeviceSize bytes = ui_items_.size() * sizeof(ui::Instance);
  FrameSlot& us = slot();
  if (us.ui_instances.size < bytes) {
    if (us.ui_instances_mapped) vmaUnmapMemory(allocator_, us.ui_instances.allocation);
    us.ui_instances_mapped = nullptr;
    destroy_buffer(us.ui_instances);
    const VkDeviceSize cap = std::max<VkDeviceSize>(bytes * 2, 64 * 1024);
    if (!create_buffer(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, us.ui_instances) ||
        vmaMapMemory(allocator_, us.ui_instances.allocation, &us.ui_instances_mapped) != VK_SUCCESS) {
      us.ui_instances_mapped = nullptr;
      return;
    }
  }
  const float inv_w = 2.0f / float(std::max(1u, width));
  const float inv_h = 2.0f / float(std::max(1u, height));
  const float hud_scale = hud_layout_.virtual_w > 0 ? hud_layout_.scale : 1.0f;
  const float off_x = hud_layout_.virtual_w > 0 ? hud_layout_.origin_x : 0.0f;
  const float off_y = hud_layout_.virtual_w > 0 ? hud_layout_.origin_y : 0.0f;
  const bool need_fullscreen_fixup =
      hud_layout_.virtual_w > 0 &&
      (off_x != 0.0f || off_y != 0.0f || hud_scale != 1.0f ||
       std::fabs(hud_layout_.virtual_w - float(width)) > 0.5f ||
       std::fabs(hud_layout_.virtual_h - float(height)) > 0.5f);
  if (!need_fullscreen_fixup) {
    std::memcpy(us.ui_instances_mapped, ui_items_.data(), size_t(bytes));
  } else {
    const float hs = std::max(1e-4f, hud_scale);
    const float vx0 = -off_x / hs;
    const float vy0 = -off_y / hs;
    const float vx1 = (float(width) - off_x) / hs;
    const float vy1 = (float(height) - off_y) / hs;
    const float vw_phys = vx1 - vx0;
    const float vh_phys = vy1 - vy0;
    const float phys_aspect = float(width) / std::max(1.0f, float(height));
    auto* dst = static_cast<ui::Instance*>(us.ui_instances_mapped);
    for (size_t i = 0; i < ui_items_.size(); ++i) {
      ui::Instance it = ui_items_[i];
      if ((it.flags & ui::kUiFlagFullscreen) != 0u) {
        it.rect[0] = vx0;
        it.rect[1] = vy0;
        it.rect[2] = vx1;
        it.rect[3] = vy1;
      } else if ((it.flags & ui::kUiFlagFullscreenCover) != 0u) {
        const float video_aspect = it.r[0] > 0.01f ? it.r[0] : (16.0f / 9.0f);
        float draw_w = vw_phys, draw_h = vh_phys;
        float du0 = it.m[0], dv0 = it.m[1], du1 = it.m[2], dv1 = it.m[3];
        const float u_span = du1 - du0;
        const float v_span = dv1 - dv0;
        if (phys_aspect < video_aspect) {
          const float keep = std::clamp(phys_aspect / video_aspect, 0.25f, 1.0f);
          const float trim = 0.5f * (1.0f - keep) * u_span;
          du0 += trim;
          du1 -= trim;
        } else if (phys_aspect > video_aspect * 1.005f) {
          if (phys_aspect <= video_aspect * 1.25f) {
            const float keep = std::clamp(video_aspect / phys_aspect, 0.5f, 1.0f);
            const float trim = 0.5f * (1.0f - keep) * v_span;
            dv0 += trim;
            dv1 -= trim;
          } else {
            draw_w = vh_phys * video_aspect;
          }
        }
        const float cx = 0.5f * (vx0 + vx1);
        const float cy = 0.5f * (vy0 + vy1);
        it.rect[0] = cx - 0.5f * draw_w;
        it.rect[1] = cy - 0.5f * draw_h;
        it.rect[2] = cx + 0.5f * draw_w;
        it.rect[3] = cy + 0.5f * draw_h;
        it.q[0] = du0;
        it.q[1] = dv0;
        it.q[2] = du1;
        it.q[3] = dv1;
      }
      dst[i] = it;
    }
  }
  image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                last_stage, last_access, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
  layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  last_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  last_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = final_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, {width, height}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  vkCmdBeginRendering(cmd, &rendering);
  VkViewport viewport{0, 0, float(width), float(height), 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, {width, height}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_layout_, 0, 1, &ui_set_, 0, nullptr);
  const float xform[4] = {hud_scale * inv_w, hud_scale * inv_h, off_x * inv_w - 1.0f, off_y * inv_h - 1.0f};
  vkCmdPushConstants(cmd, ui_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof xform, xform);
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &us.ui_instances.buffer, &zero);
  vkCmdDraw(cmd, 6, uint32_t(ui_items_.size()), 0, 0);
  vkCmdEndRendering(cmd);
  mark(cmd, "ui");
}

void Renderer::set_overlay(std::vector<uint8_t> rgba, uint32_t width, uint32_t height, int32_t x, int32_t y,
                           int layer) {
  if (layer < 0 || layer >= kOverlayLayers) return;
  if (rgba.size() != size_t(width) * height * 4) rgba.clear();
  OverlayLayer& l = overlays_[layer];
  l.pixels = std::move(rgba);
  l.width = l.pixels.empty() ? 0 : width;
  l.height = l.pixels.empty() ? 0 : height;
  l.x = x;
  l.y = y;
  l.dirty = true;
  ++l.version;
}

bool Renderer::stage_overlay(VkFormat final_format) {
  overlay_staged_ = false;
  for (auto& l : overlays_) l.staged = false;
  const bool modern_chain = uses_modern_chain();
  if (!overlay_supported_ && !modern_chain) return false;
  // Where each layer that fits goes, packed one after another (16-byte
  // aligned offsets, as copies want a multiple of the texel size).
  VkDeviceSize bytes = 0, offsets[kOverlayLayers]{};
  for (int i = 0; i < kOverlayLayers; ++i) {
    OverlayLayer& l = overlays_[i];
    if (l.pixels.empty()) continue;
    const int64_t x = l.x < 0 ? int64_t(target_width_) - l.width + l.x + 1 : l.x;
    const int64_t y = l.y < 0 ? int64_t(target_height_) - l.height + l.y + 1 : l.y;
    if (x < 0 || y < 0 || x + l.width > target_width_ || y + l.height > target_height_) continue;
    l.at = {int32_t(x), int32_t(y)};
    l.staged = true;
    offsets[i] = bytes;
    bytes += (VkDeviceSize(l.pixels.size()) + 15) & ~VkDeviceSize(15);
  }
  if (bytes == 0) return false;
  bool all = false;  // every staged layer is copied again (a new buffer, moved offsets)
  FrameSlot& os = slot();
  if (os.overlay.size < bytes) {
    if (os.overlay_mapped) vmaUnmapMemory(allocator_, os.overlay.allocation);
    os.overlay_mapped = nullptr;
    destroy_buffer(os.overlay);
    if (!create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, os.overlay))
      return false;
    if (vmaMapMemory(allocator_, os.overlay.allocation, &os.overlay_mapped) != VK_SUCCESS) {
      os.overlay_mapped = nullptr;
      return false;
    }
    all = true;
  }
  // Overlays copy into stand_in_ (VK_FORMAT_R8G8B8A8_UNORM) when the modern
  // chain is active, and directly into final_image otherwise. Match the exact
  // destination format so R/B channels are never double-swapped!
  const VkFormat eff_final = (final_format == VK_FORMAT_UNDEFINED) ? color_format_ : final_format;
  const VkFormat dst_fmt = modern_chain ? VK_FORMAT_R8G8B8A8_UNORM : eff_final;
  const bool bgra = dst_fmt == VK_FORMAT_B8G8R8A8_UNORM || dst_fmt == VK_FORMAT_B8G8R8A8_SRGB;
  for (int i = 0; i < kOverlayLayers; ++i) {
    OverlayLayer& l = overlays_[i];
    if (!l.staged) continue;
    // Each slot's buffer remembers which version & channel order of a layer it holds.
    if (!all && os.overlay_ver[i] == l.version && os.overlay_off[i] == offsets[i] && os.overlay_bgra[i] == bgra) continue;
    os.overlay_off[i] = offsets[i];
    os.overlay_ver[i] = l.version;
    os.overlay_bgra[i] = bgra;
    auto* out = static_cast<uint8_t*>(os.overlay_mapped) + os.overlay_off[i];
    for (size_t k = 0; k < l.pixels.size(); k += 4) {
      out[k + 0] = l.pixels[k + (bgra ? 2 : 0)];
      out[k + 1] = l.pixels[k + 1];
      out[k + 2] = l.pixels[k + (bgra ? 0 : 2)];
      out[k + 3] = l.pixels[k + 3];
    }
  }
  overlay_staged_ = true;
  return true;
}

}  // namespace carview
