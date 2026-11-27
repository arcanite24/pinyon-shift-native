#include "showroom_internal.h"

#include "cars/shader_mods.h"
#include "cars/texture_mods.h"
#include "contact_shadow_frag_spv.h"
#include "contact_shadow_vert_spv.h"
#include "mask_frag_spv.h"
#include "shadow_frag_spv.h"
#include "showroom_frag_spv.h"
#include "showroom_opaque_frag_spv.h"
#include "showroom_vert_spv.h"
#include "world_frag_spv.h"
#include "world_opaque_frag_spv.h"
#include "world_vert_spv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace carview {

namespace {

// Our studio: a dark floor, a soft grey wall gradient, a large overhead
// softbox and two side strips. Linear radiance.
std::array<float, 3> studio(float x, float y, float z) {
  const float len = std::sqrt(x * x + y * y + z * z);
  x /= len;
  y /= len;
  z /= len;
  float v;
  if (y < 0) {
    v = 0.05f + 0.06f * (1 + y);  // floor, lighter towards the horizon
  } else {
    v = 0.12f + 0.35f * y;
  }
  // Overhead softbox: |x| < 0.45, |z| < 0.7 in the projection on y = 1.
  if (y > 0.5f) {
    const float px = x / y, pz = z / y;
    if (std::fabs(px) < 0.45f && std::fabs(pz) < 0.75f) v = 4.0f;
  }
  // Side strips at either side, tall and thin.
  if (std::fabs(x) > 0.85f && y > -0.1f && y < 0.6f && std::fabs(z) < 0.35f) v = std::max(v, 2.0f);
  return {v, v * 0.98f, v * 0.95f};
}

}  // namespace

bool Showroom::create_texture_image(const bake::Texture& texture, bool srgb, uint32_t levels, GpuTexture& out,
                                    std::string& error, uint32_t skip) {
  if (skip >= levels) skip = levels ? levels - 1 : 0;
  const uint32_t kept = levels - skip;
  const VkFormat format = vk_format(texture.format, srgb);
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {texture.levels[skip].width, texture.levels[skip].height, 1};
  info.mipLevels = kept;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  VmaAllocationInfo alloc_info{};
  VK_TRY(vmaCreateImage(r_.allocator_, &info, &alloc, &out.image, &out.allocation, &alloc_info),
         "vmaCreateImage");
  out.bytes = alloc_info.size;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = out.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.components = {vk_swizzle(texture.swizzle[0]), vk_swizzle(texture.swizzle[1]),
                     vk_swizzle(texture.swizzle[2]), vk_swizzle(texture.swizzle[3])};
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kept, 0, 1};
  if (vkCreateImageView(r_.device_, &view, nullptr, &out.view) != VK_SUCCESS) {
    r_.destroy_image(out);
    error = "vkCreateImageView failed";
    return false;
  }
  return true;
}

bool Showroom::upload_texture(const bake::Texture* texture, bool srgb, GpuTexture& out,
                              std::string& error) {
  // Built-ins: 1x1 white or black.
  bake::Texture builtin;
  if (!texture) {
    builtin.format = bake::TextureFormat::kRgba8;
    builtin.levels.push_back({1, 1, {255, 255, 255, 255}});
    texture = &builtin;
  }
  // A mod's replacement texture (texture_mods.h), by content hash or name.
  bake::Texture replaced;
  if (texture != &builtin && cars::texture_mods::any()) {
    std::string why;
    if (cars::texture_mods::substitute(*texture, replaced, &why)) texture = &replaced;
    else if (!why.empty()) notes_.push_back("replacement texture " + why);
  }
  VkDeviceSize total = 0;
  const uint32_t levels = texture_chain(*texture, total);
  if (!levels) {
    error = "texture " + texture->name + ": level 0 has the wrong size";
    return false;
  }
  const uint32_t skip = modern::texture_levels_to_skip(texture->levels[0].width, texture->levels[0].height, levels,
                                                       texture_cap_);
  VkDeviceSize kept_bytes = 0;
  for (uint32_t i = skip; i < levels; ++i) kept_bytes += texture->levels[i].data.size();
  if (!create_texture_image(*texture, srgb, levels, out, error, skip)) return false;
  Renderer::Buffer staging;
  if (!r_.create_buffer(kept_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        staging)) {
    r_.destroy_image(out);
    error = "texture staging buffer";
    return false;
  }
  uint8_t* mapped = nullptr;
  if (vmaMapMemory(r_.allocator_, staging.allocation, reinterpret_cast<void**>(&mapped)) != VK_SUCCESS) {
    r_.destroy_buffer(staging);
    r_.destroy_image(out);
    error = "texture staging map";
    return false;
  }
  VkDeviceSize offset = 0;
  for (uint32_t i = skip; i < levels; ++i) {
    std::memcpy(mapped + offset, texture->levels[i].data.data(), texture->levels[i].data.size());
    offset += texture->levels[i].data.size();
  }
  vmaUnmapMemory(r_.allocator_, staging.allocation);
  const bool ok = r_.submit_once(
      [&](VkCommandBuffer cmd) { record_texture_copy(cmd, staging.buffer, 0, *texture, levels, out.image, skip); });
  r_.destroy_buffer(staging);
  if (!ok) {
    error = "texture upload";
    return false;
  }
  return true;
}

