#include "rhi_d3d12_internal.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace shift::rhi {
namespace d3d12 {

namespace {

std::string hr_err(const char* what, HRESULT hr) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(hr));
  return std::string(what) + " failed (HRESULT " + buf + ")";
}

std::string wide_to_utf8(const wchar_t* wstr) {
  if (!wstr || !*wstr) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string out(static_cast<size_t>(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), len, nullptr, nullptr);
  return out;
}

void set_d3d12_debug_name(ID3D12Object* obj, const char* name) {
  if (!obj || !name || !*name) return;
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
  if (wlen <= 1) return;
  std::wstring wname(static_cast<size_t>(wlen - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, name, -1, wname.data(), wlen);
  obj->SetName(wname.c_str());
}

uint64_t fnv1a_hash(const void* data, size_t size, uint64_t seed = 14695981039346656037ull) noexcept {
  const auto* p = static_cast<const uint8_t*>(data);
  uint64_t h = seed;
  for (size_t i = 0; i < size; ++i) {
    h ^= static_cast<uint64_t>(p[i]);
    h *= 1099511628211ull;
  }
  return h;
}

std::wstring pso_cache_name(wchar_t prefix, uint64_t hash) {
  wchar_t buf[32];
  std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%lc_%016llx",
                prefix, static_cast<unsigned long long>(hash));
  return std::wstring(buf);
}

D3D12_SHADER_VISIBILITY to_d3d12_visibility(ShaderStage stages) noexcept {
  const bool v = has_flag(stages, ShaderStage::Vertex);
  const bool f = has_flag(stages, ShaderStage::Fragment);
  const bool c = has_flag(stages, ShaderStage::Compute);
  if (v && !f && !c) return D3D12_SHADER_VISIBILITY_VERTEX;
  if (!v && f && !c) return D3D12_SHADER_VISIBILITY_PIXEL;
  return D3D12_SHADER_VISIBILITY_ALL;
}

UINT8 to_d3d12_write_mask(ColorWriteMask m) noexcept {
  UINT8 out = 0;
  if (has_flag(m, ColorWriteMask::R)) out |= D3D12_COLOR_WRITE_ENABLE_RED;
  if (has_flag(m, ColorWriteMask::G)) out |= D3D12_COLOR_WRITE_ENABLE_GREEN;
  if (has_flag(m, ColorWriteMask::B)) out |= D3D12_COLOR_WRITE_ENABLE_BLUE;
  if (has_flag(m, ColorWriteMask::A)) out |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
  return out;
}

D3D12_SHADER_COMPONENT_MAPPING map_component(ComponentSwizzle s,
                                             D3D12_SHADER_COMPONENT_MAPPING identity) noexcept {
  switch (s) {
    case ComponentSwizzle::Identity: return identity;
    case ComponentSwizzle::Zero: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
    case ComponentSwizzle::One: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
    case ComponentSwizzle::R: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
    case ComponentSwizzle::G: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
    case ComponentSwizzle::B: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
    case ComponentSwizzle::A: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
  }
  return identity;
}

#ifdef __ID3D12InfoQueue1_INTERFACE_DEFINED__
void CALLBACK d3d12_debug_callback(D3D12_MESSAGE_CATEGORY,
                                   D3D12_MESSAGE_SEVERITY severity,
                                   D3D12_MESSAGE_ID id,
                                   LPCSTR description,
                                   void* context) {
  if (severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
      severity == D3D12_MESSAGE_SEVERITY_ERROR) {
    if (description) {
      std::fprintf(stderr, "[rhi:d3d12] ERROR (%d): %s\n", static_cast<int>(id), description);
    }
    if (context) {
      static_cast<D3D12DeviceImpl*>(context)->record_validation_error();
    }
  }
}
#endif

}  // namespace

// -----------------------------------------------------------------------------
// DescriptorAllocator
// -----------------------------------------------------------------------------
bool DescriptorAllocator::init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                               uint32_t capacity, bool shader_visible, std::string& error) {
  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  desc.Type = type;
  desc.NumDescriptors = capacity;
  desc.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                              : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  desc.NodeMask = 0;

  const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_));
  if (FAILED(hr)) {
    error = hr_err("CreateDescriptorHeap", hr);
    return false;
  }
  cpu_start_ = heap_->GetCPUDescriptorHandleForHeapStart();
  if (shader_visible) {
    gpu_start_ = heap_->GetGPUDescriptorHandleForHeapStart();
  }
  stride_ = device->GetDescriptorHandleIncrementSize(type);
  capacity_ = capacity;
  bump_ = 0;
  free_ranges_.clear();
  return true;
}

void DescriptorAllocator::shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  heap_.Reset();
  free_ranges_.clear();
  bump_ = 0;
  capacity_ = 0;
}

uint32_t DescriptorAllocator::allocate(uint32_t count) {
  if (count == 0) return UINT32_MAX;
  std::lock_guard<std::mutex> lock(mutex_);
  // Best-fit search in free ranges first.
  size_t best_idx = SIZE_MAX;
  uint32_t best_size = UINT32_MAX;
  for (size_t i = 0; i < free_ranges_.size(); ++i) {
    if (free_ranges_[i].count >= count && free_ranges_[i].count < best_size) {
      best_idx = i;
      best_size = free_ranges_[i].count;
      if (best_size == count) break;
    }
  }
  if (best_idx != SIZE_MAX) {
    const uint32_t start = free_ranges_[best_idx].start;
    if (free_ranges_[best_idx].count == count) {
      free_ranges_.erase(free_ranges_.begin() + static_cast<ptrdiff_t>(best_idx));
    } else {
      free_ranges_[best_idx].start += count;
      free_ranges_[best_idx].count -= count;
    }
    return start;
  }
  if (bump_ + count > capacity_) return UINT32_MAX;
  const uint32_t start = bump_;
  bump_ += count;
  return start;
}

