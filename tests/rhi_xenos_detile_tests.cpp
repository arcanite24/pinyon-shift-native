// Hardware verification suite for GPU Compute Xenos 2D Texture Detiler:
// Verifies bit-exact parity against fh1::xenos_tiled_offset_2d + Xenos endian swap
// across Vulkan 1.3 and DirectX 12 for RGBA8, BC1, BC2, BC3, BC5, all 4 endian modes,
// non-32-aligned dimensions, packed mip tails, linear pass-through, and single-pass
// GPU atomic cutout alpha detection with 0 validation errors.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "fh1data/xds.h"
#include "rhi/xenos_detile_gpu.h"

namespace {

using namespace shift::rhi;

#define CHECK(cond, msg)                                                                  \
  do {                                                                                    \
    if (!(cond)) {                                                                        \
      std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));              \
      return false;                                                                       \
    }                                                                                     \
  } while (0)

uint32_t cpu_swap_word(uint32_t w, uint32_t xenos_endian) {
  switch (xenos_endian & 3u) {
    case 0u: return w;
    case 1u: return ((w & 0x00FF00FFu) << 8u) | ((w & 0xFF00FF00u) >> 8u);
    case 2u:
      return ((w & 0x000000FFu) << 24u) | ((w & 0x0000FF00u) << 8u) |
             ((w & 0x00FF0000u) >> 8u) | ((w & 0xFF000000u) >> 24u);
    case 3u: return (w << 16u) | (w >> 16u);
  }
  return w;
}

// Encodes linear little-endian blocks into Xenos 2D tiled (or linear) + endian-swapped source bytes.
std::vector<uint8_t> encode_xenos_source(const std::vector<uint8_t>& linear_blocks,
                                         const XenosDetileLevelDesc& lv) {
  const uint32_t bb = XenosDetileGpu::block_bytes(lv.format);
  const uint32_t l2 = XenosDetileGpu::log2_block_bytes(lv.format);
  const uint32_t pitch = lv.pitch_blocks ? lv.pitch_blocks : lv.width_blocks;
  const uint32_t words_per_block = bb / 4u;

  size_t max_end = lv.src_byte_offset + 64u;
  for (uint32_t by = 0; by < lv.height_blocks; ++by) {
    for (uint32_t bx = 0; bx < lv.width_blocks; ++bx) {
      const uint32_t sx = bx + lv.block_offset_x;
      const uint32_t sy = by + lv.block_offset_y;
      const size_t off = lv.src_byte_offset +
                         (lv.tiled
                              ? static_cast<size_t>(fh1::xenos_tiled_offset_2d(
                                    static_cast<int32_t>(sx), static_cast<int32_t>(sy), pitch, l2))
                              : static_cast<size_t>(sy * pitch + sx) * bb);
      max_end = std::max(max_end, off + bb);
    }
  }
  std::vector<uint8_t> tiled(max_end, 0xCCu);
  for (uint32_t by = 0; by < lv.height_blocks; ++by) {
    for (uint32_t bx = 0; bx < lv.width_blocks; ++bx) {
      const uint32_t sx = bx + lv.block_offset_x;
      const uint32_t sy = by + lv.block_offset_y;
      const size_t dst_off = lv.src_byte_offset +
                             (lv.tiled
                                  ? static_cast<size_t>(fh1::xenos_tiled_offset_2d(
                                        static_cast<int32_t>(sx), static_cast<int32_t>(sy), pitch, l2))
                                  : static_cast<size_t>(sy * pitch + sx) * bb);
      const size_t src_off = static_cast<size_t>(by * lv.width_blocks + bx) * bb;
      for (uint32_t w = 0; w < words_per_block; ++w) {
        uint32_t val = 0;
        std::memcpy(&val, linear_blocks.data() + src_off + w * 4u, 4u);
        // All 4 Xenos swap operations are involutions (applying twice restores original).
        const uint32_t swapped = cpu_swap_word(val, lv.xenos_endian);
        std::memcpy(tiled.data() + dst_off + w * 4u, &swapped, 4u);
      }
    }
  }
  return tiled;
}

bool run_case(XenosDetileGpu& detiler, const char* label, const XenosDetileLevelDesc& lv,
              const std::vector<uint8_t>& expected_linear, bool expected_cutout) {
  const std::vector<uint8_t> src = encode_xenos_source(expected_linear, lv);
  XenosDetileSyncResult res{};
  std::string err;
  if (!detiler.detile_sync(src, lv, res, err)) {
    std::fprintf(stderr, "FAIL [%s]: detile_sync error: %s\n", label, err.c_str());
    return false;
  }
  if (res.linear_blocks != expected_linear) {
    for (size_t i = 0; i < std::min(res.linear_blocks.size(), expected_linear.size()); ++i) {
      if (res.linear_blocks[i] != expected_linear[i]) {
        std::fprintf(stderr, "FAIL [%s]: byte mismatch at %zu (got 0x%02X expected 0x%02X)\n",
                     label, i, res.linear_blocks[i], expected_linear[i]);
        break;
      }
    }
    return false;
  }
  if (res.has_cutout != expected_cutout) {
    std::fprintf(stderr, "FAIL [%s]: cutout mismatch (got %d expected %d)\n",
                 label, int(res.has_cutout), int(expected_cutout));
    return false;
  }
  return true;
}

