#include "renderer_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "car_frag_spv.h"
#include "car_model.h"
#include "car_vert_spv.h"

namespace carview {

bool Renderer::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                             VkMemoryPropertyFlags flags, Buffer& out) {
  if (!allocator_ || size == 0) return false;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
    alloc.flags = (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT)
                      ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                      : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
      alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  }
  if (vmaCreateBuffer(allocator_, &info, &alloc, &out.buffer, &out.allocation, nullptr) != VK_SUCCESS) {
    out = Buffer{};
    return false;
  }
  out.size = size;
  return true;
}

void Renderer::destroy_buffer(Buffer& b) {
  if ((b.buffer || b.allocation) && allocator_) vmaDestroyBuffer(allocator_, b.buffer, b.allocation);
  b = Buffer{};
}

bool Renderer::create_image(uint32_t width, uint32_t height, uint32_t levels, VkFormat format,
                            VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                            VkImageAspectFlags aspect, Image& out) {
  if (!allocator_) return false;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = levels;
  info.arrayLayers = 1;
  info.samples = samples;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  if ((usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) && lazy_memory_supported_) {
    alloc.preferredFlags |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
  }
  VmaAllocationInfo alloc_info{};
  if (vmaCreateImage(allocator_, &info, &alloc, &out.image, &out.allocation, &alloc_info) != VK_SUCCESS) {
    out = Image{};
    return false;
  }
  out.bytes = alloc_info.size;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = out.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {aspect, 0, levels, 0, 1};
  if (vkCreateImageView(device_, &view, nullptr, &out.view) != VK_SUCCESS) {
    destroy_image(out);
    return false;
  }
  return true;
}

void Renderer::destroy_image(Image& i) {
  if (i.view && device_) vkDestroyImageView(device_, i.view, nullptr);
  if ((i.image || i.allocation) && allocator_) vmaDestroyImage(allocator_, i.image, i.allocation);
  i = Image{};
}

// A one-time submit that waits for its own work only (a fence), never the
// whole queue: frames in flight keep drawing, and any thread may call it. On
// a thread other than the render thread it uses the second queue when the
// family has one (asynchronous uploads).
bool Renderer::submit_once(const std::function<void(VkCommandBuffer)>& record) {
  const bool render_thread = std::this_thread::get_id() == render_thread_;
  // A command pool is externally synchronised: the render thread has its
  // own, other threads borrow one from the free list for the call.
  VkCommandPool use_pool = pool_;
  if (!render_thread) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (!free_pools_.empty()) {
      use_pool = free_pools_.back();
      free_pools_.pop_back();
    } else {
      VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pool.queueFamilyIndex = queue_family_;
      if (vkCreateCommandPool(device_, &pool, nullptr, &use_pool) != VK_SUCCESS) return false;
      all_pools_.push_back(use_pool);
    }
  }
  auto give_back = [&] {
    if (render_thread) return;
    std::lock_guard<std::mutex> lock(pool_mutex_);
    free_pools_.push_back(use_pool);
  };
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = use_pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd;
  if (vkAllocateCommandBuffers(device_, &alloc, &cmd) != VK_SUCCESS) {
    give_back();
    return false;
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  record(cmd);
  vkEndCommandBuffer(cmd);
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  bool ok = vkCreateFence(device_, &fence_info, nullptr, &fence) == VK_SUCCESS;
  if (ok) {
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    const bool async = !render_thread && transfer_queue_ != VK_NULL_HANDLE;
    {
      // One mutex for both queues keeps this simple: a submit is brief.
      std::lock_guard<std::mutex> lock(queue_mutex_);
      ok = vkQueueSubmit(async ? transfer_queue_ : queue_, 1, &submit, fence) == VK_SUCCESS;
    }
    ok = ok && vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
  }
  if (fence) vkDestroyFence(device_, fence, nullptr);
  vkFreeCommandBuffers(device_, use_pool, 1, &cmd);
  give_back();
  return ok;
}


