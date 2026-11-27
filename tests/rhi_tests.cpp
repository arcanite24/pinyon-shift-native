#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

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

// Verified SPIR-V 1.5 (Vulkan 1.2/1.3) compute shader:
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

// Verified SPIR-V 1.5 fullscreen triangle vertex shader at z = 0.5:
constexpr uint32_t kFullscreenVertSpv[274] = {
    0x07230203u, 0x00010500u, 0x0008000au, 0x00000029u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0007000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000cu, 0x0000001fu, 0x00030003u,
    0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00030005u, 0x00000009u,
    0x00736f70u, 0x00060005u, 0x0000000cu, 0x565f6c67u, 0x65747265u, 0x646e4978u, 0x00007865u, 0x00060005u,
    0x0000001du, 0x505f6c67u, 0x65567265u, 0x78657472u, 0x00000000u, 0x00060006u, 0x0000001du, 0x00000000u,
    0x505f6c67u, 0x7469736fu, 0x006e6f69u, 0x00070006u, 0x0000001du, 0x00000001u, 0x505f6c67u, 0x746e696fu,
    0x657a6953u, 0x00000000u, 0x00070006u, 0x0000001du, 0x00000002u, 0x435f6c67u, 0x4470696cu, 0x61747369u,
    0x0065636eu, 0x00070006u, 0x0000001du, 0x00000003u, 0x435f6c67u, 0x446c6c75u, 0x61747369u, 0x0065636eu,
    0x00030005u, 0x0000001fu, 0x00000000u, 0x00040047u, 0x0000000cu, 0x0000000bu, 0x0000002au, 0x00050048u,
    0x0000001du, 0x00000000u, 0x0000000bu, 0x00000000u, 0x00050048u, 0x0000001du, 0x00000001u, 0x0000000bu,
    0x00000001u, 0x00050048u, 0x0000001du, 0x00000002u, 0x0000000bu, 0x00000003u, 0x00050048u, 0x0000001du,
    0x00000003u, 0x0000000bu, 0x00000004u, 0x00030047u, 0x0000001du, 0x00000002u, 0x00020013u, 0x00000002u,
    0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u,
    0x00000006u, 0x00000002u, 0x00040020u, 0x00000008u, 0x00000007u, 0x00000007u, 0x00040015u, 0x0000000au,
    0x00000020u, 0x00000001u, 0x00040020u, 0x0000000bu, 0x00000001u, 0x0000000au, 0x0004003bu, 0x0000000bu,
    0x0000000cu, 0x00000001u, 0x0004002bu, 0x0000000au, 0x0000000eu, 0x00000001u, 0x00020014u, 0x0000000fu,
    0x0004002bu, 0x00000006u, 0x00000011u, 0x40400000u, 0x0004002bu, 0x00000006u, 0x00000012u, 0xbf800000u,
    0x0004002bu, 0x0000000au, 0x00000015u, 0x00000002u, 0x00040017u, 0x00000019u, 0x00000006u, 0x00000004u,
    0x00040015u, 0x0000001au, 0x00000020u, 0x00000000u, 0x0004002bu, 0x0000001au, 0x0000001bu, 0x00000001u,
    0x0004001cu, 0x0000001cu, 0x00000006u, 0x0000001bu, 0x0006001eu, 0x0000001du, 0x00000019u, 0x00000006u,
    0x0000001cu, 0x0000001cu, 0x00040020u, 0x0000001eu, 0x00000003u, 0x0000001du, 0x0004003bu, 0x0000001eu,
    0x0000001fu, 0x00000003u, 0x0004002bu, 0x0000000au, 0x00000020u, 0x00000000u, 0x0004002bu, 0x00000006u,
    0x00000022u, 0x3f000000u, 0x0004002bu, 0x00000006u, 0x00000023u, 0x3f800000u, 0x00040020u, 0x00000027u,
    0x00000003u, 0x00000019u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
    0x00000005u, 0x0004003bu, 0x00000008u, 0x00000009u, 0x00000007u, 0x0004003du, 0x0000000au, 0x0000000du,
    0x0000000cu, 0x000500aau, 0x0000000fu, 0x00000010u, 0x0000000du, 0x0000000eu, 0x000600a9u, 0x00000006u,
    0x00000013u, 0x00000010u, 0x00000011u, 0x00000012u, 0x0004003du, 0x0000000au, 0x00000014u, 0x0000000cu,
    0x000500aau, 0x0000000fu, 0x00000016u, 0x00000014u, 0x00000015u, 0x000600a9u, 0x00000006u, 0x00000017u,
    0x00000016u, 0x00000011u, 0x00000012u, 0x00050050u, 0x00000007u, 0x00000018u, 0x00000013u, 0x00000017u,
    0x0003003eu, 0x00000009u, 0x00000018u, 0x0004003du, 0x00000007u, 0x00000021u, 0x00000009u, 0x00050051u,
    0x00000006u, 0x00000024u, 0x00000021u, 0x00000000u, 0x00050051u, 0x00000006u, 0x00000025u, 0x00000021u,
    0x00000001u, 0x00070050u, 0x00000019u, 0x00000026u, 0x00000024u, 0x00000025u, 0x00000022u, 0x00000023u,
    0x00050041u, 0x00000027u, 0x00000028u, 0x0000001fu, 0x00000020u, 0x0003003eu, 0x00000028u, 0x00000026u,
    0x000100fdu, 0x00010038u,
};

// Verified SPIR-V 1.5 fragment shader writing push-constant vec4 color:
constexpr uint32_t kSolidColorFragSpv[126] = {
    0x07230203u, 0x00010500u, 0x0008000au, 0x00000012u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0007000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x0000000cu, 0x00030010u,
    0x00000004u, 0x00000007u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du,
    0x00000000u, 0x00050005u, 0x00000009u, 0x5f74756fu, 0x6f6c6f63u, 0x00000072u, 0x00040005u, 0x0000000au,
    0x68737550u, 0x00000000u, 0x00050006u, 0x0000000au, 0x00000000u, 0x6f6c6f63u, 0x00000072u, 0x00030005u,
    0x0000000cu, 0x00006370u, 0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00050048u, 0x0000000au,
    0x00000000u, 0x00000023u, 0x00000000u, 0x00030047u, 0x0000000au, 0x00000002u, 0x00020013u, 0x00000002u,
    0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u,
    0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u, 0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u,
    0x00000009u, 0x00000003u, 0x0003001eu, 0x0000000au, 0x00000007u, 0x00040020u, 0x0000000bu, 0x00000009u,
    0x0000000au, 0x0004003bu, 0x0000000bu, 0x0000000cu, 0x00000009u, 0x00040015u, 0x0000000du, 0x00000020u,
    0x00000001u, 0x0004002bu, 0x0000000du, 0x0000000eu, 0x00000000u, 0x00040020u, 0x0000000fu, 0x00000009u,
    0x00000007u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u,
    0x00050041u, 0x0000000fu, 0x00000010u, 0x0000000cu, 0x0000000eu, 0x0004003du, 0x00000007u, 0x00000011u,
    0x00000010u, 0x0003003eu, 0x00000009u, 0x00000011u, 0x000100fdu, 0x00010038u,
};

