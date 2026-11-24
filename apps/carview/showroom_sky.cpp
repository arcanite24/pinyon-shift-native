#include "showroom_internal.h"

#include "sky_frag_spv.h"
#include "sky_vert_spv.h"
#include "stars_frag_spv.h"
#include "stars_vert_spv.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace carview {

void Showroom::set_tod(const world::TodState& tod, int moon_phase, float real_time_sec, float eye_game_y) {
  tod_active_ = true;
  tod_state_ = tod;
  tod_moon_phase_ = std::clamp(moon_phase, 0, 4);
  tod_real_time_ = real_time_sec;
  tod_eye_game_y_ = eye_game_y;
}

void Showroom::apply_tod_to_frame() {
  if (!tod_active_) {
    frame_.sun_color[3] = -1.0f;
    frame_.car_sun_color[3] = -1.0f;
    return;
  }
  const auto& t = tod_state_;
  // Map ShadowIntensity [-1..+1] to shadow weight [0.15..1.0] (softening shadows under moonlight).
  const float shadow_w = std::clamp(0.5f * (t.shadow_intensity + 1.0f), 0.12f, 1.0f);
  frame_.sun_color[0] = t.sun_color[0];
  frame_.sun_color[1] = t.sun_color[1];
  frame_.sun_color[2] = t.sun_color[2];
  frame_.sun_color[3] = shadow_w;

  frame_.car_sun_color[0] = t.car_sun_color[0];
  frame_.car_sun_color[1] = t.car_sun_color[1];
  frame_.car_sun_color[2] = t.car_sun_color[2];
  frame_.car_sun_color[3] = t.ibl_direct_car;

  frame_.sky_color[0] = t.hl_top[0];
  frame_.sky_color[1] = t.hl_top[1];
  frame_.sky_color[2] = t.hl_top[2];
  frame_.sky_color[3] = t.switch_on_lights;

  frame_.ground_color[0] = t.hl_bottom[0];
  frame_.ground_color[1] = t.hl_bottom[1];
  frame_.ground_color[2] = t.hl_bottom[2];
  frame_.ground_color[3] = t.day_night_post;

  frame_.fog_color[0] = t.fog_color[0];
  frame_.fog_color[1] = t.fog_color[1];
  frame_.fog_color[2] = t.fog_color[2];
  frame_.fog_color[3] = t.fog_start;

  // Modulate atmospheric sun-scatter colour by Fog_Templates SunScatterStartDistance /
  // SunScatterRampupDistance (SPEC-SKY-AND-TOD §4) evaluated at mid-horizon distance.
  const float sc_dist_world =
      std::clamp((3500.0f - t.fog_sun_scatter_start) / std::max(t.fog_sun_scatter_rampup, 1.0f), 0.15f, 1.0f);
  frame_.fog_scatter[0] = t.fog_color[0] + (t.fog_sun_scatter_color[0] - t.fog_color[0]) * sc_dist_world;
  frame_.fog_scatter[1] = t.fog_color[1] + (t.fog_sun_scatter_color[1] - t.fog_color[1]) * sc_dist_world;
  frame_.fog_scatter[2] = t.fog_color[2] + (t.fog_sun_scatter_color[2] - t.fog_color[2]) * sc_dist_world;
  frame_.fog_scatter[3] = t.fog_sun_scatter_power;

  // Convert TimeOfDayA FogDensity (~0.03 day .. 0.08 night) to exp2 distance rate.
  frame_.fog_params[0] = std::max(t.fog_density, 0.002f) * 0.015f;
  frame_.fog_params[1] = t.fog_height_fade_start;
  frame_.fog_params[2] = t.fog_height_fade_end;
  frame_.fog_params[3] = t.fog_height_fade_amount;

  frame_.car_light_top[0] = t.car_hl_top[0];
  frame_.car_light_top[1] = t.car_hl_top[1];
  frame_.car_light_top[2] = t.car_hl_top[2];
  frame_.car_light_top[3] = t.ibl_ambient_scale;

  frame_.car_light_bot[0] = t.car_hl_bottom[0];
  frame_.car_light_bot[1] = t.car_hl_bottom[1];
  frame_.car_light_bot[2] = t.car_hl_bottom[2];
  frame_.car_light_bot[3] = tod_eye_game_y_;

  frame_.ambient_color[0] = t.ambient_color[0];
  frame_.ambient_color[1] = t.ambient_color[1];
  frame_.ambient_color[2] = t.ambient_color[2];
  frame_.ambient_color[3] = t.ibl_ambient_scale;
}