bool Showroom::upload_textures_batch(const std::vector<TextureUploadItem>& items, std::string& error) {
  if (items.empty()) return true;
  bake::Texture builtin_white;
  builtin_white.format = bake::TextureFormat::kRgba8;
  builtin_white.levels.push_back({1, 1, {255, 255, 255, 255}});

  struct PreparedTex {
    bake::Texture owned_copy;
    const bake::Texture* tex = nullptr;
    GpuTexture* dst = nullptr;
    uint32_t levels = 0;
    uint32_t skip = 0;
    VkDeviceSize offset = 0;
    VkDeviceSize bytes = 0;
  };
  std::vector<PreparedTex> prepared;
  prepared.reserve(items.size());
  VkDeviceSize total_bytes = 0;
  constexpr VkDeviceSize kAlign = 16;

  auto cleanup_all = [&]() {
    for (auto& p : prepared) {
      if (p.dst) r_.destroy_image(*p.dst);
    }
  };

  for (const auto& item : items) {
    if (!item.dst) continue;
    PreparedTex p;
    p.dst = item.dst;
    const bake::Texture* src = item.texture ? item.texture : &builtin_white;
    bool srgb = item.srgb;
    if (src != &builtin_white && cars::texture_mods::any()) {
      std::string why;
      if (cars::texture_mods::substitute(*src, p.owned_copy, &why)) src = &p.owned_copy;
      else if (!why.empty()) notes_.push_back("replacement texture " + why);
    }
    VkDeviceSize total = 0;
    uint32_t levels = texture_chain(*src, total);
    uint32_t skip = levels ? modern::texture_levels_to_skip(src->levels[0].width, src->levels[0].height, levels,
                                                            texture_cap_)
                           : 0;
    std::string local_err;
    if (!levels || !create_texture_image(*src, srgb, levels, *p.dst, local_err, skip)) {
      if (!item.fallback_white) {
        error = levels ? local_err : ("texture " + src->name + ": level 0 has the wrong size");
        cleanup_all();
        return false;
      }
      notes_.push_back(levels ? local_err : ("texture " + src->name + ": level 0 has the wrong size"));
      r_.destroy_image(*p.dst);
      src = &builtin_white;
      srgb = true;
      levels = 1;
      skip = 0;
      if (!create_texture_image(*src, srgb, levels, *p.dst, error, 0)) {
        cleanup_all();
        return false;
      }
    }
    VkDeviceSize kept_bytes = 0;
    for (uint32_t i = skip; i < levels; ++i) kept_bytes += src->levels[i].data.size();
    total_bytes = (total_bytes + kAlign - 1) & ~(kAlign - 1);
    p.tex = src;
    p.levels = levels;
    p.skip = skip;
    p.offset = total_bytes;
    p.bytes = kept_bytes;
    total_bytes += kept_bytes;
    prepared.push_back(std::move(p));
  }
  if (prepared.empty() || total_bytes == 0) return true;

  Renderer::Buffer staging;
  if (!r_.create_buffer(total_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)) {
    cleanup_all();
    error = "batched texture staging buffer";
    return false;
  }
  uint8_t* mapped = nullptr;
  if (vmaMapMemory(r_.allocator_, staging.allocation, reinterpret_cast<void**>(&mapped)) != VK_SUCCESS) {
    r_.destroy_buffer(staging);
    cleanup_all();
    error = "batched texture staging map";
    return false;
  }
  for (const auto& p : prepared) {
    VkDeviceSize off = p.offset;
    for (uint32_t i = p.skip; i < p.levels; ++i) {
      std::memcpy(mapped + off, p.tex->levels[i].data.data(), p.tex->levels[i].data.size());
      off += p.tex->levels[i].data.size();
    }
  }
  vmaUnmapMemory(r_.allocator_, staging.allocation);

  const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
    std::vector<VkImageMemoryBarrier2> pre_barriers;
    std::vector<VkImageMemoryBarrier2> post_barriers;
    pre_barriers.reserve(prepared.size());
    post_barriers.reserve(prepared.size());
    for (const auto& p : prepared) {
      const uint32_t kept = p.levels - p.skip;
      VkImageMemoryBarrier2 pre{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      pre.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      pre.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      pre.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      pre.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      pre.srcQueueFamilyIndex = pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre.image = p.dst->image;
      pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kept, 0, 1};
      pre_barriers.push_back(pre);

      VkImageMemoryBarrier2 post = pre;
      post.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      post.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      post.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      post.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      post.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      post.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      post_barriers.push_back(post);
    }
    VkDependencyInfo pre_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    pre_dep.imageMemoryBarrierCount = uint32_t(pre_barriers.size());
    pre_dep.pImageMemoryBarriers = pre_barriers.data();
    vkCmdPipelineBarrier2(cmd, &pre_dep);

    std::vector<VkBufferImageCopy> copies;
    for (const auto& p : prepared) {
      copies.clear();
      VkDeviceSize off = p.offset;
      for (uint32_t i = p.skip; i < p.levels; ++i) {
        const auto& lvl = p.tex->levels[i];
        VkBufferImageCopy c{};
        c.bufferOffset = off;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - p.skip, 0, 1};
        c.imageExtent = {lvl.width, lvl.height, 1};
        copies.push_back(c);
        off += lvl.data.size();
      }
      vkCmdCopyBufferToImage(cmd, staging.buffer, p.dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             uint32_t(copies.size()), copies.data());
    }

    VkDependencyInfo post_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    post_dep.imageMemoryBarrierCount = uint32_t(post_barriers.size());
    post_dep.pImageMemoryBarriers = post_barriers.data();
    vkCmdPipelineBarrier2(cmd, &post_dep);
  });
  r_.destroy_buffer(staging);
  if (!ok) {
    cleanup_all();
    error = "batched texture upload";
    return false;
  }
  return true;
}

