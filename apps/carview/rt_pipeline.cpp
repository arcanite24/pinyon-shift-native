#include "rt_pipeline.h"

#ifdef SHIFT_ENABLE_RAY_TRACING

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <vk_mem_alloc.h>

#include "rt_ao_comp_spv.h"
#include "rt_composite_comp_spv.h"
#include "rt_pathtrace_comp_spv.h"
#include "rt_reflections_comp_spv.h"
#include "rt_scene.h"
#include "rt_shadows_comp_spv.h"
#include "rt_svgf_atrous_comp_spv.h"
#include "rt_svgf_temporal_comp_spv.h"
#include "showroom.h"
#include "world_gpu.h"

namespace carview {

namespace {

// 4x4 column-major matrix inversion (exact cofactor expansion).
bool invert_mat4(const float m[16], float out[16]) {
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
           m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
           m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
           m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
            m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
           m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
           m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
           m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
            m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
           m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
           m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
            m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
            m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
           m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
           m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
            m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
            m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::fabs(det) < 1e-12f) return false;
  const float inv_det = 1.0f / det;
  for (int i = 0; i < 16; ++i) out[i] = inv[i] * inv_det;
  return true;
}

void compute_barrier(VkCommandBuffer cmd) {
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cmd, &dep);
}

void clear_storage_image(VkCommandBuffer cmd, VkImage img) {
  VkImageMemoryBarrier2 b1{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b1.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
  b1.srcAccessMask = VK_ACCESS_2_NONE;
  b1.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
  b1.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b1.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b1.srcQueueFamilyIndex = b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b1.image = img;
  b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo d1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  d1.imageMemoryBarrierCount = 1;
  d1.pImageMemoryBarriers = &b1;
  vkCmdPipelineBarrier2(cmd, &d1);

  const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 0.0f}};
  const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rng);

  VkImageMemoryBarrier2 b2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b2.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
  b2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  b2.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  b2.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  b2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  b2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b2.srcQueueFamilyIndex = b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b2.image = img;
  b2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  d2.imageMemoryBarrierCount = 1;
  d2.pImageMemoryBarriers = &b2;
  vkCmdPipelineBarrier2(cmd, &d2);
}

}  // namespace

RtPipeline::RtPipeline(Renderer& r) : r_(r) {}

RtPipeline::~RtPipeline() { shutdown(); }

bool RtPipeline::create_compute_pipeline(const uint32_t* spv, size_t spv_bytes, VkPipeline& out, std::string& error) {
  VkDevice d = r_.device_;
  VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smi.codeSize = spv_bytes;
  smi.pCode = spv;
  VkShaderModule mod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(d, &smi, nullptr, &mod) != VK_SUCCESS) {
    error = "RT shader module create failed";
    return false;
  }
  VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pci.stage.module = mod;
  pci.stage.pName = "main";
  pci.layout = pipe_layout_;
  const VkResult res = vkCreateComputePipelines(d, r_.pipeline_cache(), 1, &pci, nullptr, &out);
  vkDestroyShaderModule(d, mod, nullptr);
  if (res != VK_SUCCESS) {
    error = "RT compute pipeline create failed";
    return false;
  }
  return true;
}