void test_backend_parsing_and_selection() {
  using namespace shift::rhi;
  CHECK(std::strcmp(backend_kind_name(BackendKind::Auto), "auto") == 0);
  CHECK(std::strcmp(backend_kind_name(BackendKind::Vulkan), "vulkan") == 0);
  CHECK(std::strcmp(backend_kind_name(BackendKind::Metal), "metal") == 0);
  CHECK(std::strcmp(backend_kind_name(BackendKind::D3D12), "d3d12") == 0);

  CHECK(parse_backend_kind("auto") == BackendKind::Auto);
  CHECK(parse_backend_kind("  VULKAN  ") == BackendKind::Vulkan);
  CHECK(parse_backend_kind("vk") == BackendKind::Vulkan);
  CHECK(parse_backend_kind("Metal") == BackendKind::Metal);
  CHECK(parse_backend_kind("mtl") == BackendKind::Metal);
  CHECK(parse_backend_kind("d3d12") == BackendKind::D3D12);
  CHECK(parse_backend_kind("DX12") == BackendKind::D3D12);
  CHECK(parse_backend_kind("DirectX12") == BackendKind::D3D12);
  CHECK(parse_backend_kind("unknown_api", BackendKind::Vulkan) == BackendKind::Vulkan);

  BackendKind out = BackendKind::Auto;
  CHECK(!try_parse_backend_kind("", out));
  CHECK(!try_parse_backend_kind("opengl", out));
  CHECK(try_parse_backend_kind("dx12", out) && out == BackendKind::D3D12);

  CHECK(is_backend_compiled(BackendKind::Auto));
  const auto auto_order = resolve_backend_order(BackendKind::Auto);
  CHECK(!auto_order.empty());
  if (is_backend_compiled(BackendKind::Vulkan)) {
    const auto vk_order = resolve_backend_order(BackendKind::Vulkan);
    CHECK(vk_order.size() == 1 && vk_order[0] == BackendKind::Vulkan);
  }
  if (is_backend_compiled(BackendKind::Metal)) {
    const auto mtl_order = resolve_backend_order(BackendKind::Metal);
    CHECK(mtl_order.size() == 1 && mtl_order[0] == BackendKind::Metal);
#if defined(__APPLE__)
    CHECK(auto_order[0] == BackendKind::Metal);
#endif
  }

  // Requesting an uncompiled backend fails gracefully with an informative error.
  if (!is_backend_compiled(BackendKind::Metal)) {
    DeviceDesc d{};
    d.backend = BackendKind::Metal;
    std::string err;
    auto dev = create_device(d, err);
    CHECK(dev == nullptr);
    CHECK(!err.empty());
  }
}

void test_format_helpers() {
  using namespace shift::rhi;
  CHECK(format_bytes_per_block(Format::Undefined) == 0);
  CHECK(format_bytes_per_block(Format::R8Unorm) == 1);
  CHECK(format_bytes_per_block(Format::Rgba8Unorm) == 4);
  CHECK(format_bytes_per_block(Format::Rgba8Srgb) == 4);
  CHECK(format_bytes_per_block(Format::Bgra8Srgb) == 4);
  CHECK(format_bytes_per_block(Format::Rgb10A2Unorm) == 4);
  CHECK(format_bytes_per_block(Format::Rgba16Sfloat) == 8);
  CHECK(format_bytes_per_block(Format::Rgb32Sfloat) == 12);
  CHECK(format_bytes_per_block(Format::Rgba32Sfloat) == 16);
  CHECK(format_bytes_per_block(Format::D32Sfloat) == 4);
  CHECK(format_bytes_per_block(Format::D16Unorm) == 2);
  CHECK(format_bytes_per_block(Format::Bc1RgbaUnorm) == 8);
  CHECK(format_bytes_per_block(Format::Bc3Srgb) == 16);
  CHECK(format_bytes_per_block(Format::Bc4Unorm) == 8);
  CHECK(format_bytes_per_block(Format::Bc5Unorm) == 16);

  uint32_t depth_count = 0, srgb_count = 0, bc_count = 0;
  for (uint32_t i = 1; i < kFormatCount; ++i) {
    const auto f = static_cast<Format>(i);
    CHECK(format_name(f) != nullptr && format_name(f)[0] != '\0');
    CHECK(format_bytes_per_block(f) > 0);
    if (format_is_depth(f)) ++depth_count;
    if (format_is_srgb(f)) ++srgb_count;
    if (format_is_compressed(f)) {
      ++bc_count;
      CHECK(format_block_extent(f) == 4);
    } else {
      CHECK(format_block_extent(f) == 1);
    }
  }
  CHECK(depth_count == 2);
  CHECK(srgb_count == 5);   // Rgba8Srgb, Bgra8Srgb, Bc1Srgb, Bc2Srgb, Bc3Srgb
  CHECK(bc_count == 8);     // Bc1x2, Bc2x2, Bc3x2, Bc4, Bc5
  CHECK(format_image_bytes(Format::Rgba8Unorm, 16, 8) == 16ull * 8ull * 4ull);
  CHECK(format_image_bytes(Format::Bc1RgbaUnorm, 5, 5) == 2ull * 2ull * 8ull);
  CHECK(format_image_bytes(Format::Bc3Unorm, 4, 4) == 16ull);
  CHECK(format_image_bytes(Format::Undefined, 16, 16) == 0ull);
}

void test_handle_generation_safety() {
  using namespace shift::rhi;
  HandlePool<BufferHandle, int> pool;
  CHECK(pool.live_count() == 0);
  CHECK(pool.get(BufferHandle{}) == nullptr);

  BufferHandle h1 = pool.allocate(101);
  BufferHandle h2 = pool.allocate(202);
  CHECK(h1.valid() && h2.valid() && h1 != h2);
  CHECK(pool.live_count() == 2);
  CHECK(pool.get(h1) != nullptr && *pool.get(h1) == 101);
  CHECK(pool.get(h2) != nullptr && *pool.get(h2) == 202);

  // Raw roundtrip preserves index and generation.
  CHECK(BufferHandle::from_raw(h1.raw()) == h1);

  int removed = 0;
  CHECK(pool.remove(h1, &removed) && removed == 101);
  CHECK(pool.live_count() == 1);
  CHECK(pool.get(h1) == nullptr);
  CHECK(!pool.remove(h1));  // double remove rejected

  // Reallocating reuses slot index with incremented generation; stale h1 stays rejected.
  BufferHandle h3 = pool.allocate(303);
  CHECK(h3.index == h1.index);
  CHECK(h3.generation != h1.generation);
  CHECK(pool.get(h1) == nullptr);
  CHECK(pool.get(h3) != nullptr && *pool.get(h3) == 303);
}

void test_pipeline_cache_envelope() {
  using namespace shift::rhi;
  PipelineCacheKey key{};
  key.backend = BackendKind::Vulkan;
  key.vendor_id = 0x10DEu;
  key.device_id = 0x2704u;
  for (uint8_t i = 0; i < 16; ++i) key.uuid[i] = static_cast<uint8_t>(i + 1u);

  const std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  auto enc = encode_pipeline_cache_envelope(key, payload.data(), payload.size());
  std::vector<uint8_t> dec;
  CHECK(decode_pipeline_cache_envelope(enc.data(), enc.size(), key, dec));
  CHECK(dec == payload);

  // Corrupted byte fails checksum.
  auto bad = enc;
  bad.back() ^= 0x55u;
  CHECK(!decode_pipeline_cache_envelope(bad.data(), bad.size(), key, dec));

  // Mismatched vendor / UUID / backend fails cleanly.
  PipelineCacheKey wrong_vendor = key;
  wrong_vendor.vendor_id = 0x1002u;
  CHECK(!decode_pipeline_cache_envelope(enc.data(), enc.size(), wrong_vendor, dec));

  PipelineCacheKey wrong_uuid = key;
  wrong_uuid.uuid[7] ^= 0xFFu;
  CHECK(!decode_pipeline_cache_envelope(enc.data(), enc.size(), wrong_uuid, dec));

  PipelineCacheKey wrong_backend = key;
  wrong_backend.backend = BackendKind::D3D12;
  CHECK(!decode_pipeline_cache_envelope(enc.data(), enc.size(), wrong_backend, dec));
}