bool Showroom::upload_environment(std::string& error) {
  uint32_t kSize = 64, kLevels = 7;
  std::vector<std::vector<std::vector<std::array<float, 3>>>> faces;
  bool loaded_xpr = false;
  if (!guest_ && !env_xpr_path_.empty() && std::filesystem::exists(env_xpr_path_)) {
    if (FILE* f = std::fopen(env_xpr_path_.string().c_str(), "rb")) {
      std::fseek(f, 0, SEEK_END);
      const long sz = std::ftell(f);
      if (sz > 0) {
        std::vector<uint8_t> raw(static_cast<size_t>(sz));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(raw.data(), 1, raw.size(), f) == raw.size()) {
          fh1::XprCubemap xpr;
          std::string xerr;
          if (fh1::decode_xpr_cubemap(raw, xpr, &xerr) && xpr.size > 0 && (xpr.size & (xpr.size - 1)) == 0) {
            kSize = xpr.size;
            kLevels = 1;
            for (uint32_t s = kSize; s > 1; s >>= 1) ++kLevels;
            faces.resize(kLevels);
            faces[0].resize(6);
            for (uint32_t face = 0; face < 6; ++face) faces[0][face] = std::move(xpr.faces[face]);
            loaded_xpr = true;
          }
        }
      }
      std::fclose(f);
    }
  }
  if (!loaded_xpr) {
    faces.resize(kLevels);
    // Level 0 from the studio function, then 2x2 box filters per face.
    for (uint32_t face = 0; face < 6; ++face) {
      std::vector<std::array<float, 3>> texels(size_t(kSize) * kSize);
      for (uint32_t y = 0; y < kSize; ++y)
        for (uint32_t x = 0; x < kSize; ++x) {
          const float u = (float(x) + 0.5f) / kSize * 2 - 1, v = (float(y) + 0.5f) / kSize * 2 - 1;
          // Vulkan cube face orientation (+X, -X, +Y, -Y, +Z, -Z).
          float d[3];
          switch (face) {
            case 0: d[0] = 1; d[1] = -v; d[2] = -u; break;
            case 1: d[0] = -1; d[1] = -v; d[2] = u; break;
            case 2: d[0] = u; d[1] = 1; d[2] = v; break;
            case 3: d[0] = u; d[1] = -1; d[2] = -v; break;
            case 4: d[0] = u; d[1] = -v; d[2] = 1; break;
            default: d[0] = -u; d[1] = -v; d[2] = -1; break;
          }
          texels[size_t(y) * kSize + x] = studio(d[0], d[1], d[2]);
        }
      faces[0].push_back(std::move(texels));
    }
  }
  for (uint32_t level = 1; level < kLevels; ++level) {
    const uint32_t s = kSize >> level, p = s * 2;
    for (uint32_t face = 0; face < 6; ++face) {
      const auto& prev = faces[level - 1][face];
      std::vector<std::array<float, 3>> texels(size_t(s) * s);
      for (uint32_t y = 0; y < s; ++y)
        for (uint32_t x = 0; x < s; ++x)
          for (int c = 0; c < 3; ++c)
            texels[size_t(y) * s + x][size_t(c)] =
                0.25f * (prev[(2 * y) * p + 2 * x][size_t(c)] + prev[(2 * y) * p + 2 * x + 1][size_t(c)] +
                         prev[(2 * y + 1) * p + 2 * x][size_t(c)] +
                         prev[(2 * y + 1) * p + 2 * x + 1][size_t(c)]);
      faces[level].push_back(std::move(texels));
    }
  }
  std::vector<uint16_t> halfs;
  std::vector<VkBufferImageCopy> copies;
  for (uint32_t level = 0; level < kLevels; ++level)
    for (uint32_t face = 0; face < 6; ++face) {
      VkBufferImageCopy copy{};
      copy.bufferOffset = halfs.size() * 2;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1};
      copy.imageExtent = {kSize >> level, kSize >> level, 1};
      copies.push_back(copy);
      for (const auto& t : faces[level][face]) {
        halfs.push_back(to_half(t[0]));
        halfs.push_back(to_half(t[1]));
        halfs.push_back(to_half(t[2]));
        halfs.push_back(to_half(1));
      }
    }
  const VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {kSize, kSize, 1};
  info.mipLevels = kLevels;
  info.arrayLayers = 6;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  VmaAllocationInfo alloc_info{};
  VK_TRY(vmaCreateImage(r_.allocator_, &info, &alloc, &environment_.image, &environment_.allocation, &alloc_info),
         "vmaCreateImage (cube)");
  environment_.bytes = alloc_info.size;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = environment_.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  view.format = format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kLevels, 0, 6};
  if (vkCreateImageView(r_.device_, &view, nullptr, &environment_.view) != VK_SUCCESS) {
    r_.destroy_image(environment_);
    error = "vkCreateImageView (cube)";
    return false;
  }
  Renderer::Buffer staging;
  const VkDeviceSize bytes = halfs.size() * 2;
  if (!r_.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        staging)) {
    error = "environment staging buffer";
    return false;
  }
  void* mapped = nullptr;
  if (vmaMapMemory(r_.allocator_, staging.allocation, &mapped) != VK_SUCCESS) {
    r_.destroy_buffer(staging);
    error = "environment staging map";
    return false;
  }
  std::memcpy(mapped, halfs.data(), size_t(bytes));
  vmaUnmapMemory(r_.allocator_, staging.allocation);
  const bool ok = r_.submit_once([&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = environment_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kLevels, 0, 6};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkCmdCopyBufferToImage(cmd, staging.buffer, environment_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           uint32_t(copies.size()), copies.data());
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
  });
  r_.destroy_buffer(staging);
  if (!ok) error = "environment upload";
  return ok;
}

