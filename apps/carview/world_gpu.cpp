#include "world_gpu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <vk_mem_alloc.h>

#include "hiz_gen_comp_spv.h"
#include "showroom.h"  // WorldVertex
#include "tree_cull_comp_spv.h"
#include "world_cull_comp_spv.h"
#include "world_gpu_frag_spv.h"
#include "world_gpu_opaque_frag_spv.h"
#include "world_gpu_vert_spv.h"
#include "world_tree_gpu_vert_spv.h"

namespace carview {

namespace {
#define WG_TRY(call, what)                                                        \
  do {                                                                            \
    const VkResult wg_r = (call);                                                 \
    if (wg_r != VK_SUCCESS) {                                                     \
      error = std::string(what) + " failed (" + std::to_string(int(wg_r)) + ")";  \
      return false;                                                               \
    }                                                                             \
  } while (0)

void buffer_barrier(VkCommandBuffer cmd, VkBuffer buffer, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
  VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  b.srcStageMask = src_stage;
  b.srcAccessMask = src_access;
  b.dstStageMask = dst_stage;
  b.dstAccessMask = dst_access;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.buffer = buffer;
  b.size = VK_WHOLE_SIZE;
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.bufferMemoryBarrierCount = 1;
  dep.pBufferMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &dep);
}

void img_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                 VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                 VkAccessFlags2 dst_access, uint32_t base_mip, uint32_t mip_count) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = src_stage;
  b.srcAccessMask = src_access;
  b.dstStageMask = dst_stage;
  b.dstAccessMask = dst_access;
  b.oldLayout = old_layout;
  b.newLayout = new_layout;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base_mip, mip_count, 0, 1};
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &dep);
}

struct CullUniformsGpu {
  float planes[4][4];
  float eye_reach[4];
  uint32_t info[4];
  float prev_vp[16];
  uint32_t hiz_info[4];
};
static_assert(sizeof(CullUniformsGpu) == 176);

}  // namespace

WorldGpu::~WorldGpu() {
  VkDevice d = r_.device_;
  if (!d) return;
  std::lock_guard<std::mutex> lock(mutex_);
  destroy_generation(generation_);
  for (SlotBuffers& s : slots_) {
    if (s.stats_mapped) vmaUnmapMemory(r_.allocator_, s.stats.allocation);
    if (s.tree_stats_mapped) vmaUnmapMemory(r_.allocator_, s.tree_stats.allocation);
    r_.destroy_buffer(s.commands);
    r_.destroy_buffer(s.counts);
    r_.destroy_buffer(s.stats);
    r_.destroy_buffer(s.cull_ubo);
    r_.destroy_buffer(s.tree_vis);
    r_.destroy_buffer(s.tree_cmd);
    r_.destroy_buffer(s.tree_stats);
    if (s.pool) vkDestroyDescriptorPool(d, s.pool, nullptr);
  }
  for (VkImageView v : hiz_mip_views_) {
    if (v) vkDestroyImageView(d, v, nullptr);
  }
  if (hiz_sampler_) vkDestroySampler(d, hiz_sampler_, nullptr);
  r_.destroy_image(hiz_image_);
  for (VkPipeline p :
       {opaque_, opaque_after_z_, opaque_dither_, z_prepass_, coverage_, tree_coverage_, cull_, tree_cull_, hiz_gen_}) {
    if (p) vkDestroyPipeline(d, p, nullptr);
  }
  if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
  if (cull_pipeline_layout_) vkDestroyPipelineLayout(d, cull_pipeline_layout_, nullptr);
  if (hiz_pipeline_layout_) vkDestroyPipelineLayout(d, hiz_pipeline_layout_, nullptr);
  if (bindless_pool_) vkDestroyDescriptorPool(d, bindless_pool_, nullptr);
  if (table_pool_) vkDestroyDescriptorPool(d, table_pool_, nullptr);
  if (vis_pool_) vkDestroyDescriptorPool(d, vis_pool_, nullptr);
  if (bindless_layout_) vkDestroyDescriptorSetLayout(d, bindless_layout_, nullptr);
  if (table_layout_) vkDestroyDescriptorSetLayout(d, table_layout_, nullptr);
  if (vis_layout_) vkDestroyDescriptorSetLayout(d, vis_layout_, nullptr);
  if (cull_layout_) vkDestroyDescriptorSetLayout(d, cull_layout_, nullptr);
  if (hiz_set_layout_) vkDestroyDescriptorSetLayout(d, hiz_set_layout_, nullptr);
  r_.destroy_buffer(vertices_);
  r_.destroy_buffer(indices_);
  r_.destroy_buffer(empty_ssbo_);
}

VkDeviceSize WorldGpu::arena_bytes() const { return vertices_.size + indices_.size; }

