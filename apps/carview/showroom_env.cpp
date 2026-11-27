#include "showroom_internal.h"

#include "env_prefilter_frag_spv.h"
#include "glow_frag_spv.h"
#include "glow_vert_spv.h"
#include "particle_frag_spv.h"
#include "particle_vert_spv.h"
#include "post_vert_spv.h"
#include "showroom_frag_spv.h"
#include "showroom_vert_spv.h"
#include "world_frag_spv.h"
#include "world_vert_spv.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace carview {
namespace {

std::mutex g_env_glow_mutex;
std::unordered_map<const Showroom*, VkPipeline> g_env_glow_pipes;

struct MirrorExtraPipes {
  VkPipeline car_glass = VK_NULL_HANDLE;
  VkPipeline particle_alpha = VK_NULL_HANDLE;
  VkPipeline particle_add = VK_NULL_HANDLE;
  VkPipeline world_water = VK_NULL_HANDLE;
};
std::mutex g_mirror_extra_mutex;
std::unordered_map<const Showroom*, MirrorExtraPipes> g_mirror_extra_pipes;

// Cached distance-capped coarse billboard quads built from GPU-instanced `world_trees_`
// so car paint cubemaps and cockpit/virtual rearview mirrors reflect Colorado's forests.
constexpr uint32_t kMaxEnvTrees = 512;
constexpr uint32_t kMaxEnvTreeVerts = kMaxEnvTrees * 6u;

struct EnvTreeBatch {
  uint32_t texture = kNoWorldTexture;
  uint32_t first_vertex = 0;
  uint32_t vertex_count = 0;
};

struct EnvTreeCache {
  Renderer::Buffer vb;
  void* mapped = nullptr;
  uint64_t world_gen = 0;
  float eye_sc[3] = {0, 0, 0};
  uint64_t ver = 0;
  uint64_t slot_ver[Renderer::kFramesInFlight] = {};
  std::vector<WorldVertex> cpu;
  std::vector<EnvTreeBatch> batches;
};
std::mutex g_env_tree_mutex;
std::unordered_map<const Showroom*, std::unique_ptr<EnvTreeCache>> g_env_tree_caches;

MirrorExtraPipes create_mirror_extra_pipes(VkDevice device, VkPipelineCache cache, VkPipelineLayout car_layout,
                                           VkPipelineLayout particle_layout, VkPipelineLayout world_layout,
                                           VkFormat hdr_fmt, VkFormat depth_format, bool reverse_z) {
  MirrorExtraPipes out{};
  auto make_mod = [&](const uint32_t* code, size_t bytes, VkShaderModule& m) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    return vkCreateShaderModule(device, &ci, nullptr, &m);
  };
  const VkBool32 spec_vals[2] = {VK_TRUE, reverse_z ? VK_TRUE : VK_FALSE};
  const VkSpecializationMapEntry spec_map[2] = {{0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}};
  const VkSpecializationInfo spec2{2, spec_map, sizeof(spec_vals), spec_vals};

  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_FALSE;
  ds.depthCompareOp = reverse_z ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
  const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_states;
  VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rend.colorAttachmentCount = 1;
  rend.pColorAttachmentFormats = &hdr_fmt;
  rend.depthAttachmentFormat = depth_format;

  // 1. 1-sample HDR car glass pipeline for trailing cars in cockpit mirrors.
  if (car_layout) {
    VkShaderModule sv = VK_NULL_HANDLE, sf = VK_NULL_HANDLE;
    if (make_mod(kShowroom_vert_spv, sizeof(kShowroom_vert_spv), sv) == VK_SUCCESS &&
        make_mod(kShowroom_frag_spv, sizeof(kShowroom_frag_spv), sf) == VK_SUCCESS) {
      const VkPipelineShaderStageCreateInfo st[2] = {
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, sv, "main",
           nullptr},
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, sf, "main",
           &spec2}};
      VkVertexInputBindingDescription vbind{0, sizeof(cars::bake::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
      const VkVertexInputAttributeDescription vattrs[5] = {
          {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(cars::bake::Vertex, position)},
          {1, 0, VK_FORMAT_R16G16B16A16_SNORM, offsetof(cars::bake::Vertex, frame)},
          {2, 0, VK_FORMAT_R16G16_UNORM, offsetof(cars::bake::Vertex, uv0)},
          {3, 0, VK_FORMAT_R16G16_UNORM, offsetof(cars::bake::Vertex, uv1)},
          {4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(cars::bake::Vertex, lighting)}};
      VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      vi.vertexBindingDescriptionCount = 1;
      vi.pVertexBindingDescriptions = &vbind;
      vi.vertexAttributeDescriptionCount = 5;
      vi.pVertexAttributeDescriptions = vattrs;
      VkPipelineColorBlendAttachmentState ba{};
      ba.blendEnable = VK_TRUE;
      ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      ba.colorBlendOp = VK_BLEND_OP_ADD;
      ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      ba.alphaBlendOp = VK_BLEND_OP_ADD;
      ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT;
      VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      cb.attachmentCount = 1;
      cb.pAttachments = &ba;
      VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      gp.pNext = &rend;
      gp.stageCount = 2;
      gp.pStages = st;
      gp.pVertexInputState = &vi;
      gp.pInputAssemblyState = &ia;
      gp.pViewportState = &vp;
      gp.pRasterizationState = &rs;
      gp.pMultisampleState = &ms;
      gp.pDepthStencilState = &ds;
      gp.pColorBlendState = &cb;
      gp.pDynamicState = &dyn;
      gp.layout = car_layout;
      vkCreateGraphicsPipelines(device, cache, 1, &gp, nullptr, &out.car_glass);
    }
    if (sv) vkDestroyShaderModule(device, sv, nullptr);
    if (sf) vkDestroyShaderModule(device, sf, nullptr);
  }

  // 2. 1-sample HDR particle alpha + additive pipelines for RenderInMirror particles.
  if (particle_layout) {
    VkShaderModule pv = VK_NULL_HANDLE, pf = VK_NULL_HANDLE;
    if (make_mod(kParticle_vert_spv, sizeof(kParticle_vert_spv), pv) == VK_SUCCESS &&
        make_mod(kParticle_frag_spv, sizeof(kParticle_frag_spv), pf) == VK_SUCCESS) {
      const VkPipelineShaderStageCreateInfo st[2] = {
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, pv, "main",
           nullptr},
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, pf, "main",
           &spec2}};
      VkVertexInputBindingDescription bind{0, 96, VK_VERTEX_INPUT_RATE_INSTANCE};
      const VkVertexInputAttributeDescription attrs[6] = {
          {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},  {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
          {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32}, {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 48},
          {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 64}, {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 80}};
      VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      vi.vertexBindingDescriptionCount = 1;
      vi.pVertexBindingDescriptions = &bind;
      vi.vertexAttributeDescriptionCount = 6;
      vi.pVertexAttributeDescriptions = attrs;
      VkPipelineColorBlendAttachmentState ba{};
      ba.blendEnable = VK_TRUE;
      ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      ba.colorBlendOp = VK_BLEND_OP_ADD;
      ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      ba.alphaBlendOp = VK_BLEND_OP_ADD;
      ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT;
      VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      cb.attachmentCount = 1;
      cb.pAttachments = &ba;
      VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      gp.pNext = &rend;
      gp.stageCount = 2;
      gp.pStages = st;
      gp.pVertexInputState = &vi;
      gp.pInputAssemblyState = &ia;
      gp.pViewportState = &vp;
      gp.pRasterizationState = &rs;
      gp.pMultisampleState = &ms;
      gp.pDepthStencilState = &ds;
      gp.pColorBlendState = &cb;
      gp.pDynamicState = &dyn;
      gp.layout = particle_layout;
      vkCreateGraphicsPipelines(device, cache, 1, &gp, nullptr, &out.particle_alpha);
      ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
      ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      vkCreateGraphicsPipelines(device, cache, 1, &gp, nullptr, &out.particle_add);
    }
    if (pv) vkDestroyShaderModule(device, pv, nullptr);
    if (pf) vkDestroyShaderModule(device, pf, nullptr);
  }

  // 3. 1-sample HDR world water pipeline for alpine lakes & rivers in cubemaps and mirrors.
  if (world_layout) {
    VkShaderModule wv = VK_NULL_HANDLE, wf = VK_NULL_HANDLE;
    if (make_mod(kWorld_vert_spv, sizeof(kWorld_vert_spv), wv) == VK_SUCCESS &&
        make_mod(kWorld_frag_spv, sizeof(kWorld_frag_spv), wf) == VK_SUCCESS) {
      const VkPipelineShaderStageCreateInfo st[2] = {
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, wv, "main",
           &spec2},
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, wf, "main",
           &spec2}};
      VkVertexInputBindingDescription wbind{0, sizeof(WorldVertex), VK_VERTEX_INPUT_RATE_VERTEX};
      const VkVertexInputAttributeDescription wattrs[7] = {
          {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(WorldVertex, position)},
          {1, 0, VK_FORMAT_R8G8B8A8_SNORM, offsetof(WorldVertex, normal)},
          {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WorldVertex, uv0)},
          {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WorldVertex, uv1)},
          {4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(WorldVertex, colour)},
          {5, 0, VK_FORMAT_R16G16B16A16_SINT, offsetof(WorldVertex, centre)},
          {6, 0, VK_FORMAT_R16G16_SFLOAT, offsetof(WorldVertex, uv2)}};
      VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      vi.vertexBindingDescriptionCount = 1;
      vi.pVertexBindingDescriptions = &wbind;
      vi.vertexAttributeDescriptionCount = 7;
      vi.pVertexAttributeDescriptions = wattrs;
      VkPipelineRasterizationStateCreateInfo wrs = rs;
      wrs.frontFace = VK_FRONT_FACE_CLOCKWISE;
      wrs.depthBiasEnable = VK_TRUE;
      VkPipelineDepthStencilStateCreateInfo wds = ds;
      wds.depthWriteEnable = VK_TRUE;
      const VkDynamicState wdyn_states[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                             VK_DYNAMIC_STATE_DEPTH_BIAS};
      VkPipelineDynamicStateCreateInfo wdyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
      wdyn.dynamicStateCount = 3;
      wdyn.pDynamicStates = wdyn_states;
      VkPipelineColorBlendAttachmentState ba{};
      ba.blendEnable = VK_TRUE;
      ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      ba.colorBlendOp = VK_BLEND_OP_ADD;
      ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      ba.alphaBlendOp = VK_BLEND_OP_ADD;
      ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT;
      VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      cb.attachmentCount = 1;
      cb.pAttachments = &ba;
      VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      gp.pNext = &rend;
      gp.stageCount = 2;
      gp.pStages = st;
      gp.pVertexInputState = &vi;
      gp.pInputAssemblyState = &ia;
      gp.pViewportState = &vp;
      gp.pRasterizationState = &wrs;
      gp.pMultisampleState = &ms;
      gp.pDepthStencilState = &wds;
      gp.pColorBlendState = &cb;
      gp.pDynamicState = &wdyn;
      gp.layout = world_layout;
      vkCreateGraphicsPipelines(device, cache, 1, &gp, nullptr, &out.world_water);
    }
    if (wv) vkDestroyShaderModule(device, wv, nullptr);
    if (wf) vkDestroyShaderModule(device, wf, nullptr);
  }
  return out;
}

