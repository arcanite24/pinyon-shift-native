#include "renderer_internal.h"

#include <algorithm>
#include <cstring>

namespace carview {

void Renderer::record_scene(VkCommandBuffer cmd, VkImage final_image, VkImageView final_view,
                            VkImageLayout final_layout, const FrameConstants& constants,
                            VkBuffer capture_dst, bool capture_before_ui, VkFormat final_format,
                            VkBuffer inspector_dst) {
  if (final_format == VK_FORMAT_UNDEFINED) final_format = color_format_;
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;

  // Fast path when a 100% opaque fullscreen overlay (World Map or FMV) covers the
  // viewport: skip 3D scene, shadows, GTAO, Hi-Z, and post passes completely.
  const bool need_brightness = (display_brightness_offset_ > 1e-5f || display_brightness_offset_ < -1e-5f);
  const bool need_hdr_stage_fast =
      (hdr_mode_ != modern::HdrMode::kOff) || graphics_.hdr_compare || (hdr_pattern_ != 0) || need_brightness;
  if (skip_scene_ && !inspector_active_ && capture_dst == VK_NULL_HANDLE &&
      (!need_hdr_stage_fast || (uses_modern_chain() && stand_in_.view != VK_NULL_HANDLE && hdr_pipe_sdr_ != VK_NULL_HANDLE))) {
    const bool route_standin = need_hdr_stage_fast;
    VkImage dst_img = route_standin ? stand_in_.image : final_image;
    VkImageView dst_view = route_standin ? stand_in_.view : final_view;
    VkPipeline dst_ui_pipe = route_standin ? ui_pipeline_sdr_ : ui_pipeline_;
    image_barrier(cmd, dst_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, color_stage, VK_ACCESS_2_NONE,
                  color_stage, color_write);
    VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkPipelineStageFlags2 last_stage = color_stage;
    VkAccessFlags2 last_access = color_write;
    if (overlay_staged_) {
      image_barrier(cmd, dst_img, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    last_stage, last_access, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
      VkBufferImageCopy regions[kOverlayLayers]{};
      uint32_t count = 0;
      for (int li = 0; li < kOverlayLayers; ++li) {
        const auto& l = overlays_[li];
        if (!l.staged) continue;
        VkBufferImageCopy& region = regions[count++];
        region.bufferOffset = slot().overlay_off[li];
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {l.at.x, l.at.y, 0};
        region.imageExtent = {l.width, l.height, 1};
      }
      vkCmdCopyBufferToImage(cmd, slot().overlay.buffer, dst_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count,
                             regions);
      layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      last_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
      last_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    } else {
      VkRenderingAttachmentInfo clr{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      clr.imageView = dst_view;
      clr.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      clr.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      clr.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      clr.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
      VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
      ri.renderArea = {{0, 0}, {target_width_, target_height_}};
      ri.layerCount = 1;
      ri.colorAttachmentCount = 1;
      ri.pColorAttachments = &clr;
      vkCmdBeginRendering(cmd, &ri);
      vkCmdEndRendering(cmd);
    }
    record_ui(cmd, dst_img, dst_view, layout, last_stage, last_access, dst_ui_pipe, target_width_,
              target_height_);
    if (route_standin) {
      image_barrier(cmd, stand_in_.image, VK_IMAGE_ASPECT_COLOR_BIT, layout,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, last_stage, last_access,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
      image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, color_stage, VK_ACCESS_2_NONE,
                    color_stage, color_write);
      record_hdr_output(cmd, hdr_set_standin_, final_image, final_view, final_format, target_width_, target_height_);
      layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      last_stage = color_stage;
      last_access = color_write;
    }
    if (final_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
      if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, final_layout, last_stage, last_access,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      }
    } else {
      image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, final_layout, last_stage, last_access,
                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    }
    return;
  }

  const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
  if (scene_.before) scene_.before(cmd);
  const bool post = options_.post.enabled && hdr_.view;
  // Defer final_image (swapchain) COLOR_ATTACHMENT_OUTPUT barrier when rendering to
  // offscreen HDR first: lets shadows, 3D scene, Hi-Z, and post passes overlap with
  // swapchain presentation instead of stalling on image_ready before scene draw.
  if (!post) {
    image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, color_stage, VK_ACCESS_2_NONE,
                  color_stage, color_write);
  }
  if (msaa) {
    image_barrier(cmd, msaa_color_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, color_stage, VK_ACCESS_2_NONE,
                  color_stage, color_write);
  }
  const uint32_t rw = render_width();
  const uint32_t rh = render_height();
  // Under the post chain the scene goes to the HDR image (resolved into
  // it with MSAA), and the composite writes the final image.
  const VkImageView scene_view = post ? hdr_.view : final_view;
  if (post)
    image_barrier(cmd, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
                  color_stage, color_write);
  const auto depth_stages =
      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  image_barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, depth_stages,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depth_stages,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

  const VkImageView depth_res_view = depth_resolved_view();
  const bool two_pass = bool(scene_.draw_particles) && depth_res_view != VK_NULL_HANDLE &&
                        (!scene_.has_particles || scene_.has_particles());
  const bool need_hiz = bool(scene_.build_hiz) && depth_res_view != VK_NULL_HANDLE;
  const bool can_msaa_depth_resolve = msaa && depth_resolve_supported_ && depth_resolved_.view != VK_NULL_HANDLE;
  const bool gmem_msaa_resolve = can_msaa_depth_resolve;
  if (msaa && (two_pass || post || need_hiz) && depth_resolved_.image) {
    image_barrier(cmd, depth_resolved_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  can_msaa_depth_resolve ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
                                         : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE,
                  can_msaa_depth_resolve ? depth_stages : VK_PIPELINE_STAGE_2_COPY_BIT,
                  can_msaa_depth_resolve ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                         : VK_ACCESS_2_TRANSFER_WRITE_BIT);
  }

  auto set_vrs = [&](uint32_t w, uint32_t h) {
    if (!vrs_supported_ || !vkCmdSetFragmentShadingRateKHR) return;
    const VkExtent2D sz{w, h};
    const VkFragmentShadingRateCombinerOpKHR ops[2] = {
        VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
    vkCmdSetFragmentShadingRateKHR(cmd, &sz, ops);
  };

  const bool run_z_prepass = options_.z_prepass && bool(scene_.draw_z_prepass);
  if (run_z_prepass) {
    VkRenderingAttachmentInfo z_depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    z_depth.imageView = depth_.view;
    z_depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    z_depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    z_depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    z_depth.clearValue.depthStencil = {options_.reverse_z ? 0.0f : 1.0f, 0};
    VkRenderingInfo z_rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
    z_rend.renderArea = {{0, 0}, {rw, rh}};
    z_rend.layerCount = 1;
    z_rend.pDepthAttachment = &z_depth;
    vkCmdBeginRendering(cmd, &z_rend);
    VkViewport viewport{0, 0, float(rw), float(rh), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {rw, rh}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    set_vrs(1, 1);
    scene_.draw_z_prepass(cmd);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, depth_stages,
                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depth_stages,
                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    mark(cmd, "zpre");
  }

  const bool resolve_pass1 = msaa && (!two_pass || gmem_msaa_resolve);
  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = msaa ? msaa_color_.view : scene_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = resolve_pass1 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color = {{scene_.clear[0], scene_.clear[1], scene_.clear[2], scene_.clear[3]}};
  if (post) {
    for (int k = 0; k < 3; ++k) {
      // The clear colour is display (sRGB) encoded: linear for the HDR scene.
      const float c = scene_.clear[k];
      color.clearValue.color.float32[k] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    // Clear HDR alpha (surface gloss channel for Hi-Z SSR) to 0.0 so sky/matte areas stay reflection-free.
    color.clearValue.color.float32[3] = 0.0f;
  }
  if (resolve_pass1) {
    color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    color.resolveImageView = scene_view;
    color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = depth_.view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = run_z_prepass ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
  const bool need_depth_store =
      depth_readable_ || (!gmem_msaa_resolve && two_pass) || (!msaa && (post || need_hiz));
  depth.storeOp = need_depth_store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.clearValue.depthStencil = {options_.reverse_z ? 0.0f : 1.0f, 0};
  if (can_msaa_depth_resolve && (two_pass || post || need_hiz)) {
    depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    depth.resolveImageView = depth_resolved_.view;
    depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  }
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, {rw, rh}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  rendering.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &rendering);
  in_render_pass_ = true;
  VkViewport viewport{0, 0, float(rw), float(rh), 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, {rw, rh}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  set_vrs(graphics_.vrs ? 2u : 1u, graphics_.vrs ? 2u : 1u);
  if (scene_.draw) {
    scene_.draw(cmd);
  } else if (index_count_ > 0 && vertex_buffer_.buffer != VK_NULL_HANDLE &&
             index_buffer_.buffer != VK_NULL_HANDLE) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                            &descriptor_set_, 0, nullptr);
    vkCmdPushConstants(cmd, pipeline_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(FrameConstants), &constants);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, index_count_, 1, 0, 0, 0);
  }
  in_render_pass_ = false;
  vkCmdEndRendering(cmd);
  if (lazy_memory_supported_) mark(cmd, "scene");

  VkImageLayout depth_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  if (two_pass || post || need_hiz) {
    const auto read_stages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (can_msaa_depth_resolve) {
      image_barrier(cmd, depth_resolved_.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    depth_stages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    read_stages | depth_stages,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    } else if (!msaa) {
      // Zero-copy 1x MSAA depth: transition depth_ directly to DEPTH_STENCIL_READ_ONLY_OPTIMAL
      // so particles (depthWrite=false), Hi-Z, GTAO, SSCS, SSR, volumetrics, and post sample it
      // without allocating a second D32 buffer or executing a per-frame vkCmdCopyImage.
      image_barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, depth_stages,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    read_stages | depth_stages,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
      depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    } else if (depth_resolved_.image) {
      // Fallback if GPU lacks MSAA depth resolve: clear resolved depth to far plane.
      const VkClearDepthStencilValue far_v{options_.reverse_z ? 0.0f : 1.0f, 0};
      const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      vkCmdClearDepthStencilImage(cmd, depth_resolved_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far_v, 1, &rng);
      image_barrier(cmd, depth_resolved_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, read_stages,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    if (need_hiz) {
      scene_.build_hiz(cmd, depth_res_view, rw, rh, options_.reverse_z);
      mark(cmd, "hiz_build");
    }
  }

  if (two_pass) {
    if (gmem_msaa_resolve) {
      // Draw soft particles at 1x MSAA directly onto the resolved HDR/final image with
      // read-only resolved depth so Pass 1 keeps MSAA color & depth 100% transient in GMEM.
      const VkImage resolved_col_img = post ? hdr_.image : final_image;
      image_barrier(cmd, resolved_col_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, color_stage, color_write, color_stage,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | color_write);
      color.imageView = scene_view;
      color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color.resolveMode = VK_RESOLVE_MODE_NONE;
      color.resolveImageView = VK_NULL_HANDLE;
      depth.imageView = depth_resolved_.view;
      depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
      depth.resolveMode = VK_RESOLVE_MODE_NONE;
      depth.resolveImageView = VK_NULL_HANDLE;
    } else if (!msaa) {
      color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      depth.imageView = depth_.view;
      depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
      depth.resolveMode = VK_RESOLVE_MODE_NONE;
      depth.resolveImageView = VK_NULL_HANDLE;
    } else {
      color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
      color.resolveImageView = scene_view;
      color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = depth_readable_ ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
      depth.resolveMode = VK_RESOLVE_MODE_NONE;
      depth.resolveImageView = VK_NULL_HANDLE;
    }
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    set_vrs(1, 1);
    scene_.draw_particles(cmd);
    vkCmdEndRendering(cmd);
    mark(cmd, "particles");
  }

  const bool rt_active = post && (active_rt_mode() != rt::RtMode::kOff) && bool(scene_.record_rt);
  if (post && !rt_active) {
    image_barrier(cmd, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    record_ssao(cmd);
    record_sscs(cmd);
    record_ssr(cmd);
    record_volumetric(cmd);
  }
  const bool need_mblur_tiles =
      post && options_.post.motion_blur && graphics_.velocity_blur != modern::VelocityBlur::kOff &&
      mblur_tile_.view != VK_NULL_HANDLE && mblur_neighbor_.view != VK_NULL_HANDLE;
  const bool need_motion =
      post && (active_upscaler().temporal || need_mblur_tiles || rt_active ||
               (inspector_active_ && inspector_view_ == InspectorView::kMotion));
  if (need_motion && motion_vec_.view != VK_NULL_HANDLE) {
    record_motion_vectors(cmd);
  } else if (post && motion_vec_.image) {
    image_barrier(cmd, motion_vec_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  }
  if (rt_active) {
    image_barrier(cmd, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_GENERAL, color_stage, color_write,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const VkImageView mv = motion_vec_.view ? motion_vec_.view : vel_zero_.view;
    scene_.record_rt(cmd, hdr_.image, hdr_.view, depth_res_view, mv, rw, rh);
    image_barrier(cmd, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  }
  if (need_mblur_tiles) {
    record_mblur_tiles(cmd);
  } else if (post && mblur_neighbor_.image) {
    image_barrier(cmd, mblur_neighbor_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  }

  const bool need_hdr_stage =
      (hdr_mode_ != modern::HdrMode::kOff) || graphics_.hdr_compare || (hdr_pattern_ != 0) || need_brightness;
  const bool need_fxaa_world = (graphics_.aa == modern::AaMode::kFxaaWorld) && fxaa_pipe_ != VK_NULL_HANDLE;
  const bool need_fxaa_all = (graphics_.aa == modern::AaMode::kFxaaAll) && fxaa_pipe_ != VK_NULL_HANDLE;
  const bool use_modern_chain = uses_modern_chain();

  if (depth_readable_) {
    image_barrier(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                  depth_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  depth_stages | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  }

  VkImageLayout layout = post ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkPipelineStageFlags2 last_stage = color_stage;
  VkAccessFlags2 last_access = post ? VK_ACCESS_2_NONE : color_write;
  auto record_capture_copy = [&]() {
    image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  last_stage, last_access, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {target_width_, target_height_, 1};
    vkCmdCopyImageToBuffer(cmd, final_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture_dst, 1, &copy);
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(cmd, &dependency);
    layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    last_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    last_access = VK_ACCESS_2_TRANSFER_READ_BIT;
  };
  auto copy_overlays = [&](VkImage dst_img, VkImageLayout& dst_layout, VkPipelineStageFlags2& dst_stage,
                           VkAccessFlags2& dst_access) {
    if (!overlay_staged_) return;
    image_barrier(cmd, dst_img, VK_IMAGE_ASPECT_COLOR_BIT, dst_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  dst_stage, dst_access, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy regions[kOverlayLayers]{};
    uint32_t count = 0;
    for (int li = 0; li < kOverlayLayers; ++li) {
      const auto& l = overlays_[li];
      if (!l.staged) continue;
      VkBufferImageCopy& region = regions[count++];
      region.bufferOffset = slot().overlay_off[li];
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageOffset = {l.at.x, l.at.y, 0};
      region.imageExtent = {l.width, l.height, 1};
    }
    vkCmdCopyBufferToImage(cmd, slot().overlay.buffer, dst_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count, regions);
    dst_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  };

  if (!use_modern_chain) {
    if (post) {
      image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    last_stage, last_access, color_stage, color_write);
      layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      last_stage = color_stage;
      last_access = color_write;
      record_post(cmd, final_view, post_final_, 1.0f);
    }
    if (capture_dst != VK_NULL_HANDLE && capture_before_ui) record_capture_copy();
    copy_overlays(final_image, layout, last_stage, last_access);
    record_ui(cmd, final_image, final_view, layout, last_stage, last_access, ui_pipeline_, target_width_,
              target_height_);
    if (capture_dst != VK_NULL_HANDLE && !capture_before_ui) record_capture_copy();
  } else {
    image_barrier(cmd, scene_sdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_NONE, color_stage, color_write);
    record_post(cmd, scene_sdr_.view, post_final_sdr_, 0.0f);
    image_barrier(cmd, scene_sdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    VkImage cur_img = scene_sdr_.image;
    VkDescriptorSet cur_up_set = upscale_set_scene_;
    bool cur_is_fxaa = false;
    if (need_fxaa_world) {
      record_fxaa(cmd, fxaa_set_scene_, scene_fxaa_.image, scene_fxaa_.view, rw, rh);
      cur_img = scene_fxaa_.image;
      cur_up_set = upscale_set_fxaa_;
      cur_is_fxaa = true;
    }
    if (inspector_active_ && inspector_dst != VK_NULL_HANDLE) {
      record_inspector_preview(cmd, inspector_dst);
    }
    const bool direct_to_final = !need_fxaa_all && !need_hdr_stage;
    if (direct_to_final) {
      const VkPipeline up_pipe =
          (final_format == color_format_ && upscale_pipe_swap_) ? upscale_pipe_swap_ : upscale_pipe_;
      record_upscale(cmd, cur_img, cur_up_set, cur_is_fxaa, rw, rh, final_image, final_view, target_width_,
                     target_height_, up_pipe, true);
      layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      last_stage = color_stage;
      last_access = color_write;
      if (capture_dst != VK_NULL_HANDLE && capture_before_ui) record_capture_copy();
      copy_overlays(final_image, layout, last_stage, last_access);
      record_ui(cmd, final_image, final_view, layout, last_stage, last_access, ui_pipeline_, target_width_,
                target_height_);
      if (capture_dst != VK_NULL_HANDLE && !capture_before_ui) record_capture_copy();
    } else {
      record_upscale(cmd, cur_img, cur_up_set, cur_is_fxaa, rw, rh, stand_in_.image, stand_in_.view, target_width_,
                     target_height_);

      VkImageLayout si_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkPipelineStageFlags2 si_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      VkAccessFlags2 si_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

      auto resolve_standin_to_final = [&]() {
        VkDescriptorSet hdr_set = hdr_set_standin_;
        VkImage src_img = stand_in_.image;
        if (need_fxaa_all) {
          if (si_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            image_barrier(cmd, stand_in_.image, VK_IMAGE_ASPECT_COLOR_BIT, si_layout,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, si_stage, si_access,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
          }
          record_fxaa(cmd, fxaa_set_standin_, stand_in_fxaa_.image, stand_in_fxaa_.view, target_width_, target_height_);
          hdr_set = hdr_set_fxaa_;
          src_img = stand_in_fxaa_.image;
          si_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          si_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
          si_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        }
        if (need_hdr_stage) {
          if (si_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            image_barrier(cmd, src_img, VK_IMAGE_ASPECT_COLOR_BIT, si_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          si_stage, si_access, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
          }
          image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        last_stage, last_access, color_stage, color_write);
          record_hdr_output(cmd, hdr_set, final_image, final_view, final_format, target_width_, target_height_);
          layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          last_stage = color_stage;
          last_access = color_write;
        } else {
          image_barrier(cmd, src_img, VK_IMAGE_ASPECT_COLOR_BIT, si_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        si_stage, si_access, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
          image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        last_stage, last_access, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
          VkImageBlit blit{};
          blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          blit.srcOffsets[1] = {int32_t(target_width_), int32_t(target_height_), 1};
          blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          blit.dstOffsets[1] = {int32_t(target_width_), int32_t(target_height_), 1};
          vkCmdBlitImage(cmd, src_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, final_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
          layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          last_stage = VK_PIPELINE_STAGE_2_BLIT_BIT;
          last_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
          si_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
          si_stage = VK_PIPELINE_STAGE_2_BLIT_BIT;
          si_access = VK_ACCESS_2_TRANSFER_READ_BIT;
        }
      };

      if (capture_dst != VK_NULL_HANDLE && capture_before_ui) {
        resolve_standin_to_final();
        record_capture_copy();
      }
      copy_overlays(stand_in_.image, si_layout, si_stage, si_access);
      record_ui(cmd, stand_in_.image, stand_in_.view, si_layout, si_stage, si_access, ui_pipeline_sdr_, target_width_,
                target_height_);
      if (!(capture_dst != VK_NULL_HANDLE && capture_before_ui)) {
        resolve_standin_to_final();
      }
      if (capture_dst != VK_NULL_HANDLE && !capture_before_ui) record_capture_copy();
    }
  }

  if (final_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
    if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
      image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, final_layout, last_stage, last_access,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
  } else {
    image_barrier(cmd, final_image, VK_IMAGE_ASPECT_COLOR_BIT, layout, final_layout, last_stage, last_access,
                  VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
  }
}

bool Renderer::uses_standin_target() const {
  if (!uses_modern_chain()) return false;
  const bool need_brightness = (display_brightness_offset_ > 1e-5f || display_brightness_offset_ < -1e-5f);
  const bool need_hdr_stage =
      (hdr_mode_ != modern::HdrMode::kOff) || graphics_.hdr_compare || (hdr_pattern_ != 0) || need_brightness;
  const bool need_fxaa_all = (graphics_.aa == modern::AaMode::kFxaaAll) && fxaa_pipe_ != VK_NULL_HANDLE;
  return need_fxaa_all || need_hdr_stage;
}

bool Renderer::uses_modern_chain() const {
  const bool post = options_.post.enabled && hdr_.view != VK_NULL_HANDLE;
  if (!post || !modern_ready_ || !scene_sdr_.view) return false;
  const uint32_t rw = render_width();
  const uint32_t rh = render_height();
  const bool need_scale = (rw != target_width_ || rh != target_height_);
  const bool need_upscaler = need_scale || graphics_.upscaler != modern::Upscaler::kBilinear;
  const bool need_brightness = (display_brightness_offset_ > 1e-5f || display_brightness_offset_ < -1e-5f);
  const bool need_hdr_stage =
      (hdr_mode_ != modern::HdrMode::kOff) || graphics_.hdr_compare || (hdr_pattern_ != 0) || need_brightness;
  const bool need_fxaa_world = (graphics_.aa == modern::AaMode::kFxaaWorld) && fxaa_pipe_ != VK_NULL_HANDLE;
  const bool need_fxaa_all = (graphics_.aa == modern::AaMode::kFxaaAll) && fxaa_pipe_ != VK_NULL_HANDLE;
  if ((need_hdr_stage || need_fxaa_all) && !stand_in_.view) return false;
  return need_upscaler || need_hdr_stage || need_fxaa_world || need_fxaa_all || inspector_active_;
}

}  // namespace carview
