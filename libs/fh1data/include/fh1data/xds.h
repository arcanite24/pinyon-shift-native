// FH1 `.xds` textures.
//
// A 0x34-byte header (first u32 = 3) whose last 24 bytes, at 0x1C, are the
// Xenos texture fetch constant (format, size, pitch, tiling, endianness,
// swizzle, mip count, packed mips), then the base level and the mip levels.
// Levels are stored in 32x32-block tiles with every block byte-swapped by
// the endian mode; levels whose shorter side is 16 texels or less share one
// tile, the packed mip tail.
// Spec: site/findings/files.toml (XDS texture header, car body atlases);
// layout as in horizon1-recomp's tools/fh1_xds.py (feat/ac-car-inspect)
// and src/ui/hostui/xds_texture.cpp.
#pragma once

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
};

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