VkPipeline create_env_glow_pipeline(VkDevice device, VkPipelineCache cache, VkPipelineLayout layout,
                                    VkFormat hdr_fmt, VkFormat depth_format, bool reverse_z) {
  auto make_mod = [&](const uint32_t* code, size_t bytes, VkShaderModule& m) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    return vkCreateShaderModule(device, &ci, nullptr, &m);
  };
  VkShaderModule gv = VK_NULL_HANDLE, gf = VK_NULL_HANDLE;
  if (make_mod(kGlow_vert_spv, sizeof(kGlow_vert_spv), gv) != VK_SUCCESS) return VK_NULL_HANDLE;
  if (make_mod(kGlow_frag_spv, sizeof(kGlow_frag_spv), gf) != VK_SUCCESS) {
    vkDestroyShaderModule(device, gv, nullptr);
    return VK_NULL_HANDLE;
  }

  const VkBool32 linear_out = VK_TRUE;
  const VkSpecializationMapEntry spec_lin{0, 0, sizeof(VkBool32)};
  const VkSpecializationInfo spec_frag{1, &spec_lin, sizeof(VkBool32), &linear_out};
  const VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, gv, "main",
       nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, gf, "main",
       &spec_frag}};

  VkVertexInputBindingDescription vbind{0, 64, VK_VERTEX_INPUT_RATE_INSTANCE};
  const VkVertexInputAttributeDescription vattrs[4] = {
      {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
      {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
      {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
      {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 48}};
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
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_FALSE;
  ds.depthCompareOp = reverse_z ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
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

  VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rend.colorAttachmentCount = 1;
  rend.pColorAttachmentFormats = &hdr_fmt;
  rend.depthAttachmentFormat = depth_format;

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
  gp.layout = layout;

  VkPipeline pipe = VK_NULL_HANDLE;
  vkCreateGraphicsPipelines(device, cache, 1, &gp, nullptr, &pipe);
  vkDestroyShaderModule(device, gv, nullptr);
  vkDestroyShaderModule(device, gf, nullptr);
  return pipe;
}

void desaturate_rgb(float rgb[3], float sat) {
  const float luma = rgb[0] * 0.2126f + rgb[1] * 0.7152f + rgb[2] * 0.0722f;
  for (int i = 0; i < 3; ++i) rgb[i] = std::max(0.0f, luma + (rgb[i] - luma) * sat);
}

}  // namespace

void Showroom::bind_dynamic_env(VkImageView cube_view, VkSampler sampler) {
  if (!r_.device_ || !frame_set_ || !cube_view || !sampler || bound_dyn_env_view_ == cube_view) return;
  // Allocate fresh per-slot frame descriptor sets from pool_ so we NEVER mutate an in-flight set
  // or call vkDeviceWaitIdle() on traffic/race car spawn!
  VkDescriptorSetLayout layouts[Renderer::kFramesInFlight];
  for (uint32_t s = 0; s < Renderer::kFramesInFlight; ++s) layouts[s] = frame_layout_;
  VkDescriptorSet fresh[Renderer::kFramesInFlight] = {};
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = Renderer::kFramesInFlight;
  ai.pSetLayouts = layouts;
  if (vkAllocateDescriptorSets(r_.device_, &ai, fresh) == VK_SUCCESS) {
    const VkImageView mirror_view =
        (dyn_env_ready_ && mirror_tex_.view) ? mirror_tex_.view : gpu_textures_[1].view;
    for (uint32_t s = 0; s < Renderer::kFramesInFlight; ++s) {
      write_frame_set(fresh[s], s, frame_buffer_.buf.buffer, frame_buffer_.offset(s), cube_view, sampler,
                      headlight_cookie_.view, mirror_view);
      frame_sets_dip_[s] = fresh[s];
    }
    frame_set_ = fresh[r_.frame_slot()];
    sync_fullbeam_frame_sets(cube_view, sampler);
    bound_dyn_env_view_ = cube_view;
    return;
  }
  bound_dyn_env_view_ = cube_view;
}

void Showroom::bind_post_virtual_mirror(Renderer& r, VkImageView view) {
  if (!r.device_ || !r.post_sampler_ || !r.post_sets_[0]) return;
  const VkImageView target_view = view ? view : r.lum_adapt_[1].view;
  if (!target_view) return;
  VkDescriptorImageInfo imgs[Renderer::kPostSetCount];
  VkWriteDescriptorSet writes[Renderer::kPostSetCount];
  for (uint32_t s = 0; s < Renderer::kPostSetCount; ++s) {
    imgs[s] = {r.post_sampler_, target_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[s] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[s].dstSet = r.post_sets_[s];
    writes[s].dstBinding = 12;
    writes[s].descriptorCount = 1;
    writes[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[s].pImageInfo = &imgs[s];
  }
  vkUpdateDescriptorSets(r.device_, Renderer::kPostSetCount, writes, 0, nullptr);
}

void Showroom::set_virtual_mirror_enabled(bool enabled) {
  virtual_mirror_enabled_ = enabled;
  update_mirror_mode();
}

void Showroom::update_mirror_mode() {
  if (guest_) return;
  static const bool env_disabled = std::getenv("SHIFT_NO_VIRTUAL_MIRROR") != nullptr;
  virtual_mirror_active_ =
      virtual_mirror_enabled_ && !env_disabled &&
      is_bumper_or_hood_camera(world_lit_, cockpit_camera_active_, draw_car_, have_bounds_, bounds_min_, bounds_max_,
                               frame_.eye, frame_.view_projection);
  mirror_enabled_ = cockpit_camera_active_ || virtual_mirror_active_;
  if (mirror_enabled_ && dyn_env_ready_ && !mirror_tex_.view) {
    ensure_mirror_targets();
  }
  detail::set_virtual_mirror_active(&r_, virtual_mirror_active_ && mirror_valid_ && mirror_tex_.view != VK_NULL_HANDLE);
}


bool Showroom::init_dynamic_reflections(std::string& error) {
  if (guest_ || !r_.device_) return true;
  if (dyn_env_ready_) return true;

  // Prefer Colorado's outdoor staticcarcubemap.xpr over uiautoshow when running in drive mode
  // (detected by HDR post chain or sun shadows; showroom/autoshow keeps uiautoshow studio cube).
  if ((r_.post() || r_.options_.shadow.size > 0) && !env_xpr_path_.empty()) {
    const std::string parent_low = lower(env_xpr_path_.parent_path().filename().string());
    if (parent_low == "uiautoshow") {
      const auto colorado_xpr = env_xpr_path_.parent_path().parent_path() / "colorado" / "staticcarcubemap.xpr";
      std::error_code ec;
      if (std::filesystem::exists(colorado_xpr, ec)) {
        env_xpr_path_ = colorado_xpr;
        r_.destroy_image(environment_);
        if (!upload_environment(error)) return false;
      }
    }
  }

  // Packed B10G11R11 (or RGBA16F on Turnip) cubemap for live drive reflections (car + LAKE water).
  const VkFormat hdr_fmt = r_.bloom_format();
  const uint32_t env_sz = dyn_env_size_;
  const uint32_t env_mips = std::min(dyn_env_mips_, kDynEnvMips);
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = hdr_fmt;
  ci.extent = {env_sz, env_sz, 1};
  ci.mipLevels = env_mips;
  ci.arrayLayers = 6;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  VmaAllocationInfo ainfo{};
  VK_TRY(vmaCreateImage(r_.allocator_, &ci, &alloc, &dyn_env_.image, &dyn_env_.allocation, &ainfo),
         "dynamic env cubemap");
  dyn_env_.bytes = ainfo.size;

  VkImageViewCreateInfo cv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  cv.image = dyn_env_.image;
  cv.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  cv.format = hdr_fmt;
  cv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, env_mips, 0, 6};
  VK_TRY(vkCreateImageView(r_.device_, &cv, nullptr, &dyn_env_.view), "dynamic env cube view");

  VkImageViewCreateInfo cv0{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  cv0.image = dyn_env_.image;
  cv0.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  cv0.format = hdr_fmt;
  cv0.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
  VK_TRY(vkCreateImageView(r_.device_, &cv0, nullptr, &dyn_env_mip0_cube_), "dynamic env mip0 cube view");

  for (uint32_t f = 0; f < 6; ++f) {
    VkImageViewCreateInfo fv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    fv.image = dyn_env_.image;
    fv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    fv.format = hdr_fmt;
    fv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f, 1};
    VK_TRY(vkCreateImageView(r_.device_, &fv, nullptr, &dyn_env_faces_[f]), "dynamic env face view");
    for (uint32_t m = 1; m < env_mips; ++m) {
      fv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, f, 1};
      VK_TRY(vkCreateImageView(r_.device_, &fv, nullptr, &dyn_env_mip_faces_[m][f]), "dynamic env mip face view");
    }
  }

  // Cross-face GGX cubemap prefilter pipeline (post.vert + env_prefilter.frag).
  {
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                   nullptr};
    VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount = 1;
    dsl.pBindings = &b;
    VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &dsl, nullptr, &env_prefilter_set_layout_), "prefilter dsl");
    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &env_prefilter_set_layout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    VK_TRY(vkCreatePipelineLayout(r_.device_, &pl, nullptr, &env_prefilter_pipe_layout_), "prefilter pl");
    VkShaderModule pv = VK_NULL_HANDLE, pf = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(kPost_vert_spv);
    smi.pCode = kPost_vert_spv;
    VK_TRY(vkCreateShaderModule(r_.device_, &smi, nullptr, &pv), "prefilter vert");
    smi.codeSize = sizeof(kEnv_prefilter_frag_spv);
    smi.pCode = kEnv_prefilter_frag_spv;
    if (vkCreateShaderModule(r_.device_, &smi, nullptr, &pf) != VK_SUCCESS) {
      vkDestroyShaderModule(r_.device_, pv, nullptr);
      error = "prefilter frag";
      return false;
    }
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, pv, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, pf, "main",
         nullptr}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo bl{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bl.attachmentCount = 1;
    bl.pAttachments = &att;
    const VkDynamicState dyn_s[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_s;
    VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rend.colorAttachmentCount = 1;
    rend.pColorAttachmentFormats = &hdr_fmt;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &rend;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &bl;
    pci.pDynamicState = &dyn;
    pci.layout = env_prefilter_pipe_layout_;
    VkResult pr_res =
        vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &pci, nullptr, &env_prefilter_pipe_);
    vkDestroyShaderModule(r_.device_, pv, nullptr);
    vkDestroyShaderModule(r_.device_, pf, nullptr);
    VK_TRY(pr_res, "prefilter pipeline");
  }

  if (!r_.create_image(env_sz, env_sz, 1, r_.depth_format_, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT, dyn_env_depth_)) {
    error = "dynamic env depth";
    return false;
  }

  if (!create_ubo_ring(env_frame_buffer_, sizeof(ShowroomFrame), Renderer::kFramesInFlight * kMaxEnvViews, error) ||
      !create_ubo_ring(env_sky_ubo_, sizeof(SkyUniforms), Renderer::kFramesInFlight * kMaxEnvViews, error)) {
    error = "dynamic env buffers";
    return false;
  }

  // Clear cubemap to SHADER_READ_ONLY_OPTIMAL so descriptors are valid before first draw.
  // Cockpit mirror targets are allocated lazily on first cockpit camera activation.
  const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = dyn_env_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, env_mips, 0, 6};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    const VkClearColorValue clr{{0.004f, 0.006f, 0.010f, 1.0f}};
    vkCmdClearColorImage(cmd, dyn_env_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &b.subresourceRange);
    b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    VkImageMemoryBarrier2 db{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    db.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    db.dstAccessMask =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    db.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    db.image = dyn_env_depth_.image;
    db.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo ddep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    ddep.imageMemoryBarrierCount = 1;
    ddep.pImageMemoryBarriers = &db;
    vkCmdPipelineBarrier2(cmd, &ddep);
  });
  if (!ok) {
    error = "dynamic env clear";
    return false;
  }
  dyn_env_ready_ = true;
  if (mirror_enabled_) ensure_mirror_targets();
  return true;
}

