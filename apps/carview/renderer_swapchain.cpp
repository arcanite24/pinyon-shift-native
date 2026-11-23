#include "renderer_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>

namespace carview {

bool Renderer::render_offscreen(const FrameConstants& constants, uint32_t width, uint32_t height,
                                std::vector<uint8_t>& rgba, std::string& error,
                                std::vector<float>* depth) {
  if (depth && (samples_ != VK_SAMPLE_COUNT_1_BIT || depth_format_ != VK_FORMAT_D32_SFLOAT)) {
    error = "depth readback needs one sample and a D32_SFLOAT depth buffer";
    return false;
  }
  depth_readable_ = depth != nullptr;
  if (options_.window) wait_idle();  // the targets below are shared with frames in flight
  if (!create_targets(width, height, error)) return false;
  if (lut_dirty_ && lut_pending_size_ != lut_image_size_ && post_final_) {
    if (!ensure_lut_image(lut_pending_size_, error)) return false;
  }
  const VkFormat out_fmt = (hdr_mode_ == modern::HdrMode::kOff) ? color_format_ : VK_FORMAT_R8G8B8A8_UNORM;
  Image output;
  if (!create_image(width, height, 1, out_fmt, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, output)) {
    error = "offscreen colour image";
    return false;
  }
  Buffer readback;
  const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
  const VkDeviceSize readback_bytes = depth ? bytes * 2 : bytes;  // RGBA8, then f32 depth
  if (!create_buffer(readback_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     readback)) {
    destroy_image(output);
    error = "readback buffer";
    return false;
  }
  VkBuffer inspector_dst = VK_NULL_HANDLE;
  const VkDeviceSize insp_bytes = VkDeviceSize(kInspectorPreviewW) * kInspectorPreviewH * 4;
  if (inspector_active_) {
    if (!inspector_offscreen_buf_.buffer) {
      create_buffer(insp_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    inspector_offscreen_buf_);
    }
    inspector_dst = inspector_offscreen_buf_.buffer;
  }
  const bool time_offscreen = gpu_timing_ && timestamp_ns_ > 0;
  if (time_offscreen && !timestamps_) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kTimestamps;
    if (vkCreateQueryPool(device_, &info, nullptr, &timestamps_) != VK_SUCCESS) timestamp_ns_ = 0;
  }
  stage_overlay(out_fmt);  // submit_once waits for the queue: no frame is using the buffer
  const bool ok = submit_once([&](VkCommandBuffer cmd) {
    mark_labels_.clear();
    ts_pool_ = timestamps_;
    if (time_offscreen && timestamps_) {
      vkCmdResetQueryPool(cmd, timestamps_, 0, kTimestamps);
      vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps_, 0);
      timing_ = true;
    }
    record_scene(cmd, output.image, output.view, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, constants,
                 VK_NULL_HANDLE, false, out_fmt, inspector_dst);
    if (timing_) {
      mark_end_ = uint32_t(mark_labels_.size()) + 1;
      vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamps_, mark_end_);
      timing_ = false;
    }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer, 1, &copy);
    if (depth) {
      VkBufferImageCopy depth_copy{};
      depth_copy.bufferOffset = bytes;
      depth_copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      depth_copy.imageExtent = {width, height, 1};
      vkCmdCopyImageToBuffer(cmd, depth_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback.buffer, 1, &depth_copy);
    }
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(cmd, &dependency);
  });
  bool readback_ok = ok;
  if (ok) {
    void* mapped = nullptr;
    if (vmaMapMemory(allocator_, readback.allocation, &mapped) == VK_SUCCESS) {
      rgba.assign(static_cast<uint8_t*>(mapped), static_cast<uint8_t*>(mapped) + bytes);
      if (depth) {
        depth->resize(size_t(width) * height);
        std::memcpy(depth->data(), static_cast<uint8_t*>(mapped) + bytes, size_t(bytes));
      }
      vmaUnmapMemory(allocator_, readback.allocation);
    } else {
      error = "offscreen readback map failed";
      readback_ok = false;
    }
    if (inspector_active_ && inspector_dst != VK_NULL_HANDLE) {
      void* imapped = nullptr;
      if (vmaMapMemory(allocator_, inspector_offscreen_buf_.allocation, &imapped) == VK_SUCCESS) {
        inspector_rgba_.assign(static_cast<uint8_t*>(imapped), static_cast<uint8_t*>(imapped) + insp_bytes);
        vmaUnmapMemory(allocator_, inspector_offscreen_buf_.allocation);
        inspector_has_rgba_ = true;
      }
    }
    if (time_offscreen && timestamps_) {
      uint64_t ticks[kTimestamps] = {};
      if (vkGetQueryPoolResults(device_, timestamps_, 0, mark_end_ + 1, sizeof ticks, ticks, sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        auto ms = [&](uint32_t a, uint32_t b) { return double(ticks[b] - ticks[a]) * timestamp_ns_ / 1e6; };
        gpu_frame_ms_ = ms(0, mark_end_);
        slot_stages_.clear();
        for (uint32_t i = 0; i < mark_labels_.size(); ++i) slot_stages_.push_back({mark_labels_[i], ms(i, i + 1)});
        slot_stages_.push_back({"rest", ms(uint32_t(mark_labels_.size()), mark_end_)});
      }
    }
  } else {
    error = "offscreen render submission failed";
  }
  destroy_buffer(readback);
  destroy_image(output);
  if (depth_readable_) {
    destroy_targets();
    depth_readable_ = false;
  }
  return readback_ok;
}

