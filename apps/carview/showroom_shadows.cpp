#include "showroom_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace carview {

bool Showroom::shadows_on() const {
  return shadow_set_ && shadow_world_ && r_.shadow_size_ > 1 && world_textured_ && !world_draws_.empty();
}

void Showroom::place_shadow() {
  const ShadowSettings& s = r_.options_.shadow;
  const auto pcss_q = r_.graphics_settings().shadow_pcss;
  const bool three_cascades = (r_.shadow_cascades_ >= 3u) && (shadow_set_mid_ != VK_NULL_HANDLE) &&
                              (shadow_set_far_ != VK_NULL_HANDLE);
  // Phase B 3-cascade stabilized D32 CSM radii (Near ~20m, Mid ~90m, Far ~320m) on high/ultra;
  // falls back to fast 2-cascade (Near ~30m, Far ~185m) on low/low-power.
  const float near_radius = three_cascades ? 20.0f : std::min(s.radius, 30.0f);
  const float mid_radius = 90.0f;
  const float far_radius = three_cascades ? 320.0f : std::max(s.radius * 3.0f, 185.0f);
  constexpr float kDepth = 520.0f;
  auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  auto normalise = [&](float* v) {
    const float l = std::sqrt(dot3(v, v));
    if (l < 1e-6f) return false;
    for (int k = 0; k < 3; ++k) v[k] /= l;
    return true;
  };
  auto cross = [](const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
  };
  const float* from = shadow_direction_set_ ? shadow_direction_ : world_sun_;
  float sun[3] = {from[0], from[1], from[2]};
  float up[3] = {world_up_[0], world_up_[1], world_up_[2]};
  if (!normalise(sun)) sun[0] = 0, sun[1] = 1, sun[2] = 0;
  if (!normalise(up)) up[0] = 0, up[1] = 1, up[2] = 0;
  const float* vp = frame_.view_projection;
  float ahead[3] = {vp[3], vp[7], vp[11]};
  const float along_up = dot3(ahead, up);
  for (int k = 0; k < 3; ++k) ahead[k] -= along_up * up[k];
  if (!normalise(ahead)) ahead[0] = 0, ahead[1] = 0, ahead[2] = 1;
  const float forward[3] = {-sun[0], -sun[1], -sun[2]};
  float right[3], across[3];
  cross(forward, up, right);
  if (!normalise(right)) {
    const float x[3] = {1, 0, 0};
    cross(forward, x, right);
    normalise(right);
  }
  cross(right, forward, across);
  // Anchor texel grid to absolute world origin (folding scenery origin shift) so camera
  // movement AND cell regathers produce zero shadow-edge shimmer!
  const float world_zero[3] = {
      world_transform_[12] - world_transform_[0] * float(world_origin_[0]) - world_transform_[8] * float(world_origin_[1]),
      world_transform_[13] - world_transform_[1] * float(world_origin_[0]) - world_transform_[9] * float(world_origin_[1]),
      world_transform_[14] - world_transform_[2] * float(world_origin_[0]) - world_transform_[10] * float(world_origin_[1]),
  };
  const float ox = dot3(world_zero, right), oy = dot3(world_zero, across);

  auto build_cascade = [&](float radius, float ahead_bias, float m[16], float& texel_out, float centre_out[3]) {
    for (int k = 0; k < 3; ++k) centre_out[k] = frame_.eye[k] + ahead[k] * radius * ahead_bias;
    const float texel = 2.0f * radius / float(r_.shadow_size_);
    texel_out = texel;
    float cx = dot3(centre_out, right), cy = dot3(centre_out, across);
    cx = ox + std::round((cx - ox) / texel) * texel;
    cy = oy + std::round((cy - oy) / texel) * texel;
    const float cz = dot3(centre_out, sun);
    std::memset(m, 0, 16 * sizeof(float));
    for (int k = 0; k < 3; ++k) {
      m[k * 4 + 0] = right[k] / radius;
      m[k * 4 + 1] = across[k] / radius;
      m[k * 4 + 2] = -sun[k] / (2.0f * kDepth);
    }
    m[12] = -cx / radius;
    m[13] = -cy / radius;
    m[14] = 0.5f + cz / (2.0f * kDepth);
    m[15] = 1.0f;
  };

  float m_near[16], m_mid[16], m_far[16];
  float texel_near = 0.0f, texel_mid = 0.0f, texel_far = 0.0f;
  float centre_near[3], centre_mid[3], centre_far[3];
  build_cascade(near_radius, 0.42f, m_near, texel_near, centre_near);
  if (three_cascades) {
    build_cascade(mid_radius, 0.50f, m_mid, texel_mid, centre_mid);
  } else {
    std::memset(m_mid, 0, sizeof m_mid);
    centre_mid[0] = centre_mid[1] = centre_mid[2] = 0.0f;
  }
  build_cascade(far_radius, 0.52f, m_far, texel_far, centre_far);

  std::memcpy(frame_.shadow_matrix, m_near, sizeof m_near);
  std::memcpy(frame_.shadow_matrix_mid, m_mid, sizeof m_mid);
  std::memcpy(frame_.shadow_matrix_far, m_far, sizeof m_far);
  frame_.shadow[0] = 1.0f;
  frame_.shadow[1] = 1.0f / float(r_.shadow_size_);
  frame_.shadow[2] = s.strength;
  frame_.shadow[3] = float(s.taps);
  frame_.shadow_bias[0] = 1.35f * texel_near;
  frame_.shadow_bias[1] = frame_.shadow_bias[2] = frame_.shadow_bias[3] = 0.0f;
  frame_.shadow_mid_bias[0] = 1.45f * texel_mid;
  frame_.shadow_mid_bias[1] = three_cascades ? 1.0f : 0.0f;
  frame_.shadow_mid_bias[2] = (pcss_q == modern::ShadowPcss::kUltra) ? 2.0f
                            : (pcss_q == modern::ShadowPcss::kHigh)  ? 1.0f
                                                                     : 0.0f;
  frame_.shadow_mid_bias[3] = float(r_.shadow_cascades_);
  frame_.shadow_far_bias[0] = 1.55f * texel_far;
  frame_.shadow_far_bias[1] = shadow_set_far_ ? 1.0f : 0.0f;

  auto fill_shadow_frame = [&](ShowroomFrame& dst, const float m[16], const float c[3]) {
    dst = frame_;
    std::memcpy(dst.view_projection, m, 16 * sizeof(float));
    std::memcpy(dst.transparent_view_projection, m, 16 * sizeof(float));
    for (int k = 0; k < 3; ++k) dst.eye[k] = c[k] + sun[k] * 1000.0f;
    dst.eye[3] = 1.0f;
    dst.world_fade[1] = 0.0f;
  };
  fill_shadow_frame(shadow_frame_, m_near, centre_near);
  if (three_cascades) fill_shadow_frame(shadow_frame_mid_, m_mid, centre_mid);
  fill_shadow_frame(shadow_frame_far_, m_far, centre_far);
}

