// Multi-queue (Graphics / AsyncCompute / Transfer), TimelineSemaphore,
// parallel CommandList recording, dispatch_indirect, D3D12 DXC SM 6.6 runtime
// compilation, and declarative rhi::RenderGraph hardware verification suite.
// Runs live against every compiled & available backend (Vulkan 1.3 + DirectX 12).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "fh1data/worker_pool.h"
#include "rhi/render_graph.h"
#include "rhi/rhi.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

// Verified SPIR-V 1.5 compute shader (Vulkan 1.3):
//   layout(local_size_x = 16) in;
//   layout(set = 0, binding = 0) readonly buffer InBuf { uint data[]; } src;
//   layout(set = 0, binding = 1) writeonly buffer OutBuf { uint data[]; } dst;
//   layout(push_constant) uniform Push { uint addend; } pc;
//   void main() { uint i = gl_GlobalInvocationID.x; dst.data[i] = src.data[i] * 2u + pc.addend; }
constexpr uint32_t kComputeDoubleSpv[326] = {
    0x07230203u, 0x00010500u, 0x0008000au, 0x0000002cu, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0009000fu, 0x00000005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000bu, 0x00000013u, 0x0000001au,
    0x00000023u, 0x00060010u, 0x00000004u, 0x00000011u, 0x00000010u, 0x00000001u, 0x00000001u, 0x00030003u,
    0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00030005u, 0x00000008u,
    0x00000069u, 0x00080005u, 0x0000000bu, 0x475f6c67u, 0x61626f6cu, 0x766e496cu, 0x7461636fu, 0x496e6f69u,
    0x00000044u, 0x00040005u, 0x00000011u, 0x4274754fu, 0x00006675u, 0x00050006u, 0x00000011u, 0x00000000u,
    0x61746164u, 0x00000000u, 0x00030005u, 0x00000013u, 0x00747364u, 0x00040005u, 0x00000018u, 0x75426e49u,
    0x00000066u, 0x00050006u, 0x00000018u, 0x00000000u, 0x61746164u, 0x00000000u, 0x00030005u, 0x0000001au,
    0x00637273u, 0x00040005u, 0x00000021u, 0x68737550u, 0x00000000u, 0x00050006u, 0x00000021u, 0x00000000u,
    0x65646461u, 0x0000646eu, 0x00030005u, 0x00000023u, 0x00006370u, 0x00040047u, 0x0000000bu, 0x0000000bu,
    0x0000001cu, 0x00040047u, 0x00000010u, 0x00000006u, 0x00000004u, 0x00040048u, 0x00000011u, 0x00000000u,
    0x00000019u, 0x00050048u, 0x00000011u, 0x00000000u, 0x00000023u, 0x00000000u, 0x00030047u, 0x00000011u,
    0x00000002u, 0x00040047u, 0x00000013u, 0x00000022u, 0x00000000u, 0x00040047u, 0x00000013u, 0x00000021u,
    0x00000001u, 0x00040047u, 0x00000017u, 0x00000006u, 0x00000004u, 0x00040048u, 0x00000018u, 0x00000000u,
    0x00000018u, 0x00050048u, 0x00000018u, 0x00000000u, 0x00000023u, 0x00000000u, 0x00030047u, 0x00000018u,
    0x00000002u, 0x00040047u, 0x0000001au, 0x00000022u, 0x00000000u, 0x00040047u, 0x0000001au, 0x00000021u,
    0x00000000u, 0x00050048u, 0x00000021u, 0x00000000u, 0x00000023u, 0x00000000u, 0x00030047u, 0x00000021u,
    0x00000002u, 0x00040047u, 0x0000002bu, 0x0000000bu, 0x00000019u, 0x00020013u, 0x00000002u, 0x00030021u,
    0x00000003u, 0x00000002u, 0x00040015u, 0x00000006u, 0x00000020u, 0x00000000u, 0x00040020u, 0x00000007u,
    0x00000007u, 0x00000006u, 0x00040017u, 0x00000009u, 0x00000006u, 0x00000003u, 0x00040020u, 0x0000000au,
    0x00000001u, 0x00000009u, 0x0004003bu, 0x0000000au, 0x0000000bu, 0x00000001u, 0x0004002bu, 0x00000006u,
    0x0000000cu, 0x00000000u, 0x00040020u, 0x0000000du, 0x00000001u, 0x00000006u, 0x0003001du, 0x00000010u,
    0x00000006u, 0x0003001eu, 0x00000011u, 0x00000010u, 0x00040020u, 0x00000012u, 0x0000000cu, 0x00000011u,
    0x0004003bu, 0x00000012u, 0x00000013u, 0x0000000cu, 0x00040015u, 0x00000014u, 0x00000020u, 0x00000001u,
    0x0004002bu, 0x00000014u, 0x00000015u, 0x00000000u, 0x0003001du, 0x00000017u, 0x00000006u, 0x0003001eu,
    0x00000018u, 0x00000017u, 0x00040020u, 0x00000019u, 0x0000000cu, 0x00000018u, 0x0004003bu, 0x00000019u,
    0x0000001au, 0x0000000cu, 0x00040020u, 0x0000001cu, 0x0000000cu, 0x00000006u, 0x0004002bu, 0x00000006u,
    0x0000001fu, 0x00000002u, 0x0003001eu, 0x00000021u, 0x00000006u, 0x00040020u, 0x00000022u, 0x00000009u,
    0x00000021u, 0x0004003bu, 0x00000022u, 0x00000023u, 0x00000009u, 0x00040020u, 0x00000024u, 0x00000009u,
    0x00000006u, 0x0004002bu, 0x00000006u, 0x00000029u, 0x00000010u, 0x0004002bu, 0x00000006u, 0x0000002au,
    0x00000001u, 0x0006002cu, 0x00000009u, 0x0000002bu, 0x00000029u, 0x0000002au, 0x0000002au, 0x00050036u,
    0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003bu, 0x00000007u,
    0x00000008u, 0x00000007u, 0x00050041u, 0x0000000du, 0x0000000eu, 0x0000000bu, 0x0000000cu, 0x0004003du,
    0x00000006u, 0x0000000fu, 0x0000000eu, 0x0003003eu, 0x00000008u, 0x0000000fu, 0x0004003du, 0x00000006u,
    0x00000016u, 0x00000008u, 0x0004003du, 0x00000006u, 0x0000001bu, 0x00000008u, 0x00060041u, 0x0000001cu,
    0x0000001du, 0x0000001au, 0x00000015u, 0x0000001bu, 0x0004003du, 0x00000006u, 0x0000001eu, 0x0000001du,
    0x00050084u, 0x00000006u, 0x00000020u, 0x0000001eu, 0x0000001fu, 0x00050041u, 0x00000024u, 0x00000025u,
    0x00000023u, 0x00000015u, 0x0004003du, 0x00000006u, 0x00000026u, 0x00000025u, 0x00050080u, 0x00000006u,
    0x00000027u, 0x00000020u, 0x00000026u, 0x00060041u, 0x0000001cu, 0x00000028u, 0x00000013u, 0x00000015u,
    0x00000016u, 0x0003003eu, 0x00000028u, 0x00000027u, 0x000100fdu, 0x00010038u,
};

