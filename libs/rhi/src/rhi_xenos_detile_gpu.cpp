#include "rhi/xenos_detile_gpu.h"

#include <algorithm>
#include <cstring>

#include "rhi_modern_shaders_hlsl_msl.h"
#include "rhi_modern_shaders_spv.h"

namespace shift::rhi {

uint32_t XenosDetileGpu::block_bytes(XenosDetileFormat fmt) noexcept {
  switch (fmt) {
    case XenosDetileFormat::Rgba8: return 4u;
    case XenosDetileFormat::Bc1:
    case XenosDetileFormat::Bc4:   return 8u;
    case XenosDetileFormat::Bc2:
    case XenosDetileFormat::Bc3:
    case XenosDetileFormat::Bc5:   return 16u;
  }
  return 4u;
}

uint32_t XenosDetileGpu::log2_block_bytes(XenosDetileFormat fmt) noexcept {
  const uint32_t bb = block_bytes(fmt);
  return (bb == 16u) ? 4u : ((bb == 8u) ? 3u : 2u);
}

uint32_t XenosDetileGpu::endian_to_swap_mask(uint32_t xenos_endian) noexcept {
  // Matches Xenos fetch-constant endianness: 0=none, 1=8-in-16, 2=8-in-32, 3=16-in-32.
  static constexpr uint32_t kMasks[4] = {0u, 1u, 3u, 2u};
  return kMasks[xenos_endian & 3u];
}

XenosDetileGpu::XenosDetileGpu(Device& device) : device_(device) {}

XenosDetileGpu::~XenosDetileGpu() {
  shutdown();
}

bool XenosDetileGpu::init(std::string& error) {
  shutdown();
  const BackendKind bk = device_.backend();

  BindGroupLayoutBinding bindings[3]{
      {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, false, true},
      {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, false, false},
      {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, false, false},
  };
  bgl_ = device_.create_bind_group_layout({bindings}, error);
  if (!bgl_) return false;

  PushConstantRange pc_range{ShaderStage::Compute, 0, sizeof(DetilePushConstants)};
  pl_ = device_.create_pipeline_layout({std::span(&bgl_, 1), std::span(&pc_range, 1)}, error);
  if (!pl_) return false;

  ComputePipelineDesc cpd{};
  cpd.layout = pl_;
  cpd.compute_shader = shaders::make_shader(
      bk, ShaderStage::Compute, shaders::kXenosDetileCompSpv,
      shaders::kXenosDetileCompHlsl, shaders::kXenosDetileCompMsl);
  pipe_ = device_.create_compute_pipeline(cpd, error);
  if (!pipe_) return false;

  ready_ = true;
  return true;
}

void XenosDetileGpu::shutdown() {
  ready_ = false;
  if (pipe_) { device_.destroy_pipeline(pipe_); pipe_ = {}; }
  if (pl_) { device_.destroy_pipeline_layout(pl_); pl_ = {}; }
  if (bgl_) { device_.destroy_bind_group_layout(bgl_); bgl_ = {}; }
}

bool XenosDetileGpu::add_detile_pass(RenderGraph& graph,
                                     RGBufferHandle src_tiled_rg, BufferHandle src_tiled_buf,
                                     RGBufferHandle dst_linear_rg, BufferHandle dst_linear_buf,
                                     RGBufferHandle cutout_rg, BufferHandle cutout_buf,
                                     std::span<const XenosDetileLevelDesc> levels,
                                     std::string& error) {
  if (!ready_) {
    error = "XenosDetileGpu::add_detile_pass: not initialized";
    return false;
  }
  if (levels.empty()) return true;

  BindGroupEntry ents[3]{
      {0, 0, DescriptorType::StorageBuffer, src_tiled_buf},
      {1, 0, DescriptorType::StorageBuffer, dst_linear_buf},
      {2, 0, DescriptorType::StorageBuffer, cutout_buf},
  };
  BindGroupHandle bg = device_.create_bind_group({bgl_, ents}, error);
  if (!bg) return false;

  std::vector<DetilePushConstants> pcs;
  pcs.reserve(levels.size());
  for (const XenosDetileLevelDesc& lv : levels) {
    DetilePushConstants pc{};
    pc.width_blocks = lv.width_blocks;
    pc.height_blocks = lv.height_blocks;
    pc.pitch_blocks = lv.pitch_blocks ? lv.pitch_blocks : lv.width_blocks;
    pc.block_offset_x = lv.block_offset_x;
    pc.block_offset_y = lv.block_offset_y;
    pc.block_bytes = block_bytes(lv.format);
    pc.log2_block_bytes = log2_block_bytes(lv.format);
    pc.endian_mask = endian_to_swap_mask(lv.xenos_endian);
    pc.src_byte_offset = lv.src_byte_offset;
    pc.dst_byte_offset = lv.dst_byte_offset;
    pc.tiled = lv.tiled ? 1u : 0u;
    pc.format_kind = (lv.format == XenosDetileFormat::Bc4 || lv.format == XenosDetileFormat::Bc5)
                         ? 4u
                         : static_cast<uint32_t>(lv.format);
    pcs.push_back(pc);
  }

  // Keep bind group alive through graph execution using a shared deleter.
  auto bg_guard = std::shared_ptr<BindGroupHandle>(
      new BindGroupHandle(bg),
      [dev = &device_](BindGroupHandle* h) {
        if (*h) dev->destroy_bind_group(*h);
        delete h;
      });

  graph.add_pass(
      "XenosDetile", QueueKind::AsyncCompute,
      [src_tiled_rg, dst_linear_rg, cutout_rg](RenderGraphBuilder& b) {
        b.read_buffer(src_tiled_rg, ResourceState::ShaderRead);
        b.write_buffer(dst_linear_rg, ResourceState::StorageReadWrite);
        b.write_buffer(cutout_rg, ResourceState::StorageReadWrite);
      },
      [this, cutout_buf, bg_guard, pcs = std::move(pcs)](CommandList& cmd, const RenderGraphContext&) {
        const uint32_t zero = 0u;
        BufferBarrier to_dst{cutout_buf, ResourceState::StorageReadWrite, ResourceState::CopyDst};
        cmd.barrier({{}, std::span(&to_dst, 1)});
        cmd.update_buffer(cutout_buf, 0, sizeof(zero), &zero);
        BufferBarrier to_uav{cutout_buf, ResourceState::CopyDst, ResourceState::StorageReadWrite};
        cmd.barrier({{}, std::span(&to_uav, 1)});

        cmd.bind_pipeline(pipe_);
        cmd.bind_group(0, *bg_guard);
        for (const DetilePushConstants& pc : pcs) {
          if (pc.width_blocks == 0u || pc.height_blocks == 0u) continue;
          cmd.push_constants(ShaderStage::Compute, 0, sizeof(pc), &pc);
          cmd.dispatch((pc.width_blocks + 7u) / 8u, (pc.height_blocks + 7u) / 8u, 1u);
        }
      });
  return true;
}

bool XenosDetileGpu::detile_sync(std::span<const uint8_t> tiled_bytes,
                                 const XenosDetileLevelDesc& level,
                                 XenosDetileSyncResult& out,
                                 std::string& error) {
  out = {};
  if (!ready_) {
    error = "XenosDetileGpu::detile_sync: not initialized";
    return false;
  }
  const uint32_t bb = block_bytes(level.format);
  const uint64_t dst_bytes = static_cast<uint64_t>(level.width_blocks) *
                             static_cast<uint64_t>(level.height_blocks) * bb;
  if (tiled_bytes.empty() || dst_bytes == 0) {
    error = "XenosDetileGpu::detile_sync: empty input or zero output blocks";
    return false;
  }

  const uint64_t src_alloc = std::max<uint64_t>((tiled_bytes.size() + 3ull) & ~3ull, 16ull);
  const uint64_t dst_alloc = std::max<uint64_t>(level.dst_byte_offset + dst_bytes, 16ull);

  BufferHandle src_stage = device_.create_buffer(
      {src_alloc, BufferUsage::TransferSrc, MemoryHint::CpuToGpu, true, "detile_src_stage"}, error);
  BufferHandle src_gpu = device_.create_buffer(
      {src_alloc, BufferUsage::Storage | BufferUsage::TransferDst, MemoryHint::GpuOnly, false, "detile_src_gpu"}, error);
  BufferHandle dst_gpu = device_.create_buffer(
      {dst_alloc, BufferUsage::Storage | BufferUsage::TransferSrc, MemoryHint::GpuOnly, false, "detile_dst_gpu"}, error);
  BufferHandle cutout_gpu = device_.create_buffer(
      {16ull, BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc,
       MemoryHint::GpuOnly, false, "detile_cutout_gpu"}, error);
  BufferHandle dst_rb = device_.create_buffer(
      {dst_alloc, BufferUsage::TransferDst, MemoryHint::GpuToCpu, true, "detile_dst_rb"}, error);
  BufferHandle cutout_rb = device_.create_buffer(
      {16ull, BufferUsage::TransferDst, MemoryHint::GpuToCpu, true, "detile_cutout_rb"}, error);

  auto cleanup = [&]() {
    if (cutout_rb) device_.destroy_buffer(cutout_rb);
    if (dst_rb) device_.destroy_buffer(dst_rb);
    if (cutout_gpu) device_.destroy_buffer(cutout_gpu);
    if (dst_gpu) device_.destroy_buffer(dst_gpu);
    if (src_gpu) device_.destroy_buffer(src_gpu);
    if (src_stage) device_.destroy_buffer(src_stage);
  };

  if (!src_stage || !src_gpu || !dst_gpu || !cutout_gpu || !dst_rb || !cutout_rb) {
    cleanup();
    return false;
  }

  if (void* p = device_.map_buffer(src_stage)) {
    std::memset(p, 0, static_cast<size_t>(src_alloc));
    std::memcpy(p, tiled_bytes.data(), tiled_bytes.size());
  }

  RenderGraph rg(device_);
  RGBufferHandle rg_src = rg.import_buffer("detile_src", src_gpu, ResourceState::Common);
  RGBufferHandle rg_dst = rg.import_buffer("detile_dst", dst_gpu, ResourceState::Common);
  RGBufferHandle rg_cut = rg.import_buffer("detile_cut", cutout_gpu, ResourceState::Common);
  RGBufferHandle rg_dst_rb = rg.import_buffer("detile_dst_rb", dst_rb, ResourceState::Common, ResourceState::HostRead);
  RGBufferHandle rg_cut_rb = rg.import_buffer("detile_cut_rb", cutout_rb, ResourceState::Common, ResourceState::HostRead);

  rg.add_pass(
      "DetileUpload", QueueKind::Transfer,
      [rg_src](RenderGraphBuilder& b) { b.write_buffer(rg_src, ResourceState::CopyDst); },
      [src_stage, src_gpu, src_alloc](CommandList& cmd, const RenderGraphContext&) {
        cmd.copy_buffer(src_stage, 0, src_gpu, 0, src_alloc);
      });

  if (!add_detile_pass(rg, rg_src, src_gpu, rg_dst, dst_gpu, rg_cut, cutout_gpu,
                       std::span(&level, 1), error)) {
    cleanup();
    return false;
  }

  rg.add_pass(
      "DetileReadback", QueueKind::Transfer,
      [rg_dst, rg_cut, rg_dst_rb, rg_cut_rb](RenderGraphBuilder& b) {
        b.read_buffer(rg_dst, ResourceState::CopySrc);
        b.read_buffer(rg_cut, ResourceState::CopySrc);
        b.write_buffer(rg_dst_rb, ResourceState::CopyDst);
        b.write_buffer(rg_cut_rb, ResourceState::CopyDst);
      },
      [dst_gpu, dst_rb, cutout_gpu, cutout_rb, dst_alloc](CommandList& cmd, const RenderGraphContext&) {
        cmd.copy_buffer(dst_gpu, 0, dst_rb, 0, dst_alloc);
        cmd.copy_buffer(cutout_gpu, 0, cutout_rb, 0, 4ull);
      });

  if (!rg.compile(error) || !rg.execute(error)) {
    cleanup();
    return false;
  }
  device_.wait_idle();

  if (const void* p = device_.map_buffer(dst_rb)) {
    out.linear_blocks.resize(static_cast<size_t>(dst_bytes));
    std::memcpy(out.linear_blocks.data(),
                static_cast<const uint8_t*>(p) + level.dst_byte_offset,
                static_cast<size_t>(dst_bytes));
    device_.unmap_buffer(dst_rb);
  }
  if (const void* p = device_.map_buffer(cutout_rb)) {
    uint32_t flag = 0;
    std::memcpy(&flag, p, sizeof(flag));
    out.has_cutout = (flag != 0u);
    device_.unmap_buffer(cutout_rb);
  }

  cleanup();
  return true;
}

}  // namespace shift::rhi