void Showroom::record_shadow(VkCommandBuffer cmd) {
  const uint32_t size = r_.shadow_size_;
  const uint32_t cascades = std::max(1u, r_.shadow_cascades_);
  const uint32_t atlas_w = size > 1 ? size * cascades : size;
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = r_.shadow_map_.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &dep);

  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = r_.shadow_map_.view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depth.clearValue.depthStencil = {1.0f, 0};
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, {atlas_w, size}};
  rendering.layerCount = 1;
  rendering.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &rendering);

  shadow_drawn_ = shadow_cars_ = 0;
  const auto eye = eye_in_scenery(world_transform_);
  const bool ultra_pcss = r_.graphics_settings().shadow_pcss == modern::ShadowPcss::kUltra;

  auto draw_cascade = [&](uint32_t x_offset, VkDescriptorSet cascade_set, const ShowroomFrame& sf, bool is_far,
                          bool draw_cars) {
    const VkViewport viewport{float(x_offset), 0, float(size), float(size), 0, 1};
    const VkRect2D scissor{{int32_t(x_offset), 0}, {size, size}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const float* vp = sf.view_projection;
    float clip[16];
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r) {
        float s = 0;
        for (int k = 0; k < 4; ++k) s += vp[k * 4 + r] * world_transform_[c * 4 + k];
        clip[c * 4 + r] = s;
      }
    float planes[6][4];
    for (int c = 0; c < 4; ++c) {
      const float x = clip[c * 4 + 0], y = clip[c * 4 + 1], z = clip[c * 4 + 2], w = clip[c * 4 + 3];
      planes[0][c] = w + x, planes[1][c] = w - x, planes[2][c] = w + y, planes[3][c] = w - y;
      planes[4][c] = z, planes[5][c] = w - z;
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
    world_bind_buffers(cmd, false);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 1, &cascade_set, 0,
                            nullptr);
    VkPipeline bound = VK_NULL_HANDLE;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    for (const auto& d : world_draws_) {
      if (!d.cull || d.blend >= 2 || outside(d) || near_lod_beyond(d, eye)) continue;
      // Far cascade skips tiny billboard/cutout draws unless Ultra PCSS is active for fast 4K framing.
      if (is_far && d.blend == 1 && !ultra_pcss) continue;
      ++shadow_drawn_;
      const VkPipeline pipeline = d.blend == 1 ? shadow_cutout_ : shadow_world_;
      if (pipeline != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = pipeline);
      ShowroomPush push = d.push;
      if (d.blend == 1) {
        VkDescriptorSet set = d.texture != kNoWorldTexture ? world_set(d.texture) : VK_NULL_HANDLE;
        if (set) {
          std::lock_guard<std::mutex> lock(world_textures_mutex_);
          world_lru_.touch(d.texture, world_frame_);
        }
        if (!set) {
          set = world_white_set_;
          push.params[1] = 0;
        }
        if (set != bound_set)
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 1, 1,
                                  &(bound_set = set), 0, nullptr);
      }
      world_draw_model(d, world_transform_, push.model);
      std::memcpy(push.uv0, world_sun_, sizeof push.uv0);
      std::memcpy(push.uv1, world_up_, sizeof push.uv1);
      push.params[2] = 0;
      vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
    }
    // Player & traffic cars cast crisp shadows in Near (~20m) and Mid (~90m) cascades.
    if (draw_cars) {
      if (draw_car_) record_shadow_car(cmd, nullptr, cascade_set);
      if (!instances_.empty() && car_inst_buf_.ready() && shadow_car_) {
        std::vector<Showroom*> groups;
        for (const auto& in : instances_) {
          if (in.car && std::find(groups.begin(), groups.end(), in.car) == groups.end()) groups.push_back(in.car);
        }
        const uint32_t slot_base = r_.frame_slot() * kMaxCarInstances;
        uint32_t offset = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_car_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &cascade_set, 0, nullptr);
        for (Showroom* g : groups) {
          uint32_t solid_count = 0, fade_count = 0;
          for (const auto& in : instances_)
            if (in.car == g && in.fade_alpha >= 0.99f && offset + solid_count < kMaxCarInstances) ++solid_count;
          for (const auto& in : instances_)
            if (in.car == g && in.fade_alpha < 0.99f && offset + solid_count + fade_count < kMaxCarInstances)
              ++fade_count;
          if (solid_count > 0 && !g->draws_.empty()) {
            const VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &g->vertex_buffer_.buffer, &zero);
            vkCmdBindIndexBuffer(cmd, g->index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
            for (const auto& d : g->draws_) {
              if (d.glass) continue;
              ShowroomPush push = d.push;
              push.params[1] = float(uint32_t(push.params[1] + 0.5f) | 8192u);
              vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(ShowroomPush), &push);
              vkCmdDrawIndexed(cmd, d.index_count, solid_count, d.first_index, d.vertex_offset, slot_base + offset);
            }
            shadow_cars_ += solid_count;
          }
          offset += solid_count + fade_count;
        }
      }
    }
  };

  draw_cascade(0, shadow_set_, shadow_frame_, false, true);
  if (cascades >= 3u && shadow_set_mid_ && shadow_set_far_ && atlas_w >= size * 3u) {
    draw_cascade(size, shadow_set_mid_, shadow_frame_mid_, false, true);
    draw_cascade(size * 2u, shadow_set_far_, shadow_frame_far_, true, false);
  } else if (shadow_set_far_ && atlas_w >= size * 2u) {
    draw_cascade(size, shadow_set_far_, shadow_frame_far_, true, false);
  }
  vkCmdEndRendering(cmd);

  b.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier2(cmd, &dep);
}

