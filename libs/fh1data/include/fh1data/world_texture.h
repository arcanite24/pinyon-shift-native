// Colorado's world textures: `_0x<ID>.bix` (header and mip chain) with its
// `_0x<ID>_B.bix` (the base level), or a CAFF `_0x<ID>.bin`. Both carry the
// console's texture format word and hold tiled DXT1/3/5 or DXN blocks laid
// out as .xds levels, the last region trimmed (xds_trimmed_regions).
// Spec: WORLD-MATERIALS (horizon1-recomp docs/formats/WORLD_MATERIALS.md,
// "Texture payloads").
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "fh1data/xds.h"

namespace fh1 {

// The format word's fields: bits 0-5 the texel format, 6-7 the endian
// swap, 8 tiled, 9-14 the sign of x, y, z (3 on all three: gamma).
struct WorldTextureFormat {
  uint32_t word = 0;
  uint32_t format = 0;
  uint32_t endian = 0;
  bool tiled = false;
  bool gamma = false;  // sample as sRGB (probable)
};
WorldTextureFormat world_texture_format(uint32_t word);

struct WorldTexture {
  WorldTextureFormat format;
  uint32_t width = 0, height = 0, levels = 0;
  bool caff = false;
  XdsBlocks blocks;  // levels as linear blocks, base first
};

// The header of a `.bix` (28 bytes, `BIX1`).
struct BixHeader {
  uint32_t width = 0, height = 0, levels = 0, format_word = 0;
  uint32_t total_bytes = 0, base_bytes = 0;  // payload: base + chain, base
};
bool parse_bix_header(std::span<const uint8_t> bix, BixHeader& out, std::string* error);

// The CAFF fields WORLD-MATERIALS gives: the format word at 0x1A8, width and
// height (u16) at 0x1B4 and 0x1B6, the level count (u8) at 0x1C0, texels
// from 0x2B0 to the end.
struct CaffHeader {
  uint32_t width = 0, height = 0, levels = 0, format_word = 0;
  size_t payload_offset = 0x2B0;
};
bool parse_caff_header(std::span<const uint8_t> bin, CaffHeader& out, std::string* error);

// The texture's XdsHeader (tiled, packed mips, the width as the pitch, the
// identity swizzle).
XdsHeader world_xds_header(uint32_t width, uint32_t height, uint32_t levels, uint32_t format_word);

// Decodes a .bix pair (header-and-chain file, base file) or a CAFF .bin.
// `exact` (optional) is set when the files' sizes equal the layout's.
bool decode_bix(std::span<const uint8_t> bix, std::span<const uint8_t> base, WorldTexture& out, std::string* error,
                bool* exact = nullptr);
bool decode_caff(std::span<const uint8_t> bin, WorldTexture& out, std::string* error, bool* exact = nullptr);

// Whether the texture's base level has any texel with alpha under 128: DXT3
// or DXT5 alpha, or a DXT1 block in its three-colour mode using index 3.
bool world_texture_has_cutout(const WorldTexture& texture);

}  // namespace fh1
