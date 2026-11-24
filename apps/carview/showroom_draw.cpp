#include "showroom_internal.h"

#include "world/world_anim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace carview {

namespace {

struct DrivingLineAnchor {
  bool active = false;
  bool has_anchor = false;
  double prev_wx = 0.0;
  double prev_wz = 0.0;
  double ref_wx = 0.0;
  double ref_wz = 0.0;
  double ref_s = 0.0;
  double base_s = 0.0;
};

std::unordered_map<const Showroom*, DrivingLineAnchor>& driving_line_anchors() {
  static std::unordered_map<const Showroom*, DrivingLineAnchor> anchors;
  return anchors;
}

bool same_pos3(const float a[3], const float b[3]) {
  return std::fabs(a[0] - b[0]) < 1e-4f && std::fabs(a[1] - b[1]) < 1e-4f && std::fabs(a[2] - b[2]) < 1e-4f;
}

}  // namespace

void Showroom::set_background(float r, float g, float b) {
  r_.scene_.clear[0] = r;
  r_.scene_.clear[1] = g;
  r_.scene_.clear[2] = b;
  r_.scene_.clear[3] = 1.0f;
}

void Showroom::set_world_overlay(std::span<const WorldVertex> triangles) {
  const size_t n = std::min(triangles.size(), kMaxOverlayVertices);
  const size_t tri_n = (n / 3) * 3;
  overlay_cpu_.assign(triangles.begin(), triangles.begin() + ptrdiff_t(tri_n));
  overlay_count_ = uint32_t(tri_n);
  overlay_dirty_ = true;

  auto& anchor = driving_line_anchors()[this];
  anchor.active = false;
  if (tri_n < 6 || (tri_n % 6) != 0) {
    anchor.has_anchor = false;
    return;
  }
  // Detect driving/braking line quad strip emitted by drive::build_driving_line_mesh:
  // each quad is (al, ar, br, al, br, bl) with unpopulated uv0 == (0, 0).
  const auto& v0 = overlay_cpu_[0];
  const auto& v2 = overlay_cpu_[2];
  const auto& v3 = overlay_cpu_[3];
  const auto& v4 = overlay_cpu_[4];
  if (!same_pos3(v0.position, v3.position) || !same_pos3(v2.position, v4.position) ||
      v0.uv0[0] != 0.0f || v0.uv0[1] != 0.0f) {
    anchor.has_anchor = false;
    return;
  }

  constexpr float kLiftDropM = 0.065f;      // 0.10 m -> ~0.035 m (~3.5 cm) crown-safe hover above asphalt
  constexpr float kTargetHalfW = 0.70f;     // sleek feathered-edge margin matching FH1 lane proportion
  constexpr double kChevronPitchM = 3.4;    // matches world_frag.glsl kChevronPitch
  constexpr double kWrapPeriodM = kChevronPitchM * 100.0;

  const size_t quad_count = tri_n / 6;
  struct QuadSpan {
    double ax = 0.0, az = 0.0;
    double bx = 0.0, bz = 0.0;
    double loc_a = 0.0, loc_b = 0.0;
    double seg_len = 0.0;
    bool continuous = false;
  };
  std::vector<QuadSpan> spans(quad_count);
  double s_local_accum = 0.0;
  double prev_bx_l = 0.0;
  double prev_bz_l = 0.0;
  for (size_t q = 0; q < quad_count; ++q) {
    const size_t base = q * 6;
    const auto& al = overlay_cpu_[base + 0];
    const auto& ar = overlay_cpu_[base + 1];
    const auto& br = overlay_cpu_[base + 2];
    const auto& bl = overlay_cpu_[base + 5];
    const double ax = 0.5 * (double(al.position[0]) + double(ar.position[0]));
    const double az = 0.5 * (double(al.position[2]) + double(ar.position[2]));
    const double bx = 0.5 * (double(bl.position[0]) + double(br.position[0]));
    const double bz = 0.5 * (double(bl.position[2]) + double(br.position[2]));
    bool continuous = false;
    if (q > 0) {
      const double gap = std::hypot(ax - prev_bx_l, az - prev_bz_l);
      if (gap > 0.02) {
        s_local_accum += gap;
      } else {
        continuous = true;
      }
    }
    const double seg_len = std::hypot(bx - ax, bz - az);
    spans[q] = {ax, az, bx, bz, s_local_accum, s_local_accum + seg_len, seg_len, continuous};
    s_local_accum += seg_len;
    prev_bx_l = bx;
    prev_bz_l = bz;
  }

  const double wx0 = spans[0].ax + world_origin_[0];
  const double wz0 = spans[0].az + world_origin_[1];
  const double fx0 = spans[0].bx - spans[0].ax;
  const double fz0 = spans[0].bz - spans[0].az;
  const double flen0 = std::hypot(fx0, fz0);

  auto wrap_s = [&](double s) -> double {
    double w = std::fmod(s, kWrapPeriodM);
    if (w < 0.0) w += kWrapPeriodM;
    return w;
  };

  bool matched_ref = false;
  double ref_proj_local = 0.0;
  if (anchor.has_anchor) {
    const double rlx = anchor.ref_wx - world_origin_[0];
    const double rlz = anchor.ref_wz - world_origin_[1];
    double best_d2 = 1e30;
    double best_s = 0.0;
    for (size_t q = 0; q < quad_count; ++q) {
      const auto& sp = spans[q];
      const double vx = sp.bx - sp.ax;
      const double vz = sp.bz - sp.az;
      const double len2 = vx * vx + vz * vz;
      double t = 0.0;
      if (len2 > 1e-8) {
        t = std::clamp(((rlx - sp.ax) * vx + (rlz - sp.az) * vz) / len2, 0.0, 1.0);
      }
      const double px = sp.ax + t * vx;
      const double pz = sp.az + t * vz;
      const double dx = rlx - px;
      const double dz = rlz - pz;
      const double d2 = dx * dx + dz * dz;
      if (d2 < best_d2) {
        best_d2 = d2;
        best_s = sp.loc_a + t * sp.seg_len;
      }
    }
    if (best_d2 <= 3.0 * 3.0) {
      // Lock chevron phase directly to a fixed world-space reference point on the centerline:
      // eliminates per-frame chord drift on curves and BrakingOnly/GPS re-slicing slip.
      anchor.base_s = wrap_s(anchor.ref_s - best_s);
      ref_proj_local = best_s;
      matched_ref = true;
    } else if (flen0 > 1e-4) {
      const double dx = wx0 - anchor.prev_wx;
      const double dz = wz0 - anchor.prev_wz;
      const double step_dist = std::hypot(dx, dz);
      if (step_dist < 40.0) {
        const double ds = (dx * fx0 + dz * fz0) / flen0;
        anchor.base_s = wrap_s(anchor.base_s + ds);
      } else {
        anchor.base_s = 0.0;
      }
    } else {
      anchor.base_s = 0.0;
    }
  } else {
    anchor.base_s = 0.0;
  }

  // Pick or advance the fixed world-space reference point ~22 m ahead along the strip
  // whenever initializing, resetting, or approaching within 4 m of the current anchor.
  if (!matched_ref || ref_proj_local < 4.0) {
    const double target_s = std::min(s_local_accum * 0.5, 22.0);
    double pick_lx = spans[0].ax;
    double pick_lz = spans[0].az;
    double pick_s = 0.0;
    for (size_t q = 0; q < quad_count; ++q) {
      const auto& sp = spans[q];
      if (target_s <= sp.loc_b || q + 1 == quad_count) {
        const double t = sp.seg_len > 1e-6 ? std::clamp((target_s - sp.loc_a) / sp.seg_len, 0.0, 1.0) : 0.0;
        pick_lx = sp.ax + t * (sp.bx - sp.ax);
        pick_lz = sp.az + t * (sp.bz - sp.az);
        pick_s = sp.loc_a + t * sp.seg_len;
        break;
      }
    }
    anchor.ref_wx = pick_lx + world_origin_[0];
    anchor.ref_wz = pick_lz + world_origin_[1];
    anchor.ref_s = wrap_s(anchor.base_s + pick_s);
  }
  anchor.prev_wx = wx0;
  anchor.prev_wz = wz0;
  anchor.has_anchor = true;
  anchor.active = true;

  auto widen_edge = [&](WorldVertex& left, WorldVertex& right) {
    const float mx = 0.5f * (left.position[0] + right.position[0]);
    const float mz = 0.5f * (left.position[2] + right.position[2]);
    const float rx = 0.5f * (right.position[0] - left.position[0]);
    const float rz = 0.5f * (right.position[2] - left.position[2]);
    const float hw = std::hypot(rx, rz);
    if (hw > 0.05f && hw < kTargetHalfW) {
      const float scale = std::clamp(kTargetHalfW / hw, 1.0f, 1.35f);
      left.position[0] = mx - rx * scale;
      left.position[2] = mz - rz * scale;
      right.position[0] = mx + rx * scale;
      right.position[2] = mz + rz * scale;
    }
    left.position[1] -= kLiftDropM;
    right.position[1] -= kLiftDropM;
  };

  WorldVertex prev_bl{};
  WorldVertex prev_br{};
  bool has_prev_edge = false;
  for (size_t q = 0; q < quad_count; ++q) {
    const size_t base = q * 6;
    WorldVertex al = overlay_cpu_[base + 0];
    WorldVertex ar = overlay_cpu_[base + 1];
    WorldVertex br = overlay_cpu_[base + 2];
    WorldVertex bl = overlay_cpu_[base + 5];

    const auto& sp = spans[q];
    const double s_a = anchor.base_s + sp.loc_a;
    const double s_b = anchor.base_s + sp.loc_b;
    const double loc_a = sp.loc_a;
    const double loc_b = sp.loc_b;

    widen_edge(al, ar);
    widen_edge(bl, br);
    if (has_prev_edge && sp.continuous) {
      al.position[0] = prev_bl.position[0];
      al.position[1] = prev_bl.position[1];
      al.position[2] = prev_bl.position[2];
      ar.position[0] = prev_br.position[0];
      ar.position[1] = prev_br.position[1];
      ar.position[2] = prev_br.position[2];
    }
    const float fx = float(sp.bx - sp.ax);
    const float fz = float(sp.bz - sp.az);
    if (fx * fx + fz * fz > 1e-6f) {
      // Prevent inner-edge bowtie folding on tight hairpins so adjacent translucent quads
      // never overlap and double-blend.
      if ((bl.position[0] - al.position[0]) * fx + (bl.position[2] - al.position[2]) * fz < 0.0f) {
        bl.position[0] = al.position[0];
        bl.position[2] = al.position[2];
      }
      if ((br.position[0] - ar.position[0]) * fx + (br.position[2] - ar.position[2]) * fz < 0.0f) {
        br.position[0] = ar.position[0];
        br.position[2] = ar.position[2];
      }
    }
    prev_bl = bl;
    prev_br = br;
    has_prev_edge = true;

    const float p_a = float(q) / float(quad_count);
    const float p_b = float(q + 1) / float(quad_count);
    al.uv0[0] = -1.0f;
    al.uv0[1] = float(s_a);
    al.uv1[0] = p_a;
    al.uv1[1] = float(loc_a);
    ar.uv0[0] = 1.0f;
    ar.uv0[1] = float(s_a);
    ar.uv1[0] = p_a;
    ar.uv1[1] = float(loc_a);
    bl.uv0[0] = -1.0f;
    bl.uv0[1] = float(s_b);
    bl.uv1[0] = p_b;
    bl.uv1[1] = float(loc_b);
    br.uv0[0] = 1.0f;
    br.uv0[1] = float(s_b);
    br.uv1[0] = p_b;
    br.uv1[1] = float(loc_b);
    for (WorldVertex* vert : {&al, &ar, &bl, &br}) {
      vert->normal[0] = 0;
      vert->normal[1] = 127;
      vert->normal[2] = 0;
    }

    overlay_cpu_[base + 0] = al;
    overlay_cpu_[base + 1] = ar;
    overlay_cpu_[base + 2] = br;
    overlay_cpu_[base + 3] = al;
    overlay_cpu_[base + 4] = br;
    overlay_cpu_[base + 5] = bl;
  }
}