// Equivalent HLSL SM 6.6 compute shader for DirectX 12 (compiled at runtime via dxcompiler.dll).
constexpr std::string_view kComputeDoubleHlsl = R"(
ByteAddressBuffer src_buf : register(t0, space0);
RWByteAddressBuffer dst_buf : register(u1, space0);
cbuffer Push : register(b0, space15) { uint addend; };

[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint off = tid.x * 4u;
  uint val = src_buf.Load(off);
  dst_buf.Store(off, val * 2u + addend);
}
)";

// SM 6.6-only HLSL using WaveActiveSum (impossible in SM 5.1 D3DCompile; proves DXC SM 6.6 path!).
constexpr std::string_view kWaveSumHlslSm66 = R"(
ByteAddressBuffer src_buf : register(t0, space0);
RWByteAddressBuffer dst_buf : register(u1, space0);

[numthreads(32, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint v = src_buf.Load(tid.x * 4u);
  uint lane_cnt = WaveGetLaneCount();
  uint sum = WaveActiveSum(v);
  dst_buf.Store(tid.x * 4u, sum + lane_cnt * 0u);
}
)";

// Equivalent MSL 3.1 compute shader for Apple Metal 3.
constexpr std::string_view kComputeDoubleMsl = R"(
// shift_local_size(16, 1, 1)
#include <metal_stdlib>
using namespace metal;
struct Push { uint addend; };
kernel void main0(device const uint* src [[buffer(0)]],
                  device uint* dst [[buffer(1)]],
                  constant Push& pc [[buffer(30)]],
                  uint tid [[thread_position_in_grid]]) {
  dst[tid] = src[tid] * 2u + pc.addend;
}
)";