bool test_backend(BackendKind backend) {
  const char* bname = (backend == BackendKind::Vulkan) ? "vulkan" : "d3d12";

  DeviceDesc dd{};
  dd.backend = backend;
  dd.enable_validation = true;
  dd.app_name = "rhi_xenos_detile_tests";

  std::string err;
  auto dev = create_device(dd, err);
  if (!dev) {
    std::fprintf(stderr, "[%s] create_device skipped/failed: %s\n", bname, err.c_str());
    return false;
  }

  XenosDetileGpu detiler(*dev);
  CHECK(detiler.init(err), ("detiler init: " + err).c_str());

  // 1. RGBA8 non-32-aligned (37x29), all 4 endian modes, opaque & cutout.
  for (uint32_t endian = 0; endian < 4; ++endian) {
    XenosDetileLevelDesc lv{};
    lv.width_blocks = 37;
    lv.height_blocks = 29;
    lv.pitch_blocks = 64;
    lv.format = XenosDetileFormat::Rgba8;
    lv.xenos_endian = endian;
    lv.tiled = true;

    std::vector<uint8_t> linear(size_t(lv.width_blocks) * lv.height_blocks * 4u);
    for (size_t i = 0; i < linear.size(); i += 4) {
      linear[i + 0] = static_cast<uint8_t>((i * 7u) & 0xFFu);
      linear[i + 1] = static_cast<uint8_t>((i * 13u + 3u) & 0xFFu);
      linear[i + 2] = static_cast<uint8_t>((i * 29u + 11u) & 0xFFu);
      linear[i + 3] = 255u;  // opaque
    }
    CHECK(run_case(detiler, "RGBA8_opaque", lv, linear, false), "RGBA8 opaque failed");

    // Poke one cutout pixel in the middle.
    linear[(15u * lv.width_blocks + 21u) * 4u + 3u] = 42u;
    CHECK(run_case(detiler, "RGBA8_cutout", lv, linear, true), "RGBA8 cutout failed");
  }

  // 2. BC1 (8B/block) 20x14 blocks, endian=1 (8-in-16): 4-color opaque vs 3-color punchthrough.
  {
    XenosDetileLevelDesc lv{};
    lv.width_blocks = 20;
    lv.height_blocks = 14;
    lv.pitch_blocks = 32;
    lv.format = XenosDetileFormat::Bc1;
    lv.xenos_endian = 1;
    lv.tiled = true;

    std::vector<uint8_t> linear(size_t(lv.width_blocks) * lv.height_blocks * 8u);
    for (size_t b = 0; b < size_t(lv.width_blocks) * lv.height_blocks; ++b) {
      // c0 > c1 -> 4-color opaque mode even when index bits contain 0b11.
      const uint16_t c0 = static_cast<uint16_t>(0x8000u + (b & 0x7FFFu));
      const uint16_t c1 = static_cast<uint16_t>(0x1000u + (b & 0x0FFFu));
      const uint32_t idx = 0xFFFFFFFFu;
      std::memcpy(linear.data() + b * 8u + 0u, &c0, 2u);
      std::memcpy(linear.data() + b * 8u + 2u, &c1, 2u);
      std::memcpy(linear.data() + b * 8u + 4u, &idx, 4u);
    }
    CHECK(run_case(detiler, "BC1_opaque", lv, linear, false), "BC1 opaque failed");

    // Set block 99 to 3-color mode (c0 <= c1) with one pixel using index 3 (bits 0b11).
    const uint16_t c0 = 0x1234u, c1 = 0x5678u;
    const uint32_t idx = 0x0000000Cu;  // pixel 1 = 0b11
    std::memcpy(linear.data() + 99u * 8u + 0u, &c0, 2u);
    std::memcpy(linear.data() + 99u * 8u + 2u, &c1, 2u);
    std::memcpy(linear.data() + 99u * 8u + 4u, &idx, 4u);
    CHECK(run_case(detiler, "BC1_cutout", lv, linear, true), "BC1 cutout failed");
  }

  // 3. BC2 (16B/block) explicit 4-bit alpha, endian=1.
  {
    XenosDetileLevelDesc lv{};
    lv.width_blocks = 12;
    lv.height_blocks = 10;
    lv.pitch_blocks = 32;
    lv.format = XenosDetileFormat::Bc2;
    lv.xenos_endian = 1;
    lv.tiled = true;

    std::vector<uint8_t> linear(size_t(lv.width_blocks) * lv.height_blocks * 16u, 0xFFu);
    CHECK(run_case(detiler, "BC2_opaque", lv, linear, false), "BC2 opaque failed");
    linear[7u * 16u + 3u] = 0xF3u;  // low nibble = 3 (< 8)
    CHECK(run_case(detiler, "BC2_cutout", lv, linear, true), "BC2 cutout failed");
  }

  // 4. BC3 (16B/block) interpolated alpha: opaque vs 6-value index-6 literal 0 cutout.
  {
    XenosDetileLevelDesc lv{};
    lv.width_blocks = 16;
    lv.height_blocks = 12;
    lv.pitch_blocks = 32;
    lv.format = XenosDetileFormat::Bc3;
    lv.xenos_endian = 1;
    lv.tiled = true;

    std::vector<uint8_t> linear(size_t(lv.width_blocks) * lv.height_blocks * 16u, 0u);
    for (size_t b = 0; b < size_t(lv.width_blocks) * lv.height_blocks; ++b) {
      linear[b * 16u + 0u] = 200u;  // a0
      linear[b * 16u + 1u] = 220u;  // a1 (a0 <= a1 => 6-value mode)
      // All alpha indices = 0 (reads a0 = 200, opaque).
      linear[b * 16u + 8u] = static_cast<uint8_t>(b & 0xFFu);
      linear[b * 16u + 9u] = 0x5Au;
    }
    CHECK(run_case(detiler, "BC3_opaque", lv, linear, false), "BC3 opaque failed");

    // Set texel 10 (bit offset 30, spanning w0/w1 boundary!) to palette index 6 (literal 0 alpha).
    // In BC3, bits [16..63] hold 16x 3-bit indices -> bit offset 30 in 48-bit index field = byte 5 bit 6..8.
    uint64_t alpha_word = 0;
    std::memcpy(&alpha_word, linear.data() + 17u * 16u, 8u);
    alpha_word |= (uint64_t(6u) << (16u + 10u * 3u));
    std::memcpy(linear.data() + 17u * 16u, &alpha_word, 8u);
    CHECK(run_case(detiler, "BC3_cutout_boundary_idx6", lv, linear, true), "BC3 cutout failed");
  }

  // 5. BC5 (16B/block) + packed mip-tail offsets (block_offset_x=16, block_offset_y=8), endian=2.
  {
    XenosDetileLevelDesc lv{};
    lv.width_blocks = 4;
    lv.height_blocks = 4;
    lv.pitch_blocks = 32;
    lv.block_offset_x = 16;
    lv.block_offset_y = 8;
    lv.format = XenosDetileFormat::Bc5;
    lv.xenos_endian = 2;
    lv.src_byte_offset = 256;
    lv.tiled = true;

    std::vector<uint8_t> linear(size_t(lv.width_blocks) * lv.height_blocks * 16u);
    for (size_t i = 0; i < linear.size(); ++i) linear[i] = static_cast<uint8_t>((i * 37u + 9u) & 0xFFu);
    CHECK(run_case(detiler, "BC5_packed_tail", lv, linear, false), "BC5 packed tail failed");
  }

  // 6. Linear pass-through with endian=3 (16-in-32).
  {
    XenosDetileLevelDesc lv{};
    lv.width_blocks = 9;
    lv.height_blocks = 7;
    lv.pitch_blocks = 12;
    lv.format = XenosDetileFormat::Bc1;
    lv.xenos_endian = 3;
    lv.tiled = false;

    std::vector<uint8_t> linear(size_t(lv.width_blocks) * lv.height_blocks * 8u);
    for (size_t b = 0; b < size_t(lv.width_blocks) * lv.height_blocks; ++b) {
      const uint16_t c0 = 0xF800u, c1 = 0x001Fu;
      const uint32_t idx = static_cast<uint32_t>(0x12345678u ^ (b * 0x9E3779B9u));
      std::memcpy(linear.data() + b * 8u + 0u, &c0, 2u);
      std::memcpy(linear.data() + b * 8u + 2u, &c1, 2u);
      std::memcpy(linear.data() + b * 8u + 4u, &idx, 4u);
    }
    CHECK(run_case(detiler, "BC1_linear_swap", lv, linear, false), "BC1 linear failed");
  }

  detiler.shutdown();
  dev->wait_idle();
  const uint64_t val_msgs = dev->validation_message_count();
  std::printf("[%s] GPU Xenos detile bit-exact parity + cutout probe passed (validation_msgs=%llu)\n",
              bname, static_cast<unsigned long long>(val_msgs));
  CHECK(val_msgs == 0u, "validation layer reported errors");
  return true;
}

}  // namespace

int main() {
  if (!test_backend(BackendKind::Vulkan)) return 1;
#ifdef _WIN32
  if (!test_backend(BackendKind::D3D12)) return 1;
#endif
  std::printf("rhi_xenos_detile_tests: ALL PASSED\n");
  return 0;
}