bool Showroom::ensure_mirror_targets() {
  if (guest_ || !r_.device_) return false;
  if (mirror_tex_.view && mirror_depth_.view) return true;
  const VkFormat hdr_fmt = r_.bloom_format();
  if (!r_.create_image(kMirrorWidth, kMirrorHeight, 1, hdr_fmt, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, mirror_tex_) ||
      !r_.create_image(kMirrorWidth, kMirrorHeight, 1, r_.depth_format_, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT, mirror_depth_)) {
    r_.destroy_image(mirror_tex_);
    r_.destroy_image(mirror_depth_);
    return false;
  }
  const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = mirror_tex_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    const VkClearColorValue clr{{0.004f, 0.006f, 0.010f, 1.0f}};
    vkCmdClearColorImage(cmd, mirror_tex_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &b.subresourceRange);
    b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    VkImageMemoryBarrier2 db{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    db.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    db.dstAccessMask =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    db.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    db.srcQueueFamilyIndex = db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    db.image = mirror_depth_.image;
    db.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo ddep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    ddep.imageMemoryBarrierCount = 1;
    ddep.pImageMemoryBarriers = &db;
    vkCmdPipelineBarrier2(cmd, &ddep);
  });
  if (!ok) {
    r_.destroy_image(mirror_tex_);
    r_.destroy_image(mirror_depth_);
    return false;
  }
  if (frame_set_ && sampler_) {
    r_.wait_idle();
    VkDescriptorImageInfo info{sampler_, mirror_tex_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    auto upd = [&](VkDescriptorSet target) {
      if (!target) return;
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = target;
      w.dstBinding = 5;
      w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w.pImageInfo = &info;
      vkUpdateDescriptorSets(r_.device_, 1, &w, 0, nullptr);
    };
    for (uint32_t s = 0; s < Renderer::kFramesInFlight; ++s) {
      upd(frame_sets_dip_[s]);
      upd(frame_sets_fullbeam_[s]);
    }
    detail::register_virtual_mirror(&r_, mirror_tex_.view);
    bind_post_virtual_mirror(r_, mirror_tex_.view);
    Renderer* rptr = &r_;
    retired_.push_back({std::numeric_limits<uint64_t>::max(), [rptr]() {
                          detail::unregister_virtual_mirror(rptr);
                          bind_post_virtual_mirror(*rptr, VK_NULL_HANDLE);
                        }});
  }
  mirror_valid_ = false;
  return true;
}

