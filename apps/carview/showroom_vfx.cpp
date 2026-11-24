#include "showroom_internal.h"

#include "particle_frag_spv.h"
#include "particle_vert_spv.h"
#include "skid_frag_spv.h"
#include "skid_vert_spv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace carview {

namespace {

void blit_cell_bilinear(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, uint32_t atlas_w,
                        uint32_t cell_x, uint32_t cell_y, uint32_t cell_sz) {
  if (!src || sw == 0 || sh == 0) return;
  for (uint32_t y = 0; y < cell_sz; ++y) {
    const float fy = (float(y) + 0.5f) * float(sh) / float(cell_sz) - 0.5f;
    const int y0 = std::clamp(int(std::floor(fy)), 0, int(sh) - 1);
    const int y1 = std::clamp(y0 + 1, 0, int(sh) - 1);
    const float wy = std::clamp(fy - float(y0), 0.0f, 1.0f);
    for (uint32_t x = 0; x < cell_sz; ++x) {
      const float fx = (float(x) + 0.5f) * float(sw) / float(cell_sz) - 0.5f;
      const int x0 = std::clamp(int(std::floor(fx)), 0, int(sw) - 1);
      const int x1 = std::clamp(x0 + 1, 0, int(sw) - 1);
      const float wx = std::clamp(fx - float(x0), 0.0f, 1.0f);
      const uint8_t* p00 = src + (size_t(y0) * sw + size_t(x0)) * 4;
      const uint8_t* p10 = src + (size_t(y0) * sw + size_t(x1)) * 4;
      const uint8_t* p01 = src + (size_t(y1) * sw + size_t(x0)) * 4;
      const uint8_t* p11 = src + (size_t(y1) * sw + size_t(x1)) * 4;
      uint8_t* o = dst + (size_t(cell_y * cell_sz + y) * atlas_w + size_t(cell_x * cell_sz + x)) * 4;
      for (int c = 0; c < 4; ++c) {
        const float top = float(p00[c]) * (1.0f - wx) + float(p10[c]) * wx;
        const float bot = float(p01[c]) * (1.0f - wx) + float(p11[c]) * wx;
        o[c] = uint8_t(std::clamp(top * (1.0f - wy) + bot * wy + 0.5f, 0.0f, 255.0f));
      }
    }
  }
}

void fill_soft_puff_cell(uint8_t* dst, uint32_t atlas_w, uint32_t cell_sz) {
  for (uint32_t y = 0; y < cell_sz; ++y) {
    const float ny = (float(y) + 0.5f) / float(cell_sz) * 2.0f - 1.0f;
    for (uint32_t x = 0; x < cell_sz; ++x) {
      const float nx = (float(x) + 0.5f) / float(cell_sz) * 2.0f - 1.0f;
      const float r = std::sqrt(nx * nx + ny * ny);
      const float ang = std::atan2(ny, nx);
      const float billow = 0.88f + 0.12f * std::sin(ang * 5.0f + r * 3.0f) * std::cos(ang * 3.0f - r * 2.0f);
      const float a = std::clamp(1.0f - r / billow, 0.0f, 1.0f);
      const float soft = a * a * (3.0f - 2.0f * a);
      const uint8_t shade = uint8_t(std::clamp(210.0f + 45.0f * soft, 0.0f, 255.0f));
      uint8_t* o = dst + (size_t(y) * atlas_w + size_t(x)) * 4;
      o[0] = shade;
      o[1] = shade;
      o[2] = shade;
      o[3] = uint8_t(std::clamp(soft * 245.0f, 0.0f, 255.0f));
    }
  }
}

std::vector<uint8_t> make_skid_texture(uint32_t sz, bool is_rut) {
  std::vector<uint8_t> rgba(size_t(sz) * sz * 4, 0);
  auto hash2 = [](int x, int y) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
  };
  for (uint32_t y = 0; y < sz; ++y) {
    const float v = float(y) / float(sz);
    for (uint32_t x = 0; x < sz; ++x) {
      const float u = float(x) / float(sz);
      const float edge = std::sin(std::clamp(u, 0.0f, 1.0f) * 3.14159265f);
      const float n = hash2(int(x / 2), int(y / 2)) * 0.35f + hash2(int(x), int(y)) * 0.15f;
      uint8_t* o = rgba.data() + (size_t(y) * sz + size_t(x)) * 4;
      if (!is_rut) {
        // Asphalt rubber: 4 longitudinal tyre-tread ribs with feathered edges.
        const float ribs = 0.65f + 0.35f * std::pow(std::fabs(std::sin(u * 4.0f * 3.14159265f)), 0.45f);
        const float streak = 0.85f + 0.15f * std::sin(v * 12.566f + u * 25.0f);
        const float a = std::clamp(std::pow(edge, 0.45f) * ribs * streak * (0.85f + 0.2f * n), 0.0f, 1.0f);
        o[0] = 28;
        o[1] = 28;
        o[2] = 30;
        o[3] = uint8_t(a * 240.0f);
      } else {
        // Dirt/gravel rut: darker depressed centre + pale dusty shoulder ridges + tread chevrons.
        const float chev = 0.80f + 0.20f * std::sin((v * 14.0f + std::fabs(u - 0.5f) * 8.0f) * 6.28318f);
        const float ridge = std::exp(-std::pow((std::fabs(u - 0.5f) - 0.38f) / 0.12f, 2.0f));
        const float shade = std::clamp(0.62f + 0.32f * ridge + 0.12f * n, 0.0f, 1.0f) * chev;
        const float a = std::clamp(std::pow(edge, 0.55f) * (0.78f + 0.22f * ridge) * (0.82f + 0.25f * n), 0.0f, 1.0f);
        o[0] = uint8_t(std::clamp(shade * 235.0f, 0.0f, 255.0f));
        o[1] = uint8_t(std::clamp(shade * 212.0f, 0.0f, 255.0f));
        o[2] = uint8_t(std::clamp(shade * 180.0f, 0.0f, 255.0f));
        o[3] = uint8_t(a * 225.0f);
      }
    }
  }
  return rgba;
}

}  // namespace