shift::rhi::ShaderModuleDesc make_compute_double_shader(shift::rhi::BackendKind backend) {
  using namespace shift::rhi;
  ShaderModuleDesc sm{};
  sm.stage = ShaderStage::Compute;
  sm.entry_point = "main";
  if (backend == BackendKind::Vulkan) {
    sm.spirv = std::span<const uint32_t>(kComputeDoubleSpv, std::size(kComputeDoubleSpv));
  } else if (backend == BackendKind::D3D12) {
    sm.dxil = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(kComputeDoubleHlsl.data()), kComputeDoubleHlsl.size());
  } else if (backend == BackendKind::Metal) {
    sm.msl = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(kComputeDoubleMsl.data()), kComputeDoubleMsl.size());
  }
  return sm;
}

void run_backend_suite(shift::rhi::BackendKind backend) {
  using namespace shift::rhi;

  DeviceDesc dd{};
  dd.backend = backend;
  dd.enable_validation = true;
  dd.enable_async_compute = true;
  dd.enable_transfer_queue = true;
  dd.enable_ray_tracing = false;
  std::string err;
  auto dev = create_device(dd, err);
  if (!dev) {
    std::printf("[%s] skipped (device unavailable: %s)\n", backend_kind_name(backend), err.c_str());
    return;
  }

  const BackendCaps& caps = dev->caps();
  std::printf("[%s] testing on %s (timeline=%d, async_compute=%d, transfer=%d, subgroups=%d)\n",
              backend_kind_name(backend), caps.device_name.c_str(),
              caps.timeline_semaphores ? 1 : 0,
              caps.dedicated_async_compute ? 1 : 0,
              caps.dedicated_transfer_queue ? 1 : 0,
              caps.subgroup_ops ? 1 : 0);

  CHECK(caps.timeline_semaphores);

  // -------------------------------------------------------------------------
  // 1. TimelineSemaphore CPU signal / query / wait lifecycle
  // -------------------------------------------------------------------------
  {
    TimelineSemaphoreHandle sem = dev->create_timeline_semaphore(5, err);
    CHECK(sem.valid());
    CHECK(dev->timeline_semaphore_value(sem) == 5);
    CHECK(dev->wait_timeline_semaphore(sem, 5, 1000000000ull));
    CHECK(dev->signal_timeline_semaphore(sem, 12));
    CHECK(dev->timeline_semaphore_value(sem) == 12);
    CHECK(dev->wait_timeline_semaphore(sem, 12, 1000000000ull));
    dev->destroy_timeline_semaphore(sem);
  }

  // Create compute pipeline shared by cross-queue and RenderGraph tests.
  BindGroupLayoutBinding bindings[2]{};
  bindings[0].binding = 0;
  bindings[0].type = DescriptorType::StorageBuffer;
  bindings[0].stages = ShaderStage::Compute;
  bindings[1].binding = 1;
  bindings[1].type = DescriptorType::StorageBuffer;
  bindings[1].stages = ShaderStage::Compute;

  BindGroupLayoutHandle bgl = dev->create_bind_group_layout({bindings, false}, err);
  CHECK(bgl.valid());

  PushConstantRange pcr{ShaderStage::Compute, 0, sizeof(uint32_t)};
  PipelineLayoutHandle pl = dev->create_pipeline_layout(
      {std::span(&bgl, 1), std::span(&pcr, 1)}, err);
  CHECK(pl.valid());

  ComputePipelineDesc cpd{};
  cpd.layout = pl;
  cpd.compute_shader = make_compute_double_shader(backend);
  PipelineHandle pipe = dev->create_compute_pipeline(cpd, err);
  if (!pipe) {
    std::printf("[%s] compute pipeline failed: %s\n", backend_kind_name(backend), err.c_str());
  }
  CHECK(pipe.valid());

  // -------------------------------------------------------------------------
  // 2. Real Cross-Queue Timeline Pipeline:
  //    Transfer (upload) -> timeline(1) -> AsyncCompute (dispatch_indirect)
  //    -> timeline(2) -> Graphics (readback copy) -> timeline(3) -> CPU verify
  // -------------------------------------------------------------------------
  if (pipe) {
    constexpr uint32_t kCount = 64;
    constexpr uint64_t kBytes = kCount * sizeof(uint32_t);
    constexpr uint32_t kAddend = 7;

    BufferHandle stage = dev->create_buffer(
        {kBytes, BufferUsage::TransferSrc, MemoryHint::CpuToGpu, true, "xq_stage"}, err);
    BufferHandle ind_stage = dev->create_buffer(
        {16, BufferUsage::TransferSrc, MemoryHint::CpuToGpu, true, "xq_ind_stage"}, err);
    BufferHandle gpu_in = dev->create_buffer(
        {kBytes, BufferUsage::TransferDst | BufferUsage::Storage, MemoryHint::GpuOnly, false, "xq_in"}, err);
    BufferHandle gpu_out = dev->create_buffer(
        {kBytes, BufferUsage::TransferSrc | BufferUsage::Storage, MemoryHint::GpuOnly, false, "xq_out"}, err);
    BufferHandle gpu_ind = dev->create_buffer(
        {16, BufferUsage::TransferDst | BufferUsage::Indirect, MemoryHint::GpuOnly, false, "xq_ind"}, err);
    BufferHandle readback = dev->create_buffer(
        {kBytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "xq_rb"}, err);
    CHECK(stage && ind_stage && gpu_in && gpu_out && gpu_ind && readback);

    auto* src_ptr = static_cast<uint32_t*>(dev->map_buffer(stage));
    CHECK(src_ptr != nullptr);
    for (uint32_t i = 0; i < kCount; ++i) src_ptr[i] = 100u + i * 3u;

    auto* ind_ptr = static_cast<uint32_t*>(dev->map_buffer(ind_stage));
    CHECK(ind_ptr != nullptr);
    ind_ptr[0] = kCount / 16u;  // 4 workgroups
    ind_ptr[1] = 1u;
    ind_ptr[2] = 1u;
    ind_ptr[3] = 0u;

    BindGroupEntry entries[2]{};
    entries[0].binding = 0;
    entries[0].type = DescriptorType::StorageBuffer;
    entries[0].buffer = gpu_in;
    entries[1].binding = 1;
    entries[1].type = DescriptorType::StorageBuffer;
    entries[1].buffer = gpu_out;
    BindGroupHandle bg = dev->create_bind_group({bgl, entries}, err);
    CHECK(bg.valid());

    TimelineSemaphoreHandle sem = dev->create_timeline_semaphore(0, err);
    auto cl_xfer = dev->create_command_list(QueueKind::Transfer, err);
    auto cl_comp = dev->create_command_list(QueueKind::AsyncCompute, err);
    auto cl_gfx = dev->create_command_list(QueueKind::Graphics, err);
    CHECK(sem && cl_xfer && cl_comp && cl_gfx);

    // Record all 3 command lists in parallel on fh1::JobSystem workers!
    fh1::parallel_for_frame(3, [&](size_t idx) {
      if (idx == 0) {
        cl_xfer->begin();
        cl_xfer->copy_buffer(stage, 0, gpu_in, 0, kBytes);
        cl_xfer->copy_buffer(ind_stage, 0, gpu_ind, 0, 12);
        cl_xfer->end();
      } else if (idx == 1) {
        cl_comp->begin();
        BufferBarrier bbs[2] = {
            {gpu_in, ResourceState::CopyDst, ResourceState::ShaderRead},
            {gpu_ind, ResourceState::CopyDst, ResourceState::IndirectArg},
        };
        cl_comp->barrier({{}, bbs});
        cl_comp->bind_pipeline(pipe);
        cl_comp->bind_group(0, bg);
        cl_comp->push_constants(ShaderStage::Compute, 0, sizeof(uint32_t), &kAddend);
        cl_comp->dispatch_indirect(gpu_ind, 0);
        BufferBarrier out_b{gpu_out, ResourceState::StorageReadWrite, ResourceState::CopySrc};
        cl_comp->barrier({{}, std::span(&out_b, 1)});
        cl_comp->end();
      } else {
        cl_gfx->begin();
        cl_gfx->copy_buffer(gpu_out, 0, readback, 0, kBytes);
        cl_gfx->barrier({{}, {}, false, false, true});
        cl_gfx->end();
      }
    }, /*min_parallel=*/2);

    CommandList* l0 = cl_xfer.get();
    TimelineSignal s1{sem, 1, PipelineStage::Transfer};
    CHECK(dev->submit(QueueKind::Transfer, std::span(&l0, 1), {}, std::span(&s1, 1), err));

    CommandList* l1 = cl_comp.get();
    TimelineWait w1{sem, 1, PipelineStage::ComputeShader};
    TimelineSignal s2{sem, 2, PipelineStage::ComputeShader};
    CHECK(dev->submit(QueueKind::AsyncCompute, std::span(&l1, 1), std::span(&w1, 1),
                      std::span(&s2, 1), err));

    CommandList* l2 = cl_gfx.get();
    TimelineWait w2{sem, 2, PipelineStage::Transfer};
    TimelineSignal s3{sem, 3, PipelineStage::AllCommands};
    CHECK(dev->submit(QueueKind::Graphics, std::span(&l2, 1), std::span(&w2, 1),
                      std::span(&s3, 1), err));

    CHECK(dev->wait_timeline_semaphore(sem, 3, 5000000000ull));
    CHECK(dev->timeline_semaphore_value(sem) >= 3);

    auto* dst_ptr = static_cast<uint32_t*>(dev->map_buffer(readback));
    CHECK(dst_ptr != nullptr);
    if (dst_ptr) {
      for (uint32_t i = 0; i < kCount; ++i) {
        const uint32_t expected = (100u + i * 3u) * 2u + kAddend;
        if (dst_ptr[i] != expected) {
          std::printf("[%s] cross-queue mismatch at %u: got %u expected %u\n",
                      backend_kind_name(backend), i, dst_ptr[i], expected);
          CHECK(dst_ptr[i] == expected);
          break;
        }
      }
      dev->unmap_buffer(readback);
    }

    dev->destroy_timeline_semaphore(sem);
    dev->destroy_bind_group(bg);
    dev->destroy_buffer(readback);
    dev->destroy_buffer(gpu_ind);
    dev->destroy_buffer(gpu_out);
    dev->destroy_buffer(gpu_in);
    dev->destroy_buffer(ind_stage);
    dev->destroy_buffer(stage);
  }

  // -------------------------------------------------------------------------
  // 3. Declarative rhi::RenderGraph: culling, transient aliasing, multi-queue
  //    cross-queue timeline execution, and warm pool reuse across 2 frames
  // -------------------------------------------------------------------------
  if (pipe) {
    constexpr uint32_t kCount = 64;
    constexpr uint64_t kBytes = kCount * sizeof(uint32_t);

    BufferHandle stage = dev->create_buffer(
        {kBytes, BufferUsage::TransferSrc, MemoryHint::CpuToGpu, true, "rg_stage"}, err);
    BufferHandle readback = dev->create_buffer(
        {kBytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "rg_rb"}, err);
    CHECK(stage && readback);

    auto* src_ptr = static_cast<uint32_t*>(dev->map_buffer(stage));
    for (uint32_t i = 0; i < kCount; ++i) src_ptr[i] = 10u + i;

    RenderGraph rg(*dev);

    for (uint32_t frame = 0; frame < 2; ++frame) {
      rg.reset();
      const uint32_t addend = (frame == 0) ? 13u : 29u;
      std::atomic<int> culled_ran{0};

      RGBufferHandle rg_stage = rg.import_buffer("stage", stage, ResourceState::CopySrc);
      RGBufferHandle rg_rb =
          rg.import_buffer("readback", readback, ResourceState::CopyDst, ResourceState::HostRead);

      BufferDesc transient_desc{
          kBytes,
          BufferUsage::TransferSrc | BufferUsage::TransferDst | BufferUsage::Storage,
          MemoryHint::GpuOnly,
          false,
          "rg_transient"};
      RGBufferHandle rg_buf_in = rg.create_buffer("buf_in", transient_desc);
      RGBufferHandle rg_buf_out = rg.create_buffer("buf_out", transient_desc);
      // Third transient buffer whose lifetime starts only AFTER buf_in's last use -> aliases buf_in!
      RGBufferHandle rg_buf_alias = rg.create_buffer("buf_alias", transient_desc);

      // Unused transient texture + dead pass -> must be culled!
      TextureDesc tex_desc{};
      tex_desc.format = Format::Rgba8Unorm;
      tex_desc.width = 16;
      tex_desc.height = 16;
      tex_desc.usage = TextureUsage::ColorAttachment | TextureUsage::Sampled;
      RGTextureHandle rg_unused_tex = rg.create_texture("unused_tex", tex_desc);

      // Pair of non-overlapping transient textures feeding the chain to verify texture aliasing.
      RGTextureHandle rg_t0 = rg.create_texture("t0", tex_desc);
      RGTextureHandle rg_t1 = rg.create_texture("t1", tex_desc);
      RGTextureHandle rg_t2 = rg.create_texture("t2", tex_desc);

      TextureHandle resolved_t0{};
      TextureHandle resolved_t2{};
      BufferHandle resolved_in{};
      BufferHandle resolved_alias{};

      // Dead pass (writes only unconsumed texture, no side effects -> culled).
      rg.add_pass(
          "DeadPass", QueueKind::Graphics,
          [&](RenderGraphBuilder& b) { b.write_texture(rg_unused_tex, ResourceState::RenderTarget); },
          [&](CommandList&, const RenderGraphContext&) { culled_ran.fetch_add(1); });

      // Pass 0 (Transfer): stage -> rg_buf_in
      rg.add_pass(
          "UploadPass", QueueKind::Transfer,
          [&](RenderGraphBuilder& b) {
            b.read_buffer(rg_stage, ResourceState::CopySrc);
            b.write_buffer(rg_buf_in, ResourceState::CopyDst);
          },
          [&](CommandList& cmd, const RenderGraphContext& ctx) {
            resolved_in = ctx.buffer(rg_buf_in);
            cmd.copy_buffer(ctx.buffer(rg_stage), 0, ctx.buffer(rg_buf_in), 0, kBytes);
          });

      // Pass 1 (AsyncCompute): rg_buf_in -> rg_buf_out (dst = src * 2 + addend)
      BindGroupHandle frame_bg{};
      rg.add_pass(
          "ComputePass", QueueKind::AsyncCompute,
          [&](RenderGraphBuilder& b) {
            b.read_buffer(rg_buf_in, ResourceState::ShaderRead);
            b.write_buffer(rg_buf_out, ResourceState::StorageReadWrite);
          },
          [&](CommandList& cmd, const RenderGraphContext& ctx) {
            BindGroupEntry ents[2]{};
            ents[0].binding = 0;
            ents[0].type = DescriptorType::StorageBuffer;
            ents[0].buffer = ctx.buffer(rg_buf_in);
            ents[1].binding = 1;
            ents[1].type = DescriptorType::StorageBuffer;
            ents[1].buffer = ctx.buffer(rg_buf_out);
            std::string bg_err;
            frame_bg = dev->create_bind_group({bgl, ents}, bg_err);
            if (!frame_bg) return;
            cmd.bind_pipeline(pipe);
            cmd.bind_group(0, frame_bg);
            cmd.push_constants(ShaderStage::Compute, 0, sizeof(uint32_t), &addend);
            cmd.dispatch(kCount / 16u, 1, 1);
          });

      // Pass 2 (Graphics): reads rg_buf_out -> copies to rg_buf_alias (which aliases rg_buf_in!)
      // and touches t0 -> t1 -> t2 chain to verify texture aliasing too.
      rg.add_pass(
          "AliasCopyPass", QueueKind::Graphics,
          [&](RenderGraphBuilder& b) {
            b.read_buffer(rg_buf_out, ResourceState::CopySrc);
            b.write_buffer(rg_buf_alias, ResourceState::CopyDst);
            b.write_texture(rg_t0, ResourceState::RenderTarget);
          },
          [&](CommandList& cmd, const RenderGraphContext& ctx) {
            resolved_alias = ctx.buffer(rg_buf_alias);
            resolved_t0 = ctx.texture(rg_t0);
            cmd.copy_buffer(ctx.buffer(rg_buf_out), 0, ctx.buffer(rg_buf_alias), 0, kBytes);
          });

      rg.add_pass(
          "TexChainMid", QueueKind::Graphics,
          [&](RenderGraphBuilder& b) {
            b.read_texture(rg_t0, ResourceState::ShaderRead);
            b.write_texture(rg_t1, ResourceState::RenderTarget);
          },
          [&](CommandList&, const RenderGraphContext&) {});

      rg.add_pass(
          "ReadbackPass", QueueKind::Graphics,
          [&](RenderGraphBuilder& b) {
            b.read_buffer(rg_buf_alias, ResourceState::CopySrc);
            b.read_texture(rg_t1, ResourceState::ShaderRead);
            b.write_texture(rg_t2, ResourceState::RenderTarget);
            b.write_buffer(rg_rb, ResourceState::CopyDst);
          },
          [&](CommandList& cmd, const RenderGraphContext& ctx) {
            resolved_t2 = ctx.texture(rg_t2);
            cmd.copy_buffer(ctx.buffer(rg_buf_alias), 0, ctx.buffer(rg_rb), 0, kBytes);
          });

      CHECK(rg.compile(err));
      const RenderGraphStats& st = rg.stats();
      CHECK(st.culled_passes == 1);
      CHECK(st.executed_passes == 5);
      CHECK(st.aliased_buffers >= 1);
      CHECK(st.physical_buffers_allocated == 2);  // 3 logical buffers -> 2 physical buffers!
      CHECK(st.aliased_textures >= 1);
      CHECK(st.physical_textures_allocated == 2);  // 3 logical textures -> 2 physical textures!
      CHECK(st.queue_submissions == 3);  // Transfer -> AsyncCompute -> Graphics
      CHECK(st.cross_queue_syncs >= 2);

      uint64_t final_val = 0;
      CHECK(rg.execute(err, true, &final_val));
      CHECK(final_val > 0);
      CHECK(culled_ran.load() == 0);
      CHECK(resolved_in.valid() && resolved_alias == resolved_in);
      CHECK(resolved_t0.valid() && resolved_t2 == resolved_t0);

      auto* dst_ptr = static_cast<uint32_t*>(dev->map_buffer(readback));
      CHECK(dst_ptr != nullptr);
      if (dst_ptr) {
        for (uint32_t i = 0; i < kCount; ++i) {
          const uint32_t expected = (10u + i) * 2u + addend;
          if (dst_ptr[i] != expected) {
            std::printf("[%s] RenderGraph frame %u mismatch at %u: got %u expected %u\n",
                        backend_kind_name(backend), frame, i, dst_ptr[i], expected);
            CHECK(dst_ptr[i] == expected);
            break;
          }
        }
        dev->unmap_buffer(readback);
      }

      if (frame_bg) dev->destroy_bind_group(frame_bg);
    }

    rg.release_pool();
    dev->destroy_buffer(readback);
    dev->destroy_buffer(stage);
  }

  // -------------------------------------------------------------------------
  // 4. D3D12-specific: Runtime DXC SM 6.6 WaveActiveSum compilation & execution
  // -------------------------------------------------------------------------
  if (backend == BackendKind::D3D12 && caps.subgroup_ops) {
    ComputePipelineDesc wpd{};
    wpd.layout = pl;
    wpd.compute_shader.stage = ShaderStage::Compute;
    wpd.compute_shader.entry_point = "main";
    wpd.compute_shader.dxil = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(kWaveSumHlslSm66.data()), kWaveSumHlslSm66.size());
    std::string dxc_err;
    PipelineHandle wave_pipe = dev->create_compute_pipeline(wpd, dxc_err);
    if (!wave_pipe) {
      std::printf("[d3d12] DXC SM 6.6 WaveActiveSum compile failed: %s\n", dxc_err.c_str());
    }
    CHECK(wave_pipe.valid());
    if (wave_pipe) {
      constexpr uint32_t kLanes = 32;
      constexpr uint64_t kBytes = kLanes * sizeof(uint32_t);
      BufferHandle src = dev->create_buffer(
          {kBytes, BufferUsage::TransferSrc, MemoryHint::CpuToGpu, true, "wave_src"}, err);
      BufferHandle gpu_in = dev->create_buffer(
          {kBytes, BufferUsage::TransferDst | BufferUsage::Storage, MemoryHint::GpuOnly, false, "wave_in"}, err);
      BufferHandle gpu_out = dev->create_buffer(
          {kBytes, BufferUsage::TransferSrc | BufferUsage::Storage, MemoryHint::GpuOnly, false, "wave_out"}, err);
      BufferHandle rb = dev->create_buffer(
          {kBytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "wave_rb"}, err);

      auto* p = static_cast<uint32_t*>(dev->map_buffer(src));
      for (uint32_t i = 0; i < kLanes; ++i) p[i] = 1u;

      BindGroupEntry ents[2]{};
      ents[0].binding = 0;
      ents[0].type = DescriptorType::StorageBuffer;
      ents[0].buffer = gpu_in;
      ents[1].binding = 1;
      ents[1].type = DescriptorType::StorageBuffer;
      ents[1].buffer = gpu_out;
      BindGroupHandle bg = dev->create_bind_group({bgl, ents}, err);

      bool ok = dev->submit_once([&](CommandList& cmd) {
        cmd.copy_buffer(src, 0, gpu_in, 0, kBytes);
        BufferBarrier b0{gpu_in, ResourceState::CopyDst, ResourceState::ShaderRead};
        cmd.barrier({{}, std::span(&b0, 1)});
        cmd.bind_pipeline(wave_pipe);
        cmd.bind_group(0, bg);
        cmd.dispatch(1, 1, 1);
        BufferBarrier b1{gpu_out, ResourceState::StorageReadWrite, ResourceState::CopySrc};
        cmd.barrier({{}, std::span(&b1, 1)});
        cmd.copy_buffer(gpu_out, 0, rb, 0, kBytes);
      }, err);
      CHECK(ok);

      auto* out = static_cast<uint32_t*>(dev->map_buffer(rb));
      CHECK(out != nullptr);
      if (out) {
        // WaveActiveSum of all 1s equals the hardware wave size (e.g., 32 on NVIDIA, 32/64 on AMD/Intel).
        CHECK(out[0] >= 4u && out[0] <= 64u);
        dev->unmap_buffer(rb);
      }

      dev->destroy_bind_group(bg);
      dev->destroy_buffer(rb);
      dev->destroy_buffer(gpu_out);
      dev->destroy_buffer(gpu_in);
      dev->destroy_buffer(src);
      dev->destroy_pipeline(wave_pipe);
    }
  }

  dev->wait_idle();
  if (pipe) dev->destroy_pipeline(pipe);
  if (pl) dev->destroy_pipeline_layout(pl);
  if (bgl) dev->destroy_bind_group_layout(bgl);

  CHECK(dev->validation_message_count() == 0);
  std::printf("[%s] multi-queue + timelines + RenderGraph suite complete (validation_msgs=%u)\n",
              backend_kind_name(backend), dev->validation_message_count());
}

}  // namespace

int main() {
  using namespace shift::rhi;
  if (is_backend_compiled(BackendKind::Vulkan)) {
    run_backend_suite(BackendKind::Vulkan);
  }
  if (is_backend_compiled(BackendKind::D3D12)) {
    run_backend_suite(BackendKind::D3D12);
  }
  if (is_backend_compiled(BackendKind::Metal)) {
    run_backend_suite(BackendKind::Metal);
  }
  if (g_failures > 0) {
    std::printf("rhi_render_graph_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("rhi_render_graph_tests: ALL PASSED\n");
  return 0;
}