bool Showroom::create_pipelines(std::string& error) {
  // Set 0: frame UBO, dynamic/active environment cube, sun shadow map (PCF compare), headlight cookie,
  // headlights UBO, mirror map, car instances SSBO (6), static studio cube (7, staticcarcubemap.xpr),
  // shared dirt pattern (8), dirt mask (9), cracked glass (10), and raw D32 shadow depth (11, PCSS blocker search).
  const VkDescriptorSetLayoutBinding frame_bindings[12] = {
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
       nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set.bindingCount = 12;
  set.pBindings = frame_bindings;
  VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &set, nullptr, &frame_layout_), "frame set layout");
  VkDescriptorSetLayoutBinding material_bindings[5];
  for (uint32_t i = 0; i < 5; ++i)
    material_bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                            nullptr};
  set.bindingCount = 5;
  set.pBindings = material_bindings;
  VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &set, nullptr, &material_layout_),
         "material set layout");
  VkDescriptorSetLayoutBinding world_material_bindings[8];
  for (uint32_t i = 0; i < 8; ++i)
    world_material_bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                  nullptr};
  set.bindingCount = 8;
  set.pBindings = world_material_bindings;
  VK_TRY(vkCreateDescriptorSetLayout(r_.device_, &set, nullptr, &world_material_layout_),
         "world material set layout");
  const VkDescriptorSetLayout layouts[2] = {frame_layout_, material_layout_};
  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(ShowroomPush)};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 2;
  layout.pSetLayouts = layouts;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &push;
  VK_TRY(vkCreatePipelineLayout(r_.device_, &layout, nullptr, &pipeline_layout_), "pipeline layout");
  if (guest_) {
    // Guest cars are always rendered via the host Showroom's pipelines (instanced
    // record_all_instances / record_instance_offscreen); they only need the shared
    // material descriptor set layout and compatible pipeline layout.
    return true;
  }
  const VkDescriptorSetLayout world_layouts[2] = {frame_layout_, world_material_layout_};
  layout.pSetLayouts = world_layouts;
  VK_TRY(vkCreatePipelineLayout(r_.device_, &layout, nullptr, &world_pipeline_layout_), "world pipeline layout");

  // A mod's override (shader_mods.h) replaces a built-in shader of the same name.
  std::vector<std::vector<uint32_t>> override_code;
  auto make_module = [&](const uint32_t* code, size_t bytes, VkShaderModule& module, const char* name = nullptr) {
    if (name)
      if (auto o = cars::shader_mods::registry().find(name)) {
        override_code.push_back(std::move(*o));
        code = override_code.back().data();
        bytes = override_code.back().size() * sizeof(uint32_t);
        notes_.push_back(std::string("shader override: ") + name);
      }
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    return vkCreateShaderModule(r_.device_, &info, nullptr, &module);
  };
  VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE, frag_opaque = VK_NULL_HANDLE, mask = VK_NULL_HANDLE;
  VK_TRY(make_module(kShowroom_vert_spv, sizeof(kShowroom_vert_spv), vert, "showroom.vert"), "showroom.vert");
  VK_TRY(make_module(kShowroom_frag_spv, sizeof(kShowroom_frag_spv), frag, "showroom.frag"), "showroom.frag");
  VK_TRY(make_module(kShowroom_opaque_frag_spv, sizeof(kShowroom_opaque_frag_spv), frag_opaque, "showroom_opaque.frag"),
         "showroom_opaque.frag");
  VK_TRY(make_module(kMask_frag_spv, sizeof(kMask_frag_spv), mask, "mask.frag"), "mask.frag");

  static_assert(sizeof(bake::Vertex) == 32);
  VkVertexInputBindingDescription binding{0, sizeof(bake::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
  const VkVertexInputAttributeDescription attributes[5] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(bake::Vertex, position)},
      {1, 0, VK_FORMAT_R16G16B16A16_SNORM, offsetof(bake::Vertex, frame)},
      {2, 0, VK_FORMAT_R16G16_UNORM, offsetof(bake::Vertex, uv0)},
      {3, 0, VK_FORMAT_R16G16_UNORM, offsetof(bake::Vertex, uv1)},
      {4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(bake::Vertex, lighting)}};
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &binding;
  vertex_input.vertexAttributeDescriptionCount = 5;
  vertex_input.pVertexAttributeDescriptions = attributes;
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  // Both sides: the shader faces the normal towards the eye.
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  const VkDynamicState blend_const_states[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                                VK_DYNAMIC_STATE_BLEND_CONSTANTS};
  VkPipelineDynamicStateCreateInfo dynamic_blend_const{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic_blend_const.dynamicStateCount = 3;
  dynamic_blend_const.pDynamicStates = blend_const_states;

  // Under the drive's post chain the scene shaders write linear HDR
  // (constant 0 of showroom.frag and world.frag).
  // Constant 1 (world.vert): reversed depth, for the sky's depth.
  // Constant 2 (showroom_frag.glsl / world_frag.glsl): mobile fast shading path on Android low-mem presets.
#ifdef __ANDROID__
  const VkBool32 mobile_fast = low_memory_mode_ ? VK_TRUE : VK_FALSE;
#else
  const VkBool32 mobile_fast = VK_FALSE;
#endif
  const VkBool32 spec_values[3] = {r_.post() ? VK_TRUE : VK_FALSE, r_.reverse_z() ? VK_TRUE : VK_FALSE, mobile_fast};
  const VkSpecializationMapEntry spec_entries[3] = {{0, 0, sizeof(VkBool32)},
                                                    {1, sizeof(VkBool32), sizeof(VkBool32)},
                                                    {2, 2 * sizeof(VkBool32), sizeof(VkBool32)}};
  const VkSpecializationInfo spec{3, spec_entries, sizeof spec_values, spec_values};
  // The world's pipelines take a depth bias per draw (decals and road
  // paint over the surface they lie on; the rest 0).
  VkPipelineRasterizationStateCreateInfo raster_bias = raster;
  raster_bias.depthBiasEnable = VK_TRUE;
  const VkDynamicState bias_states[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                         VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo dynamic_bias{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic_bias.dynamicStateCount = 3;
  dynamic_bias.pDynamicStates = bias_states;
  const VkDynamicState vrs_states[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                        VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR};
  VkPipelineDynamicStateCreateInfo dynamic_vrs{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic_vrs.dynamicStateCount = 3;
  dynamic_vrs.pDynamicStates = vrs_states;
  const VkDynamicState vrs_bias_states[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                             VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR};
  VkPipelineDynamicStateCreateInfo dynamic_bias_vrs{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic_bias_vrs.dynamicStateCount = 4;
  dynamic_bias_vrs.pDynamicStates = vrs_bias_states;
  std::vector<std::function<VkResult(VkPipelineCache)>> tasks;
  tasks.reserve(24);
  auto queue_build = [&](VkShaderModule fragment, VkFormat color_format, VkSampleCountFlagBits samples,
                         bool depth_attachment, bool depth_write, int blend_mode, VkPipeline& out,
                         VkShaderModule vertex = VK_NULL_HANDLE,
                         const VkPipelineVertexInputStateCreateInfo* input = nullptr, bool coverage = false,
                         VkPipelineLayout pipe_layout = VK_NULL_HANDLE,
                         VkCullModeFlags cull_mode = VK_CULL_MODE_NONE, bool depth_equal = false,
                         bool color_out = true) {
    VkPipeline* dst = &out;
    tasks.emplace_back([&, fragment, color_format, samples, depth_attachment, depth_write, blend_mode, dst, vertex,
                        input, coverage, pipe_layout, cull_mode, depth_equal,
                        color_out](VkPipelineCache cache) -> VkResult {
      const VkPipelineShaderStageCreateInfo stages[2] = {
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
           vertex ? vertex : vert, "main", vertex ? &spec : nullptr},
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
           fragment, "main", fragment == mask ? nullptr : &spec}};
      VkPipelineRasterizationStateCreateInfo rstate = vertex ? raster_bias : raster;
      rstate.cullMode = cull_mode;
      if (vertex) rstate.frontFace = VK_FRONT_FACE_CLOCKWISE;  // world_transform_ flips det = -1
      VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      multisample.rasterizationSamples = samples;
      multisample.alphaToCoverageEnable = coverage && samples != VK_SAMPLE_COUNT_1_BIT;
      VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      depth.depthTestEnable = depth_attachment;
      depth.depthWriteEnable = depth_write;
      depth.depthCompareOp =
          depth_equal ? VK_COMPARE_OP_EQUAL
                      : (r_.reverse_z() ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL);
      VkPipelineColorBlendAttachmentState attachment{};
      attachment.colorWriteMask = (coverage && r_.post())
                                      ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT)
                                      : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
      if (blend_mode == 1 || blend_mode == 4) {
        // Premultiplied colour over what is behind; blend_mode == 4 (water) also writes surface gloss alpha for SSR.
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = (blend_mode == 4) ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        attachment.dstAlphaBlendFactor =
            (blend_mode == 4) ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
      } else if (blend_mode == 2) {
        // Multiply blend (dst = src * dst): FH1 detail_glass_* Pass 1 & under-car contact shadow.
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
      } else if (blend_mode == 3) {
        // Additive blend (dst = src + dst): FH1 detail_glass_* Pass 2 reflection + sun glint.
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
      } else if (blend_mode == 5) {
        // Constant-alpha fade blend (smooth traffic spawn/despawn fade-in/out without shader edits).
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
      }
      VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      blend.attachmentCount = color_out ? 1u : 0u;
      blend.pAttachments = color_out ? &attachment : nullptr;
      const VkFormat cf = color_format;
      VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
      rendering.colorAttachmentCount = color_out ? 1u : 0u;
      rendering.pColorAttachmentFormats = color_out ? &cf : nullptr;
      rendering.depthAttachmentFormat = depth_attachment ? r_.depth_format_ : VK_FORMAT_UNDEFINED;
      VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      info.pNext = &rendering;
      info.stageCount = fragment ? 2u : 1u;
      info.pStages = stages;
      info.pVertexInputState = input ? input : &vertex_input;
      info.pInputAssemblyState = &assembly;
      info.pViewportState = &viewport;
      info.pRasterizationState = &rstate;
      info.pMultisampleState = &multisample;
      info.pDepthStencilState = &depth;
      info.pColorBlendState = &blend;
      const bool use_vrs = r_.vrs_supported() && color_out &&
                           ((blend_mode == 0) || (blend_mode == 1) || (blend_mode == 2) || (blend_mode == 3) ||
                            (dst == &world_alpha_)) && samples == r_.samples_ &&
                           (dst != &env_world_opaque_) && (dst != &env_world_coverage_) && (dst != &env_car_opaque_);
      info.pDynamicState = (blend_mode == 5) ? &dynamic_blend_const
                                             : (vertex ? (use_vrs ? &dynamic_bias_vrs : &dynamic_bias)
                                                       : (use_vrs ? &dynamic_vrs : &dynamic));
      info.layout = pipe_layout ? pipe_layout : pipeline_layout_;
      return vkCreateGraphicsPipelines(r_.device_, cache, 1, &info, nullptr, dst);
    });
  };

  const VkCullModeFlags cull_back = r_.backface_cull() ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
  // Discard-free Early-Z car pipelines for solid opaque panels; full showroom.frag for grilles/cutouts/spinning spokes.
  queue_build(frag_opaque, r_.scene_format(), r_.samples_, true, true, 0, opaque_, VK_NULL_HANDLE, nullptr, false,
              VK_NULL_HANDLE, cull_back);
  queue_build(frag_opaque, r_.scene_format(), r_.samples_, true, false, 0, opaque_after_z_, VK_NULL_HANDLE, nullptr, false,
              VK_NULL_HANDLE, cull_back, true);
  queue_build(frag, r_.scene_format(), r_.samples_, true, true, 0, opaque_cutout_, VK_NULL_HANDLE, nullptr, false,
              VK_NULL_HANDLE, cull_back);
  queue_build(frag, r_.scene_format(), r_.samples_, true, false, 0, opaque_cutout_after_z_, VK_NULL_HANDLE, nullptr, false,
              VK_NULL_HANDLE, cull_back, true);
  queue_build(frag, r_.scene_format(), r_.samples_, true, false, 5, opaque_fade_, VK_NULL_HANDLE, nullptr, false,
              VK_NULL_HANDLE, cull_back);
  queue_build(VK_NULL_HANDLE, r_.scene_format(), r_.samples_, true, true, 0, car_z_prepass_, VK_NULL_HANDLE, nullptr,
              false, VK_NULL_HANDLE, cull_back, false, false);
  queue_build(frag, r_.scene_format(), r_.samples_, true, false, 1, glass_);
  queue_build(frag, r_.scene_format(), r_.samples_, true, false, 2, glass_mul_);
  queue_build(frag, r_.scene_format(), r_.samples_, true, false, 3, glass_add_);
  if (!guest_)
    queue_build(frag_opaque, r_.bloom_format(), VK_SAMPLE_COUNT_1_BIT, true, true, 0, env_car_opaque_);
  queue_build(mask, VK_FORMAT_R8_UNORM, r_.samples_, false, false, 0, mask_);

  // Under-car contact / drop shadow pipeline (drive mode multiply-blended quad).
  VkResult result = VK_SUCCESS;
  VkShaderModule cs_v = VK_NULL_HANDLE, cs_f = VK_NULL_HANDLE;
  if (!guest_) {
    VkPushConstantRange cs_push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96};
    VkPipelineLayoutCreateInfo cs_pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    cs_pl.setLayoutCount = 1;
    cs_pl.pSetLayouts = &frame_layout_;
    cs_pl.pushConstantRangeCount = 1;
    cs_pl.pPushConstantRanges = &cs_push;
    result = vkCreatePipelineLayout(r_.device_, &cs_pl, nullptr, &contact_shadow_pipe_layout_);
    if (result == VK_SUCCESS)
      result = make_module(kContact_shadow_vert_spv, sizeof(kContact_shadow_vert_spv), cs_v, "contact_shadow.vert");
    if (result == VK_SUCCESS)
      result = make_module(kContact_shadow_frag_spv, sizeof(kContact_shadow_frag_spv), cs_f, "contact_shadow.frag");
    if (result == VK_SUCCESS) {
      tasks.emplace_back([&, cs_v, cs_f](VkPipelineCache cache) -> VkResult {
        VkPipelineVertexInputStateCreateInfo empty_vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        const VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, cs_v, "main",
             nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, cs_f,
             "main", nullptr}};
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = r_.samples_;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_FALSE;
        ds.depthCompareOp = r_.reverse_z() ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                             VK_COLOR_COMPONENT_A_BIT;
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        att.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        att.colorBlendOp = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo bl{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        bl.attachmentCount = 1;
        bl.pAttachments = &att;
        const VkFormat sc_fmt = r_.scene_format();
        VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rend.colorAttachmentCount = 1;
        rend.pColorAttachmentFormats = &sc_fmt;
        rend.depthAttachmentFormat = r_.depth_format_;
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.pNext = &rend;
        pci.stageCount = 2;
        pci.pStages = stages;
        pci.pVertexInputState = &empty_vi;
        pci.pInputAssemblyState = &assembly;
        pci.pViewportState = &viewport;
        pci.pRasterizationState = &raster_bias;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &bl;
        pci.pDynamicState = &dynamic_bias;
        pci.layout = contact_shadow_pipe_layout_;
        return vkCreateGraphicsPipelines(r_.device_, cache, 1, &pci, nullptr, &contact_shadow_pipe_);
      });
    }
  }
  // The textured world (world.vert / world.frag + discard-free world_opaque.frag):
  // opaque (Early-Z), dithered opaque, alpha to coverage, and blended with depth writes off.
  VkShaderModule world_vert = VK_NULL_HANDLE, world_frag = VK_NULL_HANDLE, world_opaque_frag = VK_NULL_HANDLE;
  if (result == VK_SUCCESS) result = make_module(kWorld_vert_spv, sizeof(kWorld_vert_spv), world_vert, "world.vert");
  if (result == VK_SUCCESS) result = make_module(kWorld_frag_spv, sizeof(kWorld_frag_spv), world_frag, "world.frag");
  if (result == VK_SUCCESS)
    result = make_module(kWorld_opaque_frag_spv, sizeof(kWorld_opaque_frag_spv), world_opaque_frag, "world_opaque.frag");
  VkVertexInputBindingDescription world_binding{0, sizeof(WorldVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  const VkVertexInputAttributeDescription world_attributes[7] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(WorldVertex, position)},
      {1, 0, VK_FORMAT_R8G8B8A8_SNORM, offsetof(WorldVertex, normal)},
      {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WorldVertex, uv0)},
      {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WorldVertex, uv1)},
      {4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(WorldVertex, colour)},
      {5, 0, VK_FORMAT_R16G16B16A16_SINT, offsetof(WorldVertex, centre)},
      {6, 0, VK_FORMAT_R16G16_SFLOAT, offsetof(WorldVertex, uv2)}};
  VkPipelineVertexInputStateCreateInfo world_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  world_input.vertexBindingDescriptionCount = 1;
  world_input.pVertexBindingDescriptions = &world_binding;
  world_input.vertexAttributeDescriptionCount = 7;
  world_input.pVertexAttributeDescriptions = world_attributes;
  if (result == VK_SUCCESS) {
    queue_build(world_opaque_frag, r_.scene_format(), r_.samples_, true, true, 0, world_opaque_, world_vert,
                &world_input, false, world_pipeline_layout_, cull_back);
    queue_build(world_opaque_frag, r_.scene_format(), r_.samples_, true, false, 0, world_opaque_after_z_, world_vert,
                &world_input, false, world_pipeline_layout_, cull_back, true);
    queue_build(world_frag, r_.scene_format(), r_.samples_, true, true, 0, world_opaque_dither_, world_vert,
                &world_input, false, world_pipeline_layout_, cull_back);
    queue_build(VK_NULL_HANDLE, r_.scene_format(), r_.samples_, true, true, 0, world_z_prepass_, world_vert,
                &world_input, false, world_pipeline_layout_, cull_back, false, false);
    queue_build(world_frag, r_.scene_format(), r_.samples_, true, true, 0, world_coverage_, world_vert, &world_input,
                true, world_pipeline_layout_);
    queue_build(world_frag, r_.scene_format(), r_.samples_, true, false, 1, world_alpha_, world_vert, &world_input,
                false, world_pipeline_layout_);
    // Lake water writes surface depth (while blending over already-rendered lake bed) so
    // depth_resolved_ / Hi-Z / SSR trace from the water surface instead of the underwater bed.
    queue_build(world_frag, r_.scene_format(), r_.samples_, true, true, 4, world_water_, world_vert, &world_input,
                false, world_pipeline_layout_);
    if (!guest_) {
      const VkFormat env_fmt = r_.bloom_format();
      queue_build(world_opaque_frag, env_fmt, VK_SAMPLE_COUNT_1_BIT, true, true, 0,
                  env_world_opaque_, world_vert, &world_input, false, world_pipeline_layout_, cull_back);
      queue_build(world_frag, env_fmt, VK_SAMPLE_COUNT_1_BIT, true, true, 0,
                  env_world_coverage_, world_vert, &world_input, true, world_pipeline_layout_);
    }
  }
  // The sun shadow pass (ours): depth only into the shadow map, biased;
  // opaque casters with no fragment shader, cut-outs alpha-tested.
  VkShaderModule shadow_frag = VK_NULL_HANDLE;
  auto queue_shadow = [&](VkShaderModule vertex, VkShaderModule fragment,
                          const VkPipelineVertexInputStateCreateInfo* input, VkPipelineLayout pipe_layout,
                          VkCullModeFlags cull_mode, VkPipeline& out) {
    VkPipeline* dst = &out;
    tasks.emplace_back([&, vertex, fragment, input, pipe_layout, cull_mode, dst](VkPipelineCache cache) -> VkResult {
      const VkPipelineShaderStageCreateInfo stages[2] = {
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main",
           nullptr},
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment,
           "main", nullptr}};
      VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      depth.depthTestEnable = VK_TRUE;
      depth.depthWriteEnable = VK_TRUE;
      depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
      VkPipelineRasterizationStateCreateInfo biased = raster;
      biased.cullMode = cull_mode;
      if (vertex == world_vert) biased.frontFace = VK_FRONT_FACE_CLOCKWISE;
      biased.depthBiasEnable = VK_TRUE;
      biased.depthBiasConstantFactor = 2.0f;
      biased.depthBiasSlopeFactor = 2.5f;
      VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
      rendering.depthAttachmentFormat = Renderer::kShadowFormat;
      VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      info.pNext = &rendering;
      info.stageCount = fragment ? 2 : 1;
      info.pStages = stages;
      info.pVertexInputState = input;
      info.pInputAssemblyState = &assembly;
      info.pViewportState = &viewport;
      info.pRasterizationState = &biased;
      info.pMultisampleState = &multisample;
      info.pDepthStencilState = &depth;
      info.pColorBlendState = &blend;
      info.pDynamicState = &dynamic;
      info.layout = pipe_layout;
      return vkCreateGraphicsPipelines(r_.device_, cache, 1, &info, nullptr, dst);
    });
  };
  if (result == VK_SUCCESS && r_.options_.shadow.size > 0) {
    result = make_module(kShadow_frag_spv, sizeof(kShadow_frag_spv), shadow_frag, "shadow.frag");
    if (result == VK_SUCCESS) {
      queue_shadow(world_vert, VK_NULL_HANDLE, &world_input, world_pipeline_layout_, cull_back, shadow_world_);
      queue_shadow(world_vert, shadow_frag, &world_input, world_pipeline_layout_, VK_CULL_MODE_NONE, shadow_cutout_);
      queue_shadow(vert, VK_NULL_HANDLE, &vertex_input, pipeline_layout_, cull_back, shadow_car_);
    }
  }
  bool pipes_ok = true;
  if (result == VK_SUCCESS) {
    pipes_ok = r_.compile_pipelines_parallel(tasks, error);
  }
  if (cs_v) vkDestroyShaderModule(r_.device_, cs_v, nullptr);
  if (cs_f) vkDestroyShaderModule(r_.device_, cs_f, nullptr);
  if (shadow_frag) vkDestroyShaderModule(r_.device_, shadow_frag, nullptr);
  if (world_vert) vkDestroyShaderModule(r_.device_, world_vert, nullptr);
  if (world_frag) vkDestroyShaderModule(r_.device_, world_frag, nullptr);
  if (world_opaque_frag) vkDestroyShaderModule(r_.device_, world_opaque_frag, nullptr);
  vkDestroyShaderModule(r_.device_, vert, nullptr);
  vkDestroyShaderModule(r_.device_, frag, nullptr);
  if (frag_opaque) vkDestroyShaderModule(r_.device_, frag_opaque, nullptr);
  vkDestroyShaderModule(r_.device_, mask, nullptr);
  VK_TRY(result, "showroom pipelines");
  return pipes_ok;
}