bool Showroom::init_vfx(const world::ParticleLibrary* lib, std::string& error) {
  if (guest_ || !r_.device_) return true;
  if (vfx_ready_) return true;

  // 1. Build 2048x2048 RGBA8 particle texture atlas (8x8 grid of 256x256 cells).
  std::vector<uint8_t> atlas_rgba(size_t(kParticleAtlasSize) * kParticleAtlasSize * 4, 0);
  fill_soft_puff_cell(atlas_rgba.data(), kParticleAtlasSize, kParticleAtlasCell);
  atlas_tex_slot_.clear();

  if (lib) {
    // Prioritize core vehicle/world textures first, then fill remaining slots up to 63.
    std::vector<std::string> tex_order = {
        "gr_puffy_smoke_diff.xds",   "gr_backfire_diff.xds",     "vehicle_sparks_diff.xds",
        "gr_dirt_8x1_diff.xds",      "gr_gravel_8x1_diff.xds",   "gr_woodsplinters_diff.xds",
        "gr_aspen_leaves_diff.xds",  "gr_grass_8x1_diff.xds",    "gr_glass_diff.xds",
    };
    for (const auto& [ename, def] : lib->effects()) {
      if (!def.texture_name.empty() &&
          std::find(tex_order.begin(), tex_order.end(), def.texture_name) == tex_order.end()) {
        tex_order.push_back(def.texture_name);
      }
    }
    uint16_t slot = 1;
    const uint16_t max_slots = uint16_t(kParticleAtlasGrid * kParticleAtlasGrid);
    for (const auto& tname : tex_order) {
      if (slot >= max_slots) break;
      fh1::RgbaImage img;
      if (lib->decode_texture(tname, img)) {
        const uint32_t cx = slot % kParticleAtlasGrid;
        const uint32_t cy = slot / kParticleAtlasGrid;
        blit_cell_bilinear(img.rgba.data(), img.width, img.height, atlas_rgba.data(), kParticleAtlasSize, cx, cy,
                           kParticleAtlasCell);
        atlas_tex_slot_[tname] = slot;
        ++slot;
      }
    }
  }

  auto upload_rgba_image = [&](const uint8_t* pixels, uint32_t w, uint32_t h, bool srgb, GpuTexture& dst,
                               const char* label) -> bool {
    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    Renderer::Buffer staging;
    if (!r_.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)) {
      error = std::string(label) + " staging";
      return false;
    }
    void* mapped = nullptr;
    if (vmaMapMemory(r_.allocator_, staging.allocation, &mapped) != VK_SUCCESS) {
      r_.destroy_buffer(staging);
      error = std::string(label) + " map";
      return false;
    }
    std::memcpy(mapped, pixels, size_t(bytes));
    vmaUnmapMemory(r_.allocator_, staging.allocation);
    const VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (!r_.create_image(w, h, 1, fmt, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                         dst)) {
      r_.destroy_buffer(staging);
      error = std::string(label) + " image";
      return false;
    }
    const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
      VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = dst.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &b;
      vkCmdPipelineBarrier2(cmd, &dep);
      VkBufferImageCopy reg{};
      reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      reg.imageExtent = {w, h, 1};
      vkCmdCopyBufferToImage(cmd, staging.buffer, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
      b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier2(cmd, &dep);
    });
    r_.destroy_buffer(staging);
    if (!ok) error = std::string(label) + " upload";
    return ok;
  };

  if (!upload_rgba_image(atlas_rgba.data(), kParticleAtlasSize, kParticleAtlasSize, true, particle_atlas_,
                         "particle atlas"))
    return false;
  const auto skid_asph = make_skid_texture(256, false);
  const auto skid_rut = make_skid_texture(256, true);
  if (!upload_rgba_image(skid_asph.data(), 256, 256, true, skid_asphalt_tex_, "skid asphalt")) return false;
  if (!upload_rgba_image(skid_rut.data(), 256, 256, true, skid_rut_tex_, "skid rut")) return false;

  // 2. Samplers.
  VkSamplerCreateInfo psamp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  psamp.magFilter = psamp.minFilter = VK_FILTER_LINEAR;
  psamp.addressModeU = psamp.addressModeV = psamp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_TRY(vkCreateSampler(r_.device_, &psamp, nullptr, &particle_sampler_), "particle sampler");

  VkSamplerCreateInfo ssamp = psamp;
  ssamp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  ssamp.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  ssamp.anisotropyEnable = r_.sampler_anisotropy() > 1.0f ? VK_TRUE : VK_FALSE;
  ssamp.maxAnisotropy = r_.sampler_anisotropy();
  VK_TRY(vkCreateSampler(r_.device_, &ssamp, nullptr, &skid_sampler_), "skid sampler");

  // 3. Mapped host-visible dynamic buffers.
  if (!create_dyn_buffer(particle_buf_, VkDeviceSize(kMaxParticleGpu) * sizeof(ParticleGpu), error)) {
    error = "particle instance buffer";
    return false;
  }
  if (!create_dyn_buffer(skid_asphalt_buf_, VkDeviceSize(kMaxSkidVerts) * sizeof(SkidGpuVertex), error) ||
      !create_dyn_buffer(skid_rut_buf_, VkDeviceSize(kMaxSkidVerts) * sizeof(SkidGpuVertex), error) ||
      !create_dyn_buffer(debris_buf_, VkDeviceSize(kMaxDebrisVerts) * sizeof(WorldVertex), error) ||
      !create_dyn_buffer(showcase_buf_, VkDeviceSize(kMaxShowcaseVerts) * sizeof(WorldVertex), error) ||
      !create_dyn_buffer(driver_buf_, VkDeviceSize(kMaxDriverVerts) * sizeof(WorldVertex), error)) {
    error = "skidmark/debris/driver vertex buffers";
    return false;
  }
  if (!options_.game_root.empty()) {
    std::string spec_err;
    if (spectator_anim_.init(options_.game_root / "media", world::TrackPostConfig{}, &spec_err) &&
        spectator_anim_.catalog().has_3d_spectators()) {
      const auto& cat = spectator_anim_.catalog();
      if (upload_rgba_image(cat.atlas_rgba.data(), world::SpectatorCatalog::kAtlasSize,
                            world::SpectatorCatalog::kAtlasSize, true, spectator_atlas_, "spectator atlas") &&
          create_world_single_set(spectator_atlas_.view, spectator_set_, spec_err) &&
          create_dyn_buffer(spectator_buf_, VkDeviceSize(kMaxSpectatorVerts) * sizeof(WorldVertex), spec_err)) {
        // Authentic Spectators.zip 3D crowd atlas + dynamic vertex buffer ready.
      }
    }
  }
  update_driver_mesh(driver_wheel_rad_);

  // 4. Descriptor sets & pipeline layouts.
  {
    const VkDescriptorSetLayoutBinding pb[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 2;
    dli.pBindings = pb;
    VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &dli, nullptr, &particle_set_layout_), "particle set layout");

    const VkDescriptorSetLayout sets[2] = {frame_layout_, particle_set_layout_};
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    VK_TRY(vkCreatePipelineLayout(r_.device_, &pli, nullptr, &particle_pipe_layout_), "particle pipe layout");

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VK_TRY(vkCreateDescriptorPool(r_.device_, &dpi, nullptr, &particle_pool_), "particle pool");

    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = particle_pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &particle_set_layout_;
    VK_TRY(vkAllocateDescriptorSets(r_.device_, &dai, &particle_set_), "particle set");

    VkDescriptorImageInfo atlas_info{particle_sampler_, particle_atlas_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo depth_fallback{particle_sampler_, gpu_textures_[0].view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstSet = particle_set_;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &atlas_info;
    w[1].dstSet = particle_set_;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[1].pImageInfo = &depth_fallback;
    vkUpdateDescriptorSets(r_.device_, 2, w, 0, nullptr);
    particle_bound_depth_ = VK_NULL_HANDLE;
  }

  {
    const VkDescriptorSetLayoutBinding sb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                          nullptr};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 1;
    dli.pBindings = &sb;
    VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &dli, nullptr, &skid_set_layout_), "skid set layout");

    const VkDescriptorSetLayout sets[2] = {frame_layout_, skid_set_layout_};
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    VK_TRY(vkCreatePipelineLayout(r_.device_, &pli, nullptr, &skid_pipe_layout_), "skid pipe layout");

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VK_TRY(vkCreateDescriptorPool(r_.device_, &dpi, nullptr, &skid_pool_), "skid pool");

    const VkDescriptorSetLayout layouts[2] = {skid_set_layout_, skid_set_layout_};
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = skid_pool_;
    dai.descriptorSetCount = 2;
    dai.pSetLayouts = layouts;
    VK_TRY(vkAllocateDescriptorSets(r_.device_, &dai, skid_sets_), "skid sets");

    VkDescriptorImageInfo imgs[2] = {
        {skid_sampler_, skid_asphalt_tex_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {skid_sampler_, skid_rut_tex_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (int i = 0; i < 2; ++i) {
      w[i].dstSet = skid_sets_[i];
      w[i].dstBinding = 0;
      w[i].descriptorCount = 1;
      w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w[i].pImageInfo = &imgs[i];
    }
    vkUpdateDescriptorSets(r_.device_, 2, w, 0, nullptr);
  }

  // 5. Graphics pipelines (particle alpha + additive, and skidmark).
  auto make_mod = [&](const uint32_t* code, size_t bytes, VkShaderModule& m) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    return vkCreateShaderModule(r_.device_, &ci, nullptr, &m);
  };
  const VkBool32 spec_vals[2] = {r_.post() ? VK_TRUE : VK_FALSE, r_.reverse_z() ? VK_TRUE : VK_FALSE};
  const VkSpecializationMapEntry spec_map[2] = {{0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}};
  VkSpecializationInfo spec{2, spec_map, sizeof(spec_vals), spec_vals};
  VkSpecializationInfo spec1{1, spec_map, sizeof(VkBool32), spec_vals};

  const VkFormat color_fmt = r_.scene_format();
  VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rend.colorAttachmentCount = 1;
  rend.pColorAttachmentFormats = &color_fmt;
  rend.depthAttachmentFormat = r_.depth_format_;

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
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_FALSE;
  ds.depthCompareOp = r_.reverse_z() ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
  const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_states;

  {
    VkShaderModule pv = VK_NULL_HANDLE, pf = VK_NULL_HANDLE;
    VK_TRY(make_mod(kParticle_vert_spv, sizeof(kParticle_vert_spv), pv), "particle vert");
    VK_TRY(make_mod(kParticle_frag_spv, sizeof(kParticle_frag_spv), pf), "particle frag");
    const VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, pv, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, pf, "main",
         &spec},
    };
    const VkVertexInputBindingDescription bind{0, sizeof(ParticleGpu), VK_VERTEX_INPUT_RATE_INSTANCE};
    const VkVertexInputAttributeDescription attrs[6] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},  {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32}, {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 48},
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 64}, {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 80},
    };
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
    ba.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
    gp.layout = particle_pipe_layout_;
    VkResult r1 =
        vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &gp, nullptr, &particle_alpha_pipe_);
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    VkResult r2 = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &gp, nullptr, &particle_add_pipe_);
    vkDestroyShaderModule(r_.device_, pv, nullptr);
    vkDestroyShaderModule(r_.device_, pf, nullptr);
    VK_TRY(r1, "particle alpha pipeline");
    VK_TRY(r2, "particle additive pipeline");
  }

  {
    VkShaderModule sv = VK_NULL_HANDLE, sf = VK_NULL_HANDLE;
    VK_TRY(make_mod(kSkid_vert_spv, sizeof(kSkid_vert_spv), sv), "skid vert");
    VK_TRY(make_mod(kSkid_frag_spv, sizeof(kSkid_frag_spv), sf), "skid frag");
    const VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, sv, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, sf, "main",
         &spec1},
    };
    const VkVertexInputBindingDescription bind{0, sizeof(SkidGpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attrs[4] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, 32},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineRasterizationStateCreateInfo srs = rs;
    srs.depthBiasEnable = VK_TRUE;
    srs.depthBiasConstantFactor = r_.reverse_z() ? kDecalBiasConstant : -kDecalBiasConstant;
    srs.depthBiasSlopeFactor = r_.reverse_z() ? kDecalBiasSlope : -kDecalBiasSlope;

    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
    gp.pRasterizationState = &srs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = skid_pipe_layout_;
    VkResult sr = vkCreateGraphicsPipelines(r_.device_, r_.pipeline_cache(), 1, &gp, nullptr, &skid_pipe_);
    vkDestroyShaderModule(r_.device_, sv, nullptr);
    vkDestroyShaderModule(r_.device_, sf, nullptr);
    VK_TRY(sr, "skid pipeline");
  }

  vfx_ready_ = true;
  return true;
}