bool WorldGpu::init(const Setup& setup, std::string& error) {
  setup_ = setup;
  VkDevice d = r_.device_;
  resident_.reset(setup.vertex_capacity, setup.index_capacity);
  const VkBufferUsageFlags rt_geom_flags =
      r_.rt_supported()
          ? (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
          : 0u;
  if (!r_.create_buffer(VkDeviceSize(setup.vertex_capacity) * sizeof(WorldVertex),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rt_geom_flags,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vertices_) ||
      !r_.create_buffer(VkDeviceSize(setup.index_capacity) * sizeof(uint32_t),
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rt_geom_flags,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, indices_) ||
      !r_.create_buffer(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, empty_ssbo_)) {
    error = "world arena buffers";
    return false;
  }
  // Split bindless array (set 1): binding 0 = 1x immutable SAMPLER,
  // binding 1 = SAMPLED_IMAGE[kMaxTextures] (keeps per-stage sampler count = 1
  // for Apple Silicon MoltenVK and Android Turnip/Adreno GPUs).
  {
    const VkShaderStageFlags bindless_stages =
        VK_SHADER_STAGE_FRAGMENT_BIT | (r_.rt_supported() ? VK_SHADER_STAGE_COMPUTE_BIT : 0u);
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, bindless_stages, &setup.sampler},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures, bindless_stages, nullptr},
    };
    const VkDescriptorBindingFlags flags[2] = {
        0,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
    };
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = 2;
    bf.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bf;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 2;
    li.pBindings = bindings;
    WG_TRY(vkCreateDescriptorSetLayout(d, &li, nullptr, &bindless_layout_), "bindless layout");
    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    WG_TRY(vkCreateDescriptorPool(d, &pi, nullptr, &bindless_pool_), "bindless pool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = bindless_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &bindless_layout_;
    WG_TRY(vkAllocateDescriptorSets(d, &ai, &bindless_set_), "bindless set");
    std::vector<VkDescriptorImageInfo> infos(kMaxTextures,
                                             {VK_NULL_HANDLE, setup.white, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    infos[kDefaultBlack].imageView = setup.black;
    infos[kDefaultFlatNormal].imageView = setup.flat_normal ? setup.flat_normal : setup.white;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = bindless_set_;
    w.dstBinding = 1;
    w.descriptorCount = kMaxTextures;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = infos.data();
    vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
  }
  // Table (set 2), visible tree indices (set 3), cull pass (layout), and hiz pass (layout).
  {
    VkDescriptorSetLayoutBinding tb[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = tb;
    WG_TRY(vkCreateDescriptorSetLayout(d, &li, nullptr, &table_layout_), "table layout");

    VkDescriptorSetLayoutBinding vb{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    li.bindingCount = 1;
    li.pBindings = &vb;
    WG_TRY(vkCreateDescriptorSetLayout(d, &li, nullptr, &vis_layout_), "vis layout");

    VkDescriptorSetLayoutBinding cb[5] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    li.bindingCount = 5;
    li.pBindings = cb;
    WG_TRY(vkCreateDescriptorSetLayout(d, &li, nullptr, &cull_layout_), "cull layout");

    VkDescriptorSetLayoutBinding hb[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    li.bindingCount = 2;
    li.pBindings = hb;
    WG_TRY(vkCreateDescriptorSetLayout(d, &li, nullptr, &hiz_set_layout_), "hiz layout");

    const VkDescriptorPoolSize tsize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 8;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &tsize;
    WG_TRY(vkCreateDescriptorPool(d, &pi, nullptr, &table_pool_), "table pool");

    const VkDescriptorPoolSize vsize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Renderer::kFramesInFlight};
    pi.flags = 0;
    pi.maxSets = Renderer::kFramesInFlight;
    pi.pPoolSizes = &vsize;
    WG_TRY(vkCreateDescriptorPool(d, &pi, nullptr, &vis_pool_), "vis pool");
  }
  const VkDescriptorSetLayout set_layouts[4] = {setup.frame_layout, bindless_layout_, table_layout_, vis_layout_};
  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 4;
  pl.pSetLayouts = set_layouts;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = &push;
  WG_TRY(vkCreatePipelineLayout(d, &pl, nullptr, &layout_), "world gpu pipeline layout");

  pl.setLayoutCount = 1;
  pl.pSetLayouts = &cull_layout_;
  pl.pushConstantRangeCount = 0;
  pl.pPushConstantRanges = nullptr;
  WG_TRY(vkCreatePipelineLayout(d, &pl, nullptr, &cull_pipeline_layout_), "cull pipeline layout");

  VkPushConstantRange hpush{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &hiz_set_layout_;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = &hpush;
  WG_TRY(vkCreatePipelineLayout(d, &pl, nullptr, &hiz_pipeline_layout_), "hiz pipeline layout");

  if (!create_hiz(error) || !create_pipelines(error) || !create_buffers(error)) return false;
  ready_ = true;
  return true;
}

bool WorldGpu::create_hiz(std::string& error) {
  VkDevice d = r_.device_;
  if (!r_.create_image(kHizWidth, kHizHeight, kHizMips, VK_FORMAT_R32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, hiz_image_)) {
    error = "hiz image";
    return false;
  }
  for (uint32_t m = 0; m < kHizMips; ++m) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = hiz_image_.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
    WG_TRY(vkCreateImageView(d, &vi, nullptr, &hiz_mip_views_[m]), "hiz mip view");
  }
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = VK_FILTER_NEAREST;
  si.minFilter = VK_FILTER_NEAREST;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.maxLod = float(kHizMips - 1);
  WG_TRY(vkCreateSampler(d, &si, nullptr, &hiz_sampler_), "hiz sampler");

  const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
    img_barrier(cmd, hiz_image_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, kHizMips);
    const VkClearColorValue clr{{r_.reverse_z() ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f}};
    const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, kHizMips, 0, 1};
    vkCmdClearColorImage(cmd, hiz_image_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &rng);
    img_barrier(cmd, hiz_image_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, kHizMips);
  });
  if (!ok) {
    error = "hiz init clear";
    return false;
  }
  hiz_valid_ = false;
  return true;
}