bool RtPipeline::init(VkDescriptorSetLayout world_bindless_layout, VkSampler world_sampler, VkImageView fallback_white,
                      std::string& error, RetireFn retire_fn) {
  if (!r_.rt_supported()) {
    error = "RT not supported";
    return false;
  }
  retire_fn_ = std::move(retire_fn);
  VkDevice d = r_.device_;

  // Samplers for depth/motion/dyn_env in RT compute shaders.
  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.maxLod = VK_LOD_CLAMP_NONE;
  if (vkCreateSampler(d, &sci, nullptr, &linear_sampler_) != VK_SUCCESS) {
    error = "RT linear sampler";
    shutdown();
    return false;
  }
  sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  if (vkCreateSampler(d, &sci, nullptr, &nearest_sampler_) != VK_SUCCESS) {
    error = "RT nearest sampler";
    shutdown();
    return false;
  }

  // Set 0 layout: 21 bindings matching rt_common.glsl.
  const VkDescriptorSetLayoutBinding bindings[21] = {
      {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {15, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {16, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {17, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {18, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {19, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  };
  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  li.bindingCount = 21;
  li.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(d, &li, nullptr, &rt_set_layout_) != VK_SUCCESS) {
    error = "RT descriptor set layout";
    shutdown();
    return false;
  }

  // Fallback set 1 when WorldGpu isn't active (e.g. Autoshow studio or flat world).
  VkDescriptorSetLayout set1_layout = world_bindless_layout;
  if (!set1_layout) {
    VkSampler use_samp = world_sampler ? world_sampler : linear_sampler_;
    const VkDescriptorSetLayoutBinding fb[2] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &use_samp},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo fli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    fli.bindingCount = 2;
    fli.pBindings = fb;
    if (vkCreateDescriptorSetLayout(d, &fli, nullptr, &fallback_bindless_layout_) != VK_SUCCESS) {
      error = "RT fallback bindless layout";
      shutdown();
      return false;
    }
    const VkDescriptorPoolSize fps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4}};
    VkDescriptorPoolCreateInfo fpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    fpi.maxSets = 1;
    fpi.poolSizeCount = 2;
    fpi.pPoolSizes = fps;
    if (vkCreateDescriptorPool(d, &fpi, nullptr, &fallback_bindless_pool_) != VK_SUCCESS) {
      error = "RT fallback bindless pool";
      shutdown();
      return false;
    }
    VkDescriptorSetAllocateInfo fai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    fai.descriptorPool = fallback_bindless_pool_;
    fai.descriptorSetCount = 1;
    fai.pSetLayouts = &fallback_bindless_layout_;
    if (vkAllocateDescriptorSets(d, &fai, &fallback_bindless_set_) != VK_SUCCESS) {
      error = "RT fallback bindless set";
      shutdown();
      return false;
    }
    if (fallback_white) {
      VkDescriptorImageInfo imgs[4];
      for (int i = 0; i < 4; ++i) imgs[i] = {VK_NULL_HANDLE, fallback_white, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = fallback_bindless_set_;
      w.dstBinding = 1;
      w.descriptorCount = 4;
      w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      w.pImageInfo = imgs;
      vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
    }
    set1_layout = fallback_bindless_layout_;
  }

  // Set 2: Bindless RT geometry storage buffer array (matching rt_common.glsl set = 2, binding = 0).
  {
    const VkDescriptorSetLayoutBinding gb = {
        0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, RtScene::kMaxRtGeomBuffers, VK_SHADER_STAGE_COMPUTE_BIT, nullptr,
    };
    const VkDescriptorBindingFlags gbf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = 1;
    bf.pBindingFlags = &gbf;
    VkDescriptorSetLayoutCreateInfo gli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    gli.pNext = &bf;
    gli.bindingCount = 1;
    gli.pBindings = &gb;
    if (vkCreateDescriptorSetLayout(d, &gli, nullptr, &geom_buf_set_layout_) != VK_SUCCESS) {
      error = "RT bindless geometry buffer layout";
      shutdown();
      return false;
    }
    const VkDescriptorPoolSize gps = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        Renderer::kFramesInFlight * RtScene::kMaxRtGeomBuffers,
    };
    VkDescriptorPoolCreateInfo gpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    gpi.maxSets = Renderer::kFramesInFlight;
    gpi.poolSizeCount = 1;
    gpi.pPoolSizes = &gps;
    if (vkCreateDescriptorPool(d, &gpi, nullptr, &geom_buf_pool_) != VK_SUCCESS) {
      error = "RT bindless geometry buffer pool";
      shutdown();
      return false;
    }
    VkDescriptorSetLayout glayouts[Renderer::kFramesInFlight];
    for (uint32_t i = 0; i < Renderer::kFramesInFlight; ++i) glayouts[i] = geom_buf_set_layout_;
    VkDescriptorSetAllocateInfo gai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    gai.descriptorPool = geom_buf_pool_;
    gai.descriptorSetCount = Renderer::kFramesInFlight;
    gai.pSetLayouts = glayouts;
    if (vkAllocateDescriptorSets(d, &gai, geom_buf_sets_) != VK_SUCCESS) {
      error = "RT bindless geometry buffer sets";
      shutdown();
      return false;
    }
  }

  const VkDescriptorSetLayout layouts[3] = {rt_set_layout_, set1_layout, geom_buf_set_layout_};
  const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 3;
  pli.pSetLayouts = layouts;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  if (vkCreatePipelineLayout(d, &pli, nullptr, &pipe_layout_) != VK_SUCCESS) {
    error = "RT pipeline layout";
    shutdown();
    return false;
  }

  // Allocate per-frame-in-flight descriptor sets and mapped UBOs.
  const VkDescriptorPoolSize psizes[5] = {
      {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, Renderer::kFramesInFlight},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Renderer::kFramesInFlight * 5},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, Renderer::kFramesInFlight},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Renderer::kFramesInFlight * 3},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, Renderer::kFramesInFlight * 11},
  };
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = Renderer::kFramesInFlight;
  dpi.poolSizeCount = 5;
  dpi.pPoolSizes = psizes;
  if (vkCreateDescriptorPool(d, &dpi, nullptr, &desc_pool_) != VK_SUCCESS) {
    error = "RT descriptor pool";
    shutdown();
    return false;
  }
  VkDescriptorSetLayout set_layouts[Renderer::kFramesInFlight];
  for (uint32_t i = 0; i < Renderer::kFramesInFlight; ++i) set_layouts[i] = rt_set_layout_;
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = desc_pool_;
  dai.descriptorSetCount = Renderer::kFramesInFlight;
  dai.pSetLayouts = set_layouts;
  if (vkAllocateDescriptorSets(d, &dai, desc_sets_) != VK_SUCCESS) {
    error = "RT descriptor sets";
    shutdown();
    return false;
  }
  for (uint32_t i = 0; i < Renderer::kFramesInFlight; ++i) {
    if (!r_.create_buffer(sizeof(RtFrameUboGpu), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ubo_buf_[i]) ||
        vmaMapMemory(r_.allocator_, ubo_buf_[i].allocation, &ubo_mapped_[i]) != VK_SUCCESS) {
      error = "RT frame UBO";
      shutdown();
      return false;
    }
  }

  {
    std::vector<std::function<VkResult(VkPipelineCache)>> tasks;
    tasks.reserve(7);
    auto queue_comp = [&](const uint32_t* spv, size_t spv_bytes, VkPipeline& out) {
      VkPipeline* dst = &out;
      tasks.emplace_back([d, layout = pipe_layout_, spv, spv_bytes, dst](VkPipelineCache cache) -> VkResult {
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = spv_bytes;
        smi.pCode = spv;
        VkShaderModule mod = VK_NULL_HANDLE;
        VkResult res = vkCreateShaderModule(d, &smi, nullptr, &mod);
        if (res != VK_SUCCESS) return res;
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pci.stage.module = mod;
        pci.stage.pName = "main";
        pci.layout = layout;
        res = vkCreateComputePipelines(d, cache, 1, &pci, nullptr, dst);
        vkDestroyShaderModule(d, mod, nullptr);
        return res;
      });
    };
    queue_comp(kRt_reflections_comp_spv, sizeof(kRt_reflections_comp_spv), refl_pipe_);
    queue_comp(kRt_shadows_comp_spv, sizeof(kRt_shadows_comp_spv), shad_pipe_);
    queue_comp(kRt_ao_comp_spv, sizeof(kRt_ao_comp_spv), ao_pipe_);
    queue_comp(kRt_pathtrace_comp_spv, sizeof(kRt_pathtrace_comp_spv), pt_pipe_);
    queue_comp(kRt_svgf_temporal_comp_spv, sizeof(kRt_svgf_temporal_comp_spv), svgf_temp_pipe_);
    queue_comp(kRt_svgf_atrous_comp_spv, sizeof(kRt_svgf_atrous_comp_spv), svgf_atrous_pipe_);
    queue_comp(kRt_composite_comp_spv, sizeof(kRt_composite_comp_spv), comp_pipe_);
    if (!r_.compile_pipelines_parallel(tasks, error)) {
      shutdown();
      return false;
    }
  }

  ready_ = true;
  return true;
}