void Showroom::set_particles(const std::vector<world::ParticleSprite>& alpha,
                             const std::vector<world::ParticleSprite>& additive,
                             const std::vector<std::string>& used_textures, float near_plane, bool cockpit_camera) {
  particle_alpha_count_ = 0;
  particle_add_count_ = 0;
  particle_near_plane_ = std::max(near_plane, 0.05f);
  particle_cockpit_ = cockpit_camera;
  if (!vfx_ready_ || !particle_buf_.ready()) return;

  const float* m = world_transform_;
  auto xform_pt = [&](const std::array<float, 3>& g) -> std::array<float, 3> {
    const float rx = float(double(g[0]) - world_origin_[0]);
    const float ry = g[1];
    const float rz = float(double(g[2]) - world_origin_[1]);
    return {
        m[0] * rx + m[4] * ry + m[8] * rz + m[12],
        m[1] * rx + m[5] * ry + m[9] * rz + m[13],
        m[2] * rx + m[6] * ry + m[10] * rz + m[14],
    };
  };
  auto xform_dir = [&](const std::array<float, 3>& v) -> std::array<float, 3> {
    return {
        m[0] * v[0] + m[4] * v[1] + m[8] * v[2],
        m[1] * v[0] + m[5] * v[1] + m[9] * v[2],
        m[2] * v[0] + m[6] * v[1] + m[10] * v[2],
    };
  };

  auto* dst = static_cast<ParticleGpu*>(particle_buf_.data());
  uint32_t out_n = 0;
  const float cell_uv = 1.0f / float(kParticleAtlasGrid);
  const float inset = 1.5f / float(kParticleAtlasSize);

  auto emit_batch = [&](const std::vector<world::ParticleSprite>& list) -> uint32_t {
    uint32_t added = 0;
    for (const auto& sp : list) {
      if (out_n >= kMaxParticleGpu) break;
      uint16_t atlas_slot = 0;
      if (sp.texture_slot > 0 && size_t(sp.texture_slot - 1) < used_textures.size()) {
        auto it = atlas_tex_slot_.find(used_textures[size_t(sp.texture_slot - 1)]);
        if (it != atlas_tex_slot_.end()) atlas_slot = it->second;
      }
      const uint32_t cx = atlas_slot % kParticleAtlasGrid;
      const uint32_t cy = atlas_slot / kParticleAtlasGrid;
      const int fx = std::max(1, sp.frames_x);
      const int fy = std::max(1, sp.frames_y);
      const int fi = std::max(0, sp.frame_index) % (fx * fy);
      const int fcol = fi % fx;
      const int frow = fi / fx;
      const float sub_u = (cell_uv - 2.0f * inset) / float(fx);
      const float sub_v = (cell_uv - 2.0f * inset) / float(fy);
      const float u0 = float(cx) * cell_uv + inset + float(fcol) * sub_u;
      const float v0 = float(cy) * cell_uv + inset + float(frow) * sub_v;

      const auto rp = xform_pt(sp.pos);
      const auto rv = xform_dir(sp.vel);
      uint32_t flags = (sp.velocity_aligned ? 1u : 0u) | (sp.dynamic_lighting ? 2u : 0u) | (sp.additive ? 4u : 0u);
      const uint32_t scr_byte = uint32_t(std::clamp(sp.max_screen_size * 100.0f, 8.0f, 95.0f));
      flags |= (scr_byte << 8u);

      ParticleGpu& g = dst[out_n++];
      g.pos_sx[0] = rp[0];
      g.pos_sx[1] = rp[1];
      g.pos_sx[2] = rp[2];
      g.pos_sx[3] = sp.size_x;
      g.vel_sy[0] = rv[0];
      g.vel_sy[1] = rv[1];
      g.vel_sy[2] = rv[2];
      g.vel_sy[3] = sp.size_y;
      g.uv_rect[0] = u0;
      g.uv_rect[1] = v0;
      g.uv_rect[2] = sub_u;
      g.uv_rect[3] = sub_v;
      for (int c = 0; c < 4; ++c) {
        g.color[c] = sp.rgba[c];
        g.light[c] = sp.light_influence[c];
      }
      g.extra[0] = sp.spin_rad;
      g.extra[1] = sp.soft_depth_m;
      g.extra[2] = sp.headlight_power;
      g.extra[3] = float(flags);
      ++added;
    }
    return added;
  };

  particle_alpha_count_ = emit_batch(alpha);
  particle_add_count_ = emit_batch(additive);
  ++particle_buf_.ver;
}

