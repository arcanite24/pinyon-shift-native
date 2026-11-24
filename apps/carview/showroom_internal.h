// Private helpers and shader flag constants shared across Showroom units.
#pragma once

#include "showroom.h"
#include "cars/sources.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#include <vk_mem_alloc.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace carview {

namespace bake = cars::bake;
using bake::MaterialClass;
using bake::Role;

#define VK_TRY(expr, what)                                                       \
  do {                                                                           \
    const VkResult vk_try_result = (expr);                                       \
    if (vk_try_result != VK_SUCCESS) {                                           \
      error = std::string(what) + " failed (VkResult " +                         \
              std::to_string(int(vk_try_result)) + ")";                          \
      return false;                                                              \
    }                                                                            \
  } while (0)

// Flags in ShowroomPush::params.y (showroom.vert / showroom.frag).
inline constexpr uint32_t kFlagSlot1 = 1, kFlagMetallic = 2, kFlagLivery = 8, kFlagStripe = 32, kFlagFlat = 64,
                          kFlagNoSlot0 = 16, kFlagRaceLivery = 128, kFlagGrille = 256, kFlagCutout = 512,
                          kFlagCover = 1024, kFlagLampEmissive = 2048, kFlagMirror = 4096,
                          kFlagInstanced = 8192,  // bits 14..18 carry instanced lamp channel
                          kFlagNormalMap = 524288, kFlagNormalUv1 = 1048576, kFlagDamageMap = 2097152;

inline std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

inline bool ends_with(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

inline uint16_t float_to_half(float f) {
  uint32_t u = 0;
  std::memcpy(&u, &f, 4);
  const uint32_t sign = (u >> 16) & 0x8000u;
  int32_t exp = int32_t((u >> 23) & 0xFFu) - 127 + 15;
  uint32_t mant = u & 0x7FFFFFu;
  if (exp <= 0) {
    if (exp < -10) return uint16_t(sign);
    mant = (mant | 0x800000u) >> (1 - exp);
    return uint16_t(sign | ((mant + 0x1000u) >> 13));
  }
  if (exp >= 31) return uint16_t(sign | 0x7BFFu);
  return uint16_t(sign | (uint32_t(exp) << 10) | ((mant + 0x1000u) >> 13));
}

inline float pack_half2x16_float(float a, float b) {
  const uint32_t packed = uint32_t(float_to_half(a)) | (uint32_t(float_to_half(b)) << 16);
  float out = 0.0f;
  std::memcpy(&out, &packed, 4);
  return out;
}

inline bool parse_env_u64(const char* name, uint64_t& out) {
  const char* val = std::getenv(name);
  if (!val || !*val) return false;
  while (*val && std::isspace(static_cast<unsigned char>(*val))) ++val;
  if (*val < '0' || *val > '9') return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(val, &end, 10);
  if (errno == ERANGE || end == val) return false;
  while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
  if (*end != '\0') return false;
  out = uint64_t(parsed);
  return true;
}

inline void identity(float* m) {
  std::memset(m, 0, 16 * sizeof(float));
  m[0] = m[5] = m[10] = m[15] = 1;
}

inline void translate_scale(const std::array<float, 3>& t, const std::array<float, 3>& s, float* m) {
  identity(m);
  m[0] = s[0];
  m[5] = s[1];
  m[10] = s[2];
  m[12] = t[0];
  m[13] = t[1];
  m[14] = t[2];
}

inline void multiply(const float* a, const float* b, float* out) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = sum;
    }
}

inline std::array<float, 3> transform_point(const float* m, const std::array<float, 3>& p) {
  return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
          m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
          m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

inline float srgb_to_linear(float c) {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline uint32_t block_bytes(bake::TextureFormat f) {
  switch (f) {
    case bake::TextureFormat::kBc1:
    case bake::TextureFormat::kBc4: return 8;
    case bake::TextureFormat::kBc2:
    case bake::TextureFormat::kBc3:
    case bake::TextureFormat::kBc5: return 16;
    default: return 0;
  }
}

inline size_t level_bytes(bake::TextureFormat f, uint32_t w, uint32_t h) {
  if (f == bake::TextureFormat::kRgba8) return size_t(w) * h * 4;
  if (f == bake::TextureFormat::kR8) return size_t(w) * h;
  return size_t((w + 3) / 4) * ((h + 3) / 4) * block_bytes(f);
}

inline VkFormat vk_format(bake::TextureFormat f, bool srgb) {
  switch (f) {
    case bake::TextureFormat::kRgba8: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    case bake::TextureFormat::kBc1: return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case bake::TextureFormat::kBc2: return srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
    case bake::TextureFormat::kBc3: return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    case bake::TextureFormat::kBc4: return VK_FORMAT_BC4_UNORM_BLOCK;
    case bake::TextureFormat::kBc5: return VK_FORMAT_BC5_UNORM_BLOCK;
    case bake::TextureFormat::kR8: return VK_FORMAT_R8_UNORM;
  }
  return VK_FORMAT_UNDEFINED;
}

inline VkComponentSwizzle vk_swizzle(uint8_t s) {
  switch (s) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    default: return VK_COMPONENT_SWIZZLE_ONE;
  }
}

inline uint16_t to_half(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000u;
  int32_t exponent = int32_t((x >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = x & 0x7FFFFFu;
  if (exponent <= 0) return uint16_t(sign);
  if (exponent >= 31) return uint16_t(sign | 0x7BFFu);
  return uint16_t(sign | (uint32_t(exponent) << 10) | (mantissa >> 13));
}

inline uint32_t texture_chain(const bake::Texture& texture, VkDeviceSize& total) {
  total = 0;
  if (texture.levels.empty()) return 0;
  const uint32_t w0 = texture.levels[0].width, h0 = texture.levels[0].height;
  uint32_t levels = 0;
  for (const auto& level : texture.levels) {
    const uint32_t w = std::max(1u, w0 >> levels), h = std::max(1u, h0 >> levels);
    if (level.width != w || level.height != h || level.data.size() != level_bytes(texture.format, w, h)) break;
    total += level.data.size();
    ++levels;
  }
  return levels;
}

inline void record_texture_copy(VkCommandBuffer cmd, VkBuffer staging, VkDeviceSize offset, const bake::Texture& texture,
                                uint32_t levels, VkImage image, uint32_t skip = 0) {
  if (skip >= levels) skip = levels ? levels - 1 : 0;
  const uint32_t kept = levels - skip;
  std::vector<VkBufferImageCopy> copies;
  for (uint32_t i = skip; i < levels; ++i) {
    const auto& level = texture.levels[i];
    VkBufferImageCopy copy{};
    copy.bufferOffset = offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - skip, 0, 1};
    copy.imageExtent = {level.width, level.height, 1};
    copies.push_back(copy);
    offset += level.data.size();
  }
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
  b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kept, 0, 1};
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &dep);
  vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                         copies.data());
  b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier2(cmd, &dep);
}

// Part/LOD helpers shared between showroom_car_load.cpp and showroom_car_state.cpp.
std::string tyre_texture_member(const std::string& sheet, const std::string& tyre_media);
bool hidden_variant(const std::string& name, const std::set<std::string>& real);
std::string family_member(const std::string& name);
bool stock_hidden(const std::string& member, const cars::bake::StockRow& stock);
int32_t best_lod(const bake::Part& part);
bool placeholder(const bake::Part& part);

// Opens a car's archive/folder from base disc, Title Update v4, or DLC packages.
bool open_disc_car_folder(const std::filesystem::path& game_root, const std::string& car_name,
                          cars::Folder& out);

}  // namespace carview