void RtPipeline::destroy_targets() {
  r_.destroy_image(gbuf_norm_);
  r_.destroy_image(gbuf_albedo_);
  r_.destroy_image(refl_img_);
  r_.destroy_image(shad_ao_img_);
  r_.destroy_image(pt_img_);
  r_.destroy_image(hist_img_[0]);
  r_.destroy_image(hist_img_[1]);
  r_.destroy_image(svgf_tmp_);
  r_.destroy_image(moments_img_[0]);
  r_.destroy_image(moments_img_[1]);
  r_.destroy_buffer(reservoirs_[0]);
  r_.destroy_buffer(reservoirs_[1]);
  width_ = height_ = 0;
  history_valid_ = false;
  photo_accum_count_ = 0;
}

void RtPipeline::shutdown() {
  if (!r_.device_) return;
  VkDevice d = r_.device_;
  r_.wait_idle();
  destroy_targets();
  for (VkPipeline p : {refl_pipe_, shad_pipe_, ao_pipe_, pt_pipe_, svgf_temp_pipe_, svgf_atrous_pipe_, comp_pipe_}) {
    if (p) vkDestroyPipeline(d, p, nullptr);
  }
  refl_pipe_ = shad_pipe_ = ao_pipe_ = pt_pipe_ = svgf_temp_pipe_ = svgf_atrous_pipe_ = comp_pipe_ = VK_NULL_HANDLE;
  for (uint32_t i = 0; i < Renderer::kFramesInFlight; ++i) {
    if (ubo_mapped_[i]) {
      vmaUnmapMemory(r_.allocator_, ubo_buf_[i].allocation);
      ubo_mapped_[i] = nullptr;
    }
    r_.destroy_buffer(ubo_buf_[i]);
  }
  if (pipe_layout_) vkDestroyPipelineLayout(d, pipe_layout_, nullptr);
  if (desc_pool_) vkDestroyDescriptorPool(d, desc_pool_, nullptr);
  if (rt_set_layout_) vkDestroyDescriptorSetLayout(d, rt_set_layout_, nullptr);
  if (geom_buf_pool_) vkDestroyDescriptorPool(d, geom_buf_pool_, nullptr);
  if (geom_buf_set_layout_) vkDestroyDescriptorSetLayout(d, geom_buf_set_layout_, nullptr);
  if (fallback_bindless_pool_) vkDestroyDescriptorPool(d, fallback_bindless_pool_, nullptr);
  if (fallback_bindless_layout_) vkDestroyDescriptorSetLayout(d, fallback_bindless_layout_, nullptr);
  if (linear_sampler_) vkDestroySampler(d, linear_sampler_, nullptr);
  if (nearest_sampler_) vkDestroySampler(d, nearest_sampler_, nullptr);
  pipe_layout_ = VK_NULL_HANDLE;
  desc_pool_ = VK_NULL_HANDLE;
  rt_set_layout_ = VK_NULL_HANDLE;
  geom_buf_pool_ = VK_NULL_HANDLE;
  geom_buf_set_layout_ = VK_NULL_HANDLE;
  for (uint32_t i = 0; i < Renderer::kFramesInFlight; ++i) geom_buf_sets_[i] = VK_NULL_HANDLE;
  fallback_bindless_pool_ = VK_NULL_HANDLE;
  fallback_bindless_layout_ = VK_NULL_HANDLE;
  fallback_bindless_set_ = VK_NULL_HANDLE;
  linear_sampler_ = nearest_sampler_ = VK_NULL_HANDLE;
  ready_ = false;
}

