#include "showroom_internal.h"

#include "fh1data/archive.h"
#include "fh1data/xds.h"
#include "glow_frag_spv.h"
#include "glow_vert_spv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace carview {

void Showroom::load_car_lamps(const std::filesystem::path& game_root, const std::string& car) {
  lamp_points_.clear();
  lamp_colors_ = drive::LampColorTable{};
  if (game_root.empty() || car.empty()) return;
  cars::Folder folder;
  if (!open_disc_car_folder(game_root, car, folder)) return;
  for (const auto& name : folder.names()) {
    const std::string ml = lower(name);
    if (ends_with(ml, "shadersettings.xml")) {
      std::vector<uint8_t> data;
      if (folder.read(name, data, nullptr)) {
        const std::string xml(reinterpret_cast<const char*>(data.data()), data.size());
        lamp_colors_.load_shader_settings_xml(xml);
      }
    } else if (ends_with(ml, ".carbin") && ml.find("_cockpit") == std::string::npos &&
               ml.find("_lod0") == std::string::npos && ml.find("_damage") == std::string::npos &&
               lamp_points_.empty()) {
      std::vector<uint8_t> data;
      if (folder.read(name, data, nullptr)) {
        auto pts = fh1::parse_carbin_lamp_points(data);
        if (!pts.empty()) lamp_points_ = std::move(pts);
      }
    }
  }
  if (lamp_points_.empty()) {
    for (const auto& name : folder.names()) {
      const std::string ml = lower(name);
      if (ends_with(ml, ".carbin")) {
        std::vector<uint8_t> data;
        if (folder.read(name, data, nullptr)) {
          auto pts = fh1::parse_carbin_lamp_points(data);
          if (!pts.empty()) {
            lamp_points_ = std::move(pts);
            break;
          }
        }
      }
    }
  }
}