void Showroom::record_offscreen_view(VkCommandBuffer cmd, VkImageView color_view, VkImageView depth_view,
                                     uint32_t w, uint32_t h, const ShowroomFrame& view_frame, bool include_cars,
                                     uint32_t vx, uint32_t vy, uint32_t vw, uint32_t vh, bool clear_target,
                                     uint32_t* draw_counter) {
  const bool is_cubemap = (w == h && vw == 0u && vh == 0u);
  if (vw == 0) vw = w;
  if (vh == 0) vh = h;
  const uint32_t slot = r_.frame_slot();
  const uint32_t vidx = std::min(env_view_cursor_++, kMaxEnvViews - 1u);
  const uint32_t ring_idx = slot * kMaxEnvViews + vidx;
  env_frame_set_ = env_frame_sets_[slot][vidx];
  // Write view UBO and single-pass mode-2 sky UBO into persistently mapped per-view ring slots (zero barriers).
  write_frame_ubo(env_frame_buffer_, ring_idx, view_frame);
  if (sky_ready_ && env_sky_pipe_base_ && env_sky_sets_[0] && env_sky_ubo_.ready()) {
    SkyUniforms u{};
    fill_sky_uniforms(view_frame, u);
    if (is_cubemap && tod_active_) {
      // Apply the 6 Cube* TOD curves + TrackSettings cubeSkySaturation to dynamic cubemap sky.
      const auto& t = tod_state_;
      u.atmo_haze[0] = t.cube_atmo_haze_color[0];
      u.atmo_haze[1] = t.cube_atmo_haze_color[1];
      u.atmo_haze[2] = t.cube_atmo_haze_color[2];
      u.atmo_haze_p[0] = t.cube_atmo_haze_power;
      u.atmo_haze_p[1] = t.cube_atmo_haze_intensity;
      u.atmo_haze_p[2] = t.cube_atmo_haze_elliptical;
      u.sun_obj_col[0] = t.cube_sun_object_color[0];
      u.sun_obj_col[1] = t.cube_sun_object_color[1];
      u.sun_obj_col[2] = t.cube_sun_object_color[2];
      u.sun_obj_dir[3] = std::clamp(t.cube_sun_object_size * 0.0032f, 0.025f, 0.14f);
      u.close_back_dense_col[3] = std::clamp(t.cube_sky_saturation, 0.0f, 2.0f);
    }
    // Offscreen cubemaps & mirrors do not draw stars: always combine atmosphere + disc + clouds + fog into mode 2.
    u.cam_fwd[3] = 2.0f;
    std::memcpy(env_sky_ubo_.ptr(ring_idx), &u, sizeof(SkyUniforms));
  }

  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = color_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = clear_target ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  // Clear to TOD horizon/fog luminance rather than hardcoded daylight blue so midnight
  // rearview mirrors and dynamic cubemaps never leak bright cyan under night auto-exposure.
  if (view_frame.sun_color[3] >= 0.0f) {
    color.clearValue.color = {{std::max(0.0f, view_frame.fog_color[0] * 0.45f),
                               std::max(0.0f, view_frame.fog_color[1] * 0.45f),
                               std::max(0.0f, view_frame.fog_color[2] * 0.45f), 1.0f}};
  } else {
    color.clearValue.color = {{0.05f, 0.08f, 0.12f, 1.0f}};
  }
  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = depth_view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.clearValue.depthStencil = {r_.reverse_z() ? 0.0f : 1.0f, 0};
  VkRenderingInfo rend{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rend.renderArea = {{int32_t(vx), int32_t(vy)}, {vw, vh}};
  rend.layerCount = 1;
  rend.colorAttachmentCount = 1;
  rend.pColorAttachments = &color;
  rend.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &rend);
  const VkViewport vp{float(vx), float(vy), float(vw), float(vh), 0.0f, 1.0f};
  const VkRect2D sc{{int32_t(vx), int32_t(vy)}, {vw, vh}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);

  // Ensure lazily compiled offscreen extra pipelines (world water, trailing car glass, mirror particles) are ready.
  MirrorExtraPipes extra_pipes{};
  if (env_frame_set_) {
    std::lock_guard<std::mutex> lock(g_mirror_extra_mutex);
    auto it = g_mirror_extra_pipes.find(this);
    if (it != g_mirror_extra_pipes.end()) {
      extra_pipes = it->second;
    } else {
      extra_pipes = create_mirror_extra_pipes(r_.device_, r_.pipeline_cache(), pipeline_layout_,
                                              particle_pipe_layout_, world_pipeline_layout_, r_.bloom_format(), r_.depth_format_,
                                              r_.reverse_z());
      g_mirror_extra_pipes.emplace(this, extra_pipes);
      VkDevice dev = r_.device_;
      const Showroom* key = this;
      retired_.push_back({std::numeric_limits<uint64_t>::max(), [dev, key]() {
                            MirrorExtraPipes doomed{};
                            {
                              std::lock_guard<std::mutex> lk(g_mirror_extra_mutex);
                              auto jt = g_mirror_extra_pipes.find(key);
                              if (jt != g_mirror_extra_pipes.end()) {
                                doomed = jt->second;
                                g_mirror_extra_pipes.erase(jt);
                              }
                            }
                            if (doomed.car_glass) vkDestroyPipeline(dev, doomed.car_glass, nullptr);
                            if (doomed.particle_alpha) vkDestroyPipeline(dev, doomed.particle_alpha, nullptr);
                            if (doomed.particle_add) vkDestroyPipeline(dev, doomed.particle_add, nullptr);
                            if (doomed.world_water) vkDestroyPipeline(dev, doomed.world_water, nullptr);
                          }});
    }
  }

  // 1. Opaque AND alpha-tested (blend 0 + blend 1) world terrain, roads, mountains, trees,
  //    buildings, signs, banners, and barriers (drawn BEFORE sky for Early-Z rejection).
  //    Also collect visible water surfaces (kFlagWater = 32768) to blend over lake beds after sky.
  std::vector<const WorldGeometry::Draw*> water_draws;
  const float ground_refl_scale =
      (is_cubemap && tod_active_) ? std::clamp(tod_state_.cube_refl_ground_scale, 0.25f, 2.0f) : 1.0f;
  if (world_textured_ && (!world_draws_.empty() || !world_trees_.empty()) && env_world_opaque_ && env_frame_set_) {
    float clip[16];
    multiply(view_frame.view_projection, world_transform_, clip);
    float planes[4][4];
    for (int c = 0; c < 4; ++c) {
      const float x = clip[c * 4 + 0], y = clip[c * 4 + 1], wv = clip[c * 4 + 3];
      planes[0][c] = wv + x, planes[1][c] = wv - x, planes[2][c] = wv + y, planes[3][c] = wv - y;
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
    const auto eye_sc = [&]() -> std::array<float, 3> {
      const float e[3] = {view_frame.eye[0] - world_transform_[12], view_frame.eye[1] - world_transform_[13],
                          view_frame.eye[2] - world_transform_[14]};
      std::array<float, 3> out{};
      for (int c = 0; c < 3; ++c)
        out[size_t(c)] = world_transform_[c * 4 + 0] * e[0] + world_transform_[c * 4 + 1] * e[1] +
                         world_transform_[c * 4 + 2] * e[2];
      return out;
    }();
    const float cube_far = tod_active_ ? std::clamp(tod_state_.cube_far_distance, 120.0f, 320.0f) : 200.0f;
    const float max_dist = is_cubemap ? (low_memory_mode_ ? std::min(cube_far, 150.0f) : cube_far)
                                      : (include_cars ? (low_memory_mode_ ? 200.0f : 320.0f) : 160.0f);
    const float max_dist2 = max_dist * max_dist;
    const float cutout_dist2 =
        (!is_cubemap && include_cars) ? (low_memory_mode_ ? 110.0f * 110.0f : 165.0f * 165.0f)
                                      : (low_memory_mode_ ? 85.0f * 85.0f : 120.0f * 120.0f);
    const float layer_dist2 = (!is_cubemap && include_cars) ? 120.0f * 120.0f : 80.0f * 80.0f;
    constexpr uint32_t kExtraTexFlags =
        4u | 8u | 2048u | 4096u | 8192u | 16384u | 524288u | 2097152u | 4194304u | 8388608u;
    if (!world_draws_.empty()) world_bind_buffers(cmd, false);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 1, &env_frame_set_, 0,
                            nullptr);
    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
    VkPipeline bound_pipe = VK_NULL_HANDLE;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    for (const auto& d : world_draws_) {
      if (!d.cull) continue;
      const uint32_t fl = uint32_t(std::lround(d.push.params[1]));
      if ((fl & 2u) != 0u) continue;  // skip legacy sky dome
      const bool is_water = (fl & 32768u) != 0u;
      // Include opaque (blend 0), alpha-tested props/trees/banners (blend 1), and water planes.
      if (!is_water && d.blend > 1) continue;
      // Fast squared AABB distance cull before frustum planes (cuts ~3,400 draws down to ~150-250).
      float d2 = 0.0f;
      for (int k = 0; k < 3; ++k) {
        const float q = std::max({d.box_min[k] - eye_sc[size_t(k)], 0.0f, eye_sc[size_t(k)] - d.box_max[k]});
        d2 += q * q;
      }
      if (d2 > max_dist2) continue;
      if (!is_water && d.blend == 1 && d2 > cutout_dist2) continue;
      if (outside(d)) continue;
      if (is_water) {
        water_draws.push_back(&d);
        continue;
      }
      VkPipeline want_pipe = (d.blend == 1 && env_world_coverage_) ? env_world_coverage_ : env_world_opaque_;
      if (want_pipe != bound_pipe)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_pipe = want_pipe);

      VkDescriptorSet set = d.texture != kNoWorldTexture ? world_set(d.texture) : VK_NULL_HANDLE;
      uint32_t out_fl = fl;
      const bool has_extra = (d2 <= layer_dist2) &&
                             (d.texture_b != kNoWorldTexture || d.splat != kNoWorldTexture ||
                              d.ao_map != kNoWorldTexture || d.layer_c != kNoWorldTexture ||
                              d.light_map != kNoWorldTexture || d.spec_map != kNoWorldTexture);
      if ((set || d.texture == kNoWorldTexture) && has_extra) {
        VkDescriptorSet layered = world_layer_set(
            {d.texture, d.texture_b, d.splat, d.light_map, d.normal_map, d.ao_map, d.layer_c, d.spec_map});
        if (layered) {
          set = layered;
        } else {
          out_fl &= ~kExtraTexFlags;
        }
      } else if (!has_extra) {
        out_fl &= ~kExtraTexFlags;
      }
      if (!set) {
        set = world_white_set_;
        out_fl &= ~(1u | kExtraTexFlags);
      }
      if (set != bound_set)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 1, 1, &(bound_set = set),
                                0, nullptr);
      ShowroomPush push = d.push;
      // 1x offscreen target has no MSAA alpha-to-coverage: enable alpha test (flag 64) for blend==1.
      if (d.blend == 1) out_fl |= 64u;
      push.params[1] = float(out_fl);
      world_draw_model(d, world_transform_, push.model);
      std::memcpy(push.uv0, world_sun_, sizeof push.uv0);
      push.uv0[3] = 2.0f;
      std::memcpy(push.uv1, world_up_, sizeof push.uv1);
      push.uv1[3] = world_up_[3] * ground_refl_scale;
      vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
      if (draw_counter) ++(*draw_counter);
    }

    // 1b. Distance-capped coarse GPU/world trees (`world_trees_`) billboarded per-view in world.vert
    //     so car paint cubemaps and rearview mirrors reflect Colorado's pine & aspen forests.
    if (!world_trees_.empty() && env_world_coverage_) {
      EnvTreeCache* tc = nullptr;
      VkDeviceSize tree_vb_off = 0;
      {
        std::lock_guard<std::mutex> lock(g_env_tree_mutex);
        auto& ptr = g_env_tree_caches[this];
        if (!ptr) {
          auto c = std::make_unique<EnvTreeCache>();
          const VkDeviceSize region = VkDeviceSize(kMaxEnvTreeVerts) * sizeof(WorldVertex);
          if (r_.create_buffer(region * Renderer::kFramesInFlight, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, c->vb)) {
            if (vmaMapMemory(r_.allocator_, c->vb.allocation, &c->mapped) != VK_SUCCESS) {
              c->mapped = nullptr;
              r_.destroy_buffer(c->vb);
            }
          }
          if (c->mapped) {
            ptr = std::move(c);
            Renderer* rptr = &r_;
            const Showroom* key = this;
            retired_.push_back({std::numeric_limits<uint64_t>::max(), [rptr, key]() {
                                  std::unique_ptr<EnvTreeCache> doomed;
                                  {
                                    std::lock_guard<std::mutex> lk(g_env_tree_mutex);
                                    auto jt = g_env_tree_caches.find(key);
                                    if (jt != g_env_tree_caches.end()) {
                                      doomed = std::move(jt->second);
                                      g_env_tree_caches.erase(jt);
                                    }
                                  }
                                  if (doomed && doomed->vb.allocation) {
                                    if (doomed->mapped) vmaUnmapMemory(rptr->allocator_, doomed->vb.allocation);
                                    rptr->destroy_buffer(doomed->vb);
                                  }
                                }});
          }
        }
        if (ptr && ptr->mapped) {
          tc = ptr.get();
          const float moved2 = (eye_sc[0] - tc->eye_sc[0]) * (eye_sc[0] - tc->eye_sc[0]) +
                               (eye_sc[1] - tc->eye_sc[1]) * (eye_sc[1] - tc->eye_sc[1]) +
                               (eye_sc[2] - tc->eye_sc[2]) * (eye_sc[2] - tc->eye_sc[2]);
          if (tc->world_gen != world_generation_ || moved2 > 144.0f || tc->ver == 0) {
            tc->world_gen = world_generation_;
            for (int k = 0; k < 3; ++k) tc->eye_sc[k] = eye_sc[size_t(k)];
            const float max_tree_r = low_memory_mode_ ? 125.0f : 190.0f;
            const float max_tree_r2 = max_tree_r * max_tree_r;
            const size_t cap_trees = low_memory_mode_ ? 256u : kMaxEnvTrees;
            struct TreeCand {
              float d2;
              uint32_t tex;
              size_t idx;
            };
            std::vector<TreeCand> cands;
            cands.reserve(512);
            for (size_t i = 0; i < world_trees_.size(); ++i) {
              const auto& t = world_trees_[i];
              if (t.texture == kNoWorldTexture || t.half_width <= 0.05f || t.height <= 0.05f) continue;
              const float dx = t.base[0] - eye_sc[0];
              const float dy = t.base[1] - eye_sc[1];
              const float dz = t.base[2] - eye_sc[2];
              const float d2 = dx * dx + dy * dy + dz * dz;
              if (d2 > max_tree_r2) continue;
              cands.push_back({d2, t.texture, i});
            }
            if (cands.size() > cap_trees) {
              std::nth_element(cands.begin(), cands.begin() + ptrdiff_t(cap_trees), cands.end(),
                               [](const TreeCand& a, const TreeCand& b) { return a.d2 < b.d2; });
              cands.resize(cap_trees);
            }
            std::sort(cands.begin(), cands.end(), [](const TreeCand& a, const TreeCand& b) { return a.tex < b.tex; });
            tc->cpu.clear();
            tc->cpu.reserve(cands.size() * 6u);
            tc->batches.clear();
            static constexpr int kQuad[6] = {0, 1, 2, 0, 2, 3};
            for (const auto& c : cands) {
              const auto& t = world_trees_[c.idx];
              float ux = t.up[0], uy = t.up[1], uz = t.up[2];
              const float ul = std::sqrt(ux * ux + uy * uy + uz * uz);
              if (ul > 1e-4f) ux /= ul, uy /= ul, uz /= ul;
              else ux = 0.0f, uy = 1.0f, uz = 0.0f;
              const int8_t nx = int8_t(std::clamp(std::lround(ux * 127.0f), -127l, 127l));
              const int8_t ny = int8_t(std::clamp(std::lround(uy * 127.0f), -127l, 127l));
              const int8_t nz = int8_t(std::clamp(std::lround(uz * 127.0f), -127l, 127l));
              const uint8_t cr = uint8_t(t.rgba8 & 0xFFu);
              const uint8_t cg = uint8_t((t.rgba8 >> 8u) & 0xFFu);
              const uint8_t cb = uint8_t((t.rgba8 >> 16u) & 0xFFu);
              const uint8_t ca = uint8_t((t.rgba8 >> 24u) & 0xFFu);
              const uint32_t first_v = uint32_t(tc->cpu.size());
              for (int qi = 0; qi < 6; ++qi) {
                const int corner = kQuad[qi];
                WorldVertex v{};
                v.position[0] = t.base[0];
                v.position[1] = t.base[1];
                v.position[2] = t.base[2];
                v.normal[0] = nx;
                v.normal[1] = ny;
                v.normal[2] = nz;
                v.normal[3] = 0;
                v.uv0[0] = (corner == 1 || corner == 2) ? t.uv[2] : t.uv[0];
                v.uv0[1] = (corner >= 2) ? t.uv[1] : t.uv[3];
                v.uv1[0] = (corner == 1 || corner == 2) ? t.half_width : -t.half_width;
                v.uv1[1] = (corner >= 2) ? t.height : 0.0f;
                v.colour[0] = cr;
                v.colour[1] = cg;
                v.colour[2] = cb;
                v.colour[3] = ca;
                tc->cpu.push_back(v);
              }
              if (!tc->batches.empty() && tc->batches.back().texture == c.tex) {
                tc->batches.back().vertex_count += 6u;
              } else {
                tc->batches.push_back({c.tex, first_v, 6u});
              }
            }
            ++tc->ver;
          }
          const uint32_t slot = r_.frame_slot();
          const VkDeviceSize region = VkDeviceSize(kMaxEnvTreeVerts) * sizeof(WorldVertex);
          tree_vb_off = VkDeviceSize(slot) * region;
          if (tc->slot_ver[slot] != tc->ver && !tc->cpu.empty()) {
            std::memcpy(static_cast<uint8_t*>(tc->mapped) + tree_vb_off, tc->cpu.data(),
                        tc->cpu.size() * sizeof(WorldVertex));
            tc->slot_ver[slot] = tc->ver;
          }
        }
      }
      if (tc && !tc->batches.empty()) {
        if (bound_pipe != env_world_coverage_)
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_pipe = env_world_coverage_);
        vkCmdBindVertexBuffers(cmd, 0, 1, &tc->vb.buffer, &tree_vb_off);
        ShowroomPush tpush{};
        std::memcpy(tpush.model, world_transform_, sizeof tpush.model);
        std::memcpy(tpush.uv0, world_sun_, sizeof tpush.uv0);
        tpush.uv0[3] = 2.0f;
        std::memcpy(tpush.uv1, world_up_, sizeof tpush.uv1);
        tpush.uv1[3] = world_up_[3] * ground_refl_scale;
        tpush.tint[0] = tpush.tint[1] = tpush.tint[2] = tpush.tint[3] = 1.0f;
        tpush.params[0] = 1.0f;                  // kCoverage
        tpush.params[1] = float(1u | 32u | 64u); // kFlagTextured | kFlagBillboard | kFlagAlphaTest
        tpush.params[2] = 0.0f;
        tpush.params[3] = 0.0f;
        for (const auto& tb : tc->batches) {
          if (tb.vertex_count == 0) continue;
          VkDescriptorSet tset = world_set(tb.texture);
          if (!tset) continue;
          if (tset != bound_set)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 1, 1,
                                    &(bound_set = tset), 0, nullptr);
          vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(ShowroomPush), &tpush);
          vkCmdDraw(cmd, tb.vertex_count, 1, tb.first_vertex, 0);
          if (draw_counter) ++(*draw_counter);
        }
      }
    }
  }
  // 2. Player car's own rear bodywork (cockpit mirror pass only, clipped past mirror plane so side
  //    mirrors show rear fenders/wing) + nearby AI / traffic cars (opaque + glass in mirrors).
  if (!is_cubemap && include_cars && env_frame_set_ && cockpit_camera_active_ && draw_car_ && env_car_opaque_) {
    // Side mirrors clip past eye.z + 0.22m (shows rear haunch/fender/wing); center mirror clips past +0.65m
    // so cabin interior never blocks the rear window view while the rear spoiler/decklid stays visible.
    const float min_rear_z = view_frame.eye[2] + (std::fabs(view_frame.eye[0]) < 0.25f ? 0.65f : 0.22f);
    record_player_mirror_rear(cmd, env_car_opaque_, env_frame_set_, min_rear_z);
  }
  if (env_car_opaque_ && env_frame_set_ && !instances_.empty() && (include_cars || is_cubemap)) {
    if (is_cubemap) {
      float clip_c[4][4];
      for (int c = 0; c < 4; ++c) {
        const float x = view_frame.view_projection[c * 4 + 0];
        const float y = view_frame.view_projection[c * 4 + 1];
        const float wv = view_frame.view_projection[c * 4 + 3];
        clip_c[0][c] = wv + x;
        clip_c[1][c] = wv - x;
        clip_c[2][c] = wv + y;
        clip_c[3][c] = wv - y;
      }
      struct CarCand {
        float d2;
        size_t idx;
      };
      std::vector<CarCand> nearby;
      nearby.reserve(instances_.size());
      const float max_car_d2 = low_memory_mode_ ? (45.0f * 45.0f) : (65.0f * 65.0f);
      for (size_t i = 0; i < instances_.size(); ++i) {
        const auto& in = instances_[i];
        if (!in.car || in.fade_alpha < 0.25f) continue;
        const float px = in.model[12], py = in.model[13], pz = in.model[14];
        const float dx = px - view_frame.eye[0];
        const float dy = py - view_frame.eye[1];
        const float dz = pz - view_frame.eye[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < 2.25f || d2 > max_car_d2) continue;
        bool vis = true;
        for (const auto& pl : clip_c) {
          const float r = (std::fabs(pl[0]) + std::fabs(pl[1]) + std::fabs(pl[2])) * 3.0f;
          if (pl[0] * px + pl[1] * py + pl[2] * pz + pl[3] + r < 0.0f) {
            vis = false;
            break;
          }
        }
        if (vis) nearby.push_back({d2, i});
      }
      std::sort(nearby.begin(), nearby.end(), [](const CarCand& a, const CarCand& b) { return a.d2 < b.d2; });
      const size_t max_cars =
#ifdef __ANDROID__
          low_memory_mode_ ? 0u : 2u;
#else
          low_memory_mode_ ? 2u : 4u;
#endif
      for (size_t k = 0; k < nearby.size() && k < max_cars; ++k) {
        const auto& in = instances_[nearby[k].idx];
        in.car->record_instance_offscreen(cmd, env_car_opaque_, env_frame_set_, in.model,
                                          in.has_lamps ? &in.lamps : nullptr);
        if (draw_counter) *draw_counter += uint32_t(in.car->draw_count());
      }
    } else {
      for (const auto& in : instances_) {
        if (!in.car) continue;
        const float dx = in.model[12] - view_frame.eye[0];
        const float dy = in.model[13] - view_frame.eye[1];
        const float dz = in.model[14] - view_frame.eye[2];
        if (dx * dx + dy * dy + dz * dz > 220.0f * 220.0f) continue;
        in.car->record_instance_offscreen(cmd, env_car_opaque_, env_frame_set_, in.model,
                                          in.has_lamps ? &in.lamps : nullptr, extra_pipes.car_glass);
        if (draw_counter) *draw_counter += uint32_t(in.car->draw_count());
      }
    }
  }
  // 3. Real-time sky AFTER opaque world & cars (Early-Z rejects occluded pixels) — single-pass mode 2.
  if (sky_ready_ && env_sky_pipe_base_ && env_sky_sets_[0] && env_sky_ubo_.ready()) {
    const int disc = tod_state_.is_moon ? (1 + tod_moon_phase_) : 0;
    const uint32_t sky_off = uint32_t(env_sky_ubo_.offset(ring_idx));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, env_sky_pipe_base_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipe_layout_, 0, 1, &env_sky_sets_[disc], 1,
                            &sky_off);
    vkCmdDraw(cmd, 3, 1, 0, 0);
  }
  // 3b. Water surfaces (alpine lakes & rivers) blended over underwater beds & sky in cubemaps and mirrors.
  if (!water_draws.empty() && extra_pipes.world_water && env_frame_set_) {
    world_bind_buffers(cmd, false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, extra_pipes.world_water);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 1, &env_frame_set_, 0,
                            nullptr);
    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
    VkDescriptorSet bound_wset = VK_NULL_HANDLE;
    constexpr uint32_t kWaterExtraMask = 4096u | 4194304u;  // normal_map | layer_b water normal
    for (const WorldGeometry::Draw* dp : water_draws) {
      const auto& d = *dp;
      VkDescriptorSet set = d.texture != kNoWorldTexture ? world_set(d.texture) : VK_NULL_HANDLE;
      uint32_t out_fl = uint32_t(std::lround(d.push.params[1]));
      if ((set || d.texture == kNoWorldTexture) &&
          (d.normal_map != kNoWorldTexture || d.texture_b != kNoWorldTexture)) {
        VkDescriptorSet layered = world_layer_set(
            {d.texture, d.texture_b, d.splat, d.light_map, d.normal_map, d.ao_map, d.layer_c, d.spec_map});
        if (layered) set = layered;
        else out_fl &= ~kWaterExtraMask;
      } else {
        out_fl &= ~kWaterExtraMask;
      }
      if (!set) set = world_white_set_;
      if (set != bound_wset)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 1, 1,
                                &(bound_wset = set), 0, nullptr);
      ShowroomPush push = d.push;
      push.params[1] = float(out_fl);
      world_draw_model(d, world_transform_, push.model);
      std::memcpy(push.uv0, world_sun_, sizeof push.uv0);
      push.uv0[3] = 2.0f;
      std::memcpy(push.uv1, world_up_, sizeof push.uv1);
      push.uv1[3] = world_up_[3] * ground_refl_scale;
      vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(ShowroomPush), &push);
      vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
      if (draw_counter) ++(*draw_counter);
    }
  }
  // 4. Staged oncoming car headlights & streetlamp glow sprites (1-sample additive HDR pass).
  if (carlights_ready_ && glow_count_ > 0 && glow_pipe_layout_ && glow_set_ && glow_buf_.ready() && env_frame_set_) {
    VkPipeline glow_p = VK_NULL_HANDLE;
    {
      std::lock_guard<std::mutex> lock(g_env_glow_mutex);
      auto it = g_env_glow_pipes.find(this);
      if (it != g_env_glow_pipes.end()) {
        glow_p = it->second;
      } else {
        glow_p = create_env_glow_pipeline(r_.device_, r_.pipeline_cache(), glow_pipe_layout_, r_.bloom_format(), r_.depth_format_,
                                          r_.reverse_z());
        g_env_glow_pipes.emplace(this, glow_p);
        if (glow_p) {
          VkDevice dev = r_.device_;
          const Showroom* key = this;
          retired_.push_back({std::numeric_limits<uint64_t>::max(), [dev, key]() {
                                VkPipeline doomed = VK_NULL_HANDLE;
                                {
                                  std::lock_guard<std::mutex> lk(g_env_glow_mutex);
                                  auto jt = g_env_glow_pipes.find(key);
                                  if (jt != g_env_glow_pipes.end()) {
                                    doomed = jt->second;
                                    g_env_glow_pipes.erase(jt);
                                  }
                                }
                                if (doomed) vkDestroyPipeline(dev, doomed, nullptr);
                              }});
        }
      }
    }
    if (glow_p) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glow_p);
      const VkDescriptorSet gsets[2] = {env_frame_set_, glow_set_};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glow_pipe_layout_, 0, 2, gsets, 0, nullptr);
      vkCmdBindVertexBuffers(cmd, 0, 1, &glow_buf_.buf.buffer, &glow_vb_offset_);
      const uint32_t max_glows = is_cubemap ? (low_memory_mode_ ? 96u : 256u) : 512u;
      vkCmdDraw(cmd, 6, std::min(glow_count_, max_glows), 0, 0);
    }
  }
  // 5. RenderInMirror particles (tyre smoke, dust clouds, backfire flames) in cockpit mirrors.
  if (!is_cubemap && include_cars && vfx_ready_ && particle_set_ && particle_pipe_layout_ &&
      (particle_alpha_count_ > 0 || particle_add_count_ > 0) && env_frame_set_) {
    struct ParticlePush {
      float viewport_near[4];
      float sun_dir[4];
      float up_dir[4];
    } pc{};
    pc.viewport_near[0] = 1.0f / float(std::max(1u, vw));
    pc.viewport_near[1] = 1.0f / float(std::max(1u, vh));
    pc.viewport_near[2] = 0.35f;
    pc.viewport_near[3] = 0.50f;  // clip particles inside mirror housing
    for (int i = 0; i < 3; ++i) {
      pc.sun_dir[i] = world_sun_[i];
      pc.up_dir[i] = world_up_[i];
    }
    pc.sun_dir[3] = world_up_[3];
    pc.up_dir[3] = 0.0f;  // no main-pass scene depth sampling inside offscreen mirror pass
    const VkDescriptorSet psets[2] = {env_frame_set_, particle_set_};
    const VkDeviceSize at =
        dyn_sync(particle_buf_, VkDeviceSize(particle_alpha_count_ + particle_add_count_) * sizeof(ParticleGpu));
    if (particle_alpha_count_ > 0 && extra_pipes.particle_alpha) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, extra_pipes.particle_alpha);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particle_pipe_layout_, 0, 2, psets, 0, nullptr);
      vkCmdPushConstants(cmd, particle_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof pc, &pc);
      vkCmdBindVertexBuffers(cmd, 0, 1, &particle_buf_.buf.buffer, &at);
      vkCmdDraw(cmd, 6, particle_alpha_count_, 0, 0);
    }
    if (particle_add_count_ > 0 && extra_pipes.particle_add) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, extra_pipes.particle_add);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particle_pipe_layout_, 0, 2, psets, 0, nullptr);
      vkCmdPushConstants(cmd, particle_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof pc, &pc);
      vkCmdBindVertexBuffers(cmd, 0, 1, &particle_buf_.buf.buffer, &at);
      vkCmdDraw(cmd, 6, particle_add_count_, 0, particle_alpha_count_);
    }
  }
  vkCmdEndRendering(cmd);
}