void DescriptorAllocator::free(uint32_t base, uint32_t count) {
  if (base == UINT32_MAX || count == 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (base + count == bump_) {
    bump_ = base;
    // Also reclaim any contiguous trailing free ranges.
    bool shrunk = true;
    while (shrunk) {
      shrunk = false;
      for (auto it = free_ranges_.begin(); it != free_ranges_.end(); ++it) {
        if (it->start + it->count == bump_) {
          bump_ = it->start;
          free_ranges_.erase(it);
          shrunk = true;
          break;
        }
      }
    }
    return;
  }
  free_ranges_.push_back({base, count});
  std::sort(free_ranges_.begin(), free_ranges_.end(),
            [](const Range& a, const Range& b) { return a.start < b.start; });
  std::vector<Range> merged;
  merged.reserve(free_ranges_.size());
  for (const Range& r : free_ranges_) {
    if (!merged.empty() && merged.back().start + merged.back().count == r.start) {
      merged.back().count += r.count;
    } else {
      merged.push_back(r);
    }
  }
  free_ranges_ = std::move(merged);
}

// -----------------------------------------------------------------------------
// Format & state translators (all 28 concrete engine formats)
// -----------------------------------------------------------------------------
DXGI_FORMAT to_dxgi_format(Format fmt) noexcept {
  switch (fmt) {
    case Format::Undefined:    return DXGI_FORMAT_UNKNOWN;
    case Format::R8Unorm:      return DXGI_FORMAT_R8_UNORM;
    case Format::Rgba8Unorm:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::Rgba8Srgb:    return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::Rgba8Snorm:   return DXGI_FORMAT_R8G8B8A8_SNORM;
    case Format::Bgra8Unorm:   return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Format::Bgra8Srgb:    return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case Format::Rgb10A2Unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case Format::Rg16Unorm:    return DXGI_FORMAT_R16G16_UNORM;
    case Format::Rg16Sfloat:   return DXGI_FORMAT_R16G16_FLOAT;
    case Format::Rgba16Sfloat: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::Rgba16Snorm:  return DXGI_FORMAT_R16G16B16A16_SNORM;
    case Format::Rgba16Sint:   return DXGI_FORMAT_R16G16B16A16_SINT;
    case Format::R32Sfloat:    return DXGI_FORMAT_R32_FLOAT;
    case Format::R32Uint:      return DXGI_FORMAT_R32_UINT;
    case Format::Rg32Sfloat:   return DXGI_FORMAT_R32G32_FLOAT;
    case Format::Rgb32Sfloat:  return DXGI_FORMAT_R32G32B32_FLOAT;
    case Format::Rgba32Sfloat: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case Format::Rgba32Uint:   return DXGI_FORMAT_R32G32B32A32_UINT;
    case Format::D32Sfloat:    return DXGI_FORMAT_D32_FLOAT;
    case Format::D16Unorm:     return DXGI_FORMAT_D16_UNORM;
    case Format::Bc1RgbaUnorm: return DXGI_FORMAT_BC1_UNORM;
    case Format::Bc1RgbaSrgb:  return DXGI_FORMAT_BC1_UNORM_SRGB;
    case Format::Bc2Unorm:     return DXGI_FORMAT_BC2_UNORM;
    case Format::Bc2Srgb:      return DXGI_FORMAT_BC2_UNORM_SRGB;
    case Format::Bc3Unorm:     return DXGI_FORMAT_BC3_UNORM;
    case Format::Bc3Srgb:      return DXGI_FORMAT_BC3_UNORM_SRGB;
    case Format::Bc4Unorm:     return DXGI_FORMAT_BC4_UNORM;
    case Format::Bc5Unorm:     return DXGI_FORMAT_BC5_UNORM;
    case Format::Count:        return DXGI_FORMAT_UNKNOWN;
  }
  return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT to_dxgi_resource_format(Format fmt, TextureUsage usage) noexcept {
  if (format_is_depth(fmt) &&
      (has_flag(usage, TextureUsage::Sampled) || has_flag(usage, TextureUsage::TransferSrc))) {
    if (fmt == Format::D32Sfloat) return DXGI_FORMAT_R32_TYPELESS;
    if (fmt == Format::D16Unorm) return DXGI_FORMAT_R16_TYPELESS;
  }
  return to_dxgi_format(fmt);
}

DXGI_FORMAT to_dxgi_srv_format(Format fmt) noexcept {
  if (fmt == Format::D32Sfloat) return DXGI_FORMAT_R32_FLOAT;
  if (fmt == Format::D16Unorm) return DXGI_FORMAT_R16_UNORM;
  return to_dxgi_format(fmt);
}

DXGI_FORMAT to_dxgi_dsv_format(Format fmt) noexcept {
  if (fmt == Format::D32Sfloat) return DXGI_FORMAT_D32_FLOAT;
  if (fmt == Format::D16Unorm) return DXGI_FORMAT_D16_UNORM;
  return to_dxgi_format(fmt);
}

Format from_dxgi_format(DXGI_FORMAT fmt) noexcept {
  switch (fmt) {
    case DXGI_FORMAT_R8_UNORM:               return Format::R8Unorm;
    case DXGI_FORMAT_R8G8B8A8_UNORM:         return Format::Rgba8Unorm;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:    return Format::Rgba8Srgb;
    case DXGI_FORMAT_R8G8B8A8_SNORM:         return Format::Rgba8Snorm;
    case DXGI_FORMAT_B8G8R8A8_UNORM:         return Format::Bgra8Unorm;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:    return Format::Bgra8Srgb;
    case DXGI_FORMAT_R10G10B10A2_UNORM:      return Format::Rgb10A2Unorm;
    case DXGI_FORMAT_R16G16_UNORM:           return Format::Rg16Unorm;
    case DXGI_FORMAT_R16G16_FLOAT:           return Format::Rg16Sfloat;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:     return Format::Rgba16Sfloat;
    case DXGI_FORMAT_R16G16B16A16_SNORM:     return Format::Rgba16Snorm;
    case DXGI_FORMAT_R16G16B16A16_SINT:      return Format::Rgba16Sint;
    case DXGI_FORMAT_R32_FLOAT:              return Format::R32Sfloat;
    case DXGI_FORMAT_R32_UINT:               return Format::R32Uint;
    case DXGI_FORMAT_R32G32_FLOAT:           return Format::Rg32Sfloat;
    case DXGI_FORMAT_R32G32B32_FLOAT:        return Format::Rgb32Sfloat;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:     return Format::Rgba32Sfloat;
    case DXGI_FORMAT_R32G32B32A32_UINT:      return Format::Rgba32Uint;
    case DXGI_FORMAT_D32_FLOAT:              return Format::D32Sfloat;
    case DXGI_FORMAT_D16_UNORM:              return Format::D16Unorm;
    case DXGI_FORMAT_BC1_UNORM:              return Format::Bc1RgbaUnorm;
    case DXGI_FORMAT_BC1_UNORM_SRGB:         return Format::Bc1RgbaSrgb;
    case DXGI_FORMAT_BC2_UNORM:              return Format::Bc2Unorm;
    case DXGI_FORMAT_BC2_UNORM_SRGB:         return Format::Bc2Srgb;
    case DXGI_FORMAT_BC3_UNORM:              return Format::Bc3Unorm;
    case DXGI_FORMAT_BC3_UNORM_SRGB:         return Format::Bc3Srgb;
    case DXGI_FORMAT_BC4_UNORM:              return Format::Bc4Unorm;
    case DXGI_FORMAT_BC5_UNORM:              return Format::Bc5Unorm;
    default:                                 return Format::Undefined;
  }
}

UINT to_dxgi_swizzle_mapping(const SwizzleMapping& s) noexcept {
  const auto r = map_component(s.r, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0);
  const auto g = map_component(s.g, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1);
  const auto b = map_component(s.b, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2);
  const auto a = map_component(s.a, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3);
  return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(r, g, b, a);
}

D3D12_FILTER to_d3d12_filter(Filter min_f, Filter mag_f, Filter mip_f,
                             bool compare, bool aniso) noexcept {
  const D3D12_FILTER_REDUCTION_TYPE reduction =
      compare ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
  if (aniso) {
    return D3D12_ENCODE_ANISOTROPIC_FILTER(reduction);
  }
  const D3D12_FILTER_TYPE dmin = (min_f == Filter::Linear) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
  const D3D12_FILTER_TYPE dmag = (mag_f == Filter::Linear) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
  const D3D12_FILTER_TYPE dmip = (mip_f == Filter::Linear) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
  return D3D12_ENCODE_BASIC_FILTER(dmin, dmag, dmip, reduction);
}

D3D12_TEXTURE_ADDRESS_MODE to_d3d12_address_mode(AddressMode m) noexcept {
  switch (m) {
    case AddressMode::Repeat:         return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case AddressMode::MirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case AddressMode::ClampToEdge:    return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case AddressMode::ClampToBorder:  return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
  }
  return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

D3D12_COMPARISON_FUNC to_d3d12_compare_op(CompareOp op) noexcept {
  switch (op) {
    case CompareOp::Never:          return D3D12_COMPARISON_FUNC_NEVER;
    case CompareOp::Less:           return D3D12_COMPARISON_FUNC_LESS;
    case CompareOp::Equal:          return D3D12_COMPARISON_FUNC_EQUAL;
    case CompareOp::LessOrEqual:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case CompareOp::Greater:        return D3D12_COMPARISON_FUNC_GREATER;
    case CompareOp::NotEqual:       return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case CompareOp::GreaterOrEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case CompareOp::Always:         return D3D12_COMPARISON_FUNC_ALWAYS;
  }
  return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
}

D3D12_BLEND to_d3d12_blend_factor(BlendFactor f) noexcept {
  switch (f) {
    case BlendFactor::Zero:                  return D3D12_BLEND_ZERO;
    case BlendFactor::One:                   return D3D12_BLEND_ONE;
    case BlendFactor::SrcColor:              return D3D12_BLEND_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor:      return D3D12_BLEND_INV_SRC_COLOR;
    case BlendFactor::DstColor:              return D3D12_BLEND_DEST_COLOR;
    case BlendFactor::OneMinusDstColor:      return D3D12_BLEND_INV_DEST_COLOR;
    case BlendFactor::SrcAlpha:              return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha:      return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DstAlpha:              return D3D12_BLEND_DEST_ALPHA;
    case BlendFactor::OneMinusDstAlpha:      return D3D12_BLEND_INV_DEST_ALPHA;
    case BlendFactor::ConstantColor:         return D3D12_BLEND_BLEND_FACTOR;
    case BlendFactor::OneMinusConstantColor: return D3D12_BLEND_INV_BLEND_FACTOR;
  }
  return D3D12_BLEND_ONE;
}

D3D12_BLEND_OP to_d3d12_blend_op(BlendOp op) noexcept {
  switch (op) {
    case BlendOp::Add:             return D3D12_BLEND_OP_ADD;
    case BlendOp::Subtract:        return D3D12_BLEND_OP_SUBTRACT;
    case BlendOp::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
    case BlendOp::Min:             return D3D12_BLEND_OP_MIN;
    case BlendOp::Max:             return D3D12_BLEND_OP_MAX;
  }
  return D3D12_BLEND_OP_ADD;
}

D3D12_CULL_MODE to_d3d12_cull_mode(CullMode c) noexcept {
  switch (c) {
    case CullMode::None:  return D3D12_CULL_MODE_NONE;
    case CullMode::Front: return D3D12_CULL_MODE_FRONT;
    case CullMode::Back:  return D3D12_CULL_MODE_BACK;
  }
  return D3D12_CULL_MODE_BACK;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE to_d3d12_topology_type(PrimitiveTopology t) noexcept {
  switch (t) {
    case PrimitiveTopology::TriangleList:
    case PrimitiveTopology::TriangleStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    case PrimitiveTopology::LineList:      return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
  }
  return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

D3D12_PRIMITIVE_TOPOLOGY to_d3d12_topology(PrimitiveTopology t) noexcept {
  switch (t) {
    case PrimitiveTopology::TriangleList:  return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case PrimitiveTopology::LineList:      return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
  }
  return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

D3D12_RESOURCE_STATES to_d3d12_resource_state(ResourceState state, bool is_depth) noexcept {
  if (state == ResourceState::Undefined || state == ResourceState::Common ||
      state == ResourceState::Present) {
    return D3D12_RESOURCE_STATE_COMMON;
  }
  if (has_flag(state, ResourceState::AccelStructRead) ||
      has_flag(state, ResourceState::AccelStructWrite)) {
    return D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
  }
  if (has_flag(state, ResourceState::RenderTarget)) {
    return D3D12_RESOURCE_STATE_RENDER_TARGET;
  }
  if (has_flag(state, ResourceState::DepthWrite)) {
    return D3D12_RESOURCE_STATE_DEPTH_WRITE;
  }
  if (has_flag(state, ResourceState::StorageReadWrite)) {
    return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  if (has_flag(state, ResourceState::CopyDst)) {
    return D3D12_RESOURCE_STATE_COPY_DEST;
  }
  if (has_flag(state, ResourceState::HostRead)) {
    return D3D12_RESOURCE_STATE_COPY_DEST;
  }

  D3D12_RESOURCE_STATES out = D3D12_RESOURCE_STATE_COMMON;
  if (has_flag(state, ResourceState::VertexBuffer) ||
      has_flag(state, ResourceState::UniformBuffer)) {
    out |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
  }
  if (has_flag(state, ResourceState::IndexBuffer)) {
    out |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
  }
  if (has_flag(state, ResourceState::IndirectArg)) {
    out |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
  }
  if (has_flag(state, ResourceState::CopySrc)) {
    out |= D3D12_RESOURCE_STATE_COPY_SOURCE;
  }
  if (has_flag(state, ResourceState::DepthRead)) {
    out |= D3D12_RESOURCE_STATE_DEPTH_READ;
  }
  if (has_flag(state, ResourceState::ShaderRead)) {
    out |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (is_depth) out |= D3D12_RESOURCE_STATE_DEPTH_READ;
  }
  return out;
}

D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS to_d3d12_as_build_flags(
    AccelStructBuildFlags flags) noexcept {
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS out =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
  if (has_flag(flags, AccelStructBuildFlags::PreferFastTrace)) {
    out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  }
  if (has_flag(flags, AccelStructBuildFlags::PreferFastBuild)) {
    out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
  }
  if (has_flag(flags, AccelStructBuildFlags::AllowCompaction)) {
    out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
  }
  if (has_flag(flags, AccelStructBuildFlags::AllowUpdate)) {
    out |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
  }
  return out;
}

D3D12_RAYTRACING_GEOMETRY_FLAGS to_d3d12_geometry_flags(GeometryFlags flags) noexcept {
  D3D12_RAYTRACING_GEOMETRY_FLAGS out = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
  if (has_flag(flags, GeometryFlags::Opaque)) {
    out |= D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  }
  if (has_flag(flags, GeometryFlags::NoDuplicateAnyHitInvocation)) {
    out |= D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
  }
  return out;
}

#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
D3D12_BARRIER_LAYOUT to_d3d12_barrier_layout(ResourceState state, bool is_depth) noexcept {
  switch (state) {
    case ResourceState::Undefined:        return D3D12_BARRIER_LAYOUT_UNDEFINED;
    case ResourceState::Common:           return D3D12_BARRIER_LAYOUT_COMMON;
    case ResourceState::Present:          return D3D12_BARRIER_LAYOUT_PRESENT;
    case ResourceState::RenderTarget:     return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    case ResourceState::DepthWrite:       return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
    case ResourceState::DepthRead:        return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
    case ResourceState::StorageReadWrite: return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
    case ResourceState::CopySrc:          return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
    case ResourceState::CopyDst:          return D3D12_BARRIER_LAYOUT_COPY_DEST;
    case ResourceState::ShaderRead:
      return is_depth ? D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ
                      : D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    default:
      if (has_flag(state, ResourceState::RenderTarget)) return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
      if (has_flag(state, ResourceState::DepthWrite)) return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
      if (has_flag(state, ResourceState::DepthRead)) return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
      if (has_flag(state, ResourceState::StorageReadWrite)) return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
      if (has_flag(state, ResourceState::CopyDst)) return D3D12_BARRIER_LAYOUT_COPY_DEST;
      if (has_flag(state, ResourceState::CopySrc)) return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
      if (has_flag(state, ResourceState::ShaderRead)) return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
      return D3D12_BARRIER_LAYOUT_COMMON;
  }
}

D3D12_BARRIER_SYNC to_d3d12_barrier_sync(ResourceState state, PipelineStage override_stage) noexcept {
  if (any_flag(override_stage)) {
    D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_NONE;
    if (has_flag(override_stage, PipelineStage::AllCommands)) return D3D12_BARRIER_SYNC_ALL;
    if (has_flag(override_stage, PipelineStage::AllGraphics)) sync |= D3D12_BARRIER_SYNC_DRAW;
    if (has_flag(override_stage, PipelineStage::DrawIndirect)) sync |= D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
    if (has_flag(override_stage, PipelineStage::VertexInput)) sync |= D3D12_BARRIER_SYNC_INDEX_INPUT | D3D12_BARRIER_SYNC_VERTEX_SHADING;
    if (has_flag(override_stage, PipelineStage::VertexShader)) sync |= D3D12_BARRIER_SYNC_VERTEX_SHADING;
    if (has_flag(override_stage, PipelineStage::FragmentShader)) sync |= D3D12_BARRIER_SYNC_PIXEL_SHADING;
    if (has_flag(override_stage, PipelineStage::EarlyFragmentTests) ||
        has_flag(override_stage, PipelineStage::LateFragmentTests)) {
      sync |= D3D12_BARRIER_SYNC_DEPTH_STENCIL;
    }
    if (has_flag(override_stage, PipelineStage::ColorAttachmentOut)) sync |= D3D12_BARRIER_SYNC_RENDER_TARGET;
    if (has_flag(override_stage, PipelineStage::ComputeShader)) sync |= D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    if (has_flag(override_stage, PipelineStage::Transfer) ||
        has_flag(override_stage, PipelineStage::Copy) ||
        has_flag(override_stage, PipelineStage::Blit) ||
        has_flag(override_stage, PipelineStage::Clear)) {
      sync |= D3D12_BARRIER_SYNC_COPY;
    }
    if (has_flag(override_stage, PipelineStage::AccelStructBuild)) {
      sync |= D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
    }
    if (has_flag(override_stage, PipelineStage::AccelStructCopy)) {
      sync |= D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE;
    }
    if (sync != D3D12_BARRIER_SYNC_NONE) return sync;
  }

  switch (state) {
    case ResourceState::Undefined:        return D3D12_BARRIER_SYNC_NONE;
    case ResourceState::Common:           return D3D12_BARRIER_SYNC_ALL;
    case ResourceState::Present:          return D3D12_BARRIER_SYNC_NONE;
    case ResourceState::RenderTarget:     return D3D12_BARRIER_SYNC_RENDER_TARGET;
    case ResourceState::DepthWrite:       return D3D12_BARRIER_SYNC_DEPTH_STENCIL;
    case ResourceState::DepthRead:        return D3D12_BARRIER_SYNC_DEPTH_STENCIL | D3D12_BARRIER_SYNC_ALL_SHADING;
    case ResourceState::StorageReadWrite: return D3D12_BARRIER_SYNC_ALL_SHADING;
    case ResourceState::ShaderRead:       return D3D12_BARRIER_SYNC_ALL_SHADING;
    case ResourceState::CopySrc:
    case ResourceState::CopyDst:          return D3D12_BARRIER_SYNC_COPY;
    case ResourceState::VertexBuffer:     return D3D12_BARRIER_SYNC_VERTEX_SHADING;
    case ResourceState::IndexBuffer:      return D3D12_BARRIER_SYNC_INDEX_INPUT;
    case ResourceState::UniformBuffer:    return D3D12_BARRIER_SYNC_ALL_SHADING;
    case ResourceState::IndirectArg:      return D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
    case ResourceState::AccelStructWrite:
    case ResourceState::AccelStructRead:  return D3D12_BARRIER_SYNC_ALL;
    case ResourceState::HostRead:         return D3D12_BARRIER_SYNC_COPY;
    default:                              return D3D12_BARRIER_SYNC_ALL;
  }
}

D3D12_BARRIER_ACCESS to_d3d12_barrier_access(ResourceState state, AccessFlags override_access,
                                             bool is_depth) noexcept {
  if (any_flag(override_access)) {
    D3D12_BARRIER_ACCESS acc = D3D12_BARRIER_ACCESS_COMMON;
    bool set = false;
    auto add = [&](D3D12_BARRIER_ACCESS bit) {
      acc = set ? (acc | bit) : bit;
      set = true;
    };
    if (has_flag(override_access, AccessFlags::IndirectCommandRead)) add(D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
    if (has_flag(override_access, AccessFlags::IndexRead)) add(D3D12_BARRIER_ACCESS_INDEX_BUFFER);
    if (has_flag(override_access, AccessFlags::VertexAttributeRead)) add(D3D12_BARRIER_ACCESS_VERTEX_BUFFER);
    if (has_flag(override_access, AccessFlags::UniformRead)) add(D3D12_BARRIER_ACCESS_CONSTANT_BUFFER);
    if (has_flag(override_access, AccessFlags::ShaderRead)) add(D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    if (has_flag(override_access, AccessFlags::ShaderWrite)) add(D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    if (has_flag(override_access, AccessFlags::ColorAttachmentRead) ||
        has_flag(override_access, AccessFlags::ColorAttachmentWrite)) {
      add(D3D12_BARRIER_ACCESS_RENDER_TARGET);
    }
    if (has_flag(override_access, AccessFlags::DepthAttachmentRead)) add(D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);
    if (has_flag(override_access, AccessFlags::DepthAttachmentWrite)) add(D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
    if (has_flag(override_access, AccessFlags::TransferRead)) add(D3D12_BARRIER_ACCESS_COPY_SOURCE);
    if (has_flag(override_access, AccessFlags::TransferWrite)) add(D3D12_BARRIER_ACCESS_COPY_DEST);
    if (has_flag(override_access, AccessFlags::AccelStructRead)) add(D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);
    if (has_flag(override_access, AccessFlags::AccelStructWrite)) add(D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE);
    if (set) return acc;
  }

  switch (state) {
    case ResourceState::Undefined:        return D3D12_BARRIER_ACCESS_NO_ACCESS;
    case ResourceState::Common:           return D3D12_BARRIER_ACCESS_COMMON;
    case ResourceState::Present:          return D3D12_BARRIER_ACCESS_NO_ACCESS;
    case ResourceState::RenderTarget:     return D3D12_BARRIER_ACCESS_RENDER_TARGET;
    case ResourceState::DepthWrite:       return D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
    case ResourceState::DepthRead:        return D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
    case ResourceState::StorageReadWrite: return D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    case ResourceState::ShaderRead:
      return is_depth ? (D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ | D3D12_BARRIER_ACCESS_SHADER_RESOURCE)
                      : D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    case ResourceState::CopySrc:          return D3D12_BARRIER_ACCESS_COPY_SOURCE;
    case ResourceState::CopyDst:          return D3D12_BARRIER_ACCESS_COPY_DEST;
    case ResourceState::VertexBuffer:     return D3D12_BARRIER_ACCESS_VERTEX_BUFFER;
    case ResourceState::IndexBuffer:      return D3D12_BARRIER_ACCESS_INDEX_BUFFER;
    case ResourceState::UniformBuffer:    return D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;
    case ResourceState::IndirectArg:      return D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;
    case ResourceState::AccelStructRead:  return D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ;
    case ResourceState::AccelStructWrite: return D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
    case ResourceState::HostRead:         return D3D12_BARRIER_ACCESS_COPY_DEST;
    default:                              return D3D12_BARRIER_ACCESS_COMMON;
  }
}
#endif

// -----------------------------------------------------------------------------
// D3D12BindlessTableImpl
// -----------------------------------------------------------------------------
D3D12BindlessTableImpl::D3D12BindlessTableImpl(D3D12DeviceImpl& device,
                                               BindGroupLayoutHandle layout_handle,
                                               BindGroupHandle group_handle,
                                               uint32_t max_textures,
                                               uint32_t max_storage_buffers)
    : device_(device),
      layout_handle_(layout_handle),
      group_handle_(group_handle),
      max_textures_(max_textures),
      max_storage_buffers_(max_storage_buffers) {}

D3D12BindlessTableImpl::~D3D12BindlessTableImpl() {
  if (group_handle_) device_.destroy_bind_group(group_handle_);
  if (layout_handle_) device_.destroy_bind_group_layout(layout_handle_);
}

void D3D12BindlessTableImpl::set_sampler(SamplerHandle sampler) {
  const BindGroupEntry* ge = device_.lookup_bind_group(group_handle_);
  if (!ge || ge->sampler_base == UINT32_MAX || ge->sampler_count == 0) return;
  device_.write_sampler(ge->sampler_base, sampler);
}

void D3D12BindlessTableImpl::set_texture(uint32_t slot, TextureViewHandle view) {
  if (slot >= max_textures_) return;
  const BindGroupEntry* ge = device_.lookup_bind_group(group_handle_);
  if (!ge || ge->cbv_srv_uav_base == UINT32_MAX) return;
  const uint32_t heap_slot = ge->cbv_srv_uav_base + slot;
  if (view) {
    device_.write_texture_srv(heap_slot, view);
  } else {
    device_.write_null_texture_srv(heap_slot);
  }
}

void D3D12BindlessTableImpl::set_storage_buffer(uint32_t slot, BufferHandle buffer,
                                                uint64_t offset, uint64_t size) {
  if (slot >= max_storage_buffers_) return;
  const BindGroupEntry* ge = device_.lookup_bind_group(group_handle_);
  if (!ge || ge->cbv_srv_uav_base == UINT32_MAX) return;
  const uint32_t heap_slot = ge->cbv_srv_uav_base + max_textures_ + slot;
  if (buffer) {
    device_.write_buffer_srv(heap_slot, buffer, offset, size);
  } else {
    device_.write_null_buffer_srv(heap_slot);
  }
}

// -----------------------------------------------------------------------------
// D3D12DeviceImpl lifecycle
// -----------------------------------------------------------------------------
D3D12DeviceImpl::~D3D12DeviceImpl() {
  wait_idle();
  {
    std::lock_guard<std::mutex> lock(sig_mutex_);
    indirect_sigs_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    pipelines_.clear();
    pipeline_layouts_.clear();
    bind_groups_.clear();
    bind_group_layouts_.clear();
    samplers_.clear();
    texture_views_.clear();
    textures_.clear();
    buffers_.clear();
    query_pools_.clear();
    accel_structs_.clear();
  }
  rtv_heap_.shutdown();
  dsv_heap_.shutdown();
  cbv_srv_uav_heap_.shutdown();
  sampler_heap_.shutdown();
  pipeline_library_.Reset();
  if (idle_fence_event_) {
    CloseHandle(idle_fence_event_);
    idle_fence_event_ = nullptr;
  }
  idle_fence_.Reset();
  compute_queue_.Reset();
  copy_queue_.Reset();
  direct_queue_.Reset();
  teardown_info_queue();
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
  device10_.Reset();
#endif
  device1_.Reset();
  device_.Reset();
  adapter_.Reset();
  factory_.Reset();
  if (dxcompiler_module_) {
    FreeLibrary(dxcompiler_module_);
    dxcompiler_module_ = nullptr;
  }
}

bool D3D12DeviceImpl::create_factory_and_adapter(const DeviceDesc& desc, std::string& error) {
  UINT factory_flags = 0;
  if (desc.enable_validation) {
    ComPtr<ID3D12Debug> debug0;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug0)))) {
      debug0->EnableDebugLayer();
      validation_enabled_ = true;
      factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
  }

  HRESULT hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_));
  if (FAILED(hr) && (factory_flags & DXGI_CREATE_FACTORY_DEBUG)) {
    factory_flags &= ~DXGI_CREATE_FACTORY_DEBUG;
    hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_));
  }
  if (FAILED(hr)) {
    error = hr_err("CreateDXGIFactory2", hr);
    return false;
  }

  // Probe tearing support via IDXGIFactory5.
  BOOL tearing = FALSE;
  if (SUCCEEDED(factory_->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                              &tearing, sizeof(tearing)))) {
    allow_tearing_ = (tearing == TRUE);
  }

  // Prefer high-performance hardware adapters first.
  auto try_adapter = [&](IDXGIAdapter4* cand) -> bool {
    ComPtr<ID3D12Device5> dev;
    HRESULT dhr = D3D12CreateDevice(cand, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&dev));
    if (FAILED(dhr)) {
      dhr = D3D12CreateDevice(cand, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    }
    if (FAILED(dhr)) return false;
    adapter_ = cand;
    device_ = dev;
    return true;
  };

  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter4> cand;
    if (factory_->EnumAdapterByGpuPreference(
            i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&cand)) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    DXGI_ADAPTER_DESC3 ad{};
    if (FAILED(cand->GetDesc3(&ad))) continue;
    if (ad.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) continue;
    if (try_adapter(cand.Get())) break;
  }

  // Fallback to WARP software adapter if no hardware adapter succeeded.
  if (!device_) {
    ComPtr<IDXGIAdapter4> warp;
    if (SUCCEEDED(factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
      try_adapter(warp.Get());
    }
  }

  if (!device_) {
    error = "no DirectX 12 (Feature Level 12_0+) adapter available";
    return false;
  }

  device_.As(&device1_);
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
  device_.As(&device10_);
#endif
  return true;
}

void D3D12DeviceImpl::setup_info_queue() {
  if (!validation_enabled_ || !device_) return;
  if (SUCCEEDED(device_.As(&info_queue_))) {
    // Filter only non-error performance hints about optimized clear values.
    D3D12_MESSAGE_ID deny_ids[] = {
        D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
        D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
    };
    D3D12_INFO_QUEUE_FILTER filter{};
    filter.DenyList.NumIDs = static_cast<UINT>(std::size(deny_ids));
    filter.DenyList.pIDList = deny_ids;
    info_queue_->AddStorageFilterEntries(&filter);
  }
#ifdef __ID3D12InfoQueue1_INTERFACE_DEFINED__
  if (SUCCEEDED(device_.As(&info_queue1_))) {
    info_queue1_->RegisterMessageCallback(
        d3d12_debug_callback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, this, &info_queue_cookie_);
  }
#endif
}

void D3D12DeviceImpl::teardown_info_queue() {
#ifdef __ID3D12InfoQueue1_INTERFACE_DEFINED__
  if (info_queue1_ && info_queue_cookie_ != 0) {
    info_queue1_->UnregisterMessageCallback(info_queue_cookie_);
    info_queue_cookie_ = 0;
  }
  info_queue1_.Reset();
#endif
  info_queue_.Reset();
}

bool D3D12DeviceImpl::probe_capabilities(const DeviceDesc& desc, std::string& error) {
  (void)error;
  DXGI_ADAPTER_DESC3 ad{};
  adapter_->GetDesc3(&ad);

  caps_ = {};
  caps_.backend = BackendKind::D3D12;
  caps_.device_name = wide_to_utf8(ad.Description);
  caps_.driver_name = "Direct3D 12";
  caps_.vendor_id = ad.VendorId;
  caps_.device_id = ad.DeviceId;
  caps_.device_local_bytes = static_cast<uint64_t>(ad.DedicatedVideoMemory);
  caps_.is_software = (ad.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0;
  caps_.is_discrete = !caps_.is_software && ad.DedicatedVideoMemory >= (256ull << 20);
  caps_.is_integrated = !caps_.is_software && !caps_.is_discrete;

  LARGE_INTEGER umd_ver{};
  if (SUCCEEDED(adapter_->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd_ver))) {
    caps_.driver_version = static_cast<uint32_t>(umd_ver.LowPart);
    char drv_buf[64];
    std::snprintf(drv_buf, sizeof(drv_buf), "%u.%u.%u.%u",
                  static_cast<unsigned>((umd_ver.QuadPart >> 48) & 0xFFFF),
                  static_cast<unsigned>((umd_ver.QuadPart >> 32) & 0xFFFF),
                  static_cast<unsigned>((umd_ver.QuadPart >> 16) & 0xFFFF),
                  static_cast<unsigned>(umd_ver.QuadPart & 0xFFFF));
    caps_.driver_info = drv_buf;
  }

  // Deterministic 16-byte pipeline cache UUID from AdapterLuid + UMD version.
  std::memcpy(caps_.pipeline_cache_uuid.data(), &ad.AdapterLuid, sizeof(LUID));
  std::memcpy(caps_.pipeline_cache_uuid.data() + 8, &umd_ver.QuadPart, sizeof(uint64_t));

  // Shader Model probe (SM 6.6 dynamic resources down to SM 6.0).
  D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
  if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
    caps_.api_version = static_cast<uint32_t>(sm.HighestShaderModel);
    sm66_dynamic_resources_ = (sm.HighestShaderModel >= D3D_SHADER_MODEL_6_6);
  }

  // Resource binding tier -> bindless 4096 textures.
  D3D12_FEATURE_DATA_D3D12_OPTIONS opt0{};
  if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt0, sizeof(opt0)))) {
    if (desc.enable_bindless && opt0.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2) {
      caps_.bindless = true;
      caps_.max_bindless_textures = 4096u;
    }
  }

  // DXR 1.1 probe.
