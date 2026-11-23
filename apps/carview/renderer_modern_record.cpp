#include "renderer_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace carview {

void Renderer::record_ssao(VkCommandBuffer cmd) {
  if (!modern_ready_ || graphics_.ssao == modern::SsaoQuality::kOff || !ao_raw_.view || !ao_blur_.view) return;
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  const float frame_jitter = float(frames_submitted_ & 63u) / 64.0f;
  auto draw_ao = [&](const Image& dst, VkDescriptorSet set, float mode) {
    image_barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
    VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    col.imageView = dst.view;
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rend.renderArea = {{0, 0}, {ao_width_, ao_height_}};
    rend.layerCount = 1;
    rend.colorAttachmentCount = 1;
    rend.pColorAttachments = &col;
    vkCmdBeginRendering(cmd, &rend);
    VkViewport vp{0, 0, float(ao_width_), float(ao_height_), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {ao_width_, ao_height_}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssao_pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssao_layout_, 0, 1, &set, 0, nullptr);
    struct {
      float proj_params[4], ao_params[4], texel_step[4];
    } pc = {{cam_proj00_, cam_proj11_, cam_near_, cam_far_},
            {modern::ssao_radius_m(graphics_.ssao), float(modern::gtao_slices(graphics_.ssao)),
             float(modern::gtao_steps(graphics_.ssao)), mode},
            {1.0f / float(std::max(1u, ao_width_)), 1.0f / float(std::max(1u, ao_height_)),
             options_.reverse_z ? 1.0f : 0.0f, frame_jitter}};
    vkCmdPushConstants(cmd, ssao_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  };
  draw_ao(ao_raw_, ssao_sets_[0], 0.0f);
  draw_ao(ao_tmp_, ssao_sets_[1], 1.0f);
  draw_ao(ao_blur_, ssao_sets_[2], 2.0f);
  mark(cmd, "gtao");
}

void Renderer::record_sscs(VkCommandBuffer cmd) {
  if (!modern_ready_ || graphics_.contact_shadows == modern::ContactShadows::kOff || !sscs_pipe_ || !sscs_.view) return;
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  image_barrier(cmd, sscs_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
  VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  col.imageView = sscs_.view;
  col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rend.renderArea = {{0, 0}, {sscs_width_, sscs_height_}};
  rend.layerCount = 1;
  rend.colorAttachmentCount = 1;
  rend.pColorAttachments = &col;
  vkCmdBeginRendering(cmd, &rend);
  VkViewport vp{0, 0, float(sscs_width_), float(sscs_height_), 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {sscs_width_, sscs_height_}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sscs_pipe_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sscs_layout_, 0, 1, &sscs_set_, 0, nullptr);
  const PostSettings& s = options_.post;
  const float strength = graphics_.contact_shadows == modern::ContactShadows::kLow     ? 0.55f
                         : graphics_.contact_shadows == modern::ContactShadows::kHigh  ? 0.72f
                         : graphics_.contact_shadows == modern::ContactShadows::kUltra ? 0.80f
                                                                                       : 0.0f;
  struct {
    float proj_params[4], sun_view[4], sscs_params[4];
  } pc = {{cam_proj00_, cam_proj11_, cam_near_, cam_far_},
          {s.sun_view_dir[0], s.sun_view_dir[1], s.sun_view_dir[2], s.sun_elev_vis},
          {float(modern::contact_shadow_steps(graphics_.contact_shadows)), strength,
           options_.reverse_z ? 1.0f : 0.0f, float(frames_submitted_ & 63u) / 64.0f}};
  vkCmdPushConstants(cmd, sscs_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  image_barrier(cmd, sscs_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  mark(cmd, "sscs");
}

void Renderer::record_ssr(VkCommandBuffer cmd) {
  if (!modern_ready_ || graphics_.ssr == modern::SsrQuality::kOff || !ssr_pipe_ || !ssr_raw_.view || !ssr_blur_.view)
    return;
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  const float frame_jitter = float(frames_submitted_ & 63u) / 64.0f;
  const float strength = (graphics_.ssr == modern::SsrQuality::kFull) ? 0.90f : 0.80f;
  const float max_steps = float(modern::ssr_steps(graphics_.ssr));
  const float has_hiz = (hiz_preview_view_ != VK_NULL_HANDLE) ? 1.0f : 0.0f;
  auto draw_ssr = [&](const Image& dst, VkDescriptorSet set, float mode) {
    image_barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
    VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    col.imageView = dst.view;
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rend.renderArea = {{0, 0}, {ssr_width_, ssr_height_}};
    rend.layerCount = 1;
    rend.colorAttachmentCount = 1;
    rend.pColorAttachments = &col;
    vkCmdBeginRendering(cmd, &rend);
    VkViewport vp{0, 0, float(ssr_width_), float(ssr_height_), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {ssr_width_, ssr_height_}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssr_pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssr_layout_, 0, 1, &set, 0, nullptr);
    struct {
      float proj_params[4], ssr_params[4], texel_mode[4], hiz_params[4];
    } pc = {{cam_proj00_, cam_proj11_, cam_near_, cam_far_},
            {max_steps, options_.reverse_z ? 1.0f : 0.0f, has_hiz, frame_jitter},
            {1.0f / float(std::max(1u, ssr_width_)), 1.0f / float(std::max(1u, ssr_height_)), mode, strength},
            {512.0f, 256.0f, 9.0f, 0.0f}};
    vkCmdPushConstants(cmd, ssr_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  };
  draw_ssr(ssr_raw_, ssr_sets_[0], 0.0f);
  draw_ssr(ssr_blur_, ssr_sets_[1], 1.0f);
  mark(cmd, "ssr");
}

void Renderer::record_volumetric(VkCommandBuffer cmd) {
  if (!modern_ready_ || graphics_.volumetric_fog == modern::VolumetricFog::kOff || !vol_pipe_ || !vol_raw_.view ||
      !vol_blur_.view || !vol_ubo_.buffer)
    return;
  if (!vol_state_valid_) {
    vol_state_ = {};
  }
  vol_state_.height_p[3] = float(frames_submitted_ & 63u) / 64.0f;
  {
    VkBufferMemoryBarrier2 ub{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    ub.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    ub.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    ub.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    ub.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    ub.srcQueueFamilyIndex = ub.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ub.buffer = vol_ubo_.buffer;
    ub.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &ub;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkCmdUpdateBuffer(cmd, vol_ubo_.buffer, 0, sizeof(VolumetricState), &vol_state_);
    ub.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    ub.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    ub.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    ub.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  const float steps = float(modern::volumetric_steps(graphics_.volumetric_fog));
  auto draw_vol = [&](const Image& dst, VkDescriptorSet set, float mode) {
    image_barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
    VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    col.imageView = dst.view;
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rend.renderArea = {{0, 0}, {vol_width_, vol_height_}};
    rend.layerCount = 1;
    rend.colorAttachmentCount = 1;
    rend.pColorAttachments = &col;
    vkCmdBeginRendering(cmd, &rend);
    VkViewport vp{0, 0, float(vol_width_), float(vol_height_), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {vol_width_, vol_height_}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vol_pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vol_layout_, 0, 1, &set, 0, nullptr);
    struct {
      float proj_params[4], vol_params[4];
    } pc = {{cam_near_, cam_far_, options_.reverse_z ? 1.0f : 0.0f, 0.0f},
            {steps, 1.0f / float(std::max(1u, vol_width_)), mode, 1.0f / float(std::max(1u, vol_height_))}};
    vkCmdPushConstants(cmd, vol_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  };
  draw_vol(vol_raw_, vol_sets_[0], 0.0f);
  draw_vol(vol_blur_, vol_sets_[1], 1.0f);
  mark(cmd, "volumetric");
}

void Renderer::record_fxaa(VkCommandBuffer cmd, VkDescriptorSet set, VkImage dst_img, VkImageView dst_view, uint32_t w,
                           uint32_t h) {
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  image_barrier(cmd, dst_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
  VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  col.imageView = dst_view;
  col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rend.renderArea = {{0, 0}, {w, h}};
  rend.layerCount = 1;
  rend.colorAttachmentCount = 1;
  rend.pColorAttachments = &col;
  vkCmdBeginRendering(cmd, &rend);
  VkViewport vp{0, 0, float(w), float(h), 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {w, h}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaa_pipe_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaa_layout_, 0, 1, &set, 0, nullptr);
  const float pc[4] = {1.0f / float(std::max(1u, w)), 1.0f / float(std::max(1u, h)), 0.75f, 0.125f};
  vkCmdPushConstants(cmd, fxaa_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  image_barrier(cmd, dst_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  mark(cmd, "fxaa");
}

void Renderer::record_motion_vectors(VkCommandBuffer cmd) {
  if (!modern_ready_ || !motion_pipe_ || !motion_vec_.view) return;
  const uint32_t rw = render_width(), rh = render_height();
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

  image_barrier(cmd, motion_vec_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
  VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  col.imageView = motion_vec_.view;
  col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rend.renderArea = {{0, 0}, {rw, rh}};
  rend.layerCount = 1;
  rend.colorAttachmentCount = 1;
  rend.pColorAttachments = &col;
  vkCmdBeginRendering(cmd, &rend);
  VkViewport vp{0, 0, float(rw), float(rh), 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {rw, rh}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, motion_pipe_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, motion_layout_, 0, 1, &motion_set_, 0, nullptr);

  struct {
    float prev_vp_inv_curr[16];
    float params[4];
  } pc{};
  for (int i = 0; i < 4; ++i) pc.prev_vp_inv_curr[i * 5] = 1.0f;
  float curr_inv[16];
  if (cam_vp_set_ && prev_cam_vp_valid_ && mat4_inv(cam_vp_, curr_inv)) {
    mat4_mul(prev_cam_vp_, curr_inv, pc.prev_vp_inv_curr);
  }
  pc.params[0] = cam_jitter_uv_x_ - prev_jitter_uv_x_;
  pc.params[1] = cam_jitter_uv_y_ - prev_jitter_uv_y_;
  pc.params[2] = options_.reverse_z ? 1.0f : 0.0f;
  pc.params[3] = cam_near_;
  vkCmdPushConstants(cmd, motion_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  image_barrier(cmd, motion_vec_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  if (cam_vp_set_) {
    std::memcpy(prev_cam_vp_, cam_vp_, sizeof(cam_vp_));
    prev_jitter_uv_x_ = cam_jitter_uv_x_;
    prev_jitter_uv_y_ = cam_jitter_uv_y_;
    prev_cam_vp_valid_ = true;
  }
  mark(cmd, "motion");
}

void Renderer::record_mblur_tiles(VkCommandBuffer cmd) {
  if (!modern_ready_ || !mblur_tile_pipe_ || !mblur_tile_.view || !mblur_neighbor_.view || !motion_vec_.view) return;
  const uint32_t rw = render_width(), rh = render_height();
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

  auto draw_tile = [&](const Image& dst, VkDescriptorSet set, float inv_w, float inv_h, float mode) {
    image_barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
    VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    col.imageView = dst.view;
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rend.renderArea = {{0, 0}, {mblur_tile_w_, mblur_tile_h_}};
    rend.layerCount = 1;
    rend.colorAttachmentCount = 1;
    rend.pColorAttachments = &col;
    vkCmdBeginRendering(cmd, &rend);
    VkViewport vp{0, 0, float(mblur_tile_w_), float(mblur_tile_h_), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {mblur_tile_w_, mblur_tile_h_}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mblur_tile_pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mblur_tile_layout_, 0, 1, &set, 0, nullptr);
    const float pc[4] = {inv_w, inv_h, mode, options_.post.motion_blur_car_protect};
    vkCmdPushConstants(cmd, mblur_tile_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  };
  draw_tile(mblur_tile_, mblur_tile_set_, 1.0f / float(std::max(1u, rw)), 1.0f / float(std::max(1u, rh)), 0.0f);
  draw_tile(mblur_neighbor_, mblur_neighbor_set_, 1.0f / float(std::max(1u, mblur_tile_w_)),
            1.0f / float(std::max(1u, mblur_tile_h_)), 1.0f);
  mark(cmd, "mblur_tiles");
}

void Renderer::record_inspector_preview(VkCommandBuffer cmd, VkBuffer readback_dst) {
  if (!modern_ready_ || !inspector_pipe_ || !inspector_thumb_.view || readback_dst == VK_NULL_HANDLE) return;
  const int idx = std::clamp(int(inspector_view_), 0, int(InspectorView::kCount) - 1);
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  image_barrier(cmd, inspector_thumb_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
  VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  col.imageView = inspector_thumb_.view;
  col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rend.renderArea = {{0, 0}, {kInspectorPreviewW, kInspectorPreviewH}};
  rend.layerCount = 1;
  rend.colorAttachmentCount = 1;
  rend.pColorAttachments = &col;
  vkCmdBeginRendering(cmd, &rend);
  VkViewport vp{0, 0, float(kInspectorPreviewW), float(kInspectorPreviewH), 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {kInspectorPreviewW, kInspectorPreviewH}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inspector_pipe_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inspector_layout_, 0, 1, &inspector_sets_[idx], 0,
                          nullptr);
  const float mode = (idx == 5 && !hiz_preview_view_) ? 1.0f : float(idx);
  const float params[4] = {mode, cam_near_, options_.reverse_z ? 1.0f : 0.0f, cam_far_};
  vkCmdPushConstants(cmd, inspector_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), params);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);

  image_barrier(cmd, inspector_thumb_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color_stage, color_write, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {kInspectorPreviewW, kInspectorPreviewH, 1};
  vkCmdCopyImageToBuffer(cmd, inspector_thumb_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_dst, 1, &copy);
  VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &host;
  vkCmdPipelineBarrier2(cmd, &dep);
  mark(cmd, "inspect");
}

void Renderer::record_upscale(VkCommandBuffer cmd, VkImage /*src_img*/, VkDescriptorSet src_set, bool src_is_fxaa,
                              uint32_t src_w, uint32_t src_h, VkImage dst_img, VkImageView dst_view, uint32_t dst_w,
                              uint32_t dst_h) {
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  const float sharp = graphics_.sharpening / 100.0f;
  auto draw_up = [&](VkDescriptorSet set, VkImage target_img, VkImageView target_view, uint32_t sw, uint32_t sh,
                     uint32_t dw, uint32_t dh, float mode) {
    image_barrier(cmd, target_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, VK_ACCESS_2_NONE, color_stage, color_write);
    VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    col.imageView = target_view;
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rend.renderArea = {{0, 0}, {dw, dh}};
    rend.layerCount = 1;
    rend.colorAttachmentCount = 1;
    rend.pColorAttachments = &col;
    vkCmdBeginRendering(cmd, &rend);
    VkViewport vp{0, 0, float(dw), float(dh), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {dw, dh}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, upscale_pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, upscale_layout_, 0, 1, &set, 0, nullptr);
    struct {
      float src_size[4], dst_size[4], params[4];
    } pc = {{float(sw), float(sh), 1.0f / float(std::max(1u, sw)), 1.0f / float(std::max(1u, sh))},
            {float(dw), float(dh), 1.0f / float(std::max(1u, dw)), 1.0f / float(std::max(1u, dh))},
            {mode, sharp, 0.0f, 0.0f}};
    vkCmdPushConstants(cmd, upscale_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, target_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  };
  const auto choice = active_upscaler();
  if (choice.temporal && temporal_pipe_ && temporal_hist_[0].view && temporal_hist_[1].view) {
    if (temporal_reset_) {
      const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
      const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      for (int i = 0; i < 2; ++i) {
        image_barrier(cmd, temporal_hist_[i].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, read_stage, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(cmd, temporal_hist_[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);
        image_barrier(cmd, temporal_hist_[i].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, read_stage, read_acc);
      }
    }
    const uint32_t out_idx = temporal_idx_ & 1u;
    const Image& hist_out = temporal_hist_[out_idx];
    image_barrier(cmd, hist_out.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read_stage, read_acc, color_stage, color_write);
    VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    col.imageView = hist_out.view;
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rend.renderArea = {{0, 0}, {dst_w, dst_h}};
    rend.layerCount = 1;
    rend.colorAttachmentCount = 1;
    rend.pColorAttachments = &col;
    vkCmdBeginRendering(cmd, &rend);
    VkViewport vp{0, 0, float(dst_w), float(dst_h), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {dst_w, dst_h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, temporal_pipe_);
    const uint32_t tset_idx = (src_is_fxaa ? 2u : 0u) + out_idx;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, temporal_layout_, 0, 1, &temporal_sets_[tset_idx], 0,
                            nullptr);
    struct {
      float render_size[4], output_size[4], params[4];
    } tpc = {{float(src_w), float(src_h), 1.0f / float(std::max(1u, src_w)), 1.0f / float(std::max(1u, src_h))},
             {float(dst_w), float(dst_h), 1.0f / float(std::max(1u, dst_w)), 1.0f / float(std::max(1u, dst_h))},
             {cam_jitter_uv_x_, cam_jitter_uv_y_, temporal_reset_ ? 1.0f : 0.0f, 0.90f}};
    vkCmdPushConstants(cmd, temporal_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(tpc), &tpc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, hist_out.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
    draw_up(upscale_set_hist_[out_idx], dst_img, dst_view, dst_w, dst_h, dst_w, dst_h, sharp > 0.0f ? 3.0f : 0.0f);
    temporal_reset_ = false;
    temporal_idx_ ^= 1u;
  } else if (choice.active == modern::Upscaler::kFsr1) {
    draw_up(src_set, upscale_tmp_.image, upscale_tmp_.view, src_w, src_h, dst_w, dst_h, 2.0f);
    draw_up(upscale_set_tmp_, dst_img, dst_view, dst_w, dst_h, dst_w, dst_h, 3.0f);
  } else if (choice.active == modern::Upscaler::kCas) {
    draw_up(src_set, dst_img, dst_view, src_w, src_h, dst_w, dst_h, 1.0f);
  } else {
    draw_up(src_set, dst_img, dst_view, src_w, src_h, dst_w, dst_h, 0.0f);
  }
  mark(cmd, "upscale");
}

void Renderer::record_hdr_output(VkCommandBuffer cmd, VkDescriptorSet src_set, VkImage dst_img, VkImageView dst_view,
                                 VkFormat dst_format, uint32_t w, uint32_t h) {
  VkPipeline pipe = (dst_format == color_format_) ? hdr_pipe_swap_ : hdr_pipe_sdr_;
  if (!pipe) pipe = hdr_pipe_sdr_;
  VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  col.imageView = dst_view;
  col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rend.renderArea = {{0, 0}, {w, h}};
  rend.layerCount = 1;
  rend.colorAttachmentCount = 1;
  rend.pColorAttachments = &col;
  vkCmdBeginRendering(cmd, &rend);
  VkViewport vp{0, 0, float(w), float(h), 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {w, h}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hdr_layout_, 0, 1, &src_set, 0, nullptr);
  const float paper_scale = modern::scrgb_scale(graphics_.paper_white);
  const float hl_factor = (graphics_.hdr || graphics_.hdr_compare)
                              ? std::max(graphics_.peak_nits / std::max(graphics_.paper_white, 80.0f) - 1.0f, 0.0f) *
                                    (graphics_.hdr_highlights / 100.0f)
                              : 0.0f;
  const uint32_t out_mode = (dst_format == color_format_ && hdr_mode_ == modern::HdrMode::kHdr10) ? 2u
                            : (dst_format == color_format_ && hdr_mode_ == modern::HdrMode::kScRgb) ? 1u
                                                                                                    : 0u;
  struct {
    float p0[4], p1[4];
    uint32_t p2[4];
  } pc = {{paper_scale, hl_factor, 0.5f, 2.0f},
          {0.0f, 0.5f, graphics_.hdr_compare ? float(w) * 0.5f : -1.0f, modern::scrgb_scale(hdr_pattern_nits_)},
          {out_mode, hdr_pattern_, w, h}};
  vkCmdPushConstants(cmd, hdr_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  (void)dst_img;
  mark(cmd, "hdr");
}


}  // namespace carview