void Showroom::update_frame(VkCommandBuffer cmd) {
  update_buffer(cmd, frame_buffer_.buffer, frame_);
  if (overlay_dirty_ && overlay_vertices_.buffer) {
    overlay_dirty_ = false;
    if (!overlay_cpu_.empty()) {
      VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
      b.srcAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.buffer = overlay_vertices_.buffer;
      b.size = VK_WHOLE_SIZE;
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.bufferMemoryBarrierCount = 1;
      dep.pBufferMemoryBarriers = &b;
      vkCmdPipelineBarrier2(cmd, &dep);
      vkCmdUpdateBuffer(cmd, overlay_vertices_.buffer, 0, overlay_cpu_.size() * sizeof(WorldVertex),
                        overlay_cpu_.data());
      b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
      b.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
      vkCmdPipelineBarrier2(cmd, &dep);
    }
  }
}

void Showroom::update_buffer(VkCommandBuffer cmd, VkBuffer buffer, const ShowroomFrame& f) {
  VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  b.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.buffer = buffer;
  b.size = VK_WHOLE_SIZE;
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.bufferMemoryBarrierCount = 1;
  dep.pBufferMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &dep);
  ShowroomFrame copy = f;
  for (int k = 0; k < 4; ++k) {
    copy.cockpit_mfd[k] = cockpit_mfd_[k];
    copy.cockpit_mfd_ext[k] = cockpit_mfd_ext_[k];
    copy.damage_corners[k] = damage_corners_[k];
    copy.paint_shader_a[k] = paint_shader_a_[k];
    copy.paint_shader_b[k] = paint_shader_b_[k];
    copy.paint_shader_c[k] = paint_shader_c_[k];
    copy.paint_shader_d[k] = paint_shader_d_[k];
    copy.rim_shader[k] = rim_shader_[k];
    copy.rim_tint_dirt[k] = rim_tint_dirt_[k];
  }
  copy.cockpit_mfd_ext[3] = std::clamp(lamp_vectors_.dynamic_lights_2[0], 0.0f, 1.0f);
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 4; ++k) {
      copy.region_two_tone[r][k] = region_two_tone_[r][k];
      copy.region_finish[r][k] = region_finish_[r][k];
    }
  }
  vkCmdUpdateBuffer(cmd, buffer, 0, sizeof(ShowroomFrame), &copy);
  b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  b.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
  vkCmdPipelineBarrier2(cmd, &dep);
}