void Showroom::set_skidmarks(const std::vector<world::SkidVertex>& asphalt_tris,
                             const std::vector<world::SkidVertex>& rut_tris) {
  skid_asphalt_count_ = 0;
  skid_rut_count_ = 0;
  if (!vfx_ready_ || !skid_asphalt_buf_.ready() || !skid_rut_buf_.ready()) return;

  auto pack = [&](const std::vector<world::SkidVertex>& src, void* mapped) -> uint32_t {
    const uint32_t n = uint32_t(std::min(src.size(), size_t(kMaxSkidVerts)));
    auto* dst = static_cast<SkidGpuVertex*>(mapped);
    for (uint32_t i = 0; i < n; ++i) {
      const auto& v = src[i];
      dst[i].pos[0] = float(double(v.pos[0]) - world_origin_[0]);
      dst[i].pos[1] = v.pos[1];
      dst[i].pos[2] = float(double(v.pos[2]) - world_origin_[1]);
      dst[i].normal[0] = v.normal[0];
      dst[i].normal[1] = v.normal[1];
      dst[i].normal[2] = v.normal[2];
      dst[i].uv[0] = v.uv[0];
      dst[i].uv[1] = v.uv[1];
      for (int c = 0; c < 4; ++c) dst[i].rgba[c] = v.rgba[c];
    }
    return n;
  };
  skid_asphalt_count_ = pack(asphalt_tris, skid_asphalt_buf_.data());
  skid_rut_count_ = pack(rut_tris, skid_rut_buf_.data());
  ++skid_asphalt_buf_.ver;
  ++skid_rut_buf_.ver;
}