bool Renderer::render_timed(const FrameConstants& constants, uint32_t width, uint32_t height, double& record_ms,
                            double& gpu_ms, std::string& error) {
  if ((targets_dirty_ || target_width_ != width || target_height_ != height) && !create_targets(width, height, error))
    return false;
  if (lut_dirty_ && lut_pending_size_ != lut_image_size_ && post_final_) {
    if (!ensure_lut_image(lut_pending_size_, error)) return false;
  }
  if (!bench_output_.image &&
      !create_image(width, height, 1, color_format_, VK_SAMPLE_COUNT_1_BIT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                    bench_output_)) {
    error = "benchmark colour image";
    return false;
  }
  if (timestamp_ns_ > 0 && !timestamps_) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kTimestamps;
    if (vkCreateQueryPool(device_, &info, nullptr, &timestamps_) != VK_SUCCESS) timestamp_ns_ = 0;
  }
  stage_overlay();
  double recorded = 0;
  const auto submit_t0 = std::chrono::steady_clock::now();
  const bool ok = submit_once([&](VkCommandBuffer cmd) {
    const auto t0 = std::chrono::steady_clock::now();
    mark_labels_.clear();
    ts_pool_ = timestamps_;
    if (timestamps_) {
      vkCmdResetQueryPool(cmd, timestamps_, 0, kTimestamps);
      vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps_, 0);
      timing_ = true;
    }
    record_scene(cmd, bench_output_.image, bench_output_.view, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, constants);
    timing_ = false;
    if (timestamps_) mark_end_ = uint32_t(mark_labels_.size()) + 1;
    if (timestamps_) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamps_, mark_end_);
    recorded = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  });
  if (!ok) {
    error = "benchmark submission failed";
    return false;
  }
  record_ms = recorded;
  gpu_ms = -1;
  uint64_t ticks[kTimestamps] = {};
  stage_ms_.clear();
  if (timestamps_ && vkGetQueryPoolResults(device_, timestamps_, 0, mark_end_ + 1, sizeof ticks, ticks,
                                           sizeof(uint64_t),
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
    auto ms = [&](uint32_t a, uint32_t b) { return double(ticks[b] - ticks[a]) * timestamp_ns_ / 1e6; };
    gpu_ms = ms(0, mark_end_);
    for (uint32_t i = 0; i < mark_labels_.size(); ++i) stage_ms_.push_back({mark_labels_[i], ms(i, i + 1)});
    stage_ms_.push_back({"rest", ms(uint32_t(mark_labels_.size()), mark_end_)});
    gpu_frame_ms_ = gpu_ms;
    slot_stages_ = stage_ms_;
  }
  const double total_submit_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submit_t0).count();
  last_submit_wait_ms_ = std::max(0.0, total_submit_ms - recorded);
  return true;
}