VkDeviceAddress Renderer::buffer_device_address(VkBuffer buffer) const {
  if (!device_ || !buffer || !rt_caps_.buffer_device_address) return 0;
  VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  info.buffer = buffer;
  return vkGetBufferDeviceAddress(device_, &info);
}

bool Renderer::upload_buffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
                             Buffer& out) {
  staging_upload_bytes_ += uint64_t(size);
  Buffer staging;
  if (!create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     staging))
    return false;
  void* mapped = nullptr;
  if (vmaMapMemory(allocator_, staging.allocation, &mapped) != VK_SUCCESS) {
    destroy_buffer(staging);
    return false;
  }
  std::memcpy(mapped, data, size_t(size));
  vmaUnmapMemory(allocator_, staging.allocation);
  bool ok = create_buffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
  if (ok) {
    ok = submit_once([&](VkCommandBuffer cmd) {
      VkBufferCopy copy{0, 0, size};
      vkCmdCopyBuffer(cmd, staging.buffer, out.buffer, 1, &copy);
    });
  }
  destroy_buffer(staging);
  return ok;
}

bool Renderer::replace_geometry(const CarModel& model, std::string& error) {
  vkDeviceWaitIdle(device_);
  destroy_buffer(vertex_buffer_);
  destroy_buffer(index_buffer_);
  if (!upload_buffer(model.vertices.data(), model.vertices.size() * sizeof(Vertex),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer_) ||
      !upload_buffer(model.indices.data(), model.indices.size() * sizeof(uint32_t),
                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer_)) {
    error = "mesh upload failed";
    return false;
  }
  index_count_ = uint32_t(model.indices.size());
  return true;
}

bool Renderer::upload(const CarModel& model, std::string& error) {
  if (!upload_buffer(model.vertices.data(), model.vertices.size() * sizeof(Vertex),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer_) ||
      !upload_buffer(model.indices.data(), model.indices.size() * sizeof(uint32_t),
                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer_)) {
    error = "mesh upload failed";
    return false;
  }
  index_count_ = uint32_t(model.indices.size());

  // The atlas, every decoded level, as RGBA8.
  const auto& levels = model.texture_levels;
  VkDeviceSize total = 0;
  for (const auto& level : levels) total += level.rgba.size();
  Buffer staging;
  if (!create_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     staging)) {
    error = "texture staging buffer";
    return false;
  }
  uint8_t* mapped = nullptr;
  if (vmaMapMemory(allocator_, staging.allocation, reinterpret_cast<void**>(&mapped)) != VK_SUCCESS) {
    destroy_buffer(staging);
    error = "texture staging map";
    return false;
  }
  std::vector<VkBufferImageCopy> copies;
  VkDeviceSize offset = 0;
  for (uint32_t i = 0; i < levels.size(); ++i) {
    std::memcpy(mapped + offset, levels[i].rgba.data(), levels[i].rgba.size());
    VkBufferImageCopy copy{};
    copy.bufferOffset = offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
    copy.imageExtent = {levels[i].width, levels[i].height, 1};
    copies.push_back(copy);
    offset += levels[i].rgba.size();
  }
  vmaUnmapMemory(allocator_, staging.allocation);
  const uint32_t level_count = uint32_t(levels.size());
  if (!create_image(levels[0].width, levels[0].height, level_count, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, texture_)) {
    destroy_buffer(staging);
    error = "texture image";
    return false;
  }
  const bool ok = submit_once([&](VkCommandBuffer cmd) {
    image_barrier(cmd, texture_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, level_count);
    vkCmdCopyBufferToImage(cmd, staging.buffer, texture_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                           copies.data());
    image_barrier(cmd, texture_.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                  level_count);
  });
  destroy_buffer(staging);
  if (!ok) {
    error = "texture upload";
    return false;
  }

  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = VK_FILTER_LINEAR;
  sampler.minFilter = VK_FILTER_LINEAR;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler.maxLod = float(level_count);
  VK_TRY(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "vkCreateSampler");
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = 1;
  pool.poolSizeCount = 1;
  pool.pPoolSizes = &size;
  VK_TRY(vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_),
         "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = descriptor_pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &set_layout_;
  VK_TRY(vkAllocateDescriptorSets(device_, &alloc, &descriptor_set_), "vkAllocateDescriptorSets");
  VkDescriptorImageInfo image_info{sampler_, texture_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = descriptor_set_;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &image_info;
  vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  return true;
}