#ifdef SHIFT_ENABLE_RAY_TRACING
  if (desc.enable_ray_tracing) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5)))) {
      if (opt5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1) {
        caps_.ray_query = true;
        caps_.rt_pipeline = true;
        caps_.rt_maintenance1 = true;
        caps_.min_as_scratch_alignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
      }
    }
  }
#endif

  // VRS Tier 1/2 probe.
  D3D12_FEATURE_DATA_D3D12_OPTIONS6 opt6{};
  if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &opt6, sizeof(opt6)))) {
    caps_.fragment_shading_rate = (opt6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_1);
  }

  // Enhanced Barriers probe (ID3D12Device10 + Options12).
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 opt12{};
  if (device10_ &&
      SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &opt12, sizeof(opt12)))) {
    enhanced_barriers_enabled_ = (opt12.EnhancedBarriersSupported == TRUE);
  }
#endif

  // MSAA sample counts.
  uint32_t msaa_mask = 1u;
  for (uint32_t s : {2u, 4u, 8u}) {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
    q.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    q.SampleCount = s;
    if (SUCCEEDED(device_->CheckFeatureSupport(
            D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) &&
        q.NumQualityLevels > 0) {
      msaa_mask |= s;
    }
  }
  caps_.supported_msaa_mask = msaa_mask;

  // HDR10 / scRGB output & format capability probe.
  D3D12_FEATURE_DATA_FORMAT_SUPPORT hdr10_fmt{DXGI_FORMAT_R10G10B10A2_UNORM, {}, {}};
  D3D12_FEATURE_DATA_FORMAT_SUPPORT scrgb_fmt{DXGI_FORMAT_R16G16B16A16_FLOAT, {}, {}};
  const bool hdr10_rt =
      SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &hdr10_fmt, sizeof(hdr10_fmt))) &&
      (hdr10_fmt.Support1 & D3D12_FORMAT_SUPPORT1_DISPLAY) &&
      (hdr10_fmt.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
  const bool scrgb_rt =
      SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &scrgb_fmt, sizeof(scrgb_fmt))) &&
      (scrgb_fmt.Support1 & D3D12_FORMAT_SUPPORT1_DISPLAY) &&
      (scrgb_fmt.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET);

  bool output_hdr2084 = false;
  for (UINT oi = 0;; ++oi) {
    ComPtr<IDXGIOutput> out;
    if (adapter_->EnumOutputs(oi, &out) == DXGI_ERROR_NOT_FOUND) break;
    ComPtr<IDXGIOutput6> out6;
    if (SUCCEEDED(out.As(&out6))) {
      DXGI_OUTPUT_DESC1 od{};
      if (SUCCEEDED(out6->GetDesc1(&od)) &&
          od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
        output_hdr2084 = true;
      }
    }
  }
  caps_.hdr10 = hdr10_rt && output_hdr2084;
  caps_.scrgb = scrgb_rt && output_hdr2084;

  // GPU timestamps from direct queue frequency.
  UINT64 ts_freq = 0;
  if (direct_queue_ && SUCCEEDED(direct_queue_->GetTimestampFrequency(&ts_freq)) && ts_freq > 0) {
    caps_.timestamps = true;
    caps_.timestamp_period_ns = 1e9 / static_cast<double>(ts_freq);
  }

  caps_.dynamic_rendering = true;
  caps_.synchronization2 = enhanced_barriers_enabled_;
  caps_.indirect_count = true;
  caps_.multi_draw_indirect = true;
  caps_.depth_resolve_sample_zero = true;
  caps_.buffer_device_address = true;
  caps_.max_anisotropy = std::clamp(desc.max_anisotropy, 1.0f, 16.0f);

  // Runtime probe for dxcompiler.dll (DXIL shader compiler).
  dxcompiler_module_ = LoadLibraryW(L"dxcompiler.dll");
  dxcompiler_available_ = (dxcompiler_module_ != nullptr);

  cache_key_.backend = BackendKind::D3D12;
  cache_key_.vendor_id = caps_.vendor_id;
  cache_key_.device_id = caps_.device_id;
  cache_key_.uuid = caps_.pipeline_cache_uuid;
  return true;
}

void D3D12DeviceImpl::load_pipeline_library() {
  if (!device1_) return;
  if (!pipeline_cache_path_.empty()) {
    std::ifstream in(pipeline_cache_path_, std::ios::binary | std::ios::ate);
    if (in) {
      const std::streamsize sz = in.tellg();
      if (sz > 0) {
        std::vector<uint8_t> raw(static_cast<size_t>(sz));
        in.seekg(0);
        if (in.read(reinterpret_cast<char*>(raw.data()), sz)) {
          std::vector<uint8_t> payload;
          if (decode_pipeline_cache_envelope(raw.data(), raw.size(), cache_key_, payload)) {
            if (SUCCEEDED(device1_->CreatePipelineLibrary(
                    payload.data(), payload.size(), IID_PPV_ARGS(&pipeline_library_)))) {
              pipeline_cache_loaded_ = true;
              return;
            }
          }
        }
      }
    }
  }
  // Fall back to empty library so subsequent PSOs can be cached and saved.
  device1_->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&pipeline_library_));
}