void test_live_vulkan_gpu() {
  using namespace shift::rhi;
  const auto adapters = enumerate_adapters(BackendKind::Vulkan);
  if (adapters.empty()) {
    std::printf("[rhi_tests] no Vulkan adapters present on host; skipping live GPU checks\n");
    return;
  }

  const std::filesystem::path cache_path =
      std::filesystem::temp_directory_path() / "shift_rhi_test_pipeline_cache.bin";
  std::error_code ec;
  std::filesystem::remove(cache_path, ec);

  DeviceDesc ddesc{};
  ddesc.backend = BackendKind::Vulkan;
  ddesc.enable_validation = true;
  ddesc.enable_ray_tracing = true;
  ddesc.enable_bindless = true;
  ddesc.pipeline_cache_path = cache_path;
  ddesc.app_name = "rhi_tests";

  std::string err;
  std::unique_ptr<Device> dev = create_device(ddesc, err);
  if (!dev) {
    std::printf("[rhi_tests] Vulkan 1.3 device init skipped: %s\n", err.c_str());
    return;
  }

  const BackendCaps& caps = dev->caps();
  std::printf("[rhi_tests] Vulkan 1.3 device: %s (VRAM=%llu MiB, bindless=%d, indirect_count=%d, timestamps=%d, ray_query=%d)\n",
              caps.device_name.c_str(),
              static_cast<unsigned long long>(caps.device_local_bytes >> 20),
              caps.bindless ? 1 : 0,
              caps.indirect_count ? 1 : 0,
              caps.timestamps ? 1 : 0,
              caps.ray_query ? 1 : 0);

  CHECK(dev->backend() == BackendKind::Vulkan);
  CHECK(caps.dynamic_rendering);
  CHECK(caps.synchronization2);
  CHECK(!caps.device_name.empty());
  CHECK(caps.supported_msaa_mask >= 1u);

  // -------------------------------------------------------------------------
  // 1. Buffer upload, copy, fill_buffer, update_buffer, and readback
  // -------------------------------------------------------------------------
  {
    constexpr uint32_t kCount = 256;
    constexpr uint64_t kBytes = kCount * sizeof(uint32_t);
    BufferHandle stage = dev->create_buffer(
        {kBytes, BufferUsage::TransferSrc, MemoryHint::CpuToGpu, true, "stage"}, err);
    BufferHandle gpu = dev->create_buffer(
        {kBytes, BufferUsage::TransferSrc | BufferUsage::TransferDst | BufferUsage::Storage,
         MemoryHint::GpuOnly, false, "gpu"}, err);
    BufferHandle readback = dev->create_buffer(
        {kBytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "readback"}, err);
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
  }

  // -------------------------------------------------------------------------
  // 2. 2D Texture upload, view swizzle, bindless table (4096), blit & readback
  // -------------------------------------------------------------------------
  {
    constexpr uint32_t kW = 32, kH = 32;
    constexpr uint64_t kBytes = kW * kH * 4ull;
    BufferHandle stage = dev->create_buffer(
        {kBytes, BufferUsage::TransferSrc, MemoryHint::CpuToGpu, false, "tex_stage"}, err);
    BufferHandle readback = dev->create_buffer(
        {kBytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "tex_rb"}, err);
    TextureHandle tex = dev->create_texture(
        {TextureType::Tex2D, Format::Rgba8Unorm, kW, kH, 1, 1, 1, 1,
         TextureUsage::TransferSrc | TextureUsage::TransferDst | TextureUsage::Sampled,
         MemoryHint::GpuOnly, "tex2d"}, err);
    TextureHandle small = dev->create_texture(
        {TextureType::Tex2D, Format::Rgba8Unorm, kW / 2, kH / 2, 1, 1, 1, 1,
         TextureUsage::TransferSrc | TextureUsage::TransferDst | TextureUsage::Sampled,
         MemoryHint::GpuOnly, "tex_blit"}, err);
    CHECK(stage && readback && tex && small);

    TextureViewHandle view = dev->create_texture_view({tex, TextureType::Tex2D, Format::Undefined, 0, 1, 0, 1}, err);
    TextureViewHandle swiz_view = dev->create_texture_view(
        {tex, TextureType::Tex2D, Format::Undefined, 0, 1, 0, 1,
         {ComponentSwizzle::B, ComponentSwizzle::G, ComponentSwizzle::R, ComponentSwizzle::One}}, err);
    SamplerHandle sampler = dev->create_sampler(SamplerDesc{}, err);
    CHECK(view && swiz_view && sampler);

    auto* px = static_cast<uint8_t*>(dev->map_buffer(stage));
    CHECK(px != nullptr);
    for (uint32_t y = 0; y < kH; ++y) {
      for (uint32_t x = 0; x < kW; ++x) {
        const size_t o = (size_t(y) * kW + x) * 4;
        px[o + 0] = static_cast<uint8_t>(x * 8);
        px[o + 1] = static_cast<uint8_t>(y * 8);
        px[o + 2] = 0x5Au;
        px[o + 3] = 0xFFu;
      }
    }
    dev->unmap_buffer(stage);

    bool ok = dev->submit_once([&](CommandList& cmd) {
      TextureBarrier to_dst[2] = {
          {tex, ResourceState::Undefined, ResourceState::CopyDst},
          {small, ResourceState::Undefined, ResourceState::CopyDst},
      };
      cmd.barrier({to_dst, {}});
      BufferTextureCopy region{};
      region.width = kW;
      region.height = kH;
      cmd.copy_buffer_to_texture(stage, tex, std::span(&region, 1));

      TextureBarrier tex_to_src{tex, ResourceState::CopyDst, ResourceState::CopySrc};
      cmd.barrier({std::span(&tex_to_src, 1), {}});
      TextureBlitRegion blit_reg{};
      blit_reg.src_x1 = static_cast<int32_t>(kW);
      blit_reg.src_y1 = static_cast<int32_t>(kH);
      blit_reg.dst_x1 = static_cast<int32_t>(kW / 2);
      blit_reg.dst_y1 = static_cast<int32_t>(kH / 2);
      cmd.blit(tex, small, blit_reg, Filter::Linear);

      cmd.copy_texture_to_buffer(tex, readback, std::span(&region, 1));
      TextureBarrier to_srv[2] = {
          {tex, ResourceState::CopySrc, ResourceState::ShaderRead},
          {small, ResourceState::CopyDst, ResourceState::ShaderRead},
      };
      cmd.barrier({to_srv, {}, false, false, true});
    }, err);
    CHECK(ok);

    auto* rb = static_cast<uint8_t*>(dev->map_buffer(readback));
    CHECK(rb != nullptr);
    if (rb) {
      CHECK(rb[0] == 0 && rb[1] == 0 && rb[2] == 0x5Au && rb[3] == 0xFFu);
      const size_t corner = (size_t(kH - 1) * kW + (kW - 1)) * 4;
      CHECK(rb[corner + 0] == static_cast<uint8_t>((kW - 1) * 8));
      CHECK(rb[corner + 1] == static_cast<uint8_t>((kH - 1) * 8));
      CHECK(rb[corner + 2] == 0x5Au);
      CHECK(rb[corner + 3] == 0xFFu);
      dev->unmap_buffer(readback);
    }

    if (caps.bindless) {
      BufferHandle dummy_ssbo = dev->create_buffer(
          {64, BufferUsage::Storage, MemoryHint::GpuOnly, false, "bindless_ssbo"}, err);
      CHECK(dummy_ssbo.valid());
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
      dev->destroy_buffer(dummy_ssbo);
    }

    dev->destroy_sampler(sampler);
    dev->destroy_texture_view(swiz_view);
    dev->destroy_texture_view(view);
    dev->destroy_texture(small);
    dev->destroy_texture(tex);
    dev->destroy_buffer(readback);
    dev->destroy_buffer(stage);
  }

  // -------------------------------------------------------------------------
  // 3. Compute shader dispatch readback + GPU timestamp query pool
  // -------------------------------------------------------------------------
  {
    constexpr uint32_t kN = 64;
    constexpr uint64_t kBytes = kN * sizeof(uint32_t);
    BufferHandle in_buf = dev->create_buffer(
        {kBytes, BufferUsage::Storage, MemoryHint::CpuToGpu, false, "comp_in"}, err);
    BufferHandle out_buf = dev->create_buffer(
        {kBytes, BufferUsage::Storage | BufferUsage::TransferSrc, MemoryHint::GpuOnly, false, "comp_out"}, err);
    BufferHandle rb_buf = dev->create_buffer(
        {kBytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "comp_rb"}, err);
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
    cp_desc.compute_shader = {ShaderStage::Compute, kComputeDoubleSpv, "main"};
    PipelineHandle pipe = dev->create_compute_pipeline(cp_desc, err);
    CHECK(bgl && bg && pl && pipe);

    QueryPoolHandle ts_pool;
    if (caps.timestamps) {
      ts_pool = dev->create_query_pool({QueryType::Timestamp, 4}, err);
      CHECK(ts_pool.valid());
    }

    const uint32_t addend = 13u;
    bool ok = dev->submit_once([&](CommandList& cmd) {
      if (ts_pool) {
        cmd.reset_query_pool(ts_pool, 0, 4);
        cmd.write_timestamp(ts_pool, 0, PipelineStage::TopOfPipe);
      }
      cmd.bind_pipeline(pipe);
      cmd.bind_group(0, bg);
      cmd.push_constants(ShaderStage::Compute, 0, sizeof(addend), &addend);
      cmd.dispatch(kN / 16u, 1, 1);

      BufferBarrier to_copy{out_buf, ResourceState::StorageReadWrite, ResourceState::CopySrc};
      cmd.barrier({{}, std::span(&to_copy, 1)});
      cmd.copy_buffer(out_buf, 0, rb_buf, 0, kBytes);
      cmd.barrier({{}, {}, false, false, true});
      if (ts_pool) {
        cmd.write_timestamp(ts_pool, 1, PipelineStage::BottomOfPipe);
      }
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

    if (ts_pool) {
      uint64_t ticks[2] = {};
      CHECK(dev->get_query_pool_results(ts_pool, 0, 2, ticks, true));
      CHECK(ticks[1] >= ticks[0]);
      const double elapsed_us = static_cast<double>(ticks[1] - ticks[0]) * caps.timestamp_period_ns / 1000.0;
      std::printf("[rhi_tests] compute timestamp delta: %.2f us\n", elapsed_us);
      dev->destroy_query_pool(ts_pool);
    }

    dev->destroy_pipeline(pipe);
    dev->destroy_pipeline_layout(pl);
    dev->destroy_bind_group(bg);
    dev->destroy_bind_group_layout(bgl);
    dev->destroy_buffer(rb_buf);
    dev->destroy_buffer(out_buf);
    dev->destroy_buffer(in_buf);
  }

  // -------------------------------------------------------------------------
  // 4. Offscreen Dynamic Rendering pass + clear + draw + color/depth readback
  // -------------------------------------------------------------------------
  {
    constexpr uint32_t kW = 64, kH = 64;
    TextureHandle color_tex = dev->create_texture(
        {TextureType::Tex2D, Format::Rgba8Unorm, kW, kH, 1, 1, 1, 1,
         TextureUsage::ColorAttachment | TextureUsage::TransferSrc, MemoryHint::GpuOnly, "rt_color"}, err);
    TextureHandle depth_tex = dev->create_texture(
        {TextureType::Tex2D, Format::D32Sfloat, kW, kH, 1, 1, 1, 1,
         TextureUsage::DepthAttachment | TextureUsage::TransferSrc, MemoryHint::GpuOnly, "rt_depth"}, err);
    TextureViewHandle color_view = dev->create_texture_view({color_tex, TextureType::Tex2D}, err);
    TextureViewHandle depth_view = dev->create_texture_view({depth_tex, TextureType::Tex2D}, err);
    BufferHandle color_rb = dev->create_buffer(
        {kW * kH * 4ull, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "color_rb"}, err);
    BufferHandle depth_rb = dev->create_buffer(
        {kW * kH * 4ull, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "depth_rb"}, err);
    CHECK(color_tex && depth_tex && color_view && depth_view && color_rb && depth_rb);

    PushConstantRange pc_range{ShaderStage::Fragment, 0, sizeof(float) * 4};
    PipelineLayoutHandle pl = dev->create_pipeline_layout({{}, std::span(&pc_range, 1)}, err);
    Format color_fmt = Format::Rgba8Unorm;
    GraphicsPipelineDesc gp{};
    gp.layout = pl;
    gp.vertex_shader = {ShaderStage::Vertex, kFullscreenVertSpv, "main"};
    gp.fragment_shader = {ShaderStage::Fragment, kSolidColorFragSpv, "main"};
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
      ca.clear_color[0] = 0.0f;
      ca.clear_color[1] = 0.0f;
      ca.clear_color[2] = 0.0f;
      ca.clear_color[3] = 1.0f;

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

      cmd.barrier({{}, {}, false, false, true});
    }, err);
    CHECK(ok);

    auto* cpx = static_cast<uint8_t*>(dev->map_buffer(color_rb));
    auto* dpx = static_cast<float*>(dev->map_buffer(depth_rb));
    CHECK(cpx != nullptr && dpx != nullptr);
    if (cpx && dpx) {
      // Center pixel check: (0.25, 0.5, 0.75, 1.0) -> (~64, ~128, ~191, 255) and depth == 0.5f.
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
  }

  // -------------------------------------------------------------------------
  // 5. Hardware Ray Tracing: BLAS + Compaction Query + TLAS build
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
         MemoryHint::CpuToGpu, false, "rt_vb"}, err);
    BufferHandle ib = dev->create_buffer(
        {sizeof(inds), BufferUsage::Index | BufferUsage::AccelStructInput | BufferUsage::DeviceAddress,
         MemoryHint::CpuToGpu, false, "rt_ib"}, err);
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

    AccelStructHandle blas = dev->create_blas({AccelStructKind::BottomLevel, blas_sizes.accel_struct_size, "blas"}, err);
    CHECK(blas.valid());
    CHECK(dev->accel_struct_device_address(blas) != 0);

    const uint64_t align = std::max<uint64_t>(caps.min_as_scratch_alignment, 256ull);
    AccelStructBuildSizes tlas_sizes{};
    CHECK(dev->query_tlas_sizes(1, AccelStructBuildFlags::PreferFastTrace, tlas_sizes));
    const uint64_t scratch_bytes = std::max(blas_sizes.build_scratch_size, tlas_sizes.build_scratch_size) + align;
    BufferHandle scratch = dev->create_buffer(
        {scratch_bytes, BufferUsage::Storage | BufferUsage::DeviceAddress, MemoryHint::GpuOnly, false, "as_scratch"}, err);
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
        {AccelStructKind::BottomLevel, compacted_size, "blas_compact"}, err);
    CHECK(compacted_blas.valid());
    const uint64_t compact_addr = dev->accel_struct_device_address(compacted_blas);
    CHECK(compact_addr != 0);

    TlasInstanceDesc inst{};
    inst.custom_index_24 = 42u;
    inst.mask_8 = 0xFFu;
    inst.blas_device_address = compact_addr;
    BufferHandle inst_buf = dev->create_buffer(
        {sizeof(TlasInstanceDesc), BufferUsage::AccelStructInput | BufferUsage::DeviceAddress,
         MemoryHint::CpuToGpu, false, "tlas_inst"}, err);
    CHECK(inst_buf.valid());
    std::memcpy(dev->map_buffer(inst_buf), &inst, sizeof(inst));
    dev->unmap_buffer(inst_buf);

    AccelStructHandle tlas = dev->create_tlas({AccelStructKind::TopLevel, tlas_sizes.accel_struct_size, "tlas"}, err);
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

    std::printf("[rhi_tests] RT BLAS (%llu B -> compacted %llu B) + TLAS build passed\n",
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

  // Zero-leak & zero-validation-error assertions!
  dev->wait_idle();
  const VramStats stats = dev->vram_stats();
  CHECK(stats.allocation_count == 0);
  CHECK(dev->validation_message_count() == 0);

  // Pipeline cache save + reload round-trip.
  CHECK(dev->save_pipeline_cache(err));
  const bool cache_written = std::filesystem::is_regular_file(cache_path, ec) &&
                             std::filesystem::file_size(cache_path, ec) > 0;
  dev.reset();

  if (cache_written) {
    std::unique_ptr<Device> dev2 = create_device(ddesc, err);
    CHECK(dev2 != nullptr);
    if (dev2) {
      CHECK(dev2->pipeline_cache_loaded());
      CHECK(dev2->validation_message_count() == 0);
    }
  }
  std::filesystem::remove(cache_path, ec);
}