void Renderer::mark(VkCommandBuffer cmd, const char* label) {
  if (!timing_ || !ts_pool_ || mark_labels_.size() + 2 >= kTimestamps) return;
  mark_labels_.push_back(label);
  vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ts_pool_, uint32_t(mark_labels_.size()));
}

bool Renderer::create_swapchain(std::string& error) {
  VkSurfaceCapabilitiesKHR caps;
  VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps),
         "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data());
    hdr10_supported_ = false;
    scrgb_supported_ = false;
    VkFormat sdr_fmt = formats.empty() ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
    for (const auto& f : formats) {
      if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
          f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        sdr_fmt = f.format;
      }
      if (hdr_colorspace_ext_ && f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
          f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
        hdr10_supported_ = true;
      }
      if (hdr_colorspace_ext_ && f.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
          f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
        scrgb_supported_ = true;
      }
    }
    hdr_mode_ = modern::resolve_hdr(graphics_.hdr, hdr10_supported_, scrgb_supported_);
    VkFormat wanted_fmt = sdr_fmt;
    VkColorSpaceKHR wanted_cs = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (hdr_mode_ == modern::HdrMode::kScRgb) {
      wanted_fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
      wanted_cs = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    } else if (hdr_mode_ == modern::HdrMode::kHdr10) {
      wanted_fmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      wanted_cs = VK_COLOR_SPACE_HDR10_ST2084_EXT;
    }
    if (color_format_ != VK_FORMAT_UNDEFINED && wanted_fmt != color_format_) {
      if (ui_pipeline_ && ui_pipeline_ != ui_pipeline_sdr_) vkDestroyPipeline(device_, ui_pipeline_, nullptr);
      ui_pipeline_ = VK_NULL_HANDLE;
      ui_ready_ = false;
      destroy_modern_passes();
    }
    color_format_ = wanted_fmt;
    swapchain_colorspace_ = wanted_cs;
  }
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(options_.window, &w, &h);
    extent = {uint32_t(std::max(w, 1)), uint32_t(std::max(h, 1))};
  }
  extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
  extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
  if (extent.width == 0 || extent.height == 0) {
    swapchain_dirty_ = true;  // minimised: try again later
    return true;
  }
  uint32_t image_count = caps.minImageCount + 1;
  if (caps.maxImageCount) image_count = std::min(image_count, caps.maxImageCount);
  VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = surface_;
  info.minImageCount = image_count;
  info.imageFormat = color_format_;
  info.imageColorSpace = swapchain_colorspace_;
  info.imageExtent = extent;
  info.imageArrayLayers = 1;
  // Transfers into the image carry the overlay panel (set_overlay); transfers
  // out of the image enable F8/Photo Mode swapchain PNG capture.
  overlay_supported_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
  capture_supported_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    (overlay_supported_ ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0) |
                    (capture_supported_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  {
    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, modes.data());
    uint32_t mask = modern::vsync_bit(modern::VSync::kFifo);
    for (VkPresentModeKHR m : modes) {
      if (m == VK_PRESENT_MODE_MAILBOX_KHR) mask |= modern::vsync_bit(modern::VSync::kMailbox);
      if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mask |= modern::vsync_bit(modern::VSync::kImmediate);
    }
    vsync_active_ = modern::resolve_vsync(vsync_, mask);
    info.presentMode = vsync_active_ == modern::VSync::kImmediate ? VK_PRESENT_MODE_IMMEDIATE_KHR
                       : vsync_active_ == modern::VSync::kMailbox ? VK_PRESENT_MODE_MAILBOX_KHR
                                                                   : VK_PRESENT_MODE_FIFO_KHR;
    // Mailbox and immediate want a spare image.
    if (vsync_active_ != modern::VSync::kFifo && (!caps.maxImageCount || info.minImageCount < caps.maxImageCount))
      info.minImageCount = std::max(info.minImageCount, std::min(caps.minImageCount + 2, caps.maxImageCount ? caps.maxImageCount : caps.minImageCount + 2));
  }
  info.clipped = VK_TRUE;
  info.oldSwapchain = swapchain_;
  VkSwapchainKHR created = VK_NULL_HANDLE;
  VK_TRY(vkCreateSwapchainKHR(device_, &info, nullptr, &created), "vkCreateSwapchainKHR");
  destroy_swapchain(false);
  swapchain_ = created;
  swapchain_extent_ = extent;
  uint32_t count = 0;
  vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
  swapchain_images_.resize(count);
  vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchain_images_.data());
  for (VkImage image : swapchain_images_) {
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = color_format_;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView handle;
    VK_TRY(vkCreateImageView(device_, &view, nullptr, &handle), "swapchain image view");
    swapchain_views_.push_back(handle);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore done;
    VK_TRY(vkCreateSemaphore(device_, &semaphore, nullptr, &done), "vkCreateSemaphore");
    render_done_.push_back(done);
  }
  swapchain_dirty_ = false;
  ++swapchain_recreate_count_;
  // Skip rebuilding offscreen render targets and post descriptors when swapchain
  // pixel dimensions and color format are unchanged (fast alt-tab / focus recovery).
  const bool has_targets = depth_.image != VK_NULL_HANDLE && (!options_.post.enabled || hdr_.image != VK_NULL_HANDLE);
  if (needs_target_recreate(targets_dirty_, has_targets, extent.width, extent.height, target_width_, target_height_,
                            color_format_ != target_color_format_)) {
    return create_targets(extent.width, extent.height, error);
  }
  return true;
}