void Showroom::record_draws(VkCommandBuffer cmd, VkPipeline opaque, VkPipeline glass, int pass_mode) {
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &frame_set_, 0,
                          nullptr);
  uint32_t bound_desc = UINT32_MAX;
  auto draw = [&](const Draw& d) {
    if (d.descriptor != bound_desc) {
      bound_desc = d.descriptor;
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1,
                              &material_sets_[d.descriptor], 0, nullptr);
    }
    ShowroomPush push = d.push;
    if (d.lamp_channel != drive::LampChannel::kNone && opaque != glass) {
      const auto rgb = drive::evaluate_lamp_emission(d.lamp_channel, lamp_vectors_, lamp_colors_);
      if (rgb[0] > 1e-4f || rgb[1] > 1e-4f || rgb[2] > 1e-4f) {
        push.tint[0] = rgb[0];
        push.tint[1] = rgb[1];
        push.tint[2] = rgb[2];
        push.params[1] = float(uint32_t(push.params[1] + 0.5f) | kFlagLampEmissive);
      }
    }
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
  };
  if (pass_mode != 2) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque);
    r_.apply_vrs(cmd);
    for (const auto& d : draws_) {
      if (d.detached) continue;
      if (d.is_blur_rim) continue;
      if (d.is_static_spoke && d.push.params[3] >= 0.85f) continue;
      if (!d.glass || opaque == glass) {
        draw(d);
        ++car_draws_;
      }
    }
    if (opaque == glass) return;
    if (!guest_) r_.mark(cmd, "car_opaque");
    if (pass_mode == 1) return;
  } else if (opaque == glass) {
    return;
  }
  // Glass & blended blur_rim last, far to near from the eye.
  std::vector<const Draw*> sorted;
  for (const auto& d : draws_) {
    if (d.detached) continue;
    if (d.is_blur_rim && d.push.params[3] <= 0.05f) continue;
    if (d.glass) sorted.push_back(&d);
  }
  auto distance = [&](const Draw* d) {
    float s = 0;
    for (int k = 0; k < 3; ++k) {
      const float e = d->centre[size_t(k)] - frame_.eye[k];
      s += e * e;
    }
    return s;
  };
  std::stable_sort(sorted.begin(), sorted.end(),
                   [&](const Draw* a, const Draw* b) { return distance(a) > distance(b); });
  VkPipeline bound_glass = VK_NULL_HANDLE;
  for (const Draw* d : sorted) {
    if (d->is_cover && glass_mul_ && glass_add_) {
      // FH1 two-pass detail_glass_red/amber/clear lamp cover (SHOWROOM_LIGHTING.md:187):
      // Pass 1 Multiply transmittance with LightsSampler + GlassColor,
      // Pass 2 Additive Fresnel env reflection + sun specular.
      if (d->descriptor != bound_desc) {
        bound_desc = d->descriptor;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1,
                                &material_sets_[d->descriptor], 0, nullptr);
      }
      ShowroomPush push = d->push;
      if (bound_glass != glass_mul_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_glass = glass_mul_);
      push.params[2] = 7.0f;
      vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d->index_count, 1, d->first_index, d->vertex_offset, 0);

      if (bound_glass != glass_add_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_glass = glass_add_);
      push.params[2] = 8.0f;
      vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d->index_count, 1, d->first_index, d->vertex_offset, 0);
      car_draws_ += 2;
    } else {
      if (bound_glass != glass) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_glass = glass);
      draw(*d);
      ++car_draws_;
    }
  }
}


void Showroom::set_instances(std::vector<Instance> list) {
  instances_ = std::move(list);
  if (dyn_env_ready_ && dyn_env_.view && sampler_) {
    for (const auto& in : instances_) {
      if (in.car && in.car->bound_dyn_env_view_ != dyn_env_.view) {
        in.car->bind_dynamic_env(dyn_env_.view, sampler_);
      }
    }
  }
}

void Showroom::add_instances(const std::vector<Instance>& more) {
  instances_.insert(instances_.end(), more.begin(), more.end());
  if (dyn_env_ready_ && dyn_env_.view && sampler_) {
    for (const auto& in : more) {
      if (in.car && in.car->bound_dyn_env_view_ != dyn_env_.view) {
        in.car->bind_dynamic_env(dyn_env_.view, sampler_);
      }
    }
  }
}