void Showroom::set_debris_mesh(const std::vector<world::DebrisVertex>& tris) {
  debris_count_ = 0;
  debris_batches_.clear();
  if (!vfx_ready_ || !debris_buf_.ready() || tris.empty()) return;
  // Keep vertex count a multiple of 3 (complete triangles).
  const uint32_t n = uint32_t(std::min(tris.size(), size_t(kMaxDebrisVerts)) / 3u) * 3u;
  if (n == 0) return;
  auto* dst = static_cast<WorldVertex*>(debris_buf_.data());
  auto snorm8 = [](float x) -> int8_t {
    return int8_t(std::clamp(std::lround(double(x) * 127.0), -127l, 127l));
  };
  for (uint32_t i = 0; i < n; ++i) {
    const auto& v = tris[i];
    WorldVertex& o = dst[i];
    o = WorldVertex{};
    o.position[0] = float(double(v.pos[0]) - world_origin_[0]);
    o.position[1] = v.pos[1];
    o.position[2] = float(double(v.pos[2]) - world_origin_[1]);
    o.normal[0] = snorm8(v.normal[0]);
    o.normal[1] = snorm8(v.normal[1]);
    o.normal[2] = snorm8(v.normal[2]);
    o.normal[3] = 0;
    o.uv0[0] = v.uv[0];
    o.uv0[1] = v.uv[1];
    for (int c = 0; c < 4; ++c) o.colour[c] = v.rgba[c];
  }
  for (uint32_t i = 0; i < n; i += 3u) {
    const uint32_t tex = tris[i].texture_record;
    const uint8_t cut = tris[i].cutout;
    if (!debris_batches_.empty() && debris_batches_.back().texture_record == tex &&
        debris_batches_.back().cutout == cut) {
      debris_batches_.back().vertex_count += 3u;
    } else {
      debris_batches_.push_back({i, 3u, tex, cut});
    }
  }
  debris_count_ = n;
  ++debris_buf_.ver;
}


