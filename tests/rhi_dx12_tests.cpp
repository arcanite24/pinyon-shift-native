// Live DirectX 12 RHI Backend verification suite (Windows / RTX 4080 + WARP).
// Exercises D3D12 device init, SM 6.6 DXIL shaders, buffer/texture transfers,
// 4096-slot bindless descriptor table sampling + swizzle, compute dispatch,
// offscreen RGBA8/D32 render pass, GPU timestamps, DXR 1.1 BLAS/TLAS/compaction,
// pipeline library persistence, and zero D3D12 debug layer errors.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rhi/rhi.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

std::vector<uint32_t> decode_b64_words(std::string_view b64) {
  static constexpr int8_t kDec[256] = {
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
      52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
      -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
      15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
      -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
      41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  };
  std::vector<uint8_t> bytes;
  bytes.reserve((b64.size() * 3) / 4);
  uint32_t val = 0;
  int valb = -8;
  for (unsigned char c : b64) {
    const int8_t d = kDec[c];
    if (d < 0) continue;
    val = (val << 6) | static_cast<uint32_t>(d);
    valb += 6;
    if (valb >= 0) {
      bytes.push_back(static_cast<uint8_t>((val >> valb) & 0xFFu));
      valb -= 8;
    }
  }
  while (bytes.size() % 4 != 0) bytes.push_back(0);
  std::vector<uint32_t> words(bytes.size() / 4);
  if (!bytes.empty()) std::memcpy(words.data(), bytes.data(), bytes.size());
  return words;
}

// Signed SM 6.6 DXIL compute shader (cs_6_6):
//   ByteAddressBuffer src_buf : register(t0, space0);
//   RWByteAddressBuffer dst_buf : register(u1, space0);
//   cbuffer Push : register(b0, space15) { uint addend; };
//   [numthreads(16, 1, 1)]
//   void main(uint3 tid : SV_DispatchThreadID) {
//     uint off = tid.x * 4u;
//     dst_buf.Store(off, src_buf.Load(off) * 2u + addend);
//   }
constexpr std::string_view kComputeDoubleDxilB64 =
    "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";

// Signed SM 6.6 DXIL fullscreen triangle vertex shader (vs_6_6, z = 0.5):
constexpr std::string_view kFullscreenVertDxilB64 =
    "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";

// Signed SM 6.6 DXIL solid color push-constant fragment shader (ps_6_6):
//   cbuffer Push : register(b0, space15) { float4 color; };
constexpr std::string_view kSolidColorFragDxilB64 =
    "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"
    "AAAAAAAAAA==";

// Signed SM 6.6 DXIL 4096-slot bindless texture sampler fragment shader (ps_6_6):
//   SamplerState world_sampler : register(s0, space0);
//   Texture2D<float4> world_textures[4096] : register(t1, space0);
//   cbuffer Push : register(b0, space15) { uint slot; };
constexpr std::string_view kBindlessSampleFragDxilB64 =
    "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"
    "CAAAAAAA";