void Showroom::record_contact_shadows(VkCommandBuffer cmd) {
  if (!world_lit_ || !contact_shadow_pipe_ || !contact_shadow_pipe_layout_ || !frame_set_) return;
  struct ContactPush {
    float model[16];
    float params[4];
    float wheels_uv[4];
  };
  static_assert(sizeof(ContactPush) == 96);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, contact_shadow_pipe_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, contact_shadow_pipe_layout_, 0, 1, &frame_set_, 0,
                          nullptr);
  const float sign = r_.reverse_z() ? 1.0f : -1.0f;
  vkCmdSetDepthBias(cmd, sign * 96.0f, 0.0f, sign * 1.5f);
  const float night_amt = tod_active_ ? std::clamp(tod_state_.switch_on_lights, 0.0f, 1.0f) : 0.0f;
  // Keep cars grounded cleanly across bright sun, shade, overcast, and night on all 4 hardware tiers.
  const float min_dark = 0.14f + 0.10f * night_amt;

  auto draw_one = [&](const Showroom* car_sr, const float* car_model, const std::array<float, 3>& bmin,
                      const std::array<float, 3>& bmax, float strength) {
    const float hx = std::max(0.75f, 0.5f * (bmax[0] - bmin[0])) * 1.18f;
    const float hz = std::max(1.65f, 0.5f * (bmax[2] - bmin[2])) * 1.10f;
    const float cx = 0.5f * (bmin[0] + bmax[0]);
    const float cz = 0.5f * (bmin[2] + bmax[2]);
    const float cy = bmin[1] + 0.025f;

    // Derive per-car normalized wheel contact patch coordinates & suspension clearance from live poses/rest hubs.
    float f_u = 0.58f, f_v = -0.54f, r_u = 0.58f, r_v = 0.54f;
    float clearance = 0.25f;
    if (car_sr) {
      const auto& rest_w = car_sr->car_.car.wheel_centres;
      const auto& rest_b = car_sr->car_.car.wheel_centres_body;
      float fl_x = std::fabs(rest_w[0][0]), fr_x = std::fabs(rest_w[1][0]);
      float rl_x = std::fabs(rest_w[2][0]), rr_x = std::fabs(rest_w[3][0]);
      if (fl_x + fr_x > 0.2f && rl_x + rr_x > 0.2f) {
        f_u = std::clamp(0.5f * (fl_x + fr_x) / hx, 0.35f, 0.84f);
        r_u = std::clamp(0.5f * (rl_x + rr_x) / hx, 0.35f, 0.84f);
        const float fz = 0.5f * (rest_w[0][2] + rest_w[1][2]);
        const float rz = 0.5f * (rest_w[2][2] + rest_w[3][2]);
        f_v = std::clamp((fz - cz) / hz, -0.82f, 0.82f);
        r_v = std::clamp((rz - cz) / hz, -0.82f, 0.82f);
      }
      if (car_sr->have_wheel_poses_) {
        float avg_drop = 0.0f;
        for (int w = 0; w < 4; ++w) {
          // Lower wheel hub relative to rest body frame => chassis lifted higher off the road.
          avg_drop += (rest_b[size_t(w)][1] - car_sr->wheel_poses_[w].centre[1]);
        }
        avg_drop *= 0.25f;
        clearance = std::clamp(0.25f + avg_drop * 2.4f, 0.0f, 1.0f);
      }
    }

    float local[16] = {hx, 0, 0, 0,  0, 1, 0, 0,  0, 0, hz, 0,  cx, cy, cz, 1};
    ContactPush push{};
    if (car_model) {
      multiply(car_model, local, push.model);
    } else {
      std::memcpy(push.model, local, sizeof push.model);
    }
    push.params[0] = strength;
    push.params[1] = min_dark;
    push.params[2] = 0.24f;  // rounded body footprint corner radius
    push.params[3] = clearance;
    push.wheels_uv[0] = f_u;
    push.wheels_uv[1] = f_v;
    push.wheels_uv[2] = r_u;
    push.wheels_uv[3] = r_v;
    vkCmdPushConstants(cmd, contact_shadow_pipe_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ContactPush), &push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
  };

  if (draw_car_ && !cockpit_camera_active_ && have_bounds_) {
    draw_one(this, nullptr, bounds_min_, bounds_max_, 0.92f);
  }
  for (const auto& in : instances_) {
    if (!in.car || !in.car->have_bounds_) continue;
    const float fa = std::clamp(in.fade_alpha, 0.0f, 1.0f);
    if (fa <= 0.01f) continue;
    draw_one(in.car, in.model, in.car->bounds_min_, in.car->bounds_max_, 0.88f * fa);
  }
  vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
}

void Showroom::record_z_prepass(VkCommandBuffer cmd) {
  if (world_textured_ && !world_draws_.empty()) {
    if (arena_on() && gpu_world_->table_rows() > 0) {
      WorldGpu::DrawParams dp;
      dp.frame_set = frame_set_;
      std::memcpy(dp.model, world_transform_, sizeof dp.model);
      for (int k = 0; k < 3; ++k) dp.sun[k] = world_sun_[k];
      for (int k = 0; k < 4; ++k) dp.up[k] = world_up_[k];
      dp.bias_sign = r_.reverse_z() ? 1.0f : -1.0f;
      dp.decal_bias_constant = kDecalBiasConstant;
      dp.decal_bias_slope = kDecalBiasSlope;
      gpu_world_->record_z_prepass(cmd, dp);
    } else if (world_z_prepass_) {
      world_bind_buffers(cmd, false);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 1, &frame_set_, 0, nullptr);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_z_prepass_);
      for (const auto& d : world_draws_) {
        if (!d.cull || d.blend != 0) continue;
        ShowroomPush push = d.push;
        world_draw_model(d, world_transform_, push.model);
        vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ShowroomPush), &push);
        vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
      }
    }
  }
  if (draw_car_ && car_z_prepass_ && !draws_.empty()) {
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &frame_set_, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, car_z_prepass_);
    for (const auto& d : draws_) {
      if (d.glass || d.detached || d.is_blur_rim) continue;
      if (d.is_static_spoke && d.push.params[3] > 0.45f) continue;
      vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &d.push);
      vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
    }
  }
}