void Showroom::record_debris_draw(VkCommandBuffer cmd) {
  if (!vfx_ready_ || !world_opaque_) return;
  const bool draw_debris = debris_count_ > 0 && world_white_set_;
  const bool draw_spectators =
      spectator_set_ && spectator_buf_.ready() && spectator_anim_.catalog().has_3d_spectators();
  if (!draw_debris && !draw_spectators) return;

  VkPipeline bound_pipe = VK_NULL_HANDLE;
  auto bind_pipe = [&](VkPipeline p) {
    if (p != bound_pipe) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_pipe = p);
      vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
      r_.apply_vrs(cmd);
    }
  };

  ShowroomPush pc{};
  std::memcpy(pc.model, world_transform_, sizeof pc.model);
  for (int i = 0; i < 3; ++i) {
    pc.uv0[i] = world_sun_[i];
    pc.uv1[i] = world_up_[i];
  }
  pc.uv0[3] = 2.0f;  // swap = none
  pc.uv1[3] = world_up_[3];
  pc.tint[0] = pc.tint[1] = pc.tint[2] = pc.tint[3] = 1.0f;
  pc.params[0] = 0.0f;  // kOpaque
  pc.params[2] = 0.0f;
  pc.params[3] = 1.0f;

  if (draw_debris) {
    const VkDeviceSize at = dyn_sync(debris_buf_, VkDeviceSize(debris_count_) * sizeof(WorldVertex));
    vkCmdBindVertexBuffers(cmd, 0, 1, &debris_buf_.buf.buffer, &at);
    const bool single_sample = r_.samples_ == VK_SAMPLE_COUNT_1_BIT;
    for (const auto& b : debris_batches_) {
      if (b.vertex_count == 0) continue;
      VkDescriptorSet tex_set =
          (b.texture_record != kNoWorldTexture) ? world_set(b.texture_record) : VK_NULL_HANDLE;
      if (tex_set != VK_NULL_HANDLE) {
        // Use two-sided world_coverage_ for real FH1 smashable .rmb.bin debris so thin sign boards,
        // hollow planks, and cutout fence pieces render cleanly from both sides while tumbling.
        bind_pipe(world_coverage_ ? world_coverage_ : world_opaque_);
        const VkDescriptorSet sets[2] = {frame_set_, tex_set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 2, sets, 0, nullptr);
        uint32_t fl = 1u;  // kFlagTextured
        if (b.cutout && single_sample) fl |= 64u;  // kFlagAlphaTest
        pc.params[0] = b.cutout ? 1.0f : 0.0f;
        pc.params[1] = float(fl);
        vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof pc, &pc);
        vkCmdDraw(cmd, b.vertex_count, 1, b.first_vertex, 0);
      } else {
        bind_pipe(world_opaque_);
        const VkDescriptorSet sets[2] = {frame_set_, world_white_set_};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 2, sets, 0, nullptr);
        pc.params[0] = 0.0f;
        pc.params[1] = 65536.0f;  // kFlagVertexLit
        vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof pc, &pc);
        vkCmdDraw(cmd, b.vertex_count, 1, b.first_vertex, 0);
      }
    }
  }

  if (draw_spectators) {
    bind_pipe(world_opaque_);
    pc.params[0] = 0.0f;
    const auto eye = eye_in_scenery(world_transform_);
    const double eye_gx = world_origin_[0] + double(eye[0]);
    const double eye_gy = double(eye[1]);
    const double eye_gz = world_origin_[1] + double(eye[2]);
    const drive::Ground* active_ground = world_ground_ ? world_ground_ : world::AnimatedWorldSystem::last_ground();
    spectator_cpu_.clear();
    spectator_anim_.build_spectator_mesh(eye_gx, eye_gy, eye_gz, tod_real_time_, active_ground, spectator_cpu_);
    const uint32_t n = uint32_t(std::min(spectator_cpu_.size(), size_t(kMaxSpectatorVerts)));
    spectator_count_ = n;
    if (n > 0) {
      auto* dst = static_cast<WorldVertex*>(spectator_buf_.data());
      auto snorm8 = [](float x) -> int8_t {
        return int8_t(std::clamp(std::lround(double(x) * 127.0), -127l, 127l));
      };
      for (uint32_t i = 0; i < n; ++i) {
        const auto& v = spectator_cpu_[i];
        WorldVertex& o = dst[i];
        o = WorldVertex{};
        o.position[0] = float(double(v.pos[0]) - world_origin_[0]);
        o.position[1] = v.pos[1];
        o.position[2] = float(double(v.pos[2]) - world_origin_[1]);
        o.normal[0] = snorm8(v.normal[0]);
        o.normal[1] = snorm8(v.normal[1]);
        o.normal[2] = snorm8(v.normal[2]);
        o.normal[3] = 0;
        o.uv0[0] = v.uv[0];
        o.uv0[1] = v.uv[1];
        for (int c = 0; c < 4; ++c) o.colour[c] = v.rgba[c];
      }
      ++spectator_buf_.ver;
      const VkDescriptorSet sets[2] = {frame_set_, spectator_set_};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 2, sets, 0, nullptr);
      pc.params[1] = 1.0f;  // kFlagTextured
      vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof pc, &pc);
      VkDeviceSize at = dyn_sync(spectator_buf_, VkDeviceSize(n) * sizeof(WorldVertex));
      vkCmdBindVertexBuffers(cmd, 0, 1, &spectator_buf_.buf.buffer, &at);
      vkCmdDraw(cmd, n, 1, 0, 0);
    }
  }
}