bool D3D12DeviceImpl::init(const DeviceDesc& desc, std::string& error) {
  pipeline_cache_path_ = desc.pipeline_cache_path;

  if (!create_factory_and_adapter(desc, error)) return false;
  setup_info_queue();

  // Direct command queue.
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  HRESULT hr = device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&direct_queue_));
  if (FAILED(hr)) {
    error = hr_err("CreateCommandQueue(DIRECT)", hr);
    return false;
  }
  set_d3d12_debug_name(direct_queue_.Get(), "shift_rhi_direct_queue");

  // Optional Copy & Compute queues.
  if (desc.enable_transfer_queue) {
    D3D12_COMMAND_QUEUE_DESC cqd{};
    cqd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (SUCCEEDED(device_->CreateCommandQueue(&cqd, IID_PPV_ARGS(&copy_queue_)))) {
      set_d3d12_debug_name(copy_queue_.Get(), "shift_rhi_copy_queue");
    }
  }
  {
    D3D12_COMMAND_QUEUE_DESC cmpqd{};
    cmpqd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (SUCCEEDED(device_->CreateCommandQueue(&cmpqd, IID_PPV_ARGS(&compute_queue_)))) {
      set_d3d12_debug_name(compute_queue_.Get(), "shift_rhi_compute_queue");
    }
  }

  hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idle_fence_));
  if (FAILED(hr)) {
    error = hr_err("CreateFence(idle)", hr);
    return false;
  }
  idle_fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!idle_fence_event_) {
    error = "CreateEventW(idle_fence) failed";
    return false;
  }

  if (!probe_capabilities(desc, error)) return false;

  // Descriptor heaps: RTV (1024), DSV (512), shader-visible CBV/SRV/UAV (32768), Sampler (2048).
  if (!rtv_heap_.init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024u, false, error)) return false;
  if (!dsv_heap_.init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 512u, false, error)) return false;
  if (!cbv_srv_uav_heap_.init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32768u, true, error)) {
    return false;
  }
  if (!sampler_heap_.init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048u, true, error)) {
    return false;
  }

  load_pipeline_library();
  return true;
}

VramStats D3D12DeviceImpl::vram_stats() const {
  VramStats s{};
  s.allocation_count = live_alloc_count_.load();
  s.block_count = s.allocation_count;
  s.allocation_bytes = live_alloc_bytes_.load();
  s.block_bytes = s.allocation_bytes;
  if (adapter_) {
    DXGI_QUERY_VIDEO_MEMORY_INFO vmi{};
    if (SUCCEEDED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmi))) {
      s.device_local_usage = vmi.CurrentUsage;
      s.device_local_budget = vmi.Budget;
    }
  }
  return s;
}

uint32_t D3D12DeviceImpl::validation_message_count() const {
  uint32_t count = validation_errors_.load();
  // If ID3D12InfoQueue1 callback was not registered, scan stored messages directly.
#ifdef __ID3D12InfoQueue1_INTERFACE_DEFINED__
  const bool has_cb = (info_queue1_ != nullptr && info_queue_cookie_ != 0);
#else
  const bool has_cb = false;
#endif
  if (!has_cb && info_queue_) {
    const UINT64 n = info_queue_->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < n; ++i) {
      SIZE_T msg_len = 0;
      if (SUCCEEDED(info_queue_->GetMessage(i, nullptr, &msg_len)) && msg_len > 0) {
        std::vector<uint8_t> storage(msg_len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(info_queue_->GetMessage(i, msg, &msg_len))) {
          if (msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
              msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
            ++count;
          }
        }
      }
    }
  }
  return count;
}

bool D3D12DeviceImpl::save_pipeline_cache(std::string& error) {
  if (pipeline_cache_path_.empty()) return true;
  if (!pipeline_library_) return true;

  std::vector<uint8_t> payload;
  {
    std::lock_guard<std::mutex> lock(pipeline_library_mutex_);
    const SIZE_T bytes = pipeline_library_->GetSerializedSize();
    if (bytes == 0) return true;
    payload.resize(bytes);
    const HRESULT hr = pipeline_library_->Serialize(payload.data(), bytes);
    if (FAILED(hr)) {
      error = hr_err("ID3D12PipelineLibrary::Serialize", hr);
      return false;
    }
  }

  const std::vector<uint8_t> encoded =
      encode_pipeline_cache_envelope(cache_key_, payload.data(), payload.size());
  std::error_code ec;
  if (pipeline_cache_path_.has_parent_path()) {
    std::filesystem::create_directories(pipeline_cache_path_.parent_path(), ec);
  }
  const std::filesystem::path tmp = pipeline_cache_path_.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "failed to open pipeline cache file for writing: " + tmp.string();
      return false;
    }
    out.write(reinterpret_cast<const char*>(encoded.data()),
              static_cast<std::streamsize>(encoded.size()));
    if (!out) {
      error = "failed writing pipeline cache file";
      return false;
    }
  }
  std::filesystem::rename(tmp, pipeline_cache_path_, ec);
  if (ec) {
    std::filesystem::remove(pipeline_cache_path_, ec);
    ec.clear();
    std::filesystem::rename(tmp, pipeline_cache_path_, ec);
    if (ec) {
      error = "failed to commit pipeline cache file: " + ec.message();
      return false;
    }
  }
  return true;
}

void D3D12DeviceImpl::signal_and_wait_queue(ID3D12CommandQueue* queue) {
  if (!queue || !idle_fence_ || !idle_fence_event_) return;
  const uint64_t val = ++idle_fence_value_;
  if (SUCCEEDED(queue->Signal(idle_fence_.Get(), val))) {
    if (idle_fence_->GetCompletedValue() < val) {
      if (SUCCEEDED(idle_fence_->SetEventOnCompletion(val, idle_fence_event_))) {
        WaitForSingleObject(idle_fence_event_, INFINITE);
      }
    }
  }
}

void D3D12DeviceImpl::wait_idle() {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (direct_queue_) signal_and_wait_queue(direct_queue_.Get());
  if (copy_queue_) signal_and_wait_queue(copy_queue_.Get());
  if (compute_queue_) signal_and_wait_queue(compute_queue_.Get());
}

// -----------------------------------------------------------------------------
// Resource lookups
// -----------------------------------------------------------------------------
BufferEntry* D3D12DeviceImpl::lookup_buffer(BufferHandle h) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return buffers_.get(h);
}
const BufferEntry* D3D12DeviceImpl::lookup_buffer(BufferHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return buffers_.get(h);
}
TextureEntry* D3D12DeviceImpl::lookup_texture(TextureHandle h) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return textures_.get(h);
}
const TextureEntry* D3D12DeviceImpl::lookup_texture(TextureHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return textures_.get(h);
}
const TextureViewEntry* D3D12DeviceImpl::lookup_texture_view(TextureViewHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return texture_views_.get(h);
}
const SamplerEntry* D3D12DeviceImpl::lookup_sampler(SamplerHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return samplers_.get(h);
}
const BindGroupLayoutEntry* D3D12DeviceImpl::lookup_bind_group_layout(BindGroupLayoutHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return bind_group_layouts_.get(h);
}
const BindGroupEntry* D3D12DeviceImpl::lookup_bind_group(BindGroupHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return bind_groups_.get(h);
}
const PipelineLayoutEntry* D3D12DeviceImpl::lookup_pipeline_layout(PipelineLayoutHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return pipeline_layouts_.get(h);
}
PipelineEntry* D3D12DeviceImpl::lookup_pipeline(PipelineHandle h) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return pipelines_.get(h);
}
const PipelineEntry* D3D12DeviceImpl::lookup_pipeline(PipelineHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return pipelines_.get(h);
}
const QueryPoolEntry* D3D12DeviceImpl::lookup_query_pool(QueryPoolHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return query_pools_.get(h);
}
const AccelStructEntry* D3D12DeviceImpl::lookup_accel_struct(AccelStructHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return accel_structs_.get(h);
}

// -----------------------------------------------------------------------------
// Buffers
// -----------------------------------------------------------------------------
BufferHandle D3D12DeviceImpl::create_buffer(const BufferDesc& desc, std::string& error) {
  if (desc.size == 0) {
    error = "create_buffer: size must be > 0";
    return {};
  }

  D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_STATES init_state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_FLAGS res_flags = D3D12_RESOURCE_FLAG_NONE;
  bool allow_uav = false;
  bool fixed_state = false;

  uint64_t alloc_size = desc.size;
  if (has_flag(desc.usage, BufferUsage::Uniform) ||
      has_flag(desc.usage, BufferUsage::AccelStructStorage)) {
    alloc_size = align_up(alloc_size, 256ull);
  } else {
    alloc_size = align_up(alloc_size, 4ull);
  }

  switch (desc.memory) {
    case MemoryHint::CpuToGpu:
      heap_type = D3D12_HEAP_TYPE_UPLOAD;
      init_state = D3D12_RESOURCE_STATE_GENERIC_READ;
      fixed_state = true;
      break;
    case MemoryHint::GpuToCpu:
      heap_type = D3D12_HEAP_TYPE_READBACK;
      init_state = D3D12_RESOURCE_STATE_COPY_DEST;
      fixed_state = true;
      break;
    case MemoryHint::GpuOnly:
    case MemoryHint::Transient:
      heap_type = D3D12_HEAP_TYPE_DEFAULT;
      if (has_flag(desc.usage, BufferUsage::AccelStructStorage)) {
        res_flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        init_state = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        allow_uav = true;
        fixed_state = true;
      } else if (has_flag(desc.usage, BufferUsage::Storage)) {
        res_flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        allow_uav = true;
      }
      break;
  }

  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = heap_type;
  hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Alignment = 0;
  rd.Width = alloc_size;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_UNKNOWN;
  rd.SampleDesc.Count = 1;
  rd.SampleDesc.Quality = 0;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd.Flags = res_flags;

  BufferEntry entry{};
  const HRESULT hr = device_->CreateCommittedResource(
      &hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr, IID_PPV_ARGS(&entry.resource));
  if (FAILED(hr)) {
    error = hr_err("CreateCommittedResource(buffer)", hr);
    return {};
  }

  set_d3d12_debug_name(entry.resource.Get(), desc.debug_name);
  entry.size = desc.size;
  entry.allocated_size = alloc_size;
  entry.usage = desc.usage;
  entry.memory = desc.memory;
  entry.heap_type = heap_type;
  entry.current_state = init_state;
  entry.allow_uav = allow_uav;
  entry.fixed_state = fixed_state;
  entry.gpu_va = entry.resource->GetGPUVirtualAddress();

  if (desc.persistently_mapped &&
      (heap_type == D3D12_HEAP_TYPE_UPLOAD || heap_type == D3D12_HEAP_TYPE_READBACK)) {
    D3D12_RANGE read_range = (heap_type == D3D12_HEAP_TYPE_READBACK)
                                 ? D3D12_RANGE{0, static_cast<SIZE_T>(alloc_size)}
                                 : D3D12_RANGE{0, 0};
    void* ptr = nullptr;
    if (SUCCEEDED(entry.resource->Map(0, &read_range, &ptr))) {
      entry.persistent_map = ptr;
    }
  }

  ++live_alloc_count_;
  live_alloc_bytes_ += alloc_size;

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return buffers_.allocate(std::move(entry));
}

void D3D12DeviceImpl::destroy_buffer(BufferHandle handle) {
  BufferEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!buffers_.remove(handle, &entry)) return;
  }
  if (entry.persistent_map && entry.resource) {
    entry.resource->Unmap(0, nullptr);
    entry.persistent_map = nullptr;
  }
  if (entry.resource) {
    --live_alloc_count_;
    live_alloc_bytes_ -= entry.allocated_size;
    entry.resource.Reset();
  }
}

void* D3D12DeviceImpl::map_buffer(BufferHandle handle) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  BufferEntry* be = buffers_.get(handle);
  if (!be || !be->resource) return nullptr;
  if (be->persistent_map) return be->persistent_map;
  if (be->heap_type != D3D12_HEAP_TYPE_UPLOAD && be->heap_type != D3D12_HEAP_TYPE_READBACK) {
    return nullptr;
  }
  D3D12_RANGE read_range = (be->heap_type == D3D12_HEAP_TYPE_READBACK)
                               ? D3D12_RANGE{0, static_cast<SIZE_T>(be->allocated_size)}
                               : D3D12_RANGE{0, 0};
  void* ptr = nullptr;
  if (FAILED(be->resource->Map(0, &read_range, &ptr))) return nullptr;
  return ptr;
}

void D3D12DeviceImpl::unmap_buffer(BufferHandle handle) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  BufferEntry* be = buffers_.get(handle);
  if (!be || !be->resource || be->persistent_map) return;
  D3D12_RANGE write_range = (be->heap_type == D3D12_HEAP_TYPE_READBACK)
                                ? D3D12_RANGE{0, 0}
                                : D3D12_RANGE{0, static_cast<SIZE_T>(be->allocated_size)};
  be->resource->Unmap(0, &write_range);
}

uint64_t D3D12DeviceImpl::buffer_device_address(BufferHandle handle) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  const BufferEntry* be = buffers_.get(handle);
  return be ? be->gpu_va : 0ull;
}