void Showroom::record_all_instances(VkCommandBuffer cmd, bool depth_equal, int pass_mode) {
  if (instances_.empty()) return;
  if (!car_inst_buf_.ready() || !frame_set_) {
    if (pass_mode == 2) return;
    for (const auto& in : instances_)
      if (in.car) in.car->record_instance(cmd, in.model, in.has_lamps ? &in.lamps : nullptr);
    return;
  }
  std::vector<Showroom*> groups;
  for (const auto& in : instances_) {
    if (in.car && std::find(groups.begin(), groups.end(), in.car) == groups.end()) groups.push_back(in.car);
  }
  const uint32_t slot_base = r_.frame_slot() * kMaxCarInstances;
  uint32_t offset = 0;
  const VkPipeline op_pipe = (depth_equal && opaque_after_z_) ? opaque_after_z_ : opaque_;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &frame_set_, 0, nullptr);
  const int pass_begin = (pass_mode == 2) ? 1 : 0;
  const int pass_end = (pass_mode == 1) ? 1 : 2;
  for (Showroom* g : groups) {
    uint32_t solid_count = 0;
    float min_inst_d2 = 1e18f;
    std::vector<float> fade_alphas;
    for (const auto& in : instances_) {
      if (in.car != g) continue;
      const float dx = in.model[12] - frame_.eye[0];
      const float dy = in.model[13] - frame_.eye[1];
      const float dz = in.model[14] - frame_.eye[2];
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (d2 < min_inst_d2) min_inst_d2 = d2;
      if (in.fade_alpha >= 0.99f && offset + solid_count < kMaxCarInstances) ++solid_count;
    }
    for (const auto& in : instances_)
      if (in.car == g && in.fade_alpha < 0.99f && offset + solid_count + uint32_t(fade_alphas.size()) < kMaxCarInstances)
        fade_alphas.push_back(std::clamp(in.fade_alpha, 0.0f, 1.0f));
    const uint32_t total_count = solid_count + uint32_t(fade_alphas.size());
    if (total_count == 0 || g->draws_.empty() || g->material_sets_.empty()) continue;
    // Distance-based runtime LOD detail culling for distant AI/traffic cars:
    // beyond 55 m skip interior, gauges, calipers, and cockpit-only parts;
    // beyond 145 m skip subtle mirror/emissive/trim overlays.
    const bool lod_mid = min_inst_d2 > 55.0f * 55.0f;
    const bool lod_far = min_inst_d2 > 145.0f * 145.0f;
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g->vertex_buffer_.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, g->index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
    if (solid_count > 0) {
      for (int pass = pass_begin; pass < pass_end; ++pass) {
        VkPipeline bound_p = VK_NULL_HANDLE;
        uint32_t bound_desc = UINT32_MAX;
        for (const auto& d : g->draws_) {
          if (d.detached) continue;
          if (d.is_blur_rim && d.push.params[3] <= 0.05f) continue;
          if (d.is_static_spoke && d.push.params[3] >= 0.85f) continue;
          if (d.glass != (pass == 1)) continue;
          if (d.cockpit_role >= 0) continue;
          if (lod_mid) {
            const uint32_t kl = uint32_t(d.push.params[0] + 0.5f);
            if (kl == 4u || kl == 5u || kl == 8u) continue;  // kInterior, kGauge, kCaliper
            if (lod_far && d.glass && d.is_cover) continue;
          }
          ShowroomPush push = d.push;
          uint32_t flags = uint32_t(push.params[1] + 0.5f) | kFlagInstanced;
          flags |= (uint32_t(d.lamp_channel) & 31u) << 14u;
          push.params[1] = float(flags);
          if (d.descriptor != bound_desc) {
            bound_desc = d.descriptor;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1,
                                    &g->material_sets_[d.descriptor], 0, nullptr);
          }
          if (pass == 1 && d.is_cover && glass_mul_ && glass_add_) {
            if (bound_p != glass_mul_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_p = glass_mul_);
            push.params[2] = 7.0f;
            vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(ShowroomPush), &push);
            vkCmdDrawIndexed(cmd, d.index_count, solid_count, d.first_index, d.vertex_offset, slot_base + offset);
            if (bound_p != glass_add_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_p = glass_add_);
            push.params[2] = 8.0f;
            vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(ShowroomPush), &push);
            vkCmdDrawIndexed(cmd, d.index_count, solid_count, d.first_index, d.vertex_offset, slot_base + offset);
          } else {
            VkPipeline want = pass ? g->glass_ : op_pipe;
            if (bound_p != want) {
              vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_p = want);
              if (!pass) r_.apply_vrs(cmd);
            }
            vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(ShowroomPush), &push);
            vkCmdDrawIndexed(cmd, d.index_count, solid_count, d.first_index, d.vertex_offset, slot_base + offset);
          }
        }
      }
    }
    // Smooth alpha fade-in/out for spawning/despawning traffic cars (pass_mode == 2 blended pass).
    if (!fade_alphas.empty() && pass_mode != 1 && opaque_fade_) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque_fade_);
      uint32_t bound_desc = UINT32_MAX;
      for (uint32_t fi = 0; fi < uint32_t(fade_alphas.size()); ++fi) {
        const float a = fade_alphas[fi];
        if (a <= 0.005f) continue;
        const float bc[4] = {a, a, a, a};
        vkCmdSetBlendConstants(cmd, bc);
        const uint32_t inst_slot = slot_base + offset + solid_count + fi;
        for (const auto& d : g->draws_) {
          if (d.detached || d.is_blur_rim) continue;
          if (d.glass && d.is_cover) continue;
          ShowroomPush push = d.push;
          uint32_t flags = uint32_t(push.params[1] + 0.5f) | kFlagInstanced;
          flags |= (uint32_t(d.lamp_channel) & 31u) << 14u;
          push.params[1] = float(flags);
          if (d.descriptor != bound_desc) {
            bound_desc = d.descriptor;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1,
                                    &g->material_sets_[d.descriptor], 0, nullptr);
          }
          vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(ShowroomPush), &push);
          vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, inst_slot);
        }
      }
    }
    offset += total_count;
  }
}


void Showroom::record_instance(VkCommandBuffer cmd, const float m[16], const drive::LampShaderVectors* lamps) {
  if (draws_.empty() || material_sets_.empty()) return;
  const drive::LampShaderVectors& active_lamps = lamps ? *lamps : lamp_vectors_;
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &frame_set_, 0,
                          nullptr);
  for (int pass = 0; pass < 2; ++pass) {
    VkPipeline bound_p = VK_NULL_HANDLE;
    for (const auto& d : draws_) {
      if (d.detached) continue;
      if (d.is_blur_rim && d.push.params[3] <= 0.05f) continue;
      if (d.is_static_spoke && d.push.params[3] >= 0.85f) continue;
      if (d.glass != (pass == 1)) continue;
      ShowroomPush push = d.push;
      multiply(m, d.push.model, push.model);
      if (d.lamp_channel != drive::LampChannel::kNone) {
        const auto rgb = drive::evaluate_lamp_emission(d.lamp_channel, active_lamps, lamp_colors_);
        if (rgb[0] > 1e-4f || rgb[1] > 1e-4f || rgb[2] > 1e-4f) {
          push.tint[0] = rgb[0];
          push.tint[1] = rgb[1];
          push.tint[2] = rgb[2];
          push.params[1] = float(uint32_t(push.params[1] + 0.5f) | kFlagLampEmissive);
        }
      }
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1,
                              &material_sets_[d.descriptor], 0, nullptr);
      if (pass == 1 && d.is_cover && glass_mul_ && glass_add_) {
        if (bound_p != glass_mul_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_p = glass_mul_);
        push.params[2] = 7.0f;
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ShowroomPush), &push);
        vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);

        if (bound_p != glass_add_) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_p = glass_add_);
        push.params[2] = 8.0f;
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ShowroomPush), &push);
        vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
      } else {
        VkPipeline want = pass ? glass_ : opaque_;
        if (bound_p != want) {
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_p = want);
          if (!pass) r_.apply_vrs(cmd);
        }
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ShowroomPush), &push);
        vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
      }
    }
  }
}