bool WorldGpu::create_pipelines(std::string& error) {
  VkDevice d = r_.device_;
  auto module = [&](const uint32_t* code, size_t bytes, VkShaderModule& out) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    return vkCreateShaderModule(d, &info, nullptr, &out);
  };
  VkShaderModule vert = VK_NULL_HANDLE, tree_vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE,
                 frag_opaque = VK_NULL_HANDLE, cull_comp = VK_NULL_HANDLE, tree_comp = VK_NULL_HANDLE,
                 hiz_comp = VK_NULL_HANDLE;
  WG_TRY(module(kWorld_gpu_vert_spv, sizeof(kWorld_gpu_vert_spv), vert), "world_gpu.vert");
  WG_TRY(module(kWorld_tree_gpu_vert_spv, sizeof(kWorld_tree_gpu_vert_spv), tree_vert), "world_tree_gpu.vert");
  WG_TRY(module(kWorld_gpu_frag_spv, sizeof(kWorld_gpu_frag_spv), frag), "world_gpu.frag");
  WG_TRY(module(kWorld_gpu_opaque_frag_spv, sizeof(kWorld_gpu_opaque_frag_spv), frag_opaque), "world_gpu_opaque.frag");
  WG_TRY(module(kWorld_cull_comp_spv, sizeof(kWorld_cull_comp_spv), cull_comp), "world_cull.comp");
  WG_TRY(module(kTree_cull_comp_spv, sizeof(kTree_cull_comp_spv), tree_comp), "tree_cull.comp");
  WG_TRY(module(kHiz_gen_comp_spv, sizeof(kHiz_gen_comp_spv), hiz_comp), "hiz_gen.comp");

  std::vector<std::function<VkResult(VkPipelineCache)>> tasks;
  tasks.reserve(9);
  auto queue_comp = [&](VkShaderModule mod, VkPipelineLayout playout, VkPipeline& out) {
    VkPipeline* dst = &out;
    tasks.emplace_back([d, mod, playout, dst](VkPipelineCache cache) -> VkResult {
      VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod,
                  "main", nullptr};
      ci.layout = playout;
      return vkCreateComputePipelines(d, cache, 1, &ci, nullptr, dst);
    });
  };
  queue_comp(cull_comp, cull_pipeline_layout_, cull_);
  queue_comp(tree_comp, cull_pipeline_layout_, tree_cull_);
  queue_comp(hiz_comp, hiz_pipeline_layout_, hiz_gen_);

  const VkBool32 spec_values[2] = {r_.post() ? VK_TRUE : VK_FALSE, r_.reverse_z() ? VK_TRUE : VK_FALSE};
  const VkSpecializationMapEntry spec_entries[2] = {{0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}};
  const VkSpecializationInfo spec{2, spec_entries, sizeof spec_values, spec_values};

  VkVertexInputBindingDescription binding{0, sizeof(WorldVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  const VkVertexInputAttributeDescription attributes[7] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(WorldVertex, position)},
      {1, 0, VK_FORMAT_R8G8B8A8_SNORM, offsetof(WorldVertex, normal)},
      {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WorldVertex, uv0)},
      {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WorldVertex, uv1)},
      {4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(WorldVertex, colour)},
      {5, 0, VK_FORMAT_R16G16B16A16_SINT, offsetof(WorldVertex, centre)},
      {6, 0, VK_FORMAT_R16G16_SFLOAT, offsetof(WorldVertex, uv2)}};
  VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  input.vertexBindingDescriptionCount = 1;
  input.pVertexBindingDescriptions = &binding;
  input.vertexAttributeDescriptionCount = 7;
  input.pVertexAttributeDescriptions = attributes;
  VkPipelineVertexInputStateCreateInfo empty_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  // world_transform_ includes the game->render Z mirror (det = -1), flipping scenery CCW to CW.
  raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
  raster.lineWidth = 1.0f;
  raster.depthBiasEnable = VK_TRUE;
  const VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                 VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = r_.vrs_supported() ? 4u : 3u;
  dynamic.pDynamicStates = dyn;
  VkPipelineColorBlendAttachmentState attachment{};
  attachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;

  const VkFormat color_format = r_.scene_format();
  auto queue_gfx = [&](VkShaderModule vs, VkShaderModule fs, const VkPipelineVertexInputStateCreateInfo& vi,
                       VkCullModeFlags cull, bool coverage, bool depth_write, VkCompareOp depth_op, bool color_out,
                       VkPipeline& out) {
    const VkPipelineVertexInputStateCreateInfo* vi_ptr = &vi;
    VkPipeline* dst = &out;
    tasks.emplace_back([&, vs, fs, vi_ptr, cull, coverage, depth_write, depth_op, color_out,
                        dst](VkPipelineCache cache) -> VkResult {
      const VkPipelineShaderStageCreateInfo stages[2] = {
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main",
           &spec},
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main",
           &spec}};
      VkPipelineRasterizationStateCreateInfo rstate = raster;
      rstate.cullMode = cull;
      VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      multisample.rasterizationSamples = r_.samples_;
      multisample.alphaToCoverageEnable = (coverage && r_.samples_ != VK_SAMPLE_COUNT_1_BIT) ? VK_TRUE : VK_FALSE;
      VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      depth.depthTestEnable = VK_TRUE;
      depth.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
      depth.depthCompareOp = depth_op;
      VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
      rendering.colorAttachmentCount = color_out ? 1u : 0u;
      rendering.pColorAttachmentFormats = color_out ? &color_format : nullptr;
      rendering.depthAttachmentFormat = r_.depth_format_;
      VkPipelineColorBlendAttachmentState catt = attachment;
      if (coverage && r_.post()) {
        // Alpha-to-coverage consumes fragment .a before colorWriteMask; keeping A_BIT off prevents
        // matte foliage/trees from stamping gloss into hdr_.a for Hi-Z SSR.
        catt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
      }
      VkPipelineColorBlendStateCreateInfo cblend = blend;
      cblend.attachmentCount = color_out ? 1u : 0u;
      cblend.pAttachments = color_out ? &catt : nullptr;
      VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      info.pNext = &rendering;
      info.stageCount = fs ? 2u : 1u;
      info.pStages = stages;
      info.pVertexInputState = vi_ptr;
      info.pInputAssemblyState = &assembly;
      info.pViewportState = &viewport;
      info.pRasterizationState = &rstate;
      info.pMultisampleState = &multisample;
      info.pDepthStencilState = &depth;
      info.pColorBlendState = &cblend;
      info.pDynamicState = &dynamic;
      info.layout = layout_;
      return vkCreateGraphicsPipelines(d, cache, 1, &info, nullptr, dst);
    });
  };

  const VkCullModeFlags opaque_cull = r_.backface_cull() ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
  const VkCompareOp less_eq = r_.reverse_z() ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
  queue_gfx(vert, frag_opaque, input, opaque_cull, false, true, less_eq, true, opaque_);
  queue_gfx(vert, frag_opaque, input, opaque_cull, false, false, VK_COMPARE_OP_EQUAL, true, opaque_after_z_);
  queue_gfx(vert, frag, input, opaque_cull, false, true, less_eq, true, opaque_dither_);
  queue_gfx(vert, VK_NULL_HANDLE, input, opaque_cull, false, true, less_eq, false, z_prepass_);
  queue_gfx(vert, frag, input, VK_CULL_MODE_NONE, true, true, less_eq, true, coverage_);
  queue_gfx(tree_vert, frag, empty_input, VK_CULL_MODE_NONE, true, true, less_eq, true, tree_coverage_);

  const bool pipes_ok = r_.compile_pipelines_parallel(tasks, error);

  vkDestroyShaderModule(d, vert, nullptr);
  vkDestroyShaderModule(d, tree_vert, nullptr);
  vkDestroyShaderModule(d, frag, nullptr);
  vkDestroyShaderModule(d, frag_opaque, nullptr);
  vkDestroyShaderModule(d, cull_comp, nullptr);
  vkDestroyShaderModule(d, tree_comp, nullptr);
  vkDestroyShaderModule(d, hiz_comp, nullptr);
  return pipes_ok;
}

