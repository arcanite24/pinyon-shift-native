#include "fh1data/world_texture.h"

#include <cstring>
#include <numeric>

#include "fh1data/bytes.h"

namespace fh1 {
namespace {

bool fail(std::string* error, const std::string& why) {
  if (error) *error = why;
  return false;
}

// X, Y, Z, W in order.
constexpr uint32_t kIdentitySwizzle = 0 | 1 << 3 | 2 << 6 | 3 << 9;


}  // namespace

WorldTextureFormat world_texture_format(uint32_t word) {
  WorldTextureFormat f;
  f.word = word;
  f.format = word & 0x3F;
  f.endian = (word >> 6) & 3;
  f.tiled = ((word >> 8) & 1) != 0;
  f.gamma = ((word >> 9) & 0x3F) == 0x3F;
  return f;
}

XdsHeader world_xds_header(uint32_t width, uint32_t height, uint32_t levels, uint32_t format_word) {
  const WorldTextureFormat f = world_texture_format(format_word);
  XdsHeader h;
  h.format = f.format;
  h.width = width;
  h.height = height;
  h.pitch = width;
  h.tiled = f.tiled;
  h.endian = f.endian;
  h.levels = std::max(levels, 1u);
  h.packed_mips = true;
  h.dimension = XdsDimension::k2D;
  h.swizzle = kIdentitySwizzle;
  return h;
}

bool parse_bix_header(std::span<const uint8_t> bix, BixHeader& out, std::string* error) {
  if (bix.size() < 28 || std::memcmp(bix.data(), "BIX1", 4) != 0) return fail(error, "not a .bix (BIX1)");
  out.width = load_be32(&bix[4]);
  out.height = load_be32(&bix[8]);
  out.levels = load_be32(&bix[12]);
  out.format_word = load_be32(&bix[16]);
  out.total_bytes = load_be32(&bix[20]);
  out.base_bytes = load_be32(&bix[24]);
  if (out.width == 0 || out.height == 0 || out.width > 4096 || out.height > 4096 ||
      out.levels == 0 || out.levels > 13 || out.base_bytes > out.total_bytes)
    return fail(error, ".bix header out of range");
  return true;
}

bool parse_caff_header(std::span<const uint8_t> bin, CaffHeader& out, std::string* error) {
  if (bin.size() < 0x2B0 || std::memcmp(bin.data(), "CAFF", 4) != 0) return fail(error, "not a CAFF texture");
  out.format_word = load_be32(&bin[0x1A8]);
  out.width = load_be16(&bin[0x1B4]);
  out.height = load_be16(&bin[0x1B6]);
  out.levels = bin[0x1C0];
  if (out.width == 0 || out.height == 0 || out.width > 4096 || out.height > 4096 ||
      out.levels == 0 || out.levels > 13)
    return fail(error, "CAFF texture header out of range");
  return true;
}

bool decode_bix(std::span<const uint8_t> bix, std::span<const uint8_t> base, WorldTexture& out, std::string* error,
                bool* exact) {
  out = WorldTexture{};
  BixHeader b;
  if (!parse_bix_header(bix, b, error)) return false;
  out.format = world_texture_format(b.format_word);
  out.width = b.width, out.height = b.height, out.levels = b.levels;
  const XdsHeader h = world_xds_header(b.width, b.height, b.levels, b.format_word);
  const std::vector<size_t> sizes = xds_trimmed_regions(h);
  if (sizes.empty()) return fail(error, "format " + std::to_string(out.format.format) + " not laid out");
  // The base is the _B file whatever its size; the chain runs region by
  // region from the end of the header.
  std::vector<std::span<const uint8_t>> regions{base};
  const auto chain = bix.subspan(28);
  size_t at = 0;
  for (size_t r = 1; r < sizes.size() && at + sizes[r] <= chain.size(); ++r) {
    regions.push_back(chain.subspan(at, sizes[r]));
    at += sizes[r];
  }
  if (exact) {
    const size_t chain_bytes = std::accumulate(sizes.begin() + 1, sizes.end(), size_t(0));
    *exact = base.size() == b.base_bytes && chain.size() == size_t(b.total_bytes - b.base_bytes) &&
             chain_bytes == chain.size();
  }
  return extract_region_blocks(h, regions, out.blocks, error);
}

bool decode_caff(std::span<const uint8_t> bin, WorldTexture& out, std::string* error, bool* exact) {
  out = WorldTexture{};
  out.caff = true;
  CaffHeader c;
  if (!parse_caff_header(bin, c, error)) return false;
  out.format = world_texture_format(c.format_word);
  out.width = c.width, out.height = c.height, out.levels = c.levels;
  const XdsHeader h = world_xds_header(c.width, c.height, c.levels, c.format_word);
  std::vector<size_t> sizes = xds_trimmed_regions(h);
  if (sizes.empty()) return fail(error, "format " + std::to_string(out.format.format) + " not laid out");
  // A texture whose levels are all in the tail is one full region.
  if (sizes.size() == 1) sizes[0] = xds_layout_bytes(h);
  const auto payload = bin.subspan(c.payload_offset);
  std::vector<std::span<const uint8_t>> regions;
  size_t at = 0;
  for (size_t r = 0; r < sizes.size() && at < payload.size(); ++r) {
    regions.push_back(payload.subspan(at, std::min(sizes[r], payload.size() - at)));
    at += sizes[r];
  }
  if (exact) *exact = std::accumulate(sizes.begin(), sizes.end(), size_t(0)) == payload.size();
  return extract_region_blocks(h, regions, out.blocks, error);
}

bool world_texture_has_cutout(const WorldTexture& t) {
  if (t.blocks.levels.empty()) return false;
  const auto& blocks = t.blocks.levels[0].blocks;
  const uint32_t bb = t.blocks.block_bytes;
  if (bb == 0) return false;
  for (size_t at = 0; at + bb <= blocks.size(); at += bb) {
    const uint8_t* b = &blocks[at];
    switch (t.blocks.format) {
      case XdsBlockFormat::kBc1: {
        const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
        if (c0 > c1) break;
        const uint32_t bits = load_le32(b + 4);
        for (int i = 0; i < 16; ++i)
          if (((bits >> (2 * i)) & 3) == 3) return true;
        break;
      }
      case XdsBlockFormat::kBc2:
        for (int i = 0; i < 8; ++i)
          if ((b[i] & 0x0F) < 8 || (b[i] >> 4) < 8) return true;
        break;
      case XdsBlockFormat::kBc3: {
        const uint32_t a0 = b[0], a1 = b[1];
        uint32_t palette[8] = {a0, a1};
        if (a0 > a1) {
          for (uint32_t i = 1; i < 7; ++i) palette[i + 1] = ((7 - i) * a0 + i * a1) / 7;
        } else {
          for (uint32_t i = 1; i < 5; ++i) palette[i + 1] = ((5 - i) * a0 + i * a1) / 5;
          palette[6] = 0, palette[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i)
          if (palette[(bits >> (3 * i)) & 7] < 128) return true;
        break;
      }
      default: return false;
    }
  }
  return false;
}

}  // namespace fh1