bool RtPipeline::prewarm_targets(uint32_t width, uint32_t height) {
  if (!ready_ || width == 0 || height == 0) return true;
  if (width_ == width && height_ == height && gbuf_norm_.image) return true;
  bool ok = false;
  const bool submitted = r_.submit_once([&](VkCommandBuffer cmd) { ok = ensure_targets(width, height, cmd); });
  return submitted && ok;
}

bool RtPipeline::ensure_targets(uint32_t width, uint32_t height, VkCommandBuffer cmd) {
  if (width_ == width && height_ == height && gbuf_norm_.image) return true;
  if (gbuf_norm_.image) {
    if (retire_fn_) {
      // Defer destruction until in-flight frames finish: zero vkDeviceWaitIdle hitch on resize!
      Renderer* r = &r_;
      auto old_gn = gbuf_norm_, old_ga = gbuf_albedo_, old_rf = refl_img_, old_sa = shad_ao_img_, old_pt = pt_img_;
      auto old_h0 = hist_img_[0], old_h1 = hist_img_[1], old_st = svgf_tmp_;
      auto old_m0 = moments_img_[0], old_m1 = moments_img_[1];
      auto old_r0 = reservoirs_[0], old_r1 = reservoirs_[1];
      gbuf_norm_ = gbuf_albedo_ = refl_img_ = shad_ao_img_ = pt_img_ = {};
      hist_img_[0] = hist_img_[1] = svgf_tmp_ = moments_img_[0] = moments_img_[1] = {};
      reservoirs_[0] = reservoirs_[1] = {};
      width_ = height_ = 0;
      history_valid_ = false;
      photo_accum_count_ = 0;
      retire_fn_([r, old_gn, old_ga, old_rf, old_sa, old_pt, old_h0, old_h1, old_st, old_m0, old_m1, old_r0,
                  old_r1]() mutable {
        r->destroy_image(old_gn);
        r->destroy_image(old_ga);
        r->destroy_image(old_rf);
        r->destroy_image(old_sa);
        r->destroy_image(old_pt);
        r->destroy_image(old_h0);
        r->destroy_image(old_h1);
        r->destroy_image(old_st);
        r->destroy_image(old_m0);
        r->destroy_image(old_m1);
        r->destroy_buffer(old_r0);
        r->destroy_buffer(old_r1);
      });
    } else {
      r_.wait_idle();
      destroy_targets();
    }
  }

  const VkImageUsageFlags storage_usage =
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!r_.create_image(width, height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, storage_usage,
                       VK_IMAGE_ASPECT_COLOR_BIT, gbuf_norm_) ||
      !r_.create_image(width, height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, storage_usage,
                       VK_IMAGE_ASPECT_COLOR_BIT, gbuf_albedo_) ||
      !r_.create_image(width, height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, storage_usage,
                       VK_IMAGE_ASPECT_COLOR_BIT, refl_img_) ||
      !r_.create_image(width, height, 1, VK_FORMAT_R16G16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, storage_usage,
                       VK_IMAGE_ASPECT_COLOR_BIT, shad_ao_img_) ||
      !r_.create_image(width, height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, storage_usage,
                       VK_IMAGE_ASPECT_COLOR_BIT, pt_img_) ||
      !r_.create_image(width, height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, storage_usage,
                       VK_IMAGE_ASPECT_COLOR_BIT, hist_img_[0]) ||
      !r_.create_image(width, height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, storage_usage,
                       VK_IMAGE_ASPECT_COLOR_BIT, hist_img_[1]) ||
      !r_.create_image(width, height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, storage_usage,
                       VK_IMAGE_ASPECT_COLOR_BIT, svgf_tmp_) ||
      !r_.create_image(width, height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, storage_usage,
                       VK_IMAGE_ASPECT_COLOR_BIT, moments_img_[0]) ||
      !r_.create_image(width, height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, storage_usage,
                       VK_IMAGE_ASPECT_COLOR_BIT, moments_img_[1])) {
    destroy_targets();
    return false;
  }

  const VkDeviceSize res_bytes = VkDeviceSize(width) * VkDeviceSize(height) * sizeof(RtReservoirGpu);
  if (!r_.create_buffer(res_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, reservoirs_[0]) ||
      !r_.create_buffer(res_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, reservoirs_[1])) {
    destroy_targets();
    return false;
  }

  // Initialize all storage images to GENERAL layout and zero ReSTIR reservoirs cleanly.
  for (VkImage img : {gbuf_norm_.image, gbuf_albedo_.image, refl_img_.image, shad_ao_img_.image, pt_img_.image,
                      hist_img_[0].image, hist_img_[1].image, svgf_tmp_.image, moments_img_[0].image,
                      moments_img_[1].image}) {
    clear_storage_image(cmd, img);
  }
  vkCmdFillBuffer(cmd, reservoirs_[0].buffer, 0, res_bytes, 0u);
  vkCmdFillBuffer(cmd, reservoirs_[1].buffer, 0, res_bytes, 0u);
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
  mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cmd, &dep);

  width_ = width;
  height_ = height;
  history_valid_ = false;
  photo_accum_count_ = 0;
  return true;
}