void Showroom::record_skidmarks_draw(VkCommandBuffer cmd) {
  if (!vfx_ready_ || !skid_pipe_ || (skid_asphalt_count_ == 0 && skid_rut_count_ == 0)) return;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skid_pipe_);
  struct SkidPush {
    float model[16];
    float sun_dir[4];
    float up_dir[4];
  } pc{};
  std::memcpy(pc.model, world_transform_, sizeof pc.model);
  for (int i = 0; i < 3; ++i) {
    pc.sun_dir[i] = world_sun_[i];
    pc.up_dir[i] = world_up_[i];
  }
  pc.sun_dir[3] = world_up_[3];

  if (skid_asphalt_count_ > 0) {
    pc.up_dir[3] = 0.0f;
    const VkDescriptorSet sets[2] = {frame_set_, skid_sets_[0]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skid_pipe_layout_, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, skid_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    VkDeviceSize at = dyn_sync(skid_asphalt_buf_, VkDeviceSize(skid_asphalt_count_) * sizeof(SkidGpuVertex));
    vkCmdBindVertexBuffers(cmd, 0, 1, &skid_asphalt_buf_.buf.buffer, &at);
    vkCmdDraw(cmd, skid_asphalt_count_, 1, 0, 0);
  }
  if (skid_rut_count_ > 0) {
    pc.up_dir[3] = 1.0f;
    const VkDescriptorSet sets[2] = {frame_set_, skid_sets_[1]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skid_pipe_layout_, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, skid_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    VkDeviceSize at = dyn_sync(skid_rut_buf_, VkDeviceSize(skid_rut_count_) * sizeof(SkidGpuVertex));
    vkCmdBindVertexBuffers(cmd, 0, 1, &skid_rut_buf_.buf.buffer, &at);
    vkCmdDraw(cmd, skid_rut_count_, 1, 0, 0);
  }
}

void Showroom::record_particles_draw(VkCommandBuffer cmd) {
  if (!vfx_ready_ || (particle_alpha_count_ == 0 && particle_add_count_ == 0)) return;
  struct ParticlePush {
    float viewport_near[4];
    float sun_dir[4];
    float up_dir[4];
  } pc{};
  const uint32_t w = std::max(1u, r_.render_width());
  const uint32_t h = std::max(1u, r_.render_height());
  pc.viewport_near[0] = 1.0f / float(w);
  pc.viewport_near[1] = 1.0f / float(h);
  pc.viewport_near[2] = particle_near_plane_;
  pc.viewport_near[3] = particle_cockpit_ ? 1.85f : 0.0f;
  for (int i = 0; i < 3; ++i) {
    pc.sun_dir[i] = world_sun_[i];
    pc.up_dir[i] = world_up_[i];
  }
  pc.sun_dir[3] = world_up_[3];
  pc.up_dir[3] = (particle_bound_depth_ != VK_NULL_HANDLE) ? 1.0f : 0.0f;

  const VkDescriptorSet sets[2] = {frame_set_, particle_set_};
  const VkDeviceSize at =
      dyn_sync(particle_buf_, VkDeviceSize(particle_alpha_count_ + particle_add_count_) * sizeof(ParticleGpu));
  if (particle_alpha_count_ > 0 && particle_alpha_pipe_) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particle_alpha_pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particle_pipe_layout_, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, particle_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    vkCmdBindVertexBuffers(cmd, 0, 1, &particle_buf_.buf.buffer, &at);
    vkCmdDraw(cmd, 6, particle_alpha_count_, 0, 0);
  }
  if (particle_add_count_ > 0 && particle_add_pipe_) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particle_add_pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particle_pipe_layout_, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, particle_pipe_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    vkCmdBindVertexBuffers(cmd, 0, 1, &particle_buf_.buf.buffer, &at);
    vkCmdDraw(cmd, 6, particle_add_count_, 0, particle_alpha_count_);
  }
}


}  // namespace carview