std::vector<uint32_t> msl_to_words(const char* src) {
  const size_t len = std::strlen(src);
  const size_t word_count = (len + 3u) / 4u;
  std::vector<uint32_t> words(word_count, 0u);
  if (len > 0) std::memcpy(words.data(), src, len);
  return words;
}

// MSL 3.1 compute shader matching kComputeDoubleSpv:
//   dst[i] = src[i] * 2u + pc.addend (local_size_x = 16)
constexpr const char* kComputeDoubleMsl = R"(
#include <metal_stdlib>
using namespace metal;
// shift_local_size(16, 1, 1)
struct Push { uint addend; };
kernel void main0(device const uint* src [[buffer(0)]],
                  device uint* dst [[buffer(1)]],
                  constant Push& pc [[buffer(30)]],
                  uint i [[thread_position_in_grid]]) {
  dst[i] = src[i] * 2u + pc.addend;
}
)";

// MSL 3.1 compute shader sampling a Metal 3 Tier 2 Argument Buffer (1x sampler + 4096 textures).
constexpr const char* kBindlessSampleMsl = R"(
#include <metal_stdlib>
using namespace metal;
// shift_local_size(1, 1, 1)
struct BindlessArgs {
  sampler default_sampler;
  array<texture2d<float>, 4096> textures;
};
kernel void main0(device float4* out_rgba [[buffer(0)]],
                  constant BindlessArgs& bl [[buffer(1)]],
                  uint gid [[thread_position_in_grid]]) {
  if (gid == 0) {
    out_rgba[0] = bl.textures[0].sample(bl.default_sampler, float2(0.01f, 0.01f));
    out_rgba[1] = bl.textures[7].sample(bl.default_sampler, float2(0.01f, 0.01f));
  }
}
)";