void Showroom::record_shadow_car(VkCommandBuffer cmd, const float* m, VkDescriptorSet cascade_set) {
  VkDescriptorSet set = cascade_set ? cascade_set : shadow_set_;
  if (draws_.empty() || !shadow_car_ || !set) return;
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_car_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
  for (const auto& d : draws_) {
    if (d.glass) continue;  // glass and lamp covers cast none (ours)
    ShowroomPush push = d.push;
    if (m) multiply(m, d.push.model, push.model);
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
  }
}


bool Showroom::render_mask(uint32_t width, uint32_t height, std::vector<uint8_t>& mask,
                           std::string& error, bool fraction) {
  // Drawn at the colour pass's MSAA and resolved; a pixel is covered when
  // any of its samples is, as the reference masks count every pixel the
  // car's draws change after the resolve (AUTOSHOW_REFERENCE, Mask).
  const bool msaa = r_.samples_ != VK_SAMPLE_COUNT_1_BIT;
  Renderer::Image target, multi;
  if (!r_.create_image(width, height, 1, VK_FORMAT_R8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, target) ||
      (msaa && !r_.create_image(width, height, 1, VK_FORMAT_R8_UNORM, r_.samples_,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, multi))) {
    r_.destroy_image(target);
    r_.destroy_image(multi);
    error = "mask target";
    return false;
  }
  Renderer::Buffer readback;
  const VkDeviceSize bytes = VkDeviceSize(width) * height;
  if (!r_.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, readback)) {
    r_.destroy_image(target);
    r_.destroy_image(multi);
    error = "mask readback";
    return false;
  }
  const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
    update_frame(cmd);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = target.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    if (msaa) {
      b.image = multi.image;
      vkCmdPipelineBarrier2(cmd, &dep);
      b.image = target.image;
    }
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = msaa ? multi.view : target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0, 0, 0, 0}};
    if (msaa) {
      color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
      color.resolveImageView = target.view;
      color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
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
    record_draws(cmd, mask_, mask_);
    vkCmdEndRendering(cmd);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo host_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    host_dep.memoryBarrierCount = 1;
    host_dep.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(cmd, &host_dep);
  });
  bool readback_ok = ok;
  if (ok) {
    void* mapped = nullptr;
    if (vmaMapMemory(r_.allocator_, readback.allocation, &mapped) == VK_SUCCESS) {
      const uint8_t* src = static_cast<const uint8_t*>(mapped);
      mask.resize(size_t(bytes));
      for (size_t i = 0; i < mask.size(); ++i) mask[i] = fraction ? src[i] : (src[i] != 0 ? 255 : 0);
      vmaUnmapMemory(r_.allocator_, readback.allocation);
    } else {
      error = "mask readback map failed";
      readback_ok = false;
    }
  } else {
    error = "mask render submission failed";
  }
  r_.destroy_buffer(readback);
  r_.destroy_image(target);
  r_.destroy_image(multi);
  return readback_ok;
}


}  // namespace carview