// -----------------------------------------------------------------------------
// Textures & Texture Views
// -----------------------------------------------------------------------------
TextureHandle D3D12DeviceImpl::create_texture(const TextureDesc& desc, std::string& error) {
  const DXGI_FORMAT res_fmt = to_dxgi_resource_format(desc.format, desc.usage);
  if (res_fmt == DXGI_FORMAT_UNKNOWN) {
    error = "create_texture: unsupported format";
    return {};
  }

  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
  if (has_flag(desc.usage, TextureUsage::ColorAttachment)) {
    flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  }
  if (has_flag(desc.usage, TextureUsage::DepthAttachment)) {
    flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  }
  if (has_flag(desc.usage, TextureUsage::Storage)) {
    flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  }

  D3D12_CLEAR_VALUE clear_val{};
  const D3D12_CLEAR_VALUE* p_clear = nullptr;
  if (has_flag(desc.usage, TextureUsage::ColorAttachment)) {
    clear_val.Format = to_dxgi_format(desc.format);
    clear_val.Color[0] = 0.0f;
    clear_val.Color[1] = 0.0f;
    clear_val.Color[2] = 0.0f;
    clear_val.Color[3] = 1.0f;
    p_clear = &clear_val;
  } else if (has_flag(desc.usage, TextureUsage::DepthAttachment)) {
    clear_val.Format = to_dxgi_dsv_format(desc.format);
    clear_val.DepthStencil.Depth = 0.0f;
    clear_val.DepthStencil.Stencil = 0;
    p_clear = &clear_val;
  }

  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;

  const UINT16 depth_or_array =
      (desc.type == TextureType::Tex3D)
          ? static_cast<UINT16>(std::max(desc.depth, 1u))
          : static_cast<UINT16>(std::max(desc.array_layers, 1u));
  const D3D12_RESOURCE_DIMENSION dim =
      (desc.type == TextureType::Tex3D) ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                        : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  const uint32_t sample_count = std::max(desc.sample_count, 1u);

  TextureEntry entry{};
  HRESULT hr = E_FAIL;

#ifdef __ID3D12Device10_INTERFACE_DEFINED__
  if (enhanced_barriers_enabled_ && device10_) {
    D3D12_RESOURCE_DESC1 rd1{};
    rd1.Dimension = dim;
    rd1.Alignment = 0;
    rd1.Width = std::max(desc.width, 1u);
    rd1.Height = std::max(desc.height, 1u);
    rd1.DepthOrArraySize = depth_or_array;
    rd1.MipLevels = static_cast<UINT16>(std::max(desc.mip_levels, 1u));
    rd1.Format = res_fmt;
    rd1.SampleDesc.Count = sample_count;
    rd1.SampleDesc.Quality = 0;
    rd1.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd1.Flags = flags;
    hr = device10_->CreateCommittedResource3(
        &hp, D3D12_HEAP_FLAG_NONE, &rd1, D3D12_BARRIER_LAYOUT_COMMON, p_clear,
        nullptr, 0, nullptr, IID_PPV_ARGS(&entry.resource));
    if (SUCCEEDED(hr)) {
      entry.enhanced_barrier_compatible = true;
      entry.current_layout = D3D12_BARRIER_LAYOUT_COMMON;
    }
  }
#endif

  if (FAILED(hr)) {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = dim;
    rd.Alignment = 0;
    rd.Width = std::max(desc.width, 1u);
    rd.Height = std::max(desc.height, 1u);
    rd.DepthOrArraySize = depth_or_array;
    rd.MipLevels = static_cast<UINT16>(std::max(desc.mip_levels, 1u));
    rd.Format = res_fmt;
    rd.SampleDesc.Count = sample_count;
    rd.SampleDesc.Quality = 0;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = flags;
    hr = device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, p_clear,
        IID_PPV_ARGS(&entry.resource));
    if (FAILED(hr)) {
      error = hr_err("CreateCommittedResource(texture)", hr);
      return {};
    }
    entry.enhanced_barrier_compatible = false;
  }

  set_d3d12_debug_name(entry.resource.Get(), desc.debug_name);
  const D3D12_RESOURCE_DESC created_desc = entry.resource->GetDesc();
  const D3D12_RESOURCE_ALLOCATION_INFO alloc_info =
      device_->GetResourceAllocationInfo(0, 1, &created_desc);

  entry.resource_format = res_fmt;
  entry.copy_format = (res_fmt == DXGI_FORMAT_R32_TYPELESS) ? DXGI_FORMAT_R32_FLOAT
                      : (res_fmt == DXGI_FORMAT_R16_TYPELESS) ? DXGI_FORMAT_R16_UNORM
                                                              : res_fmt;
  entry.format = desc.format;
  entry.type = desc.type;
  entry.usage = desc.usage;
  entry.width = std::max(desc.width, 1u);
  entry.height = std::max(desc.height, 1u);
  entry.depth = std::max(desc.depth, 1u);
  entry.mip_levels = std::max(desc.mip_levels, 1u);
  entry.array_layers = std::max(desc.array_layers, 1u);
  entry.sample_count = sample_count;
  entry.allocated_size = alloc_info.SizeInBytes;
  entry.current_state = D3D12_RESOURCE_STATE_COMMON;
  entry.owned = true;

  ++live_alloc_count_;
  live_alloc_bytes_ += entry.allocated_size;

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return textures_.allocate(std::move(entry));
}

void D3D12DeviceImpl::destroy_texture(TextureHandle handle) {
  TextureEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!textures_.remove(handle, &entry)) return;
  }
  if (entry.owned && entry.resource) {
    --live_alloc_count_;
    live_alloc_bytes_ -= entry.allocated_size;
  }
  entry.resource.Reset();
}

TextureViewHandle D3D12DeviceImpl::create_texture_view(const TextureViewDesc& desc, std::string& error) {
  TextureEntry tex_copy{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const TextureEntry* te = textures_.get(desc.texture);
    if (!te || !te->resource) {
      error = "create_texture_view: invalid TextureHandle";
      return {};
    }
    tex_copy = *te;
  }

  const Format view_fmt = (desc.format == Format::Undefined) ? tex_copy.format : desc.format;
  const bool is_depth = format_is_depth(view_fmt);

  TextureViewEntry entry{};
  entry.texture = desc.texture;
  entry.format = view_fmt;
  entry.type = desc.type;
  entry.base_mip = desc.base_mip;
  entry.mip_count = std::max(desc.mip_count, 1u);
  entry.base_layer = desc.base_layer;
  entry.layer_count = std::max(desc.layer_count, 1u);
  entry.swizzle = desc.swizzle;
  entry.is_depth = is_depth;

  if (!is_depth && has_flag(tex_copy.usage, TextureUsage::ColorAttachment)) {
    const uint32_t slot = rtv_heap_.allocate(1);
    if (slot == UINT32_MAX) {
      error = "create_texture_view: RTV descriptor heap exhausted";
      return {};
    }
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = to_dxgi_format(view_fmt);
    if (tex_copy.sample_count > 1) {
      if (tex_copy.array_layers > 1 || desc.base_layer > 0) {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
        rtv.Texture2DMSArray.FirstArraySlice = desc.base_layer;
        rtv.Texture2DMSArray.ArraySize = entry.layer_count;
      } else {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
      }
    } else if (tex_copy.type == TextureType::Tex3D) {
      rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
      rtv.Texture3D.MipSlice = desc.base_mip;
      rtv.Texture3D.FirstWSlice = desc.base_layer;
      rtv.Texture3D.WSize = entry.layer_count;
    } else if (tex_copy.array_layers > 1 || desc.base_layer > 0) {
      rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
      rtv.Texture2DArray.MipSlice = desc.base_mip;
      rtv.Texture2DArray.FirstArraySlice = desc.base_layer;
      rtv.Texture2DArray.ArraySize = entry.layer_count;
    } else {
      rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      rtv.Texture2D.MipSlice = desc.base_mip;
    }
    device_->CreateRenderTargetView(tex_copy.resource.Get(), &rtv, rtv_heap_.cpu_handle(slot));
    entry.has_rtv = true;
    entry.rtv_slot = slot;
  }

  if (is_depth && has_flag(tex_copy.usage, TextureUsage::DepthAttachment)) {
    const uint32_t slot = dsv_heap_.allocate(1);
    if (slot == UINT32_MAX) {
      if (entry.has_rtv) rtv_heap_.free(entry.rtv_slot, 1);
      error = "create_texture_view: DSV descriptor heap exhausted";
      return {};
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = to_dxgi_dsv_format(view_fmt);
    if (tex_copy.sample_count > 1) {
      if (tex_copy.array_layers > 1 || desc.base_layer > 0) {
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
        dsv.Texture2DMSArray.FirstArraySlice = desc.base_layer;
        dsv.Texture2DMSArray.ArraySize = entry.layer_count;
      } else {
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
      }
    } else if (tex_copy.array_layers > 1 || desc.base_layer > 0) {
      dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
      dsv.Texture2DArray.MipSlice = desc.base_mip;
      dsv.Texture2DArray.FirstArraySlice = desc.base_layer;
      dsv.Texture2DArray.ArraySize = entry.layer_count;
    } else {
      dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
      dsv.Texture2D.MipSlice = desc.base_mip;
    }
    device_->CreateDepthStencilView(tex_copy.resource.Get(), &dsv, dsv_heap_.cpu_handle(slot));
    entry.has_dsv = true;
    entry.dsv_slot = slot;
  }

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return texture_views_.allocate(std::move(entry));
}

void D3D12DeviceImpl::destroy_texture_view(TextureViewHandle handle) {
  TextureViewEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!texture_views_.remove(handle, &entry)) return;
  }
  if (entry.has_rtv) rtv_heap_.free(entry.rtv_slot, 1);
  if (entry.has_dsv) dsv_heap_.free(entry.dsv_slot, 1);
}

TextureHandle D3D12DeviceImpl::register_external_texture(ComPtr<ID3D12Resource> res,
                                                         DXGI_FORMAT fmt,
                                                         uint32_t w, uint32_t h) {
  TextureEntry entry{};
  entry.resource = std::move(res);
  entry.resource_format = fmt;
  entry.copy_format = fmt;
  entry.format = from_dxgi_format(fmt);
  entry.type = TextureType::Tex2D;
  entry.usage = TextureUsage::ColorAttachment | TextureUsage::TransferDst | TextureUsage::TransferSrc;
  entry.width = w;
  entry.height = h;
  entry.current_state = D3D12_RESOURCE_STATE_COMMON;
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
  entry.current_layout = D3D12_BARRIER_LAYOUT_COMMON;
#endif
  entry.enhanced_barrier_compatible = enhanced_barriers_enabled_;
  entry.owned = false;
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return textures_.allocate(std::move(entry));
}

TextureViewHandle D3D12DeviceImpl::register_external_texture_view(TextureHandle tex, DXGI_FORMAT fmt) {
  std::string err;
  TextureViewDesc desc{};
  desc.texture = tex;
  desc.type = TextureType::Tex2D;
  desc.format = from_dxgi_format(fmt);
  return create_texture_view(desc, err);
}

void D3D12DeviceImpl::unregister_external_texture_view(TextureViewHandle h) {
  destroy_texture_view(h);
}

void D3D12DeviceImpl::unregister_external_texture(TextureHandle h) {
  destroy_texture(h);
}

// -----------------------------------------------------------------------------
// Samplers
// -----------------------------------------------------------------------------
SamplerHandle D3D12DeviceImpl::create_sampler(const SamplerDesc& desc, std::string& error) {
  (void)error;
  SamplerEntry entry{};
  const bool aniso = desc.max_anisotropy > 1.0f;
  entry.desc.Filter = to_d3d12_filter(desc.min_filter, desc.mag_filter, desc.mip_filter,
                                      desc.compare_enabled, aniso);
  entry.desc.AddressU = to_d3d12_address_mode(desc.address_u);
  entry.desc.AddressV = to_d3d12_address_mode(desc.address_v);
  entry.desc.AddressW = to_d3d12_address_mode(desc.address_w);
  entry.desc.MipLODBias = desc.mip_lod_bias;
  entry.desc.MaxAnisotropy = static_cast<UINT>(std::clamp(desc.max_anisotropy, 1.0f, caps_.max_anisotropy));
  entry.desc.ComparisonFunc = desc.compare_enabled ? to_d3d12_compare_op(desc.compare_op)
                                                   : D3D12_COMPARISON_FUNC_NEVER;
  switch (desc.border_color) {
    case BorderColor::TransparentBlack:
      entry.desc.BorderColor[0] = 0.0f; entry.desc.BorderColor[1] = 0.0f;
      entry.desc.BorderColor[2] = 0.0f; entry.desc.BorderColor[3] = 0.0f;
      break;
    case BorderColor::OpaqueBlack:
      entry.desc.BorderColor[0] = 0.0f; entry.desc.BorderColor[1] = 0.0f;
      entry.desc.BorderColor[2] = 0.0f; entry.desc.BorderColor[3] = 1.0f;
      break;
    case BorderColor::OpaqueWhite:
      entry.desc.BorderColor[0] = 1.0f; entry.desc.BorderColor[1] = 1.0f;
      entry.desc.BorderColor[2] = 1.0f; entry.desc.BorderColor[3] = 1.0f;
      break;
  }
  entry.desc.MinLOD = desc.min_lod;
  entry.desc.MaxLOD = desc.max_lod;

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return samplers_.allocate(std::move(entry));
}

void D3D12DeviceImpl::destroy_sampler(SamplerHandle handle) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  samplers_.remove(handle);
}

// -----------------------------------------------------------------------------
// Descriptor writing helpers
// -----------------------------------------------------------------------------
void D3D12DeviceImpl::write_null_texture_srv(uint32_t heap_slot) {
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(nullptr, &srv, cbv_srv_uav_heap_.cpu_handle(heap_slot));
}

void D3D12DeviceImpl::write_texture_srv(uint32_t heap_slot, TextureViewHandle view_h) {
  TextureViewEntry tv{};
  TextureEntry te{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const TextureViewEntry* tvp = texture_views_.get(view_h);
    if (!tvp) {
      write_null_texture_srv(heap_slot);
      return;
    }
    const TextureEntry* tep = textures_.get(tvp->texture);
    if (!tep || !tep->resource) {
      write_null_texture_srv(heap_slot);
      return;
    }
    tv = *tvp;
    te = *tep;
  }

  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = to_dxgi_srv_format(tv.format);
  srv.Shader4ComponentMapping = to_dxgi_swizzle_mapping(tv.swizzle);

  if (tv.type == TextureType::TexCube) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.TextureCube.MostDetailedMip = tv.base_mip;
    srv.TextureCube.MipLevels = tv.mip_count;
  } else if (tv.type == TextureType::Tex3D) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Texture3D.MostDetailedMip = tv.base_mip;
    srv.Texture3D.MipLevels = tv.mip_count;
  } else if (te.sample_count > 1) {
    if (te.array_layers > 1 || tv.base_layer > 0) {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
      srv.Texture2DMSArray.FirstArraySlice = tv.base_layer;
      srv.Texture2DMSArray.ArraySize = tv.layer_count;
    } else {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    }
  } else if (te.array_layers > 1 || tv.base_layer > 0) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray.MostDetailedMip = tv.base_mip;
    srv.Texture2DArray.MipLevels = tv.mip_count;
    srv.Texture2DArray.FirstArraySlice = tv.base_layer;
    srv.Texture2DArray.ArraySize = tv.layer_count;
  } else {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MostDetailedMip = tv.base_mip;
    srv.Texture2D.MipLevels = tv.mip_count;
  }
  device_->CreateShaderResourceView(te.resource.Get(), &srv, cbv_srv_uav_heap_.cpu_handle(heap_slot));
}