bool WorldGpu::create_buffers(std::string& error) {
  VkDevice d = r_.device_;
  for (SlotBuffers& s : slots_) {
    if (!r_.create_buffer(VkDeviceSize(gpuworld::kBatchCount) * kMaxPerBatch * sizeof(gpuworld::IndirectCommand),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, s.commands) ||
        !r_.create_buffer(48, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, s.counts) ||
        !r_.create_buffer(48, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, s.stats) ||
        !r_.create_buffer(sizeof(CullUniformsGpu),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, s.cull_ubo) ||
        !r_.create_buffer(VkDeviceSize(kMaxTrees) * sizeof(uint32_t),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, s.tree_vis) ||
        !r_.create_buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, s.tree_cmd) ||
        !r_.create_buffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, s.tree_stats) ||
        vmaMapMemory(r_.allocator_, s.stats.allocation, &s.stats_mapped) != VK_SUCCESS ||
        vmaMapMemory(r_.allocator_, s.tree_stats.allocation, &s.tree_stats_mapped) != VK_SUCCESS) {
      error = "world cull buffers";
      s.stats_mapped = s.tree_stats_mapped = nullptr;
      return false;
    }
    std::memset(s.stats_mapped, 0, 48);
    std::memset(s.tree_stats_mapped, 0, 16);

    const VkDescriptorPoolSize sizes[4] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 + kHizMips},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kHizMips}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 2 + kHizMips;
    pi.poolSizeCount = 4;
    pi.pPoolSizes = sizes;
    WG_TRY(vkCreateDescriptorPool(d, &pi, nullptr, &s.pool), "cull pool");

    VkDescriptorSetAllocateInfo vi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    vi.descriptorPool = vis_pool_;
    vi.descriptorSetCount = 1;
    vi.pSetLayouts = &vis_layout_;
    WG_TRY(vkAllocateDescriptorSets(d, &vi, &s.vis_set), "vis set");
    VkDescriptorBufferInfo vbi{s.tree_vis.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet vw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    vw.dstSet = s.vis_set;
    vw.dstBinding = 0;
    vw.descriptorCount = 1;
    vw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    vw.pBufferInfo = &vbi;
    vkUpdateDescriptorSets(d, 1, &vw, 0, nullptr);
  }
  return true;
}