void Showroom::record_env_and_mirror(VkCommandBuffer cmd) {
  env_draws_ = 0;
  mirror_draws_ = 0;
  env_view_cursor_ = 0;
  if (!dyn_env_ready_ || !env_frame_sets_[0][0]) return;
  auto make_view_proj = [&](const std::array<float, 3>& eye, const std::array<float, 3>& right,
                            const std::array<float, 3>& up, const std::array<float, 3>& fwd,
                            float tan_x, float tan_y, float out[16], float near_p = 0.5f) {
    const float dx = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
    const float dy = -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]);
    const float dz = -(fwd[0] * eye[0] + fwd[1] * eye[1] + fwd[2] * eye[2]);
    const float far_p = 8000.0f;
    out[0] = right[0] / tan_x; out[4] = right[1] / tan_x; out[8]  = right[2] / tan_x; out[12] = dx / tan_x;
    out[1] = -up[0] / tan_y;   out[5] = -up[1] / tan_y;   out[9]  = -up[2] / tan_y;   out[13] = -dy / tan_y;
    if (r_.reverse_z()) {
      out[2] = 0.0f; out[6] = 0.0f; out[10] = 0.0f; out[14] = near_p;
    } else {
      const float k = far_p / (far_p - near_p);
      out[2] = fwd[0] * k; out[6] = fwd[1] * k; out[10] = fwd[2] * k; out[14] = dz * k - near_p * k;
    }
    out[3] = fwd[0]; out[7] = fwd[1]; out[11] = fwd[2]; out[15] = dz;
  };
  auto game_to_render = [&](const std::array<float, 3>& g) -> std::array<float, 3> {
    return {world_transform_[0] * g[0] + world_transform_[4] * g[1] + world_transform_[8] * g[2],
            world_transform_[1] * g[0] + world_transform_[5] * g[1] + world_transform_[9] * g[2],
            world_transform_[2] * g[0] + world_transform_[6] * g[1] + world_transform_[10] * g[2]};
  };

  // Vulkan cubemap face basis in game space (+X, -X, +Y, -Y, +Z, -Z).
  static constexpr std::array<float, 3> kFwd[6] = {
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  static constexpr std::array<float, 3> kRight[6] = {
      {0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
  static constexpr std::array<float, 3> kUp[6] = {
      {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};

  // Schedule dynamic cubemap face updates per preset.reflection_rate and stagger 3-band cockpit mirrors.
  const modern::ReflectionStep rstep = modern::schedule_reflections(
      world_frame_, reflection_rate_, dyn_env_initialized_, mirror_enabled_, mirror_valid_);
  const uint32_t env_sz = dyn_env_size_;
  const uint32_t env_mips = std::min(dyn_env_mips_, kDynEnvMips);
  // Probe center slightly above car roof in render space.
  const std::array<float, 3> cube_eye = {0.0f, 1.35f, 0.0f};
  // Pass A: Render updated face(s) into mip 0 and transition mip 0 to SHADER_READ_ONLY_OPTIMAL.
  if (rstep.render_cubemap) {
    for (uint32_t i = 0; i < rstep.face_count; ++i) {
      const uint32_t f = (rstep.first_face + i) % 6;
      VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = dyn_env_.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f, 1};
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &b;
      vkCmdPipelineBarrier2(cmd, &dep);

      ShowroomFrame vf = frame_;
      if (world_lit_ && vf.drive[3] < 0.5f) {
        for (int k = 0; k < 3; ++k) {
          vf.key_light[k] = world_sun_[k];
          vf.drive[k] = world_up_[k];
          vf.tint[k] = vf.reflect_tint[k] = 1.0f;
        }
        vf.key_light[3] = 1.0f;
        vf.fill_light[3] = 0.0f;
        vf.options[0] = 1.0f;
        vf.drive[3] = 1.0f;
      }
      vf.shadow[0] = 0.0f;
      vf.shadow_far_bias[1] = vf.shadow_far_bias[2] = vf.shadow_far_bias[3] = 0.0f;
      // Clip player car's own self-glows within 3.2m of roof probe while keeping oncoming car & streetlamp glows.
      vf.shadow_bias[1] = 3.2f;
      if (tod_active_) {
        const float sat = std::clamp(tod_state_.cube_sky_saturation, 0.0f, 2.0f);
        const float gscale = std::clamp(tod_state_.cube_refl_ground_scale, 0.25f, 2.0f);
        desaturate_rgb(vf.sky_color, sat);
        desaturate_rgb(vf.fog_color, sat);
        for (int c = 0; c < 3; ++c) vf.ground_color[c] *= gscale;
      }
      vf.eye[0] = cube_eye[0];
      vf.eye[1] = cube_eye[1];
      vf.eye[2] = cube_eye[2];
      vf.eye[3] = 1.0f;
      make_view_proj(cube_eye, game_to_render(kRight[f]), game_to_render(kUp[f]), game_to_render(kFwd[f]),
                     1.0f, 1.0f, vf.view_projection);
      std::memcpy(vf.transparent_view_projection, vf.view_projection, sizeof vf.view_projection);

      record_offscreen_view(cmd, dyn_env_faces_[f], dyn_env_depth_.view, env_sz, env_sz, vf, true,
                            0u, 0u, 0u, 0u, true, &env_draws_);

      b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier2(cmd, &dep);
    }
    r_.mark(cmd, "env_cubemap");

    // Pass B: Cross-face GGX prefilter mips 1..env_mips-1 sampling mip 0 across all 6 faces
    // seamlessly (batched into 2 pipeline barriers instead of 2*(env_mips-1)*face_count barriers).
    if (env_prefilter_pipe_ && env_prefilter_set_ && env_mips > 1u) {
      VkImageMemoryBarrier2 pre_bars[6]{};
      VkImageMemoryBarrier2 post_bars[6]{};
      const uint32_t bar_n = std::min(rstep.face_count, 6u);
      for (uint32_t i = 0; i < bar_n; ++i) {
        const uint32_t f = (rstep.first_face + i) % 6;
        pre_bars[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        pre_bars[i].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        pre_bars[i].srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        pre_bars[i].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        pre_bars[i].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        pre_bars[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        pre_bars[i].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        pre_bars[i].srcQueueFamilyIndex = pre_bars[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre_bars[i].image = dyn_env_.image;
        pre_bars[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 1u, env_mips - 1u, f, 1u};

        post_bars[i] = pre_bars[i];
        post_bars[i].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        post_bars[i].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        post_bars[i].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        post_bars[i].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        post_bars[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        post_bars[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
      VkDependencyInfo pre_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      pre_dep.imageMemoryBarrierCount = bar_n;
      pre_dep.pImageMemoryBarriers = pre_bars;
      vkCmdPipelineBarrier2(cmd, &pre_dep);

      for (uint32_t i = 0; i < bar_n; ++i) {
        const uint32_t f = (rstep.first_face + i) % 6;
        uint32_t mip_w = env_sz;
        for (uint32_t mip = 1; mip < env_mips; ++mip) {
          mip_w = std::max(1u, mip_w / 2u);
          VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
          ca.imageView = dyn_env_mip_faces_[mip][f];
          ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
          ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
          VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
          ri.renderArea = {{0, 0}, {mip_w, mip_w}};
          ri.layerCount = 1;
          ri.colorAttachmentCount = 1;
          ri.pColorAttachments = &ca;
          vkCmdBeginRendering(cmd, &ri);
          const VkViewport vp{0, 0, float(mip_w), float(mip_w), 0.0f, 1.0f};
          const VkRect2D sc{{0, 0}, {mip_w, mip_w}};
          vkCmdSetViewport(cmd, 0, 1, &vp);
          vkCmdSetScissor(cmd, 0, 1, &sc);
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, env_prefilter_pipe_);
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, env_prefilter_pipe_layout_, 0, 1,
                                  &env_prefilter_set_, 0, nullptr);
          const float rough = float(mip) / float(env_mips - 1u);
          const float pp[4] = {float(f), rough, float(mip), 1.0f / float(mip_w)};
          vkCmdPushConstants(cmd, env_prefilter_pipe_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pp), pp);
          vkCmdDraw(cmd, 3, 1, 0, 0);
          vkCmdEndRendering(cmd);
        }
      }

      VkDependencyInfo post_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      post_dep.imageMemoryBarrierCount = bar_n;
      post_dep.pImageMemoryBarriers = post_bars;
      vkCmdPipelineBarrier2(cmd, &post_dep);
    }
    r_.mark(cmd, "env_prefilter");
    dyn_env_initialized_ = true;
  }

  // Dedicated 3-band rearview mirror pass (1024x256):
  //   Band 0 [0..341): mirrorLeft, Band 1 [341..683): mirrorMiddle, Band 2 [683..1024): mirrorRight.
  // - Cockpit camera: all 3 bands on initial populate, then center mirror (band 1) EVERY frame
  //   plus alternating left (0) / right (2) side mirrors on even/odd frames.
  // - Bumper / Hood camera (virtual_mirror_active_): center mirror (band 1) EVERY frame.
  const MirrorBandSchedule msched =
      schedule_cockpit_mirror_bands(world_frame_, cockpit_camera_active_, virtual_mirror_active_, mirror_valid_);
  if (msched.count > 0 && mirror_tex_.view && mirror_depth_.view) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = mirror_valid_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = mirror_tex_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);

    struct MirrorBand {
      uint32_t x, w;
      std::array<float, 3> eye, fwd;
      float tx, ty;
    };
    const float sl = 1.0f / std::sqrt(0.20f * 0.20f + 0.04f * 0.04f + 0.98f * 0.98f);
    // Per-car mirror camera origins derived from actual mirrorLeft/mirrorMiddle/mirrorRight bounds
    // (or scaled car bbox fallback when a mirror part is absent). For Bumper/Hood virtual mirror,
    // lift center probe slightly above roof height so the rear view clears the car cleanly.
    const std::array<float, 3> center_eye =
        cockpit_camera_active_
            ? mirror_origins_[1]
            : std::array<float, 3>{0.0f, have_bounds_ ? (bounds_max_[1] + 0.12f) : 1.38f,
                                   have_bounds_ ? (bounds_max_[2] + 0.15f) : 1.85f};
    const MirrorBand bands[3] = {
        {0u, 341u, mirror_origins_[0], {-0.20f * sl, -0.04f * sl, 0.98f * sl}, 0.50f, 0.28f},
        {341u, 342u, center_eye, {0.0f, -0.02f, 0.9998f}, 0.58f, 0.22f},
        {683u, 341u, mirror_origins_[2], {0.20f * sl, -0.04f * sl, 0.98f * sl}, 0.50f, 0.28f},
    };
    for (uint32_t bi = 0; bi < msched.count; ++bi) {
      const auto& mb = bands[msched.bands[bi] % 3u];
      // right = cross(fwd, up=(0,1,0)) = (-fwd.z, 0, fwd.x)
      const std::array<float, 3> mright = {-mb.fwd[2], 0.0f, mb.fwd[0]};
      ShowroomFrame mf = frame_;
      if (world_lit_ && mf.drive[3] < 0.5f) {
        for (int k = 0; k < 3; ++k) {
          mf.key_light[k] = world_sun_[k];
          mf.drive[k] = world_up_[k];
          mf.tint[k] = mf.reflect_tint[k] = 1.0f;
        }
        mf.key_light[3] = 1.0f;
        mf.fill_light[3] = 0.0f;
        mf.options[0] = 1.0f;
        mf.drive[3] = 1.0f;
      }
      mf.shadow[0] = 0.0f;
      mf.shadow_far_bias[1] = mf.shadow_far_bias[2] = mf.shadow_far_bias[3] = 0.0f;
      mf.eye[0] = mb.eye[0];
      mf.eye[1] = mb.eye[1];
      mf.eye[2] = mb.eye[2];
      mf.eye[3] = 1.0f;
      make_view_proj(mb.eye, mright, {0.0f, 1.0f, 0.0f}, mb.fwd, mb.tx, mb.ty, mf.view_projection, 0.35f);
      std::memcpy(mf.transparent_view_projection, mf.view_projection, sizeof mf.view_projection);
      record_offscreen_view(cmd, mirror_tex_.view, mirror_depth_.view, kMirrorWidth, kMirrorHeight, mf, true,
                            mb.x, 0u, mb.w, kMirrorHeight, true, &mirror_draws_);
    }

    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
    mirror_valid_ = true;
    r_.mark(cmd, "cockpit_mirrors");
  } else if (!mirror_enabled_) {
    mirror_valid_ = false;
  }
  detail::set_virtual_mirror_active(&r_, virtual_mirror_active_ && mirror_valid_ && mirror_tex_.view != VK_NULL_HANDLE);
}

}  // namespace carview