void RtPipeline::record(VkCommandBuffer cmd, const Showroom& host, const RtScene& scene, VkImage /*hdr_img*/,
                        VkImageView hdr_view, VkImageView depth_view, VkImageView motion_view, uint32_t width,
                        uint32_t height) {
  if (!ready_ || !scene.ready() || !scene.tlas() || !hdr_view || !depth_view || width == 0 || height == 0) return;
  if (!ensure_targets(width, height, cmd)) return;

  const uint32_t slot = r_.frame_slot();
  const auto& gfx = r_.graphics_settings();
  rt::RtMode mode = r_.active_rt_mode();
  if (r_.photo_pt_accumulate() && mode != rt::RtMode::kOff) {
    mode = rt::RtMode::kPhotoModePt;
  }

  // Track camera stability for Photo Mode progressive multi-SPP accumulation.
  const bool explicit_reset = r_.consume_photo_pt_reset();
  if (mode == rt::RtMode::kPhotoModePt) {
    bool cam_moved = explicit_reset || !have_last_vp_;
    if (!cam_moved) {
      float diff = 0.0f;
      for (int i = 0; i < 16; ++i) diff = std::max(diff, std::fabs(host.frame_.view_projection[i] - last_vp_[i]));
      if (diff > 1e-4f) cam_moved = true;
    }
    if (cam_moved) photo_accum_count_ = 0;
    photo_accum_count_ = std::min(photo_accum_count_ + 1u, 256u);
  } else {
    photo_accum_count_ = 0;
  }

  // Populate RtFrameUboGpu for this frame slot.
  RtFrameUboGpu u{};
  std::memcpy(u.view_proj, host.frame_.view_projection, sizeof(u.view_proj));
  if (!invert_mat4(u.view_proj, u.inv_view_proj)) {
    std::memset(u.inv_view_proj, 0, sizeof(u.inv_view_proj));
    u.inv_view_proj[0] = u.inv_view_proj[5] = u.inv_view_proj[10] = u.inv_view_proj[15] = 1.0f;
  }
  if (have_last_vp_) {
    std::memcpy(u.prev_view_proj, last_vp_, sizeof(u.prev_view_proj));
  } else {
    std::memcpy(u.prev_view_proj, u.view_proj, sizeof(u.prev_view_proj));
  }
  std::memcpy(last_vp_, host.frame_.view_projection, sizeof(last_vp_));
  have_last_vp_ = true;

  ++frame_index_;
  u.eye_frame[0] = host.frame_.eye[0];
  u.eye_frame[1] = host.frame_.eye[1];
  u.eye_frame[2] = host.frame_.eye[2];
  u.eye_frame[3] = float(history_valid_ ? frame_index_ : 1u);

  const float* kl = host.frame_.key_light;
  const float klen = std::max(std::sqrt(kl[0] * kl[0] + kl[1] * kl[1] + kl[2] * kl[2]), 1e-4f);
  u.sun_dir_elev[0] = kl[0] / klen;
  u.sun_dir_elev[1] = kl[1] / klen;
  u.sun_dir_elev[2] = kl[2] / klen;
  u.sun_dir_elev[3] = host.world_lit_ ? r_.post_settings().sun_elev_vis : 1.0f;

  const bool has_tod = host.frame_.sun_color[3] >= 0.0f;
  u.sun_color_night[0] = has_tod ? host.frame_.sun_color[0] : 0.9695f;
  u.sun_color_night[1] = has_tod ? host.frame_.sun_color[1] : 0.8358f;
  u.sun_color_night[2] = has_tod ? host.frame_.sun_color[2] : 0.5840f;
  u.sun_color_night[3] = has_tod ? host.frame_.sky_color[3] : 0.0f;

  u.sky_color_exp[0] = has_tod ? host.frame_.sky_color[0] : 0.1687f;
  u.sky_color_exp[1] = has_tod ? host.frame_.sky_color[1] : 0.2229f;
  u.sky_color_exp[2] = has_tod ? host.frame_.sky_color[2] : 0.2746f;
  u.sky_color_exp[3] = r_.post_settings().exposure;

  u.ground_color_revz[0] = has_tod ? host.frame_.ground_color[0] : 0.1788f;
  u.ground_color_revz[1] = has_tod ? host.frame_.ground_color[1] : 0.1729f;
  u.ground_color_revz[2] = has_tod ? host.frame_.ground_color[2] : 0.1024f;
  u.ground_color_revz[3] = r_.reverse_z() ? 1.0f : 0.0f;

  for (int k = 0; k < 4; ++k) {
    u.fog_color_start[k] = host.frame_.fog_color[k];
    u.env_basis_x[k] = host.frame_.env_basis_x[k];
    u.env_basis_y[k] = host.frame_.env_basis_y[k];
    u.env_basis_z[k] = host.frame_.env_basis_z[k];
    u.paint_two_tone[k] = host.frame_.paint_two_tone[k];
    u.paint_finish[k] = host.frame_.paint_finish[k];
  }
  u.fog_params[0] = host.frame_.fog_params[0];
  u.fog_params[1] = r_.cam_near_;
  u.fog_params[2] = r_.cam_far_;
  u.fog_params[3] = float(photo_accum_count_);

  u.env_basis_y[3] = float(int(mode));
  u.env_basis_z[3] = float(rt::clamp_rt_bounces(gfx.rt_bounces));
  u.rt_params[0] = float(rt::clamp_rt_spp(gfx.rt_spp));
  u.rt_params[1] = float(scene.light_count());
  u.rt_params[2] = float(width);
  u.rt_params[3] = float(height);

  std::memcpy(ubo_mapped_[slot], &u, sizeof(u));

  // Update Set 0 descriptor bindings for this frame slot.
  const uint32_t prev_hi = hist_idx_ ^ 1u;
  const uint32_t curr_hi = hist_idx_;
  VkAccelerationStructureKHR tlas_h = scene.tlas();
  VkWriteDescriptorSetAccelerationStructureKHR as_write{
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  as_write.accelerationStructureCount = 1;
  as_write.pAccelerationStructures = &tlas_h;

  VkDescriptorBufferInfo b_inst{scene.instance_info_buffer(), 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo b_geom{scene.geom_info_buffer(), 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo b_light{scene.light_buffer(), 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo b_ubo{ubo_buf_[slot].buffer, 0, sizeof(RtFrameUboGpu)};
  VkDescriptorBufferInfo b_res_prev{reservoirs_[prev_hi].buffer, 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo b_res_curr{reservoirs_[curr_hi].buffer, 0, VK_WHOLE_SIZE};

  VkImageView env_view = host.dynamic_env_view() ? host.dynamic_env_view() : host.environment_.view;
  VkSampler env_samp = host.dynamic_env_sampler() ? host.dynamic_env_sampler() : linear_sampler_;
  VkDescriptorImageInfo i_env{env_samp, env_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorImageInfo i_depth{nearest_sampler_, depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
  VkDescriptorImageInfo i_motion{nearest_sampler_, motion_view ? motion_view : r_.vel_zero_.view,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

  VkDescriptorImageInfo i_hdr{VK_NULL_HANDLE, hdr_view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo i_gnorm{VK_NULL_HANDLE, gbuf_norm_.view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo i_galb{VK_NULL_HANDLE, gbuf_albedo_.view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo i_refl{VK_NULL_HANDLE, refl_img_.view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo i_shao{VK_NULL_HANDLE, shad_ao_img_.view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo i_pt{VK_NULL_HANDLE, pt_img_.view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo i_hprev{VK_NULL_HANDLE, hist_img_[prev_hi].view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo i_hcurr{VK_NULL_HANDLE, hist_img_[curr_hi].view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo i_tmp{VK_NULL_HANDLE, svgf_tmp_.view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo i_mprev{VK_NULL_HANDLE, moments_img_[prev_hi].view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo i_mcurr{VK_NULL_HANDLE, moments_img_[curr_hi].view, VK_IMAGE_LAYOUT_GENERAL};

  VkWriteDescriptorSet w[21]{};
  for (uint32_t i = 0; i < 21; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = desc_sets_[slot];
    w[i].dstBinding = i;
    w[i].descriptorCount = 1;
  }
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  w[0].pNext = &as_write;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         w[1].pBufferInfo = &b_inst;
  w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         w[2].pBufferInfo = &b_geom;
  w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         w[3].pBufferInfo = &b_light;
  w[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         w[4].pBufferInfo = &b_ubo;
  w[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[5].pImageInfo = &i_env;
  w[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[6].pImageInfo = &i_depth;
  w[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[7].pImageInfo = &i_motion;
  w[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          w[8].pImageInfo = &i_hdr;
  w[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          w[9].pImageInfo = &i_gnorm;
  w[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;         w[10].pImageInfo = &i_galb;
  w[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;         w[11].pImageInfo = &i_refl;
  w[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;         w[12].pImageInfo = &i_shao;
  w[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;         w[13].pImageInfo = &i_pt;
  w[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;         w[14].pImageInfo = &i_hprev;
  w[15].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;         w[15].pImageInfo = &i_hcurr;
  w[16].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;         w[16].pImageInfo = &i_tmp;
  w[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;         w[17].pImageInfo = &i_mprev;
  w[18].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;         w[18].pImageInfo = &i_mcurr;
  w[19].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;        w[19].pBufferInfo = &b_res_prev;
  w[20].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;        w[20].pBufferInfo = &b_res_curr;
  vkUpdateDescriptorSets(r_.device_, 21, w, 0, nullptr);

  // Update Set 2 bindless geometry storage buffers for this frame slot.
  {
    const auto& gbufs = scene.geom_buffers();
    std::vector<VkDescriptorBufferInfo> gb_infos;
    if (!gbufs.empty()) {
      const size_t n = std::min<size_t>(gbufs.size(), RtScene::kMaxRtGeomBuffers);
      gb_infos.resize(n);
      for (size_t i = 0; i < n; ++i) {
        gb_infos[i] = {gbufs[i], 0, VK_WHOLE_SIZE};
      }
    } else if (scene.fallback_geom_buffer()) {
      gb_infos.push_back({scene.fallback_geom_buffer(), 0, VK_WHOLE_SIZE});
    }
    if (!gb_infos.empty()) {
      VkWriteDescriptorSet gw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      gw.dstSet = geom_buf_sets_[slot];
      gw.dstBinding = 0;
      gw.dstArrayElement = 0;
      gw.descriptorCount = uint32_t(gb_infos.size());
      gw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      gw.pBufferInfo = gb_infos.data();
      vkUpdateDescriptorSets(r_.device_, 1, &gw, 0, nullptr);
    }
  }

  VkDescriptorSet set1 =
      (host.gpu_world_ && host.gpu_world_->bindless_set()) ? host.gpu_world_->bindless_set() : fallback_bindless_set_;
  const VkDescriptorSet bound[3] = {desc_sets_[slot], set1, geom_buf_sets_[slot]};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout_, 0, 3, bound, 0, nullptr);

  const uint32_t gx = (width + 7u) / 8u;
  const uint32_t gy = (height + 7u) / 8u;
  auto dispatch = [&](VkPipeline pipe, int32_t p0, int32_t p1 = 0) {
    const int32_t pc[4] = {p0, p1, 0, 0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdPushConstants(cmd, pipe_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(cmd, std::max(gx, 1u), std::max(gy, 1u), 1);
  };

  if (mode == rt::RtMode::kHybrid) {
    // Tier 1: Inline VK_KHR_ray_query RT Reflections + Soft Sun/Headlight Shadows + RTAO.
    // rt_refl populates rt_gbuf_norm/rt_gbuf_albedo (or reads external thin G-buf).
    // rt_ao reads shad_ao_img_.r written by rt_shad and re-writes (.r, .g), so a barrier
    // between rt_shad and rt_ao is required to prevent RAW/WAW hazards.
    dispatch(refl_pipe_, 0, external_thin_gbuf_ ? 1 : 0);
    compute_barrier(cmd);
    r_.mark(cmd, "rt_refl");

    dispatch(shad_pipe_, 0);
    compute_barrier(cmd);
    r_.mark(cmd, "rt_shad");

    dispatch(ao_pipe_, 0);
    compute_barrier(cmd);
    r_.mark(cmd, "rt_ao");

    if (gfx.rt_denoise) {
      dispatch(svgf_temp_pipe_, 0);
      compute_barrier(cmd);
      // 4-pass A-Trous wavelet filter (ping-pong ends back in rt_hist_curr!).
      dispatch(svgf_atrous_pipe_, 1, 0); compute_barrier(cmd);
      dispatch(svgf_atrous_pipe_, 2, 1); compute_barrier(cmd);
      dispatch(svgf_atrous_pipe_, 4, 0); compute_barrier(cmd);
      dispatch(svgf_atrous_pipe_, 8, 1); compute_barrier(cmd);
      r_.mark(cmd, "rt_denoise");
    }

    dispatch(comp_pipe_, gfx.rt_denoise ? 1 : 0);
    r_.mark(cmd, "rt_comp");
  } else if (mode == rt::RtMode::kPathTraced) {
    // Tier 2: Full Real-Time Multi-Bounce Path Tracing + ReSTIR DI + SVGF.
    dispatch(pt_pipe_, 0);
    compute_barrier(cmd);
    r_.mark(cmd, "rt_pt");

    if (gfx.rt_denoise) {
      dispatch(svgf_temp_pipe_, 0);
      compute_barrier(cmd);
      dispatch(svgf_atrous_pipe_, 1, 0); compute_barrier(cmd);
      dispatch(svgf_atrous_pipe_, 2, 1); compute_barrier(cmd);
      dispatch(svgf_atrous_pipe_, 4, 0); compute_barrier(cmd);
      dispatch(svgf_atrous_pipe_, 8, 1); compute_barrier(cmd);
      r_.mark(cmd, "rt_denoise");
    }

    dispatch(comp_pipe_, gfx.rt_denoise ? 1 : 0);
    r_.mark(cmd, "rt_comp");
  } else if (mode == rt::RtMode::kPhotoModePt) {
    // Photo Mode Convergent Progressive Multi-SPP Path Tracing.
    dispatch(pt_pipe_, 0);
    compute_barrier(cmd);
    r_.mark(cmd, "rt_photo_pt");

    dispatch(comp_pipe_, 0);
    r_.mark(cmd, "rt_comp");
  }

  history_valid_ = true;
  hist_idx_ ^= 1u;
}

}  // namespace carview

#else  // !SHIFT_ENABLE_RAY_TRACING

namespace carview {
RtPipeline::RtPipeline(Renderer& r) : r_(r) {}
RtPipeline::~RtPipeline() {}
bool RtPipeline::init(VkDescriptorSetLayout, VkSampler, VkImageView, std::string& error, RetireFn) {
  error = "RT disabled at compile time";
  return false;
}
bool RtPipeline::prewarm_targets(uint32_t, uint32_t) { return false; }
void RtPipeline::shutdown() {}
void RtPipeline::record(VkCommandBuffer, const Showroom&, const RtScene&, VkImage, VkImageView, VkImageView,
                        VkImageView, uint32_t, uint32_t) {}
}  // namespace carview

#endif