void WorldGpu::destroy_generation(Generation& g) {
  if (g.set) vkFreeDescriptorSets(r_.device_, table_pool_, 1, &g.set);
  g.set = VK_NULL_HANDLE;
  r_.destroy_buffer(g.table);
  r_.destroy_buffer(g.trees);
  r_.destroy_buffer(g.table_staging);
  r_.destroy_buffer(g.trees_staging);
  g.table_bytes = g.trees_bytes = 0;
  g.rows = g.trees_count = 0;
  g.pending_upload = false;
}

void WorldGpu::record_copies(VkCommandBuffer cmd, VkBuffer staged_vertices, const std::vector<Copy>& vertex_copies,
                             VkBuffer staged_indices, const std::vector<Copy>& index_copies) {
  auto run = [&](VkBuffer src, VkBuffer dst, const std::vector<Copy>& copies) {
    if (copies.empty()) return;
    std::vector<VkBufferCopy> regions;
    regions.reserve(copies.size());
    for (const Copy& c : copies) regions.push_back({c.src, c.dst, c.size});
    vkCmdCopyBuffer(cmd, src, dst, uint32_t(regions.size()), regions.data());
  };
  run(staged_vertices, vertices_.buffer, vertex_copies);
  run(staged_indices, indices_.buffer, index_copies);
}

void WorldGpu::queue_index_zero(uint32_t first_index, uint32_t count) {
  if (count == 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  pending_index_patches_.push_back({first_index, count, {}});
}

void WorldGpu::queue_index_restore(uint32_t first_index, std::vector<uint32_t> indices) {
  if (indices.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  const uint32_t count = uint32_t(indices.size());
  pending_index_patches_.push_back({first_index, count, std::move(indices)});
}

uint32_t WorldGpu::texture_slot(uint32_t record) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = texture_slots_.find(record);
  return it == texture_slots_.end() ? kDefaultWhite : it->second;
}

bool WorldGpu::has_texture(uint32_t record) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return texture_slots_.count(record) != 0;
}

uint32_t WorldGpu::add_texture(uint32_t record, VkImageView view) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (const auto it = texture_slots_.find(record); it != texture_slots_.end()) return it->second;
  uint32_t slot;
  if (!free_slots_.empty()) {
    slot = free_slots_.back();
    free_slots_.pop_back();
  } else if (next_slot_ < kMaxTextures) {
    slot = next_slot_++;
  } else {
    return kDefaultWhite;
  }
  VkDescriptorImageInfo info{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = bindless_set_;
  w.dstBinding = 1;
  w.dstArrayElement = slot;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  w.pImageInfo = &info;
  vkUpdateDescriptorSets(r_.device_, 1, &w, 0, nullptr);
  texture_slots_[record] = slot;
  return slot;
}

void WorldGpu::remove_texture(uint32_t record) {
  uint32_t slot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = texture_slots_.find(record);
    if (it == texture_slots_.end()) return;
    slot = it->second;
    texture_slots_.erase(it);
  }
  setup_.retire([this, slot] {
    std::lock_guard<std::mutex> lock(mutex_);
    VkDescriptorImageInfo info{VK_NULL_HANDLE, setup_.white, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = bindless_set_;
    w.dstBinding = 1;
    w.dstArrayElement = slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(r_.device_, 1, &w, 0, nullptr);
    free_slots_.push_back(slot);
  });
}