void Showroom::record_instance_offscreen(VkCommandBuffer cmd, VkPipeline car_opaque, VkDescriptorSet view_frame_set,
                                         const float m[16], const drive::LampShaderVectors* lamps,
                                         VkPipeline car_glass) {
  if (draws_.empty() || material_sets_.empty() || !car_opaque || !view_frame_set) return;
  const drive::LampShaderVectors& active_lamps = lamps ? *lamps : lamp_vectors_;
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &view_frame_set, 0, nullptr);
  const int passes = (car_glass != VK_NULL_HANDLE) ? 2 : 1;
  for (int pass = 0; pass < passes; ++pass) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == 0 ? car_opaque : car_glass);
    uint32_t bound_desc = UINT32_MAX;
    for (const auto& d : draws_) {
      if (d.detached || d.is_blur_rim) continue;
      if (d.glass != (pass == 1)) continue;
      if (d.cockpit_role >= 0) continue;
      ShowroomPush push = d.push;
      multiply(m, d.push.model, push.model);
      if (d.lamp_channel != drive::LampChannel::kNone) {
        const auto rgb = drive::evaluate_lamp_emission(d.lamp_channel, active_lamps, lamp_colors_);
        if (rgb[0] > 1e-4f || rgb[1] > 1e-4f || rgb[2] > 1e-4f) {
          push.tint[0] = rgb[0];
          push.tint[1] = rgb[1];
          push.tint[2] = rgb[2];
          push.params[1] = float(uint32_t(push.params[1] + 0.5f) | kFlagLampEmissive);
        }
      }
      if (d.descriptor != bound_desc) {
        bound_desc = d.descriptor;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1,
                                &material_sets_[d.descriptor], 0, nullptr);
      }
      vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
    }
  }
}

void Showroom::record_player_mirror_rear(VkCommandBuffer cmd, VkPipeline car_opaque, VkDescriptorSet view_frame_set,
                                         float min_rear_z) {
  if (draws_.empty() || material_sets_.empty() || !car_opaque || !view_frame_set) return;
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, car_opaque);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &view_frame_set, 0, nullptr);
  uint32_t bound_desc = UINT32_MAX;
  for (const auto& d : draws_) {
    if (d.glass || d.detached || d.is_blur_rim || d.cockpit_role >= 0) continue;
    const uint32_t klass = uint32_t(d.push.params[0] + 0.5f);
    if (klass == 4u || klass == 5u) continue;  // kInterior, kGauge
    const uint32_t flags = uint32_t(d.push.params[1] + 0.5f);
    if ((flags & kFlagMirror) != 0u) continue;
    // Clip to rear bodywork past mirror plane so side mirrors see rear haunch/fender/wing without mirror/cabin blockage.
    if (d.part_max[2] <= min_rear_z + 0.15f || d.centre[2] <= min_rear_z - 0.55f) continue;
    const std::string pn = lower(d.part_name);
    if (pn.rfind("mirror", 0) == 0 || pn.rfind("interior", 0) == 0 || pn.rfind("cockpit", 0) == 0 ||
        pn.rfind("dash", 0) == 0 || pn.rfind("seat", 0) == 0 || pn.rfind("steering", 0) == 0 ||
        pn.rfind("hood", 0) == 0 || pn.rfind("bumperf", 0) == 0) {
      continue;
    }
    ShowroomPush push = d.push;
    if (d.lamp_channel != drive::LampChannel::kNone) {
      const auto rgb = drive::evaluate_lamp_emission(d.lamp_channel, lamp_vectors_, lamp_colors_);
      if (rgb[0] > 1e-4f || rgb[1] > 1e-4f || rgb[2] > 1e-4f) {
        push.tint[0] = rgb[0];
        push.tint[1] = rgb[1];
        push.tint[2] = rgb[2];
        push.params[1] = float(flags | kFlagLampEmissive);
      }
    }
    if (d.descriptor != bound_desc) {
      bound_desc = d.descriptor;
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1,
                              &material_sets_[d.descriptor], 0, nullptr);
    }
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
  }
}


void Showroom::set_world_transform(const float transform[16]) {
  std::memcpy(world_transform_, transform, sizeof world_transform_);
  for (auto& d : world_draws_) std::memcpy(d.push.model, transform, sizeof d.push.model);
}

void Showroom::set_world_light(const float sun[3], const float up[3], float exposure) {
  for (int k = 0; k < 3; ++k) world_sun_[k] = sun[k], world_up_[k] = up[k];
  world_up_[3] = exposure;
  world_lit_ = true;
}


void Showroom::record_world(VkCommandBuffer cmd) {
  record_world_opaque(cmd);
  if (world_textured_ && sky_ready_) {
    record_sky_draw(cmd);
    r_.mark(cmd, "sky");
  }
  record_world_blended(cmd);
}

void Showroom::record_world_opaque(VkCommandBuffer cmd) {
  current_world_swap_ = 2.0f;
  if (world_draws_.empty() || material_sets_.empty()) return;
  world_bind_buffers(cmd, false);
  if (world_textured_) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 1, &frame_set_, 0, nullptr);
    world_stats_ = {};
    float swap = 2.0f;
    if (fading_on_) {
      const double t =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - fade_start_).count() / swap_fade_seconds_;
      if (t >= 1.0) drop_fading_world(false);
      else swap = float(std::max(t, 0.0));
    }
    current_world_swap_ = swap;
    if (arena_on() && gpu_world_->table_rows() > 0) {
      // The opaque and cut-out draws, culled on the GPU into indirect batches.
      WorldGpu::DrawParams dp;
      dp.frame_set = frame_set_;
      std::memcpy(dp.model, world_transform_, sizeof dp.model);
      for (int k = 0; k < 3; ++k) dp.sun[k] = world_sun_[k];
      for (int k = 0; k < 4; ++k) dp.up[k] = world_up_[k];
      dp.swap = swap;
      dp.bias_sign = r_.reverse_z() ? 1.0f : -1.0f;
      dp.decal_bias_constant = kDecalBiasConstant;
      dp.decal_bias_slope = kDecalBiasSlope;
      dp.depth_prepass = r_.z_prepass();
      dp.time_sec = tod_real_time_;
      dp.wind_speed = wind_speed_;
      dp.wind_amplitude = wind_amplitude_;
      gpu_world_->record_draw(cmd, dp);
      const size_t tested = gpu_world_->last_tested(), drawn = gpu_world_->last_drawn();
      const size_t trees_drawn = gpu_world_->last_trees_drawn();
      world_stats_.drawn += drawn + (trees_drawn > 0 ? 1 : 0);
      world_stats_.culled += tested > drawn ? tested - drawn : 0;
      world_stats_.indices += trees_drawn * 6;
      world_stats_.white += gpu_table_white_;
      world_stats_.layer_fallback += gpu_table_layer_fallback_;
      r_.mark(cmd, "world gpu");
    }
    record_world_textured(cmd, world_draws_, world_transform_, swap, false, 1);
    if (fading_on_) {
      const float dx = float(fading_.origin[0] - world_origin_[0]), dz = float(fading_.origin[1] - world_origin_[1]);
      float m[16];
      std::memcpy(m, world_transform_, sizeof m);
      for (int r = 0; r < 4; ++r) m[12 + r] += world_transform_[r] * dx + world_transform_[8 + r] * dz;
      world_bind_buffers(cmd, true);
      record_world_textured(cmd, fading_.draws, m, 3.0f + swap, true, 1);
    }
    return;
  }
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &frame_set_, 0, nullptr);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1, &material_sets_[0], 0,
                          nullptr);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque_);
  r_.apply_vrs(cmd);
  for (const auto& d : world_draws_) {
    ShowroomPush push = d.push;
    std::memcpy(push.model, world_transform_, sizeof push.model);
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, 0, 0);
  }
}

