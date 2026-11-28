// GPU Compute Xenos 2D Texture Detiler + Endian Byte-Swap + Atomic Cutout Alpha Probe.
//
// Runs xenos_detile.comp on QueueKind::AsyncCompute across Vulkan 1.3, DirectX 12,
// and Metal 3 — eliminating CPU untiling + CPU cutout alpha scans on texture streaming.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rhi/render_graph.h"
#include "rhi/rhi.h"

namespace shift::rhi {

enum class XenosDetileFormat : uint32_t {
  Rgba8 = 0,  // 4 bytes/block (1x1 texel)
  Bc1 = 1,    // 8 bytes/block (4x4 texels)
  Bc2 = 2,    // 16 bytes/block (4x4 texels)
  Bc3 = 3,    // 16 bytes/block (4x4 texels)
  Bc4 = 4,    // 8 bytes/block (4x4 texels, opaque cutout check)
  Bc5 = 5,    // 16 bytes/block (4x4 texels, opaque cutout check)
};

// Describes one level (or packed mip-tail slice) to detile on the GPU.
struct XenosDetileLevelDesc {
  uint32_t width_blocks = 0;
  uint32_t height_blocks = 0;
  uint32_t pitch_blocks = 0;       // storage pitch in blocks (aligned to 32 for tiled)
  uint32_t block_offset_x = 0;     // packed mip-tail X block offset (0 for normal levels)
  uint32_t block_offset_y = 0;     // packed mip-tail Y block offset (0 for normal levels)
  XenosDetileFormat format = XenosDetileFormat::Rgba8;
  uint32_t xenos_endian = 0;       // 0=none, 1=8-in-16, 2=8-in-32, 3=16-in-32
  uint32_t src_byte_offset = 0;
  uint32_t dst_byte_offset = 0;
  bool tiled = true;
};

struct XenosDetileSyncResult {
  std::vector<uint8_t> linear_blocks;
  bool has_cutout = false;
};

class XenosDetileGpu {
 public:
  explicit XenosDetileGpu(Device& device);
  ~XenosDetileGpu();

  XenosDetileGpu(const XenosDetileGpu&) = delete;
  XenosDetileGpu& operator=(const XenosDetileGpu&) = delete;

  bool init(std::string& error);
  void shutdown();
  bool is_ready() const noexcept { return ready_; }

  // Adds an AsyncCompute detile pass to a RenderGraph operating on pre-uploaded GPU buffers.
  // `cutout_flag_buf` must be >= 4 bytes (Storage + TransferDst); pass zeroes it before dispatch.
  bool add_detile_pass(RenderGraph& graph,
                       RGBufferHandle src_tiled_rg, BufferHandle src_tiled_buf,
                       RGBufferHandle dst_linear_rg, BufferHandle dst_linear_buf,
                       RGBufferHandle cutout_rg, BufferHandle cutout_buf,
                       std::span<const XenosDetileLevelDesc> levels,
                       std::string& error);

  // Convenience synchronous GPU detile using Transfer -> AsyncCompute -> Transfer
  // inside a RenderGraph, returning linear block bytes + single-pass GPU cutout flag.
  bool detile_sync(std::span<const uint8_t> tiled_bytes,
                   const XenosDetileLevelDesc& level,
                   XenosDetileSyncResult& out,
                   std::string& error);

  static uint32_t block_bytes(XenosDetileFormat fmt) noexcept;
  static uint32_t log2_block_bytes(XenosDetileFormat fmt) noexcept;
  static uint32_t endian_to_swap_mask(uint32_t xenos_endian) noexcept;

 private:
  struct DetilePushConstants {
    uint32_t width_blocks;
    uint32_t height_blocks;
    uint32_t pitch_blocks;
    uint32_t block_offset_x;
    uint32_t block_offset_y;
    uint32_t block_bytes;
    uint32_t log2_block_bytes;
    uint32_t endian_mask;
    uint32_t src_byte_offset;
    uint32_t dst_byte_offset;
    uint32_t tiled;
    uint32_t format_kind;
  };
  static_assert(sizeof(DetilePushConstants) == 48);

  Device& device_;
  bool ready_ = false;
  BindGroupLayoutHandle bgl_{};
  PipelineLayoutHandle pl_{};
  PipelineHandle pipe_{};
};

}  // namespace shift::rhi