void test_live_d3d12_gpu() {
  using namespace shift::rhi;

  CHECK(is_backend_compiled(BackendKind::D3D12));

  const auto adapters = enumerate_adapters(BackendKind::D3D12);
  CHECK(!adapters.empty());
  if (adapters.empty()) {
    std::printf("[rhi_dx12_tests] no DirectX 12 adapters present on host\n");
    return;
  }
  for (const AdapterInfo& a : adapters) {
    std::printf("[rhi_dx12_tests] adapter: %s (vendor=0x%04X device=0x%04X VRAM=%llu MiB discrete=%d dxr1_1=%d)\n",
                a.name.c_str(), a.vendor_id, a.device_id,
                static_cast<unsigned long long>(a.device_local_bytes >> 20),
                a.is_discrete ? 1 : 0, a.supports_ray_query ? 1 : 0);
  }

  const std::filesystem::path cache_path =
      std::filesystem::temp_directory_path() / "shift_rhi_dx12_test_pipeline_cache.bin";
  std::error_code ec;
  std::filesystem::remove(cache_path, ec);

  DeviceDesc ddesc{};
  ddesc.backend = BackendKind::D3D12;
  ddesc.enable_validation = true;
  ddesc.enable_ray_tracing = true;
  ddesc.enable_bindless = true;
  ddesc.pipeline_cache_path = cache_path;
  ddesc.app_name = "rhi_dx12_tests";

  std::string err;
  std::unique_ptr<Device> dev = create_device(ddesc, err);
  if (!dev) {
    std::printf("[rhi_dx12_tests] FAILED to create D3D12 device: %s\n", err.c_str());
    CHECK(dev != nullptr);
    return;
  }

  const BackendCaps& caps = dev->caps();
  std::printf("[rhi_dx12_tests] D3D12 device: %s (driver=%s, VRAM=%llu MiB, SM=0x%X, bindless=%d, "
              "timestamps=%d (%.3f ns/tick), enhanced_barriers=%d, DXR1.1=%d, hdr10=%d, scrgb=%d)\n",
              caps.device_name.c_str(),
              caps.driver_info.c_str(),
              static_cast<unsigned long long>(caps.device_local_bytes >> 20),
              caps.api_version,
              caps.bindless ? 1 : 0,
              caps.timestamps ? 1 : 0,
              caps.timestamp_period_ns,
              caps.synchronization2 ? 1 : 0,
              caps.ray_query ? 1 : 0,
              caps.hdr10 ? 1 : 0,
              caps.scrgb ? 1 : 0);

  CHECK(dev->backend() == BackendKind::D3D12);
  CHECK(!caps.device_name.empty());
  CHECK(caps.dynamic_rendering);
  CHECK(caps.bindless);
  CHECK(caps.max_bindless_textures >= 4096u);
  CHECK(caps.timestamps);
  CHECK(caps.timestamp_period_ns > 0.0);
  CHECK(caps.supported_msaa_mask >= 1u);
  if (caps.is_discrete) {
    CHECK(caps.ray_query);
  }

  const std::vector<uint32_t> compute_dxil = decode_b64_words(kComputeDoubleDxilB64);
  const std::vector<uint32_t> vs_dxil = decode_b64_words(kFullscreenVertDxilB64);
  const std::vector<uint32_t> solid_ps_dxil = decode_b64_words(kSolidColorFragDxilB64);
  const std::vector<uint32_t> bindless_ps_dxil = decode_b64_words(kBindlessSampleFragDxilB64);

  // -------------------------------------------------------------------------
  // 1. Buffer upload, copy, fill_buffer, update_buffer, and readback
  // -------------------------------------------------------------------------
  {
    constexpr uint32_t kCount = 256;
    constexpr uint64_t kBytes = kCount * sizeof(uint32_t);
    BufferHandle stage = dev->create_buffer(
        {kBytes, BufferUsage::TransferSrc, MemoryHint::CpuToGpu, true, "dx12_stage"}, err);
    BufferHandle gpu = dev->create_buffer(
        {kBytes, BufferUsage::TransferSrc | BufferUsage::TransferDst | BufferUsage::Storage,
         MemoryHint::GpuOnly, false, "dx12_gpu"}, err);
    BufferHandle readback = dev->create_buffer(
        {kBytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "dx12_readback"}, err);
    CHECK(stage && gpu && readback);

    auto* src_ptr = static_cast<uint32_t*>(dev->map_buffer(stage));
    CHECK(src_ptr != nullptr);
    for (uint32_t i = 0; i < kCount; ++i) src_ptr[i] = 0x1000u + i * 7u;

    const uint32_t inline_patch[4] = {0xCAFE0001u, 0xCAFE0002u, 0xCAFE0003u, 0xCAFE0004u};
    bool ok = dev->submit_once([&](CommandList& cmd) {
      cmd.copy_buffer(stage, 0, gpu, 0, kBytes);
      BufferBarrier bb{gpu, ResourceState::CopyDst, ResourceState::CopyDst};
      cmd.barrier({{}, std::span(&bb, 1)});
      cmd.fill_buffer(gpu, 16 * sizeof(uint32_t), 16 * sizeof(uint32_t), 0xDEADBEEFu);
      cmd.barrier({{}, std::span(&bb, 1)});
      cmd.update_buffer(gpu, 4 * sizeof(uint32_t), sizeof(inline_patch), inline_patch);
      BufferBarrier to_src{gpu, ResourceState::CopyDst, ResourceState::CopySrc};
      cmd.barrier({{}, std::span(&to_src, 1)});
      cmd.copy_buffer(gpu, 0, readback, 0, kBytes);
      cmd.barrier({{}, {}, false, false, true});
    }, err);
    CHECK(ok);

    auto* dst_ptr = static_cast<uint32_t*>(dev->map_buffer(readback));
    CHECK(dst_ptr != nullptr);
    if (dst_ptr) {
      for (uint32_t i = 0; i < kCount; ++i) {
        uint32_t expected = 0x1000u + i * 7u;
        if (i >= 4 && i < 8) expected = inline_patch[i - 4];
        if (i >= 16 && i < 32) expected = 0xDEADBEEFu;
        if (dst_ptr[i] != expected) {
          CHECK(dst_ptr[i] == expected);
          break;
        }
      }
      dev->unmap_buffer(readback);
    }

    dev->destroy_buffer(readback);
    dev->destroy_buffer(gpu);
    dev->destroy_buffer(stage);
    std::printf("[rhi_dx12_tests] 1. Buffer upload/fill/update/copy/readback: OK\n");
  }

  // -------------------------------------------------------------------------
  // 2. 2D Texture upload (32x32 non-256-aligned pitch), readback,
  //    4096-slot bindless table + swizzled view live GPU sampling
  // -------------------------------------------------------------------------
  {
    constexpr uint32_t kW = 32, kH = 32;
    constexpr uint64_t kBytes = kW * kH * 4ull;
    BufferHandle stage = dev->create_buffer(
        {kBytes, BufferUsage::TransferSrc, MemoryHint::CpuToGpu, false, "dx12_tex_stage"}, err);
    BufferHandle readback = dev->create_buffer(
        {kBytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "dx12_tex_rb"}, err);
    TextureHandle tex = dev->create_texture(
        {TextureType::Tex2D, Format::Rgba8Unorm, kW, kH, 1, 1, 1, 1,
         TextureUsage::TransferSrc | TextureUsage::TransferDst | TextureUsage::Sampled,
         MemoryHint::GpuOnly, "dx12_tex2d"}, err);
    TextureHandle out_rt = dev->create_texture(
        {TextureType::Tex2D, Format::Rgba8Unorm, 16, 16, 1, 1, 1, 1,
         TextureUsage::ColorAttachment | TextureUsage::TransferSrc,
         MemoryHint::GpuOnly, "dx12_bindless_out"}, err);
    BufferHandle out_rb = dev->create_buffer(
        {16 * 16 * 4ull, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "dx12_bindless_rb"}, err);
    CHECK(stage && readback && tex && out_rt && out_rb);

    TextureViewHandle view = dev->create_texture_view({tex, TextureType::Tex2D}, err);
    TextureViewHandle swiz_view = dev->create_texture_view(
        {tex, TextureType::Tex2D, Format::Undefined, 0, 1, 0, 1,
         {ComponentSwizzle::B, ComponentSwizzle::G, ComponentSwizzle::R, ComponentSwizzle::One}}, err);
    TextureViewHandle out_view = dev->create_texture_view({out_rt, TextureType::Tex2D}, err);
    SamplerHandle sampler = dev->create_sampler(SamplerDesc{}, err);
    CHECK(view && swiz_view && out_view && sampler);

    // Fill texture with constant RGBA = (40, 120, 200, 255).
    auto* px = static_cast<uint8_t*>(dev->map_buffer(stage));
    CHECK(px != nullptr);
    for (uint32_t i = 0; i < kW * kH; ++i) {
      px[i * 4 + 0] = 40u;
      px[i * 4 + 1] = 120u;
      px[i * 4 + 2] = 200u;
      px[i * 4 + 3] = 255u;
    }
    dev->unmap_buffer(stage);

    BufferHandle dummy_ssbo = dev->create_buffer(
        {64, BufferUsage::Storage, MemoryHint::GpuOnly, false, "dx12_bindless_ssbo"}, err);
    BindlessTableDesc btd{};
    btd.default_sampler = sampler;
    btd.fallback_texture = view;
    btd.fallback_buffer = dummy_ssbo;
    btd.max_textures = 4096;
    btd.max_storage_buffers = 256;
    auto table = dev->create_bindless_table(btd, err);
    CHECK(table != nullptr);
    if (table) {
      CHECK(table->layout().valid() && table->bind_group().valid());
      CHECK(table->max_textures() == 4096);
      CHECK(table->max_storage_buffers() == 256);
      table->set_texture(0, view);
      table->set_texture(7, swiz_view);
      table->set_texture(4095, view);
      table->set_storage_buffer(0, dummy_ssbo);
    }

    BindGroupLayoutHandle bl_layout = table ? table->layout() : BindGroupLayoutHandle{};
    PushConstantRange pc_range{ShaderStage::Fragment, 0, sizeof(uint32_t)};
    PipelineLayoutHandle pl = dev->create_pipeline_layout(
        {std::span(&bl_layout, 1), std::span(&pc_range, 1)}, err);
    Format color_fmt = Format::Rgba8Unorm;
    GraphicsPipelineDesc gp{};
    gp.layout = pl;
    gp.vertex_shader = {ShaderStage::Vertex, vs_dxil, "main"};
    gp.fragment_shader = {ShaderStage::Fragment, bindless_ps_dxil, "main"};
    gp.color_formats = std::span(&color_fmt, 1);
    gp.raster.cull_mode = CullMode::None;
    PipelineHandle bl_pipe = dev->create_graphics_pipeline(gp, err);
    CHECK(pl && bl_pipe);

    const uint32_t sampled_slot = 7u;  // swizzled B, G, R, 1 -> expects (200, 120, 40, 255)!
    bool ok = dev->submit_once([&](CommandList& cmd) {
      TextureBarrier to_dst[2] = {
          {tex, ResourceState::Undefined, ResourceState::CopyDst},
          {out_rt, ResourceState::Undefined, ResourceState::RenderTarget},
      };
      cmd.barrier({to_dst, {}});
      BufferTextureCopy region{};
      region.width = kW;
      region.height = kH;
      cmd.copy_buffer_to_texture(stage, tex, std::span(&region, 1));

      TextureBarrier tex_to_src{tex, ResourceState::CopyDst, ResourceState::CopySrc};
      cmd.barrier({std::span(&tex_to_src, 1), {}});
      cmd.copy_texture_to_buffer(tex, readback, std::span(&region, 1));

      TextureBarrier tex_to_srv{tex, ResourceState::CopySrc, ResourceState::ShaderRead};
      cmd.barrier({std::span(&tex_to_srv, 1), {}});

      ColorAttachmentDesc ca{};
      ca.view = out_view;
      ca.load_op = LoadOp::Clear;
      ca.store_op = StoreOp::Store;
      RenderPassDesc rp{};
      rp.width = 16;
      rp.height = 16;
      rp.color_attachments = std::span(&ca, 1);
      cmd.begin_render_pass(rp);
      cmd.set_viewport(0.0f, 0.0f, 16.0f, 16.0f);
      cmd.set_scissor(0, 0, 16, 16);
      cmd.bind_pipeline(bl_pipe);
      cmd.bind_bindless(0, *table);
      cmd.push_constants(ShaderStage::Fragment, 0, sizeof(sampled_slot), &sampled_slot);
      cmd.draw(3);
      cmd.end_render_pass();

      TextureBarrier out_to_src{out_rt, ResourceState::RenderTarget, ResourceState::CopySrc};
      cmd.barrier({std::span(&out_to_src, 1), {}});
      BufferTextureCopy out_copy{};
      out_copy.width = 16;
      out_copy.height = 16;
      cmd.copy_texture_to_buffer(out_rt, out_rb, std::span(&out_copy, 1));
    }, err);
    CHECK(ok);

    auto* rb = static_cast<uint8_t*>(dev->map_buffer(readback));
    CHECK(rb != nullptr);
    if (rb) {
      CHECK(rb[0] == 40u && rb[1] == 120u && rb[2] == 200u && rb[3] == 255u);
      const size_t last = (size_t(kH - 1) * kW + (kW - 1)) * 4;
      CHECK(rb[last + 0] == 40u && rb[last + 1] == 120u && rb[last + 2] == 200u && rb[last + 3] == 255u);
      dev->unmap_buffer(readback);
    }

    auto* bl_rb = static_cast<uint8_t*>(dev->map_buffer(out_rb));
    CHECK(bl_rb != nullptr);
    if (bl_rb) {
      const size_t mid = (size_t(8) * 16 + 8) * 4;
      CHECK(std::abs(int(bl_rb[mid + 0]) - 200) <= 1);
      CHECK(std::abs(int(bl_rb[mid + 1]) - 120) <= 1);
      CHECK(std::abs(int(bl_rb[mid + 2]) - 40) <= 1);
      CHECK(bl_rb[mid + 3] == 255);
      dev->unmap_buffer(out_rb);
    }

    dev->destroy_pipeline(bl_pipe);
    dev->destroy_pipeline_layout(pl);
    table.reset();
    dev->destroy_buffer(dummy_ssbo);
    dev->destroy_sampler(sampler);
    dev->destroy_texture_view(out_view);
    dev->destroy_texture_view(swiz_view);
    dev->destroy_texture_view(view);
    dev->destroy_buffer(out_rb);
    dev->destroy_texture(out_rt);
    dev->destroy_texture(tex);
    dev->destroy_buffer(readback);
    dev->destroy_buffer(stage);
    std::printf("[rhi_dx12_tests] 2. 2D texture upload + 4096 bindless slot/swizzle sample + readback: OK\n");
  }

  // -------------------------------------------------------------------------
  // 3. SM 6.6 DXIL Compute shader dispatch + readback + GPU timestamp delta
  // -------------------------------------------------------------------------
  {
    constexpr uint32_t kN = 64;
    constexpr uint64_t kBytes = kN * sizeof(uint32_t);
    BufferHandle in_buf = dev->create_buffer(
        {kBytes, BufferUsage::Storage, MemoryHint::CpuToGpu, false, "dx12_comp_in"}, err);
    BufferHandle out_buf = dev->create_buffer(
        {kBytes, BufferUsage::Storage | BufferUsage::TransferSrc, MemoryHint::GpuOnly, false, "dx12_comp_out"}, err);
    BufferHandle rb_buf = dev->create_buffer(
        {kBytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "dx12_comp_rb"}, err);
    CHECK(in_buf && out_buf && rb_buf);

    auto* inp = static_cast<uint32_t*>(dev->map_buffer(in_buf));
    CHECK(inp != nullptr);
    for (uint32_t i = 0; i < kN; ++i) inp[i] = i + 10u;
    dev->unmap_buffer(in_buf);

    BindGroupLayoutBinding bgl_bindings[2] = {
        {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
        {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute},
    };
    BindGroupLayoutHandle bgl = dev->create_bind_group_layout({bgl_bindings, false}, err);
    BindGroupEntry bg_entries[2] = {
        {0, 0, DescriptorType::StorageBuffer, in_buf},
        {1, 0, DescriptorType::StorageBuffer, out_buf},
    };
    BindGroupHandle bg = dev->create_bind_group({bgl, bg_entries}, err);
    PushConstantRange pc_range{ShaderStage::Compute, 0, sizeof(uint32_t)};
    PipelineLayoutHandle pl = dev->create_pipeline_layout({std::span(&bgl, 1), std::span(&pc_range, 1)}, err);
    ComputePipelineDesc cp_desc{};
    cp_desc.layout = pl;
    cp_desc.compute_shader = {ShaderStage::Compute, compute_dxil, "main"};
    PipelineHandle pipe = dev->create_compute_pipeline(cp_desc, err);
    CHECK(bgl && bg && pl && pipe);

    QueryPoolHandle ts_pool = dev->create_query_pool({QueryType::Timestamp, 4}, err);
    CHECK(ts_pool.valid());

    const uint32_t addend = 13u;
    bool ok = dev->submit_once([&](CommandList& cmd) {
      cmd.reset_query_pool(ts_pool, 0, 4);
      cmd.write_timestamp(ts_pool, 0, PipelineStage::TopOfPipe);
      cmd.bind_pipeline(pipe);
      cmd.bind_group(0, bg);
      cmd.push_constants(ShaderStage::Compute, 0, sizeof(addend), &addend);
      cmd.dispatch(kN / 16u, 1, 1);

      BufferBarrier to_copy{out_buf, ResourceState::StorageReadWrite, ResourceState::CopySrc};
      cmd.barrier({{}, std::span(&to_copy, 1)});
      cmd.copy_buffer(out_buf, 0, rb_buf, 0, kBytes);
      cmd.write_timestamp(ts_pool, 1, PipelineStage::BottomOfPipe);
    }, err);
    CHECK(ok);

    auto* outp = static_cast<uint32_t*>(dev->map_buffer(rb_buf));
    CHECK(outp != nullptr);
    if (outp) {
      for (uint32_t i = 0; i < kN; ++i) {
        const uint32_t expected = (i + 10u) * 2u + addend;
        if (outp[i] != expected) {
          CHECK(outp[i] == expected);
          break;
        }
      }
      dev->unmap_buffer(rb_buf);
    }

    uint64_t ticks[2] = {};
    CHECK(dev->get_query_pool_results(ts_pool, 0, 2, ticks, true));
    CHECK(ticks[1] >= ticks[0]);
    const double elapsed_us =
        static_cast<double>(ticks[1] - ticks[0]) * caps.timestamp_period_ns / 1000.0;
    std::printf("[rhi_dx12_tests] 3. SM 6.6 DXIL compute dispatch + timestamp delta: %.2f us (ticks=%llu -> %llu)\n",
                elapsed_us,
                static_cast<unsigned long long>(ticks[0]),
                static_cast<unsigned long long>(ticks[1]));

    dev->destroy_query_pool(ts_pool);
    dev->destroy_pipeline(pipe);
    dev->destroy_pipeline_layout(pl);
    dev->destroy_bind_group(bg);
    dev->destroy_bind_group_layout(bgl);
    dev->destroy_buffer(rb_buf);
    dev->destroy_buffer(out_buf);
    dev->destroy_buffer(in_buf);
  }

  // -------------------------------------------------------------------------
  // 4. Offscreen render pass clear + draw + RGBA8 & D32_SFLOAT readback
  // -------------------------------------------------------------------------
  {
    constexpr uint32_t kW = 64, kH = 64;
    TextureHandle color_tex = dev->create_texture(
        {TextureType::Tex2D, Format::Rgba8Unorm, kW, kH, 1, 1, 1, 1,
         TextureUsage::ColorAttachment | TextureUsage::TransferSrc, MemoryHint::GpuOnly, "dx12_rt_color"}, err);
    TextureHandle depth_tex = dev->create_texture(
        {TextureType::Tex2D, Format::D32Sfloat, kW, kH, 1, 1, 1, 1,
         TextureUsage::DepthAttachment | TextureUsage::TransferSrc, MemoryHint::GpuOnly, "dx12_rt_depth"}, err);
    TextureViewHandle color_view = dev->create_texture_view({color_tex, TextureType::Tex2D}, err);
    TextureViewHandle depth_view = dev->create_texture_view({depth_tex, TextureType::Tex2D}, err);
    BufferHandle color_rb = dev->create_buffer(
        {kW * kH * 4ull, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "dx12_color_rb"}, err);
    BufferHandle depth_rb = dev->create_buffer(
        {kW * kH * 4ull, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "dx12_depth_rb"}, err);
    CHECK(color_tex && depth_tex && color_view && depth_view && color_rb && depth_rb);

    PushConstantRange pc_range{ShaderStage::Fragment, 0, sizeof(float) * 4};
    PipelineLayoutHandle pl = dev->create_pipeline_layout({{}, std::span(&pc_range, 1)}, err);
    Format color_fmt = Format::Rgba8Unorm;
    GraphicsPipelineDesc gp{};
    gp.layout = pl;
    gp.vertex_shader = {ShaderStage::Vertex, vs_dxil, "main"};
    gp.fragment_shader = {ShaderStage::Fragment, solid_ps_dxil, "main"};
    gp.color_formats = std::span(&color_fmt, 1);
    gp.depth_format = Format::D32Sfloat;
    gp.depth = {true, true, CompareOp::Always, false};
    gp.raster.cull_mode = CullMode::None;
    PipelineHandle pipe = dev->create_graphics_pipeline(gp, err);
    CHECK(pl && pipe);

    const float draw_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    bool ok = dev->submit_once([&](CommandList& cmd) {
      TextureBarrier begin_barriers[2] = {
          {color_tex, ResourceState::Undefined, ResourceState::RenderTarget},
          {depth_tex, ResourceState::Undefined, ResourceState::DepthWrite, {}, {}, {}, {}, 0, 1, 0, 1, true},
      };
      cmd.barrier({begin_barriers, {}});

      ColorAttachmentDesc ca{};
      ca.view = color_view;
      ca.load_op = LoadOp::Clear;
      ca.store_op = StoreOp::Store;
      DepthAttachmentDesc da{};
      da.view = depth_view;
      da.load_op = LoadOp::Clear;
      da.store_op = StoreOp::Store;
      da.clear_depth = 0.0f;

      RenderPassDesc rp{};
      rp.width = kW;
      rp.height = kH;
      rp.color_attachments = std::span(&ca, 1);
      rp.depth_attachment = &da;
      cmd.begin_render_pass(rp);
      cmd.set_viewport(0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH));
      cmd.set_scissor(0, 0, kW, kH);
      cmd.bind_pipeline(pipe);
      cmd.push_constants(ShaderStage::Fragment, 0, sizeof(draw_color), draw_color);
      cmd.draw(3);
      cmd.end_render_pass();

      TextureBarrier end_barriers[2] = {
          {color_tex, ResourceState::RenderTarget, ResourceState::CopySrc},
          {depth_tex, ResourceState::DepthWrite, ResourceState::CopySrc, {}, {}, {}, {}, 0, 1, 0, 1, true},
      };
      cmd.barrier({end_barriers, {}});

      BufferTextureCopy color_copy{};
      color_copy.width = kW;
      color_copy.height = kH;
      cmd.copy_texture_to_buffer(color_tex, color_rb, std::span(&color_copy, 1));

      BufferTextureCopy depth_copy{};
      depth_copy.width = kW;
      depth_copy.height = kH;
      depth_copy.is_depth = true;
      cmd.copy_texture_to_buffer(depth_tex, depth_rb, std::span(&depth_copy, 1));
    }, err);
    CHECK(ok);

    auto* cpx = static_cast<uint8_t*>(dev->map_buffer(color_rb));
    auto* dpx = static_cast<float*>(dev->map_buffer(depth_rb));
    CHECK(cpx != nullptr && dpx != nullptr);
    if (cpx && dpx) {
      const size_t mid = size_t(kH / 2) * kW + (kW / 2);
      CHECK(std::abs(int(cpx[mid * 4 + 0]) - 64) <= 1);
      CHECK(std::abs(int(cpx[mid * 4 + 1]) - 128) <= 1);
      CHECK(std::abs(int(cpx[mid * 4 + 2]) - 191) <= 1);
      CHECK(cpx[mid * 4 + 3] == 255);
      CHECK(std::fabs(dpx[mid] - 0.5f) < 1e-4f);
    }
    if (cpx) dev->unmap_buffer(color_rb);
    if (dpx) dev->unmap_buffer(depth_rb);

    dev->destroy_pipeline(pipe);
    dev->destroy_pipeline_layout(pl);
    dev->destroy_buffer(depth_rb);
    dev->destroy_buffer(color_rb);
    dev->destroy_texture_view(depth_view);
    dev->destroy_texture_view(color_view);
    dev->destroy_texture(depth_tex);
    dev->destroy_texture(color_tex);
    std::printf("[rhi_dx12_tests] 4. Offscreen render pass clear/draw + RGBA8/D32 readback: OK\n");
  }

  // -------------------------------------------------------------------------
  // 4b. 4x MSAA ResolveSubresource + ExecuteIndirect (draw_indirect & indexed count)
  // -------------------------------------------------------------------------
  if (caps.supported_msaa_mask & 4u) {
    constexpr uint32_t kW = 32, kH = 32;
    TextureHandle msaa_color = dev->create_texture(
        {TextureType::Tex2D, Format::Rgba8Unorm, kW, kH, 1, 1, 1, 4,
         TextureUsage::ColorAttachment, MemoryHint::GpuOnly, "dx12_msaa_color"}, err);
    TextureHandle resolved_color = dev->create_texture(
        {TextureType::Tex2D, Format::Rgba8Unorm, kW, kH, 1, 1, 1, 1,
         TextureUsage::ColorAttachment | TextureUsage::TransferSrc, MemoryHint::GpuOnly, "dx12_resolved"}, err);
    TextureViewHandle msaa_view = dev->create_texture_view({msaa_color, TextureType::Tex2D}, err);
    TextureViewHandle res_view = dev->create_texture_view({resolved_color, TextureType::Tex2D}, err);
    BufferHandle rb = dev->create_buffer(
        {kW * kH * 4ull, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "dx12_msaa_rb"}, err);

    const uint32_t indices[3] = {0, 1, 2};
    BufferHandle ib = dev->create_buffer(
        {sizeof(indices), BufferUsage::Index, MemoryHint::CpuToGpu, false, "dx12_ind_ib"}, err);
    std::memcpy(dev->map_buffer(ib), indices, sizeof(indices));
    dev->unmap_buffer(ib);

    struct DrawIndexedArgs { uint32_t index_count, inst_count, first_index; int32_t vtx_off; uint32_t first_inst; };
    const DrawIndexedArgs ind_args{3, 1, 0, 0, 0};
    BufferHandle arg_buf = dev->create_buffer(
        {sizeof(ind_args), BufferUsage::Indirect, MemoryHint::CpuToGpu, false, "dx12_ind_args"}, err);
    std::memcpy(dev->map_buffer(arg_buf), &ind_args, sizeof(ind_args));
    dev->unmap_buffer(arg_buf);

    const uint32_t draw_cnt = 1u;
    BufferHandle cnt_buf = dev->create_buffer(
        {sizeof(draw_cnt), BufferUsage::Indirect, MemoryHint::CpuToGpu, false, "dx12_ind_cnt"}, err);
    std::memcpy(dev->map_buffer(cnt_buf), &draw_cnt, sizeof(draw_cnt));
    dev->unmap_buffer(cnt_buf);

    PushConstantRange pc_range{ShaderStage::Fragment, 0, sizeof(float) * 4};
    PipelineLayoutHandle pl = dev->create_pipeline_layout({{}, std::span(&pc_range, 1)}, err);
    Format color_fmt = Format::Rgba8Unorm;
    GraphicsPipelineDesc gp{};
    gp.layout = pl;
    gp.vertex_shader = {ShaderStage::Vertex, vs_dxil, "main"};
    gp.fragment_shader = {ShaderStage::Fragment, solid_ps_dxil, "main"};
    gp.color_formats = std::span(&color_fmt, 1);
    gp.raster.cull_mode = CullMode::None;
    gp.raster.sample_count = 4;
    PipelineHandle pipe = dev->create_graphics_pipeline(gp, err);
    CHECK(msaa_color && resolved_color && msaa_view && res_view && rb && ib && arg_buf && cnt_buf && pl && pipe);

    const float msaa_col[4] = {1.0f, 0.25f, 0.5f, 1.0f};
    bool ok = dev->submit_once([&](CommandList& cmd) {
      TextureBarrier b0[2] = {
          {msaa_color, ResourceState::Undefined, ResourceState::RenderTarget},
          {resolved_color, ResourceState::Undefined, ResourceState::Common},
      };
      cmd.barrier({b0, {}});

      ColorAttachmentDesc ca{};
      ca.view = msaa_view;
      ca.resolve_view = res_view;
      ca.load_op = LoadOp::Clear;
      ca.store_op = StoreOp::MultisampleResolve;
      ca.resolve_mode = ResolveMode::Average;
      RenderPassDesc rp{};
      rp.width = kW;
      rp.height = kH;
      rp.color_attachments = std::span(&ca, 1);
      cmd.begin_render_pass(rp);
      cmd.set_viewport(0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH));
      cmd.set_scissor(0, 0, kW, kH);
      cmd.bind_pipeline(pipe);
      cmd.push_constants(ShaderStage::Fragment, 0, sizeof(msaa_col), msaa_col);
      cmd.bind_index_buffer(ib, 0, IndexType::Uint32);
      cmd.draw_indexed_indirect_count(arg_buf, 0, cnt_buf, 0, 1, sizeof(DrawIndexedArgs));
      cmd.end_render_pass();

      TextureBarrier b1{resolved_color, ResourceState::Common, ResourceState::CopySrc};
      cmd.barrier({std::span(&b1, 1), {}});
      BufferTextureCopy cp{};
      cp.width = kW;
      cp.height = kH;
      cmd.copy_texture_to_buffer(resolved_color, rb, std::span(&cp, 1));
    }, err);
    CHECK(ok);

    auto* px = static_cast<uint8_t*>(dev->map_buffer(rb));
    CHECK(px != nullptr);
    if (px) {
      const size_t mid = (size_t(kH / 2) * kW + (kW / 2)) * 4;
      CHECK(px[mid + 0] == 255);
      CHECK(std::abs(int(px[mid + 1]) - 64) <= 1);
      CHECK(std::abs(int(px[mid + 2]) - 128) <= 1);
      CHECK(px[mid + 3] == 255);
      dev->unmap_buffer(rb);
    }

    dev->destroy_pipeline(pipe);
    dev->destroy_pipeline_layout(pl);
    dev->destroy_buffer(cnt_buf);
    dev->destroy_buffer(arg_buf);
    dev->destroy_buffer(ib);
    dev->destroy_buffer(rb);
    dev->destroy_texture_view(res_view);
    dev->destroy_texture_view(msaa_view);
    dev->destroy_texture(resolved_color);
    dev->destroy_texture(msaa_color);
    std::printf("[rhi_dx12_tests] 4b. 4x MSAA ResolveSubresource + ExecuteIndirect (indexed count): OK\n");
  }

  // -------------------------------------------------------------------------
  // 5. Native DXR 1.1: BLAS build + Compaction Postbuild Query + Compact + TLAS
  // -------------------------------------------------------------------------
  if (caps.ray_query) {
    const float verts[9] = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
    };
    const uint32_t inds[3] = {0, 1, 2};
    BufferHandle vb = dev->create_buffer(
        {sizeof(verts), BufferUsage::Vertex | BufferUsage::AccelStructInput | BufferUsage::DeviceAddress,
         MemoryHint::CpuToGpu, false, "dxr_vb"}, err);
    BufferHandle ib = dev->create_buffer(
        {sizeof(inds), BufferUsage::Index | BufferUsage::AccelStructInput | BufferUsage::DeviceAddress,
         MemoryHint::CpuToGpu, false, "dxr_ib"}, err);
    CHECK(vb && ib);
    CHECK(dev->buffer_device_address(vb) != 0);
    CHECK(dev->buffer_device_address(ib) != 0);

    std::memcpy(dev->map_buffer(vb), verts, sizeof(verts));
    dev->unmap_buffer(vb);
    std::memcpy(dev->map_buffer(ib), inds, sizeof(inds));
    dev->unmap_buffer(ib);

    BlasTriangleGeometryDesc geom{};
    geom.vertex_buffer = vb;
    geom.vertex_stride = sizeof(float) * 3;
    geom.max_vertex = 2;
    geom.vertex_format = Format::Rgb32Sfloat;
    geom.index_buffer = ib;
    geom.index_type = IndexType::Uint32;
    geom.primitive_count = 1;
    geom.flags = GeometryFlags::Opaque;

    const auto blas_flags = AccelStructBuildFlags::PreferFastTrace | AccelStructBuildFlags::AllowCompaction;
    AccelStructBuildSizes blas_sizes{};
    CHECK(dev->query_blas_sizes(std::span(&geom, 1), blas_flags, blas_sizes));
    CHECK(blas_sizes.accel_struct_size > 0 && blas_sizes.build_scratch_size > 0);

    AccelStructHandle blas = dev->create_blas({AccelStructKind::BottomLevel, blas_sizes.accel_struct_size, "dxr_blas"}, err);
    CHECK(blas.valid());
    CHECK(dev->accel_struct_device_address(blas) != 0);

    const uint64_t align = std::max<uint64_t>(caps.min_as_scratch_alignment, 256ull);
    AccelStructBuildSizes tlas_sizes{};
    CHECK(dev->query_tlas_sizes(1, AccelStructBuildFlags::PreferFastTrace, tlas_sizes));
    CHECK(tlas_sizes.accel_struct_size > 0 && tlas_sizes.build_scratch_size > 0);

    const uint64_t scratch_bytes = std::max(blas_sizes.build_scratch_size, tlas_sizes.build_scratch_size) + align;
    BufferHandle scratch = dev->create_buffer(
        {scratch_bytes, BufferUsage::Storage | BufferUsage::DeviceAddress, MemoryHint::GpuOnly, false, "dxr_scratch"}, err);
    CHECK(scratch.valid());
    const uint64_t scratch_addr = dev->buffer_device_address(scratch);
    const uint64_t aligned_off = (align - (scratch_addr % align)) % align;

    QueryPoolHandle compact_qp = dev->create_query_pool({QueryType::AccelStructCompactedSize, 1}, err);
    CHECK(compact_qp.valid());

    bool ok = dev->submit_once([&](CommandList& cmd) {
      cmd.reset_query_pool(compact_qp, 0, 1);
      BlasBuildDesc bdesc{};
      bdesc.dst = blas;
      bdesc.scratch_buffer = scratch;
      bdesc.scratch_offset = aligned_off;
      bdesc.geometries = std::span(&geom, 1);
      bdesc.flags = blas_flags;
      cmd.cmd_build_blas(bdesc);
      cmd.barrier({{}, {}, false, true, false});
      cmd.cmd_write_blas_compacted_size(blas, compact_qp, 0);
    }, err);
    CHECK(ok);

    uint64_t compacted_size = 0;
    CHECK(dev->get_query_pool_results(compact_qp, 0, 1, std::span(&compacted_size, 1), true));
    CHECK(compacted_size > 0);

    AccelStructHandle compacted_blas = dev->create_blas(
        {AccelStructKind::BottomLevel, compacted_size, "dxr_blas_compact"}, err);
    CHECK(compacted_blas.valid());
    const uint64_t compact_addr = dev->accel_struct_device_address(compacted_blas);
    CHECK(compact_addr != 0);

    TlasInstanceDesc inst{};
    inst.custom_index_24 = 42u;
    inst.mask_8 = 0xFFu;
    inst.blas_device_address = compact_addr;
    BufferHandle inst_buf = dev->create_buffer(
        {sizeof(TlasInstanceDesc), BufferUsage::AccelStructInput | BufferUsage::DeviceAddress,
         MemoryHint::CpuToGpu, false, "dxr_tlas_inst"}, err);
    CHECK(inst_buf.valid());
    std::memcpy(dev->map_buffer(inst_buf), &inst, sizeof(inst));
    dev->unmap_buffer(inst_buf);

    AccelStructHandle tlas = dev->create_tlas({AccelStructKind::TopLevel, tlas_sizes.accel_struct_size, "dxr_tlas"}, err);
    CHECK(tlas.valid());
    CHECK(dev->accel_struct_device_address(tlas) != 0);

    ok = dev->submit_once([&](CommandList& cmd) {
      cmd.cmd_copy_compact_blas(blas, compacted_blas);
      cmd.barrier({{}, {}, false, true, false});
      TlasBuildDesc tdesc{};
      tdesc.dst = tlas;
      tdesc.instance_buffer = inst_buf;
      tdesc.instance_count = 1;
      tdesc.scratch_buffer = scratch;
      tdesc.scratch_offset = aligned_off;
      tdesc.flags = AccelStructBuildFlags::PreferFastTrace;
      cmd.cmd_build_tlas(tdesc);
    }, err);
    CHECK(ok);

    std::printf("[rhi_dx12_tests] 5. DXR 1.1 BLAS (%llu B -> compacted %llu B) + TLAS build: OK\n",
                static_cast<unsigned long long>(blas_sizes.accel_struct_size),
                static_cast<unsigned long long>(compacted_size));

    dev->destroy_accel_struct(tlas);
    dev->destroy_buffer(inst_buf);
    dev->destroy_accel_struct(compacted_blas);
    dev->destroy_query_pool(compact_qp);
    dev->destroy_buffer(scratch);
    dev->destroy_accel_struct(blas);
    dev->destroy_buffer(ib);
    dev->destroy_buffer(vb);
  }

  // -------------------------------------------------------------------------
  // 6. Zero leaks, zero D3D12 debug layer errors, and PipelineLibrary round-trip
  // -------------------------------------------------------------------------
  dev->wait_idle();
  const VramStats stats = dev->vram_stats();
  CHECK(stats.allocation_count == 0);
  CHECK(dev->validation_message_count() == 0);

  CHECK(dev->save_pipeline_cache(err));
  const bool cache_written = std::filesystem::is_regular_file(cache_path, ec) &&
                             std::filesystem::file_size(cache_path, ec) > 0;
  CHECK(cache_written);
  dev.reset();

  if (cache_written) {
    std::unique_ptr<Device> dev2 = create_device(ddesc, err);
    CHECK(dev2 != nullptr);
    if (dev2) {
      CHECK(dev2->pipeline_cache_loaded());
      CHECK(dev2->validation_message_count() == 0);
      std::printf("[rhi_dx12_tests] 6. ID3D12PipelineLibrary cache round-trip + 0 debug layer errors: OK\n");
    }
  }
  std::filesystem::remove(cache_path, ec);
}

}  // namespace

int main() {
  test_live_d3d12_gpu();
  if (g_failures == 0) {
    std::printf("rhi_dx12_tests: ALL PASSED\n");
    return 0;
  }
  std::printf("rhi_dx12_tests: %d FAILED\n", g_failures);
  return 1;
}