bool Showroom::upload_sky(const world::RealTimeSkyAssets& assets, std::string& error) {
  if (guest_ || !r_.device_) return true;
  if (sky_ready_) return true;

  auto upload_rgba = [&](const fh1::RgbaImage& img, GpuTexture& out, bool fallback_black) -> bool {
    bake::Texture tex;
    tex.format = bake::TextureFormat::kRgba8;
    if (img.width > 0 && img.height > 0 && img.rgba.size() == size_t(img.width) * img.height * 4) {
      tex.levels.push_back({img.width, img.height, img.rgba});
    } else {
      const uint8_t v = fallback_black ? 0 : 255;
      tex.levels.push_back({1, 1, {v, v, v, v}});
    }
    return upload_texture(&tex, false, out, error);
  };

  if (!upload_rgba(assets.sun, sky_sun_, true)) return false;
  for (int i = 0; i < 5; ++i)
    if (!upload_rgba(assets.moons[size_t(i)], sky_moons_[i], true)) return false;
  if (!upload_rgba(assets.dither, sky_dither_, false)) return false;
  if (!upload_rgba(assets.far_cloud_front, sky_far_f_, true)) return false;
  if (!upload_rgba(assets.far_cloud_back, sky_far_b_, true)) return false;
  if (!upload_rgba(assets.far_cloud_mask, sky_far_mask_, false)) return false;
  if (!upload_rgba(assets.close_alpha_front, sky_close_ad_f_, true)) return false;
  if (!upload_rgba(assets.close_alpha_back, sky_close_ad_b_, true)) return false;
  if (!upload_rgba(assets.close_ambient_front, sky_close_am_f_, false)) return false;
  if (!upload_rgba(assets.close_ambient_back, sky_close_am_b_, false)) return false;
  if (!upload_rgba(assets.close_tb_front, sky_close_tb_f_, true)) return false;
  if (!upload_rgba(assets.close_tb_back, sky_close_tb_b_, true)) return false;
  sky_has_textures_ = assets.loaded();

  if (!assets.stars.empty()) {
    std::vector<float> star_data;
    star_data.reserve(assets.stars.size() * 4);
    for (const auto& s : assets.stars) {
      star_data.push_back(s.x);
      star_data.push_back(s.y);
      star_data.push_back(s.z);
      star_data.push_back(s.brightness);
    }
    if (!r_.upload_buffer(star_data.data(), star_data.size() * sizeof(float),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, star_vertices_)) {
      error = "starfield vertex buffer";
      return false;
    }
    star_count_ = uint32_t(assets.stars.size());
  }

  for (int i = 0; i < 2; ++i) {
    if (!r_.create_buffer(sizeof(SkyUniforms),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sky_ubo_[i])) {
      error = "sky uniform buffer";
      return false;
    }
  }

  // Sky descriptor set layout: binding 0 UBO, bindings 1..11 combined image samplers.
  VkDescriptorSetLayoutBinding bindings[12]{};
  bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  for (uint32_t b = 1; b < 12; ++b)
    bindings[b] = {b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_ci.bindingCount = 12;
  set_ci.pBindings = bindings;
  VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &set_ci, nullptr, &sky_set_layout_), "sky set layout");

  VkPipelineLayoutCreateInfo pl_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl_ci.setLayoutCount = 1;
  pl_ci.pSetLayouts = &sky_set_layout_;
  VK_TRY(vkCreatePipelineLayout(r_.device_, &pl_ci, nullptr, &sky_pipe_layout_), "sky pipeline layout");

  // Stars pipeline layout: push constants (mat4 + 2*vec4 = 96 bytes).
  VkPushConstantRange star_pc{VK_SHADER_STAGE_VERTEX_BIT, 0, 96};
  VkPipelineLayoutCreateInfo spl_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  spl_ci.pushConstantRangeCount = 1;
  spl_ci.pPushConstantRanges = &star_pc;
  VK_TRY(vkCreatePipelineLayout(r_.device_, &spl_ci, nullptr, &stars_pipe_layout_), "stars pipeline layout");

  // Descriptor pool & 12 sets (2 modes x 6 discs: 0=sun, 1..5=moon0..4).
  const VkDescriptorPoolSize psizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 12},
                                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12 * 11}};
  VkDescriptorPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_ci.maxSets = 12;
  pool_ci.poolSizeCount = 2;
  pool_ci.pPoolSizes = psizes;
  VK_TRY(vkCreateDescriptorPool(r_.device_, &pool_ci, nullptr, &sky_pool_), "sky descriptor pool");

  VkDescriptorSetLayout set_layouts[12];
  for (int i = 0; i < 12; ++i) set_layouts[i] = sky_set_layout_;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = sky_pool_;
  alloc.descriptorSetCount = 12;
  alloc.pSetLayouts = set_layouts;
  VK_TRY(vkAllocateDescriptorSets(r_.device_, &alloc, sky_sets_), "sky descriptor sets");

  for (int mode = 0; mode < 2; ++mode) {
    for (int disc = 0; disc < 6; ++disc) {
      const int idx = mode * 6 + disc;
      VkDescriptorBufferInfo buf{sky_ubo_[mode].buffer, 0, sizeof(SkyUniforms)};
      const VkImageView views[11] = {
          disc == 0 ? sky_sun_.view : sky_moons_[disc - 1].view,
          sky_dither_.view,
          sky_far_f_.view,
          sky_far_b_.view,
          sky_far_mask_.view,
          sky_close_ad_f_.view,
          sky_close_ad_b_.view,
          sky_close_am_f_.view,
          sky_close_am_b_.view,
          sky_close_tb_f_.view,
          sky_close_tb_b_.view,
      };
      VkDescriptorImageInfo imgs[11];
      VkWriteDescriptorSet writes[12]{};
      writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[0].dstSet = sky_sets_[idx];
      writes[0].dstBinding = 0;
      writes[0].descriptorCount = 1;
      writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      writes[0].pBufferInfo = &buf;
      for (uint32_t b = 0; b < 11; ++b) {
        imgs[b] = {sampler_, views[b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[b + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[b + 1].dstSet = sky_sets_[idx];
        writes[b + 1].dstBinding = b + 1;
        writes[b + 1].descriptorCount = 1;
        writes[b + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[b + 1].pImageInfo = &imgs[b];
      }
      vkUpdateDescriptorSets(r_.device_, 12, writes, 0, nullptr);
    }
  }

  // Create sky pipelines (base opaque + layers premultiplied alpha) & starfield instanced-quad pipeline.
  auto make_mod = [&](const uint32_t* code, size_t bytes, VkShaderModule& m) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    return vkCreateShaderModule(r_.device_, &ci, nullptr, &m);
  };
  VkShaderModule sv = VK_NULL_HANDLE, sf = VK_NULL_HANDLE, stv = VK_NULL_HANDLE, stf = VK_NULL_HANDLE;
  VK_TRY(make_mod(kSky_vert_spv, sizeof(kSky_vert_spv), sv), "sky.vert");
  VK_TRY(make_mod(kSky_frag_spv, sizeof(kSky_frag_spv), sf), "sky.frag");
  VK_TRY(make_mod(kStars_vert_spv, sizeof(kStars_vert_spv), stv), "stars.vert");
  VK_TRY(make_mod(kStars_frag_spv, sizeof(kStars_frag_spv), stf), "stars.frag");

  const VkBool32 linear_out = r_.post() ? VK_TRUE : VK_FALSE;
  const VkBool32 rev_z = r_.reverse_z() ? VK_TRUE : VK_FALSE;
  const VkSpecializationMapEntry spec_lin{0, 0, sizeof(VkBool32)};
  const VkSpecializationInfo spec_frag{1, &spec_lin, sizeof(VkBool32), &linear_out};
  const VkSpecializationMapEntry spec_rev{1, 0, sizeof(VkBool32)};
  const VkSpecializationInfo spec_vert{1, &spec_rev, sizeof(VkBool32), &rev_z};

  const VkFormat fmt = r_.scene_format();
  VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rend.colorAttachmentCount = 1;
  rend.pColorAttachmentFormats = &fmt;
  rend.depthAttachmentFormat = r_.depth_format_;

  VkPipelineVertexInputStateCreateInfo empty_vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo tri_ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  tri_ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = r_.samples_;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  // Sky & stars run AFTER opaque geometry at far depth (0.0 reverse-Z, 1.0 forward-Z)
  // so Hardware Early-Z rejects all occluded pixels before sky.frag executes.
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_FALSE;
  ds.depthCompareOp = r_.reverse_z() ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
  const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_states;

  const VkPipelineShaderStageCreateInfo sky_stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, sv, "main",
       &spec_vert},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, sf, "main",
       &spec_frag}};

  VkPipelineColorBlendAttachmentState att{};
  att.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &att;

  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.pNext = &rend;
  gp.stageCount = 2;
  gp.pStages = sky_stages;
  gp.pVertexInputState = &empty_vi;
  gp.pInputAssemblyState = &tri_ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pDepthStencilState = &ds;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &dyn;
  gp.layout = sky_pipe_layout_;
  VkResult res = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &gp, nullptr, &sky_pipe_base_);

  att.blendEnable = VK_TRUE;
  att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  att.colorBlendOp = VK_BLEND_OP_ADD;
  att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  att.alphaBlendOp = VK_BLEND_OP_ADD;
  if (res == VK_SUCCESS)
    res = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &gp, nullptr, &sky_pipe_layers_);

  // 1-sample HDR sky pipelines & descriptor sets for dynamic cubemap and rearview mirror passes.
  if (res == VK_SUCCESS && dyn_env_ready_) {
    const VkFormat hdr_fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkPipelineRenderingCreateInfo env_rend = rend;
    env_rend.pColorAttachmentFormats = &hdr_fmt;
    VkPipelineMultisampleStateCreateInfo ms1{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms1.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkGraphicsPipelineCreateInfo egp = gp;
    egp.pNext = &env_rend;
    egp.pMultisampleState = &ms1;
    VkPipelineColorBlendAttachmentState eatt{};
    eatt.colorWriteMask = att.colorWriteMask;
    VkPipelineColorBlendStateCreateInfo ecb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    ecb.attachmentCount = 1;
    ecb.pAttachments = &eatt;
    egp.pColorBlendState = &ecb;
    res = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &egp, nullptr, &env_sky_pipe_base_);
    if (res == VK_SUCCESS) {
      eatt = att;
      res = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &egp, nullptr, &env_sky_pipe_layers_);
    }
    if (res == VK_SUCCESS) {
      const VkDescriptorPoolSize eps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 12},
                                           {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12 * 11}};
      VkDescriptorPoolCreateInfo epci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      epci.maxSets = 12;
      epci.poolSizeCount = 2;
      epci.pPoolSizes = eps;
      if (vkCreateDescriptorPool(r_.device_, &epci, nullptr, &env_pool_) == VK_SUCCESS) {
        VkDescriptorSetAllocateInfo ealloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ealloc.descriptorPool = env_pool_;
        ealloc.descriptorSetCount = 12;
        ealloc.pSetLayouts = set_layouts;
        if (vkAllocateDescriptorSets(r_.device_, &ealloc, env_sky_sets_) == VK_SUCCESS) {
          for (int mode = 0; mode < 2; ++mode) {
            for (int disc = 0; disc < 6; ++disc) {
              const int idx = mode * 6 + disc;
              VkDescriptorBufferInfo buf{env_sky_ubo_[mode].buffer, 0, sizeof(SkyUniforms)};
              const VkImageView views[11] = {
                  disc == 0 ? sky_sun_.view : sky_moons_[disc - 1].view,
                  sky_dither_.view,
                  sky_far_f_.view,
                  sky_far_b_.view,
                  sky_far_mask_.view,
                  sky_close_ad_f_.view,
                  sky_close_ad_b_.view,
                  sky_close_am_f_.view,
                  sky_close_am_b_.view,
                  sky_close_tb_f_.view,
                  sky_close_tb_b_.view,
              };
              VkDescriptorImageInfo imgs[11];
              VkWriteDescriptorSet writes[12]{};
              writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
              writes[0].dstSet = env_sky_sets_[idx];
              writes[0].dstBinding = 0;
              writes[0].descriptorCount = 1;
              writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
              writes[0].pBufferInfo = &buf;
              for (uint32_t b = 0; b < 11; ++b) {
                imgs[b] = {sampler_, views[b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                writes[b + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[b + 1].dstSet = env_sky_sets_[idx];
                writes[b + 1].dstBinding = b + 1;
                writes[b + 1].descriptorCount = 1;
                writes[b + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[b + 1].pImageInfo = &imgs[b];
              }
              vkUpdateDescriptorSets(r_.device_, 12, writes, 0, nullptr);
            }
          }
        }
      }
    }
  }

  // Starfield pipeline (instanced 6-vertex quads, premultiplied alpha over sky atmosphere).
  VkVertexInputBindingDescription star_bind{0, 16, VK_VERTEX_INPUT_RATE_INSTANCE};
  VkVertexInputAttributeDescription star_attr{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
  VkPipelineVertexInputStateCreateInfo star_vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  star_vi.vertexBindingDescriptionCount = 1;
  star_vi.pVertexBindingDescriptions = &star_bind;
  star_vi.vertexAttributeDescriptionCount = 1;
  star_vi.pVertexAttributeDescriptions = &star_attr;
  const VkPipelineShaderStageCreateInfo star_stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, stv, "main",
       &spec_vert},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, stf, "main",
       &spec_frag}};
  gp.pStages = star_stages;
  gp.pVertexInputState = &star_vi;
  gp.pInputAssemblyState = &tri_ia;
  gp.layout = stars_pipe_layout_;
  if (res == VK_SUCCESS)
    res = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &gp, nullptr, &stars_pipe_);

  vkDestroyShaderModule(r_.device_, sv, nullptr);
  vkDestroyShaderModule(r_.device_, sf, nullptr);
  vkDestroyShaderModule(r_.device_, stv, nullptr);
  vkDestroyShaderModule(r_.device_, stf, nullptr);
  VK_TRY(res, "sky pipelines");

  sky_ready_ = true;
  return true;
}