void Showroom::update_frame_cookie_descriptor() {
  if (!r_.device_ || !headlight_cookie_.view) return;
  for (VkDescriptorSet target : {frame_set_, shadow_set_, shadow_set_mid_, shadow_set_far_, env_frame_set_}) {
    if (!target) continue;
    VkDescriptorImageInfo cookie{sampler_, headlight_cookie_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = target;
    w.dstBinding = 3;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &cookie;
    vkUpdateDescriptorSets(r_.device_, 1, &w, 0, nullptr);
  }
}

bool Showroom::upload_carlights(const std::filesystem::path& media_dir, std::string& error) {
  if (guest_ || !r_.device_) return true;
  if (carlights_ready_) return true;

  auto load_xds_rgba = [&](const std::filesystem::path& path, GpuTexture& out, bool fallback_radial) -> bool {
    bake::Texture tex;
    tex.format = bake::TextureFormat::kRgba8;
    std::vector<uint8_t> raw;
    fh1::RgbaImage img;
    bool decoded = false;
    if (std::filesystem::exists(path)) {
      if (FILE* f = std::fopen(path.string().c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        if (sz > 0) {
          raw.resize(size_t(sz));
          std::fseek(f, 0, SEEK_SET);
          if (std::fread(raw.data(), 1, raw.size(), f) == raw.size()) {
            auto xr = fh1::decode_xds(raw);
            if (!xr.levels.empty() && xr.levels[0].width > 0 && xr.levels[0].height > 0 &&
                xr.levels[0].rgba.size() == size_t(xr.levels[0].width) * xr.levels[0].height * 4) {
              img = std::move(xr.levels[0]);
              decoded = true;
            }
          }
        }
        std::fclose(f);
      }
    }
    if (decoded) {
      tex.levels.push_back({img.width, img.height, std::move(img.rgba)});
    } else if (fallback_radial) {
      constexpr uint32_t kS = 32;
      std::vector<uint8_t> px(kS * kS * 4);
      for (uint32_t y = 0; y < kS; ++y) {
        for (uint32_t x = 0; x < kS; ++x) {
          const float u = (float(x) + 0.5f) / float(kS) * 2.0f - 1.0f;
          const float v = (float(y) + 0.5f) / float(kS) * 2.0f - 1.0f;
          const float r = std::sqrt(u * u + v * v);
          const float a = std::pow(std::clamp(1.0f - r, 0.0f, 1.0f), 2.2f);
          const uint8_t b = uint8_t(std::lround(a * 255.0f));
          const size_t idx = (size_t(y) * kS + x) * 4;
          px[idx + 0] = b;
          px[idx + 1] = b;
          px[idx + 2] = b;
          px[idx + 3] = b;
        }
      }
      tex.levels.push_back({kS, kS, std::move(px)});
    } else {
      tex.levels.push_back({1, 1, {255, 255, 255, 255}});
    }
    return upload_texture(&tex, false, out, error);
  };

  const auto carlights = media_dir / "carlights";
  // Replace the 1x1 default headlight cookie with media/carlights/headlight_beam_dip.xds.
  GpuTexture cookie;
  if (load_xds_rgba(carlights / "headlight_beam_dip.xds", cookie, true)) {
    r_.wait_idle();
    r_.destroy_image(headlight_cookie_);
    headlight_cookie_ = cookie;
    update_frame_cookie_descriptor();
  }

  for (int i = 0; i < 7; ++i) {
    const std::string name = "headlight_glow_" + std::to_string(i + 1) + ".xds";
    if (!load_xds_rgba(carlights / name, glow_tex_[i], true)) return false;
  }

  if (!r_.create_buffer(kMaxGlowSprites * sizeof(GlowSpriteGpu),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, glow_buffer_)) {
    error = "glow instance buffer";
    return false;
  }

  // Descriptor set layout for glow_tex[7] (set 1, binding 0).
  const VkDescriptorSetLayoutBinding glow_binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7,
                                                  VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_ci.bindingCount = 1;
  set_ci.pBindings = &glow_binding;
  VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &set_ci, nullptr, &glow_set_layout_), "glow set layout");

  const VkDescriptorSetLayout pl_sets[2] = {frame_layout_, glow_set_layout_};
  VkPipelineLayoutCreateInfo pl_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl_ci.setLayoutCount = 2;
  pl_ci.pSetLayouts = pl_sets;
  VK_TRY(vkCreatePipelineLayout(r_.device_, &pl_ci, nullptr, &glow_pipe_layout_), "glow pipeline layout");

  const VkDescriptorPoolSize psize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7};
  VkDescriptorPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_ci.maxSets = 1;
  pool_ci.poolSizeCount = 1;
  pool_ci.pPoolSizes = &psize;
  VK_TRY(vkCreateDescriptorPool(r_.device_, &pool_ci, nullptr, &glow_pool_), "glow descriptor pool");

  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = glow_pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &glow_set_layout_;
  VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, &glow_set_), "glow descriptor set");

  VkDescriptorImageInfo imgs[7];
  for (int i = 0; i < 7; ++i)
    imgs[i] = {sampler_, glow_tex_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = glow_set_;
  write.dstBinding = 0;
  write.descriptorCount = 7;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = imgs;
  vkUpdateDescriptorSets(r_.device_, 1, &write, 0, nullptr);

  auto make_mod = [&](const uint32_t* code, size_t bytes, VkShaderModule& m) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    return vkCreateShaderModule(r_.device_, &ci, nullptr, &m);
  };
  VkShaderModule gv = VK_NULL_HANDLE, gf = VK_NULL_HANDLE;
  VK_TRY(make_mod(kGlow_vert_spv, sizeof(kGlow_vert_spv), gv), "glow.vert");
  VK_TRY(make_mod(kGlow_frag_spv, sizeof(kGlow_frag_spv), gf), "glow.frag");

  const VkBool32 linear_out = r_.post() ? VK_TRUE : VK_FALSE;
  const VkSpecializationMapEntry spec_lin{0, 0, sizeof(VkBool32)};
  const VkSpecializationInfo spec_frag{1, &spec_lin, sizeof(VkBool32), &linear_out};

  const VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, gv, "main",
       nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, gf, "main",
       &spec_frag}};

  VkVertexInputBindingDescription vbind{0, sizeof(GlowSpriteGpu), VK_VERTEX_INPUT_RATE_INSTANCE};
  const VkVertexInputAttributeDescription vattrs[4] = {
      {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GlowSpriteGpu, pos_radius)},
      {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GlowSpriteGpu, dir_inner)},
      {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GlowSpriteGpu, color_outer)},
      {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GlowSpriteGpu, extra)}};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &vbind;
  vi.vertexAttributeDescriptionCount = 4;
  vi.pVertexAttributeDescriptions = vattrs;

  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = r_.samples_;
  // Depth-tested against scene depth, depth write OFF (CAR_LAMPS.md §4.3).
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_FALSE;
  ds.depthCompareOp = r_.reverse_z() ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
  // Additive blend: SRC_ALPHA / ONE.
  VkPipelineColorBlendAttachmentState att{};
  att.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  att.blendEnable = VK_TRUE;
  att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
  att.colorBlendOp = VK_BLEND_OP_ADD;
  att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  att.alphaBlendOp = VK_BLEND_OP_ADD;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &att;
  const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_states;

  const VkFormat fmt = r_.scene_format();
  VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rend.colorAttachmentCount = 1;
  rend.pColorAttachmentFormats = &fmt;
  rend.depthAttachmentFormat = r_.depth_format_;

  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.pNext = &rend;
  gp.stageCount = 2;
  gp.pStages = stages;
  gp.pVertexInputState = &vi;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pDepthStencilState = &ds;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &dyn;
  gp.layout = glow_pipe_layout_;
  const VkResult res = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &gp, nullptr, &glow_pipe_);
  vkDestroyShaderModule(r_.device_, gv, nullptr);
  vkDestroyShaderModule(r_.device_, gf, nullptr);
  VK_TRY(res, "glow pipeline");

  carlights_ready_ = true;
  return true;
}