void D3D12DeviceImpl::write_texture_uav(uint32_t heap_slot, TextureViewHandle view_h) {
  TextureViewEntry tv{};
  TextureEntry te{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const TextureViewEntry* tvp = texture_views_.get(view_h);
    const TextureEntry* tep = tvp ? textures_.get(tvp->texture) : nullptr;
    if (!tvp || !tep || !tep->resource) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
      uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      device_->CreateUnorderedAccessView(nullptr, nullptr, &uav, cbv_srv_uav_heap_.cpu_handle(heap_slot));
      return;
    }
    tv = *tvp;
    te = *tep;
  }

  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = to_dxgi_srv_format(tv.format);
  if (tv.type == TextureType::Tex3D) {
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uav.Texture3D.MipSlice = tv.base_mip;
    uav.Texture3D.FirstWSlice = tv.base_layer;
    uav.Texture3D.WSize = tv.layer_count;
  } else if (te.array_layers > 1 || tv.base_layer > 0) {
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uav.Texture2DArray.MipSlice = tv.base_mip;
    uav.Texture2DArray.FirstArraySlice = tv.base_layer;
    uav.Texture2DArray.ArraySize = tv.layer_count;
  } else {
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Texture2D.MipSlice = tv.base_mip;
  }
  device_->CreateUnorderedAccessView(te.resource.Get(), nullptr, &uav,
                                     cbv_srv_uav_heap_.cpu_handle(heap_slot));
}

void D3D12DeviceImpl::write_null_buffer_srv(uint32_t heap_slot) {
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R32_TYPELESS;
  srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Buffer.FirstElement = 0;
  srv.Buffer.NumElements = 1;
  srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  device_->CreateShaderResourceView(nullptr, &srv, cbv_srv_uav_heap_.cpu_handle(heap_slot));
}

void D3D12DeviceImpl::write_null_buffer_uav(uint32_t heap_slot) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R32_TYPELESS;
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.FirstElement = 0;
  uav.Buffer.NumElements = 1;
  uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device_->CreateUnorderedAccessView(nullptr, nullptr, &uav, cbv_srv_uav_heap_.cpu_handle(heap_slot));
}

void D3D12DeviceImpl::write_buffer_srv(uint32_t heap_slot, BufferHandle buf_h,
                                       uint64_t offset, uint64_t range) {
  BufferEntry be{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const BufferEntry* bep = buffers_.get(buf_h);
    if (!bep || !bep->resource) {
      write_null_buffer_srv(heap_slot);
      return;
    }
    be = *bep;
  }
  const uint64_t clamped_off = std::min(offset, be.allocated_size);
  const uint64_t avail = be.allocated_size - clamped_off;
  const uint64_t bytes = (range == UINT64_MAX) ? avail : std::min(range, avail);

  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R32_TYPELESS;
  srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Buffer.FirstElement = clamped_off / 4ull;
  srv.Buffer.NumElements = static_cast<UINT>(std::max<uint64_t>(bytes / 4ull, 1ull));
  srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  device_->CreateShaderResourceView(be.resource.Get(), &srv, cbv_srv_uav_heap_.cpu_handle(heap_slot));
}

void D3D12DeviceImpl::write_buffer_uav(uint32_t heap_slot, BufferHandle buf_h,
                                       uint64_t offset, uint64_t range) {
  BufferEntry be{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const BufferEntry* bep = buffers_.get(buf_h);
    if (!bep || !bep->resource || !bep->allow_uav) {
      write_null_buffer_uav(heap_slot);
      return;
    }
    be = *bep;
  }
  const uint64_t clamped_off = std::min(offset, be.allocated_size);
  const uint64_t avail = be.allocated_size - clamped_off;
  const uint64_t bytes = (range == UINT64_MAX) ? avail : std::min(range, avail);

  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R32_TYPELESS;
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.FirstElement = clamped_off / 4ull;
  uav.Buffer.NumElements = static_cast<UINT>(std::max<uint64_t>(bytes / 4ull, 1ull));
  uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device_->CreateUnorderedAccessView(be.resource.Get(), nullptr, &uav,
                                     cbv_srv_uav_heap_.cpu_handle(heap_slot));
}

void D3D12DeviceImpl::write_cbv(uint32_t heap_slot, BufferHandle buf_h,
                                uint64_t offset, uint64_t range) {
  BufferEntry be{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const BufferEntry* bep = buffers_.get(buf_h);
    if (!bep || !bep->resource) {
      D3D12_CONSTANT_BUFFER_VIEW_DESC null_cbv{};
      device_->CreateConstantBufferView(&null_cbv, cbv_srv_uav_heap_.cpu_handle(heap_slot));
      return;
    }
    be = *bep;
  }
  const uint64_t clamped_off = std::min(offset, be.allocated_size);
  const uint64_t avail = be.allocated_size - clamped_off;
  const uint64_t bytes = (range == UINT64_MAX) ? avail : std::min(range, avail);

  D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
  cbv.BufferLocation = be.gpu_va + clamped_off;
  cbv.SizeInBytes = static_cast<UINT>(std::min(align_up(std::max<uint64_t>(bytes, 1ull), 256ull), avail));
  device_->CreateConstantBufferView(&cbv, cbv_srv_uav_heap_.cpu_handle(heap_slot));
}

void D3D12DeviceImpl::write_accel_struct_srv(uint32_t heap_slot, AccelStructHandle as_h) {
  const uint64_t va = accel_struct_device_address(as_h);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_UNKNOWN;
  srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.RaytracingAccelerationStructure.Location = va;
  device_->CreateShaderResourceView(nullptr, &srv, cbv_srv_uav_heap_.cpu_handle(heap_slot));
}

void D3D12DeviceImpl::write_sampler(uint32_t sampler_slot, SamplerHandle samp_h) {
  D3D12_SAMPLER_DESC sd{};
  bool valid = false;
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (const SamplerEntry* se = samplers_.get(samp_h)) {
      sd = se->desc;
      valid = true;
    }
  }
  if (!valid) {
    sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sd.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sd.MaxLOD = D3D12_FLOAT32_MAX;
  }
  device_->CreateSampler(&sd, sampler_heap_.cpu_handle(sampler_slot));
}

// -----------------------------------------------------------------------------
// Bind Group Layouts, Bind Groups, & Bindless Table
// -----------------------------------------------------------------------------
BindGroupLayoutHandle D3D12DeviceImpl::create_bind_group_layout(
    const BindGroupLayoutDesc& desc, std::string& error) {
  (void)error;
  BindGroupLayoutEntry entry{};
  entry.update_after_bind = desc.update_after_bind_pool;
  entry.slots.reserve(desc.bindings.size());

  // Track occupied SRV register intervals in space+0 to avoid overlaps when
  // two large descriptor arrays share a BindGroupLayout (e.g. BindlessTable).
  uint32_t max_srv_end_space0 = 0;

  for (const BindGroupLayoutBinding& b : desc.bindings) {
    const uint32_t cnt = std::max(b.count, 1u);
    BindGroupLayoutSlotInfo info{};
    info.binding = b.binding;
    info.type = b.type;
    info.count = cnt;
    info.stages = b.stages;
    info.partially_bound = b.partially_bound;
    info.update_after_bind = b.update_after_bind;

    switch (b.type) {
      case DescriptorType::UniformBuffer:
        info.srv_cbv_offset = entry.cbv_srv_uav_count;
        info.srv_cbv_shader_reg = b.binding;
        entry.cbv_srv_uav_count += cnt;
        break;
      case DescriptorType::SampledTexture:
      case DescriptorType::AccelStruct:
        info.srv_cbv_offset = entry.cbv_srv_uav_count;
        if (cnt > 1 && b.binding < max_srv_end_space0) {
          info.srv_cbv_shader_reg = 0;
          info.srv_cbv_space_offset = 16;
        } else {
          info.srv_cbv_shader_reg = b.binding;
          max_srv_end_space0 = std::max(max_srv_end_space0, b.binding + cnt);
        }
        entry.cbv_srv_uav_count += cnt;
        break;
      case DescriptorType::StorageBuffer:
        info.srv_cbv_offset = entry.cbv_srv_uav_count;
        if (cnt > 1 && b.binding < max_srv_end_space0) {
          info.srv_cbv_shader_reg = 0;
          info.srv_cbv_space_offset = 16;
        } else {
          info.srv_cbv_shader_reg = b.binding;
          max_srv_end_space0 = std::max(max_srv_end_space0, b.binding + cnt);
        }
        entry.cbv_srv_uav_count += cnt;
        // Single-element StorageBuffers also get a UAV slot (u<binding>) so
        // compute/graphics shaders can access them read-only (t#) OR read-write (u#).
        if (cnt == 1) {
          info.uav_offset = entry.cbv_srv_uav_count;
          info.uav_shader_reg = b.binding;
          entry.cbv_srv_uav_count += 1;
        }
        break;
      case DescriptorType::StorageTexture:
        info.uav_offset = entry.cbv_srv_uav_count;
        info.uav_shader_reg = b.binding;
        entry.cbv_srv_uav_count += cnt;
        break;
      case DescriptorType::Sampler:
        info.sampler_offset = entry.sampler_count;
        info.sampler_shader_reg = b.binding;
        entry.sampler_count += cnt;
        break;
      case DescriptorType::CombinedTextureSampler:
        info.srv_cbv_offset = entry.cbv_srv_uav_count;
        info.srv_cbv_shader_reg = b.binding;
        max_srv_end_space0 = std::max(max_srv_end_space0, b.binding + cnt);
        entry.cbv_srv_uav_count += cnt;
        info.sampler_offset = entry.sampler_count;
        info.sampler_shader_reg = b.binding;
        entry.sampler_count += cnt;
        break;
    }
    entry.slots.push_back(info);
  }

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return bind_group_layouts_.allocate(std::move(entry));
}

void D3D12DeviceImpl::destroy_bind_group_layout(BindGroupLayoutHandle handle) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  bind_group_layouts_.remove(handle);
}

BindGroupHandle D3D12DeviceImpl::create_bind_group(const BindGroupDesc& desc, std::string& error) {
  BindGroupLayoutEntry layout{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const BindGroupLayoutEntry* lp = bind_group_layouts_.get(desc.layout);
    if (!lp) {
      error = "create_bind_group: invalid BindGroupLayoutHandle";
      return {};
    }
    layout = *lp;
  }

  BindGroupEntry entry{};
  entry.layout = desc.layout;
  entry.cbv_srv_uav_count = layout.cbv_srv_uav_count;
  entry.sampler_count = layout.sampler_count;

  if (entry.cbv_srv_uav_count > 0) {
    entry.cbv_srv_uav_base = cbv_srv_uav_heap_.allocate(entry.cbv_srv_uav_count);
    if (entry.cbv_srv_uav_base == UINT32_MAX) {
      error = "create_bind_group: shader-visible CBV/SRV/UAV heap exhausted";
      return {};
    }
    // Pre-populate null descriptors for partially bound / uninitialized slots.
    for (const BindGroupLayoutSlotInfo& s : layout.slots) {
      if (s.srv_cbv_offset != UINT32_MAX) {
        for (uint32_t i = 0; i < s.count; ++i) {
          const uint32_t slot = entry.cbv_srv_uav_base + s.srv_cbv_offset + i;
          if (s.type == DescriptorType::StorageBuffer) {
            write_null_buffer_srv(slot);
          } else if (s.type == DescriptorType::UniformBuffer) {
            write_cbv(slot, BufferHandle{}, 0, 0);
          } else {
            write_null_texture_srv(slot);
          }
        }
      }
      if (s.uav_offset != UINT32_MAX) {
        for (uint32_t i = 0; i < s.count; ++i) {
          write_null_buffer_uav(entry.cbv_srv_uav_base + s.uav_offset + i);
        }
      }
    }
  }

  if (entry.sampler_count > 0) {
    entry.sampler_base = sampler_heap_.allocate(entry.sampler_count);
    if (entry.sampler_base == UINT32_MAX) {
      if (entry.cbv_srv_uav_base != UINT32_MAX) {
        cbv_srv_uav_heap_.free(entry.cbv_srv_uav_base, entry.cbv_srv_uav_count);
      }
      error = "create_bind_group: shader-visible Sampler heap exhausted";
      return {};
    }
    for (uint32_t i = 0; i < entry.sampler_count; ++i) {
      write_sampler(entry.sampler_base + i, SamplerHandle{});
    }
  }

  for (const shift::rhi::BindGroupEntry& e : desc.entries) {
    const BindGroupLayoutSlotInfo* found = nullptr;
    for (const BindGroupLayoutSlotInfo& s : layout.slots) {
      if (s.binding == e.binding) {
        found = &s;
        break;
      }
    }
    if (!found || e.array_element >= found->count) continue;

    switch (e.type) {
      case DescriptorType::UniformBuffer:
        if (found->srv_cbv_offset != UINT32_MAX) {
          write_cbv(entry.cbv_srv_uav_base + found->srv_cbv_offset + e.array_element,
                    e.buffer, e.buffer_offset, e.buffer_range);
        }
        break;
      case DescriptorType::StorageBuffer:
        if (found->srv_cbv_offset != UINT32_MAX) {
          write_buffer_srv(entry.cbv_srv_uav_base + found->srv_cbv_offset + e.array_element,
                           e.buffer, e.buffer_offset, e.buffer_range);
        }
        if (found->uav_offset != UINT32_MAX) {
          write_buffer_uav(entry.cbv_srv_uav_base + found->uav_offset + e.array_element,
                           e.buffer, e.buffer_offset, e.buffer_range);
        }
        break;
      case DescriptorType::SampledTexture:
        if (found->srv_cbv_offset != UINT32_MAX) {
          write_texture_srv(entry.cbv_srv_uav_base + found->srv_cbv_offset + e.array_element,
                            e.texture_view);
        }
        break;
      case DescriptorType::StorageTexture:
        if (found->uav_offset != UINT32_MAX) {
          write_texture_uav(entry.cbv_srv_uav_base + found->uav_offset + e.array_element,
                            e.texture_view);
        }
        break;
      case DescriptorType::Sampler:
        if (found->sampler_offset != UINT32_MAX) {
          write_sampler(entry.sampler_base + found->sampler_offset + e.array_element, e.sampler);
        }
        break;
      case DescriptorType::CombinedTextureSampler:
        if (found->srv_cbv_offset != UINT32_MAX) {
          write_texture_srv(entry.cbv_srv_uav_base + found->srv_cbv_offset + e.array_element,
                            e.texture_view);
        }
        if (found->sampler_offset != UINT32_MAX) {
          write_sampler(entry.sampler_base + found->sampler_offset + e.array_element, e.sampler);
        }
        break;
      case DescriptorType::AccelStruct:
        if (found->srv_cbv_offset != UINT32_MAX) {
          write_accel_struct_srv(entry.cbv_srv_uav_base + found->srv_cbv_offset + e.array_element,
                                 e.accel_struct);
        }
        break;
    }
  }

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return bind_groups_.allocate(std::move(entry));
}