void Showroom::record_world_blended(VkCommandBuffer cmd) {
  if (!world_textured_ || material_sets_.empty()) return;
  const float swap = current_world_swap_;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 1, &frame_set_, 0, nullptr);
  if (!world_draws_.empty()) {
    world_bind_buffers(cmd, false);
    record_world_textured(cmd, world_draws_, world_transform_, swap, false, 2);
    if (fading_on_) {
      const float dx = float(fading_.origin[0] - world_origin_[0]), dz = float(fading_.origin[1] - world_origin_[1]);
      float m[16];
      std::memcpy(m, world_transform_, sizeof m);
      for (int r = 0; r < 4; ++r) m[12 + r] += world_transform_[r] * dx + world_transform_[8 + r] * dz;
      world_bind_buffers(cmd, true);
      record_world_textured(cmd, fading_.draws, m, 3.0f + swap, true, 2);
    }
  }
  if (overlay_count_ >= 3 && overlay_vertices_.buffer && world_alpha_ && world_white_set_) {
    const auto it_anchor = driving_line_anchors().find(this);
    const bool is_driving_line = it_anchor != driving_line_anchors().end() && it_anchor->second.active;
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &overlay_vertices_.buffer, &off);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_alpha_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 1, 1, &world_white_set_, 0,
                            nullptr);
    const float sign = r_.reverse_z() ? 1.0f : -1.0f;
    const float bias_scale = is_driving_line ? 1.5f : 1.0f;
    vkCmdSetDepthBias(cmd, sign * kDecalBiasConstant * bias_scale, 0.0f, sign * kDecalBiasSlope * bias_scale);
    ShowroomPush push{};
    std::memcpy(push.model, world_transform_, sizeof push.model);
    std::memcpy(push.uv0, world_sun_, sizeof push.uv0);
    push.uv0[3] = 2.0f;  // swap = none (world_frag.glsl reads draw.sun.w)
    std::memcpy(push.uv1, world_up_, sizeof push.uv1);
    push.tint[0] = push.tint[1] = push.tint[2] = push.tint[3] = 1.0f;
    push.params[0] = 2.0f;                           // kAlpha
    push.params[1] = 1024.0f;                        // kFlagUnlitVertexColor
    push.params[2] = 0.0f;                           // near world (must NOT be >0.5 or world_vert pushes >=550m as UberLOD)
    push.params[3] = is_driving_line ? 2.0f : 1.0f;  // 2.0 enables FH1 3D chevron driving-line shader
    vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ShowroomPush), &push);
    vkCmdDraw(cmd, overlay_count_, 1, 0, 0);
    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
  }
}

// The camera's eye in the scenery's frame (the transform is a rotation and
// a translation).
std::array<float, 3> Showroom::eye_in_scenery(const float transform[16]) const {
  const float e[3] = {frame_.eye[0] - transform[12], frame_.eye[1] - transform[13], frame_.eye[2] - transform[14]};
  std::array<float, 3> out{};
  for (int c = 0; c < 3; ++c) out[size_t(c)] = transform[c * 4 + 0] * e[0] + transform[c * 4 + 1] * e[1] + transform[c * 4 + 2] * e[2];
  return out;
}

// A cell of prop LOD00 copies wholly past the LOD band from the eye: every
// instance in it would be hidden (world.vert), so it is not drawn at all.
bool Showroom::near_lod_beyond(const WorldGeometry::Draw& d, const std::array<float, 3>& eye) const {
  if ((uint32_t(std::lround(d.push.params[1])) & 128u) == 0 || world_fade_[1] <= 0) return false;
  float d2 = 0;
  for (int i = 0; i < 3; ++i) {
    const float v = std::max({d.box_min[i] - eye[size_t(i)], 0.0f, eye[size_t(i)] - d.box_max[i]});
    d2 += v * v;
  }
  const float reach = world_fade_[0] + world_fade_[1] + kPropReach;
  return d2 > reach * reach;
}