bool WorldGpu::set_table(const std::vector<gpuworld::DrawGpu>& rows, std::string& error,
                         const std::vector<gpuworld::TreeInstanceGpu>& trees) {
  // Non-blocking table swap: stage rows/trees into host-visible buffers on CPU (<0.1 ms)
  // and record the GPU-side transfer into the next frame's record_cull() command buffer
  // before compute culling runs (zero submit_once / vkWaitForFences stall on streaming swap).
  auto stage_device_buffer = [&](const void* data, VkDeviceSize bytes, Renderer::Buffer& device_buf,
                                 Renderer::Buffer& staging_buf) -> bool {
    if (!r_.create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, device_buf)) {
      return false;
    }
    if (!r_.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging_buf)) {
      r_.destroy_buffer(device_buf);
      return false;
    }
    void* mapped = nullptr;
    if (vmaMapMemory(r_.allocator_, staging_buf.allocation, &mapped) != VK_SUCCESS) {
      r_.destroy_buffer(staging_buf);
      r_.destroy_buffer(device_buf);
      return false;
    }
    std::memcpy(mapped, data, size_t(bytes));
    vmaUnmapMemory(r_.allocator_, staging_buf.allocation);
    return true;
  };

  Generation next;
  const uint32_t n = uint32_t(std::min<size_t>(rows.size(), size_t(kMaxPerBatch) * 2));
  const uint32_t tn = uint32_t(std::min<size_t>(trees.size(), size_t(kMaxTrees)));
  if (n > 0) {
    next.table_bytes = VkDeviceSize(n) * sizeof(gpuworld::DrawGpu);
    if (!stage_device_buffer(rows.data(), next.table_bytes, next.table, next.table_staging)) {
      error = "world draw table";
      return false;
    }
    if (tn > 0) {
      next.trees_bytes = VkDeviceSize(tn) * sizeof(gpuworld::TreeInstanceGpu);
      if (!stage_device_buffer(trees.data(), next.trees_bytes, next.trees, next.trees_staging)) {
        destroy_generation(next);
        error = "world tree instances";
        return false;
      }
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = table_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &table_layout_;
    VkResult res;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      res = vkAllocateDescriptorSets(r_.device_, &ai, &next.set);
    }
    if (res != VK_SUCCESS) {
      destroy_generation(next);
      error = "world table set";
      return false;
    }
    const VkDescriptorBufferInfo bis[2] = {
        {next.table.buffer, 0, VK_WHOLE_SIZE},
        {tn > 0 ? next.trees.buffer : empty_ssbo_.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet ws[2]{};
    for (uint32_t k = 0; k < 2; ++k) {
      ws[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      ws[k].dstSet = next.set;
      ws[k].dstBinding = k;
      ws[k].descriptorCount = 1;
      ws[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      ws[k].pBufferInfo = &bis[k];
    }
    vkUpdateDescriptorSets(r_.device_, 2, ws, 0, nullptr);
    next.rows = n;
    next.trees_count = tn;
    next.pending_upload = true;
  }
  Generation old = generation_;
  generation_ = next;
  if (old.table.buffer || old.trees.buffer || old.table_staging.buffer || old.trees_staging.buffer || old.set) {
    setup_.retire([this, old]() mutable {
      std::lock_guard<std::mutex> lock(mutex_);
      destroy_generation(old);
    });
  }
  return true;
}

void WorldGpu::record_cull(VkCommandBuffer cmd, const gpuworld::CullView& view) {
  SlotBuffers& s = slots_[r_.frame_slot()];
  recorded_slot_ = r_.frame_slot();
  if (s.used && s.stats_mapped) {
    const auto* c = static_cast<const uint32_t*>(s.stats_mapped);
    uint32_t drawn = 0, tested = 0, hiz_culled = 0;
    for (int b = 0; b < 4; ++b) {
      drawn += std::min(c[b], kMaxPerBatch);
      tested += c[4 + b];
      hiz_culled += c[8 + b];
    }
    last_drawn_ = drawn;
    last_tested_ = tested;
    last_hiz_culled_ = hiz_culled;
  }
  if (s.used && s.tree_stats_mapped) {
    const auto* tc = static_cast<const uint32_t*>(s.tree_stats_mapped);
    last_trees_drawn_ = std::min(tc[1], kMaxTrees);
  }
  vkResetDescriptorPool(r_.device_, s.pool, 0);

  // Apply queued per-instance smashable prop hides/restores directly to the resident index arena
  // before any draw pass (opaque, cutout, z-prepass, shadow, env) reads indices_.buffer.
  {
    std::vector<IndexPatch> patches;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      patches.swap(pending_index_patches_);
    }
    if (!patches.empty() && indices_.buffer) {
      const VkDeviceSize max_bytes = indices_.size;
      bool patched = false;
      for (const auto& pt : patches) {
        if (pt.count == 0) continue;
        const VkDeviceSize byte_off = VkDeviceSize(pt.first_index) * sizeof(uint32_t);
        const VkDeviceSize byte_len = VkDeviceSize(pt.count) * sizeof(uint32_t);
        if (byte_off >= max_bytes || byte_len > max_bytes - byte_off) continue;
        if (pt.data.empty()) {
          vkCmdFillBuffer(cmd, indices_.buffer, byte_off, byte_len, 0u);
          patched = true;
        } else {
          // vkCmdUpdateBuffer supports up to 65536 bytes (16384 uint32_t) per call.
          const size_t total_u32 = std::min<size_t>(pt.count, pt.data.size());
          for (size_t done = 0; done < total_u32; done += 16384u) {
            const size_t chunk = std::min<size_t>(16384u, total_u32 - done);
            vkCmdUpdateBuffer(cmd, indices_.buffer, byte_off + VkDeviceSize(done) * sizeof(uint32_t),
                              VkDeviceSize(chunk) * sizeof(uint32_t), pt.data.data() + done);
          }
          patched = true;
        }
      }
      if (patched) {
        buffer_barrier(cmd, indices_.buffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                       VK_ACCESS_2_INDEX_READ_BIT);
      }
    }
  }

  if (generation_.rows == 0) return;

  if (generation_.pending_upload) {
    generation_.pending_upload = false;
    if (generation_.table_staging.buffer && generation_.table.buffer && generation_.table_bytes > 0) {
      const VkBufferCopy copy{0, 0, generation_.table_bytes};
      vkCmdCopyBuffer(cmd, generation_.table_staging.buffer, generation_.table.buffer, 1, &copy);
      buffer_barrier(cmd, generation_.table.buffer, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    if (generation_.trees_staging.buffer && generation_.trees.buffer && generation_.trees_bytes > 0) {
      const VkBufferCopy copy{0, 0, generation_.trees_bytes};
      vkCmdCopyBuffer(cmd, generation_.trees_staging.buffer, generation_.trees.buffer, 1, &copy);
      buffer_barrier(cmd, generation_.trees.buffer, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    Renderer::Buffer st_table = generation_.table_staging;
    Renderer::Buffer st_trees = generation_.trees_staging;
    generation_.table_staging = {};
    generation_.trees_staging = {};
    if (st_table.buffer || st_trees.buffer) {
      setup_.retire([this, st_table, st_trees]() mutable {
        r_.destroy_buffer(st_table);
        r_.destroy_buffer(st_trees);
      });
    }
  }

  CullUniformsGpu u{};
  std::memcpy(u.planes, view.planes, sizeof u.planes);
  u.eye_reach[0] = view.eye[0], u.eye_reach[1] = view.eye[1], u.eye_reach[2] = view.eye[2];
  u.eye_reach[3] = view.lod_reach;
  u.info[0] = generation_.rows;
  u.info[1] = kMaxPerBatch;
  u.info[2] = view.frustum ? 1u : 0u;
  std::memcpy(u.prev_vp, view.prev_vp, sizeof u.prev_vp);
  u.hiz_info[0] = (view.hiz && hiz_valid_) ? 1u : 0u;
  u.hiz_info[1] = view.reverse_z ? 1u : 0u;
  u.hiz_info[2] = kHizWidth;
  u.hiz_info[3] = kHizHeight;
  vkCmdUpdateBuffer(cmd, s.cull_ubo.buffer, 0, sizeof u, &u);
  buffer_barrier(cmd, s.cull_ubo.buffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT);

  auto alloc_and_write = [&](VkBuffer b0, VkBuffer b1, VkBuffer b2, VkDescriptorSet& out) -> bool {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &cull_layout_;
    if (vkAllocateDescriptorSets(r_.device_, &ai, &out) != VK_SUCCESS) return false;
    const VkDescriptorBufferInfo b_infos[3] = {{b0, 0, VK_WHOLE_SIZE}, {b1, 0, VK_WHOLE_SIZE}, {b2, 0, VK_WHOLE_SIZE}};
    const VkDescriptorImageInfo hiz_info{hiz_sampler_, hiz_image_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo ubo_info{s.cull_ubo.buffer, 0, sizeof u};
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t i = 0; i < 3; ++i) {
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet = out;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &b_infos[i];
    }
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[3].dstSet = out;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo = &hiz_info;
    writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[4].dstSet = out;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[4].pBufferInfo = &ubo_info;
    vkUpdateDescriptorSets(r_.device_, 5, writes, 0, nullptr);
    return true;
  };

  VkDescriptorSet draw_set = VK_NULL_HANDLE;
  if (!alloc_and_write(generation_.table.buffer, s.commands.buffer, s.counts.buffer, draw_set)) return;

  vkCmdFillBuffer(cmd, s.counts.buffer, 0, 48, 0);
  buffer_barrier(cmd, s.counts.buffer, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cull_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipeline_layout_, 0, 1, &draw_set, 0, nullptr);
  vkCmdDispatch(cmd, (generation_.rows + 63) / 64, 1, 1);

  buffer_barrier(cmd, s.commands.buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
  buffer_barrier(cmd, s.counts.buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
  const VkBufferCopy copy{0, 0, 48};
  vkCmdCopyBuffer(cmd, s.counts.buffer, s.stats.buffer, 1, &copy);

  if (generation_.trees_count > 0 && generation_.trees.buffer) {
    u.info[0] = generation_.trees_count;
    u.info[1] = kMaxTrees;
    vkCmdUpdateBuffer(cmd, s.cull_ubo.buffer, 0, sizeof u, &u);
    buffer_barrier(cmd, s.cull_ubo.buffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT);

    const uint32_t init_cmd[4] = {6u, 0u, 0u, 0u};
    vkCmdUpdateBuffer(cmd, s.tree_cmd.buffer, 0, sizeof init_cmd, init_cmd);
    buffer_barrier(cmd, s.tree_cmd.buffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    VkDescriptorSet tree_set = VK_NULL_HANDLE;
    if (alloc_and_write(generation_.trees.buffer, s.tree_vis.buffer, s.tree_cmd.buffer, tree_set)) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tree_cull_);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipeline_layout_, 0, 1, &tree_set, 0, nullptr);
      vkCmdDispatch(cmd, (generation_.trees_count + 63) / 64, 1, 1);
      buffer_barrier(cmd, s.tree_vis.buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
      buffer_barrier(cmd, s.tree_cmd.buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
      const VkBufferCopy tcopy{0, 0, 16};
      vkCmdCopyBuffer(cmd, s.tree_cmd.buffer, s.tree_stats.buffer, 1, &tcopy);
    }
  }

  VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &host;
  vkCmdPipelineBarrier2(cmd, &dep);
  s.used = true;
}

void WorldGpu::record_z_prepass(VkCommandBuffer cmd, const DrawParams& p) {
  if (generation_.rows == 0 || !slots_[recorded_slot_].used || !z_prepass_) return;
  SlotBuffers& s = slots_[recorded_slot_];
  const VkDescriptorSet sets[4] = {p.frame_set, bindless_set_, generation_.set, s.vis_set};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 4, sets, 0, nullptr);
  struct Push {
    float model[16];
    float sun[4];
    float up[4];
    float swap_pad[4];
    float pad[4];
  } pc{};
  std::memcpy(pc.model, p.model, sizeof pc.model);
  for (int i = 0; i < 3; ++i) pc.sun[i] = p.sun[i];
  for (int i = 0; i < 4; ++i) pc.up[i] = p.up[i];
  pc.swap_pad[0] = p.swap;
  vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
  const VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, z_prepass_);
  for (uint32_t b = 0; b <= gpuworld::kOpaqueDecal; ++b) {
    const bool decal = (b & 1u) != 0;
    vkCmdSetDepthBias(cmd, decal ? p.bias_sign * p.decal_bias_constant : 0.0f, 0.0f,
                      decal ? p.bias_sign * p.decal_bias_slope : 0.0f);
    vkCmdDrawIndexedIndirectCount(cmd, s.commands.buffer,
                                  VkDeviceSize(b) * kMaxPerBatch * sizeof(gpuworld::IndirectCommand),
                                  s.counts.buffer, VkDeviceSize(b) * sizeof(uint32_t), kMaxPerBatch,
                                  sizeof(gpuworld::IndirectCommand));
  }
  vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
}

void WorldGpu::record_draw(VkCommandBuffer cmd, const DrawParams& p) {
  if (generation_.rows == 0 || !slots_[recorded_slot_].used) return;
  SlotBuffers& s = slots_[recorded_slot_];
  const VkDescriptorSet sets[4] = {p.frame_set, bindless_set_, generation_.set, s.vis_set};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 4, sets, 0, nullptr);
  struct Push {
    float model[16];
    float sun[4];
    float up[4];
    float swap_pad[4];
    float pad[4];
  } pc{};
  std::memcpy(pc.model, p.model, sizeof pc.model);
  for (int i = 0; i < 3; ++i) pc.sun[i] = p.sun[i];
  for (int i = 0; i < 4; ++i) pc.up[i] = p.up[i];
  pc.swap_pad[0] = p.swap;
  vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
  const VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
  VkPipeline bound = VK_NULL_HANDLE;
  const VkPipeline op_pipe = (p.swap < 1.5f && opaque_dither_)
                                 ? opaque_dither_
                                 : ((p.depth_prepass && opaque_after_z_) ? opaque_after_z_ : opaque_);
  for (uint32_t b = 0; b < gpuworld::kBatchCount; ++b) {
    const VkPipeline pipeline = b >= gpuworld::kCutout ? coverage_ : op_pipe;
    if (pipeline != bound) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = pipeline);
      r_.apply_vrs(cmd);
    }
    const bool decal = (b & 1u) != 0;
    vkCmdSetDepthBias(cmd, decal ? p.bias_sign * p.decal_bias_constant : 0.0f, 0.0f,
                      decal ? p.bias_sign * p.decal_bias_slope : 0.0f);
    vkCmdDrawIndexedIndirectCount(cmd, s.commands.buffer,
                                  VkDeviceSize(b) * kMaxPerBatch * sizeof(gpuworld::IndirectCommand),
                                  s.counts.buffer, VkDeviceSize(b) * sizeof(uint32_t), kMaxPerBatch,
                                  sizeof(gpuworld::IndirectCommand));
  }
  if (generation_.trees_count > 0 && tree_coverage_) {
    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tree_coverage_);
    r_.apply_vrs(cmd);
    vkCmdDrawIndirect(cmd, s.tree_cmd.buffer, 0, 1, sizeof(VkDrawIndirectCommand));
  }
  vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
}

void WorldGpu::record_hiz(VkCommandBuffer cmd, VkImageView depth_view, uint32_t depth_w, uint32_t depth_h,
                          bool reverse_z) {
  if (!ready_ || !hiz_gen_ || !depth_view || !depth_w || !depth_h) return;
  SlotBuffers& s = slots_[recorded_slot_];
  img_barrier(cmd, hiz_image_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0, kHizMips);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_gen_);

  uint32_t sw = depth_w, sh = depth_h;
  uint32_t dw = kHizWidth, dh = kHizHeight;
  for (uint32_t m = 0; m < kHizMips; ++m) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &hiz_set_layout_;
    if (vkAllocateDescriptorSets(r_.device_, &ai, &set) != VK_SUCCESS) break;

    const VkImageView src_view = (m == 0) ? depth_view : hiz_mip_views_[m - 1];
    const VkImageLayout src_layout =
        (m == 0) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkDescriptorImageInfo src_info{hiz_sampler_, src_view, src_layout};
    const VkDescriptorImageInfo dst_info{VK_NULL_HANDLE, hiz_mip_views_[m], VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2]{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &src_info;
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &dst_info;
    vkUpdateDescriptorSets(r_.device_, 2, writes, 0, nullptr);

    struct Push {
      uint32_t src_w, src_h, dst_w, dst_h, rev_z, is_mip0;
    } pc{sw, sh, dw, dh, reverse_z ? 1u : 0u, m == 0 ? 1u : 0u};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipeline_layout_, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, hiz_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, (dw + 7) / 8, (dh + 7) / 8, 1);

    img_barrier(cmd, hiz_image_.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, m, 1);
    sw = dw;
    sh = dh;
    dw = std::max(1u, dw / 2u);
    dh = std::max(1u, dh / 2u);
  }
  hiz_valid_ = true;
}

}  // namespace carview
