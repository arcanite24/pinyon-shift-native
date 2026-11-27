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
    const float ao_slices =
#ifdef __ANDROID__
        float(std::min(modern::gtao_slices(graphics_.ssao), 2));
#else
        float(modern::gtao_slices(graphics_.ssao));
#endif
    const float ao_steps =
#ifdef __ANDROID__
        float(std::min(modern::gtao_steps(graphics_.ssao), 3));
#else
        float(modern::gtao_steps(graphics_.ssao));
#endif
    struct {
      float proj_params[4], ao_params[4], texel_step[4];
    } pc = {{cam_proj00_, cam_proj11_, cam_near_, cam_far_},
            {modern::ssao_radius_m(graphics_.ssao), ao_slices, ao_steps, mode},
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
  const float max_steps =
#ifdef __ANDROID__
      float(std::min(modern::ssr_steps(graphics_.ssr), graphics_.ssr == modern::SsrQuality::kFull ? 20 : 14));
#else
      float(modern::ssr_steps(graphics_.ssr));
#endif
  const bool hiz_ready =
      (hiz_preview_view_ != VK_NULL_HANDLE) && (hiz_width_ > 0) && (hiz_height_ > 0) && (hiz_mips_ > 0);
  const float has_hiz = hiz_ready ? 1.0f : 0.0f;
  const float hiz_w = hiz_ready ? float(hiz_width_) : float(std::max(1u, render_width()));
  const float hiz_h = hiz_ready ? float(hiz_height_) : float(std::max(1u, render_height()));
  const float hiz_max_mip = hiz_ready ? float(hiz_mips_ - 1u) : 0.0f;
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
            {hiz_w, hiz_h, hiz_max_mip, 0.0f}};
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
  const uint32_t vol_dyn_off = uint32_t(slot_ * kVolUboStride);
  if (vol_ubo_mapped_) {
    std::memcpy(static_cast<uint8_t*>(vol_ubo_mapped_) + vol_dyn_off, &vol_state_, sizeof(VolumetricState));
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
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vol_layout_, 0, 1, &set, 1, &vol_dyn_off);
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

  const float jitter_dx = cam_jitter_uv_x_ - prev_jitter_uv_x_;
  const float jitter_dy = cam_jitter_uv_y_ - prev_jitter_uv_y_;
  const float safe_near = std::max(cam_near_, 0.05f);
  if (!prev_cam_vp_valid_) {
    prev_motion_scene_valid_ = false;
  }

  float curr_inv[16];
  const bool have_curr_inv = cam_vp_set_ && motion_math::mat4_inv(cam_vp_, curr_inv);
  const bool have_prev = have_curr_inv && prev_cam_vp_valid_;
  const bool have_world =
      motion_scene_.has_world_transform && prev_motion_scene_valid_ && prev_motion_scene_.has_world_transform;

  // Detect camera/car teleports (Fast Travel, Reset to Road, Hub room transitions)
  // so single-frame jumps emit zero velocity instead of a full-screen motion smear.
  bool scene_cut = false;
  float inv_curr_world[16], inv_prev_world[16];
  bool have_world_inv = false;
  if (have_world && motion_math::mat4_inv(motion_scene_.world_transform, inv_curr_world) &&
      motion_math::mat4_inv(prev_motion_scene_.world_transform, inv_prev_world)) {
    have_world_inv = true;
    const double curr_wx = double(inv_curr_world[12]) + motion_scene_.world_origin[0];
    const double curr_wy = double(inv_curr_world[13]);
    const double curr_wz = double(inv_curr_world[14]) + motion_scene_.world_origin[1];
    const double prev_wx = double(inv_prev_world[12]) + prev_motion_scene_.world_origin[0];
    const double prev_wy = double(inv_prev_world[13]);
    const double prev_wz = double(inv_prev_world[14]) + prev_motion_scene_.world_origin[1];
    const double dx = curr_wx - prev_wx, dy = curr_wy - prev_wy, dz = curr_wz - prev_wz;
    if (dx * dx + dy * dy + dz * dz > 40.0 * 40.0) scene_cut = true;
  }

  // Pass 0: Fullscreen static-world reprojection (or camera-only fallback when no world is active).
  struct {
    float prev_vp_inv_curr[16];
    float params[4];
  } pc{};
  motion_math::mat4_identity(pc.prev_vp_inv_curr);
  if (have_prev && !scene_cut) {
    if (have_world) {
      if (!motion_math::build_static_world_reproj(
              prev_cam_vp_, curr_inv, prev_motion_scene_.world_transform, motion_scene_.world_transform,
              prev_motion_scene_.world_origin[0], prev_motion_scene_.world_origin[1], motion_scene_.world_origin[0],
              motion_scene_.world_origin[1], pc.prev_vp_inv_curr)) {
        motion_math::mat4_mul(prev_cam_vp_, curr_inv, pc.prev_vp_inv_curr);
      }
    } else {
      motion_math::mat4_mul(prev_cam_vp_, curr_inv, pc.prev_vp_inv_curr);
    }
  }
  const bool need_reactive = active_upscaler().temporal || (active_rt_mode() != rt::RtMode::kOff);
  pc.params[0] = jitter_dx;
  pc.params[1] = jitter_dy;
  pc.params[2] = (options_.reverse_z ? 1.0f : 0.0f) + (need_reactive ? 0.0f : 2.0f);
  pc.params[3] = safe_near;
  vkCmdPushConstants(cmd, motion_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
  vkCmdDraw(cmd, 3, 1, 0, 0);

  // Pass 1..K: Exact per-object dynamic vehicle OBB passes (player car + up to 8 nearest AI/traffic cars).
  // Rendered inside the same dynamic rendering pass with tight per-vehicle screen-space scissors.
  const bool can_dynamic = have_prev && !scene_cut && std::fabs(cam_proj00_) > 1e-4f && std::fabs(cam_proj11_) > 1e-4f &&
                           (motion_scene_.has_player_car || motion_scene_.dynamic_car_count > 0);
  if (can_dynamic) {
    float view_mat[16];
    motion_math::extract_view_from_vp(cam_vp_, cam_proj00_, cam_proj11_, view_mat);
    const float inv_p00 = 1.0f / cam_proj00_;
    const float inv_p11 = 1.0f / cam_proj11_;

    struct DynJob {
      float depth_m = 0.0f;
      bool is_player = false;
      float curr_model[16];
      float prev_model[16];
      float bmin[3];
      float bmax[3];
      VkRect2D scissor{};
    };
    DynJob jobs[MotionSceneState::kMaxDynamicCars + 1]{};
    uint32_t job_count = 0;

    auto compute_scissor = [&](const float model[16], const float bmin[3], const float bmax[3],
                               VkRect2D& out_sc) -> bool {
      float clip_from_loc[16];
      motion_math::mat4_mul(cam_vp_, model, clip_from_loc);
      float u0 = 1.0f, v0 = 1.0f, u1 = 0.0f, v1 = 0.0f;
      bool any_in_front = false;
      bool any_near = false;
      const float w_eps = safe_near * 0.25f;
      for (int c = 0; c < 8; ++c) {
        const float x = (c & 1) ? bmax[0] : bmin[0];
        const float y = (c & 2) ? bmax[1] : bmin[1];
        const float z = (c & 4) ? bmax[2] : bmin[2];
        const float cx = clip_from_loc[0] * x + clip_from_loc[4] * y + clip_from_loc[8] * z + clip_from_loc[12];
        const float cy = clip_from_loc[1] * x + clip_from_loc[5] * y + clip_from_loc[9] * z + clip_from_loc[13];
        const float cw = clip_from_loc[3] * x + clip_from_loc[7] * y + clip_from_loc[11] * z + clip_from_loc[15];
        if (cw <= w_eps) {
          any_near = true;
        } else {
          any_in_front = true;
          const float u = (cx / cw) * 0.5f + 0.5f;
          const float v = (cy / cw) * 0.5f + 0.5f;
          u0 = std::min(u0, u);
          v0 = std::min(v0, v);
          u1 = std::max(u1, u);
          v1 = std::max(v1, v);
        }
      }
      if (!any_in_front) return false;
      if (any_near) {
        out_sc = sc;
        return true;
      }
      if (u1 <= 0.0f || v1 <= 0.0f || u0 >= 1.0f || v0 >= 1.0f) return false;
      const int32_t x0 = std::clamp(int32_t(std::floor(u0 * float(rw))) - 4, 0, int32_t(rw));
      const int32_t y0 = std::clamp(int32_t(std::floor(v0 * float(rh))) - 4, 0, int32_t(rh));
      const int32_t x1 = std::clamp(int32_t(std::ceil(u1 * float(rw))) + 4, 0, int32_t(rw));
      const int32_t y1 = std::clamp(int32_t(std::ceil(v1 * float(rh))) + 4, 0, int32_t(rh));
      if (x1 <= x0 || y1 <= y0) return false;
      out_sc = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
      return true;
    };

    if (motion_scene_.has_player_car) {
      const float hx = 0.5f * (motion_scene_.player_bmax[0] - motion_scene_.player_bmin[0]);
      const float hy = 0.5f * (motion_scene_.player_bmax[1] - motion_scene_.player_bmin[1]);
      const float hz = 0.5f * (motion_scene_.player_bmax[2] - motion_scene_.player_bmin[2]);
      if (hx > 0.1f && hy > 0.1f && hz > 0.1f) {
        float id_m[16];
        motion_math::mat4_identity(id_m);
        VkRect2D psc{};
        if (compute_scissor(id_m, motion_scene_.player_bmin, motion_scene_.player_bmax, psc)) {
          DynJob& j = jobs[job_count++];
          j.is_player = true;
          std::memcpy(j.curr_model, id_m, sizeof(id_m));
          std::memcpy(j.prev_model, id_m, sizeof(id_m));
          for (int k = 0; k < 3; ++k) {
            j.bmin[k] = motion_scene_.player_bmin[k];
            j.bmax[k] = motion_scene_.player_bmax[k];
          }
          const float cx = 0.5f * (j.bmin[0] + j.bmax[0]);
          const float cy = 0.5f * (j.bmin[1] + j.bmax[1]);
          const float cz = 0.5f * (j.bmin[2] + j.bmax[2]);
          j.depth_m = -(view_mat[2] * cx + view_mat[6] * cy + view_mat[10] * cz + view_mat[14]);
          j.scissor = psc;
        }
      }
    }

    // Match each current AI/traffic car to its exact previous-frame pose in world space
    // (invariant to player motion and streaming origin re-centering).
    if (prev_motion_scene_valid_ && motion_scene_.dynamic_car_count > 0 && prev_motion_scene_.dynamic_car_count > 0) {
      const uint32_t curr_n = std::min(motion_scene_.dynamic_car_count, MotionSceneState::kMaxDynamicCars);
      const uint32_t prev_n = std::min(prev_motion_scene_.dynamic_car_count, MotionSceneState::kMaxDynamicCars);
      double prev_wpos[MotionSceneState::kMaxDynamicCars][3]{};
      float prev_wfwd[MotionSceneState::kMaxDynamicCars][3]{};
      bool prev_used[MotionSceneState::kMaxDynamicCars]{};
      auto to_match_frame = [&](const float car_model[16], const float inv_w[16], double ox, double oz,
                                double out_p[3], float out_fwd[3]) {
        const float px = car_model[12], py = car_model[13], pz = car_model[14];
        const float fx = car_model[8], fy = car_model[9], fz = car_model[10];
        if (have_world_inv) {
          out_p[0] = double(inv_w[0] * px + inv_w[4] * py + inv_w[8] * pz + inv_w[12]) + ox;
          out_p[1] = double(inv_w[1] * px + inv_w[5] * py + inv_w[9] * pz + inv_w[13]);
          out_p[2] = double(inv_w[2] * px + inv_w[6] * py + inv_w[10] * pz + inv_w[14]) + oz;
          out_fwd[0] = inv_w[0] * fx + inv_w[4] * fy + inv_w[8] * fz;
          out_fwd[1] = inv_w[1] * fx + inv_w[5] * fy + inv_w[9] * fz;
          out_fwd[2] = inv_w[2] * fx + inv_w[6] * fy + inv_w[10] * fz;
        } else {
          out_p[0] = double(px);
          out_p[1] = double(py);
          out_p[2] = double(pz);
          out_fwd[0] = fx;
          out_fwd[1] = fy;
          out_fwd[2] = fz;
        }
      };
      for (uint32_t pi = 0; pi < prev_n; ++pi) {
        to_match_frame(prev_motion_scene_.dynamic_cars[pi].model, inv_prev_world, prev_motion_scene_.world_origin[0],
                       prev_motion_scene_.world_origin[1], prev_wpos[pi], prev_wfwd[pi]);
      }
      for (uint32_t ci = 0; ci < curr_n; ++ci) {
        const auto& cc = motion_scene_.dynamic_cars[ci];
        double cwpos[3]{};
        float cwfwd[3]{};
        to_match_frame(cc.model, inv_curr_world, motion_scene_.world_origin[0], motion_scene_.world_origin[1], cwpos,
                       cwfwd);
        int best_pi = -1;
        double best_d2 = 12.0 * 12.0;  // reject newly spawned or teleported cars > 12 m/frame
        for (uint32_t pi = 0; pi < prev_n; ++pi) {
          if (prev_used[pi]) continue;
          if (prev_motion_scene_.dynamic_cars[pi].model_id != cc.model_id) continue;
          const float fdot = cwfwd[0] * prev_wfwd[pi][0] + cwfwd[1] * prev_wfwd[pi][1] + cwfwd[2] * prev_wfwd[pi][2];
          if (fdot < 0.6f) continue;
          const double dx = cwpos[0] - prev_wpos[pi][0];
          const double dy = cwpos[1] - prev_wpos[pi][1];
          const double dz = cwpos[2] - prev_wpos[pi][2];
          const double d2 = dx * dx + dy * dy + dz * dz;
          if (d2 < best_d2) {
            best_d2 = d2;
            best_pi = int(pi);
          }
        }
        if (best_pi < 0) continue;
        prev_used[best_pi] = true;
        VkRect2D csc{};
        if (!compute_scissor(cc.model, cc.bmin, cc.bmax, csc)) continue;
        DynJob& j = jobs[job_count++];
        j.is_player = false;
        std::memcpy(j.curr_model, cc.model, sizeof(j.curr_model));
        std::memcpy(j.prev_model, prev_motion_scene_.dynamic_cars[best_pi].model, sizeof(j.prev_model));
        for (int k = 0; k < 3; ++k) {
          j.bmin[k] = cc.bmin[k];
          j.bmax[k] = cc.bmax[k];
        }
        float mv[16];
        motion_math::mat4_mul(view_mat, cc.model, mv);
        const float cx = 0.5f * (j.bmin[0] + j.bmax[0]);
        const float cy = 0.5f * (j.bmin[1] + j.bmax[1]);
        const float cz = 0.5f * (j.bmin[2] + j.bmax[2]);
        j.depth_m = -(mv[2] * cx + mv[6] * cy + mv[10] * cz + mv[14]);
        j.scissor = csc;
      }
    }

    // Draw farthest-to-nearest so closer vehicles win if two OBBs touch bumper-to-bumper.
    std::sort(jobs, jobs + job_count, [](const DynJob& a, const DynJob& b) { return a.depth_m > b.depth_m; });
    for (uint32_t ji = 0; ji < job_count; ++ji) {
      const DynJob& j = jobs[ji];
      float reproj[16];
      if (j.is_player) {
        motion_math::mat4_mul(prev_cam_vp_, curr_inv, reproj);
      } else if (!motion_math::build_object_reproj(prev_cam_vp_, curr_inv, j.prev_model, j.curr_model, reproj)) {
        continue;
      }
      float mv[16];
      motion_math::mat4_mul(view_mat, j.curr_model, mv);
      const float c_loc[3] = {0.5f * (j.bmin[0] + j.bmax[0]), 0.5f * (j.bmin[1] + j.bmax[1]),
                              0.5f * (j.bmin[2] + j.bmax[2])};
      const float h_loc[3] = {std::max(0.5f * (j.bmax[0] - j.bmin[0]), 0.05f),
                              std::max(0.5f * (j.bmax[1] - j.bmin[1]), 0.05f),
                              std::max(0.5f * (j.bmax[2] - j.bmin[2]), 0.05f)};
      const float center_view[3] = {mv[0] * c_loc[0] + mv[4] * c_loc[1] + mv[8] * c_loc[2] + mv[12],
                                    mv[1] * c_loc[0] + mv[5] * c_loc[1] + mv[9] * c_loc[2] + mv[13],
                                    mv[2] * c_loc[0] + mv[6] * c_loc[1] + mv[10] * c_loc[2] + mv[14]};
      float q[4];
      motion_math::mat3_to_quat_pos_w(mv, q);
      uint32_t dyn_words[20]{};
      motion_math::pack_dynamic_push(reproj, jitter_dx, jitter_dy, center_view, q, h_loc, inv_p00, inv_p11, safe_near,
                                     options_.reverse_z, j.is_player, dyn_words);
      if (!need_reactive) {
        float w_enc = 0.0f;
        std::memcpy(&w_enc, &dyn_words[19], sizeof(w_enc));
        w_enc -= 4000.0f;
        std::memcpy(&dyn_words[19], &w_enc, sizeof(w_enc));
      }
      vkCmdSetScissor(cmd, 0, 1, &j.scissor);
      vkCmdPushConstants(cmd, motion_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dyn_words), dyn_words);
      vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    if (job_count > 0) vkCmdSetScissor(cmd, 0, 1, &sc);
  }
  vkCmdEndRendering(cmd);
  image_barrier(cmd, motion_vec_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
  if (cam_vp_set_) {
    std::memcpy(prev_cam_vp_, cam_vp_, sizeof(cam_vp_));
    prev_jitter_uv_x_ = cam_jitter_uv_x_;
    prev_jitter_uv_y_ = cam_jitter_uv_y_;
    prev_cam_vp_valid_ = true;
    prev_motion_scene_ = motion_scene_;
    prev_motion_scene_valid_ = true;
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
    const bool has_dynamic_mv = motion_scene_.has_player_car || motion_scene_.has_world_transform;
    const float car_protect = has_dynamic_mv ? 0.0f : options_.post.motion_blur_car_protect;
    const float pc[4] = {inv_w, inv_h, mode, car_protect};
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
                              uint32_t dst_h, VkPipeline final_pipe, bool final_is_color_attachment) {
  const auto color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  const auto color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  const auto read_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  const auto read_acc = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  const float sharp = graphics_.sharpening / 100.0f;
  const VkPipeline out_pipe = final_pipe ? final_pipe : upscale_pipe_;
  auto draw_up = [&](VkDescriptorSet set, VkImage target_img, VkImageView target_view, uint32_t sw, uint32_t sh,
                     uint32_t dw, uint32_t dh, float mode, VkPipeline pipe, bool leave_color_att) {
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, upscale_layout_, 0, 1, &set, 0, nullptr);
    struct {
      float src_size[4], dst_size[4], params[4];
    } pc = {{float(sw), float(sh), 1.0f / float(std::max(1u, sw)), 1.0f / float(std::max(1u, sh))},
            {float(dw), float(dh), 1.0f / float(std::max(1u, dw)), 1.0f / float(std::max(1u, dh))},
            {mode, sharp, 0.0f, leave_color_att ? display_brightness_offset_ : 0.0f}};
    vkCmdPushConstants(cmd, upscale_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (!leave_color_att) {
      image_barrier(cmd, target_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_stage, color_write, read_stage, read_acc);
    }
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
    draw_up(upscale_set_hist_[out_idx], dst_img, dst_view, dst_w, dst_h, dst_w, dst_h, sharp > 0.0f ? 3.0f : 0.0f,
            out_pipe, final_is_color_attachment);
    temporal_reset_ = false;
    temporal_idx_ ^= 1u;
  } else if (choice.active == modern::Upscaler::kFsr1) {
    draw_up(src_set, upscale_tmp_.image, upscale_tmp_.view, src_w, src_h, dst_w, dst_h, 2.0f, upscale_pipe_, false);
    draw_up(upscale_set_tmp_, dst_img, dst_view, dst_w, dst_h, dst_w, dst_h, 3.0f, out_pipe, final_is_color_attachment);
  } else if (choice.active == modern::Upscaler::kCas) {
    draw_up(src_set, dst_img, dst_view, src_w, src_h, dst_w, dst_h, 1.0f, out_pipe, final_is_color_attachment);
  } else {
    draw_up(src_set, dst_img, dst_view, src_w, src_h, dst_w, dst_h, 0.0f, out_pipe, final_is_color_attachment);
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
    float p3[4];
  } pc = {{paper_scale, hl_factor, 0.5f, 2.0f},
          {0.0f, 0.5f, graphics_.hdr_compare ? float(w) * 0.5f : -1.0f, modern::scrgb_scale(hdr_pattern_nits_)},
          {out_mode, hdr_pattern_, w, h},
          {display_brightness_offset_, 0.0f, 0.0f, 0.0f}};
  vkCmdPushConstants(cmd, hdr_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  (void)dst_img;
  mark(cmd, "hdr");
}


}  // namespace carview
