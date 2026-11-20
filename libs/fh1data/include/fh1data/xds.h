// FH1 `.xds` textures.
//
// A 0x34-byte header (first u32 = 3) whose last 24 bytes, at 0x1C, are the
// Xenos texture fetch constant (format, size, pitch, tiling, endianness,
// swizzle, mip count, packed mips), then the base level and the mip levels.
// Levels are stored in 32x32-block tiles with every block byte-swapped by
// the endian mode; levels whose shorter side is 16 texels or less share one
// tile, the packed mip tail.
// Spec: FORMAT-CARS (.xds: header, fetch constant, layout, packed tail);
// site/findings/files.toml (XDS texture header, car body atlases);
// layout as in horizon1-recomp's tools/fh1_xds.py (feat/ac-car-inspect)
// and src/ui/hostui/xds_texture.cpp.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fh1 {

enum class XdsDimension : uint32_t { k1D = 0, k2D = 1, k3D = 2, kCube = 3 };

struct XdsHeader {
  uint32_t format = 0;       // Xenos texture format number
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch = 0;        // base-level row pitch in texels
  bool tiled = false;
  uint32_t endian = 0;       // 0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32
  uint32_t levels = 1;       // base plus mips
  bool packed_mips = false;
  XdsDimension dimension = XdsDimension::k2D;
  uint32_t swizzle = 0;      // 4 x 3-bit selectors for X, Y, Z, W
};

struct RgbaImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;  // tightly packed, straight alpha
};

enum class XdsStatus {
  kOk,           // every level decoded
  kBaseOnly,     // the base level decoded, the mips did not (or are not laid out)
  kUnsupported,  // a format or dimension this reader does not decode
  kFailed,       // not an .xds, or the data is shorter than its layout
};

struct XdsResult {
  XdsStatus status = XdsStatus::kFailed;
  XdsHeader header;
  std::vector<RgbaImage> levels;  // levels[0] is the base
  std::string error;
  size_t layout_bytes = 0;   // payload size the level layout needs (2D, known format)
  size_t payload_bytes = 0;  // bytes after the header
};

// A texture's levels as linear block data, for upload as a block-compressed
// image: rows of blocks, top row first, each block in host byte order.
enum class XdsBlockFormat : uint32_t { kRgba8 = 0, kBc1 = 1, kBc2 = 2, kBc3 = 3, kBc4 = 4, kBc5 = 5 };

struct XdsBlockLevel {
  uint32_t width = 0, height = 0;  // texels
  std::vector<uint8_t> blocks;
};

struct XdsBlocks {
  XdsHeader header;
  XdsBlockFormat format = XdsBlockFormat::kRgba8;
  // For each sampled component (R, G, B, A): the block format's component
  // it reads (0-3 = R, G, B, A), or 4 for zero and 5 for one. Applies the
  // fetch constant's swizzle.
  std::array<uint8_t, 4> swizzle{0, 1, 2, 3};
  uint32_t block_bytes = 0, block_texels = 0;
  size_t layout_bytes = 0;
  std::vector<XdsBlockLevel> levels;  // base first; linear textures: base only
};

// Untiles every level of a 2D texture into linear blocks.
bool extract_xds_blocks(std::span<const uint8_t> data, XdsBlocks& out, std::string* error);

// Colorado's world textures (WORLD-MATERIALS, "Layout and untiling"): the
// .xds layout, but the last region (the packed tail, or the last level when
// the chain stops before it) only as large as the tiled bytes its levels
// touch, rounded up to 4,096. The sizes of the regions, base first; empty
// for a linear or unknown texture.
std::vector<size_t> xds_trimmed_regions(const XdsHeader& header);
// Untiles each level from regions stored apart (regions[r] holds region r:
// the base, then each stored mip level, the packed tail last). Levels whose
// region is missing or short are dropped from the end; the base must fit.
bool extract_region_blocks(const XdsHeader& header, std::span<const std::span<const uint8_t>> regions,
                           XdsBlocks& out, std::string* error);

// Payload bytes the level layout needs (0 for an unknown format).
size_t xds_layout_bytes(const XdsHeader& header);

// Reads the header only. Returns false (with a reason) if it is not one.
bool parse_xds_header(std::span<const uint8_t> data, XdsHeader& header, std::string* error);

// Decodes the base level and, when `all_levels`, every mip level to RGBA8
// with the fetch constant's swizzle applied.
XdsResult decode_xds(std::span<const uint8_t> data, bool all_levels = true);

// Name of a Xenos texture format number, for reports.
const char* xenos_format_name(uint32_t format);

// Byte offset of block (x, y) in a Xenos 2D tiled surface `pitch` blocks wide.
int32_t xenos_tiled_offset_2d(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_block_bytes);

}  // namespace fh1