void Showroom::record_lights_before(VkCommandBuffer cmd) {
  auto transform_dir = [](const float m[16], const std::array<float, 3>& v) -> std::array<float, 3> {
    std::array<float, 3> r = {m[0] * v[0] + m[4] * v[1] + m[8] * v[2],
                              m[1] * v[0] + m[5] * v[1] + m[9] * v[2],
                              m[2] * v[0] + m[6] * v[1] + m[10] * v[2]};
    const float len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (len > 1e-6f) r[0] /= len, r[1] /= len, r[2] /= len;
    return r;
  };
  auto build_beam = [&](const float render_from_body[16], const std::vector<fh1::CarbinLampPoint>& pts,
                        const drive::LampShaderVectors& vecs, HeadlightBeamGpu& out) -> bool {
    const float head_amt = vecs.dynamic_lights_2[0];
    if (head_amt <= 0.01f) return false;
    const bool fullbeam = vecs.dynamic_lights_2[1] > 0.5f;
    const auto spec = drive::resolve_headlight_beam(pts, fullbeam);
    const auto origin = transform_point(render_from_body, spec.origin_body);
    const float pitch_rad = (fullbeam ? -2.2f : -6.2f) * (3.14159265f / 180.0f);
    const std::array<float, 3> fwd_b = {0.0f, std::sin(pitch_rad), std::cos(pitch_rad)};
    const std::array<float, 3> up_b = {0.0f, std::cos(pitch_rad), -std::sin(pitch_rad)};
    const std::array<float, 3> right_b = {1.0f, 0.0f, 0.0f};
    const auto fwd = transform_dir(render_from_body, fwd_b);
    const auto up = transform_dir(render_from_body, up_b);
    const auto right = transform_dir(render_from_body, right_b);
    const float tan_x = std::tan((fullbeam ? 36.0f : 42.0f) * (3.14159265f / 180.0f));
    const float tan_y = std::tan((fullbeam ? 18.0f : 19.0f) * (3.14159265f / 180.0f));
    const float dx = -(right[0] * origin[0] + right[1] * origin[1] + right[2] * origin[2]);
    const float dy = -(up[0] * origin[0] + up[1] * origin[1] + up[2] * origin[2]);
    const float dz = -(fwd[0] * origin[0] + fwd[1] * origin[1] + fwd[2] * origin[2]);
    // Column-major projection: c.x = x_cam / tan_x, c.y = -y_cam / tan_y, c.z = z_cam, c.w = z_cam.
    float* p = out.proj;
    p[0] = right[0] / tan_x;  p[4] = right[1] / tan_x;  p[8] = right[2] / tan_x;   p[12] = dx / tan_x;
    p[1] = -up[0] / tan_y;    p[5] = -up[1] / tan_y;    p[9] = -up[2] / tan_y;     p[13] = -dy / tan_y;
    p[2] = fwd[0];            p[6] = fwd[1];            p[10] = fwd[2];            p[14] = dz;
    p[3] = fwd[0];            p[7] = fwd[1];            p[11] = fwd[2];            p[15] = dz;
    out.origin_range[0] = origin[0];
    out.origin_range[1] = origin[1];
    out.origin_range[2] = origin[2];
    out.origin_range[3] = spec.range_m;
    out.dir_power[0] = fwd[0];
    out.dir_power[1] = fwd[1];
    out.dir_power[2] = fwd[2];
    out.dir_power[3] = (fullbeam ? 7.5f : 5.8f) * head_amt;
    out.color[0] = spec.color[0];
    out.color[1] = spec.color[1];
    out.color[2] = spec.color[2];
    out.color[3] = 1.0f;
    return true;
  };

  // .carbin lamp points live in car model space (same vertical origin as body mesh,
  // only Z-mirror/pitch rotation from body_to_model_ applies; no lift offset!).
  auto lamp_to_model = [](const float btm[16], float out[16]) {
    std::memcpy(out, btm, 16 * sizeof(float));
    out[12] = out[13] = out[14] = 0.0f;
  };
  HeadlightUbo hl{};
  const float night_factor = tod_active_ ? std::clamp(tod_state_.switch_on_lights, 0.0f, 1.0f) : 0.0f;
  int beam_count = 0;
  // Player headlight beam first (when car drawn and headlamps active).
  if (draw_car_ && world_lit_) {
    float ltm[16];
    lamp_to_model(body_to_model_, ltm);
    if (build_beam(ltm, lamp_points_, lamp_vectors_, hl.beams[beam_count])) ++beam_count;
  }
  // Nearest traffic / AI vehicles with headlamps on (up to 16 total beams).
  struct Cand {
    float dist2;
    const Instance* inst;
  };
  std::vector<Cand> cands;
  cands.reserve(instances_.size());
  for (const auto& in : instances_) {
    if (!in.car || !in.has_lamps || in.lamps.dynamic_lights_2[0] <= 0.01f) continue;
    const float dx = in.model[12] - frame_.eye[0];
    const float dy = in.model[13] - frame_.eye[1];
    const float dz = in.model[14] - frame_.eye[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 < 180.0f * 180.0f) cands.push_back({d2, &in});
  }
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dist2 < b.dist2; });
  for (const auto& c : cands) {
    if (beam_count >= 16) break;
    float ltm[16], rfb[16];
    lamp_to_model(c.inst->car->body_to_model_, ltm);
    multiply(c.inst->model, ltm, rfb);
    if (build_beam(rfb, c.inst->car->lamp_points_, c.inst->lamps, hl.beams[beam_count])) ++beam_count;
  }
  // Enable road beam projection at dusk/night, or softly in daylight if user forces fullbeam/lowbeam override.
  const float beam_gate = std::max(night_factor, lamp_vectors_.dynamic_lights_2[1] > 0.5f ? 0.35f : 0.0f);
  hl.count_night[0] = float(beam_count);
  hl.count_night[1] = beam_gate;
  hl.count_night[2] = tod_real_time_;
  hl.count_night[3] = 0.0f;
  // Mirror nearest up to 8 active headlight cones for Phase B 3D volumetric headlight scattering.
  vol_beam_count_ = uint32_t(std::min(beam_count, 8));
  vol_night_factor_ = beam_gate;
  for (uint32_t bi = 0; bi < vol_beam_count_; ++bi) {
    const auto& src_b = hl.beams[bi];
    auto& dst_b = vol_beams_[bi];
    for (int k = 0; k < 4; ++k) {
      dst_b.origin_range[k] = src_b.origin_range[k];
      dst_b.dir_power[k] = src_b.dir_power[k];
    }
    dst_b.color_cos[0] = src_b.color[0];
    dst_b.color_cos[1] = src_b.color[1];
    dst_b.color_cos[2] = src_b.color[2];
    dst_b.color_cos[3] = 0.72f;  // ~44 deg volumetric cone half-angle matching automotive low/high beam
  }

  {
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = headlights_ubo_.buffer;
    b.size = sizeof(HeadlightUbo);
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkCmdUpdateBuffer(cmd, headlights_ubo_.buffer, 0, sizeof(HeadlightUbo), &hl);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // Collect additive glow sprites (car .carbin lamp points + world streetlamp/outpost glows).
  glow_count_ = 0;
  if (!carlights_ready_ || !glow_buffer_.buffer) return;
  std::vector<GlowSpriteGpu> sprites;
  sprites.reserve(256);

  auto point_amt = [](const fh1::CarbinLampPoint& pt, const drive::LampShaderVectors& vecs) -> float {
    float a = 0.0f;
    if (pt.side_flags & 0x80u) a = 1.0f;
    if (pt.channel_mask & 0x01u) a = std::max(a, vecs.dynamic_lights_2[0]);
    if (pt.channel_mask & 0x02u) a = std::max(a, vecs.dynamic_lights_2[0] * 0.65f);
    if (pt.channel_mask & 0x04u) a = std::max(a, vecs.dynamic_lights_amount[0]);
    if (pt.channel_mask & 0x08u) {
      if (pt.side_flags & 0x01u) a = std::max(a, vecs.dynamic_lights_amount[2]);
      if (pt.side_flags & 0x02u) a = std::max(a, vecs.dynamic_lights_amount[3]);
    }
    if (pt.channel_mask & 0x10u) a = std::max(a, vecs.dynamic_lights_amount[1]);
    return a;
  };

  auto push_car_glows = [&](const float render_from_body[16], const std::vector<fh1::CarbinLampPoint>& pts,
                            const drive::LampShaderVectors& vecs, float max_dist) {
    for (const auto& pt : pts) {
      if (sprites.size() >= kMaxGlowSprites) break;
      const float amt = point_amt(pt, vecs);
      if (amt <= 0.01f) continue;
      const auto g = drive::resolve_lamp_glow(pt);
      const auto p = transform_point(render_from_body, pt.position);
      const auto d = transform_dir(render_from_body, pt.direction);
      const float inner_cos = std::cos(g.inner_cone_deg * (3.14159265f / 180.0f));
      const float outer_cos = std::cos(g.outer_cone_deg * (3.14159265f / 180.0f));
      // Unclamped HDR glow luminance (> 4.0) drives authentic FH1 crimson taillight & headlamp bloom.
      const float gain = (pt.channel_mask & 0x01u) ? 5.8f
                       : (pt.channel_mask & 0x04u) ? 6.2f
                       : (pt.channel_mask & 0x02u) ? 5.8f
                                                   : 2.5f;
      const float r_scale = (pt.channel_mask & 0x01u) ? 0.26f : 0.45f;
      GlowSpriteGpu s{};
      s.pos_radius[0] = p[0];
      s.pos_radius[1] = p[1];
      s.pos_radius[2] = p[2];
      s.pos_radius[3] = std::clamp(g.radius_m * r_scale, 0.08f, 0.45f);
      s.dir_inner[0] = d[0];
      s.dir_inner[1] = d[1];
      s.dir_inner[2] = d[2];
      s.dir_inner[3] = inner_cos;
      s.color_outer[0] = g.color[0] * gain * amt;
      s.color_outer[1] = g.color[1] * gain * amt;
      s.color_outer[2] = g.color[2] * gain * amt;
      s.color_outer[3] = outer_cos;
      s.extra[0] = float(std::clamp(g.texture_index - 1, 0, 6));
      s.extra[1] = 0.16f;  // bias toward camera (m) to clear lamp lens & bumper lip
      s.extra[2] = max_dist;
      s.extra[3] = 1.0f;
      sprites.push_back(s);
    }
  };

  if (draw_car_ && world_lit_) {
    float ltm[16];
    lamp_to_model(body_to_model_, ltm);
    push_car_glows(ltm, lamp_points_, lamp_vectors_, 250.0f);
  }
  for (const auto& in : instances_) {
    if (!in.car || !in.has_lamps || in.car->lamp_points_.empty()) continue;
    const float dx = in.model[12] - frame_.eye[0];
    const float dy = in.model[13] - frame_.eye[1];
    const float dz = in.model[14] - frame_.eye[2];
    if (dx * dx + dy * dy + dz * dz > 350.0f * 350.0f) continue;
    float ltm[16], rfb[16];
    lamp_to_model(in.car->body_to_model_, ltm);
    multiply(in.model, ltm, rfb);
    push_car_glows(rfb, in.car->lamp_points_, in.lamps, 350.0f);
  }

  // World streetlamp / outpost / festival glow sprites (gated by SwitchOnLights).
  if (world_textured_ && night_factor > 0.01f && !world_glows_.empty()) {
    struct WorldCand {
      float dist2;
      size_t idx;
      float fade;
      std::array<float, 3> pos;
    };
    std::vector<WorldCand> wc;
    wc.reserve(world_glows_.size());
    for (size_t i = 0; i < world_glows_.size(); ++i) {
      const auto& g = world_glows_[i];
      const float fade = std::clamp((night_factor - g.threshold) / 0.14f, 0.0f, 1.0f);
      if (fade <= 0.005f) continue;
      const auto p = transform_point(world_transform_, g.position);
      const float dx = p[0] - frame_.eye[0];
      const float dy = p[1] - frame_.eye[1];
      const float dz = p[2] - frame_.eye[2];
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (d2 > 420.0f * 420.0f) continue;
      wc.push_back({d2, i, fade, p});
    }
    std::sort(wc.begin(), wc.end(), [](const WorldCand& a, const WorldCand& b) { return a.dist2 < b.dist2; });
    for (const auto& c : wc) {
      if (sprites.size() >= kMaxGlowSprites) break;
      const auto& g = world_glows_[c.idx];
      GlowSpriteGpu s{};
      s.pos_radius[0] = c.pos[0];
      s.pos_radius[1] = c.pos[1];
      s.pos_radius[2] = c.pos[2];
      s.pos_radius[3] = g.radius;
      s.dir_inner[0] = 0.0f;
      s.dir_inner[1] = 0.0f;
      s.dir_inner[2] = 0.0f;
      s.dir_inner[3] = -1.0f;  // omnidirectional
      s.color_outer[0] = g.color[0] * 2.8f;
      s.color_outer[1] = g.color[1] * 2.8f;
      s.color_outer[2] = g.color[2] * 2.8f;
      s.color_outer[3] = -1.0f;
      s.extra[0] = 0.0f;  // headlight_glow_1.xds soft radial glow
      s.extra[1] = 0.25f;
      s.extra[2] = 420.0f;
      s.extra[3] = c.fade;
      sprites.push_back(s);
    }
  }

  glow_count_ = uint32_t(sprites.size());
  if (glow_count_ > 0) {
    const VkDeviceSize bytes = VkDeviceSize(glow_count_) * sizeof(GlowSpriteGpu);
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = glow_buffer_.buffer;
    b.size = bytes;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    // vkCmdUpdateBuffer limit is 65536 bytes (= 1024 * 64 = kMaxGlowSprites * sizeof(GlowSpriteGpu)).
    vkCmdUpdateBuffer(cmd, glow_buffer_.buffer, 0, bytes, sprites.data());
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
}

void Showroom::record_glows_draw(VkCommandBuffer cmd) {
  if (!carlights_ready_ || glow_count_ == 0 || !glow_pipe_) return;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glow_pipe_);
  const VkDescriptorSet sets[2] = {frame_set_, glow_set_};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glow_pipe_layout_, 0, 2, sets, 0, nullptr);
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &glow_buffer_.buffer, &zero);
  vkCmdDraw(cmd, 6, glow_count_, 0, 0);
}


}  // namespace carview