void Showroom::fill_sky_uniforms(const ShowroomFrame& view_frame, SkyUniforms& u) const {
  // Compute clip_from_game = view_frame.view_projection * world_transform_ (column-major).
  float clip[16];
  multiply(view_frame.view_projection, world_transform_, clip);
  // Invert 3x3 matrix A = [row0(x); row1(y); row3(w)] (columns 0..2) so
  // V_game = inv(A) * [ndc.x, ndc.y, 1]^T maps screen NDC to exact game-frame ray direction.
  const double a00 = clip[0], a01 = clip[4], a02 = clip[8];
  const double a10 = clip[1], a11 = clip[5], a12 = clip[9];
  const double a20 = clip[3], a21 = clip[7], a22 = clip[11];
  const double c00 = a11 * a22 - a12 * a21;
  const double c01 = -(a10 * a22 - a12 * a20);
  const double c02 = a10 * a21 - a11 * a20;
  const double det = a00 * c00 + a01 * c01 + a02 * c02;
  double inv[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  if (std::fabs(det) > 1e-12) {
    const double id = 1.0 / det;
    inv[0][0] = c00 * id;
    inv[1][0] = c01 * id;
    inv[2][0] = c02 * id;
    inv[0][1] = -(a01 * a22 - a02 * a21) * id;
    inv[1][1] = (a00 * a22 - a02 * a20) * id;
    inv[2][1] = -(a00 * a21 - a01 * a20) * id;
    inv[0][2] = (a01 * a12 - a02 * a11) * id;
    inv[1][2] = -(a00 * a12 - a02 * a10) * id;
    inv[2][2] = (a00 * a11 - a01 * a10) * id;
  }

  const auto& t = tod_state_;
  u = {};
  // V = cam_fwd + ndc.x * cam_right - ndc.y * cam_up = C2 + ndc.x * C0 + ndc.y * C1.
  u.cam_fwd[0] = float(inv[0][2]);
  u.cam_fwd[1] = float(inv[1][2]);
  u.cam_fwd[2] = float(inv[2][2]);
  u.cam_fwd[3] = 0.0f;
  u.cam_right[0] = float(inv[0][0]);
  u.cam_right[1] = float(inv[1][0]);
  u.cam_right[2] = float(inv[2][0]);
  u.cam_right[3] = t.tod_sec / 86400.0f;
  u.cam_up[0] = float(-inv[0][1]);
  u.cam_up[1] = float(-inv[1][1]);
  u.cam_up[2] = float(-inv[2][1]);
  u.cam_up[3] = tod_real_time_;

  u.sun_dir[0] = t.sun_dir[0]; u.sun_dir[1] = t.sun_dir[1]; u.sun_dir[2] = t.sun_dir[2]; u.sun_dir[3] = t.sky_gain;
  const float disc_rad = std::clamp(t.sun_object_size * 0.0032f, 0.025f, 0.11f);
  u.sun_obj_dir[0] = t.sun_object_dir[0]; u.sun_obj_dir[1] = t.sun_object_dir[1];
  u.sun_obj_dir[2] = t.sun_object_dir[2]; u.sun_obj_dir[3] = disc_rad;
  u.haze_dir[0] = t.haze_dir[0]; u.haze_dir[1] = t.haze_dir[1]; u.haze_dir[2] = t.haze_dir[2];
  u.haze_dir[3] = t.is_moon ? 1.0f : 0.0f;

  u.atmo_top[0] = t.atmo_top[0]; u.atmo_top[1] = t.atmo_top[1]; u.atmo_top[2] = t.atmo_top[2]; u.atmo_top[3] = t.atmo_color_power;
  u.atmo_bottom[0] = t.atmo_bottom[0]; u.atmo_bottom[1] = t.atmo_bottom[1]; u.atmo_bottom[2] = t.atmo_bottom[2]; u.atmo_bottom[3] = t.atmo_clamp_lo;
  u.atmo_haze[0] = t.atmo_haze_color[0]; u.atmo_haze[1] = t.atmo_haze_color[1]; u.atmo_haze[2] = t.atmo_haze_color[2]; u.atmo_haze[3] = t.atmo_clamp_hi;
  u.atmo_haze_p[0] = t.atmo_haze_power; u.atmo_haze_p[1] = t.atmo_haze_intensity;
  u.atmo_haze_p[2] = t.atmo_haze_elliptical; u.atmo_haze_p[3] = t.atmo_haze_alpha;

  u.sun_obj_col[0] = t.sun_object_color[0]; u.sun_obj_col[1] = t.sun_object_color[1];
  u.sun_obj_col[2] = t.sun_object_color[2]; u.sun_obj_col[3] = t.sun_object_alpha;

  u.far_dense[0] = t.far_dense[0]; u.far_dense[1] = t.far_dense[1]; u.far_dense[2] = t.far_dense[2]; u.far_dense[3] = t.far_thin_intensity;
  u.far_thin[0] = t.far_thin[0]; u.far_thin[1] = t.far_thin[1]; u.far_thin[2] = t.far_thin[2]; u.far_thin[3] = t.far_alpha_power;
  u.far_params[0] = t.far_centre_alpha; u.far_params[1] = t.far_edge_alpha;
  u.far_params[2] = t.far_coverage; u.far_params[3] = t.far_back_coverage;
  u.far_back_dense[0] = t.far_back_dense[0]; u.far_back_dense[1] = t.far_back_dense[1];
  u.far_back_dense[2] = t.far_back_dense[2]; u.far_back_dense[3] = t.far_back_centre_alpha;
  u.far_back_thin[0] = t.far_back_thin[0]; u.far_back_thin[1] = t.far_back_thin[1];
  u.far_back_thin[2] = t.far_back_thin[2]; u.far_back_thin[3] = t.far_back_edge_alpha;
  const float day_frac = t.tod_sec / 86400.0f;
  u.far_scroll[0] = std::fmod(tod_real_time_ * 0.0012f + day_frac * 0.4f, 1.0f);
  u.far_scroll[1] = std::fmod(tod_real_time_ * 0.0008f, 1.0f);
  u.far_scroll[2] = std::fmod(t.far_back_speed_x * 0.000015f * tod_real_time_, 1.0f);
  u.far_scroll[3] = std::fmod(t.far_back_speed_z * 0.000015f * tod_real_time_, 1.0f);

  u.close_top_col[0] = t.close_top_color[0]; u.close_top_col[1] = t.close_top_color[1];
  u.close_top_col[2] = t.close_top_color[2]; u.close_top_col[3] = t.close_top_power;
  u.close_bot_col[0] = t.close_bottom_color[0]; u.close_bot_col[1] = t.close_bottom_color[1];
  u.close_bot_col[2] = t.close_bottom_color[2]; u.close_bot_col[3] = t.close_bottom_power;
  u.close_back_col[0] = t.close_back_color[0]; u.close_back_col[1] = t.close_back_color[1];
  u.close_back_col[2] = t.close_back_color[2]; u.close_back_col[3] = t.close_back_power;
  u.close_dense_col[0] = t.close_dense_color[0]; u.close_dense_col[1] = t.close_dense_color[1];
  u.close_dense_col[2] = t.close_dense_color[2]; u.close_dense_col[3] = t.close_dense_intensity;
  u.close_thin_col[0] = t.close_thin_color[0]; u.close_thin_col[1] = t.close_thin_color[1];
  u.close_thin_col[2] = t.close_thin_color[2]; u.close_thin_col[3] = t.close_thin_intensity;
  u.close_intens[0] = t.close_back_intensity; u.close_intens[1] = t.close_top_intensity;
  u.close_intens[2] = t.close_bottom_intensity; u.close_intens[3] = t.close_alpha_curve;
  u.close_masks[0] = t.close_top_mask_power; u.close_masks[1] = t.close_bottom_mask_power;
  u.close_masks[2] = t.close_top_alpha; u.close_masks[3] = t.close_bottom_alpha;
  u.close_layers[0] = t.close_front_alpha; u.close_layers[1] = t.close_back_alpha;
  u.close_layers[2] = std::fmod(t.close_front_revs * day_frac + tod_real_time_ * 0.0005f, 1.0f);
  u.close_layers[3] = std::fmod(t.close_back_revs * day_frac - tod_real_time_ * 0.0003f, 1.0f);
  u.close_back_dense_col[0] = t.close_back_dense_color[0]; u.close_back_dense_col[1] = t.close_back_dense_color[1];
  u.close_back_dense_col[2] = t.close_back_dense_color[2]; u.close_back_dense_col[3] = 1.0f;  // sky saturation (1.0 main)
  u.close_back_thin_col[0] = t.close_back_thin_color[0]; u.close_back_thin_col[1] = t.close_back_thin_color[1];
  u.close_back_thin_col[2] = t.close_back_thin_color[2]; u.close_back_thin_col[3] = t.close_back_bottom_intensity;
  u.close_back_bot_col[0] = t.close_back_bottom_color[0]; u.close_back_bot_col[1] = t.close_back_bottom_color[1];
  u.close_back_bot_col[2] = t.close_back_bottom_color[2]; u.close_back_bot_col[3] = t.close_back_bottom_power;

  u.fog_color[0] = t.fog_color[0]; u.fog_color[1] = t.fog_color[1]; u.fog_color[2] = t.fog_color[2];
  u.fog_color[3] = std::clamp(t.fog_sky_height_fade_start / 15000.0f, 0.0f, 0.25f);
  const float sc_dist_sky =
      std::clamp((15000.0f - t.fog_sun_scatter_start) / std::max(t.fog_sun_scatter_rampup, 1.0f), 0.15f, 1.0f);
  u.fog_scatter[0] = t.fog_color[0] + (t.fog_sun_scatter_color[0] - t.fog_color[0]) * sc_dist_sky;
  u.fog_scatter[1] = t.fog_color[1] + (t.fog_sun_scatter_color[1] - t.fog_color[1]) * sc_dist_sky;
  u.fog_scatter[2] = t.fog_color[2] + (t.fog_sun_scatter_color[2] - t.fog_color[2]) * sc_dist_sky;
  u.fog_scatter[3] = t.fog_sun_scatter_power;
  u.fog_sky[0] = std::clamp(t.fog_sky_height_fade_end / 15000.0f, 0.05f, 0.35f);
  u.fog_sky[1] = std::clamp(t.fog_density * 14.0f, 0.15f, 0.85f);
  u.fog_sky[2] = sky_has_textures_ ? 1.0f : 0.0f;
  u.fog_sky[3] = t.star_fade;
}

void Showroom::record_sky_before(VkCommandBuffer cmd) {
  SkyUniforms u{};
  fill_sky_uniforms(frame_, u);

  // Update mode 0 (atmosphere) and mode 1 (disc + clouds + horizon fog) UBOs.
  // Pre-transfer WAR barrier ensures prior frame fragment reads complete before overwriting the UBO.
  for (int m = 0; m < 2; ++m) {
    u.cam_fwd[3] = float(m);
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = sky_ubo_[m].buffer;
    b.size = sizeof(SkyUniforms);
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);

    vkCmdUpdateBuffer(cmd, sky_ubo_[m].buffer, 0, sizeof(SkyUniforms), &u);

    b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
}

void Showroom::record_sky_draw(VkCommandBuffer cmd) {
  if (!sky_ready_) return;
  const int disc = tod_state_.is_moon ? (1 + tod_moon_phase_) : 0;

  // 1. Atmosphere gradient dome + elliptical sun haze.
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipe_base_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipe_layout_, 0, 1, &sky_sets_[disc], 0, nullptr);
  vkCmdDraw(cmd, 3, 1, 0, 0);

  // 2. Stars.bin starfield instanced quads (drawn only when StarFade > 0.005).
  if (star_count_ > 0 && tod_state_.star_fade > 0.005f) {
    struct {
      float view_proj_rot[16];
      float params[4];
      float viewport_inv[4];
    } spc{};
    static_assert(sizeof(spc) == 96);
    multiply(frame_.view_projection, world_transform_, spc.view_proj_rot);
    spc.params[0] = tod_state_.star_fade;
    // Slow sidereal rotation: 15 deg/hour = 2*pi / 24h.
    spc.params[1] = tod_state_.tod_sec * (6.2831853f / 86400.0f);
    spc.params[2] = 2.6f;
    spc.params[3] = 1.0f;
    const uint32_t vp_w = std::max(1u, r_.render_width());
    const uint32_t vp_h = std::max(1u, r_.render_height());
    spc.viewport_inv[0] = 1.0f / float(vp_w);
    spc.viewport_inv[1] = 1.0f / float(vp_h);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, stars_pipe_);
    vkCmdPushConstants(cmd, stars_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(spc), &spc);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &star_vertices_.buffer, &zero);
    vkCmdDraw(cmd, 6, star_count_, 0, 0);
  }

  // 3. Sun/Moon disc + Far clouds (2 layers) + Close lit clouds (2 layers) + Horizon fog skirt.
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipe_layers_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipe_layout_, 0, 1, &sky_sets_[6 + disc], 0,
                          nullptr);
  vkCmdDraw(cmd, 3, 1, 0, 0);
}


}  // namespace carview