void Renderer::destroy_swapchain(bool keep_handle) {
  for (VkImageView view : swapchain_views_) vkDestroyImageView(device_, view, nullptr);
  for (VkSemaphore s : render_done_) vkDestroySemaphore(device_, s, nullptr);
  swapchain_views_.clear();
  render_done_.clear();
  swapchain_images_.clear();
  if (!keep_handle && swapchain_) {
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
}

void Renderer::request_capture(bool include_ui) {
  capture_requested_ = true;
  capture_include_ui_ = include_ui;
  capture_ready_ = false;
}

bool Renderer::take_captured_frame(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
  if (!capture_ready_) return false;
  rgba = std::move(capture_rgba_);
  width = capture_w_;
  height = capture_h_;
  capture_ready_ = false;
  capture_w_ = 0;
  capture_h_ = 0;
  return !rgba.empty() && width > 0 && height > 0;
}

// Reads the timestamps of a slot's last frame (its fence is done).
void Renderer::collect_slot_timing(FrameSlot& s) {
  if (!s.timed || !s.timestamps) return;
  s.timed = false;
  uint64_t ticks[kTimestamps] = {};
  if (vkGetQueryPoolResults(device_, s.timestamps, 0, s.mark_end + 1, sizeof ticks, ticks, sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
    return;
  auto ms = [&](uint32_t a, uint32_t b) { return double(ticks[b] - ticks[a]) * timestamp_ns_ / 1e6; };
  gpu_frame_ms_ = ms(0, s.mark_end);
  slot_stages_.clear();
  for (uint32_t i = 0; i < s.labels.size(); ++i) slot_stages_.push_back({s.labels[i], ms(i, i + 1)});
  slot_stages_.push_back({"rest", ms(uint32_t(s.labels.size()), s.mark_end)});
}

bool Renderer::draw_frame(const FrameConstants& constants, std::string& error) {
  FrameSlot& fs = slot();
  // This slot's last frame (two submissions ago) must be done before its
  // command buffer, staging buffers and queries are reused; the other
  // slot's frame may still be drawing.
  vkWaitForFences(device_, 1, &fs.fence, VK_TRUE, UINT64_MAX);
  if (fs.submit_id > frames_done_) frames_done_ = fs.submit_id;
  collect_slot_timing(fs);
  if (fs.inspector_ready && fs.inspector_readback_mapped) {
    const size_t insp_bytes = size_t(kInspectorPreviewW) * kInspectorPreviewH * 4;
    inspector_rgba_.assign(static_cast<const uint8_t*>(fs.inspector_readback_mapped),
                           static_cast<const uint8_t*>(fs.inspector_readback_mapped) + insp_bytes);
    inspector_has_rgba_ = true;
    fs.inspector_ready = false;
  }
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(options_.window, &w, &h);
  if (w <= 0 || h <= 0) return true;  // minimised
  if (needs_swapchain_recreate(swapchain_dirty_, swapchain_ != VK_NULL_HANDLE, uint32_t(w), uint32_t(h),
                               swapchain_extent_.width, swapchain_extent_.height)) {
    wait_idle();
    if (!create_swapchain(error)) return false;
    if (!swapchain_) return true;
  }
  const bool has_targets = depth_.image != VK_NULL_HANDLE && (!options_.post.enabled || hdr_.image != VK_NULL_HANDLE);
  if (needs_target_recreate(targets_dirty_, has_targets, swapchain_extent_.width, swapchain_extent_.height,
                            target_width_, target_height_, color_format_ != target_color_format_)) {
    wait_idle();
    if (!create_targets(swapchain_extent_.width, swapchain_extent_.height, error)) return false;
  }
  if (lut_dirty_ && lut_pending_size_ != lut_image_size_ && post_final_) {
    if (!ensure_lut_image(lut_pending_size_, error)) return false;
  }
  uint32_t index = 0;
  // Use a finite acquire timeout so occluded/unfocused MoltenVK or DXGI surfaces
  // never stall the SDL event loop for 1+ seconds during alt-tab / Cmd-Tab.
  const uint64_t acquire_timeout_ns = window_focused_ ? 50'000'000ULL : 16'000'000ULL;
  VkResult result =
      vkAcquireNextImageKHR(device_, swapchain_, acquire_timeout_ns, fs.image_ready, VK_NULL_HANDLE, &index);
  if (result == VK_TIMEOUT || result == VK_NOT_READY) {
    return true;
  }
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    swapchain_dirty_ = true;
    return true;
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    error = "vkAcquireNextImageKHR failed (" + std::to_string(int(result)) + ")";
    return false;
  }
  const bool capture_direct_fmt =
      color_format_ == VK_FORMAT_B8G8R8A8_UNORM || color_format_ == VK_FORMAT_R8G8B8A8_UNORM ||
      color_format_ == VK_FORMAT_B8G8R8A8_SRGB || color_format_ == VK_FORMAT_R8G8B8A8_SRGB;
  bool do_capture = false;
  const VkDeviceSize cap_bytes = VkDeviceSize(swapchain_extent_.width) * swapchain_extent_.height * 4;
  if (capture_requested_ && capture_supported_ && capture_direct_fmt && cap_bytes > 0) {
    if (capture_buffer_.size < cap_bytes) {
      destroy_buffer(capture_buffer_);
      create_buffer(cap_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, capture_buffer_);
    }
    do_capture = capture_buffer_.buffer != VK_NULL_HANDLE;
  }
  vkResetFences(device_, 1, &fs.fence);
  vkResetCommandBuffer(fs.cmd, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(fs.cmd, &begin);
  mark_labels_.clear();
  ts_pool_ = fs.timestamps;
  timing_ = gpu_timing_ && fs.timestamps != VK_NULL_HANDLE;
  if (timing_) {
    vkCmdResetQueryPool(fs.cmd, fs.timestamps, 0, kTimestamps);
    vkCmdWriteTimestamp2(fs.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, fs.timestamps, 0);
  }
  VkBuffer inspector_dst = VK_NULL_HANDLE;
  if (inspector_active_) {
    const VkDeviceSize insp_bytes = VkDeviceSize(kInspectorPreviewW) * kInspectorPreviewH * 4;
    if (!fs.inspector_readback.buffer) {
      if (create_buffer(insp_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        fs.inspector_readback)) {
        vmaMapMemory(allocator_, fs.inspector_readback.allocation, &fs.inspector_readback_mapped);
      }
    }
    inspector_dst = fs.inspector_readback.buffer;
  }
  stage_overlay();  // the slot's fence above: its last frame is done with its buffers
  record_scene(fs.cmd, swapchain_images_[index], swapchain_views_[index],
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, constants,
               do_capture ? capture_buffer_.buffer : VK_NULL_HANDLE, !capture_include_ui_,
               VK_FORMAT_UNDEFINED, inspector_dst);
  if (inspector_dst != VK_NULL_HANDLE) fs.inspector_ready = true;
  if (timing_) {
    fs.labels = mark_labels_;
    fs.mark_end = uint32_t(fs.labels.size()) + 1;
    vkCmdWriteTimestamp2(fs.cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, fs.timestamps, fs.mark_end);
    fs.timed = true;
    timing_ = false;
  }
  vkEndCommandBuffer(fs.cmd);
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &fs.image_ready;
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &fs.cmd;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &render_done_[index];
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    VK_TRY(vkQueueSubmit(queue_, 1, &submit, fs.fence), "vkQueueSubmit");
  }
  fs.submit_id = ++frames_submitted_;
  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &render_done_[index];
  present.swapchainCount = 1;
  present.pSwapchains = &swapchain_;
  present.pImageIndices = &index;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    result = vkQueuePresentKHR(queue_, &present);
  }
  slot_ = (slot_ + 1) % frames_in_flight_;
  if (do_capture) {
    vkWaitForFences(device_, 1, &fs.fence, VK_TRUE, UINT64_MAX);
    if (fs.submit_id > frames_done_) frames_done_ = fs.submit_id;
    void* mapped = nullptr;
    if (vmaMapMemory(allocator_, capture_buffer_.allocation, &mapped) == VK_SUCCESS) {
      capture_w_ = swapchain_extent_.width;
      capture_h_ = swapchain_extent_.height;
      capture_rgba_.resize(size_t(cap_bytes));
      const auto* src = static_cast<const uint8_t*>(mapped);
      const bool bgra = color_format_ == VK_FORMAT_B8G8R8A8_UNORM || color_format_ == VK_FORMAT_B8G8R8A8_SRGB;
      for (size_t i = 0; i < size_t(cap_bytes); i += 4) {
        capture_rgba_[i + 0] = src[i + (bgra ? 2 : 0)];
        capture_rgba_[i + 1] = src[i + 1];
        capture_rgba_[i + 2] = src[i + (bgra ? 0 : 2)];
        capture_rgba_[i + 3] = 255;
      }
      vmaUnmapMemory(allocator_, capture_buffer_.allocation);
      capture_ready_ = true;
    }
    capture_requested_ = false;
  } else if (capture_requested_ && (!capture_supported_ || !capture_direct_fmt)) {
    // Fallback when swapchain does not support TRANSFER_SRC_BIT: render offscreen.
    wait_idle();  // render_offscreen rebuilds the targets the other frame may still use
    std::string cap_err;
    std::vector<uint8_t> saved_ui_items_guard;
    const auto saved_ui = ui_items_;
    if (!capture_include_ui_) ui_items_.clear();
    if (render_offscreen(constants, swapchain_extent_.width, swapchain_extent_.height, capture_rgba_, cap_err)) {
      const bool bgra = color_format_ == VK_FORMAT_B8G8R8A8_UNORM || color_format_ == VK_FORMAT_B8G8R8A8_SRGB;
      for (size_t i = 0; i < capture_rgba_.size(); i += 4) {
        if (bgra) std::swap(capture_rgba_[i + 0], capture_rgba_[i + 2]);
        capture_rgba_[i + 3] = 255;
      }
      capture_w_ = swapchain_extent_.width;
      capture_h_ = swapchain_extent_.height;
      capture_ready_ = true;
    }
    if (!capture_include_ui_) ui_items_ = saved_ui;
    capture_requested_ = false;
  }
  int cur_w = 0, cur_h = 0;
  SDL_GetWindowSizeInPixels(options_.window, &cur_w, &cur_h);
  if (should_mark_dirty_on_present(result, uint32_t(std::max(0, cur_w)), uint32_t(std::max(0, cur_h)),
                                   swapchain_extent_.width, swapchain_extent_.height)) {
    swapchain_dirty_ = true;
  } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    error = "vkQueuePresentKHR failed (" + std::to_string(int(result)) + ")";
    return false;
  }
  return true;
}


}  // namespace carview