void D3D12DeviceImpl::destroy_bind_group(BindGroupHandle handle) {
  BindGroupEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!bind_groups_.remove(handle, &entry)) return;
  }
  if (entry.cbv_srv_uav_base != UINT32_MAX && entry.cbv_srv_uav_count > 0) {
    cbv_srv_uav_heap_.free(entry.cbv_srv_uav_base, entry.cbv_srv_uav_count);
  }
  if (entry.sampler_base != UINT32_MAX && entry.sampler_count > 0) {
    sampler_heap_.free(entry.sampler_base, entry.sampler_count);
  }
}

std::unique_ptr<BindlessTable> D3D12DeviceImpl::create_bindless_table(
    const BindlessTableDesc& desc, std::string& error) {
  if (!caps_.bindless) {
    error = "bindless descriptor tables are not supported on this device";
    return nullptr;
  }
  const uint32_t max_tex = std::min(desc.max_textures, caps_.max_bindless_textures);
  if (max_tex == 0) {
    error = "create_bindless_table: max_textures must be > 0";
    return nullptr;
  }

  std::vector<BindGroupLayoutBinding> bindings;
  bindings.push_back({0, DescriptorType::Sampler, 1, desc.stages, false, false});
  bindings.push_back({1, DescriptorType::SampledTexture, max_tex, desc.stages, true, true});
  if (desc.max_storage_buffers > 0) {
    bindings.push_back({2, DescriptorType::StorageBuffer, desc.max_storage_buffers,
                        desc.stages, true, false});
  }

  BindGroupLayoutDesc ldesc{};
  ldesc.bindings = bindings;
  ldesc.update_after_bind_pool = true;
  BindGroupLayoutHandle layout_h = create_bind_group_layout(ldesc, error);
  if (!layout_h) return nullptr;

  BindGroupDesc gdesc{};
  gdesc.layout = layout_h;
  BindGroupHandle group_h = create_bind_group(gdesc, error);
  if (!group_h) {
    destroy_bind_group_layout(layout_h);
    return nullptr;
  }

  auto table = std::make_unique<D3D12BindlessTableImpl>(
      *this, layout_h, group_h, max_tex, desc.max_storage_buffers);
  if (desc.default_sampler) table->set_sampler(desc.default_sampler);
  if (desc.fallback_texture) {
    for (uint32_t i = 0; i < max_tex; ++i) {
      table->set_texture(i, desc.fallback_texture);
    }
  }
  if (desc.fallback_buffer && desc.max_storage_buffers > 0) {
    for (uint32_t i = 0; i < desc.max_storage_buffers; ++i) {
      table->set_storage_buffer(i, desc.fallback_buffer, 0, UINT64_MAX);
    }
  }
  return table;
}

// -----------------------------------------------------------------------------
// Pipeline Layout (Root Signature) & Pipelines (Graphics + Compute)
// -----------------------------------------------------------------------------
PipelineLayoutHandle D3D12DeviceImpl::create_pipeline_layout(
    const PipelineLayoutDesc& desc, std::string& error) {
  std::vector<D3D12_ROOT_PARAMETER1> params;
  // Keep range arrays alive until serialization completes.
  std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> range_storage;
  range_storage.reserve(desc.bind_group_layouts.size() * 2 + 1);

  PipelineLayoutEntry entry{};
  entry.set_mappings.resize(desc.bind_group_layouts.size());

  // Root Parameter 0 (when push constants present): <= 32 DWORDs (128 B) at b0, space15.
  uint32_t max_pc_bytes = 0;
  ShaderStage pc_stages = ShaderStage::None;
  for (const PushConstantRange& r : desc.push_constants) {
    max_pc_bytes = std::max(max_pc_bytes, r.offset + r.size);
    pc_stages |= r.stages;
  }
  if (max_pc_bytes > 0) {
    const uint32_t dwords = std::min((max_pc_bytes + 3u) / 4u, 32u);
    D3D12_ROOT_PARAMETER1 rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.ShaderRegister = 0;
    rp.Constants.RegisterSpace = 15;
    rp.Constants.Num32BitValues = dwords;
    rp.ShaderVisibility = to_d3d12_visibility(pc_stages);
    entry.push_const_root_param = static_cast<uint32_t>(params.size());
    entry.push_const_dwords = dwords;
    params.push_back(rp);
  }

  for (size_t set_idx = 0; set_idx < desc.bind_group_layouts.size(); ++set_idx) {
    BindGroupLayoutEntry bgl{};
    {
      std::lock_guard<std::mutex> lock(resource_mutex_);
      const BindGroupLayoutEntry* lp = bind_group_layouts_.get(desc.bind_group_layouts[set_idx]);
      if (!lp) {
        error = "create_pipeline_layout: invalid BindGroupLayoutHandle";
        return {};
      }
      bgl = *lp;
    }

    std::vector<D3D12_DESCRIPTOR_RANGE1> cbv_srv_uav_ranges;
    std::vector<D3D12_DESCRIPTOR_RANGE1> sampler_ranges;
    ShaderStage cbv_stages = ShaderStage::None;
    ShaderStage samp_stages = ShaderStage::None;

    const D3D12_DESCRIPTOR_RANGE_FLAGS volatile_flags =
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
        D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    const D3D12_DESCRIPTOR_RANGE_FLAGS samp_flags =
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

    for (const BindGroupLayoutSlotInfo& s : bgl.slots) {
      if (s.srv_cbv_offset != UINT32_MAX) {
        D3D12_DESCRIPTOR_RANGE1 rng{};
        rng.RangeType = (s.type == DescriptorType::UniformBuffer)
                            ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                            : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rng.NumDescriptors = s.count;
        rng.BaseShaderRegister = s.srv_cbv_shader_reg;
        rng.RegisterSpace = static_cast<UINT>(set_idx) + s.srv_cbv_space_offset;
        rng.Flags = volatile_flags;
        rng.OffsetInDescriptorsFromTableStart = s.srv_cbv_offset;
        cbv_srv_uav_ranges.push_back(rng);
        cbv_stages |= s.stages;
      }
      if (s.uav_offset != UINT32_MAX) {
        D3D12_DESCRIPTOR_RANGE1 rng{};
        rng.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        rng.NumDescriptors = s.count;
        rng.BaseShaderRegister = s.uav_shader_reg;
        rng.RegisterSpace = static_cast<UINT>(set_idx);
        rng.Flags = volatile_flags;
        rng.OffsetInDescriptorsFromTableStart = s.uav_offset;
        cbv_srv_uav_ranges.push_back(rng);
        cbv_stages |= s.stages;
      }
      if (s.sampler_offset != UINT32_MAX) {
        D3D12_DESCRIPTOR_RANGE1 rng{};
        rng.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        rng.NumDescriptors = s.count;
        rng.BaseShaderRegister = s.sampler_shader_reg;
        rng.RegisterSpace = static_cast<UINT>(set_idx);
        rng.Flags = samp_flags;
        rng.OffsetInDescriptorsFromTableStart = s.sampler_offset;
        sampler_ranges.push_back(rng);
        samp_stages |= s.stages;
      }
    }

    if (!cbv_srv_uav_ranges.empty()) {
      range_storage.push_back(std::move(cbv_srv_uav_ranges));
      D3D12_ROOT_PARAMETER1 rp{};
      rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      rp.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(range_storage.back().size());
      rp.DescriptorTable.pDescriptorRanges = range_storage.back().data();
      rp.ShaderVisibility = to_d3d12_visibility(cbv_stages);
      entry.set_mappings[set_idx].cbv_srv_uav_root_param = static_cast<uint32_t>(params.size());
      params.push_back(rp);
    }
    if (!sampler_ranges.empty()) {
      range_storage.push_back(std::move(sampler_ranges));
      D3D12_ROOT_PARAMETER1 rp{};
      rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      rp.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(range_storage.back().size());
      rp.DescriptorTable.pDescriptorRanges = range_storage.back().data();
      rp.ShaderVisibility = to_d3d12_visibility(samp_stages);
      entry.set_mappings[set_idx].sampler_root_param = static_cast<uint32_t>(params.size());
      params.push_back(rp);
    }
  }

  D3D12_ROOT_SIGNATURE_FLAGS rs_flags =
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  if (sm66_dynamic_resources_) {
    rs_flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
  }

  D3D12_VERSIONED_ROOT_SIGNATURE_DESC vdesc{};
  vdesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  vdesc.Desc_1_1.NumParameters = static_cast<UINT>(params.size());
  vdesc.Desc_1_1.pParameters = params.empty() ? nullptr : params.data();
  vdesc.Desc_1_1.Flags = rs_flags;

  ComPtr<ID3DBlob> serialized;
  ComPtr<ID3DBlob> err_blob;
  HRESULT hr = D3D12SerializeVersionedRootSignature(&vdesc, &serialized, &err_blob);
  if (FAILED(hr)) {
    error = hr_err("D3D12SerializeVersionedRootSignature", hr);
    if (err_blob) {
      error += ": ";
      error.append(static_cast<const char*>(err_blob->GetBufferPointer()),
                   err_blob->GetBufferSize());
    }
    return {};
  }

  hr = device_->CreateRootSignature(
      0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
      IID_PPV_ARGS(&entry.root_signature));
  if (FAILED(hr)) {
    error = hr_err("CreateRootSignature", hr);
    return {};
  }

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return pipeline_layouts_.allocate(std::move(entry));
}

void D3D12DeviceImpl::destroy_pipeline_layout(PipelineLayoutHandle handle) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  pipeline_layouts_.remove(handle);
}

bool D3D12DeviceImpl::extract_or_compile_shader(const ShaderModuleDesc& mod,
                                                std::vector<uint8_t>& out_bytecode,
                                                std::string& error) const {
  out_bytecode.clear();
  if (mod.spirv.empty()) return true;

  const auto* bytes = reinterpret_cast<const uint8_t*>(mod.spirv.data());
  const size_t byte_len = mod.spirv.size_bytes();

  // Direct DXIL / DXBC container ('DXBC' magic 0x43425844).
  if (byte_len >= 32 && mod.spirv[0] == 0x43425844u) {
    uint32_t container_size = 0;
    std::memcpy(&container_size, bytes + 24, sizeof(uint32_t));
    const size_t actual = (container_size >= 32 && container_size <= byte_len)
                              ? static_cast<size_t>(container_size)
                              : byte_len;
    out_bytecode.assign(bytes, bytes + actual);
    return true;
  }

  // Fallback: compile HLSL source text using D3DCompile (SM 5.1 supports register spaces).
  const char* profile = "vs_5_1";
  if (mod.stage == ShaderStage::Fragment) profile = "ps_5_1";
  else if (mod.stage == ShaderStage::Compute) profile = "cs_5_1";

  size_t text_len = byte_len;
  while (text_len > 0 && bytes[text_len - 1] == 0) --text_len;

  ComPtr<ID3DBlob> code;
  ComPtr<ID3DBlob> errs;
  const HRESULT hr = D3DCompile(
      bytes, text_len, "rhi_d3d12_inline.hlsl", nullptr, nullptr,
      mod.entry_point ? mod.entry_point : "main", profile,
      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs);
  if (FAILED(hr)) {
    error = hr_err("D3DCompile", hr);
    if (errs) {
      error += ": ";
      error.append(static_cast<const char*>(errs->GetBufferPointer()), errs->GetBufferSize());
    }
    return false;
  }
  const auto* p = static_cast<const uint8_t*>(code->GetBufferPointer());
  out_bytecode.assign(p, p + code->GetBufferSize());
  return true;
}