bool Renderer::create_pipeline(VkFormat color_format, std::string& error) {
  VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set.bindingCount = 1;
  set.pBindings = &binding;
  VK_TRY(vkCreateDescriptorSetLayout(device_, &set, nullptr, &set_layout_),
         "vkCreateDescriptorSetLayout");
  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(FrameConstants)};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &set_layout_;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &push;
  VK_TRY(vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_),
         "vkCreatePipelineLayout");

  auto make_module = [&](const uint32_t* code, size_t bytes, VkShaderModule& module) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    return vkCreateShaderModule(device_, &info, nullptr, &module);
  };
  VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
  VK_TRY(make_module(kCar_vert_spv, sizeof(kCar_vert_spv), vert), "vertex shader");
  VK_TRY(make_module(kCar_frag_spv, sizeof(kCar_frag_spv), frag), "fragment shader");
  VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr}};

  VkVertexInputBindingDescription vertex_binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attributes[4] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
      {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
      {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
      {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, occlusion)}};
  VkPipelineVertexInputStateCreateInfo vertex_input{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &vertex_binding;
  vertex_input.vertexAttributeDescriptionCount = 4;
  vertex_input.pVertexAttributeDescriptions = attributes;
  VkPipelineInputAssemblyStateCreateInfo assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;  // strip winding is not established yet
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = samples_;
  VkPipelineDepthStencilStateCreateInfo depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_TRUE;
  depth.depthCompareOp = options_.reverse_z ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS;
  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_attachment;
  const VkDynamicState dynamic_states[3] = {
      VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = vrs_supported_ ? 3u : 2u;
  dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &color_format;
  rendering.depthAttachmentFormat = depth_format_;
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
  info.layout = pipeline_layout_;
  const VkResult result =
      vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &pipeline_);
  vkDestroyShaderModule(device_, vert, nullptr);
  vkDestroyShaderModule(device_, frag, nullptr);
  VK_TRY(result, "vkCreateGraphicsPipelines");
  return true;
}

