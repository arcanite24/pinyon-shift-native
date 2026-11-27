#include "fh1data/xds.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

#include "fh1data/bytes.h"

namespace fh1 {
namespace {

constexpr size_t kHeaderSize = 0x34;
constexpr size_t kFetchOffset = 0x1C;

enum class Kind { k8888, kDxt1, kDxt3, kDxt5, kDxn, kDxt3a, kDxt5a };

struct Format {
  uint32_t number;
  Kind kind;
  uint32_t block;        // texels per block edge (1 or 4)
  uint32_t block_bytes;
  const char* name;
};

// Xenos TextureFormat numbers. The _AS_16_16_16_16 variants store the same
// blocks as their base formats.
constexpr std::array<Format, 11> kFormats = {{
    {6, Kind::k8888, 1, 4, "k_8_8_8_8"},
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
  uint32_t region = 0;     // which storage region: the level, or the tail's
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
  // A texture that is all tail (its shorter side, rounded up to a power of
  // two, is 16 texels or less) takes its one region's pitch and height from
  // the size rounded up to powers of two, not from the header pitch.
  const bool all_tail = h.packed_mips && tail == 0;
  const uint32_t base_pitch =
      all_tail ? align_up(std::max((1u << log2_ceil(h.width)) / bt, 1u), 32)
               : align_up(std::max(std::max(h.pitch, h.width) / bt, 1u), 32);
  const uint32_t base_rows =
      all_tail ? align_up(std::max((1u << log2_ceil(h.height)) / bt, 1u), 32)
               : align_up((h.height + bt - 1) / bt, 32);
  const size_t base_size = align_up(uint32_t(size_t(base_pitch) * base_rows << log2), 4096);
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
    lv.region = stored;
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

// Copies one level's blocks into row-major order, byte-swapped to host order.
bool copy_level_blocks(std::span<const uint8_t> payload, const XdsHeader& h, const Format& f,
                       const Level& lv, std::vector<uint8_t>& out) {
  const uint32_t mask = kEndianMasks[h.endian & 3];
  const uint32_t bb = f.block_bytes;
  const uint32_t log2 = uint32_t(std::bit_width(bb) - 1);
  const size_t payload_sz = payload.size();
  const uint8_t* src_base = payload.data();
  out.resize(size_t(lv.width_blocks) * lv.height_blocks * bb);

  auto swap_copy = [&](uint8_t* dst, const uint8_t* src) {
    if (mask == 0u) {
      std::memcpy(dst, src, bb);
    } else if (mask == 1u) {
      // 8-in-16 byte swap: swap adjacent byte pairs using 64-bit/32-bit SWAR.
      if (bb == 16u) {
        uint64_t a, b;
        std::memcpy(&a, src, 8);
        std::memcpy(&b, src + 8, 8);
        a = ((a & 0x00FF00FF00FF00FFull) << 8) | ((a >> 8) & 0x00FF00FF00FF00FFull);
        b = ((b & 0x00FF00FF00FF00FFull) << 8) | ((b >> 8) & 0x00FF00FF00FF00FFull);
        std::memcpy(dst, &a, 8);
        std::memcpy(dst + 8, &b, 8);
      } else if (bb == 8u) {
        uint64_t a;
        std::memcpy(&a, src, 8);
        a = ((a & 0x00FF00FF00FF00FFull) << 8) | ((a >> 8) & 0x00FF00FF00FF00FFull);
        std::memcpy(dst, &a, 8);
      } else {
        for (uint32_t i = 0; i < bb; i += 2) {
          dst[i] = src[i + 1];
          dst[i + 1] = src[i];
        }
      }
    } else if (mask == 3u) {
      // 8-in-32 byte swap (full 32-bit endian swap).
      for (uint32_t i = 0; i < bb; i += 4) {
        const uint32_t v = load_be32(src + i);
        std::memcpy(dst + i, &v, 4);
      }
    } else {
      // 16-in-32 word swap (mask == 2).
      for (uint32_t i = 0; i < bb; i += 4) {
        uint32_t v = load_le32(src + i);
        v = (v << 16) | (v >> 16);
        std::memcpy(dst + i, &v, 4);
      }
    }
  };

  if (h.tiled) {
    const uint32_t pitch_aligned = (lv.pitch + 31u) & ~31u;
    const uint32_t pitch_macro = pitch_aligned >> 5;
    for (uint32_t by = 0; by < lv.height_blocks; ++by) {
      const uint32_t ty = by + lv.y;
      const uint32_t y_macro_base = (ty >> 5) * pitch_macro;
      const uint32_t y_micro_base = (ty & 0xEu) << 2;
      const uint32_t y_bit0 = (ty & 1u) << 4;
      const uint32_t y_bit4 = (ty & 16u) << 7;
      const uint32_t y_bit3 = (ty & 8u) >> 2;
      uint8_t* dst_row = out.data() + size_t(by) * lv.width_blocks * bb;
      for (uint32_t bx = 0; bx < lv.width_blocks; ++bx) {
        const uint32_t tx = bx + lv.x;
        const uint32_t macro = ((tx >> 5) + y_macro_base) << (log2 + 7u);
        const uint32_t micro = ((tx & 7u) + y_micro_base) << log2;
        const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xFu) + y_bit0;
        const size_t tiled_off =
            lv.start + size_t(((offset & ~0x1FFu) << 3) + y_bit4 + ((offset & 0x1C0u) << 2) +
                              (((y_bit3 + (tx >> 3)) & 3u) << 6) + (offset & 0x3Fu));
        if (tiled_off + bb > payload_sz) return false;
        swap_copy(dst_row + size_t(bx) * bb, src_base + tiled_off);
      }
    }
  } else {
    for (uint32_t by = 0; by < lv.height_blocks; ++by) {
      const size_t row_off = lv.start + (size_t(by + lv.y) * lv.pitch + lv.x) * bb;
      if (row_off + size_t(lv.width_blocks) * bb > payload_sz) return false;
      uint8_t* dst_row = out.data() + size_t(by) * lv.width_blocks * bb;
      if (mask == 0u) {
        std::memcpy(dst_row, src_base + row_off, size_t(lv.width_blocks) * bb);
      } else {
        for (uint32_t bx = 0; bx < lv.width_blocks; ++bx) {
          swap_copy(dst_row + size_t(bx) * bb, src_base + row_off + size_t(bx) * bb);
        }
      }
    }
  }
  return true;
}

}  // namespace

bool extract_xds_blocks(std::span<const uint8_t> data, XdsBlocks& out, std::string* error) {
  out = XdsBlocks{};
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (!parse_xds_header(data, out.header, error)) return false;
  const XdsHeader& h = out.header;
  if (h.dimension != XdsDimension::k2D) return fail("not a 2D texture");
  const Format* f = find_format(h.format);
  if (!f) return fail("format " + std::to_string(h.format));
  // What each stored channel (X, Y, Z, W after decoding) is in the block
  // format's sampled result (0-3 = R, G, B, A; 4 = 0; 5 = 1).
  std::array<uint8_t, 4> channel{0, 1, 2, 3};
  switch (f->kind) {
    case Kind::k8888: out.format = XdsBlockFormat::kRgba8; break;
    case Kind::kDxt1: out.format = XdsBlockFormat::kBc1; break;
    case Kind::kDxt3: out.format = XdsBlockFormat::kBc2; break;
    case Kind::kDxt5: out.format = XdsBlockFormat::kBc3; break;
    case Kind::kDxn:
      out.format = XdsBlockFormat::kBc5;
      channel = {0, 1, 4, 5};
      break;
    case Kind::kDxt5a:
      out.format = XdsBlockFormat::kBc4;
      channel = {0, 0, 0, 0};
      break;
    case Kind::kDxt3a: return fail("DXT3A has no block format equivalent");
  }
  for (int c = 0; c < 4; ++c) {
    const uint32_t sel = (h.swizzle >> (3 * c)) & 7;
    out.swizzle[size_t(c)] = sel < 4 ? channel[sel] : (sel == 5 ? 5 : 4);
  }
  out.block_bytes = f->block_bytes;
  out.block_texels = f->block;
  std::vector<Level> levels;
  out.layout_bytes = layout(h, *f, levels);
  const auto payload = data.subspan(kHeaderSize);
  // Mips of a linear texture are not laid out: keep its base level only.
  const size_t count = h.tiled && payload.size() >= out.layout_bytes ? levels.size() : 1;
  for (size_t i = 0; i < count; ++i) {
    XdsBlockLevel level;
    level.width = levels[i].width;
    level.height = levels[i].height;
    if (!copy_level_blocks(payload, h, *f, levels[i], level.blocks))
      return fail("level " + std::to_string(i) + " outside the data");
    out.levels.push_back(std::move(level));
  }
  return true;
}

size_t xds_layout_bytes(const XdsHeader& h) {
  const Format* f = find_format(h.format);
  if (!f || h.dimension != XdsDimension::k2D) return 0;
  std::vector<Level> levels;
  return layout(h, *f, levels);
}

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
  if (h.dimension == XdsDimension::k2D || h.dimension == XdsDimension::kCube) {
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
  r.layout_bytes = needed;
  r.payload_bytes = payload.size();
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

namespace {

// The block format and the sampled-channel map of a format, as for .xds.
bool block_format(const XdsHeader& h, const Format& f, XdsBlocks& out, std::string* error) {
  std::array<uint8_t, 4> channel{0, 1, 2, 3};
  switch (f.kind) {
    case Kind::k8888: out.format = XdsBlockFormat::kRgba8; break;
    case Kind::kDxt1: out.format = XdsBlockFormat::kBc1; break;
    case Kind::kDxt3: out.format = XdsBlockFormat::kBc2; break;
    case Kind::kDxt5: out.format = XdsBlockFormat::kBc3; break;
    case Kind::kDxn:
      out.format = XdsBlockFormat::kBc5;
      channel = {0, 1, 4, 5};
      break;
    case Kind::kDxt5a:
      out.format = XdsBlockFormat::kBc4;
      channel = {0, 0, 0, 0};
      break;
    case Kind::kDxt3a:
      if (error) *error = "DXT3A has no block format equivalent";
      return false;
  }
  for (int c = 0; c < 4; ++c) {
    const uint32_t sel = (h.swizzle >> (3 * c)) & 7;
    out.swizzle[size_t(c)] = sel < 4 ? channel[sel] : (sel == 5 ? 5 : 4);
  }
  out.block_bytes = f.block_bytes;
  out.block_texels = f.block;
  return true;
}

}  // namespace

std::vector<size_t> xds_trimmed_regions(const XdsHeader& h) {
  const Format* f = find_format(h.format);
  if (!f || h.dimension != XdsDimension::k2D || !h.tiled) return {};
  std::vector<Level> levels;
  const size_t total = layout(h, *f, levels);
  const uint32_t log2 = uint32_t(std::bit_width(f->block_bytes) - 1);
  // Region starts, in order; each runs to the next, the last to `total`.
  std::vector<size_t> starts;
  for (const Level& lv : levels)
    if (starts.size() <= lv.region) starts.push_back(lv.start);
  std::vector<size_t> sizes(starts.size());
  for (size_t r = 0; r < starts.size(); ++r) sizes[r] = (r + 1 < starts.size() ? starts[r + 1] : total) - starts[r];
  // The last region: the tiled bytes its levels touch, rounded to 4,096.
  size_t touched = 0;
  for (const Level& lv : levels) {
    if (lv.region + 1 != starts.size()) continue;
    for (uint32_t by = 0; by < lv.height_blocks; ++by)
      for (uint32_t bx = 0; bx < lv.width_blocks; ++bx)
        touched = std::max(touched, size_t(xenos_tiled_offset_2d(int32_t(bx + lv.x), int32_t(by + lv.y),
                                                                 lv.pitch, log2)) + f->block_bytes);
  }
  sizes.back() = std::min(sizes.back(), (touched + 4095) / 4096 * 4096);
  return sizes;
}

bool extract_region_blocks(const XdsHeader& h, std::span<const std::span<const uint8_t>> regions, XdsBlocks& out,
                           std::string* error) {
  out = XdsBlocks{};
  out.header = h;
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (h.dimension != XdsDimension::k2D || !h.tiled) return fail("not a tiled 2D texture");
  const Format* f = find_format(h.format);
  if (!f) return fail("format " + std::to_string(h.format));
  if (!block_format(h, *f, out, error)) return false;
  std::vector<Level> levels;
  out.layout_bytes = layout(h, *f, levels);
  size_t region_start = 0;
  uint32_t region = 0;
  for (size_t i = 0; i < levels.size(); ++i) {
    const Level& lv = levels[i];
    if (lv.region != region) region = lv.region, region_start = lv.start;
    if (lv.region >= regions.size()) break;
    Level local = lv;
    local.start = lv.start - region_start;
    XdsBlockLevel level;
    level.width = lv.width;
    level.height = lv.height;
    if (!copy_level_blocks(regions[lv.region], h, *f, local, level.blocks)) {
      if (i == 0) return fail("the base level runs outside its data");
      break;  // keep the levels before it
    }
    out.levels.push_back(std::move(level));
  }
  if (out.levels.empty()) return fail("no level");
  return true;
}

bool decode_xpr_cubemap(std::span<const uint8_t> data, XprCubemap& out, std::string* error) {
  out = XprCubemap{};
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (data.size() < 32 || std::memcmp(data.data(), "XPR2", 4) != 0) return fail("not an XPR2 file");
  const uint32_t header_size = load_be32(data.data() + 4);
  const uint32_t res_count = load_be32(data.data() + 12);
  if (res_count == 0 || 12 + size_t(header_size) > data.size()) return fail("truncated XPR2 header");

  size_t xds_off = 0;
  bool found = false;
  for (uint32_t i = 0; i < res_count; ++i) {
    const size_t entry = 16 + size_t(i) * 16;
    if (entry + 16 > data.size()) break;
    if (std::memcmp(data.data() + entry, "TXCM", 4) == 0) {
      xds_off = 12 + size_t(load_be32(data.data() + entry + 4));
      found = true;
      break;
    }
  }
  if (!found || xds_off + kHeaderSize > data.size()) return fail("no TXCM cube resource in XPR2");

  XdsHeader hdr;
  if (!parse_xds_header(data.subspan(xds_off, kHeaderSize), hdr, error)) return false;
  if (hdr.dimension != XdsDimension::kCube || hdr.width == 0 || hdr.width != hdr.height)
    return fail("XPR2 TXCM is not a square cubemap");
  if (hdr.format != 6 && hdr.format != 50) return fail("unsupported XPR2 cubemap format");

  const uint32_t size = hdr.width;
  const uint32_t pitch = align_up(size, 32);
  const uint32_t h_aligned = align_up(size, 32);
  const size_t face_bytes = size_t(pitch) * h_aligned * 4;
  const size_t data_start = 12 + size_t(header_size);
  if (data_start + face_bytes * 6 > data.size()) return fail("XPR2 cubemap face data truncated");

  // Compute exact inverse of LogLuv forward matrix M (SHOWROOM_LIGHTING.md):
  //   X' = 0.2209 R + 0.1138 G + 0.0102 B
  //   Y  = 0.3390 R + 0.6780 G + 0.1130 B
  //   Z' = 0.4184 R + 0.7319 G + 0.2969 B
  const double M[3][3] = {
      {0.2209, 0.1138, 0.0102},
      {0.3390, 0.6780, 0.1130},
      {0.4184, 0.7319, 0.2969},
  };
  double inv[3][3]{};
  {
    const double c00 = M[1][1] * M[2][2] - M[1][2] * M[2][1];
    const double c01 = -(M[1][0] * M[2][2] - M[1][2] * M[2][0]);
    const double c02 = M[1][0] * M[2][1] - M[1][1] * M[2][0];
    const double det = M[0][0] * c00 + M[0][1] * c01 + M[0][2] * c02;
    const double id = 1.0 / det;
    inv[0][0] = c00 * id;
    inv[1][0] = c01 * id;
    inv[2][0] = c02 * id;
    inv[0][1] = -(M[0][1] * M[2][2] - M[0][2] * M[2][1]) * id;
    inv[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) * id;
    inv[2][1] = -(M[0][0] * M[2][1] - M[0][1] * M[2][0]) * id;
    inv[0][2] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) * id;
    inv[1][2] = -(M[0][0] * M[1][2] - M[0][2] * M[1][0]) * id;
    inv[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) * id;
  }

  out.size = size;
  for (uint32_t face = 0; face < 6; ++face) {
    const uint8_t* src = data.data() + data_start + size_t(face) * face_bytes;
    auto& dst = out.faces[face];
    dst.resize(size_t(size) * size);
    for (uint32_t y = 0; y < size; ++y) {
      for (uint32_t x = 0; x < size; ++x) {
        const size_t off = hdr.tiled ? size_t(xenos_tiled_offset_2d(int32_t(x), int32_t(y), pitch, 2))
                                     : (size_t(y) * size + x) * 4;
        const uint8_t* b = src + off;
        // XPR2 ARGB8888 big-endian bytes [A, R, G, B] -> shader (r, g, b, a) = bytes (1, 2, 3, 0).
        const double r = double(b[1]) / 255.0;
        const double g = std::max(double(b[2]) / 255.0, 1e-6);
        const double bl = double(b[3]) / 255.0;
        const double a = double(b[0]) / 255.0;
        const double Le = 255.0 * bl + a;
        const double Y = std::exp2((Le - 127.0) * 0.5);
        const double Zp = Y / g;
        const double Xp = r * Zp;
        const float R = float(std::max(0.0, inv[0][0] * Xp + inv[0][1] * Y + inv[0][2] * Zp));
        const float G = float(std::max(0.0, inv[1][0] * Xp + inv[1][1] * Y + inv[1][2] * Zp));
        const float B = float(std::max(0.0, inv[2][0] * Xp + inv[2][1] * Y + inv[2][2] * Zp));
        dst[size_t(y) * size + x] = {R, G, B};
      }
    }
  }
  return true;
}

}  // namespace fh1