int run_precompile_shaders(int argc, char** argv) {
  std::filesystem::path play_dir;
  std::filesystem::path cache_path;
  uint32_t msaa = 4;
  bool post_on = true;
  bool low_preset = false;
  bool validation = std::getenv("SHIFT_VALIDATION") != nullptr;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--play") play_dir = next();
    else if (a == "--cache" || a == "--pipeline-cache") cache_path = next();
    else if (a == "--msaa") msaa = uint32_t(std::max(1, std::atoi(next())));
    else if (a == "--no-post") post_on = false;
    else if (a == "--validation") validation = true;
    else if (a == "--preset") {
      const std::string p = next();
      if (p == "low" || p == "low-power") {
        low_preset = true;
        msaa = 1;
      }
    }
  }

  Renderer::Options ropts{};
  ropts.window = nullptr;
  ropts.validation = validation;
  ropts.msaa = msaa;
  ropts.reverse_z = true;
  ropts.backface_cull = true;
  ropts.z_prepass = true;
  ropts.post.enabled = post_on;
  ropts.post.bloom = post_on && !low_preset;
  ropts.shadow.size = low_preset ? 1024u : 2048u;
  ropts.shadow.radius = low_preset ? 40.0f : 60.0f;
  ropts.shadow.taps = low_preset ? 1u : 4u;
  ropts.shadow.cascades = low_preset ? 2u : 3u;
  if (const char* pc_env = std::getenv("SHIFT_PIPELINE_CACHE"); pc_env && *pc_env) {
    ropts.pipeline_cache_path = pc_env;
  } else if (!cache_path.empty()) {
    ropts.pipeline_cache_path = cache_path;
  } else if (!play_dir.empty()) {
    ropts.pipeline_cache_path = play_dir / "cache" / "vk_pipeline_cache.bin";
  } else {
    ropts.pipeline_cache_path = std::filesystem::path("out") / "cache" / "vk_pipeline_cache.bin";
  }

  const auto t0 = std::chrono::steady_clock::now();
  Renderer renderer;
  std::string err;
  if (!renderer.init(ropts, err)) {
    std::fprintf(stderr, "carview --precompile-shaders: renderer init failed: %s\n", err.c_str());
    return 1;
  }
  const bool had_cache = renderer.pipeline_cache_loaded();
  int compiled_groups = 1;  // Renderer base + post + modern + UI

  {
    Showroom showroom(renderer);
    showroom.set_low_memory_mode(low_preset);
    showroom.init_minimal_for_precompile();
    if (!showroom.upload(err)) {
      std::fprintf(stderr, "carview --precompile-shaders: showroom upload failed: %s\n", err.c_str());
      return 1;
    }
    ++compiled_groups;  // Showroom car/world/shadow/contact_shadow/env_prefilter + WorldGpu

    // Sky + Stars (provide 1 dummy star so stars_pipe_ is also compiled).
    world::RealTimeSkyAssets dummy_sky{};
    dummy_sky.stars.emplace_back();
    std::string sky_err;
    if (showroom.upload_sky(dummy_sky, sky_err)) ++compiled_groups;
    else std::fprintf(stderr, "carview --precompile-shaders: sky note: %s\n", sky_err.c_str());

    // Car lights glow pipeline.
    std::string cl_err;
    if (showroom.upload_carlights({}, cl_err)) ++compiled_groups;
    else std::fprintf(stderr, "carview --precompile-shaders: carlights note: %s\n", cl_err.c_str());

    // VFX particles (alpha + additive) + skidmarks pipelines.
    std::string vfx_err;
    if (showroom.init_vfx(nullptr, vfx_err)) ++compiled_groups;
    else std::fprintf(stderr, "carview --precompile-shaders: vfx note: %s\n", vfx_err.c_str());

    // Hardware Ray Tracing / Path Tracing compute pipelines (when supported by device).
    if (renderer.rt_supported()) {
      std::string rt_err;
      if (showroom.precompile_rt_pipelines(rt_err)) ++compiled_groups;
      else std::fprintf(stderr, "carview --precompile-shaders: RT note: %s\n", rt_err.c_str());
    }
  }

  renderer.save_pipeline_cache();
  const double total_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("[aot] precompiled %d pipeline groups (msaa=%u, rt=%s, cache=%s) in %.1f ms -> %s (%.1f KiB)\n",
              compiled_groups, renderer.sample_count(), renderer.rt_supported() ? "yes" : "no",
              had_cache ? "warm" : "cold", total_ms, ropts.pipeline_cache_path.string().c_str(),
              double(renderer.pipeline_cache_bytes()) / 1024.0);
  return 0;
}


}  // namespace carview
