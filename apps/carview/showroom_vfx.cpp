#include "showroom_internal.h"

#include "particle_frag_spv.h"
#include "particle_vert_spv.h"
#include "skid_frag_spv.h"
#include "skid_vert_spv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_set>

#include "fh1data/archive.h"
#include "fh1data/xds.h"

namespace carview {

namespace {

// Expanded 10x10 mipmapped particle atlas (2560x2560 = 100 cells of 256x256, 9 mip levels
// stopping cleanly at 1x1 per cell so all 72 retail effects.zip .xds textures fit with zero
// truncation and zero cross-cell mip bleed).
//   slot  0    : procedural soft puff fallback
//   slots 1..31: <Texture2> / *_grad*.xds secondary & gradient lookup textures (5-bit shader index)
//   slots 32..99: primary particle diffuse / flipbook textures
constexpr uint32_t kExpandedAtlasGrid = 10;
constexpr uint32_t kExpandedAtlasCell = 256;
constexpr uint32_t kExpandedAtlasSize = kExpandedAtlasGrid * kExpandedAtlasCell;  // 2560
constexpr uint32_t kExpandedAtlasMips = 9;                                      // 2560 -> 10 (1x1/cell)
constexpr uint16_t kExpandedAtlasMaxSlots = uint16_t(kExpandedAtlasGrid * kExpandedAtlasGrid);
constexpr uint16_t kMaxTex2Slot = 31;

bool load_retail_treadmark(const std::filesystem::path& game_root, fh1::RgbaImage& out) {
  if (game_root.empty()) return false;
  std::error_code ec;
  const std::filesystem::path loose_cands[] = {
      game_root / "media" / "tracks" / "treadmark.xds",
      game_root / "media" / "tracks" / "Treadmark.xds",
      game_root / "Media" / "Tracks" / "treadmark.xds",
  };
  for (const auto& p : loose_cands) {
    if (!std::filesystem::exists(p, ec)) continue;
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) continue;
    const auto sz = f.tellg();
    if (sz <= 0 || sz > 16 * 1024 * 1024) continue;
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), sz)) continue;
    const auto dec = fh1::decode_xds(bytes, false);
    if ((dec.status == fh1::XdsStatus::kOk || dec.status == fh1::XdsStatus::kBaseOnly) && !dec.levels.empty() &&
        dec.levels[0].width >= 32 && dec.levels[0].height >= 32) {
      out = dec.levels[0];
      return true;
    }
  }
  // Fallback search inside media/cars/Shared.zip if present there.
  const auto shared_zip = game_root / "media" / "cars" / "Shared.zip";
  if (std::filesystem::exists(shared_zip, ec)) {
    if (auto ar = fh1::Archive::open(shared_zip, nullptr)) {
      for (const auto& m : ar->members()) {
        if (lower(m.name).find("treadmark.xds") != std::string::npos) {
          const auto rd = ar->read(m);
          if (rd.ok() && !rd.data.empty()) {
            const auto dec = fh1::decode_xds(rd.data, false);
            if ((dec.status == fh1::XdsStatus::kOk || dec.status == fh1::XdsStatus::kBaseOnly) &&
                !dec.levels.empty() && dec.levels[0].width >= 32 && dec.levels[0].height >= 32) {
              out = dec.levels[0];
              return true;
            }
          }
        }
      }
    }
  }
  return false;
}

std::vector<uint8_t> crop_rgba_band(const fh1::RgbaImage& src, uint32_t x0, uint32_t w) {
  std::vector<uint8_t> out(size_t(w) * src.height * 4, 0);
  for (uint32_t y = 0; y < src.height; ++y) {
    const uint8_t* s = src.rgba.data() + (size_t(y) * src.width + x0) * 4;
    uint8_t* d = out.data() + size_t(y) * w * 4;
    std::memcpy(d, s, size_t(w) * 4);
  }
  return out;
}

uint32_t full_mip_count(uint32_t w, uint32_t h) {
  uint32_t m = 1;
  uint32_t d = std::min(w, h);
  while (d > 1u) {
    d /= 2u;
    ++m;
  }
  return m;
}

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
      // Fade RGB to zero with alpha so corners are strictly black & transparent even under additive blending.
      const uint8_t shade = uint8_t(std::clamp(soft * 255.0f, 0.0f, 255.0f));
      uint8_t* o = dst + (size_t(y) * atlas_w + size_t(x)) * 4;
      o[0] = shade;
      o[1] = shade;
      o[2] = shade;
      o[3] = uint8_t(std::clamp(soft * 245.0f, 0.0f, 255.0f));
    }
  }
}