// MSL 3.1 fullscreen triangle vertex shader at z = 0.5 matching kFullscreenVertSpv:
constexpr const char* kFullscreenVertMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct VsOut { float4 pos [[position]]; };
vertex VsOut main0(uint vid [[vertex_id]]) {
  float2 pos = float2((vid == 1u) ? 3.0f : -1.0f, (vid == 2u) ? 3.0f : -1.0f);
  VsOut out;
  out.pos = float4(pos, 0.5f, 1.0f);
  return out;
}
)";

// MSL 3.1 fragment shader writing push-constant float4 color matching kSolidColorFragSpv:
constexpr const char* kSolidColorFragMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct Push { float4 color; };
fragment float4 main0(constant Push& pc [[buffer(30)]]) {
  return pc.color;
}
)";

void test_live_metal_gpu(bool require_device) {
  using namespace shift::rhi;
  if (!is_backend_compiled(BackendKind::Metal)) {
    if (require_device) {
      std::printf("[rhi_tests] Metal backend requested but not compiled on this platform\n");
      ++g_failures;
    }
    return;
  }

  const auto adapters = enumerate_adapters(BackendKind::Metal);
  if (adapters.empty()) {
    std::printf("[rhi_tests] no Metal adapters present on host\n");
    if (require_device) ++g_failures;
    return;
  }

  const std::filesystem::path cache_path =
      std::filesystem::temp_directory_path() / "shift_rhi_test_metal_pipeline_cache.bin";
  std::error_code ec;
  std::filesystem::remove(cache_path, ec);

  DeviceDesc ddesc{};
  ddesc.backend = BackendKind::Metal;
  ddesc.enable_validation = true;
  ddesc.enable_ray_tracing = true;
  ddesc.enable_bindless = true;
  ddesc.pipeline_cache_path = cache_path;
  ddesc.app_name = "rhi_tests_metal";

  std::string err;
  std::unique_ptr<Device> dev = create_device(ddesc, err);
  if (!dev) {
    std::printf("[rhi_tests] Metal 3 device init failed: %s\n", err.c_str());
    ++g_failures;
    return;
  }

  const BackendCaps& caps = dev->caps();
  std::printf("[rhi_tests] Metal 3 device: %s (%s, VRAM=%llu MiB, bindless=%d, timestamps=%d, ray_query=%d, lazy_tbdr=%d, edr=%.2f)\n",
              caps.device_name.c_str(),
              caps.driver_info.c_str(),
              static_cast<unsigned long long>(caps.device_local_bytes >> 20),
              caps.bindless ? 1 : 0,
              caps.timestamps ? 1 : 0,
              caps.ray_query ? 1 : 0,
              caps.lazy_memory ? 1 : 0,
              caps.edr_max_headroom);

  CHECK(dev->backend() == BackendKind::Metal);
  CHECK(caps.dynamic_rendering);
  CHECK(caps.synchronization2);
  CHECK(!caps.device_name.empty());
  CHECK(caps.supported_msaa_mask >= 1u);
  CHECK(caps.bindless);
  CHECK(caps.edr);

  // -------------------------------------------------------------------------
  // 1. Buffer upload, copy, fill_buffer (32-bit pattern), update_buffer & readback
  // -------------------------------------------------------------------------
  {
    constexpr uint32_t kCount = 256;
    constexpr uint64_t kBytes = kCount * sizeof(uint32_t);
    BufferHandle stage = dev->create_buffer(
        {kBytes, BufferUsage::TransferSrc, MemoryHint::CpuToGpu, true, "mtl_stage"}, err);
    BufferHandle gpu = dev->create_buffer(
        {kBytes, BufferUsage::TransferSrc | BufferUsage::TransferDst | BufferUsage::Storage,
         MemoryHint::GpuOnly, false, "mtl_gpu"}, err);
    BufferHandle readback = dev->create_buffer(
        {kBytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "mtl_rb"}, err);
    CHECK(stage && gpu && readback);
    CHECK(dev->buffer_device_address(gpu) != 0);

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
  }

  // -------------------------------------------------------------------------
  // 2. 2D Texture upload, view swizzle, blit, and LIVE 4096-slot Tier 2 Bindless sampling
  // -------------------------------------------------------------------------
  {
    constexpr uint32_t kW = 32, kH = 32;
    constexpr uint64_t kBytes = kW * kH * 4ull;
    BufferHandle stage = dev->create_buffer(
        {kBytes, BufferUsage::TransferSrc, MemoryHint::CpuToGpu, false, "mtl_tex_stage"}, err);
    BufferHandle readback = dev->create_buffer(
        {kBytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "mtl_tex_rb"}, err);
    TextureHandle tex = dev->create_texture(
        {TextureType::Tex2D, Format::Rgba8Unorm, kW, kH, 1, 1, 1, 1,
         TextureUsage::TransferSrc | TextureUsage::TransferDst | TextureUsage::Sampled,
         MemoryHint::GpuOnly, "mtl_tex2d"}, err);
    TextureHandle small = dev->create_texture(
        {TextureType::Tex2D, Format::Rgba8Unorm, kW / 2, kH / 2, 1, 1, 1, 1,
         TextureUsage::TransferSrc | TextureUsage::TransferDst | TextureUsage::Sampled,
         MemoryHint::GpuOnly, "mtl_tex_blit"}, err);
    CHECK(stage && readback && tex && small);

    TextureViewHandle view = dev->create_texture_view({tex, TextureType::Tex2D, Format::Undefined, 0, 1, 0, 1}, err);
    TextureViewHandle swiz_view = dev->create_texture_view(
        {tex, TextureType::Tex2D, Format::Undefined, 0, 1, 0, 1,
         {ComponentSwizzle::B, ComponentSwizzle::G, ComponentSwizzle::R, ComponentSwizzle::One}}, err);
    SamplerDesc sdesc{};
    sdesc.min_filter = Filter::Nearest;
    sdesc.mag_filter = Filter::Nearest;
    SamplerHandle sampler = dev->create_sampler(sdesc, err);
    CHECK(view && swiz_view && sampler);

    auto* px = static_cast<uint8_t*>(dev->map_buffer(stage));
    CHECK(px != nullptr);
    for (uint32_t y = 0; y < kH; ++y) {
      for (uint32_t x = 0; x < kW; ++x) {
        const size_t o = (size_t(y) * kW + x) * 4;
        px[o + 0] = (x == 0 && y == 0) ? 0x20u : static_cast<uint8_t>(x * 8);
        px[o + 1] = (x == 0 && y == 0) ? 0x40u : static_cast<uint8_t>(y * 8);
        px[o + 2] = 0x80u;
        px[o + 3] = 0xFFu;
      }
    }
    dev->unmap_buffer(stage);

    bool ok = dev->submit_once([&](CommandList& cmd) {
      BufferTextureCopy region{};
      region.width = kW;
      region.height = kH;
      cmd.copy_buffer_to_texture(stage, tex, std::span(&region, 1));

      TextureBlitRegion blit_reg{};
      blit_reg.src_x1 = static_cast<int32_t>(kW);
      blit_reg.src_y1 = static_cast<int32_t>(kH);
      blit_reg.dst_x1 = static_cast<int32_t>(kW / 2);
      blit_reg.dst_y1 = static_cast<int32_t>(kH / 2);
      cmd.blit(tex, small, blit_reg, Filter::Linear);

      cmd.copy_texture_to_buffer(tex, readback, std::span(&region, 1));
    }, err);
    CHECK(ok);

    auto* rb = static_cast<uint8_t*>(dev->map_buffer(readback));
    CHECK(rb != nullptr);
    if (rb) {
      CHECK(rb[0] == 0x20u && rb[1] == 0x40u && rb[2] == 0x80u && rb[3] == 0xFFu);
      const size_t corner = (size_t(kH - 1) * kW + (kW - 1)) * 4;
      CHECK(rb[corner + 0] == static_cast<uint8_t>((kW - 1) * 8));
      CHECK(rb[corner + 1] == static_cast<uint8_t>((kH - 1) * 8));
      CHECK(rb[corner + 2] == 0x80u);
      CHECK(rb[corner + 3] == 0xFFu);
      dev->unmap_buffer(readback);
    }

    // Live Metal 3 Tier 2 Argument Buffer 4096-texture bindless sampling check!
    {
      BufferHandle dummy_ssbo = dev->create_buffer(
          {64, BufferUsage::Storage, MemoryHint::GpuOnly, false, "mtl_bindless_ssbo"}, err);
      BufferHandle sample_out = dev->create_buffer(
          {sizeof(float) * 8, BufferUsage::Storage, MemoryHint::GpuToCpu, false, "mtl_bl_out"}, err);
      CHECK(dummy_ssbo && sample_out);

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

        BindGroupLayoutBinding b0{0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute};
        BindGroupLayoutHandle bgl0 = dev->create_bind_group_layout({std::span(&b0, 1), false}, err);
        BindGroupEntry e0{0, 0, DescriptorType::StorageBuffer, sample_out};
        BindGroupHandle bg0 = dev->create_bind_group({bgl0, std::span(&e0, 1)}, err);
        BindGroupLayoutHandle layouts[2] = {bgl0, table->layout()};
        PipelineLayoutHandle pl = dev->create_pipeline_layout({layouts, {}}, err);
        const auto msl_words = msl_to_words(kBindlessSampleMsl);
        ComputePipelineDesc cpd{pl, {ShaderStage::Compute, msl_words, "main0"}};
        PipelineHandle pipe = dev->create_compute_pipeline(cpd, err);
        CHECK(bgl0 && bg0 && pl && pipe);

        bool sample_ok = dev->submit_once([&](CommandList& cmd) {
          cmd.bind_pipeline(pipe);
          cmd.bind_group(0, bg0);
          cmd.bind_bindless(1, *table);
          cmd.dispatch(1, 1, 1);
        }, err);
        CHECK(sample_ok);

        const auto* res = static_cast<const float*>(dev->map_buffer(sample_out));
        CHECK(res != nullptr);
        if (res) {
          // Slot 0 (identity): (0x20/255, 0x40/255, 0x80/255, 1.0)
          CHECK(std::fabs(res[0] - (0x20 / 255.0f)) < 0.02f);
          CHECK(std::fabs(res[1] - (0x40 / 255.0f)) < 0.02f);
          CHECK(std::fabs(res[2] - (0x80 / 255.0f)) < 0.02f);
          // Slot 7 (swizzled B,G,R,1): R and B swapped!
          CHECK(std::fabs(res[4] - (0x80 / 255.0f)) < 0.02f);
          CHECK(std::fabs(res[5] - (0x40 / 255.0f)) < 0.02f);
          CHECK(std::fabs(res[6] - (0x20 / 255.0f)) < 0.02f);
          CHECK(std::fabs(res[7] - 1.0f) < 0.02f);
          dev->unmap_buffer(sample_out);
        }

        dev->destroy_pipeline(pipe);
        dev->destroy_pipeline_layout(pl);
        dev->destroy_bind_group(bg0);
        dev->destroy_bind_group_layout(bgl0);
      }
      dev->destroy_buffer(sample_out);
      dev->destroy_buffer(dummy_ssbo);
    }

    dev->destroy_sampler(sampler);
    dev->destroy_texture_view(swiz_view);
    dev->destroy_texture_view(view);
    dev->destroy_texture(small);
    dev->destroy_texture(tex);
    dev->destroy_buffer(readback);
    dev->destroy_buffer(stage);
  }

  // -------------------------------------------------------------------------
  // 3. Compute shader dispatch + readback + GPU timestamp query pool
  // -------------------------------------------------------------------------
  {
    constexpr uint32_t kN = 64;
    constexpr uint64_t kBytes = kN * sizeof(uint32_t);
    BufferHandle in_buf = dev->create_buffer(
        {kBytes, BufferUsage::Storage, MemoryHint::CpuToGpu, false, "mtl_comp_in"}, err);
    BufferHandle out_buf = dev->create_buffer(
        {kBytes, BufferUsage::Storage | BufferUsage::TransferSrc, MemoryHint::GpuOnly, false, "mtl_comp_out"}, err);
    BufferHandle rb_buf = dev->create_buffer(
        {kBytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "mtl_comp_rb"}, err);
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
    const auto comp_words = msl_to_words(kComputeDoubleMsl);
    ComputePipelineDesc cp_desc{};
    cp_desc.layout = pl;
    cp_desc.compute_shader = {ShaderStage::Compute, comp_words, "main0"};
    PipelineHandle pipe = dev->create_compute_pipeline(cp_desc, err);
    CHECK(bgl && bg && pl && pipe);

    QueryPoolHandle ts_pool;
    if (caps.timestamps) {
      ts_pool = dev->create_query_pool({QueryType::Timestamp, 4}, err);
      CHECK(ts_pool.valid());
    }

    const uint32_t addend = 13u;
    bool ok = dev->submit_once([&](CommandList& cmd) {
      if (ts_pool) {
        cmd.reset_query_pool(ts_pool, 0, 4);
        cmd.write_timestamp(ts_pool, 0, PipelineStage::TopOfPipe);
      }
      cmd.bind_pipeline(pipe);
      cmd.bind_group(0, bg);
      cmd.push_constants(ShaderStage::Compute, 0, sizeof(addend), &addend);
      cmd.dispatch(kN / 16u, 1, 1);

      BufferBarrier to_copy{out_buf, ResourceState::StorageReadWrite, ResourceState::CopySrc};
      cmd.barrier({{}, std::span(&to_copy, 1)});
      cmd.copy_buffer(out_buf, 0, rb_buf, 0, kBytes);
      cmd.barrier({{}, {}, false, false, true});
      if (ts_pool) {
        cmd.write_timestamp(ts_pool, 1, PipelineStage::BottomOfPipe);
      }
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

    if (ts_pool) {
      uint64_t ticks[2] = {};
      CHECK(dev->get_query_pool_results(ts_pool, 0, 2, ticks, true));
      CHECK(ticks[0] != UINT64_MAX && ticks[1] != UINT64_MAX);
      CHECK(ticks[1] >= ticks[0]);
      const double elapsed_us = static_cast<double>(ticks[1] - ticks[0]) * caps.timestamp_period_ns / 1000.0;
      std::printf("[rhi_tests] Metal compute timestamp delta: %.2f us\n", elapsed_us);
      dev->destroy_query_pool(ts_pool);
    }

    dev->destroy_pipeline(pipe);
    dev->destroy_pipeline_layout(pl);
    dev->destroy_bind_group(bg);
    dev->destroy_bind_group_layout(bgl);
    dev->destroy_buffer(rb_buf);
    dev->destroy_buffer(out_buf);
    dev->destroy_buffer(in_buf);
  }

  // -------------------------------------------------------------------------
  // 4. Offscreen render pass (RGBA8 + D32 readback) + Memoryless TBDR 4x MSAA resolve
  // -------------------------------------------------------------------------
  {
    constexpr uint32_t kW = 64, kH = 64;
    TextureHandle color_tex = dev->create_texture(
        {TextureType::Tex2D, Format::Rgba8Unorm, kW, kH, 1, 1, 1, 1,
         TextureUsage::ColorAttachment | TextureUsage::TransferSrc, MemoryHint::GpuOnly, "mtl_rt_color"}, err);
    TextureHandle depth_tex = dev->create_texture(
        {TextureType::Tex2D, Format::D32Sfloat, kW, kH, 1, 1, 1, 1,
         TextureUsage::DepthAttachment | TextureUsage::TransferSrc, MemoryHint::GpuOnly, "mtl_rt_depth"}, err);
    TextureViewHandle color_view = dev->create_texture_view({color_tex, TextureType::Tex2D}, err);
    TextureViewHandle depth_view = dev->create_texture_view({depth_tex, TextureType::Tex2D}, err);
    BufferHandle color_rb = dev->create_buffer(
        {kW * kH * 4ull, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "mtl_color_rb"}, err);
    BufferHandle depth_rb = dev->create_buffer(
        {kW * kH * 4ull, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "mtl_depth_rb"}, err);
    CHECK(color_tex && depth_tex && color_view && depth_view && color_rb && depth_rb);

    PushConstantRange pc_range{ShaderStage::Fragment, 0, sizeof(float) * 4};
    PipelineLayoutHandle pl = dev->create_pipeline_layout({{}, std::span(&pc_range, 1)}, err);
    const auto vs_words = msl_to_words(kFullscreenVertMsl);
    const auto fs_words = msl_to_words(kSolidColorFragMsl);
    Format color_fmt = Format::Rgba8Unorm;
    GraphicsPipelineDesc gp{};
    gp.layout = pl;
    gp.vertex_shader = {ShaderStage::Vertex, vs_words, "main0"};
    gp.fragment_shader = {ShaderStage::Fragment, fs_words, "main0"};
    gp.color_formats = std::span(&color_fmt, 1);
    gp.depth_format = Format::D32Sfloat;
    gp.depth = {true, true, CompareOp::Always, false};
    gp.raster.cull_mode = CullMode::None;
    PipelineHandle pipe = dev->create_graphics_pipeline(gp, err);
    CHECK(pl && pipe);

    const float draw_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    bool ok = dev->submit_once([&](CommandList& cmd) {
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

    // Memoryless TBDR 4x MSAA resolve check (when 4x MSAA supported).
    if ((caps.supported_msaa_mask & 4u) != 0u) {
      TextureHandle msaa_color = dev->create_texture(
          {TextureType::Tex2D, Format::Rgba8Unorm, kW, kH, 1, 1, 1, 4,
           TextureUsage::ColorAttachment | TextureUsage::Transient, MemoryHint::Transient, "mtl_msaa_c"}, err);
      TextureHandle msaa_depth = dev->create_texture(
          {TextureType::Tex2D, Format::D32Sfloat, kW, kH, 1, 1, 1, 4,
           TextureUsage::DepthAttachment | TextureUsage::Transient, MemoryHint::Transient, "mtl_msaa_d"}, err);
      TextureViewHandle msaa_cv = dev->create_texture_view({msaa_color, TextureType::Tex2D}, err);
      TextureViewHandle msaa_dv = dev->create_texture_view({msaa_depth, TextureType::Tex2D}, err);
      GraphicsPipelineDesc gp4 = gp;
      gp4.raster.sample_count = 4;
      PipelineHandle pipe4 = dev->create_graphics_pipeline(gp4, err);
      CHECK(msaa_color && msaa_depth && msaa_cv && msaa_dv && pipe4);

      const float msaa_col[4] = {0.5f, 0.25f, 1.0f, 1.0f};
      bool msaa_ok = dev->submit_once([&](CommandList& cmd) {
        ColorAttachmentDesc ca{};
        ca.view = msaa_cv;
        ca.resolve_view = color_view;
        ca.load_op = LoadOp::Clear;
        ca.store_op = StoreOp::MultisampleResolve;
        ca.resolve_mode = ResolveMode::Average;

        DepthAttachmentDesc da{};
        da.view = msaa_dv;
        da.resolve_view = depth_view;
        da.load_op = LoadOp::Clear;
        da.store_op = StoreOp::MultisampleResolve;
        da.resolve_mode = ResolveMode::SampleZero;
        da.clear_depth = 0.0f;

        RenderPassDesc rp{};
        rp.width = kW;
        rp.height = kH;
        rp.color_attachments = std::span(&ca, 1);
        rp.depth_attachment = &da;
        cmd.begin_render_pass(rp);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH));
        cmd.set_scissor(0, 0, kW, kH);
        cmd.bind_pipeline(pipe4);
        cmd.push_constants(ShaderStage::Fragment, 0, sizeof(msaa_col), msaa_col);
        cmd.draw(3);
        cmd.end_render_pass();

        BufferTextureCopy copy{};
        copy.width = kW;
        copy.height = kH;
        cmd.copy_texture_to_buffer(color_tex, color_rb, std::span(&copy, 1));
      }, err);
      CHECK(msaa_ok);

      auto* mpx = static_cast<uint8_t*>(dev->map_buffer(color_rb));
      CHECK(mpx != nullptr);
      if (mpx) {
        const size_t mid = size_t(kH / 2) * kW + (kW / 2);
        CHECK(std::abs(int(mpx[mid * 4 + 0]) - 128) <= 1);
        CHECK(std::abs(int(mpx[mid * 4 + 1]) - 64) <= 1);
        CHECK(mpx[mid * 4 + 2] == 255);
        dev->unmap_buffer(color_rb);
      }

      dev->destroy_pipeline(pipe4);
      dev->destroy_texture_view(msaa_dv);
      dev->destroy_texture_view(msaa_cv);
      dev->destroy_texture(msaa_depth);
      dev->destroy_texture(msaa_color);
    }

    dev->destroy_pipeline(pipe);
    dev->destroy_pipeline_layout(pl);
    dev->destroy_buffer(depth_rb);
    dev->destroy_buffer(color_rb);
    dev->destroy_texture_view(depth_view);
    dev->destroy_texture_view(color_view);
    dev->destroy_texture(depth_tex);
    dev->destroy_texture(color_tex);
  }

  // -------------------------------------------------------------------------
  // 5. Native Apple Silicon Hardware Ray Tracing: BLAS + Compaction + TLAS
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
         MemoryHint::CpuToGpu, false, "mtl_rt_vb"}, err);
    BufferHandle ib = dev->create_buffer(
        {sizeof(inds), BufferUsage::Index | BufferUsage::AccelStructInput | BufferUsage::DeviceAddress,
         MemoryHint::CpuToGpu, false, "mtl_rt_ib"}, err);
    CHECK(vb && ib);

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

    AccelStructHandle blas = dev->create_blas({AccelStructKind::BottomLevel, blas_sizes.accel_struct_size, "mtl_blas"}, err);
    CHECK(blas.valid());
    CHECK(dev->accel_struct_device_address(blas) != 0);

    const uint64_t align = std::max<uint64_t>(caps.min_as_scratch_alignment, 256ull);
    AccelStructBuildSizes tlas_sizes{};
    CHECK(dev->query_tlas_sizes(1, AccelStructBuildFlags::PreferFastTrace, tlas_sizes));
    const uint64_t scratch_bytes = std::max(blas_sizes.build_scratch_size, tlas_sizes.build_scratch_size) + align;
    BufferHandle scratch = dev->create_buffer(
        {scratch_bytes, BufferUsage::Storage | BufferUsage::DeviceAddress, MemoryHint::GpuOnly, false, "mtl_as_scratch"}, err);
    CHECK(scratch.valid());

    QueryPoolHandle compact_qp = dev->create_query_pool({QueryType::AccelStructCompactedSize, 1}, err);
    CHECK(compact_qp.valid());

    bool ok = dev->submit_once([&](CommandList& cmd) {
      cmd.reset_query_pool(compact_qp, 0, 1);
      BlasBuildDesc bdesc{};
      bdesc.dst = blas;
      bdesc.scratch_buffer = scratch;
      bdesc.scratch_offset = 0;
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
        {AccelStructKind::BottomLevel, compacted_size, "mtl_blas_compact"}, err);
    CHECK(compacted_blas.valid());
    const uint64_t compact_addr = dev->accel_struct_device_address(compacted_blas);
    CHECK(compact_addr != 0);

    TlasInstanceDesc inst{};
    inst.custom_index_24 = 42u;
    inst.mask_8 = 0xFFu;
    inst.blas_device_address = compact_addr;
    BufferHandle inst_buf = dev->create_buffer(
        {sizeof(TlasInstanceDesc), BufferUsage::AccelStructInput | BufferUsage::DeviceAddress,
         MemoryHint::CpuToGpu, false, "mtl_tlas_inst"}, err);
    CHECK(inst_buf.valid());
    std::memcpy(dev->map_buffer(inst_buf), &inst, sizeof(inst));
    dev->unmap_buffer(inst_buf);

    AccelStructHandle tlas = dev->create_tlas({AccelStructKind::TopLevel, tlas_sizes.accel_struct_size, "mtl_tlas"}, err);
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
      tdesc.scratch_offset = 0;
      tdesc.flags = AccelStructBuildFlags::PreferFastTrace;
      cmd.cmd_build_tlas(tdesc);
    }, err);
    CHECK(ok);

    std::printf("[rhi_tests] Metal 3 RT BLAS (%llu B -> compacted %llu B) + TLAS build passed\n",
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

  // Zero-leak & zero-validation-error assertions!
  dev->wait_idle();
  const VramStats stats = dev->vram_stats();
  CHECK(stats.allocation_count == 0);
  CHECK(dev->validation_message_count() == 0);

  // MTLBinaryArchive pipeline cache save + reload round-trip.
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
    }
  }
  std::filesystem::remove(cache_path, ec);
}

}  // namespace

int main(int argc, char** argv) {
  shift::rhi::BackendKind filter = shift::rhi::BackendKind::Auto;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
      filter = shift::rhi::parse_backend_kind(argv[++i], shift::rhi::BackendKind::Auto);
    } else if (std::strncmp(argv[i], "--backend=", 10) == 0) {
      filter = shift::rhi::parse_backend_kind(argv[i] + 10, shift::rhi::BackendKind::Auto);
    }
  }

  test_backend_parsing_and_selection();
  test_format_helpers();
  test_handle_generation_safety();
  test_pipeline_cache_envelope();

  if (filter == shift::rhi::BackendKind::Auto || filter == shift::rhi::BackendKind::Vulkan) {
    test_live_vulkan_gpu();
  }
  if (filter == shift::rhi::BackendKind::Auto || filter == shift::rhi::BackendKind::Metal) {
    test_live_metal_gpu(filter == shift::rhi::BackendKind::Metal);
  }

  if (g_failures == 0) {
    std::printf("rhi_tests: ALL PASSED\n");
    return 0;
  }
  std::printf("rhi_tests: %d FAILED\n", g_failures);
  return 1;
}
