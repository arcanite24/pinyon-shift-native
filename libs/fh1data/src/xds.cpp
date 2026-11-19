#include "fh1data/xds.h"

#include <algorithm>
#include <array>
#include <bit>

#include "fh1data/bytes.h"

namespace fh1 {
namespace {

constexpr size_t kHeaderSize = 0x34;
constexpr size_t kFetchOffset = 0x1C;

enum class Kind { k8888, kDxt1, kDxt3, kDxt5, kDxn, kDxt3a, kDxt5a, k8, k88, k565, k4444, k1555 };

struct Format {
  uint32_t number;
  Kind kind;
  uint32_t block;        // texels per block edge (1 or 4)
  uint32_t block_bytes;
  const char* name;
};

// Xenos TextureFormat numbers. The _AS_16_16_16_16 variants store the same
// blocks as their base formats.
constexpr std::array<Format, 16> kFormats = {{
    {2, Kind::k8, 1, 1, "k_8"},
    {3, Kind::k1555, 1, 2, "k_1_5_5_5"},
    {4, Kind::k565, 1, 2, "k_5_6_5"},
    {6, Kind::k8888, 1, 4, "k_8_8_8_8"},
    {10, Kind::k88, 1, 2, "k_8_8"},
    {15, Kind::k4444, 1, 2, "k_4_4_4_4"},
    {18, Kind::kDxt1, 4, 8, "k_DXT1"},
    {19, Kind::kDxt3, 4, 16, "k_DXT2_3"},
    {20, Kind::kDxt5, 4, 16, "k_DXT4_5"},
    {49, Kind::kDxn, 4, 16, "k_DXN"},
    {50, Kind::k8888, 1, 4, "k_8_8_8_8_AS_16_16_16_16"},
    {51, Kind::kDxt1, 4, 8, "k_DXT1_AS_16_16_16_16"},
    {52, Kind::kDxt3, 4, 16, "k_DXT2_3_AS_16_16_16_16"},
    {53, Kind::kDxt5, 4, 16, "k_DXT4_5_AS_16_16_16_16"},
    {58, Kind::kDxt3a, 4, 8, "k_DXT3A"},
    {59, Kind::kDxt5a, 4, 8, "k_DXT5A"},
}};

const Format* find_format(uint32_t number) {
  for (const Format& f : kFormats)
    if (f.number == number) return &f;
  return nullptr;
}

// Byte permutation masks for endian modes none, 8-in-16, 8-in-32, 16-in-32.
constexpr std::array<uint32_t, 4> kEndianMasks = {0, 1, 3, 2};

using Texel = std::array<uint8_t, 4>;

uint32_t log2_ceil(uint32_t v) { return v <= 1 ? 0 : uint32_t(std::bit_width(v - 1)); }
uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

void bc1_palette(const uint8_t* block, bool allow_transparent, std::array<Texel, 4>& palette) {
  const uint32_t c0 = load_le16(block), c1 = load_le16(block + 2);
  auto expand = [](uint32_t c) {
    const uint32_t r = c >> 11, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return Texel{uint8_t(r << 3 | r >> 2), uint8_t(g << 2 | g >> 4), uint8_t(b << 3 | b >> 2), 255};
  };
  palette[0] = expand(c0);
  palette[1] = expand(c1);
  const bool four = c0 > c1 || !allow_transparent;
  for (int c = 0; c < 3; ++c) {
    const uint32_t a = palette[0][c], b = palette[1][c];
    palette[2][c] = uint8_t(four ? (2 * a + b + 1) / 3 : (a + b + 1) / 2);
    palette[3][c] = uint8_t(four ? (a + 2 * b + 1) / 3 : 0);
  }
  palette[2][3] = 255;
  palette[3][3] = four ? 255 : 0;
}

// An 8-byte interpolated (BC4-style) channel block.
void interpolated_channel(const uint8_t* block, std::array<uint8_t, 16>& out) {
  const uint32_t a0 = block[0], a1 = block[1];
  std::array<uint8_t, 8> p{uint8_t(a0), uint8_t(a1)};
  if (a0 > a1) {
    for (uint32_t i = 1; i < 7; ++i) p[i + 1] = uint8_t(((7 - i) * a0 + i * a1 + 3) / 7);
  } else {
    for (uint32_t i = 1; i < 5; ++i) p[i + 1] = uint8_t(((5 - i) * a0 + i * a1 + 2) / 5);
    p[6] = 0;
    p[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(block[2 + i]) << (8 * i);
  for (int t = 0; t < 16; ++t) out[t] = p[(bits >> (3 * t)) & 7];
}

// An 8-byte explicit 4-bit channel block.
void explicit_channel(const uint8_t* block, std::array<uint8_t, 16>& out) {
  for (int t = 0; t < 16; ++t) out[t] = uint8_t(((block[t / 2] >> (4 * (t & 1))) & 0xF) * 17);
}

// Decodes one block, already in host (little-endian) byte order, to XYZW.
void decode_block(Kind kind, const uint8_t* b, std::array<Texel, 16>& texels) {
  std::array<uint8_t, 16> ch0, ch1;
  switch (kind) {
    case Kind::k8888:
      texels[0] = {b[0], b[1], b[2], b[3]};
      return;
    case Kind::k8:
      texels[0] = {b[0], b[0], b[0], b[0]};
      return;
    case Kind::k88:
      texels[0] = {b[0], b[1], 0, 255};
      return;
    case Kind::k565: {
      const uint32_t v = load_le16(b);
      const uint32_t x = v & 0x1F, y = (v >> 5) & 0x3F, z = v >> 11;
      texels[0] = {uint8_t(x << 3 | x >> 2), uint8_t(y << 2 | y >> 4), uint8_t(z << 3 | z >> 2), 255};
      return;
    }
    case Kind::k4444: {
      const uint32_t v = load_le16(b);
      texels[0] = {uint8_t((v & 0xF) * 17), uint8_t(((v >> 4) & 0xF) * 17),
                   uint8_t(((v >> 8) & 0xF) * 17), uint8_t((v >> 12) * 17)};
      return;
    }
    case Kind::k1555: {
      const uint32_t v = load_le16(b);
      auto e = [](uint32_t c) { return uint8_t(c << 3 | c >> 2); };
      texels[0] = {e(v & 0x1F), e((v >> 5) & 0x1F), e((v >> 10) & 0x1F), uint8_t(v >> 15 ? 255 : 0)};
      return;
    }
    case Kind::kDxt3a:
    case Kind::kDxt5a:
      // One-channel formats: the value in every component.
      kind == Kind::kDxt3a ? explicit_channel(b, ch0) : interpolated_channel(b, ch0);
      for (int t = 0; t < 16; ++t) texels[t] = {ch0[t], ch0[t], ch0[t], ch0[t]};
      return;
    case Kind::kDxn:
      // Two interpolated channels (X then Y), as BC5.
      interpolated_channel(b, ch0);
      interpolated_channel(b + 8, ch1);
      for (int t = 0; t < 16; ++t) texels[t] = {ch0[t], ch1[t], 0, 255};
      return;
    case Kind::kDxt1:
    case Kind::kDxt3:
    case Kind::kDxt5: {
      const bool alpha_block = kind != Kind::kDxt1;
      const uint8_t* colors = b + (alpha_block ? 8 : 0);
      std::array<Texel, 4> palette;
      bc1_palette(colors, !alpha_block, palette);
      const uint32_t indices = load_le32(colors + 4);
      if (kind == Kind::kDxt3) explicit_channel(b, ch0);
      if (kind == Kind::kDxt5) interpolated_channel(b, ch0);
      for (int t = 0; t < 16; ++t) {
        texels[t] = palette[(indices >> (2 * t)) & 3];
        if (alpha_block) texels[t][3] = ch0[t];
      }
      return;
    }
  }
}

struct Level {
  uint32_t width_blocks, height_blocks;
  uint32_t width, height;  // texels
  size_t start;            // byte offset of its storage in the payload
  uint32_t pitch;          // storage pitch in blocks
  uint32_t x, y;           // block offset inside the storage (packed tail)
};

// The first level stored in the packed mip tail.
uint32_t packed_level(uint32_t w, uint32_t h) {
  const uint32_t size = log2_ceil(std::min(w, h));
  return size > 4 ? size - 4 : 0;
}

// Block offset of a packed level inside the tail.
void packed_offset(uint32_t w, uint32_t h, uint32_t level, uint32_t block, uint32_t& x,
                   uint32_t& y) {
  const uint32_t lw = log2_ceil(w), lh = log2_ceil(h);
  const uint32_t base = std::min(lw, lh) > 4 ? std::min(lw, lh) - 4 : 0;
  const uint32_t packed = level - base;
  if (packed < 3) {
    x = lw > lh ? 0 : 16u >> packed;
    y = lw > lh ? 16u >> packed : 0;
  } else {
    const uint32_t offset = (1u << ((lw > lh ? lw : lh) - base)) >> (packed - 2);
    x = lw > lh ? offset : 0;
    y = lw > lh ? 0 : offset;
  }
  x /= block;
  y /= block;
}

// Every level's placement; returns the payload size the layout needs.
size_t layout(const XdsHeader& h, const Format& f, std::vector<Level>& levels) {
  const uint32_t bt = f.block;
  const uint32_t log2 = uint32_t(std::bit_width(f.block_bytes) - 1);
  const uint32_t tail = h.packed_mips ? packed_level(h.width, h.height) : h.levels;
  const uint32_t base_pitch = align_up(std::max(std::max(h.pitch, h.width) / bt, 1u), 32);
  const size_t base_size =
      align_up(uint32_t(size_t(base_pitch) * align_up((h.height + bt - 1) / bt, 32) << log2), 4096);
  std::vector<std::pair<size_t, uint32_t>> starts{{0, base_pitch}};
  size_t at = base_size;
  for (uint32_t stored = 1; stored <= std::min(h.levels - 1, tail); ++stored) {
    const uint32_t pitch =
        align_up(std::max(((1u << log2_ceil(h.width)) >> stored) / bt, 1u), 32);
    const uint32_t rows =
        align_up(std::max(((1u << log2_ceil(h.height)) >> stored) / bt, 1u), 32);
    starts.emplace_back(at, pitch);
    at += align_up(uint32_t(size_t(pitch) * rows << log2), 4096);
  }
  levels.clear();
  for (uint32_t level = 0; level < h.levels; ++level) {
    const uint32_t stored = std::min(level, tail);
    Level lv{};
    if (level >= tail && h.packed_mips) packed_offset(h.width, h.height, level, bt, lv.x, lv.y);
    lv.start = starts[stored].first;
    lv.pitch = starts[stored].second;
    lv.width = std::max(h.width >> level, 1u);
    lv.height = std::max(h.height >> level, 1u);
    lv.width_blocks = (lv.width + bt - 1) / bt;
    lv.height_blocks = (lv.height + bt - 1) / bt;
    levels.push_back(lv);
  }
  return at;
}

bool decode_level(std::span<const uint8_t> payload, const XdsHeader& h, const Format& f,
                  const Level& lv, RgbaImage& image) {
  const uint32_t mask = kEndianMasks[h.endian & 3];
  const uint32_t log2 = uint32_t(std::bit_width(f.block_bytes) - 1);
  image.width = lv.width;
  image.height = lv.height;
  image.rgba.assign(size_t(lv.width) * lv.height * 4, 0);
  std::array<uint8_t, 16> block;
  std::array<Texel, 16> texels;
  const uint32_t texel_count = f.block * f.block;
  for (uint32_t by = 0; by < lv.height_blocks; ++by) {
    for (uint32_t bx = 0; bx < lv.width_blocks; ++bx) {
      const size_t offset =
          lv.start + (h.tiled ? size_t(xenos_tiled_offset_2d(int32_t(bx + lv.x), int32_t(by + lv.y),
                                                             lv.pitch, log2))
                              : (size_t(by + lv.y) * lv.pitch + bx + lv.x) * f.block_bytes);
      if (offset + f.block_bytes > payload.size()) return false;
      for (uint32_t i = 0; i < f.block_bytes; ++i) block[i] = payload[offset + (i ^ mask)];
      decode_block(f.kind, block.data(), texels);
      for (uint32_t i = 0; i < texel_count; ++i) {
        const uint32_t tx = bx * f.block + i % f.block;
        const uint32_t ty = by * f.block + i / f.block;
        if (tx >= lv.width || ty >= lv.height) continue;
        uint8_t* out = &image.rgba[(size_t(ty) * lv.width + tx) * 4];
        for (int c = 0; c < 4; ++c) {
          const uint32_t sel = (h.swizzle >> (3 * c)) & 7;
          out[c] = sel < 4 ? texels[i][sel] : (sel == 5 ? 255 : 0);
        }
      }
    }
  }
  return true;
}

}  // namespace

int32_t xenos_tiled_offset_2d(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_block_bytes) {
  pitch = (pitch + 31) & ~31u;
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_block_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_block_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

const char* xenos_format_name(uint32_t format) {
  const Format* f = find_format(format);
  return f ? f->name : "unsupported";
}

bool parse_xds_header(std::span<const uint8_t> data, XdsHeader& h, std::string* error) {
  if (data.size() < kHeaderSize || load_be32(data.data()) != 3) {
    if (error) *error = "not an .xds texture (header)";
    return false;
  }
  std::array<uint32_t, 6> f;
  for (size_t i = 0; i < 6; ++i) f[i] = load_be32(&data[kFetchOffset + 4 * i]);
  if ((f[0] & 3) != 2) {
    if (error) *error = "no texture fetch constant";
    return false;
  }
  h.dimension = XdsDimension((f[5] >> 9) & 3);
  h.format = f[1] & 0x3F;
  h.endian = (f[1] >> 6) & 3;
  h.tiled = (f[0] >> 31) != 0;
  h.pitch = ((f[0] >> 22) & 0x1FF) * 32;
  h.swizzle = (f[3] >> 1) & 0xFFF;
  h.levels = ((f[4] >> 6) & 0xF) + 1;
  h.packed_mips = ((f[5] >> 11) & 1) != 0;
  if (h.dimension == XdsDimension::k2D) {
    h.width = (f[2] & 0x1FFF) + 1;
    h.height = ((f[2] >> 13) & 0x1FFF) + 1;
  } else if (h.dimension == XdsDimension::k1D) {
    h.width = (f[2] & 0xFFFFFF) + 1;
    h.height = 1;
  } else {
    // 3D and cube: 11-bit width and height (depth/faces above), not decoded.
    h.width = (f[2] & 0x7FF) + 1;
    h.height = ((f[2] >> 11) & 0x7FF) + 1;
  }
  return true;
}

XdsResult decode_xds(std::span<const uint8_t> data, bool all_levels) {
  XdsResult r;
  if (!parse_xds_header(data, r.header, &r.error)) return r;
  const XdsHeader& h = r.header;
  if (h.dimension != XdsDimension::k2D) {
    r.status = XdsStatus::kUnsupported;
    r.error = h.dimension == XdsDimension::kCube ? "cube texture" :
              h.dimension == XdsDimension::k3D ? "3D texture" : "1D texture";
    return r;
  }
  const Format* f = find_format(h.format);
  if (!f) {
    r.status = XdsStatus::kUnsupported;
    r.error = "format " + std::to_string(h.format);
    return r;
  }
  std::vector<Level> levels;
  const size_t needed = layout(h, *f, levels);
  const auto payload = data.subspan(kHeaderSize);
  RgbaImage base;
  if (!decode_level(payload, h, *f, levels[0], base)) {
    r.error = "data shorter than the base level";
    return r;
  }
  r.levels.push_back(std::move(base));
  r.status = XdsStatus::kOk;
  if (!all_levels || h.levels == 1) return r;
  if (!h.tiled) {
    r.status = XdsStatus::kBaseOnly;
    r.error = "mips of linear textures are not laid out";
    return r;
  }
  if (payload.size() < needed) {
    r.status = XdsStatus::kBaseOnly;
    r.error = "data shorter than the mip layout";
    return r;
  }
  for (size_t i = 1; i < levels.size(); ++i) {
    RgbaImage mip;
    if (!decode_level(payload, h, *f, levels[i], mip)) {
      r.status = XdsStatus::kBaseOnly;
      r.error = "mip " + std::to_string(i) + " outside the data";
      return r;
    }
    r.levels.push_back(std::move(mip));
  }
  return r;
}

}  // namespace fh1