bool Renderer::create_targets(uint32_t width, uint32_t height, std::string& error) {
  destroy_targets();
  const modern::Extent re = options_.post.enabled
                                ? modern::scene_extent({width, height}, graphics_.scale, graphics_.base_540p)
                                : modern::Extent{width, height};
  const uint32_t rw = re.w ? re.w : width;
  const uint32_t rh = re.h ? re.h : height;
  if (samples_ != VK_SAMPLE_COUNT_1_BIT &&
      !create_image(rw, rh, 1, scene_format(), samples_,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, msaa_color_)) {
    error = "MSAA colour target";
    return false;
  }
  const VkImageUsageFlags depth_usage =
      samples_ != VK_SAMPLE_COUNT_1_BIT
          ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
          : (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  if (!create_image(rw, rh, 1, depth_format_, samples_, depth_usage,
                    VK_IMAGE_ASPECT_DEPTH_BIT, depth_)) {
    error = "depth target";
    return false;
  }
  if (!create_image(rw, rh, 1, depth_format_, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, depth_resolved_)) {
    error = "resolved depth target";
    return false;
  }
  if (options_.post.enabled) {
    // Resolved HDR scene (always needed when post is on) + lazily allocated optional passes.
    const bool rt_on = active_rt_mode() != rt::RtMode::kOff;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags hdr_usage =
        usage | (rt_on ? (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                       : 0u);
    if (!create_image(rw, rh, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, hdr_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                      hdr_)) {
      error = "post hdr target";
      return false;
    }
    const auto post_t0 = std::chrono::steady_clock::now();
    if (!create_post(error)) return false;
    if (!create_modern_passes(error)) return false;
    pipeline_compile_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - post_t0).count();
    const auto up_choice = modern::resolve_upscaler(graphics_.upscaler, upscaler_runtime_.support, {rw, rh},
                                                    {width, height});
    const bool need_brightness = (display_brightness_offset_ > 1e-5f || display_brightness_offset_ < -1e-5f);
    const modern::ModernTargetPlan plan = modern::plan_modern_targets(
        graphics_, {rw, rh}, {width, height}, options_.post.bloom, hdr_mode_, hdr_pattern_, inspector_active_,
        inspector_view_ == InspectorView::kMotion, up_choice, options_.post.motion_blur, need_brightness);
    allocated_targets_ = plan;

    if (plan.bloom) {
      // 6-level Jimenez / Dual-Kawase pyramid starting at half-resolution (rw/2 x rh/2).
      uint32_t bw = std::max(1u, rw / 2), bh = std::max(1u, rh / 2);
      for (uint32_t k = 0; k < kBloomLevels; ++k) {
        bloom_w_[k] = bw;
        bloom_h_[k] = bh;
        if (!create_image(bw, bh, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                          bloom_a_[k]) ||
            !create_image(bw, bh, 1, scene_format(), VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                          bloom_b_[k])) {
          error = "post bloom pyramid";
          return false;
        }
        bw = std::max(1u, bw / 2);
        bh = std::max(1u, bh / 2);
      }
    }
    if (plan.ssao) {
      const bool full_ao = modern::gtao_full_res(graphics_.ssao);
      const uint32_t aow = full_ao ? rw : std::max(1u, rw / 2);
      const uint32_t aoh = full_ao ? rh : std::max(1u, rh / 2);
      ao_width_ = aow;
      ao_height_ = aoh;
      if (!create_image(aow, aoh, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, ao_raw_) ||
          !create_image(aow, aoh, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, ao_tmp_) ||
          !create_image(aow, aoh, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, ao_blur_)) {
        error = "post gtao targets";
        return false;
      }
    }
    if (plan.sscs) {
      const bool full_cs = modern::contact_shadow_full_res(graphics_.contact_shadows);
      sscs_width_ = full_cs ? rw : std::max(1u, rw / 2);
      sscs_height_ = full_cs ? rh : std::max(1u, rh / 2);
      if (!create_image(sscs_width_, sscs_height_, 1, VK_FORMAT_R8_UNORM, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, sscs_)) {
        error = "post sscs target";
        return false;
      }
    }
    if (plan.ssr) {
      const bool full_ssr = modern::ssr_full_res(graphics_.ssr);
      ssr_width_ = full_ssr ? rw : std::max(1u, rw / 2);
      ssr_height_ = full_ssr ? rh : std::max(1u, rh / 2);
      if (!create_image(ssr_width_, ssr_height_, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, ssr_raw_) ||
          !create_image(ssr_width_, ssr_height_, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, ssr_blur_)) {
        error = "post ssr targets";
        return false;
      }
    }
    if (plan.volumetric) {
      const uint32_t div = std::max(2u, modern::volumetric_scale_div(graphics_.volumetric_fog));
      vol_width_ = std::max(1u, rw / div);
      vol_height_ = std::max(1u, rh / div);
      if (!create_image(vol_width_, vol_height_, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, vol_raw_) ||
          !create_image(vol_width_, vol_height_, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, vol_blur_)) {
        error = "post volumetric targets";
        return false;
      }
    }
    const VkImageUsageFlags sdr_usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (plan.modern_chain) {
      if (!create_image(rw, rh, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, sdr_usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, scene_sdr_) ||
          !create_image(width, height, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, sdr_usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, stand_in_)) {
        error = "post modern targets";
        return false;
      }
    }
    if (plan.fxaa_scene &&
        !create_image(rw, rh, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, sdr_usage,
                      VK_IMAGE_ASPECT_COLOR_BIT, scene_fxaa_)) {
      error = "post fxaa target";
      return false;
    }
    if (plan.fxaa_standin &&
        !create_image(width, height, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, sdr_usage,
                      VK_IMAGE_ASPECT_COLOR_BIT, stand_in_fxaa_)) {
      error = "post standin fxaa target";
      return false;
    }
    if (plan.upscale_tmp &&
        !create_image(width, height, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, sdr_usage,
                      VK_IMAGE_ASPECT_COLOR_BIT, upscale_tmp_)) {
      error = "post upscale target";
      return false;
    }
    if (plan.motion_vec &&
        !create_image(rw, rh, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                      motion_vec_)) {
      error = "post motion target";
      return false;
    }
    if (plan.mblur_tiles) {
      mblur_tile_w_ = std::max(1u, (rw + 15u) / 16u);
      mblur_tile_h_ = std::max(1u, (rh + 15u) / 16u);
      if (!create_image(mblur_tile_w_, mblur_tile_h_, 1, VK_FORMAT_R16G16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, mblur_tile_) ||
          !create_image(mblur_tile_w_, mblur_tile_h_, 1, VK_FORMAT_R16G16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, usage,
                        VK_IMAGE_ASPECT_COLOR_BIT, mblur_neighbor_)) {
        error = "post mblur tile targets";
        return false;
      }
    }
    if (plan.temporal_hist &&
        (!create_image(width, height, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, sdr_usage,
                       VK_IMAGE_ASPECT_COLOR_BIT, temporal_hist_[0]) ||
         !create_image(width, height, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, sdr_usage,
                       VK_IMAGE_ASPECT_COLOR_BIT, temporal_hist_[1]))) {
      error = "post temporal targets";
      return false;
    }
    if (plan.inspector &&
        !create_image(kInspectorPreviewW, kInspectorPreviewH, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, inspector_thumb_)) {
      error = "inspector thumbnail image";
      return false;
    }
    temporal_reset_ = true;
    temporal_idx_ = 0;
    // Wire the 14 post descriptor sets:
    //   0: lum16 (hdr, hdr)
    //   1: lum_adapt (lum_16, lum_adapt[1])
    //   2: bloom_down[0] (hdr, hdr) -> bloom_a[0] (Karis prefilter)
    //   3..7: bloom_down[1..5] (bloom_a[k-1], bloom_a[k-1]) -> bloom_a[k]
    //   8: bloom_up[4] (bloom_a[4], bloom_a[5]) -> bloom_b[4]
    //   9: bloom_up[3] (bloom_a[3], bloom_b[4]) -> bloom_b[3]
    //   10: bloom_up[2] (bloom_a[2], bloom_b[3]) -> bloom_b[2]
    //   11: bloom_up[1] (bloom_a[1], bloom_b[2]) -> bloom_b[1]
    //   12: bloom_up[0] (bloom_a[0], bloom_b[1]) -> bloom_b[0]
    //   13: composite (hdr, bloom_b[0])
    const VkImageView fallback_v = ao_white_.view;
    const VkImageView vel_fb = vel_zero_.view ? vel_zero_.view : fallback_v;
    auto bv = [&](const Image& img) { return img.view ? img.view : fallback_v; };
    VkImageView sources[kPostSetCount][2] = {};
    sources[0][0] = hdr_.view; sources[0][1] = hdr_.view;
    sources[1][0] = lum_16_.view; sources[1][1] = lum_adapt_[1].view;
    sources[2][0] = hdr_.view; sources[2][1] = bv(bloom_b_[0]);
    bloom_prev_valid_ = false;
    for (uint32_t k = 1; k < kBloomLevels; ++k) {
      sources[2 + k][0] = bv(bloom_a_[k - 1]); sources[2 + k][1] = bv(bloom_a_[k - 1]);
    }
    sources[8][0] = bv(bloom_a_[4]);  sources[8][1] = bv(bloom_a_[5]);
    sources[9][0] = bv(bloom_a_[3]);  sources[9][1] = bv(bloom_b_[4]);
    sources[10][0] = bv(bloom_a_[2]); sources[10][1] = bv(bloom_b_[3]);
    sources[11][0] = bv(bloom_a_[1]); sources[11][1] = bv(bloom_b_[2]);
    sources[12][0] = bv(bloom_a_[0]); sources[12][1] = bv(bloom_b_[1]);
    sources[13][0] = hdr_.view;       sources[13][1] = bv(bloom_b_[0]);

    std::vector<VkDescriptorImageInfo> images(kPostSetCount * 11);
    std::vector<VkDescriptorBufferInfo> buf_infos(kPostSetCount);
    std::vector<VkWriteDescriptorSet> writes(kPostSetCount * 12);
    const VkImageView ao_view =
        (!rt_on && graphics_.ssao != modern::SsaoQuality::kOff && ao_blur_.view) ? ao_blur_.view : ao_white_.view;
    const VkImageView sscs_view =
        (!rt_on && graphics_.contact_shadows != modern::ContactShadows::kOff && sscs_.view) ? sscs_.view
                                                                                            : ao_white_.view;
    const VkImageView motion_view = motion_vec_.view ? motion_vec_.view : vel_fb;
    const VkImageView neighbor_view = mblur_neighbor_.view ? mblur_neighbor_.view : vel_fb;
    const VkImageView ssr_view =
        (!rt_on && graphics_.ssr != modern::SsrQuality::kOff && ssr_blur_.view) ? ssr_blur_.view : ssr_zero_.view;
    const VkImageView vol_view =
        (!rt_on && graphics_.volumetric_fog != modern::VolumetricFog::kOff && vol_blur_.view) ? vol_blur_.view
                                                                                               : vol_identity_.view;
    for (uint32_t s = 0; s < kPostSetCount; ++s) {
      // Only the final composite (s == 13) samples lum_adapt_[0]; pass 1 renders into lum_adapt_[0],
      // so earlier sets bind lum_adapt_[1] (already SHADER_READ_ONLY_OPTIMAL) to avoid a layout conflict.
      const VkImageView adapt_view = (s == 13) ? lum_adapt_[0].view : lum_adapt_[1].view;
      const VkImageView views[4] = {sources[s][0], sources[s][1], lut3d_.view, adapt_view};
      for (uint32_t b = 0; b < 4; ++b) {
        const size_t iidx = size_t(s) * 11 + b;
        const size_t widx = size_t(s) * 12 + b;
        images[iidx] = {post_sampler_, views[b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet& w = writes[widx];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = post_sets_[s];
        w.dstBinding = b;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &images[iidx];
      }
      {
        const size_t iidx = size_t(s) * 11 + 4;
        const size_t widx = size_t(s) * 12 + 4;
        images[iidx] = {post_depth_sampler_, depth_resolved_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet& w = writes[widx];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = post_sets_[s];
        w.dstBinding = 4;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &images[iidx];
      }
      {
        const size_t widx = size_t(s) * 12 + 5;
        buf_infos[s] = {post_optical_buf_.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet& w = writes[widx];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = post_sets_[s];
        w.dstBinding = 5;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &buf_infos[s];
      }
      const VkImageView extra_views[6] = {ao_view, sscs_view, motion_view, neighbor_view, ssr_view, vol_view};
      for (uint32_t e = 0; e < 6; ++e) {
        const size_t iidx = size_t(s) * 11 + 5 + e;
        const size_t widx = size_t(s) * 12 + 6 + e;
        images[iidx] = {post_sampler_, extra_views[e], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet& w = writes[widx];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = post_sets_[s];
        w.dstBinding = 6 + e;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &images[iidx];
      }
    }
    vkUpdateDescriptorSets(device_, uint32_t(writes.size()), writes.data(), 0, nullptr);
    update_modern_descriptors();
  }
  render_width_ = rw;
  render_height_ = rh;
  target_width_ = width;
  target_height_ = height;
  target_color_format_ = color_format_;
  targets_dirty_ = false;
  ++target_recreate_count_;
  return true;
}

bool Renderer::ensure_shadow_map(std::string& error) {
  if (shadow_map_.view) return true;
  uint32_t size = options_.shadow.size ? options_.shadow.size : 1;
  const uint32_t cascades = (size > 1 && options_.shadow.cascades >= 3) ? 3u : 2u;
  if (size > 1 && physical_) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_, &props);
    const uint32_t max_side = std::max(512u, props.limits.maxImageDimension2D / cascades);
    if (size > max_side) size = max_side;
  }
  const uint32_t width = size > 1 ? size * cascades : 1;
  if (!create_image(width, size, 1, kShadowFormat, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT,
                    shadow_map_)) {
    error = "shadow map";
    return false;
  }
  shadow_size_ = size;
  shadow_cascades_ = size > 1 ? cascades : 1u;
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = VK_FILTER_LINEAR;
  sampler.minFilter = VK_FILTER_LINEAR;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  sampler.compareEnable = VK_TRUE;
  sampler.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  sampler.maxLod = 0.0f;
  if (vkCreateSampler(device_, &sampler, nullptr, &shadow_sampler_) != VK_SUCCESS) {
    error = "shadow sampler";
    return false;
  }
  VkSamplerCreateInfo raw_sampler = sampler;
  raw_sampler.magFilter = VK_FILTER_NEAREST;
  raw_sampler.minFilter = VK_FILTER_NEAREST;
  raw_sampler.compareEnable = VK_FALSE;
  if (vkCreateSampler(device_, &raw_sampler, nullptr, &shadow_raw_sampler_) != VK_SUCCESS) {
    error = "shadow raw sampler";
    return false;
  }
  // Cleared to the far plane (lit) and left ready to sample.
  const bool ok = submit_once([&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = shadow_map_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    const VkClearDepthStencilValue far{1.0f, 0};
    vkCmdClearDepthStencilImage(cmd, shadow_map_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1,
                                &b.subresourceRange);
    b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
  });
  if (!ok) error = "shadow map clear";
  return ok;
}


void Renderer::destroy_targets() {
  destroy_image(msaa_color_);
  destroy_image(depth_);
  destroy_image(depth_resolved_);
  destroy_image(hdr_);
  destroy_image(ao_raw_);
  destroy_image(ao_tmp_);
  destroy_image(ao_blur_);
  destroy_image(sscs_);
  destroy_image(ssr_raw_);
  destroy_image(ssr_blur_);
  destroy_image(vol_raw_);
  destroy_image(vol_blur_);
  destroy_image(scene_sdr_);
  destroy_image(scene_fxaa_);
  destroy_image(upscale_tmp_);
  destroy_image(stand_in_);
  destroy_image(stand_in_fxaa_);
  destroy_image(motion_vec_);
  destroy_image(mblur_tile_);
  destroy_image(mblur_neighbor_);
  destroy_image(temporal_hist_[0]);
  destroy_image(temporal_hist_[1]);
  destroy_image(inspector_thumb_);
  allocated_targets_ = {};
  temporal_reset_ = true;
  prev_cam_vp_valid_ = false;
  ao_width_ = ao_height_ = 0;
  sscs_width_ = sscs_height_ = 0;
  ssr_width_ = ssr_height_ = 0;
  vol_width_ = vol_height_ = 0;
  mblur_tile_w_ = mblur_tile_h_ = 0;
  for (uint32_t k = 0; k < kBloomLevels; ++k) {
    destroy_image(bloom_a_[k]);
    destroy_image(bloom_b_[k]);
    bloom_w_[k] = bloom_h_[k] = 0;
  }
  target_width_ = target_height_ = 0;
  render_width_ = render_height_ = 0;
}

}  // namespace carview