// Ensures primary particle diffuse textures (DXT1 / RGB-only / missing alpha masks) always carry
// a clean soft alpha mask so world smoke/flare emitters never render as hard-edged opaque squares.
void sanitize_primary_particle_alpha(fh1::RgbaImage& img) {
  if (img.width == 0 || img.height == 0 || img.rgba.size() < size_t(img.width) * img.height * 4) return;
  const uint32_t w = img.width;
  const uint32_t h = img.height;
  uint8_t min_a = 255;
  for (size_t i = 3; i < img.rgba.size(); i += 4) {
    if (img.rgba[i] < min_a) min_a = img.rgba[i];
  }
  if (min_a < 245) return;  // Texture already provides an authored alpha channel.

  // Measure average border luminance to distinguish black-background additive/smoke masks from solid fills.
  double border_sum = 0.0;
  uint32_t border_cnt = 0;
  auto add_border = [&](uint32_t x, uint32_t y) {
    const uint8_t* p = img.rgba.data() + (size_t(y) * w + x) * 4;
    border_sum += double(std::max({p[0], p[1], p[2]})) / 255.0;
    ++border_cnt;
  };
  for (uint32_t x = 0; x < w; ++x) {
    add_border(x, 0);
    if (h > 1) add_border(x, h - 1);
  }
  for (uint32_t y = 1; y + 1 < h; ++y) {
    add_border(0, y);
    if (w > 1) add_border(w - 1, y);
  }
  const float border_luma = (border_cnt > 0) ? float(border_sum / double(border_cnt)) : 0.0f;

  for (uint32_t y = 0; y < h; ++y) {
    const float ny = (float(y) + 0.5f) / float(h) * 2.0f - 1.0f;
    for (uint32_t x = 0; x < w; ++x) {
      const float nx = (float(x) + 0.5f) / float(w) * 2.0f - 1.0f;
      const float r = std::sqrt(nx * nx + ny * ny);
      const float edge = std::clamp(1.0f - (r - 0.62f) / 0.36f, 0.0f, 1.0f);
      const float smooth_edge = edge * edge * (3.0f - 2.0f * edge);
      uint8_t* p = img.rgba.data() + (size_t(y) * w + x) * 4;
      if (border_luma < 0.22f) {
        // Dark-border RGB-only particle texture (e.g. DXT1 glow/smoke): derive alpha from RGB luminance.
        const float peak = float(std::max({p[0], p[1], p[2]})) / 255.0f;
        const float norm = std::clamp((peak - border_luma) / std::max(1.0f - border_luma, 0.1f), 0.0f, 1.0f);
        p[3] = uint8_t(std::clamp(norm * smooth_edge * 255.0f, 0.0f, 255.0f));
      } else {
        // Uniformly opaque texture with bright corners: apply billowy radial puff falloff.
        const float ang = std::atan2(ny, nx);
        const float billow = 0.88f + 0.12f * std::sin(ang * 5.0f + r * 3.0f) * std::cos(ang * 3.0f - r * 2.0f);
        const float a = std::clamp(1.0f - r / billow, 0.0f, 1.0f);
        const float soft = a * a * (3.0f - 2.0f * a);
        p[0] = uint8_t(float(p[0]) * soft);
        p[1] = uint8_t(float(p[1]) * soft);
        p[2] = uint8_t(float(p[2]) * soft);
        p[3] = uint8_t(std::clamp(soft * 245.0f, 0.0f, 255.0f));
      }
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

  // 1. Build RGBA8 mipmapped particle texture atlas (10x10 grid, 100 slots).
  //    Slot 0 = procedural soft puff; slots 1..31 = <Texture2> / *_grad*.xds ramps; slots 32..99 = primary textures.
#ifdef __ANDROID__
  const uint32_t atlas_cell = low_memory_mode_ ? 128u : kExpandedAtlasCell;
  const uint32_t atlas_mips = low_memory_mode_ ? 8u : kExpandedAtlasMips;
#else
  const uint32_t atlas_cell = kExpandedAtlasCell;
  const uint32_t atlas_mips = kExpandedAtlasMips;
#endif
  const uint32_t atlas_size = kExpandedAtlasGrid * atlas_cell;
  std::vector<uint8_t> atlas_rgba(size_t(atlas_size) * atlas_size * 4, 0);
  fill_soft_puff_cell(atlas_rgba.data(), atlas_size, atlas_cell);
  atlas_tex_slot_.clear();

  if (lib) {
    std::vector<std::string> grad_order;
    std::vector<std::string> primary_order = {
        "gr_puffy_smoke_diff.xds",   "gr_backfire_diff.xds",     "vehicle_sparks_diff.xds",
        "gr_dirt_8x1_diff.xds",      "gr_gravel_8x1_diff.xds",   "gr_woodsplinters_diff.xds",
        "gr_aspen_leaves_diff.xds",  "gr_grass_8x1_diff.xds",    "gr_glassshards_diff.xds",
        "gr_glass_diff.xds",
    };
    std::unordered_set<std::string> seen_grad;
    std::unordered_set<std::string> seen_primary(primary_order.begin(), primary_order.end());

    auto push_grad = [&](const std::string& tname) {
      if (!tname.empty() && seen_grad.insert(tname).second) grad_order.push_back(tname);
    };
    auto push_primary = [&](const std::string& tname) {
      if (!tname.empty() && !seen_grad.count(tname) && seen_primary.insert(tname).second) {
        primary_order.push_back(tname);
      }
    };

    // Collect all <Texture2> and *_grad*.xds textures first so they occupy 5-bit shader slots [1..31].
    for (const auto& [ename, def] : lib->effects()) {
      (void)ename;
      if (!def.texture2_name.empty()) push_grad(def.texture2_name);
    }
    for (const auto& xname : lib->xds_names()) {
      if (xname.find("_grad") != std::string::npos) push_grad(xname);
    }
    for (const auto& [ename, def] : lib->effects()) {
      (void)ename;
      if (!def.texture_name.empty()) push_primary(def.texture_name);
    }
    for (const auto& xname : lib->xds_names()) {
      push_primary(xname);
    }

    auto pack_tex = [&](const std::string& tname, uint16_t& slot, uint16_t slot_limit, bool sanitize_alpha) {
      if (slot >= slot_limit || atlas_tex_slot_.count(tname)) return;
      fh1::RgbaImage img;
      if (lib->decode_texture(tname, img)) {
        if (sanitize_alpha) sanitize_primary_particle_alpha(img);
        const uint32_t cx = slot % kExpandedAtlasGrid;
        const uint32_t cy = slot / kExpandedAtlasGrid;
        blit_cell_bilinear(img.rgba.data(), img.width, img.height, atlas_rgba.data(), atlas_size, cx, cy,
                           atlas_cell);
        atlas_tex_slot_[tname] = slot;
        ++slot;
      }
    };

    uint16_t grad_slot = 1;
    for (const auto& tname : grad_order) {
      pack_tex(tname, grad_slot, uint16_t(kMaxTex2Slot + 1), false);
    }
    // Primary textures start right after the reserved <Texture2> range (slot 32..99),
    // and overflow any remaining slots cleanly into unused slots < 32 if ever needed.
    uint16_t prim_slot = uint16_t(kMaxTex2Slot + 1);
    for (const auto& tname : primary_order) {
      if (prim_slot < kExpandedAtlasMaxSlots) {
        pack_tex(tname, prim_slot, kExpandedAtlasMaxSlots, true);
      } else if (grad_slot <= kMaxTex2Slot) {
        pack_tex(tname, grad_slot, uint16_t(kMaxTex2Slot + 1), true);
      }
    }
  }

  auto upload_rgba_image_mips = [&](const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t mip_levels, bool srgb,
                                    GpuTexture& dst, const char* label) -> bool {
    mip_levels = std::max(1u, mip_levels);
    // Build sRGB-correct 2x2 box-filtered mip chain up to mip_levels (stops before cell boundaries merge).
    std::vector<std::vector<uint8_t>> mips(mip_levels);
    mips[0].assign(pixels, pixels + size_t(w) * h * 4);
    uint32_t mw = w, mh = h;
    auto u8_to_lin = [&](uint8_t v) -> float {
      const float c = float(v) / 255.0f;
      return srgb ? srgb_to_linear(c) : c;
    };
    auto lin_to_u8 = [&](float c) -> uint8_t {
      c = std::clamp(c, 0.0f, 1.0f);
      const float s = srgb ? (c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f) : c;
      return uint8_t(std::clamp(std::lround(double(s) * 255.0), 0l, 255l));
    };
    for (uint32_t lvl = 1; lvl < mip_levels; ++lvl) {
      const uint32_t nw = std::max(1u, mw / 2u);
      const uint32_t nh = std::max(1u, mh / 2u);
      mips[lvl].resize(size_t(nw) * nh * 4);
      const uint8_t* src = mips[lvl - 1].data();
      uint8_t* o = mips[lvl].data();
      for (uint32_t y = 0; y < nh; ++y) {
        const uint32_t sy0 = std::min(y * 2u, mh - 1u);
        const uint32_t sy1 = std::min(sy0 + 1u, mh - 1u);
        for (uint32_t x = 0; x < nw; ++x) {
          const uint32_t sx0 = std::min(x * 2u, mw - 1u);
          const uint32_t sx1 = std::min(sx0 + 1u, mw - 1u);
          const uint8_t* p00 = src + (size_t(sy0) * mw + sx0) * 4;
          const uint8_t* p10 = src + (size_t(sy0) * mw + sx1) * 4;
          const uint8_t* p01 = src + (size_t(sy1) * mw + sx0) * 4;
          const uint8_t* p11 = src + (size_t(sy1) * mw + sx1) * 4;
          uint8_t* d = o + (size_t(y) * nw + x) * 4;
          for (int c = 0; c < 3; ++c) {
            const float avg = 0.25f * (u8_to_lin(p00[c]) + u8_to_lin(p10[c]) + u8_to_lin(p01[c]) + u8_to_lin(p11[c]));
            d[c] = lin_to_u8(avg);
          }
          d[3] = uint8_t((uint32_t(p00[3]) + p10[3] + p01[3] + p11[3] + 2u) / 4u);
        }
      }
      mw = nw;
      mh = nh;
    }

    VkDeviceSize total_bytes = 0;
    std::vector<VkBufferImageCopy> copies(mip_levels);
    mw = w;
    mh = h;
    for (uint32_t lvl = 0; lvl < mip_levels; ++lvl) {
      copies[lvl] = {};
      copies[lvl].bufferOffset = total_bytes;
      copies[lvl].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lvl, 0, 1};
      copies[lvl].imageExtent = {mw, mh, 1};
      total_bytes += mips[lvl].size();
      mw = std::max(1u, mw / 2u);
      mh = std::max(1u, mh / 2u);
    }

    Renderer::Buffer staging;
    if (!r_.create_buffer(total_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
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
    auto* mp = static_cast<uint8_t*>(mapped);
    for (uint32_t lvl = 0; lvl < mip_levels; ++lvl) {
      std::memcpy(mp + copies[lvl].bufferOffset, mips[lvl].data(), mips[lvl].size());
    }
    vmaUnmapMemory(r_.allocator_, staging.allocation);
    const VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (!r_.create_image(w, h, mip_levels, fmt, VK_SAMPLE_COUNT_1_BIT,
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
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &b;
      vkCmdPipelineBarrier2(cmd, &dep);
      vkCmdCopyBufferToImage(cmd, staging.buffer, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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
    if (!ok) error = std::string(label) + " upload";
    return ok;
  };

  if (!upload_rgba_image_mips(atlas_rgba.data(), atlas_size, atlas_size, atlas_mips, true,
                              particle_atlas_, "particle atlas"))
    return false;
  std::vector<uint8_t>().swap(atlas_rgba);

  // Load retail media/tracks/treadmark.xds (512x128 two-band strip: left = asphalt rubber, right = dirt/gravel rut),
  // falling back cleanly to procedural skid textures when unavailable.
  fh1::RgbaImage retail_tm;
  bool uploaded_retail_skids = false;
  if (load_retail_treadmark(options_.game_root, retail_tm)) {
    if (retail_tm.width >= retail_tm.height * 2u) {
      const uint32_t half_w = retail_tm.width / 2u;
      const auto left_band = crop_rgba_band(retail_tm, 0u, half_w);
      const auto right_band = crop_rgba_band(retail_tm, half_w, half_w);
      const uint32_t mips = full_mip_count(half_w, retail_tm.height);
      if (upload_rgba_image_mips(left_band.data(), half_w, retail_tm.height, mips, true, skid_asphalt_tex_,
                                 "skid asphalt") &&
          upload_rgba_image_mips(right_band.data(), half_w, retail_tm.height, mips, true, skid_rut_tex_, "skid rut")) {
        uploaded_retail_skids = true;
      }
    } else {
      const uint32_t mips = full_mip_count(retail_tm.width, retail_tm.height);
      if (upload_rgba_image_mips(retail_tm.rgba.data(), retail_tm.width, retail_tm.height, mips, true,
                                 skid_asphalt_tex_, "skid asphalt") &&
          upload_rgba_image_mips(retail_tm.rgba.data(), retail_tm.width, retail_tm.height, mips, true, skid_rut_tex_,
                                 "skid rut")) {
        uploaded_retail_skids = true;
      }
    }
  }
  if (!uploaded_retail_skids) {
    const auto skid_asph = make_skid_texture(256, false);
    const auto skid_rut = make_skid_texture(256, true);
    if (!upload_rgba_image_mips(skid_asph.data(), 256, 256, 8u, true, skid_asphalt_tex_, "skid asphalt")) return false;
    if (!upload_rgba_image_mips(skid_rut.data(), 256, 256, 8u, true, skid_rut_tex_, "skid rut")) return false;
  }

  // 2. Samplers (trilinear mipmapping for both particle atlas and skidmark strips).
  VkSamplerCreateInfo psamp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  psamp.magFilter = psamp.minFilter = VK_FILTER_LINEAR;
  psamp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  psamp.minLod = 0.0f;
  psamp.maxLod = float(atlas_mips - 1u);
  psamp.addressModeU = psamp.addressModeV = psamp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_TRY(vkCreateSampler(r_.device_, &psamp, nullptr, &particle_sampler_), "particle sampler");

  VkSamplerCreateInfo ssamp = psamp;
  ssamp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  ssamp.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  ssamp.maxLod = 12.0f;
  ssamp.anisotropyEnable = r_.sampler_anisotropy() > 1.0f ? VK_TRUE : VK_FALSE;
  ssamp.maxAnisotropy = r_.sampler_anisotropy();
  VK_TRY(vkCreateSampler(r_.device_, &ssamp, nullptr, &skid_sampler_), "skid sampler");

  // 3. Mapped host-visible dynamic buffers.
  if (!create_dyn_buffer(particle_buf_, VkDeviceSize(kMaxParticleGpu) * sizeof(ParticleGpu), error)) {
    error = "particle instance buffer";
    return false;
  }
#ifdef __ANDROID__
  const uint32_t debris_cap_verts = low_memory_mode_ ? 16384u : 32768u;
  const uint32_t spectator_cap_verts = 32768u;
#else
  const uint32_t debris_cap_verts = kMaxDebrisVerts;
  const uint32_t spectator_cap_verts = kMaxSpectatorVerts;
#endif
  if (!create_dyn_buffer(skid_asphalt_buf_, VkDeviceSize(kMaxSkidVerts) * sizeof(SkidGpuVertex), error) ||
      !create_dyn_buffer(skid_rut_buf_, VkDeviceSize(kMaxSkidVerts) * sizeof(SkidGpuVertex), error) ||
      !create_dyn_buffer(debris_buf_, VkDeviceSize(debris_cap_verts) * sizeof(WorldVertex), error) ||
      !create_dyn_buffer(showcase_buf_, VkDeviceSize(kMaxShowcaseVerts) * sizeof(WorldVertex), error) ||
      !create_dyn_buffer(driver_buf_, VkDeviceSize(kMaxDriverVerts) * sizeof(WorldVertex), error)) {
    error = "skidmark/debris/driver vertex buffers";
    return false;
  }
  if (!options_.game_root.empty()) {
    std::string spec_err;
    bool anim_ok = false;
    if (auto* shared_anim = world::AnimatedWorldSystem::shared()) {
      // Reuse the single DriveApp-initialized AnimatedWorldSystem (including real TrackPostConfig
      // crowd tints/LOD distances, parsed .pgeo objects, and active Colorado/Rally world state).
      spectator_anim_.share_from(*shared_anim);
      anim_ok = spectator_anim_.catalog().has_3d_spectators();
    } else if (!low_memory_mode_) {
      world::TrackPostConfig post_cfg{};
      post_cfg.load_file(options_.game_root / "media" / "tracks" / "colorado" / "TrackSettings.xml");
      anim_ok = spectator_anim_.init(options_.game_root / "media", post_cfg, &spec_err) &&
                spectator_anim_.catalog().has_3d_spectators();
    }
    if (anim_ok) {
      const auto& cat = spectator_anim_.catalog();
      if (!cat.atlas_rgba.empty() &&
          upload_rgba_image_mips(cat.atlas_rgba.data(), world::SpectatorCatalog::kAtlasSize,
                                 world::SpectatorCatalog::kAtlasSize, world::SpectatorCatalog::kAtlasMipLevels, true,
                                 spectator_atlas_, "spectator atlas") &&
          gpu_textures_.size() >= 2 && allocate_world_set(spectator_set_, spec_err) &&
          create_dyn_buffer(spectator_buf_, VkDeviceSize(spectator_cap_verts) * sizeof(WorldVertex), spec_err)) {
#ifdef __ANDROID__
        // Free the 16 MiB CPU-side 2048x2048 RGBA spectator atlas once uploaded to GPU.
        if (!r_.rt_supported()) {
          const_cast<world::SpectatorCatalog&>(cat).release_atlas_rgba();
        }
#endif
        // Bind mipmapped 2048x2048 spectator atlas to both binding 0 (albedo) and binding 3
        // (light_map) so dusk/night festival & outpost floodlight bounce illuminates crowds cleanly.
        const VkImageView white = gpu_textures_[0].view;
        const VkImageView flat_n = flat_normal_tex_.view ? flat_normal_tex_.view : white;
        const VkImageView views[8] = {spectator_atlas_.view, white, white, spectator_atlas_.view,
                                      flat_n,                white, white, white};
        VkDescriptorImageInfo imgs[8];
        VkWriteDescriptorSet writes[8];
        for (uint32_t b = 0; b < 8; ++b) {
          imgs[b] = {sampler_, views[b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
          writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          writes[b].dstSet = spectator_set_;
          writes[b].dstBinding = b;
          writes[b].descriptorCount = 1;
          writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          writes[b].pImageInfo = &imgs[b];
        }
        vkUpdateDescriptorSets(r_.device_, 8, writes, 0, nullptr);
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

    VkPipelineMultisampleStateCreateInfo pms = ms;
    // When MSAA depth resolve is supported, particles draw onto the resolved 1-sample target
    // so Pass 1 keeps MSAA color & depth 100% transient in TBDR GMEM (STORE_OP_DONT_CARE).
    if (r_.samples_ != VK_SAMPLE_COUNT_1_BIT && r_.depth_resolve_supported()) {
      pms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    }
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rend;
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &pms;
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
  const float cell_uv = 1.0f / float(kExpandedAtlasGrid);
  const float inset = 1.5f / float(kExpandedAtlasSize);

  auto emit_batch = [&](const std::vector<world::ParticleSprite>& list) -> uint32_t {
    uint32_t added = 0;
    for (const auto& sp : list) {
      if (out_n >= kMaxParticleGpu) break;
      uint16_t atlas_slot = 0;
      if (sp.texture_slot > 0 && size_t(sp.texture_slot - 1) < used_textures.size()) {
        auto it = atlas_tex_slot_.find(used_textures[size_t(sp.texture_slot - 1)]);
        if (it != atlas_tex_slot_.end()) atlas_slot = it->second;
      }
      uint16_t atlas_slot2 = 0;
      if (sp.texture2_slot > 0 && size_t(sp.texture2_slot - 1) < used_textures.size()) {
        auto it2 = atlas_tex_slot_.find(used_textures[size_t(sp.texture2_slot - 1)]);
        if (it2 != atlas_tex_slot_.end() && it2->second >= 1u && it2->second <= kMaxTex2Slot) {
          atlas_slot2 = it2->second;
        }
      }
      const uint32_t cx = atlas_slot % kExpandedAtlasGrid;
      const uint32_t cy = atlas_slot / kExpandedAtlasGrid;
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
      // Bind <Texture2> / <Gradient> atlas slot (1..31) in bits 3..7 of flags for particle.frag.
      const uint32_t tex2_idx = (sp.use_gradient || atlas_slot2 > 0u) ? uint32_t(atlas_slot2 & 0x1Fu) : 0u;
      flags |= (tex2_idx << 3u);
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
      const float clamped_soft = std::clamp(sp.soft_depth_m, 0.0f, 3.5f);
      if (tex2_idx > 0u) {
        const uint32_t grad_q = uint32_t(std::clamp(int(std::lround(sp.gradient_row_v * 16.0f - 0.5f)), 0, 15));
        g.extra[1] = clamped_soft + float(grad_q + 1u) * 4.0f;
      } else {
        g.extra[1] = clamped_soft;
      }
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
  const uint32_t max_debris = uint32_t(debris_buf_.cap / sizeof(WorldVertex));
  const uint32_t max_in = uint32_t(std::min(tris.size(), size_t(max_debris)) / 3u) * 3u;
  if (max_in == 0) return;

  // Camera near-lens exclusion sphere in scenery coordinates: cull debris triangles
  // that drift right in front of the camera lens (< 2.15 m) so a smashed road sign
  // can never hover giant across the viewport.
  const auto cam_eye = eye_in_scenery(world_transform_);
  const bool have_cam_eye =
      std::isfinite(cam_eye[0]) && std::isfinite(cam_eye[1]) && std::isfinite(cam_eye[2]);
  constexpr float kLensCullRadius2 = 2.15f * 2.15f;

  auto* dst = static_cast<WorldVertex*>(debris_buf_.data());
  auto snorm8 = [](float x) -> int8_t {
    return int8_t(std::clamp(std::lround(double(x) * 127.0), -127l, 127l));
  };
  uint32_t out_n = 0;
  for (uint32_t i = 0; i < max_in; i += 3u) {
    float p[3][3];
    bool bad = false;
    float min_d2 = 1e30f;
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    for (int k = 0; k < 3; ++k) {
      const auto& v = tris[i + uint32_t(k)];
      p[k][0] = float(double(v.pos[0]) - world_origin_[0]);
      p[k][1] = v.pos[1];
      p[k][2] = float(double(v.pos[2]) - world_origin_[1]);
      if (!std::isfinite(p[k][0]) || !std::isfinite(p[k][1]) || !std::isfinite(p[k][2])) {
        bad = true;
        break;
      }
      cx += p[k][0];
      cy += p[k][1];
      cz += p[k][2];
      if (have_cam_eye) {
        const float dx = p[k][0] - cam_eye[0];
        const float dy = p[k][1] - cam_eye[1];
        const float dz = p[k][2] - cam_eye[2];
        min_d2 = std::min(min_d2, dx * dx + dy * dy + dz * dz);
      }
    }
    if (bad) continue;
    if (have_cam_eye) {
      const float dx = cx * (1.0f / 3.0f) - cam_eye[0];
      const float dy = cy * (1.0f / 3.0f) - cam_eye[1];
      const float dz = cz * (1.0f / 3.0f) - cam_eye[2];
      const float cd2 = dx * dx + dy * dy + dz * dz;
      if (min_d2 < kLensCullRadius2 || cd2 < kLensCullRadius2) continue;
    }

    const uint32_t first_v = out_n;
    for (int k = 0; k < 3; ++k) {
      const auto& v = tris[i + uint32_t(k)];
      WorldVertex& o = dst[out_n++];
      o = WorldVertex{};
      o.position[0] = p[k][0];
      o.position[1] = p[k][1];
      o.position[2] = p[k][2];
      o.normal[0] = snorm8(v.normal[0]);
      o.normal[1] = snorm8(v.normal[1]);
      o.normal[2] = snorm8(v.normal[2]);
      o.normal[3] = 0;
      o.uv0[0] = v.uv[0];
      o.uv0[1] = v.uv[1];
      for (int c = 0; c < 4; ++c) o.colour[c] = v.rgba[c];
    }
    const uint32_t tex = tris[i].texture_record;
    const uint8_t cut = tris[i].cutout;
    if (!debris_batches_.empty() && debris_batches_.back().texture_record == tex &&
        debris_batches_.back().cutout == cut) {
      debris_batches_.back().vertex_count += 3u;
    } else {
      debris_batches_.push_back({first_v, 3u, tex, cut});
    }
  }
  debris_count_ = out_n;
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
        // Bind world_coverage_ ONLY for alpha-cutout debris (b.cutout != 0); opaque debris
        // (b.cutout == 0, pc.params[0] = 0.0f) writes surf_gloss (~0.0) into .a under linear HDR,
        // which hardware alpha-to-coverage at MSAA would discard if bound to world_coverage_.
        bind_pipe((b.cutout && world_coverage_) ? world_coverage_ : world_opaque_);
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
    const auto eye = eye_in_scenery(world_transform_);
    const double eye_gx = world_origin_[0] + double(eye[0]);
    const double eye_gy = double(eye[1]);
    const double eye_gz = world_origin_[1] + double(eye[2]);
    const float night_f = tod_active_ ? std::clamp(tod_state_.switch_on_lights, 0.0f, 1.0f) : 0.0f;

    // Extract 5 normalized scenery-space frustum planes (left, right, bottom, top, in-front)
    // from view_projection * world_transform_ for tight cluster & spectator frustum culling.
    world::SpectatorFrustum frustum{};
    frustum.origin_x = world_origin_[0];
    frustum.origin_z = world_origin_[1];
    float clip[16];
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) {
        float s = 0.0f;
        for (int k = 0; k < 4; ++k) s += frame_.view_projection[k * 4 + r] * world_transform_[c * 4 + k];
        clip[c * 4 + r] = s;
      }
    }
    for (int c = 0; c < 4; ++c) {
      const float x = clip[c * 4 + 0], y = clip[c * 4 + 1], w = clip[c * 4 + 3];
      frustum.planes[0][size_t(c)] = w + x;
      frustum.planes[1][size_t(c)] = w - x;
      frustum.planes[2][size_t(c)] = w + y;
      frustum.planes[3][size_t(c)] = w - y;
      frustum.planes[4][size_t(c)] = w;
    }
    bool frustum_ok = true;
    for (auto& pl : frustum.planes) {
      const float len = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
      if (len > 1e-6f) {
        const float inv = 1.0f / len;
        pl[0] *= inv;
        pl[1] *= inv;
        pl[2] *= inv;
        pl[3] *= inv;
      } else {
        frustum_ok = false;
      }
    }
    frustum.active = frustum_ok && world_cull_;
    const std::array<float, 3> cam_fwd = frustum_ok
        ? std::array<float, 3>{frustum.planes[4][0], frustum.planes[4][1], frustum.planes[4][2]}
        : std::array<float, 3>{0.0f, 0.0f, 1.0f};

    auto& cache = spectator_anim_.frame_cache();
    const double cdx = eye_gx - cache.eye_x;
    const double cdy = eye_gy - cache.eye_y;
    const double cdz = eye_gz - cache.eye_z;
    const double cam_move2 = cdx * cdx + cdy * cdy + cdz * cdz;
    const float fwd_dot =
        cam_fwd[0] * cache.cam_fwd[0] + cam_fwd[1] * cache.cam_fwd[1] + cam_fwd[2] * cache.cam_fwd[2];
    const float dt_anim = std::fabs(tod_real_time_ - cache.time_sec);
    // Throttle animated crowd rebuilds cleanly: 60 Hz close-up (<= 40 m), 25 Hz distant (> 40 m),
    // and skip completely when paused (dt_anim == 0) with stationary camera/world!
    const float min_interval = (cache.nearest_dist > 40.0f) ? (1.0f / 25.0f) : (1.0f / 60.0f);
    const bool need_rebuild = !cache.valid || cache.world_gen != world_generation_ ||
                              cache.origin_x != world_origin_[0] || cache.origin_z != world_origin_[1] ||
                              std::fabs(night_f - cache.night_f) > 0.04f || cam_move2 > (2.5 * 2.5) ||
                              fwd_dot < 0.96f || dt_anim >= min_interval;

    if (need_rebuild) {
      const drive::Ground* active_ground = world_ground_ ? world_ground_ : world::AnimatedWorldSystem::last_ground();
      spectator_cpu_.clear();
      cache.shadow_discs.clear();
      float nearest_dist = 1e6f;
      spectator_anim_.build_spectator_mesh(eye_gx, eye_gy, eye_gz, tod_real_time_, active_ground, spectator_cpu_,
                                           &cache.shadow_discs, &frustum, &nearest_dist);
      constexpr uint32_t kShadowSegs = 8;
      const uint32_t max_spec_verts = uint32_t(spectator_buf_.cap / sizeof(WorldVertex));
      const uint32_t max_shadow_verts = uint32_t(cache.shadow_discs.size()) * kShadowSegs * 3u;
      const uint32_t spec_cap =
          (max_spec_verts > max_shadow_verts) ? (max_spec_verts - max_shadow_verts) : max_spec_verts;
      const uint32_t n = (uint32_t(std::min(spectator_cpu_.size(), size_t(spec_cap))) / 3u) * 3u;
      uint32_t shadow_verts = 0;

      if (n > 0) {
        // Stage into cached CPU vector (`spectator_buf_.cpu`), then dyn_sync copies once into GPU memory.
        auto* dst = static_cast<WorldVertex*>(spectator_buf_.data());
        auto fast_snorm8 = [](float x) -> int8_t {
          const int v = int(x * 127.0f + (x >= 0.0f ? 0.5f : -0.5f));
          return int8_t(v < -127 ? -127 : (v > 127 ? 127 : v));
        };
        const double ox = world_origin_[0];
        const double oz = world_origin_[1];
        for (uint32_t i = 0; i < n; ++i) {
          const auto& v = spectator_cpu_[i];
          WorldVertex& o = dst[i];
          o.position[0] = float(double(v.pos[0]) - ox);
          o.position[1] = v.pos[1];
          o.position[2] = float(double(v.pos[2]) - oz);
          o.normal[0] = fast_snorm8(v.normal[0]);
          o.normal[1] = fast_snorm8(v.normal[1]);
          o.normal[2] = fast_snorm8(v.normal[2]);
          o.normal[3] = 0;
          o.uv0[0] = v.uv[0];
          o.uv0[1] = v.uv[1];
          o.uv1[0] = 0.0f;
          o.uv1[1] = 0.0f;
          o.colour[0] = v.rgba[0];
          o.colour[1] = v.rgba[1];
          o.colour[2] = v.rgba[2];
          o.colour[3] = v.rgba[3];
          o.centre[0] = o.centre[1] = o.centre[2] = o.centre[3] = 0;
          o.uv2[0] = to_half(v.uv2[0]);
          o.uv2[1] = to_half(v.uv2[1]);
        }

        // Append soft ground contact shadow discs under each spectator's feet so crowds ground cleanly.
        if (world_alpha_ && world_white_set_ && !cache.shadow_discs.empty()) {
          const uint8_t center_a = uint8_t(std::lround(112.0f * (1.0f - 0.58f * night_f)));
          static constexpr std::array<float, kShadowSegs + 1> kCosSeg{{
              1.0f, 0.70710678f, 0.0f, -0.70710678f, -1.0f, -0.70710678f, 0.0f, 0.70710678f, 1.0f,
          }};
          static constexpr std::array<float, kShadowSegs + 1> kSinSeg{{
              0.0f, 0.70710678f, 1.0f, 0.70710678f, 0.0f, -0.70710678f, -1.0f, -0.70710678f, 0.0f,
          }};
          for (const auto& d : cache.shadow_discs) {
            if (n + shadow_verts + kShadowSegs * 3u > max_spec_verts) break;
            const float cx = float(double(d[0]) - ox);
            const float cy = d[1];
            const float cz = float(double(d[2]) - oz);
            const float rad = d[3];
            WorldVertex vc{};
            vc.position[0] = cx;
            vc.position[1] = cy;
            vc.position[2] = cz;
            vc.normal[1] = 127;
            vc.colour[3] = center_a;
            WorldVertex v0 = vc;
            v0.colour[3] = 0;
            WorldVertex v1 = v0;
            for (uint32_t s = 0; s < kShadowSegs; ++s) {
              v0.position[0] = cx + kCosSeg[s] * rad;
              v0.position[2] = cz + kSinSeg[s] * rad;
              v1.position[0] = cx + kCosSeg[s + 1u] * rad;
              v1.position[2] = cz + kSinSeg[s + 1u] * rad;
              dst[n + shadow_verts + 0] = vc;
              dst[n + shadow_verts + 1] = v0;
              dst[n + shadow_verts + 2] = v1;
              shadow_verts += 3u;
            }
          }
        }
        ++spectator_buf_.ver;
      }

      cache.valid = true;
      cache.world_gen = world_generation_;
      cache.origin_x = world_origin_[0];
      cache.origin_z = world_origin_[1];
      cache.eye_x = eye_gx;
      cache.eye_y = eye_gy;
      cache.eye_z = eye_gz;
      cache.cam_fwd = cam_fwd;
      cache.time_sec = tod_real_time_;
      cache.night_f = night_f;
      cache.nearest_dist = nearest_dist;
      cache.spec_verts = n;
      cache.shadow_verts = shadow_verts;
    }

    const uint32_t n = cache.spec_verts;
    const uint32_t shadow_verts = cache.shadow_verts;
    spectator_count_ = n;
    if (n > 0) {
      VkDeviceSize at = dyn_sync(spectator_buf_, VkDeviceSize(n + shadow_verts) * sizeof(WorldVertex));
      vkCmdBindVertexBuffers(cmd, 0, 1, &spectator_buf_.buf.buffer, &at);

      if (shadow_verts > 0) {
        bind_pipe(world_alpha_);
        const VkDescriptorSet ssets[2] = {frame_set_, world_white_set_};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 2, ssets, 0, nullptr);
        pc.params[0] = 2.0f;     // kAlpha
        pc.params[1] = 1024.0f;  // kFlagUnlitVertexColor
        pc.params[3] = 1.0f;
        vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof pc, &pc);
        vkCmdDraw(cmd, shadow_verts, 1, n, 0);
      }

      bind_pipe(world_opaque_);
      const VkDescriptorSet sets[2] = {frame_set_, spectator_set_};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 2, sets, 0, nullptr);
      pc.params[0] = 0.0f;            // kOpaque
      pc.params[1] = float(1u | 2048u);  // kFlagTextured | kFlagLightMap (dusk/night stage light bounce)
      pc.params[3] = 1.0f;
      vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof pc, &pc);
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

namespace {

struct DetachedCarPartBody {
  int part_kind = 0;  // 0=front bumper, 1=rear bumper, 2=left mirror, 3=right mirror, 4=rear wing
  std::array<float, 16> car_m0{};   // car model-to-world matrix at exact detachment instant
  std::array<float, 3> pivot_w0{};  // part centroid in world space at detachment instant
  std::array<float, 3> pos{};       // current centroid position in world space
  std::array<float, 3> vel{};
  drive::Quat delta_rot{0.0, 0.0, 0.0, 1.0};  // world-space tumble rotation since detachment
  std::array<float, 3> omega{};
  float ground_radius = 0.12f;
  float age = 0.0f;
};

std::mutex g_detached_parts_mutex;
std::unordered_map<const void*, std::vector<DetachedCarPartBody>> g_detached_parts_by_owner;

void mul4x4_col(const float a[16], const float b[16], float out[16]) {
  float r[16];
  for (int c = 0; c < 4; ++c) {
    for (int row = 0; row < 4; ++row) {
      r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1] +
                       a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
    }
  }
  std::memcpy(out, r, sizeof(r));
}

bool invert_rigid_or_affine(const float m[16], float out[16]) {
  const double a = m[0], b = m[4], c = m[8];
  const double d = m[1], e = m[5], f = m[9];
  const double g = m[2], h = m[6], k = m[10];
  const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
  if (std::fabs(det) < 1e-12) return false;
  const double id = 1.0 / det;
  const double inv[9] = {
      (e * k - f * h) * id, (c * h - b * k) * id, (b * f - c * e) * id,
      (f * g - d * k) * id, (a * k - c * g) * id, (c * d - a * f) * id,
      (d * h - e * g) * id, (b * g - a * h) * id, (a * e - b * d) * id,
  };
  std::memset(out, 0, sizeof(float) * 16);
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) out[col * 4 + row] = float(inv[row * 3 + col]);
  for (int row = 0; row < 3; ++row) {
    out[12 + row] = float(-(inv[row * 3 + 0] * m[12] + inv[row * 3 + 1] * m[13] + inv[row * 3 + 2] * m[14]));
  }
  out[15] = 1.0f;
  return true;
}

void build_part_world_delta(const DetachedCarPartBody& b, float scale, float out[16]) {
  const drive::Vec3 rx = drive::rotate(b.delta_rot, {double(scale), 0.0, 0.0});
  const drive::Vec3 ry = drive::rotate(b.delta_rot, {0.0, double(scale), 0.0});
  const drive::Vec3 rz = drive::rotate(b.delta_rot, {0.0, 0.0, double(scale)});
  // D(v) = pos + R(delta_rot) * scale * (v - pivot_w0)
  out[0] = float(rx.x); out[1] = float(rx.y); out[2] = float(rx.z); out[3] = 0.0f;
  out[4] = float(ry.x); out[5] = float(ry.y); out[6] = float(ry.z); out[7] = 0.0f;
  out[8] = float(rz.x); out[9] = float(rz.y); out[10] = float(rz.z); out[11] = 0.0f;
  const double px = b.pivot_w0[0], py = b.pivot_w0[1], pz = b.pivot_w0[2];
  out[12] = float(double(b.pos[0]) - (rx.x * px + ry.x * py + rz.x * pz));
  out[13] = float(double(b.pos[1]) - (rx.y * px + ry.y * py + rz.y * pz));
  out[14] = float(double(b.pos[2]) - (rx.z * px + ry.z * py + rz.z * pz));
  out[15] = 1.0f;
}

}  // namespace

void spawn_detached_car_part(const void* owner_key, int part_kind, const float car_model_to_world[16],
                             const std::array<float, 3>& part_center_model,
                             const std::array<float, 3>& part_half_extents,
                             const std::array<float, 3>& car_vel, const std::array<float, 3>& car_fwd,
                             const std::array<float, 3>& car_right, const std::array<float, 3>& car_up) {
  if (!owner_key || !car_model_to_world || part_kind < 0 || part_kind >= 5) return;
  DetachedCarPartBody b{};
  b.part_kind = part_kind;
  std::memcpy(b.car_m0.data(), car_model_to_world, sizeof(float) * 16);
  const float cx = part_center_model[0], cy = part_center_model[1], cz = part_center_model[2];
  b.pivot_w0 = {
      car_model_to_world[0] * cx + car_model_to_world[4] * cy + car_model_to_world[8] * cz + car_model_to_world[12],
      car_model_to_world[1] * cx + car_model_to_world[5] * cy + car_model_to_world[9] * cz + car_model_to_world[13],
      car_model_to_world[2] * cx + car_model_to_world[6] * cy + car_model_to_world[10] * cz + car_model_to_world[14],
  };
  b.pos = b.pivot_w0;
  b.delta_rot = {0.0, 0.0, 0.0, 1.0};
  b.ground_radius = std::clamp(std::min({part_half_extents[0], part_half_extents[1], part_half_extents[2]}), 0.06f, 0.28f);

  // Deterministic tumble impulse tuned per detached component so the car's actual baked
  // bumper/mirror/wing detaches cleanly from its exact mounting pose and tumbles down the road.
  float fwd_kick = 0.0f, right_kick = 0.0f, up_kick = 2.4f;
  switch (part_kind) {
    case 0:  // front bumper
      fwd_kick = 3.0f;
      right_kick = 1.5f;
      up_kick = 2.2f;
      b.omega = {4.0f, 2.6f, -1.8f};
      break;
    case 1:  // rear bumper
      fwd_kick = -3.6f;
      right_kick = -1.4f;
      up_kick = 2.1f;
      b.omega = {-4.4f, 2.2f, 2.0f};
      break;
    case 2:  // left mirror
      fwd_kick = -1.2f;
      right_kick = -4.0f;
      up_kick = 3.4f;
      b.omega = {8.0f, -6.0f, 8.5f};
      break;
    case 3:  // right mirror
      fwd_kick = -1.2f;
      right_kick = 4.0f;
      up_kick = 3.4f;
      b.omega = {-8.0f, 6.2f, -8.5f};
      break;
    default:  // rear wing (4)
      fwd_kick = -4.2f;
      right_kick = 0.9f;
      up_kick = 4.2f;
      b.omega = {-6.5f, 1.8f, 3.2f};
      break;
  }

  for (int k = 0; k < 3; ++k) {
    b.vel[size_t(k)] = car_vel[size_t(k)] * 0.78f + car_fwd[size_t(k)] * fwd_kick +
                       car_right[size_t(k)] * right_kick + car_up[size_t(k)] * up_kick;
  }

  std::lock_guard<std::mutex> lock(g_detached_parts_mutex);
  auto& pool = g_detached_parts_by_owner[owner_key];
  pool.erase(std::remove_if(pool.begin(), pool.end(),
                            [&](const DetachedCarPartBody& existing) { return existing.part_kind == part_kind; }),
             pool.end());
  pool.push_back(b);
}

void clear_detached_car_parts(const void* owner_key) {
  std::lock_guard<std::mutex> lock(g_detached_parts_mutex);
  if (!owner_key) {
    g_detached_parts_by_owner.clear();
  } else {
    g_detached_parts_by_owner.erase(owner_key);
  }
}

void step_detached_car_parts(const void* owner_key, float dt, const drive::Ground* ground,
                             const float car_model_to_world_now[16], Showroom& showroom) {
  if (!owner_key || !car_model_to_world_now) return;
  constexpr float kMaxAge = 12.0f;
  const float step_dt = std::clamp(dt, 0.0f, 0.05f);

  float world_to_car_now[16];
  if (!invert_rigid_or_affine(car_model_to_world_now, world_to_car_now)) return;

  bool active_mask[5] = {false, false, false, false, false};
  float xforms[5][16]{};

  {
    std::lock_guard<std::mutex> lock(g_detached_parts_mutex);
    auto it = g_detached_parts_by_owner.find(owner_key);
    if (it != g_detached_parts_by_owner.end() && !it->second.empty()) {
      auto& pool = it->second;
      size_t write = 0;
      for (size_t i = 0; i < pool.size(); ++i) {
        DetachedCarPartBody& b = pool[i];
        b.age += step_dt;
        if (b.age >= kMaxAge) continue;

        if (step_dt > 0.0f) {
          b.vel[1] -= 9.81f * step_dt;
          const float air_drag = std::exp(-0.42f * step_dt);
          b.vel[0] *= air_drag;
          b.vel[2] *= air_drag;
          b.pos[0] += b.vel[0] * step_dt;
          b.pos[1] += b.vel[1] * step_dt;
          b.pos[2] += b.vel[2] * step_dt;

          const double wx = b.omega[0], wy = b.omega[1], wz = b.omega[2];
          const double half_dt = 0.5 * double(step_dt);
          const drive::Quat q = b.delta_rot;
          drive::Quat nq{
              q.x + half_dt * (wx * q.w + wy * q.z - wz * q.y),
              q.y + half_dt * (wy * q.w + wz * q.x - wx * q.z),
              q.z + half_dt * (wz * q.w + wx * q.y - wy * q.x),
              q.w + half_dt * (-wx * q.x - wy * q.y - wz * q.z),
          };
          const double qn = std::sqrt(nq.x * nq.x + nq.y * nq.y + nq.z * nq.z + nq.w * nq.w);
          if (qn > 1e-8) {
            nq.x /= qn;
            nq.y /= qn;
            nq.z /= qn;
            nq.w /= qn;
            b.delta_rot = nq;
          }

          if (ground) {
            drive::Hit hit{};
            const double probe_y = double(b.pos[1]) + 1.5;
            if (ground->raycast({double(b.pos[0]), probe_y, double(b.pos[2])}, {0.0, -1.0, 0.0}, 10.0, hit)) {
              const float gnd_y = float(probe_y - hit.distance);
              const float r_gnd = std::max(b.ground_radius, 0.075f);
              if (b.pos[1] - r_gnd < gnd_y) {
                b.pos[1] = gnd_y + r_gnd;
                if (b.vel[1] < -0.65f) {
                  b.vel[1] = -b.vel[1] * 0.36f;
                  b.vel[0] *= 0.74f;
                  b.vel[2] *= 0.74f;
                  b.omega[0] *= 0.78f;
                  b.omega[1] *= 0.78f;
                  b.omega[2] *= 0.78f;
                } else {
                  b.vel[1] = 0.0f;
                  const float fric = std::exp(-4.2f * step_dt);
                  b.vel[0] *= fric;
                  b.vel[2] *= fric;
                  const float ang_damp = std::exp(-5.0f * step_dt);
                  b.omega[0] *= ang_damp;
                  b.omega[1] *= ang_damp;
                  b.omega[2] *= ang_damp;
                }
              }
            }
          }
        }

        const float fade = (b.age > kMaxAge - 1.2f) ? std::clamp((kMaxAge - b.age) / 1.2f, 0.0f, 1.0f) : 1.0f;
        if (fade > 0.02f && b.part_kind >= 0 && b.part_kind < 5) {
          float delta_w[16], part_w[16], part_in_car[16];
          build_part_world_delta(b, fade, delta_w);
          mul4x4_col(delta_w, b.car_m0.data(), part_w);
          mul4x4_col(world_to_car_now, part_w, part_in_car);
          active_mask[b.part_kind] = true;
          std::memcpy(xforms[b.part_kind], part_in_car, sizeof(part_in_car));
        }

        if (write != i) pool[write] = b;
        ++write;
      }
      pool.resize(write);
      if (pool.empty()) g_detached_parts_by_owner.erase(it);
    }
  }

  for (int pk = 0; pk < 5; ++pk) {
    showroom.set_detached_part_transform(pk, active_mask[pk], active_mask[pk] ? xforms[pk] : nullptr);
  }
}


}  // namespace carview
