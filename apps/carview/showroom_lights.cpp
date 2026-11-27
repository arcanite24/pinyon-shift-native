#include "showroom_internal.h"

#include "cars/visual_setup.h"
#include "fh1data/archive.h"
#include "fh1data/xds.h"
#include "glow_frag_spv.h"
#include "glow_vert_spv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

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
  auto upd = [&](VkDescriptorSet target) {
    if (!target) return;
    VkDescriptorImageInfo cookie{sampler_, headlight_cookie_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = target;
    w.dstBinding = 3;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &cookie;
    vkUpdateDescriptorSets(r_.device_, 1, &w, 0, nullptr);
  };
  for (uint32_t s = 0; s < Renderer::kFramesInFlight; ++s) {
    upd(frame_sets_dip_[s]);
    upd(shadow_sets_[s]);
    upd(shadow_sets_mid_[s]);
    upd(shadow_sets_far_[s]);
    for (uint32_t v = 0; v < kMaxEnvViews; ++v) upd(env_frame_sets_[s][v]);
  }
}

void Showroom::sync_fullbeam_frame_sets(VkImageView cube_view, VkSampler cube_sampler) {
  if (guest_ || !r_.device_ || !pool_ || !frame_layout_ || !headlight_cookie_full_.view) return;
  VkImageView use_cube = cube_view
                             ? cube_view
                             : (bound_dyn_env_view_
                                    ? bound_dyn_env_view_
                                    : ((dyn_env_bound_ && dyn_env_.view) ? dyn_env_.view : environment_.view));
  VkSampler use_sampler = cube_sampler ? cube_sampler : sampler_;
  const VkImageView mirror_view =
      (dyn_env_ready_ && mirror_tex_.view) ? mirror_tex_.view : gpu_textures_[1].view;
  VkDescriptorSetLayout layouts[Renderer::kFramesInFlight];
  for (uint32_t s = 0; s < Renderer::kFramesInFlight; ++s) layouts[s] = frame_layout_;
  VkDescriptorSet fresh[Renderer::kFramesInFlight] = {};
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = Renderer::kFramesInFlight;
  ai.pSetLayouts = layouts;
  if (vkAllocateDescriptorSets(r_.device_, &ai, fresh) != VK_SUCCESS) return;
  for (uint32_t s = 0; s < Renderer::kFramesInFlight; ++s) {
    write_frame_set(fresh[s], s, frame_buffer_.buf.buffer, frame_buffer_.offset(s), use_cube, use_sampler,
                    headlight_cookie_full_.view, mirror_view);
    frame_sets_fullbeam_[s] = fresh[s];
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
  // R3-REN-10: Also load media/carlights/headlight_beam_full.xds for high-beam projection when present.
  {
    const auto full_path = carlights / "headlight_beam_full.xds";
    std::error_code fec;
    if (std::filesystem::exists(full_path, fec)) {
      std::vector<uint8_t> raw;
      if (FILE* f = std::fopen(full_path.string().c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        if (sz > 0) {
          raw.resize(size_t(sz));
          std::fseek(f, 0, SEEK_SET);
          if (std::fread(raw.data(), 1, raw.size(), f) == raw.size()) {
            auto xr = fh1::decode_xds(raw);
            if (!xr.levels.empty() && xr.levels[0].width > 0 && xr.levels[0].height > 0 &&
                xr.levels[0].rgba.size() == size_t(xr.levels[0].width) * xr.levels[0].height * 4) {
              bake::Texture tex;
              tex.format = bake::TextureFormat::kRgba8;
              tex.levels.push_back({xr.levels[0].width, xr.levels[0].height, std::move(xr.levels[0].rgba)});
              GpuTexture full_cookie;
              std::string full_err;
              if (upload_texture(&tex, false, full_cookie, full_err)) {
                headlight_cookie_full_ = full_cookie;
                Renderer* rptr = &r_;
                GpuTexture doomed_full = headlight_cookie_full_;
                retired_.push_back({std::numeric_limits<uint64_t>::max(), [rptr, doomed_full]() mutable {
                                      rptr->destroy_image(doomed_full);
                                    }});
                sync_fullbeam_frame_sets(VK_NULL_HANDLE, VK_NULL_HANDLE);
              }
            }
          }
        }
        std::fclose(f);
      }
    }
  }

  for (int i = 0; i < 7; ++i) {
    const std::string name = "headlight_glow_" + std::to_string(i + 1) + ".xds";
    if (!load_xds_rgba(carlights / name, glow_tex_[i], true)) return false;
  }

  if (!create_dyn_buffer(glow_buf_, kMaxGlowSprites * sizeof(GlowSpriteGpu), error)) {
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
  const bool has_full_cookie = headlight_cookie_full_.view != VK_NULL_HANDLE;
  const bool player_fullbeam = draw_car_ && world_lit_ && (lamp_vectors_.dynamic_lights_2[1] > 0.5f);
  const uint32_t slot = r_.frame_slot();
  if (frame_sets_dip_[slot]) {
    frame_set_ = (player_fullbeam && frame_sets_fullbeam_[slot]) ? frame_sets_fullbeam_[slot] : frame_sets_dip_[slot];
  }
  auto build_beam_at = [&](const float render_from_body[16], const std::array<float, 3>& origin_body,
                           const std::array<float, 3>& color, float head_amt, bool fullbeam, float yaw_deg,
                           float power_scale, HeadlightBeamGpu& out) -> bool {
    if (head_amt <= 0.01f || power_scale <= 0.005f) return false;
    const auto origin = transform_point(render_from_body, origin_body);
    const float pitch_rad = (fullbeam ? -1.8f : -6.0f) * (3.14159265f / 180.0f);
    const float yaw_rad = yaw_deg * (3.14159265f / 180.0f);
    const float cp = std::cos(pitch_rad), sp = std::sin(pitch_rad);
    const float cy = std::cos(yaw_rad), sy = std::sin(yaw_rad);
    const std::array<float, 3> fwd_b = {sy * cp, sp, cy * cp};
    const std::array<float, 3> right_b = {cy, 0.0f, -sy};
    const std::array<float, 3> up_b = {-sy * sp, cp, -cy * sp};
    const auto fwd = transform_dir(render_from_body, fwd_b);
    const auto up = transform_dir(render_from_body, up_b);
    const auto right = transform_dir(render_from_body, right_b);
    // High beam extends reach and widens spread (extra wide/long fallback when headlight_beam_full.xds is absent).
    const float half_fov_x_deg = fullbeam ? (has_full_cookie ? 40.0f : 46.0f) : 40.0f;
    const float half_fov_y_deg = fullbeam ? (has_full_cookie ? 19.5f : 21.5f) : 18.5f;
    const float tan_x = std::tan(half_fov_x_deg * (3.14159265f / 180.0f));
    const float tan_y = std::tan(half_fov_y_deg * (3.14159265f / 180.0f));
    const float dx = -(right[0] * origin[0] + right[1] * origin[1] + right[2] * origin[2]);
    const float dy = -(up[0] * origin[0] + up[1] * origin[1] + up[2] * origin[2]);
    const float dz = -(fwd[0] * origin[0] + fwd[1] * origin[1] + fwd[2] * origin[2]);
    // Column-major projection: c.x = x_cam / tan_x, c.y = -y_cam / tan_y, c.z = z_cam, c.w = z_cam.
    float* p = out.proj;
    p[0] = right[0] / tan_x;  p[4] = right[1] / tan_x;  p[8] = right[2] / tan_x;   p[12] = dx / tan_x;
    p[1] = -up[0] / tan_y;    p[5] = -up[1] / tan_y;    p[9] = -up[2] / tan_y;     p[13] = -dy / tan_y;
    p[2] = fwd[0];            p[6] = fwd[1];            p[10] = fwd[2];            p[14] = dz;
    p[3] = fwd[0];            p[7] = fwd[1];            p[11] = fwd[2];            p[15] = dz;
    const float range_m = fullbeam ? (has_full_cookie ? 105.0f : 115.0f) : 58.0f;
    out.origin_range[0] = origin[0];
    out.origin_range[1] = origin[1];
    out.origin_range[2] = origin[2];
    out.origin_range[3] = range_m;
    out.dir_power[0] = fwd[0];
    out.dir_power[1] = fwd[1];
    out.dir_power[2] = fwd[2];
    out.dir_power[3] = (fullbeam ? 7.8f : 5.8f) * head_amt * power_scale;
    out.color[0] = color[0];
    out.color[1] = color[1];
    out.color[2] = color[2];
    out.color[3] = 1.0f;
    return true;
  };
  auto build_beam = [&](const float render_from_body[16], const std::vector<fh1::CarbinLampPoint>& pts,
                        const drive::LampShaderVectors& vecs, HeadlightBeamGpu& out) -> bool {
    const float head_amt = vecs.dynamic_lights_2[0];
    if (head_amt <= 0.01f) return false;
    const bool fullbeam = vecs.dynamic_lights_2[1] > 0.5f;
    const auto spec = drive::resolve_headlight_beam(pts, fullbeam);
    return build_beam_at(render_from_body, spec.origin_body, spec.color, head_amt, fullbeam, 0.0f, 1.0f, out);
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
  // R3-REN-10: Player dual Left + Right headlamp projectors gated independently by
  // damage_corners_[0] (FL) and damage_corners_[1] (FR) so smashing one front corner
  // kills only that side's beam.
  if (draw_car_ && world_lit_ && lamp_vectors_.dynamic_lights_2[0] > 0.01f) {
    float ltm[16];
    lamp_to_model(body_to_model_, ltm);
    const float head_amt = lamp_vectors_.dynamic_lights_2[0];
    const auto spec = drive::resolve_headlight_beam(lamp_points_, player_fullbeam);
    std::vector<float> pts_xyz;
    std::vector<uint8_t> pts_mask;
    pts_xyz.reserve(lamp_points_.size() * 3u);
    pts_mask.reserve(lamp_points_.size());
    for (const auto& pt : lamp_points_) {
      pts_xyz.push_back(pt.position[0]);
      pts_xyz.push_back(pt.position[1]);
      pts_xyz.push_back(pt.position[2]);
      pts_mask.push_back(pt.channel_mask);
    }
    const auto dual =
        resolve_dual_headlight_origins(pts_xyz.data(), pts_mask.data(), pts_mask.size(), spec.origin_body);
    const bool has_corner_dmg = (damage_corners_[0] > 0.001f || damage_corners_[1] > 0.001f);
    const float legacy_front_dmg = damage_zones_[0] * damage_dirt_[3];
    const float fl_mult =
        eval_headlight_corner_damage(damage_corners_[0], legacy_front_dmg, has_corner_dmg, tod_real_time_, 0.0f);
    const float fr_mult =
        eval_headlight_corner_damage(damage_corners_[1], legacy_front_dmg, has_corner_dmg, tod_real_time_, 1.73f);
    // Calibrate dual overlapping beams (~0.64x each) so center road illumination stays natural
    // while distinct L/R pools separate crisply on close walls and vehicles.
    if (fl_mult > 0.005f && beam_count < 16 &&
        build_beam_at(ltm, dual.left, spec.color, head_amt, player_fullbeam, -1.5f, 0.64f * fl_mult,
                      hl.beams[beam_count])) {
      ++beam_count;
    }
    if (fr_mult > 0.005f && beam_count < 16 &&
        build_beam_at(ltm, dual.right, spec.color, head_amt, player_fullbeam, +1.5f, 0.64f * fr_mult,
                      hl.beams[beam_count])) {
      ++beam_count;
    }
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

  if (headlights_ubo_.ready()) {
    std::memcpy(headlights_ubo_.ptr(slot), &hl, sizeof(HeadlightUbo));
  }

  // R3-REN-10 & R3-REN-19: Populate dynamic car point lights (player brake/reverse/backfire spill
  // + top-N nearest active night streetlamp/tunnel fixtures) and front/rear brake rotor heat.
  std::memset(frame_.dyn_light_pos, 0, sizeof(frame_.dyn_light_pos));
  std::memset(frame_.dyn_light_col, 0, sizeof(frame_.dyn_light_col));
  frame_.dyn_light_meta[0] = 0.0f;
  frame_.dyn_light_meta[1] = rotor_glow_front_;
  frame_.dyn_light_meta[2] = rotor_glow_rear_;
  frame_.dyn_light_meta[3] = 0.0f;
  int dyn_light_count = 0;
  auto push_dyn_light = [&](float px, float py, float pz, float radius_m, float r, float g, float b, float spec_scale) {
    if (dyn_light_count >= 6) return;
    frame_.dyn_light_pos[dyn_light_count][0] = px;
    frame_.dyn_light_pos[dyn_light_count][1] = py;
    frame_.dyn_light_pos[dyn_light_count][2] = pz;
    frame_.dyn_light_pos[dyn_light_count][3] = radius_m;
    frame_.dyn_light_col[dyn_light_count][0] = r;
    frame_.dyn_light_col[dyn_light_count][1] = g;
    frame_.dyn_light_col[dyn_light_count][2] = b;
    frame_.dyn_light_col[dyn_light_count][3] = spec_scale;
    ++dyn_light_count;
  };
  if (world_lit_) {
    const float rear_z = have_bounds_ ? bounds_max_[2] : 2.15f;
    // 1) Exhaust backfire flash point light detected from near-car hot additive flame particles.
    if (draw_car_ && particle_add_count_ > 0 && particle_buf_.ready()) {
      const auto* pdata = reinterpret_cast<const ParticleGpu*>(particle_buf_.cpu.data());
      const uint32_t begin_i = std::min(particle_alpha_count_, kMaxParticleGpu);
      const uint32_t end_i = std::min(particle_alpha_count_ + particle_add_count_, kMaxParticleGpu);
      double sx = 0.0, sy = 0.0, sz = 0.0, wsum = 0.0;
      float max_e = 0.0f;
      for (uint32_t pi = begin_i; pi < end_i; ++pi) {
        const auto& sp = pdata[pi];
        const float px = sp.pos_sx[0], py = sp.pos_sx[1], pz = sp.pos_sx[2];
        const float d2 = px * px + py * py + pz * pz;
        if (d2 > 16.0f || pz < 0.4f) continue;  // exhaust sits behind car center (+Z in model space)
        const float alpha = sp.color[3];
        if (alpha < 0.05f || sp.color[0] < 0.35f) continue;
        const float e = std::min(alpha * (sp.color[0] + sp.color[1] * 0.5f), 2.5f);
        sx += double(px) * e;
        sy += double(py) * e;
        sz += double(pz) * e;
        wsum += double(e);
        max_e = std::max(max_e, e);
      }
      if (wsum > 0.08 && max_e > 0.12f) {
        const float cx = float(sx / wsum);
        const float cy = float(sy / wsum);
        const float cz = float(sz / wsum);
        const float inten = std::clamp(max_e * 1.65f, 0.35f, 2.8f) * (0.65f + 0.55f * night_factor);
        push_dyn_light(cx, cy + 0.12f, cz + 0.18f, 6.5f, 3.6f * inten, 1.35f * inten, 0.28f * inten, 1.65f);
      }
    }
    // 2) Active player brake lamp spill light (rear bumper, spoiler, trunk, close trailing rivals).
    if (draw_car_ && lamp_vectors_.dynamic_lights_amount[0] > 0.05f &&
        (damage_corners_[2] < 0.45f || damage_corners_[3] < 0.45f)) {
      const float brk = std::clamp(lamp_vectors_.dynamic_lights_amount[0], 0.0f, 1.0f);
      const float gain = brk * (0.40f + 0.65f * night_factor);
      push_dyn_light(0.0f, 0.78f, rear_z + 0.38f, 6.8f, 2.5f * gain, 0.14f * gain, 0.05f * gain, 1.15f);
    }
    // 3) Active player reverse lamp spill light.
    if (draw_car_ && lamp_vectors_.dynamic_lights_amount[1] > 0.05f) {
      const float rev = std::clamp(lamp_vectors_.dynamic_lights_amount[1], 0.0f, 1.0f);
      const float gain = rev * (0.35f + 0.65f * night_factor);
      push_dyn_light(0.0f, 0.72f, rear_z + 0.35f, 5.5f, 1.55f * gain, 1.50f * gain, 1.38f * gain, 1.0f);
    }
    frame_.dyn_light_meta[0] = float(dyn_light_count);
  }

  // Collect additive glow sprites (car .carbin lamp points + world streetlamp/outpost glows).
  glow_count_ = 0;
  if (!carlights_ready_ || !glow_buf_.ready()) return;
  std::vector<GlowSpriteGpu> sprites;
  sprites.reserve(256);

  auto push_car_glows = [&](const float render_from_body[16], const std::vector<fh1::CarbinLampPoint>& pts,
                            const drive::LampShaderVectors& vecs, float max_dist, bool check_player_damage) {
    bool has_dedicated_brake_pts = false;
    for (const auto& pt : pts) {
      if ((pt.channel_mask & 0x04u) != 0u) {
        has_dedicated_brake_pts = true;
        break;
      }
    }
    const float head_amt = vecs.dynamic_lights_2[0];
    const float brake_amt = vecs.dynamic_lights_amount[0];
    const float rev_amt = vecs.dynamic_lights_amount[1];
    const float ind_l_amt = vecs.dynamic_lights_amount[2];
    const float ind_r_amt = vecs.dynamic_lights_amount[3];
    for (const auto& pt : pts) {
      if (sprites.size() >= kMaxGlowSprites) break;
      if (check_player_damage) {
        // Body frame: +Z front, -Z rear, -X left, +X right.
        const bool is_front = (pt.position[2] >= 0.0f);
        const bool is_left = (pt.position[0] < 0.0f);
        const float corner_val =
            is_front ? (is_left ? damage_corners_[0] : damage_corners_[1])
                     : (is_left ? damage_corners_[2] : damage_corners_[3]);
        if (corner_val >= 0.45f) continue;
        if (corner_val <= 0.001f && damage_dirt_[3] > 0.01f) {
          const float z_dmg = is_front ? damage_zones_[0] : damage_zones_[1];
          const float x_dmg = is_left ? damage_zones_[2] : damage_zones_[3];
          const float corner_dmg = (z_dmg * 0.75f + x_dmg * 0.30f) * damage_dirt_[3];
          if (corner_dmg > 0.65f) continue;
        }
      }
      const auto g = drive::resolve_lamp_glow(pt);
      const auto p = transform_point(render_from_body, pt.position);
      const float dx = p[0] - frame_.eye[0];
      const float dy = p[1] - frame_.eye[1];
      const float dz = p[2] - frame_.eye[2];
      const float dist_to_eye = std::sqrt(dx * dx + dy * dy + dz * dz);
      const auto eval = cars::evaluate_car_lamp_glow_sprite(
          pt, g.radius_m, head_amt, brake_amt, rev_amt, ind_l_amt, ind_r_amt, has_dedicated_brake_pts,
          check_player_damage, dist_to_eye, night_factor);
      if (!eval.emit || eval.intensity <= 0.005f || eval.radius_m <= 0.001f) continue;
      const auto d = transform_dir(render_from_body, pt.direction);
      const float inner_cos = std::cos(g.inner_cone_deg * (3.14159265f / 180.0f));
      const float outer_cos = std::cos(g.outer_cone_deg * (3.14159265f / 180.0f));
      GlowSpriteGpu s{};
      s.pos_radius[0] = p[0];
      s.pos_radius[1] = p[1];
      s.pos_radius[2] = p[2];
      s.pos_radius[3] = eval.radius_m;
      s.dir_inner[0] = d[0];
      s.dir_inner[1] = d[1];
      s.dir_inner[2] = d[2];
      s.dir_inner[3] = inner_cos;
      s.color_outer[0] = g.color[0] * eval.intensity;
      s.color_outer[1] = g.color[1] * eval.intensity;
      s.color_outer[2] = g.color[2] * eval.intensity;
      s.color_outer[3] = outer_cos;
      s.extra[0] = float(std::clamp(g.texture_index - 1, 0, 6));
      s.extra[1] = eval.view_bias_m;
      s.extra[2] = max_dist;
      s.extra[3] = 1.0f;
      sprites.push_back(s);
    }
  };

  if (draw_car_ && world_lit_) {
    float ltm[16];
    lamp_to_model(body_to_model_, ltm);
    push_car_glows(ltm, lamp_points_, lamp_vectors_, 250.0f, true);
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
    push_car_glows(rfb, in.car->lamp_points_, in.lamps, 350.0f, false);
  }

  // World streetlamp / outpost / festival glow sprites (gated cleanly by dusk/night ToD,
  // validated once per world swap against actual loaded prop draw AABBs & terrain elevation
  // so no detached or culled white orbs ever float in the sky).
  const bool daytime_sky = tod_active_ && !tod_state_.is_moon && tod_state_.sun_dir[1] > 0.16f;
  if (world_textured_ && !daytime_sky && night_factor > 0.14f && !world_glows_.empty()) {
    struct ValidatedWorldGlow {
      std::array<float, 3> position{};  // scenery space
      std::array<float, 3> color{};
      float radius = 0.45f;
      float threshold = 0.35f;
    };
    struct ValidatedGlowCache {
      const Showroom* owner = nullptr;
      uint64_t world_gen = 0;
      double origin_x = 0.0;
      double origin_z = 0.0;
      size_t raw_count = 0;
      size_t draw_count = 0;
      bool has_ground = false;
      std::array<float, 3> first_pos{};
      std::vector<ValidatedWorldGlow> glows;
    };
    static ValidatedGlowCache s_wg_cache;

    const drive::Ground* active_ground = world_ground_ ? world_ground_ : world::AnimatedWorldSystem::last_ground();
    const bool has_ground = (active_ground != nullptr);
    const bool cache_hit = s_wg_cache.owner == this && s_wg_cache.world_gen == world_generation_ &&
                           s_wg_cache.origin_x == world_origin_[0] && s_wg_cache.origin_z == world_origin_[1] &&
                           s_wg_cache.raw_count == world_glows_.size() &&
                           s_wg_cache.draw_count == world_draws_.size() &&
                           s_wg_cache.has_ground == has_ground &&
                           s_wg_cache.first_pos == world_glows_[0].position;
    if (!cache_hit) {
      s_wg_cache.owner = this;
      s_wg_cache.world_gen = world_generation_;
      s_wg_cache.origin_x = world_origin_[0];
      s_wg_cache.origin_z = world_origin_[1];
      s_wg_cache.raw_count = world_glows_.size();
      s_wg_cache.draw_count = world_draws_.size();
      s_wg_cache.has_ground = has_ground;
      s_wg_cache.first_pos = world_glows_[0].position;
      s_wg_cache.glows.clear();
      s_wg_cache.glows.reserve(world_glows_.size());

      // Collect visible prop draw AABBs (exclude huge far-ring/terrain slabs) once for fast fixture anchoring.
      struct PropBox {
        float bmin[3];
        float bmax[3];
        bool compact;
      };
      std::vector<PropBox> prop_boxes;
      prop_boxes.reserve(world_draws_.size());
      for (const auto& d : world_draws_) {
        if (d.index_count == 0 || !d.cull) continue;
        const float ex = d.box_max[0] - d.box_min[0];
        const float ey = d.box_max[1] - d.box_min[1];
        const float ez = d.box_max[2] - d.box_min[2];
        if (ex > 320.0f || ez > 320.0f || ey > 35.0f || ey < 0.25f) continue;
        const bool compact = (ex <= 24.0f && ez <= 24.0f);
        prop_boxes.push_back({{d.box_min[0], d.box_min[1], d.box_min[2]},
                              {d.box_max[0], d.box_max[1], d.box_max[2]},
                              compact});
      }

      for (const auto& g : world_glows_) {
        std::array<float, 3> pos = g.position;
        if (!prop_boxes.empty()) {
          bool anchored = false;
          float best_below_top = -1e9f;
          for (const auto& pb : prop_boxes) {
            if (pos[0] < pb.bmin[0] - 1.5f || pos[0] > pb.bmax[0] + 1.5f ||
                pos[2] < pb.bmin[2] - 1.5f || pos[2] > pb.bmax[2] + 1.5f) {
              continue;
            }
            if (pos[1] >= pb.bmin[1] - 0.5f && pos[1] <= pb.bmax[1] + 0.8f) {
              // Seat glow cleanly inside compact fixture tops rather than floating above the housing.
              if (pb.compact && pos[1] > pb.bmax[1] - 0.18f) pos[1] = pb.bmax[1] - 0.18f;
              anchored = true;
              break;
            }
            if (pb.compact && pb.bmax[1] < pos[1] && pos[1] - pb.bmax[1] <= 6.0f && pb.bmax[1] > best_below_top) {
              best_below_top = pb.bmax[1];
            }
          }
          if (!anchored) {
            if (best_below_top > -1e8f) {
              pos[1] = best_below_top - 0.18f;
              anchored = true;
            } else {
              // Skip orphaned glow sprites with no matching prop fixture geometry.
              continue;
            }
          }
        }

        // Validate height above terrain so sky-pointing searchlight beam tips or floating boxes are dropped.
        if (active_ground) {
          drive::Hit h;
          const double gx = world_origin_[0] + double(pos[0]);
          const double gz = world_origin_[1] + double(pos[2]);
          const double start_y = double(pos[1]) + 2.0;
          if (active_ground->raycast({gx, start_y, gz}, {0.0, -1.0, 0.0}, 45.0, h)) {
            const float gy = float(start_y - h.distance);
            const float h_above = pos[1] - gy;
            if (h_above > 17.5f || h_above < 1.0f) continue;
          }
        }

        const bool is_floodlight = (g.color[2] > 0.70f);
        ValidatedWorldGlow vg{};
        vg.position = pos;
        if (is_floodlight) {
          // Warm metal-halide festival/outpost fixture tint (never sterile cyan-white UFO orb).
          vg.color = {1.0f * 0.95f, 0.92f * 0.95f, 0.76f * 0.95f};
          vg.radius = std::clamp(g.radius * 0.36f, 0.28f, 0.65f);
        } else {
          // Warm sodium-amber streetlamp halo.
          vg.color = {g.color[0] * 1.15f, g.color[1] * 1.15f, g.color[2] * 1.15f};
          vg.radius = std::clamp(g.radius * 0.42f, 0.28f, 0.56f);
        }
        vg.threshold = 0.16f + std::clamp(g.threshold, 0.0f, 1.0f) * 0.69f;
        s_wg_cache.glows.push_back(vg);
      }
    }

    // Sync world glow range to prop edge fade-out distance so glows never outlive culled poles.
    const float max_glow_dist =
        (world_fade_[3] > 30.0f) ? std::min(300.0f, world_fade_[3] - 10.0f) : 240.0f;
    const float fade_start_dist =
        (world_fade_[2] > 20.0f && world_fade_[2] < max_glow_dist) ? world_fade_[2] : (max_glow_dist * 0.65f);
    const float max_glow_d2 = max_glow_dist * max_glow_dist;
    const float dusk_env = std::clamp((night_factor - 0.14f) / 0.34f, 0.0f, 1.0f);

    struct WorldCand {
      float dist2;
      size_t idx;
      float fade;
      std::array<float, 3> pos;
    };
    std::vector<WorldCand> wc;
    wc.reserve(s_wg_cache.glows.size());
    for (size_t i = 0; i < s_wg_cache.glows.size(); ++i) {
      const auto& g = s_wg_cache.glows[i];
      const float tod_fade = dusk_env * std::clamp((night_factor - g.threshold) / 0.16f, 0.0f, 1.0f);
      if (tod_fade <= 0.005f) continue;
      const auto p = transform_point(world_transform_, g.position);
      const float dx = p[0] - frame_.eye[0];
      const float dy = p[1] - frame_.eye[1];
      const float dz = p[2] - frame_.eye[2];
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (d2 > max_glow_d2) continue;
      const float dist = std::sqrt(d2);
      const float t_edge = std::clamp((dist - fade_start_dist) / std::max(max_glow_dist - fade_start_dist, 1.0f), 0.0f, 1.0f);
      const float dist_fade = 1.0f - (t_edge * t_edge * (3.0f - 2.0f * t_edge));
      const float fade = tod_fade * dist_fade;
      if (fade <= 0.005f) continue;
      wc.push_back({d2, i, fade, p});
    }
    std::sort(wc.begin(), wc.end(), [](const WorldCand& a, const WorldCand& b) { return a.dist2 < b.dist2; });
    // R3-REN-10: Feed top-N nearest active night streetlamp / tunnel lamp fixtures to showroom.frag
    // so warm sodium/metal-halide highlights glide smoothly across car paint & clearcoat at night.
    if (world_lit_ && dyn_light_count < 6 && !wc.empty()) {
      const float ref_x = draw_car_ ? 0.0f : frame_.eye[0];
      const float ref_y = draw_car_ ? 0.85f : frame_.eye[1];
      const float ref_z = draw_car_ ? 0.0f : frame_.eye[2];
      struct CarLightCand {
        float car_d2;
        size_t wc_idx;
      };
      std::vector<CarLightCand> car_lights;
      car_lights.reserve(std::min<size_t>(wc.size(), 64u));
      for (size_t wi = 0; wi < wc.size(); ++wi) {
        const auto& c = wc[wi];
        const float cdx = c.pos[0] - ref_x;
        const float cdy = c.pos[1] - ref_y;
        const float cdz = c.pos[2] - ref_z;
        const float cd2 = cdx * cdx + cdy * cdy + cdz * cdz;
        if (cd2 <= 42.0f * 42.0f) car_lights.push_back({cd2, wi});
      }
      std::sort(car_lights.begin(), car_lights.end(),
                [](const CarLightCand& a, const CarLightCand& b) { return a.car_d2 < b.car_d2; });
      for (const auto& cl : car_lights) {
        if (dyn_light_count >= 6) break;
        const auto& c = wc[cl.wc_idx];
        const auto& g = s_wg_cache.glows[c.idx];
        const float radius_m = std::clamp(g.radius * 36.0f, 16.0f, 34.0f);
        const float gain = c.fade * 2.65f;
        push_dyn_light(c.pos[0], c.pos[1], c.pos[2], radius_m, g.color[0] * gain, g.color[1] * gain,
                       g.color[2] * gain, 1.45f);
      }
      frame_.dyn_light_meta[0] = float(dyn_light_count);
    }
    for (const auto& c : wc) {
      if (sprites.size() >= kMaxGlowSprites) break;
      const auto& g = s_wg_cache.glows[c.idx];
      GlowSpriteGpu s{};
      s.pos_radius[0] = c.pos[0];
      s.pos_radius[1] = c.pos[1];
      s.pos_radius[2] = c.pos[2];
      s.pos_radius[3] = g.radius;
      s.dir_inner[0] = 0.0f;
      s.dir_inner[1] = 0.0f;
      s.dir_inner[2] = 0.0f;
      s.dir_inner[3] = -1.0f;  // omnidirectional world fixture glow
      s.color_outer[0] = g.color[0];
      s.color_outer[1] = g.color[1];
      s.color_outer[2] = g.color[2];
      s.color_outer[3] = -1.0f;
      s.extra[0] = 0.0f;
      s.extra[1] = 0.08f;  // tight camera bias so halo stays seated on the fixture
      s.extra[2] = max_glow_dist;
      s.extra[3] = c.fade;
      sprites.push_back(s);
    }
  }

  glow_count_ = uint32_t(sprites.size());
  if (glow_count_ > 0 && glow_buf_.ready()) {
    const VkDeviceSize bytes = VkDeviceSize(glow_count_) * sizeof(GlowSpriteGpu);
    std::memcpy(glow_buf_.data(), sprites.data(), size_t(bytes));
    ++glow_buf_.ver;
    glow_vb_offset_ = dyn_sync(glow_buf_, bytes);
  }
}

void Showroom::record_glows_draw(VkCommandBuffer cmd) {
  if (!carlights_ready_ || glow_count_ == 0 || !glow_pipe_ || !glow_buf_.ready()) return;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glow_pipe_);
  const VkDescriptorSet sets[2] = {frame_set_, glow_set_};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glow_pipe_layout_, 0, 2, sets, 0, nullptr);
  vkCmdBindVertexBuffers(cmd, 0, 1, &glow_buf_.buf.buffer, &glow_vb_offset_);
  vkCmdDraw(cmd, 6, glow_count_, 0, 0);
}


}  // namespace carview