void Showroom::record_world_textured(VkCommandBuffer cmd, const std::vector<WorldGeometry::Draw>& draws,
                                     const float transform[16], float swap, bool outgoing, int pass_mode) {
  {
    VkPipeline bound = VK_NULL_HANDLE;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    // The benchmark's split (Renderer::mark): the sky, then each blend group.
    static const char* const kGroup[] = {"world opaque", "world cut-outs", "world blended"};
    int group = outgoing ? 99 : -1;  // the outgoing scenery adds no marks
    // The view's side planes in the scenery's frame (clip = view_projection
    // x the scenery's transform, column-major): w + x, w - x, w + y, w - y.
    // A box wholly outside one of them is not drawn. The near and far
    // planes are left out (the far ring's depth push).
    float clip[16];
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r) {
        float s = 0;
        for (int k = 0; k < 4; ++k) s += frame_.view_projection[k * 4 + r] * transform[c * 4 + k];
        clip[c * 4 + r] = s;
      }
    float planes[4][4];
    for (int c = 0; c < 4; ++c) {
      const float x = clip[c * 4 + 0], y = clip[c * 4 + 1], w = clip[c * 4 + 3];
      planes[0][c] = w + x, planes[1][c] = w - x, planes[2][c] = w + y, planes[3][c] = w - y;
    }
    auto outside = [&](const WorldGeometry::Draw& d) {
      for (const auto& p : planes) {
        // The box's corner furthest along the plane's normal.
        const float x = p[0] >= 0 ? d.box_max[0] : d.box_min[0];
        const float y = p[1] >= 0 ? d.box_max[1] : d.box_min[1];
        const float z = p[2] >= 0 ? d.box_max[2] : d.box_min[2];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0) return true;
      }
      return false;
    };
    int bias = -1;  // the depth bias set: -1 none yet, else the decal flag
    const auto eye = eye_in_scenery(transform);
    for (const auto& d : draws) {
      if (pass_mode == 1 && d.blend >= 2) continue;
      if (pass_mode == 2 && d.blend < 2) continue;
      if (!outgoing && d.gpu) continue;  // drawn by the culling pass
      if (outgoing && !d.cull) continue;  // the sky: the incoming one's is drawn whole
      if (d.cull && ((world_cull_ && outside(d)) || near_lod_beyond(d, eye))) {
        ++world_stats_.culled;
        continue;
      }
      ++world_stats_.drawn;
      world_stats_.indices += d.index_count;
      const uint32_t draw_fl = uint32_t(std::lround(d.push.params[1]));
      const bool sky = draw_fl == 2;
      if (sky && sky_ready_) continue;
      if (group < 0 && !sky) {
        if (!sky_ready_) r_.mark(cmd, "sky");
        group = d.blend;
      }
      if (group >= 0 && group < 99 && d.blend != group) {
        r_.mark(cmd, kGroup[std::min(group, 2)]);
        group = d.blend;
      }
      // Additive (3) shares the blended pipeline: its fragments carry alpha 0.
      const bool needs_dither =
          (swap < 1.5f || swap > 2.5f) || ((draw_fl & (128u | 256u | 512u)) != 0u) || sky;
      const VkPipeline op_pipe = (needs_dither && world_opaque_dither_)
                                     ? world_opaque_dither_
                                     : ((r_.z_prepass() && world_opaque_after_z_) ? world_opaque_after_z_ : world_opaque_);
      const VkPipeline pipeline =
          d.blend >= 2 ? (((draw_fl & 32768u) != 0u && world_water_) ? world_water_ : world_alpha_)
                       : d.blend == 1 ? world_coverage_
                                      : op_pipe;
      if (pipeline != bound) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = pipeline);
        if (d.blend < 2) r_.apply_vrs(cmd);
        bias = -1;
      }
      // Decals and road paint: pulled towards the eye by a few float ulps
      // and the slope (ours; the sign follows the depth direction).
      if (int(d.decal) != bias) {
        bias = d.decal;
        const float sign = r_.reverse_z() ? 1.0f : -1.0f;
        vkCmdSetDepthBias(cmd, d.decal ? sign * kDecalBiasConstant : 0.0f, 0.0f, d.decal ? sign * kDecalBiasSlope : 0.0f);
      }
      VkDescriptorSet set = d.texture != kNoWorldTexture ? world_set(d.texture) : VK_NULL_HANDLE;
      if (set) {
        std::lock_guard<std::mutex> lock(world_textures_mutex_);
        world_lru_.touch(d.texture, world_frame_);
      }
      ShowroomPush push = d.push;
      const bool has_extra = d.texture_b != kNoWorldTexture || d.splat != kNoWorldTexture ||
                             d.light_map != kNoWorldTexture || d.normal_map != kNoWorldTexture ||
                             d.ao_map != kNoWorldTexture || d.layer_c != kNoWorldTexture ||
                             d.spec_map != kNoWorldTexture;
      if ((set || d.texture == kNoWorldTexture) && has_extra) {
        VkDescriptorSet layered = world_layer_set(
            {d.texture, d.texture_b, d.splat, d.light_map, d.normal_map, d.ao_map, d.layer_c, d.spec_map});
        if (layered) {
          set = layered;
          std::lock_guard<std::mutex> lock(world_textures_mutex_);
          if (d.texture_b != kNoWorldTexture) world_lru_.touch(d.texture_b, world_frame_);
          if (d.splat != kNoWorldTexture) world_lru_.touch(d.splat, world_frame_);
          if (d.light_map != kNoWorldTexture) world_lru_.touch(d.light_map, world_frame_);
          if (d.normal_map != kNoWorldTexture) world_lru_.touch(d.normal_map, world_frame_);
          if (d.ao_map != kNoWorldTexture) world_lru_.touch(d.ao_map, world_frame_);
          if (d.layer_c != kNoWorldTexture) world_lru_.touch(d.layer_c, world_frame_);
          if (d.spec_map != kNoWorldTexture) world_lru_.touch(d.spec_map, world_frame_);
        } else if (d.texture_b != kNoWorldTexture) {
          // A blend/extra layer did not upload: keep slot 0 and surface flags, drop extra-map flags.
          constexpr uint32_t kExtraTexMask = 4u | 8u | 16384u | 2048u | 4096u | 8192u | 524288u | 2097152u | 4194304u |
                                             8388608u;
          const uint32_t fl = uint32_t(std::lround(push.params[1])) & ~kExtraTexMask;
          push.params[1] = float(fl);
          ++world_stats_.layer_fallback;
        }
      }
      if (!set) {
        if (d.texture != kNoWorldTexture) ++world_stats_.white;
        set = world_white_set_;
        const uint32_t fl = uint32_t(std::lround(push.params[1]));
        if ((fl & 1u) != 0u) {
          // Texture missing or evicted: clear textured flags and replace UV tiling in push.tint
          // with a neutral road/terrain linear albedo instead of pure white (1, 1, 1).
          constexpr uint32_t kAnyTexMask = 1u | 4u | 8u | 16384u | 2048u | 4096u | 8192u | 524288u | 2097152u |
                                           4194304u | 8388608u;
          push.params[1] = float(fl & ~kAnyTexMask);
          if ((fl & 1048576u) != 0u) {
            push.tint[0] = 0.055f;
            push.tint[1] = 0.054f;
            push.tint[2] = 0.052f;
          } else {
            push.tint[0] = 0.13f;
            push.tint[1] = 0.11f;
            push.tint[2] = 0.085f;
          }
          push.tint[3] = 1.0f;
        }
      }
      if (set != bound_set)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 1, 1, &(bound_set = set),
                                0, nullptr);
      // Cut-outs without MSAA: world.frag's alpha test (flag 64).
      if (d.blend == 1 && r_.samples_ == VK_SAMPLE_COUNT_1_BIT)
        push.params[1] = float(uint32_t(std::lround(push.params[1])) | 64u);
      world_draw_model(d, transform, push.model);
      std::memcpy(push.uv0, world_sun_, sizeof push.uv0);
      push.uv0[3] = swap;  // world.frag's swap dither
      std::memcpy(push.uv1, world_up_, sizeof push.uv1);
      vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
    }
    if (group >= 0 && group < 99) r_.mark(cmd, kGroup[std::min(group, 2)]);
  }
}


}  // namespace carview