PipelineHandle D3D12DeviceImpl::create_graphics_pipeline(
    const GraphicsPipelineDesc& desc, std::string& error) {
  ID3D12RootSignature* root_sig = nullptr;
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const PipelineLayoutEntry* pl = pipeline_layouts_.get(desc.layout);
    if (!pl || !pl->root_signature) {
      error = "create_graphics_pipeline: invalid PipelineLayoutHandle";
      return {};
    }
    root_sig = pl->root_signature.Get();
  }

  PipelineEntry entry{};
  entry.root_signature = root_sig;
  entry.layout_handle = desc.layout;
  entry.is_compute = false;
  entry.topology = to_d3d12_topology(desc.raster.topology);
  entry.dynamic_depth_bias = desc.raster.dynamic_depth_bias;

  if (!extract_or_compile_shader(desc.vertex_shader, entry.vs_bytecode, error) ||
      entry.vs_bytecode.empty()) {
    if (error.empty()) error = "create_graphics_pipeline: empty vertex shader bytecode";
    return {};
  }
  if (!desc.fragment_shader.spirv.empty()) {
    if (!extract_or_compile_shader(desc.fragment_shader, entry.ps_bytecode, error)) {
      return {};
    }
  }

  for (const VertexBindingDesc& vb : desc.vertex_bindings) {
    if (vb.binding < entry.vertex_strides.size()) {
      entry.vertex_strides[vb.binding] = vb.stride;
    }
  }

  // Build D3D12 Input Layout ("TEXCOORD" + location matches SPIRV-Cross HLSL conventions).
  entry.semantic_names.reserve(desc.vertex_attributes.size());
  entry.input_elements.reserve(desc.vertex_attributes.size());
  for (const VertexAttributeDesc& attr : desc.vertex_attributes) {
    bool per_instance = false;
    for (const VertexBindingDesc& vb : desc.vertex_bindings) {
      if (vb.binding == attr.binding) {
        per_instance = vb.per_instance;
        break;
      }
    }
    entry.semantic_names.emplace_back("TEXCOORD");
    D3D12_INPUT_ELEMENT_DESC el{};
    el.SemanticName = entry.semantic_names.back().c_str();
    el.SemanticIndex = attr.location;
    el.Format = to_dxgi_format(attr.format);
    el.InputSlot = attr.binding;
    el.AlignedByteOffset = attr.offset;
    el.InputSlotClass = per_instance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                     : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    el.InstanceDataStepRate = per_instance ? 1u : 0u;
    entry.input_elements.push_back(el);
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = root_sig;
  pd.VS = {entry.vs_bytecode.data(), entry.vs_bytecode.size()};
  if (!entry.ps_bytecode.empty()) {
    pd.PS = {entry.ps_bytecode.data(), entry.ps_bytecode.size()};
  }

  pd.BlendState.AlphaToCoverageEnable = desc.raster.alpha_to_coverage ? TRUE : FALSE;
  pd.BlendState.IndependentBlendEnable = (desc.color_blends.size() > 1) ? TRUE : FALSE;
  for (size_t i = 0; i < 8; ++i) {
    const BlendState b =
        (i < desc.color_blends.size()) ? desc.color_blends[i] : BlendState::opaque();
    auto& rt = pd.BlendState.RenderTarget[i];
    rt.BlendEnable = b.enabled ? TRUE : FALSE;
    rt.SrcBlend = to_d3d12_blend_factor(b.src_color);
    rt.DestBlend = to_d3d12_blend_factor(b.dst_color);
    rt.BlendOp = to_d3d12_blend_op(b.color_op);
    rt.SrcBlendAlpha = to_d3d12_blend_factor(b.src_alpha);
    rt.DestBlendAlpha = to_d3d12_blend_factor(b.dst_alpha);
    rt.BlendOpAlpha = to_d3d12_blend_op(b.alpha_op);
    rt.RenderTargetWriteMask = to_d3d12_write_mask(b.write_mask);
  }
  pd.SampleMask = UINT_MAX;

  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = to_d3d12_cull_mode(desc.raster.cull_mode);
  pd.RasterizerState.FrontCounterClockwise =
      (desc.raster.front_face == FrontFace::CounterClockwise) ? TRUE : FALSE;
  pd.RasterizerState.DepthClipEnable = desc.raster.depth_clamp ? FALSE : TRUE;
  pd.RasterizerState.MultisampleEnable = (desc.raster.sample_count > 1) ? TRUE : FALSE;

  pd.DepthStencilState.DepthEnable = desc.depth.test_enabled ? TRUE : FALSE;
  pd.DepthStencilState.DepthWriteMask =
      desc.depth.write_enabled ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  pd.DepthStencilState.DepthFunc = to_d3d12_compare_op(desc.depth.compare_op);

  pd.InputLayout.pInputElementDescs =
      entry.input_elements.empty() ? nullptr : entry.input_elements.data();
  pd.InputLayout.NumElements = static_cast<UINT>(entry.input_elements.size());
  pd.PrimitiveTopologyType = to_d3d12_topology_type(desc.raster.topology);

  const uint32_t num_rt = static_cast<uint32_t>(std::min<size_t>(desc.color_formats.size(), 8u));
  pd.NumRenderTargets = num_rt;
  for (uint32_t i = 0; i < num_rt; ++i) {
    pd.RTVFormats[i] = to_dxgi_format(desc.color_formats[i]);
  }
  pd.DSVFormat = (desc.depth_format != Format::Undefined)
                     ? to_dxgi_dsv_format(desc.depth_format)
                     : DXGI_FORMAT_UNKNOWN;
  pd.SampleDesc.Count = std::max(desc.raster.sample_count, 1u);
  pd.SampleDesc.Quality = 0;

  // Compute deterministic PSO key for ID3D12PipelineLibrary caching.
  uint64_t hash = fnv1a_hash(entry.vs_bytecode.data(), entry.vs_bytecode.size());
  if (!entry.ps_bytecode.empty()) {
    hash = fnv1a_hash(entry.ps_bytecode.data(), entry.ps_bytecode.size(), hash);
  }
  hash = fnv1a_hash(&pd.BlendState, sizeof(pd.BlendState), hash);
  hash = fnv1a_hash(&pd.RasterizerState, sizeof(pd.RasterizerState), hash);
  hash = fnv1a_hash(&pd.DepthStencilState, sizeof(pd.DepthStencilState), hash);
  hash = fnv1a_hash(&pd.NumRenderTargets, sizeof(pd.NumRenderTargets), hash);
  hash = fnv1a_hash(pd.RTVFormats, sizeof(pd.RTVFormats), hash);
  hash = fnv1a_hash(&pd.DSVFormat, sizeof(pd.DSVFormat), hash);
  hash = fnv1a_hash(&pd.SampleDesc, sizeof(pd.SampleDesc), hash);
  const std::wstring cache_name = pso_cache_name(L'G', hash);

  bool loaded_from_lib = false;
  if (pipeline_library_) {
    std::lock_guard<std::mutex> lib_lock(pipeline_library_mutex_);
    if (SUCCEEDED(pipeline_library_->LoadGraphicsPipeline(
            cache_name.c_str(), &pd, IID_PPV_ARGS(&entry.pso)))) {
      loaded_from_lib = true;
    }
  }
  if (!loaded_from_lib) {
    const HRESULT hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&entry.pso));
    if (FAILED(hr)) {
      error = hr_err("CreateGraphicsPipelineState", hr);
      return {};
    }
    if (pipeline_library_) {
      std::lock_guard<std::mutex> lib_lock(pipeline_library_mutex_);
      pipeline_library_->StorePipeline(cache_name.c_str(), entry.pso.Get());
    }
  }

  entry.base_graphics_desc = pd;

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return pipelines_.allocate(std::move(entry));
}

PipelineHandle D3D12DeviceImpl::create_compute_pipeline(
    const ComputePipelineDesc& desc, std::string& error) {
  ID3D12RootSignature* root_sig = nullptr;
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const PipelineLayoutEntry* pl = pipeline_layouts_.get(desc.layout);
    if (!pl || !pl->root_signature) {
      error = "create_compute_pipeline: invalid PipelineLayoutHandle";
      return {};
    }
    root_sig = pl->root_signature.Get();
  }

  std::vector<uint8_t> cs_bytecode;
  if (!extract_or_compile_shader(desc.compute_shader, cs_bytecode, error) || cs_bytecode.empty()) {
    if (error.empty()) error = "create_compute_pipeline: empty compute shader bytecode";
    return {};
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = root_sig;
  pd.CS = {cs_bytecode.data(), cs_bytecode.size()};

  const uint64_t hash = fnv1a_hash(cs_bytecode.data(), cs_bytecode.size());
  const std::wstring cache_name = pso_cache_name(L'C', hash);

  PipelineEntry entry{};
  entry.root_signature = root_sig;
  entry.layout_handle = desc.layout;
  entry.is_compute = true;

  bool loaded_from_lib = false;
  if (pipeline_library_) {
    std::lock_guard<std::mutex> lib_lock(pipeline_library_mutex_);
    if (SUCCEEDED(pipeline_library_->LoadComputePipeline(
            cache_name.c_str(), &pd, IID_PPV_ARGS(&entry.pso)))) {
      loaded_from_lib = true;
    }
  }
  if (!loaded_from_lib) {
    const HRESULT hr = device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&entry.pso));
    if (FAILED(hr)) {
      error = hr_err("CreateComputePipelineState", hr);
      return {};
    }
    if (pipeline_library_) {
      std::lock_guard<std::mutex> lib_lock(pipeline_library_mutex_);
      pipeline_library_->StorePipeline(cache_name.c_str(), entry.pso.Get());
    }
  }

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return pipelines_.allocate(std::move(entry));
}

void D3D12DeviceImpl::destroy_pipeline(PipelineHandle handle) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  pipelines_.remove(handle);
}

ID3D12PipelineState* D3D12DeviceImpl::get_or_create_bias_pso(
    PipelineEntry& pe, float constant_factor, float clamp, float slope_factor) {
  if (!pe.dynamic_depth_bias || pe.is_compute) return pe.pso.Get();
  const int32_t c_q = static_cast<int32_t>(std::lround(constant_factor));
  const int32_t s_q = static_cast<int32_t>(std::lround(slope_factor * 100.0f));
  if (c_q == 0 && s_q == 0) return pe.pso.Get();

  const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(c_q)) << 32) |
                       static_cast<uint64_t>(static_cast<uint32_t>(s_q));
  auto it = pe.bias_variants.find(key);
  if (it != pe.bias_variants.end()) return it->second.Get();

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = pe.base_graphics_desc;
  pd.VS = {pe.vs_bytecode.data(), pe.vs_bytecode.size()};
  if (!pe.ps_bytecode.empty()) pd.PS = {pe.ps_bytecode.data(), pe.ps_bytecode.size()};
  pd.InputLayout.pInputElementDescs =
      pe.input_elements.empty() ? nullptr : pe.input_elements.data();
  pd.InputLayout.NumElements = static_cast<UINT>(pe.input_elements.size());
  pd.RasterizerState.DepthBias = c_q;
  pd.RasterizerState.DepthBiasClamp = clamp;
  pd.RasterizerState.SlopeScaledDepthBias = slope_factor;

  ComPtr<ID3D12PipelineState> variant;
  if (FAILED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&variant)))) {
    return pe.pso.Get();
  }
  ID3D12PipelineState* raw = variant.Get();
  pe.bias_variants.emplace(key, std::move(variant));
  return raw;
}

ID3D12CommandSignature* D3D12DeviceImpl::get_or_create_indirect_signature(
    bool indexed, uint32_t stride) {
  const uint64_t key = (static_cast<uint64_t>(indexed ? 1u : 0u) << 32) | stride;
  std::lock_guard<std::mutex> lock(sig_mutex_);
  auto it = indirect_sigs_.find(key);
  if (it != indirect_sigs_.end()) return it->second.Get();

  D3D12_INDIRECT_ARGUMENT_DESC arg{};
  arg.Type = indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED
                     : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

  D3D12_COMMAND_SIGNATURE_DESC csd{};
  csd.ByteStride = stride;
  csd.NumArgumentDescs = 1;
  csd.pArgumentDescs = &arg;

  ComPtr<ID3D12CommandSignature> sig;
  if (FAILED(device_->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&sig)))) {
    return nullptr;
  }
  ID3D12CommandSignature* raw = sig.Get();
  indirect_sigs_.emplace(key, std::move(sig));
  return raw;
}

// -----------------------------------------------------------------------------
// Query Pools (GPU Timestamps & DXR BLAS Compacted Size)
// -----------------------------------------------------------------------------
QueryPoolHandle D3D12DeviceImpl::create_query_pool(const QueryPoolDesc& desc, std::string& error) {
  if (desc.count == 0) {
    error = "create_query_pool: count must be > 0";
    return {};
  }

  QueryPoolEntry entry{};
  entry.type = desc.type;
  entry.count = desc.count;
  const uint64_t rb_bytes = align_up(static_cast<uint64_t>(desc.count) * sizeof(uint64_t), 256ull);

  D3D12_HEAP_PROPERTIES rb_hp{};
  rb_hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rb_rd{};
  rb_rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rb_rd.Width = rb_bytes;
  rb_rd.Height = 1;
  rb_rd.DepthOrArraySize = 1;
  rb_rd.MipLevels = 1;
  rb_rd.SampleDesc.Count = 1;
  rb_rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  HRESULT hr = device_->CreateCommittedResource(
      &rb_hp, D3D12_HEAP_FLAG_NONE, &rb_rd, D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr, IID_PPV_ARGS(&entry.readback));
  if (FAILED(hr)) {
    error = hr_err("CreateCommittedResource(query_readback)", hr);
    return {};
  }

  if (desc.type == QueryType::Timestamp) {
    D3D12_QUERY_HEAP_DESC qhd{};
    qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhd.Count = desc.count;
    hr = device_->CreateQueryHeap(&qhd, IID_PPV_ARGS(&entry.heap));
    if (FAILED(hr)) {
      error = hr_err("CreateQueryHeap(TIMESTAMP)", hr);
      return {};
    }
  } else {
    // AccelStructCompactedSize uses a UAV buffer written by EmitRaytracingAccelerationStructurePostbuildInfo.
    D3D12_HEAP_PROPERTIES gpu_hp{};
    gpu_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC gpu_rd = rb_rd;
    gpu_rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = device_->CreateCommittedResource(
        &gpu_hp, D3D12_HEAP_FLAG_NONE, &gpu_rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr, IID_PPV_ARGS(&entry.gpu_buffer));
    if (FAILED(hr)) {
      error = hr_err("CreateCommittedResource(as_postbuild)", hr);
      return {};
    }
  }

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return query_pools_.allocate(std::move(entry));
}

void D3D12DeviceImpl::destroy_query_pool(QueryPoolHandle handle) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  query_pools_.remove(handle);
}

bool D3D12DeviceImpl::get_query_pool_results(QueryPoolHandle pool, uint32_t first,
                                             uint32_t count, std::span<uint64_t> out_values,
                                             bool wait) {
  if (count == 0 || out_values.size() < count) return false;
  if (wait) wait_idle();

  ComPtr<ID3D12Resource> rb;
  uint32_t total_count = 0;
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const QueryPoolEntry* qe = query_pools_.get(pool);
    if (!qe || !qe->readback || first + count > qe->count) return false;
    rb = qe->readback;
    total_count = qe->count;
  }

  const D3D12_RANGE read_range{
      static_cast<SIZE_T>(first) * sizeof(uint64_t),
      static_cast<SIZE_T>(first + count) * sizeof(uint64_t),
  };
  void* mapped = nullptr;
  if (FAILED(rb->Map(0, &read_range, &mapped)) || !mapped) return false;
  const auto* src = static_cast<const uint64_t*>(mapped);
  std::memcpy(out_values.data(), src + first, count * sizeof(uint64_t));
  const D3D12_RANGE empty_write{0, 0};
  rb->Unmap(0, &empty_write);
  (void)total_count;
  return true;
}

}  // namespace d3d12

// -----------------------------------------------------------------------------
// Factory entry points exposed to rhi_factory.cpp
// -----------------------------------------------------------------------------
void enumerate_d3d12_adapters(std::vector<AdapterInfo>& out) {
  using namespace d3d12;
  ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return;

  std::vector<uint64_t> seen_luids;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter4> adapter;
    if (factory->EnumAdapterByGpuPreference(
            i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    DXGI_ADAPTER_DESC3 ad{};
    if (FAILED(adapter->GetDesc3(&ad))) continue;
    const uint64_t luid_key =
        (static_cast<uint64_t>(static_cast<uint32_t>(ad.AdapterLuid.HighPart)) << 32) |
        static_cast<uint64_t>(ad.AdapterLuid.LowPart);
    if (std::find(seen_luids.begin(), seen_luids.end(), luid_key) != seen_luids.end()) {
      continue;
    }
    seen_luids.push_back(luid_key);

    ComPtr<ID3D12Device5> probe_dev;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&probe_dev)))) {
      continue;
    }

    AdapterInfo info{};
    info.backend = BackendKind::D3D12;
    info.name = wide_to_utf8(ad.Description);
    info.vendor_id = ad.VendorId;
    info.device_id = ad.DeviceId;
    info.device_local_bytes = static_cast<uint64_t>(ad.DedicatedVideoMemory);
    info.is_software = (ad.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0;
    info.is_discrete = !info.is_software && ad.DedicatedVideoMemory >= (256ull << 20);
    info.is_integrated = !info.is_software && !info.is_discrete;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
    if (SUCCEEDED(probe_dev->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5)))) {
      info.supports_ray_query = (opt5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1);
    }
    out.push_back(std::move(info));
  }
}

std::unique_ptr<Device> create_d3d12_device(const DeviceDesc& desc, std::string& error) {
  auto dev = std::make_unique<d3d12::D3D12DeviceImpl>();
  if (!dev->init(desc, error)) return nullptr;
  return dev;
}

}  // namespace shift::rhi
