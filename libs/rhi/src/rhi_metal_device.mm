#include "rhi_metal_internal.h"

#import <TargetConditionals.h>
#ifdef SHIFT_RHI_HAS_APPKIT
#import <AppKit/AppKit.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace shift::rhi {
namespace mtl {

namespace {

std::string ns_err_str(const char* what, NSError* err) {
  if (!err) return std::string(what) + " failed";
  const char* desc = [[err localizedDescription] UTF8String];
  return std::string(what) + " failed: " + (desc ? desc : "unknown Metal error");
}

uint64_t fnv1a_64_str(const char* s, uint64_t seed = 14695981039346656037ull) noexcept {
  uint64_t h = seed;
  if (!s) return h;
  while (*s) {
    h ^= static_cast<uint64_t>(static_cast<unsigned char>(*s++));
    h *= 1099511628211ull;
  }
  return h;
}

bool parse_local_size_hint(const char* src, size_t len, MTLSize& out_tg) noexcept {
  if (!src || len == 0) return false;
  const std::string_view sv(src, len);
  const size_t pos = sv.find("shift_local_size(");
  if (pos == std::string_view::npos) return false;
  const size_t start = pos + 17;
  const size_t end = sv.find(')', start);
  if (end == std::string_view::npos) return false;
  unsigned x = 1, y = 1, z = 1;
  const std::string sub(sv.substr(start, end - start));
  if (std::sscanf(sub.c_str(), "%u , %u , %u", &x, &y, &z) >= 1) {
    out_tg = MTLSizeMake(std::max(1u, x), std::max(1u, y), std::max(1u, z));
    return true;
  }
  return false;
}

const char* kInternalBlitMsl = R"(
#include <metal_stdlib>
using namespace metal;

struct BlitParams {
  float4 uv_rect; // (u0, v0, u1, v1)
};

struct BlitVsOut {
  float4 pos [[position]];
  float2 uv;
};

vertex BlitVsOut shift_blit_vs(uint vid [[vertex_id]],
                               constant BlitParams& p [[buffer(0)]]) {
  float2 corner = float2((vid == 1) ? 2.0f : 0.0f, (vid == 2) ? 2.0f : 0.0f);
  BlitVsOut o;
  o.pos = float4(corner.x * 2.0f - 1.0f, 1.0f - corner.y * 2.0f, 0.0f, 1.0f);
  o.uv = mix(p.uv_rect.xy, p.uv_rect.zw, corner);
  return o;
}

fragment float4 shift_blit_fs(BlitVsOut in [[stage_in]],
                              texture2d<float> src [[texture(0)]],
                              sampler samp [[sampler(0)]]) {
  return src.sample(samp, in.uv);
}
)";

}  // namespace

// -----------------------------------------------------------------------------
// Format & flag translation helpers (all 28 engine formats)
// -----------------------------------------------------------------------------
MTLPixelFormat to_mtl_pixel_format(Format fmt, bool bc_supported) noexcept {
  switch (fmt) {
    case Format::Undefined:    return MTLPixelFormatInvalid;
    case Format::R8Unorm:      return MTLPixelFormatR8Unorm;
    case Format::Rgba8Unorm:   return MTLPixelFormatRGBA8Unorm;
    case Format::Rgba8Srgb:    return MTLPixelFormatRGBA8Unorm_sRGB;
    case Format::Rgba8Snorm:   return MTLPixelFormatRGBA8Snorm;
    case Format::Bgra8Unorm:   return MTLPixelFormatBGRA8Unorm;
    case Format::Bgra8Srgb:    return MTLPixelFormatBGRA8Unorm_sRGB;
    case Format::Rgb10A2Unorm: return MTLPixelFormatRGB10A2Unorm;
    case Format::Rg16Unorm:    return MTLPixelFormatRG16Unorm;
    case Format::Rg16Sfloat:   return MTLPixelFormatRG16Float;
    case Format::Rgba16Sfloat: return MTLPixelFormatRGBA16Float;
    case Format::Rgba16Snorm:  return MTLPixelFormatRGBA16Snorm;
    case Format::Rgba16Sint:   return MTLPixelFormatRGBA16Sint;
    case Format::R32Sfloat:    return MTLPixelFormatR32Float;
    case Format::R32Uint:      return MTLPixelFormatR32Uint;
    case Format::Rg32Sfloat:   return MTLPixelFormatRG32Float;
    case Format::Rg32Uint:     return MTLPixelFormatRG32Uint;
    case Format::Rgb32Sfloat:  return MTLPixelFormatInvalid;  // vertex/RT attribute format only in Metal
    case Format::Rgba32Sfloat: return MTLPixelFormatRGBA32Float;
    case Format::Rgba32Uint:   return MTLPixelFormatRGBA32Uint;
    case Format::D32Sfloat:    return MTLPixelFormatDepth32Float;
    case Format::D16Unorm:     return MTLPixelFormatDepth16Unorm;
#if TARGET_OS_OSX || TARGET_OS_MACCATALYST
    case Format::Bc1RgbaUnorm: return bc_supported ? MTLPixelFormatBC1_RGBA : MTLPixelFormatInvalid;
    case Format::Bc1RgbaSrgb:  return bc_supported ? MTLPixelFormatBC1_RGBA_sRGB : MTLPixelFormatInvalid;
    case Format::Bc2Unorm:     return bc_supported ? MTLPixelFormatBC2_RGBA : MTLPixelFormatInvalid;
    case Format::Bc2Srgb:      return bc_supported ? MTLPixelFormatBC2_RGBA_sRGB : MTLPixelFormatInvalid;
    case Format::Bc3Unorm:     return bc_supported ? MTLPixelFormatBC3_RGBA : MTLPixelFormatInvalid;
    case Format::Bc3Srgb:      return bc_supported ? MTLPixelFormatBC3_RGBA_sRGB : MTLPixelFormatInvalid;
    case Format::Bc4Unorm:     return bc_supported ? MTLPixelFormatBC4_RUnorm : MTLPixelFormatInvalid;
    case Format::Bc5Unorm:     return bc_supported ? MTLPixelFormatBC5_RGUnorm : MTLPixelFormatInvalid;
#else
    case Format::Bc1RgbaUnorm:
    case Format::Bc1RgbaSrgb:
    case Format::Bc2Unorm:
    case Format::Bc2Srgb:
    case Format::Bc3Unorm:
    case Format::Bc3Srgb:
    case Format::Bc4Unorm:
    case Format::Bc5Unorm:
      (void)bc_supported;
      return MTLPixelFormatInvalid;
#endif
    case Format::Count:        return MTLPixelFormatInvalid;
  }
  return MTLPixelFormatInvalid;
}

Format from_mtl_pixel_format(MTLPixelFormat fmt) noexcept {
  switch (fmt) {
    case MTLPixelFormatR8Unorm:         return Format::R8Unorm;
    case MTLPixelFormatRGBA8Unorm:      return Format::Rgba8Unorm;
    case MTLPixelFormatRGBA8Unorm_sRGB: return Format::Rgba8Srgb;
    case MTLPixelFormatRGBA8Snorm:      return Format::Rgba8Snorm;
    case MTLPixelFormatBGRA8Unorm:      return Format::Bgra8Unorm;
    case MTLPixelFormatBGRA8Unorm_sRGB: return Format::Bgra8Srgb;
    case MTLPixelFormatRGB10A2Unorm:    return Format::Rgb10A2Unorm;
    case MTLPixelFormatRG16Unorm:       return Format::Rg16Unorm;
    case MTLPixelFormatRG16Float:       return Format::Rg16Sfloat;
    case MTLPixelFormatRGBA16Float:     return Format::Rgba16Sfloat;
    case MTLPixelFormatRGBA16Snorm:     return Format::Rgba16Snorm;
    case MTLPixelFormatRGBA16Sint:      return Format::Rgba16Sint;
    case MTLPixelFormatR32Float:        return Format::R32Sfloat;
    case MTLPixelFormatR32Uint:         return Format::R32Uint;
    case MTLPixelFormatRG32Float:       return Format::Rg32Sfloat;
    case MTLPixelFormatRG32Uint:        return Format::Rg32Uint;
    case MTLPixelFormatRGBA32Float:     return Format::Rgba32Sfloat;
    case MTLPixelFormatRGBA32Uint:      return Format::Rgba32Uint;
    case MTLPixelFormatDepth32Float:    return Format::D32Sfloat;
    case MTLPixelFormatDepth16Unorm:    return Format::D16Unorm;
#if TARGET_OS_OSX || TARGET_OS_MACCATALYST
    case MTLPixelFormatBC1_RGBA:        return Format::Bc1RgbaUnorm;
    case MTLPixelFormatBC1_RGBA_sRGB:   return Format::Bc1RgbaSrgb;
    case MTLPixelFormatBC2_RGBA:        return Format::Bc2Unorm;
    case MTLPixelFormatBC2_RGBA_sRGB:   return Format::Bc2Srgb;
    case MTLPixelFormatBC3_RGBA:        return Format::Bc3Unorm;
    case MTLPixelFormatBC3_RGBA_sRGB:   return Format::Bc3Srgb;
    case MTLPixelFormatBC4_RUnorm:      return Format::Bc4Unorm;
    case MTLPixelFormatBC5_RGUnorm:     return Format::Bc5Unorm;
#endif
    default:                            return Format::Undefined;
  }
}

MTLVertexFormat to_mtl_vertex_format(Format fmt) noexcept {
  switch (fmt) {
    case Format::R8Unorm:      return MTLVertexFormatUCharNormalized;
    case Format::Rgba8Unorm:   return MTLVertexFormatUChar4Normalized;
    case Format::Rgba8Snorm:   return MTLVertexFormatChar4Normalized;
    case Format::Bgra8Unorm:   return MTLVertexFormatUChar4Normalized_BGRA;
    case Format::Rgb10A2Unorm: return MTLVertexFormatUInt1010102Normalized;
    case Format::Rg16Unorm:    return MTLVertexFormatUShort2Normalized;
    case Format::Rg16Sfloat:   return MTLVertexFormatHalf2;
    case Format::Rgba16Sfloat: return MTLVertexFormatHalf4;
    case Format::Rgba16Snorm:  return MTLVertexFormatShort4Normalized;
    case Format::Rgba16Sint:   return MTLVertexFormatShort4;
    case Format::R32Sfloat:    return MTLVertexFormatFloat;
    case Format::R32Uint:      return MTLVertexFormatUInt;
    case Format::Rg32Sfloat:   return MTLVertexFormatFloat2;
    case Format::Rg32Uint:     return MTLVertexFormatUInt2;
    case Format::Rgb32Sfloat:  return MTLVertexFormatFloat3;
    case Format::Rgba32Sfloat: return MTLVertexFormatFloat4;
    case Format::Rgba32Uint:   return MTLVertexFormatUInt4;
    default:                   return MTLVertexFormatInvalid;
  }
}

MTLAttributeFormat to_mtl_attribute_format(Format fmt) noexcept {
  switch (fmt) {
    case Format::Rg16Sfloat:   return MTLAttributeFormatHalf2;
    case Format::Rgba16Sfloat: return MTLAttributeFormatHalf4;
    case Format::Rg32Sfloat:   return MTLAttributeFormatFloat2;
    case Format::Rgb32Sfloat:  return MTLAttributeFormatFloat3;
    case Format::Rgba32Sfloat: return MTLAttributeFormatFloat4;
    default:                   return MTLAttributeFormatFloat3;
  }
}

MTLTextureSwizzle to_mtl_swizzle(ComponentSwizzle s, MTLTextureSwizzle identity_channel) noexcept {
  switch (s) {
    case ComponentSwizzle::Zero: return MTLTextureSwizzleZero;
    case ComponentSwizzle::One:  return MTLTextureSwizzleOne;
    case ComponentSwizzle::R:    return MTLTextureSwizzleRed;
    case ComponentSwizzle::G:    return MTLTextureSwizzleGreen;
    case ComponentSwizzle::B:    return MTLTextureSwizzleBlue;
    case ComponentSwizzle::A:    return MTLTextureSwizzleAlpha;
    case ComponentSwizzle::Identity:
    default:                     return identity_channel;
  }
}

MTLTextureSwizzleChannels to_mtl_swizzle_channels(const SwizzleMapping& m) noexcept {
  return MTLTextureSwizzleChannelsMake(
      to_mtl_swizzle(m.r, MTLTextureSwizzleRed),
      to_mtl_swizzle(m.g, MTLTextureSwizzleGreen),
      to_mtl_swizzle(m.b, MTLTextureSwizzleBlue),
      to_mtl_swizzle(m.a, MTLTextureSwizzleAlpha));
}

bool is_identity_swizzle(const SwizzleMapping& m) noexcept {
  const bool r_id = (m.r == ComponentSwizzle::Identity || m.r == ComponentSwizzle::R);
  const bool g_id = (m.g == ComponentSwizzle::Identity || m.g == ComponentSwizzle::G);
  const bool b_id = (m.b == ComponentSwizzle::Identity || m.b == ComponentSwizzle::B);
  const bool a_id = (m.a == ComponentSwizzle::Identity || m.a == ComponentSwizzle::A);
  return r_id && g_id && b_id && a_id;
}

MTLSamplerMinMagFilter to_mtl_filter(Filter f) noexcept {
  return f == Filter::Nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
}

MTLSamplerMipFilter to_mtl_mip_filter(Filter f) noexcept {
  return f == Filter::Nearest ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
}

MTLSamplerAddressMode to_mtl_address_mode(AddressMode m) noexcept {
  switch (m) {
    case AddressMode::MirroredRepeat: return MTLSamplerAddressModeMirrorRepeat;
    case AddressMode::ClampToEdge:    return MTLSamplerAddressModeClampToEdge;
    case AddressMode::ClampToBorder:  return MTLSamplerAddressModeClampToBorderColor;
    case AddressMode::Repeat:
    default:                          return MTLSamplerAddressModeRepeat;
  }
}

MTLCompareFunction to_mtl_compare_func(CompareOp op) noexcept {
  switch (op) {
    case CompareOp::Never:          return MTLCompareFunctionNever;
    case CompareOp::Less:           return MTLCompareFunctionLess;
    case CompareOp::Equal:          return MTLCompareFunctionEqual;
    case CompareOp::LessOrEqual:    return MTLCompareFunctionLessEqual;
    case CompareOp::Greater:        return MTLCompareFunctionGreater;
    case CompareOp::NotEqual:       return MTLCompareFunctionNotEqual;
    case CompareOp::GreaterOrEqual: return MTLCompareFunctionGreaterEqual;
    case CompareOp::Always:         return MTLCompareFunctionAlways;
  }
  return MTLCompareFunctionAlways;
}

MTLSamplerBorderColor to_mtl_border_color(BorderColor c) noexcept {
  switch (c) {
    case BorderColor::TransparentBlack: return MTLSamplerBorderColorTransparentBlack;
    case BorderColor::OpaqueWhite:      return MTLSamplerBorderColorOpaqueWhite;
    case BorderColor::OpaqueBlack:
    default:                            return MTLSamplerBorderColorOpaqueBlack;
  }
}

MTLBlendFactor to_mtl_blend_factor(BlendFactor f) noexcept {
  switch (f) {
    case BlendFactor::Zero:                  return MTLBlendFactorZero;
    case BlendFactor::One:                   return MTLBlendFactorOne;
    case BlendFactor::SrcColor:              return MTLBlendFactorSourceColor;
    case BlendFactor::OneMinusSrcColor:      return MTLBlendFactorOneMinusSourceColor;
    case BlendFactor::DstColor:              return MTLBlendFactorDestinationColor;
    case BlendFactor::OneMinusDstColor:      return MTLBlendFactorOneMinusDestinationColor;
    case BlendFactor::SrcAlpha:              return MTLBlendFactorSourceAlpha;
    case BlendFactor::OneMinusSrcAlpha:      return MTLBlendFactorOneMinusSourceAlpha;
    case BlendFactor::DstAlpha:              return MTLBlendFactorDestinationAlpha;
    case BlendFactor::OneMinusDstAlpha:      return MTLBlendFactorOneMinusDestinationAlpha;
    case BlendFactor::ConstantColor:         return MTLBlendFactorBlendColor;
    case BlendFactor::OneMinusConstantColor: return MTLBlendFactorOneMinusBlendColor;
  }
  return MTLBlendFactorOne;
}

MTLBlendOperation to_mtl_blend_op(BlendOp op) noexcept {
  switch (op) {
    case BlendOp::Add:             return MTLBlendOperationAdd;
    case BlendOp::Subtract:        return MTLBlendOperationSubtract;
    case BlendOp::ReverseSubtract: return MTLBlendOperationReverseSubtract;
    case BlendOp::Min:             return MTLBlendOperationMin;
    case BlendOp::Max:             return MTLBlendOperationMax;
  }
  return MTLBlendOperationAdd;
}

MTLColorWriteMask to_mtl_write_mask(ColorWriteMask m) noexcept {
  MTLColorWriteMask out = MTLColorWriteMaskNone;
  if (has_flag(m, ColorWriteMask::R)) out |= MTLColorWriteMaskRed;
  if (has_flag(m, ColorWriteMask::G)) out |= MTLColorWriteMaskGreen;
  if (has_flag(m, ColorWriteMask::B)) out |= MTLColorWriteMaskBlue;
  if (has_flag(m, ColorWriteMask::A)) out |= MTLColorWriteMaskAlpha;
  return out;
}

MTLCullMode to_mtl_cull_mode(CullMode c) noexcept {
  switch (c) {
    case CullMode::Front: return MTLCullModeFront;
    case CullMode::Back:  return MTLCullModeBack;
    case CullMode::None:
    default:              return MTLCullModeNone;
  }
}

MTLWinding to_mtl_front_face(FrontFace f) noexcept {
  return f == FrontFace::Clockwise ? MTLWindingClockwise : MTLWindingCounterClockwise;
}

MTLPrimitiveType to_mtl_primitive_type(PrimitiveTopology t) noexcept {
  switch (t) {
    case PrimitiveTopology::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
    case PrimitiveTopology::LineList:      return MTLPrimitiveTypeLine;
    case PrimitiveTopology::TriangleList:
    default:                               return MTLPrimitiveTypeTriangle;
  }
}

MTLLoadAction to_mtl_load_action(LoadOp op) noexcept {
  switch (op) {
    case LoadOp::Load:     return MTLLoadActionLoad;
    case LoadOp::Clear:    return MTLLoadActionClear;
    case LoadOp::DontCare: return MTLLoadActionDontCare;
  }
  return MTLLoadActionClear;
}

MTLStoreAction to_mtl_store_action(StoreOp op, bool has_resolve) noexcept {
  switch (op) {
    case StoreOp::DontCare:
      return has_resolve ? MTLStoreActionMultisampleResolve : MTLStoreActionDontCare;
    case StoreOp::MultisampleResolve:
      return has_resolve ? MTLStoreActionMultisampleResolve : MTLStoreActionDontCare;
    case StoreOp::StoreAndResolve:
      return has_resolve ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionStore;
    case StoreOp::Store:
    default:
      return has_resolve ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionStore;
  }
}

MTLMultisampleDepthResolveFilter to_mtl_depth_resolve_filter(ResolveMode mode) noexcept {
  switch (mode) {
    case ResolveMode::SampleZero: return MTLMultisampleDepthResolveFilterSample0;
    case ResolveMode::Average:    return MTLMultisampleDepthResolveFilterMin;
    case ResolveMode::None:
    default:                      return MTLMultisampleDepthResolveFilterSample0;
  }
}

MTLAccelerationStructureUsage to_mtl_as_usage(AccelStructBuildFlags flags) noexcept {
  MTLAccelerationStructureUsage usage = MTLAccelerationStructureUsageNone;
  if (has_flag(flags, AccelStructBuildFlags::PreferFastBuild)) {
    usage |= MTLAccelerationStructureUsagePreferFastBuild;
  }
  if (has_flag(flags, AccelStructBuildFlags::AllowUpdate)) {
    usage |= MTLAccelerationStructureUsageRefit;
  }
  return usage;
}

// -----------------------------------------------------------------------------
// Metal 3 Tier 2 Argument Buffer BindlessTable
// -----------------------------------------------------------------------------
MetalBindlessTableImpl::MetalBindlessTableImpl(MetalDeviceImpl& device,
                                               BindGroupLayoutHandle layout_handle,
                                               BindGroupHandle group_handle,
                                               id<MTLBuffer> arg_buffer,
                                               uint32_t max_textures,
                                               uint32_t max_storage_buffers)
    : device_(device),
      layout_handle_(layout_handle),
      group_handle_(group_handle),
      arg_buffer_(arg_buffer),
      max_textures_(max_textures),
      max_storage_buffers_(max_storage_buffers),
      resident_textures_(max_textures, nil),
      resident_buffers_(max_storage_buffers, nil) {
  if (@available(macOS 15.0, iOS 18.0, *)) {
    MTLResidencySetDescriptor* rsd = [MTLResidencySetDescriptor new];
    rsd.label = @"shift_rhi_bindless_residency";
    rsd.initialCapacity = std::min<NSUInteger>(max_textures + max_storage_buffers + 16, 8192);
    NSError* err = nil;
    id<MTLResidencySet> rset = [device_.mtl_device() newResidencySetWithDescriptor:rsd error:&err];
    if (rset && !err) {
      residency_set_ = rset;
      has_residency_set_ = true;
      [residency_set_ addAllocation:arg_buffer_];
      [residency_set_ commit];
      id<MTLCommandQueue> gq = device_.mtl_queue(QueueKind::Graphics);
      id<MTLCommandQueue> cq = device_.mtl_queue(QueueKind::AsyncCompute);
      id<MTLCommandQueue> tq = device_.mtl_queue(QueueKind::Transfer);
      if (gq) [gq addResidencySet:residency_set_];
      if (cq && cq != gq) [cq addResidencySet:residency_set_];
      if (tq && tq != gq && tq != cq) [tq addResidencySet:residency_set_];
    }
  }
}

MetalBindlessTableImpl::~MetalBindlessTableImpl() {
  if (@available(macOS 15.0, iOS 18.0, *)) {
    if (has_residency_set_ && residency_set_) {
      id<MTLCommandQueue> gq = device_.mtl_queue(QueueKind::Graphics);
      id<MTLCommandQueue> cq = device_.mtl_queue(QueueKind::AsyncCompute);
      id<MTLCommandQueue> tq = device_.mtl_queue(QueueKind::Transfer);
      if (gq) [gq removeResidencySet:residency_set_];
      if (cq && cq != gq) [cq removeResidencySet:residency_set_];
      if (tq && tq != gq && tq != cq) [tq removeResidencySet:residency_set_];
      residency_set_ = nil;
    }
  }
  device_.destroy_bind_group(group_handle_);
  device_.destroy_bind_group_layout(layout_handle_);
}

void MetalBindlessTableImpl::mark_residency_dirty_locked() {
  residency_dirty_ = true;
}

void MetalBindlessTableImpl::set_sampler(SamplerHandle sampler) {
  SamplerEntry se{};
  if (!device_.lookup_sampler_copy(sampler, se) || !se.sampler || !arg_buffer_) return;
  std::lock_guard<std::mutex> lock(table_mutex_);
  auto* base = static_cast<uint8_t*>([arg_buffer_ contents]);
  const MTLResourceID rid = [se.sampler gpuResourceID];
  std::memcpy(base, &rid, sizeof(MTLResourceID));
}

void MetalBindlessTableImpl::set_texture(uint32_t slot, TextureViewHandle view) {
  if (slot >= max_textures_ || !arg_buffer_) return;
  TextureViewEntry tv{};
  if (!device_.lookup_texture_view_copy(view, tv) || !tv.view) return;
  std::lock_guard<std::mutex> lock(table_mutex_);
  auto* base = static_cast<uint8_t*>([arg_buffer_ contents]);
  const size_t offset = sizeof(MTLResourceID) + static_cast<size_t>(slot) * sizeof(MTLResourceID);
  const MTLResourceID rid = [tv.view gpuResourceID];
  std::memcpy(base + offset, &rid, sizeof(MTLResourceID));

  id<MTLTexture> prev = resident_textures_[slot];
  if (prev != tv.view) {
    resident_textures_[slot] = tv.view;
    if (@available(macOS 15.0, iOS 18.0, *)) {
      if (has_residency_set_ && residency_set_) {
        [residency_set_ addAllocation:tv.view];
        mark_residency_dirty_locked();
      }
    }
  }
}

void MetalBindlessTableImpl::set_storage_buffer(uint32_t slot, BufferHandle buffer,
                                                uint64_t offset, uint64_t /*size*/) {
  if (slot >= max_storage_buffers_ || !arg_buffer_) return;
  BufferEntry be{};
  if (!device_.lookup_buffer_copy(buffer, be) || !be.buffer) return;
  std::lock_guard<std::mutex> lock(table_mutex_);
  auto* base = static_cast<uint8_t*>([arg_buffer_ contents]);
  const size_t byte_off =
      sizeof(MTLResourceID) * (1u + static_cast<size_t>(max_textures_)) +
      static_cast<size_t>(slot) * sizeof(uint64_t);
  const uint64_t addr = [be.buffer gpuAddress] + offset;
  std::memcpy(base + byte_off, &addr, sizeof(uint64_t));

  id<MTLBuffer> prev = resident_buffers_[slot];
  if (prev != be.buffer) {
    resident_buffers_[slot] = be.buffer;
    if (@available(macOS 15.0, iOS 18.0, *)) {
      if (has_residency_set_ && residency_set_) {
        [residency_set_ addAllocation:be.buffer];
        mark_residency_dirty_locked();
      }
    }
  }
}

void MetalBindlessTableImpl::commit_residency_if_needed() {
  std::lock_guard<std::mutex> lock(table_mutex_);
  if (@available(macOS 15.0, iOS 18.0, *)) {
    if (has_residency_set_ && residency_set_ && residency_dirty_) {
      [residency_set_ commit];
      residency_dirty_ = false;
    }
  }
}

void MetalBindlessTableImpl::encode_use_resources_render(id<MTLRenderCommandEncoder> enc) const {
  if (!enc) return;
  std::lock_guard<std::mutex> lock(table_mutex_);
  if (has_residency_set_) {
    if (@available(macOS 15.0, iOS 18.0, *)) {
      if (residency_set_ && residency_dirty_) {
        [residency_set_ commit];
        const_cast<MetalBindlessTableImpl*>(this)->residency_dirty_ = false;
      }
    }
    return;
  }
  std::vector<__unsafe_unretained id<MTLResource>> unique_res;
  unique_res.reserve(64);
  auto push_unique = [&](id<MTLResource> r) {
    if (!r) return;
    if (std::find(unique_res.begin(), unique_res.end(), r) == unique_res.end()) {
      unique_res.push_back(r);
    }
  };
  for (id<MTLTexture> t : resident_textures_) push_unique(t);
  for (id<MTLBuffer> b : resident_buffers_) push_unique(b);
  if (!unique_res.empty()) {
    [enc useResources:unique_res.data()
                count:unique_res.size()
                usage:MTLResourceUsageRead
               stages:MTLRenderStageVertex | MTLRenderStageFragment];
  }
}

void MetalBindlessTableImpl::encode_use_resources_compute(id<MTLComputeCommandEncoder> enc) const {
  if (!enc) return;
  std::lock_guard<std::mutex> lock(table_mutex_);
  if (has_residency_set_) {
    if (@available(macOS 15.0, iOS 18.0, *)) {
      if (residency_set_ && residency_dirty_) {
        [residency_set_ commit];
        const_cast<MetalBindlessTableImpl*>(this)->residency_dirty_ = false;
      }
    }
    return;
  }
  std::vector<__unsafe_unretained id<MTLResource>> unique_res;
  unique_res.reserve(64);
  auto push_unique = [&](id<MTLResource> r) {
    if (!r) return;
    if (std::find(unique_res.begin(), unique_res.end(), r) == unique_res.end()) {
      unique_res.push_back(r);
    }
  };
  for (id<MTLTexture> t : resident_textures_) push_unique(t);
  for (id<MTLBuffer> b : resident_buffers_) push_unique(b);
  if (!unique_res.empty()) {
    [enc useResources:unique_res.data()
                count:unique_res.size()
                usage:MTLResourceUsageRead];
  }
}

// -----------------------------------------------------------------------------
// MetalDeviceImpl lifecycle & capability probing
// -----------------------------------------------------------------------------
MetalDeviceImpl::~MetalDeviceImpl() {
  @autoreleasepool {
    if (device_) {
      wait_idle();
      std::string cache_err;
      save_pipeline_cache(cache_err);

      // Clear all resource pools cleanly.
      std::lock_guard<std::mutex> lock(resource_mutex_);
      accel_structs_.clear();
      as_address_map_.clear();
      pipelines_.clear();
      pipeline_layouts_.clear();
      bind_groups_.clear();
      bind_group_layouts_.clear();
      samplers_.clear();
      texture_views_.clear();
      textures_.clear();
      buffers_.clear();
      query_pools_.clear();
      timeline_semaphores_.clear();
      live_alloc_count_ = 0;
      live_alloc_bytes_ = 0;
    }
    blit_pipelines_.clear();
    blit_sampler_nearest_ = nil;
    blit_sampler_linear_ = nil;
    blit_lib_ = nil;
    binary_archive_ = nil;
    timestamp_counter_set_ = nil;
    shared_event_listener_ = nil;
    transfer_queue_ = nil;
    compute_queue_ = nil;
    queue_ = nil;
    device_ = nil;
  }
}

bool MetalDeviceImpl::init(const DeviceDesc& desc, std::string& error) {
  @autoreleasepool {
    device_ = MTLCreateSystemDefaultDevice();
    if (!device_) {
      error = "MTLCreateSystemDefaultDevice() returned nil (no Metal GPU available)";
      return false;
    }

    queue_ = [device_ newCommandQueue];
    if (!queue_) {
      error = "failed to create MTLCommandQueue";
      return false;
    }
    if (desc.enable_async_compute) {
      compute_queue_ = [device_ newCommandQueue];
    }
    if (desc.enable_transfer_queue) {
      transfer_queue_ = [device_ newCommandQueue];
    }
    dispatch_queue_t ev_q =
        dispatch_queue_create("shift.rhi.metal.timeline", DISPATCH_QUEUE_SERIAL);
    shared_event_listener_ = [[MTLSharedEventListener alloc] initWithDispatchQueue:ev_q];

    pipeline_cache_path_ = desc.pipeline_cache_path;
    probe_capabilities();
    load_pipeline_cache();
    return true;
  }
}

void MetalDeviceImpl::probe_capabilities() {
  caps_ = BackendCaps{};
  caps_.backend = BackendKind::Metal;
  const char* dev_name = [[device_ name] UTF8String];
  caps_.device_name = dev_name ? dev_name : "Apple Metal GPU";
  caps_.vendor_id = 0x106Bu;  // Apple PCI vendor ID
  const uint64_t reg_id = [device_ registryID];
  caps_.device_id = static_cast<uint32_t>((reg_id ^ (reg_id >> 32)) & 0xFFFFFFFFull);
  caps_.api_version = 30100u;  // Metal 3.1

  // Determine Apple GPU family string for driver_info and cache invalidation.
  const char* gpu_family = "Metal2";
  if ([device_ supportsFamily:MTLGPUFamilyApple9]) {
    gpu_family = "Apple9 (Metal 3)";
  } else if ([device_ supportsFamily:MTLGPUFamilyApple8]) {
    gpu_family = "Apple8 (Metal 3)";
  } else if ([device_ supportsFamily:MTLGPUFamilyApple7]) {
    gpu_family = "Apple7 (Metal 3)";
  } else if ([device_ supportsFamily:MTLGPUFamilyMetal3]) {
    gpu_family = "Metal3";
  } else if ([device_ supportsFamily:MTLGPUFamilyMac2]) {
    gpu_family = "Mac2";
  }

  NSOperatingSystemVersion os_ver = [[NSProcessInfo processInfo] operatingSystemVersion];
  NSString* os_build = [[NSProcessInfo processInfo] operatingSystemVersionString];
  const char* os_str = os_build ? [os_build UTF8String] : "macOS";

  caps_.driver_name = "Apple Metal 3";
  caps_.driver_info = std::string(gpu_family) + " / " + os_str;
  caps_.driver_version = static_cast<uint32_t>(
      (os_ver.majorVersion << 16) | (os_ver.minorVersion << 8) | os_ver.patchVersion);

  // Deterministic 16-byte pipeline cache UUID tied to device, GPU family, and OS build.
  const uint64_t h0 = fnv1a_64_str(caps_.device_name.c_str(), 14695981039346656037ull);
  const uint64_t h1 = fnv1a_64_str(caps_.driver_info.c_str(), h0 ^ reg_id);
  std::memcpy(caps_.pipeline_cache_uuid.data(), &h0, 8);
  std::memcpy(caps_.pipeline_cache_uuid.data() + 8, &h1, 8);

  cache_key_.backend = BackendKind::Metal;
  cache_key_.vendor_id = caps_.vendor_id;
  cache_key_.device_id = caps_.device_id;
  cache_key_.uuid = caps_.pipeline_cache_uuid;

  // Memory & architecture flags
  const bool unified = [device_ hasUnifiedMemory];
  caps_.is_integrated = unified;
  caps_.is_discrete = !unified;
  caps_.is_software = false;
  caps_.is_mobile_tbdr = [device_ supportsFamily:MTLGPUFamilyApple1];
  caps_.is_moltenvk = false;
  caps_.lazy_memory = caps_.is_mobile_tbdr;  // MTLStorageModeMemoryless for TBDR attachments
  caps_.device_local_bytes = [device_ recommendedMaxWorkingSetSize];
  if (caps_.device_local_bytes == 0) {
    caps_.device_local_bytes = [[NSProcessInfo processInfo] physicalMemory] / 2;
  }

  // Modern rendering features
  caps_.dynamic_rendering = true;
  caps_.synchronization2 = true;
  caps_.timeline_semaphores = true;
  caps_.dedicated_async_compute = (compute_queue_ != nil);
  caps_.dedicated_transfer_queue = (transfer_queue_ != nil);
  caps_.subgroup_ops = true;
  caps_.mesh_shaders = [device_ supportsFamily:MTLGPUFamilyMetal3];
  caps_.buffer_device_address = true;  // Metal 3 MTLBuffer.gpuAddress
  caps_.depth_resolve_sample_zero = true;
  caps_.max_anisotropy = 16.0f;

  // Bindless Tier 2 Argument Buffers
  const bool tier2_args = ([device_ argumentBuffersSupport] == MTLArgumentBuffersTier2);
  caps_.bindless = tier2_args;
  caps_.max_bindless_textures = tier2_args ? 500000u : 0u;

  // Metal lacks a single native GPU-driven drawIndexedIndirectCount inside an active
  // render encoder without a pre-pass compute->ICB encode; keep indirect_count=false
  // until pre-pass ICB encoding is wired into draw_indexed_indirect_count().
  caps_.indirect_count = false;
  caps_.multi_draw_indirect = true;
  caps_.fragment_shading_rate = [device_ supportsRasterizationRateMapWithLayerCount:1];

  // MSAA sample counts
  caps_.supported_msaa_mask = 1u;
  if ([device_ supportsTextureSampleCount:2]) caps_.supported_msaa_mask |= 2u;
  if ([device_ supportsTextureSampleCount:4]) caps_.supported_msaa_mask |= 4u;
  if ([device_ supportsTextureSampleCount:8]) caps_.supported_msaa_mask |= 8u;

  // Dynamic libraries & BC texture compression
  supports_dynamic_libraries_ = [device_ supportsDynamicLibraries];
#if TARGET_OS_OSX || TARGET_OS_MACCATALYST
  supports_bc_textures_ = [device_ supportsBCTextureCompression];
#else
  supports_bc_textures_ = false;
#endif

  // Apple Silicon Hardware Ray Tracing
  const bool has_rt = [device_ supportsRaytracing];
  caps_.ray_query = has_rt;
  caps_.rt_pipeline = has_rt;
  caps_.rt_maintenance1 = has_rt;
  caps_.min_as_scratch_alignment = 256u;

  // Timestamp counter set & sampling boundary support
  timestamp_counter_set_ = nil;
  for (id<MTLCounterSet> cs in [device_ counterSets]) {
    if ([[cs name] isEqualToString:MTLCommonCounterSetTimestamp]) {
      timestamp_counter_set_ = cs;
      break;
    }
  }
  supports_stage_boundary_counters_ =
      [device_ supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary];
  supports_blit_boundary_counters_ =
      [device_ supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary];
  caps_.timestamps = (timestamp_counter_set_ != nil) &&
                     (supports_stage_boundary_counters_ || supports_blit_boundary_counters_);
  // Metal resolved timestamp counters on Apple Silicon / macOS are in nanoseconds.
  caps_.timestamp_period_ns = 1.0;

  // Apple XDR EDR HDR headroom
  caps_.hdr10 = false;
  caps_.scrgb = true;
  caps_.edr = true;
  caps_.edr_max_headroom = 1.0f;
#ifdef SHIFT_RHI_HAS_APPKIT
  if (NSScreen* screen = [NSScreen mainScreen]) {
    const CGFloat max_edr = [screen maximumPotentialExtendedDynamicRangeColorComponentValue];
    const CGFloat cur_edr = [screen maximumExtendedDynamicRangeColorComponentValue];
    const CGFloat best = std::max(max_edr, cur_edr);
    if (best > 1.0) caps_.edr_max_headroom = static_cast<float>(best);
  }
#endif

  // Optional MetalFX spatial / temporal upscaling probe
#ifdef SHIFT_RHI_HAS_METALFX
  if (@available(macOS 13.0, iOS 16.0, *)) {
    supports_metalfx_spatial_ = [MTLFXSpatialScalerDescriptor supportsDevice:device_];
    supports_metalfx_temporal_ = [MTLFXTemporalScalerDescriptor supportsDevice:device_];
  }
#endif
}

void MetalDeviceImpl::load_pipeline_cache() {
  pipeline_cache_loaded_ = false;
  pipeline_cache_dirty_ = false;

  std::vector<uint8_t> initial_payload;
  if (!pipeline_cache_path_.empty()) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(pipeline_cache_path_, ec)) {
      std::ifstream in(pipeline_cache_path_, std::ios::binary | std::ios::ate);
      if (in) {
        const auto sz = in.tellg();
        if (sz > 0 && sz < (128LL << 20)) {
          std::vector<uint8_t> raw(static_cast<size_t>(sz));
          in.seekg(0);
          if (in.read(reinterpret_cast<char*>(raw.data()), sz)) {
            decode_pipeline_cache_envelope(raw.data(), raw.size(), cache_key_, initial_payload);
          }
        }
      }
    }
  }

  MTLBinaryArchiveDescriptor* bdesc = [MTLBinaryArchiveDescriptor new];
  NSError* ns_err = nil;
  if (!initial_payload.empty()) {
    const std::filesystem::path tmp_load = pipeline_cache_path_.string() + ".load.metallibarchive";
    {
      std::ofstream out(tmp_load, std::ios::binary | std::ios::trunc);
      if (out) {
        out.write(reinterpret_cast<const char*>(initial_payload.data()),
                  static_cast<std::streamsize>(initial_payload.size()));
      }
    }
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:tmp_load.c_str()]];
    bdesc.url = url;
    binary_archive_ = [device_ newBinaryArchiveWithDescriptor:bdesc error:&ns_err];
    std::error_code ec;
    std::filesystem::remove(tmp_load, ec);
    if (binary_archive_ && !ns_err) {
      pipeline_cache_loaded_ = true;
      return;
    }
    binary_archive_ = nil;
    ns_err = nil;
  }

  bdesc.url = nil;
  binary_archive_ = [device_ newBinaryArchiveWithDescriptor:bdesc error:&ns_err];
}

bool MetalDeviceImpl::save_pipeline_cache(std::string& error) {
  if (!device_ || !binary_archive_ || pipeline_cache_path_.empty()) return true;
  if (!pipeline_cache_dirty_ && pipeline_cache_loaded_) return true;

  @autoreleasepool {
    std::error_code ec;
    if (pipeline_cache_path_.has_parent_path()) {
      std::filesystem::create_directories(pipeline_cache_path_.parent_path(), ec);
    }

    const std::filesystem::path tmp_archive = pipeline_cache_path_.string() + ".tmp.metallibarchive";
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:tmp_archive.c_str()]];
    NSError* ns_err = nil;
    if (![binary_archive_ serializeToURL:url error:&ns_err]) {
      std::filesystem::remove(tmp_archive, ec);
      error = ns_err_str("MTLBinaryArchive serializeToURL", ns_err);
      return false;
    }

    std::vector<uint8_t> payload;
    {
      std::ifstream in(tmp_archive, std::ios::binary | std::ios::ate);
      if (!in) {
        std::filesystem::remove(tmp_archive, ec);
        error = "failed to read serialized MTLBinaryArchive";
        return false;
      }
      const auto sz = in.tellg();
      if (sz > 0) {
        payload.resize(static_cast<size_t>(sz));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(payload.data()), sz);
      }
    }
    std::filesystem::remove(tmp_archive, ec);
    if (payload.empty()) return true;

    const std::vector<uint8_t> encoded =
        encode_pipeline_cache_envelope(cache_key_, payload.data(), payload.size());
    const std::filesystem::path tmp_path = pipeline_cache_path_.string() + ".tmp";
    {
      std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
      if (!out) {
        error = "failed to open temporary pipeline cache file for writing";
        return false;
      }
      out.write(reinterpret_cast<const char*>(encoded.data()),
                static_cast<std::streamsize>(encoded.size()));
      if (!out.good()) {
        error = "failed writing pipeline cache envelope";
        return false;
      }
    }
    std::filesystem::rename(tmp_path, pipeline_cache_path_, ec);
    if (ec) {
      std::error_code ec2;
      std::filesystem::remove(pipeline_cache_path_, ec2);
      std::filesystem::rename(tmp_path, pipeline_cache_path_, ec2);
      if (ec2) {
        std::filesystem::remove(tmp_path, ec2);
        error = "failed renaming Metal pipeline cache into place";
        return false;
      }
    }
    pipeline_cache_loaded_ = true;
    pipeline_cache_dirty_ = false;
    return true;
  }
}

VramStats MetalDeviceImpl::vram_stats() const {
  VramStats out{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    out.allocation_count = live_alloc_count_;
    out.block_count = live_alloc_count_;
    out.allocation_bytes = live_alloc_bytes_;
    out.block_bytes = live_alloc_bytes_;
  }
  if (device_) {
    out.device_local_usage = [device_ currentAllocatedSize];
    out.device_local_budget = [device_ recommendedMaxWorkingSetSize];
  }
  return out;
}

id<MTLCommandQueue> MetalDeviceImpl::mtl_queue(QueueKind kind) const noexcept {
  switch (kind) {
    case QueueKind::AsyncCompute: return compute_queue_ ? compute_queue_ : queue_;
    case QueueKind::Transfer: return transfer_queue_ ? transfer_queue_ : queue_;
    case QueueKind::Graphics:
    default: return queue_;
  }
}

void MetalDeviceImpl::wait_idle() {
  @autoreleasepool {
    auto drain = [](id<MTLCommandQueue> q) {
      if (!q) return;
      id<MTLCommandBuffer> fence_cb = [q commandBuffer];
      if (fence_cb) {
        [fence_cb commit];
        [fence_cb waitUntilCompleted];
      }
    };
    drain(queue_);
    if (compute_queue_ && compute_queue_ != queue_) drain(compute_queue_);
    if (transfer_queue_ && transfer_queue_ != queue_ && transfer_queue_ != compute_queue_) {
      drain(transfer_queue_);
    }
  }
}

// -----------------------------------------------------------------------------
// Thread-safe lookup helpers (return ARC-safe value copies)
// -----------------------------------------------------------------------------
bool MetalDeviceImpl::lookup_buffer_copy(BufferHandle h, BufferEntry& out) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  const BufferEntry* e = buffers_.get(h);
  if (!e) return false;
  out = *e;
  return true;
}

bool MetalDeviceImpl::lookup_texture_copy(TextureHandle h, TextureEntry& out) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  const TextureEntry* e = textures_.get(h);
  if (!e) return false;
  out = *e;
  return true;
}

bool MetalDeviceImpl::lookup_texture_view_copy(TextureViewHandle h, TextureViewEntry& out) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  const TextureViewEntry* e = texture_views_.get(h);
  if (!e) return false;
  out = *e;
  return true;
}

bool MetalDeviceImpl::lookup_sampler_copy(SamplerHandle h, SamplerEntry& out) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  const SamplerEntry* e = samplers_.get(h);
  if (!e) return false;
  out = *e;
  return true;
}

bool MetalDeviceImpl::lookup_bind_group_copy(BindGroupHandle h, BindGroupEntry& out) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  const BindGroupEntry* e = bind_groups_.get(h);
  if (!e) return false;
  out = *e;
  return true;
}

bool MetalDeviceImpl::lookup_pipeline_copy(PipelineHandle h, PipelineEntry& out) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  const PipelineEntry* e = pipelines_.get(h);
  if (!e) return false;
  out = *e;
  return true;
}

bool MetalDeviceImpl::lookup_query_pool_copy(QueryPoolHandle h, QueryPoolEntry& out) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  const QueryPoolEntry* e = query_pools_.get(h);
  if (!e) return false;
  out = *e;
  return true;
}

bool MetalDeviceImpl::lookup_accel_struct_copy(AccelStructHandle h, AccelStructEntry& out) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  const AccelStructEntry* e = accel_structs_.get(h);
  if (!e) return false;
  out = *e;
  return true;
}

id<MTLAccelerationStructure> MetalDeviceImpl::lookup_as_by_address(uint64_t gpu_addr) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  auto it = as_address_map_.find(gpu_addr);
  if (it == as_address_map_.end()) return nil;
  const AccelStructEntry* e = accel_structs_.get(it->second);
  return e ? e->handle : nil;
}

void MetalDeviceImpl::set_tlas_instanced_blases(AccelStructHandle h,
                                                NSArray<id<MTLAccelerationStructure>>* blases) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  if (AccelStructEntry* e = accel_structs_.get(h)) {
    e->instanced_blases = blases;
  }
}

// -----------------------------------------------------------------------------
// External swapchain drawable registration
// -----------------------------------------------------------------------------
TextureHandle MetalDeviceImpl::register_external_texture(id<MTLTexture> tex, Format fmt,
                                                         uint32_t w, uint32_t h) {
  TextureEntry e{};
  e.texture = tex;
  e.mtl_format = [tex pixelFormat];
  e.format = fmt;
  e.type = TextureType::Tex2D;
  e.width = w;
  e.height = h;
  e.owned = false;
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return textures_.allocate(std::move(e));
}

TextureViewHandle MetalDeviceImpl::register_external_texture_view(TextureHandle tex_h,
                                                                  id<MTLTexture> tex, Format fmt) {
  TextureViewEntry e{};
  e.view = tex;
  e.texture = tex_h;
  e.mtl_format = [tex pixelFormat];
  e.format = fmt;
  e.is_depth = format_is_depth(fmt);
  e.owned = false;
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return texture_views_.allocate(std::move(e));
}

void MetalDeviceImpl::unregister_external_texture_view(TextureViewHandle h) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  texture_views_.remove(h);
}

void MetalDeviceImpl::unregister_external_texture(TextureHandle h) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  textures_.remove(h);
}

// -----------------------------------------------------------------------------
// Buffers
// -----------------------------------------------------------------------------
BufferHandle MetalDeviceImpl::create_buffer(const BufferDesc& desc, std::string& error) {
  if (desc.size == 0) {
    error = "create_buffer: size must be > 0";
    return {};
  }
  @autoreleasepool {
    MTLResourceOptions opts = MTLResourceStorageModePrivate;
    switch (desc.memory) {
      case MemoryHint::CpuToGpu:
        opts = MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
        break;
      case MemoryHint::GpuToCpu:
        opts = MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache;
        break;
      case MemoryHint::Transient:
      case MemoryHint::GpuOnly:
      default:
        opts = MTLResourceStorageModePrivate;
        break;
    }

    id<MTLBuffer> buf = [device_ newBufferWithLength:desc.size options:opts];
    if (!buf) {
      error = "MTLDevice newBufferWithLength failed";
      return {};
    }
    if (desc.debug_name && *desc.debug_name) {
      buf.label = [NSString stringWithUTF8String:desc.debug_name];
    }

    BufferEntry entry{};
    entry.buffer = buf;
    entry.size = desc.size;
    entry.usage = desc.usage;
    entry.memory = desc.memory;
    entry.device_address = [buf gpuAddress];
    entry.tracked_alloc = true;

    std::lock_guard<std::mutex> lock(resource_mutex_);
    ++live_alloc_count_;
    live_alloc_bytes_ += desc.size;
    return buffers_.allocate(std::move(entry));
  }
}

void MetalDeviceImpl::destroy_buffer(BufferHandle handle) {
  BufferEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!buffers_.remove(handle, &entry)) return;
    if (entry.tracked_alloc) {
      if (live_alloc_count_ > 0) --live_alloc_count_;
      live_alloc_bytes_ = (live_alloc_bytes_ >= entry.size) ? (live_alloc_bytes_ - entry.size) : 0;
    }
  }
  entry.buffer = nil;
}

void* MetalDeviceImpl::map_buffer(BufferHandle handle) {
  BufferEntry e{};
  if (!lookup_buffer_copy(handle, e) || !e.buffer) return nullptr;
  if ([e.buffer storageMode] == MTLStorageModePrivate) return nullptr;
  return [e.buffer contents];
}

void MetalDeviceImpl::unmap_buffer(BufferHandle /*handle*/) {
  // Apple Silicon unified memory MTLStorageModeShared is coherent with zero unmap needed.
}

uint64_t MetalDeviceImpl::buffer_device_address(BufferHandle handle) const {
  BufferEntry e{};
  if (!lookup_buffer_copy(handle, e)) return 0ull;
  return e.device_address;
}

// -----------------------------------------------------------------------------
// Textures & Views (2D, Array, Cube, 3D, MSAA, Memoryless TBDR)
// -----------------------------------------------------------------------------
TextureHandle MetalDeviceImpl::create_texture(const TextureDesc& desc, std::string& error) {
  if (desc.width == 0 || desc.height == 0) {
    error = "create_texture: width and height must be > 0";
    return {};
  }
  const MTLPixelFormat mtl_fmt = to_mtl_pixel_format(desc.format, supports_bc_textures_);
  if (mtl_fmt == MTLPixelFormatInvalid) {
    error = std::string("create_texture: unsupported Metal pixel format ") + format_name(desc.format);
    return {};
  }

  @autoreleasepool {
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.pixelFormat = mtl_fmt;
    td.width = desc.width;
    td.height = desc.height;
    td.depth = (desc.type == TextureType::Tex3D) ? std::max(desc.depth, 1u) : 1u;
    td.mipmapLevelCount = std::max(desc.mip_levels, 1u);
    td.sampleCount = std::max(desc.sample_count, 1u);

    const uint32_t layers = std::max(desc.array_layers, 1u);
    if (desc.type == TextureType::Tex3D) {
      td.textureType = MTLTextureType3D;
      td.arrayLength = 1;
    } else if (desc.type == TextureType::TexCube) {
      if (layers > 6u) {
        td.textureType = MTLTextureTypeCubeArray;
        td.arrayLength = layers / 6u;
      } else {
        td.textureType = MTLTextureTypeCube;
        td.arrayLength = 1;
      }
    } else {
      if (td.sampleCount > 1) {
        td.textureType = (layers > 1u) ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample;
        td.arrayLength = layers;
      } else if (layers > 1u) {
        td.textureType = MTLTextureType2DArray;
        td.arrayLength = layers;
      } else {
        td.textureType = MTLTextureType2D;
        td.arrayLength = 1;
      }
    }

    const bool is_compressed = format_is_compressed(desc.format);
    const bool is_depth = format_is_depth(desc.format);
    MTLTextureUsage mtl_usage = 0;
    if (has_flag(desc.usage, TextureUsage::Sampled)) mtl_usage |= MTLTextureUsageShaderRead;
    if (has_flag(desc.usage, TextureUsage::Storage)) mtl_usage |= MTLTextureUsageShaderWrite;
    if (has_flag(desc.usage, TextureUsage::ColorAttachment) ||
        has_flag(desc.usage, TextureUsage::DepthAttachment)) {
      mtl_usage |= MTLTextureUsageRenderTarget;
    }
    // Allow internal fullscreen-triangle blit & render-pass clear on TransferSrc/TransferDst textures.
    if (has_flag(desc.usage, TextureUsage::TransferSrc) && !is_compressed) {
      mtl_usage |= MTLTextureUsageShaderRead;
    }
    if (has_flag(desc.usage, TextureUsage::TransferDst) && !is_compressed) {
      mtl_usage |= MTLTextureUsageRenderTarget;
    }
    // Enable swizzled/reinterpreted views on sampled color textures.
    if (has_flag(desc.usage, TextureUsage::Sampled) && !is_depth) {
      mtl_usage |= MTLTextureUsagePixelFormatView;
    }
    if (mtl_usage == 0) mtl_usage = MTLTextureUsageShaderRead;
    td.usage = mtl_usage;

    // Memoryless TBDR MSAA / transient attachment detection on Apple Silicon!
    const bool wants_transient =
        (desc.memory == MemoryHint::Transient || has_flag(desc.usage, TextureUsage::Transient));
    const bool attachment_only =
        !has_flag(desc.usage, TextureUsage::Sampled | TextureUsage::Storage |
                                  TextureUsage::TransferSrc | TextureUsage::TransferDst);
    const bool use_memoryless = wants_transient && attachment_only && caps_.lazy_memory &&
                                ((mtl_usage & MTLTextureUsageRenderTarget) != 0);
    if (use_memoryless) {
      td.storageMode = MTLStorageModeMemoryless;
      td.usage = MTLTextureUsageRenderTarget;
    } else {
      td.storageMode = MTLStorageModePrivate;
    }

    id<MTLTexture> tex = [device_ newTextureWithDescriptor:td];
    if (!tex) {
      error = "MTLDevice newTextureWithDescriptor failed";
      return {};
    }
    if (desc.debug_name && *desc.debug_name) {
      tex.label = [NSString stringWithUTF8String:desc.debug_name];
    }

    const uint64_t approx_bytes =
        use_memoryless ? 0ull
                       : format_image_bytes(desc.format, desc.width, desc.height,
                                            desc.type == TextureType::Tex3D ? std::max(desc.depth, 1u)
                                                                            : layers) *
                             std::max(desc.sample_count, 1u);

    TextureEntry entry{};
    entry.texture = tex;
    entry.mtl_format = mtl_fmt;
    entry.format = desc.format;
    entry.type = desc.type;
    entry.width = desc.width;
    entry.height = desc.height;
    entry.depth = desc.depth;
    entry.mip_levels = std::max(desc.mip_levels, 1u);
    entry.array_layers = layers;
    entry.sample_count = std::max(desc.sample_count, 1u);
    entry.owned = true;
    entry.is_memoryless = use_memoryless;
    entry.approx_bytes = approx_bytes;

    std::lock_guard<std::mutex> lock(resource_mutex_);
    ++live_alloc_count_;
    live_alloc_bytes_ += approx_bytes;
    return textures_.allocate(std::move(entry));
  }
}

void MetalDeviceImpl::destroy_texture(TextureHandle handle) {
  TextureEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!textures_.remove(handle, &entry)) return;
    if (entry.owned) {
      if (live_alloc_count_ > 0) --live_alloc_count_;
      live_alloc_bytes_ =
          (live_alloc_bytes_ >= entry.approx_bytes) ? (live_alloc_bytes_ - entry.approx_bytes) : 0;
    }
  }
  entry.texture = nil;
}

TextureViewHandle MetalDeviceImpl::create_texture_view(const TextureViewDesc& desc, std::string& error) {
  TextureEntry te{};
  if (!lookup_texture_copy(desc.texture, te) || !te.texture) {
    error = "create_texture_view: invalid TextureHandle";
    return {};
  }

  @autoreleasepool {
    const Format fmt = (desc.format == Format::Undefined) ? te.format : desc.format;
    const MTLPixelFormat mtl_fmt = to_mtl_pixel_format(fmt, supports_bc_textures_);
    if (mtl_fmt == MTLPixelFormatInvalid) {
      error = "create_texture_view: invalid pixel format";
      return {};
    }

    MTLTextureType view_type = [te.texture textureType];
    if (te.sample_count == 1) {
      if (desc.type == TextureType::TexCube) {
        view_type = (desc.layer_count > 6u) ? MTLTextureTypeCubeArray : MTLTextureTypeCube;
      } else if (desc.type == TextureType::Tex3D) {
        view_type = MTLTextureType3D;
      } else {
        view_type = (desc.layer_count > 1u) ? MTLTextureType2DArray : MTLTextureType2D;
      }
    }

    const bool identity_swiz = is_identity_swizzle(desc.swizzle);
    const bool full_range = (mtl_fmt == te.mtl_format) &&
                            (view_type == [te.texture textureType]) &&
                            (desc.base_mip == 0 && desc.mip_count == te.mip_levels) &&
                            (desc.base_layer == 0 && desc.layer_count == te.array_layers);

    id<MTLTexture> view = nil;
    if (identity_swiz && full_range) {
      view = te.texture;
    } else if (identity_swiz) {
      view = [te.texture newTextureViewWithPixelFormat:mtl_fmt
                                           textureType:view_type
                                                levels:NSMakeRange(desc.base_mip, desc.mip_count)
                                                slices:NSMakeRange(desc.base_layer, desc.layer_count)];
    } else {
      view = [te.texture newTextureViewWithPixelFormat:mtl_fmt
                                           textureType:view_type
                                                levels:NSMakeRange(desc.base_mip, desc.mip_count)
                                                slices:NSMakeRange(desc.base_layer, desc.layer_count)
                                               swizzle:to_mtl_swizzle_channels(desc.swizzle)];
    }

    if (!view) {
      error = "MTLTexture newTextureViewWithPixelFormat failed";
      return {};
    }

    TextureViewEntry entry{};
    entry.view = view;
    entry.texture = desc.texture;
    entry.mtl_format = mtl_fmt;
    entry.format = fmt;
    entry.is_depth = format_is_depth(fmt);
    entry.owned = true;

    std::lock_guard<std::mutex> lock(resource_mutex_);
    return texture_views_.allocate(std::move(entry));
  }
}

void MetalDeviceImpl::destroy_texture_view(TextureViewHandle handle) {
  TextureViewEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!texture_views_.remove(handle, &entry)) return;
  }
  entry.view = nil;
}

// -----------------------------------------------------------------------------
// Samplers (supportArgumentBuffers = YES for Metal 3 Tier 2 Bindless)
// -----------------------------------------------------------------------------
SamplerHandle MetalDeviceImpl::create_sampler(const SamplerDesc& desc, std::string& error) {
  @autoreleasepool {
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = to_mtl_filter(desc.min_filter);
    sd.magFilter = to_mtl_filter(desc.mag_filter);
    sd.mipFilter = to_mtl_mip_filter(desc.mip_filter);
    sd.sAddressMode = to_mtl_address_mode(desc.address_u);
    sd.tAddressMode = to_mtl_address_mode(desc.address_v);
    sd.rAddressMode = to_mtl_address_mode(desc.address_w);
    sd.lodMinClamp = desc.min_lod;
    sd.lodMaxClamp = desc.max_lod;
    sd.maxAnisotropy = static_cast<NSUInteger>(std::clamp(desc.max_anisotropy, 1.0f, caps_.max_anisotropy));
    sd.compareFunction = desc.compare_enabled ? to_mtl_compare_func(desc.compare_op) : MTLCompareFunctionNever;
    sd.borderColor = to_mtl_border_color(desc.border_color);
    sd.supportArgumentBuffers = YES;

    id<MTLSamplerState> samp = [device_ newSamplerStateWithDescriptor:sd];
    if (!samp) {
      error = "MTLDevice newSamplerStateWithDescriptor failed";
      return {};
    }

    SamplerEntry entry{};
    entry.sampler = samp;
    std::lock_guard<std::mutex> lock(resource_mutex_);
    return samplers_.allocate(std::move(entry));
  }
}

void MetalDeviceImpl::destroy_sampler(SamplerHandle handle) {
  SamplerEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!samplers_.remove(handle, &entry)) return;
  }
  entry.sampler = nil;
}

// -----------------------------------------------------------------------------
// Bind Groups & Metal 3 Tier 2 Argument Buffer BindlessTable
// -----------------------------------------------------------------------------
BindGroupLayoutHandle MetalDeviceImpl::create_bind_group_layout(const BindGroupLayoutDesc& desc,
                                                                std::string& /*error*/) {
  BindGroupLayoutEntry entry{};
  entry.bindings.assign(desc.bindings.begin(), desc.bindings.end());
  entry.update_after_bind = desc.update_after_bind_pool;
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return bind_group_layouts_.allocate(std::move(entry));
}

void MetalDeviceImpl::destroy_bind_group_layout(BindGroupLayoutHandle handle) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  bind_group_layouts_.remove(handle);
}

BindGroupHandle MetalDeviceImpl::create_bind_group(const BindGroupDesc& desc, std::string& error) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  if (!bind_group_layouts_.get(desc.layout)) {
    error = "create_bind_group: invalid BindGroupLayoutHandle";
    return {};
  }

  BindGroupEntry entry{};
  entry.layout = desc.layout;
  entry.entries.reserve(desc.entries.size());

  for (const shift::rhi::BindGroupEntry& e : desc.entries) {
    ResolvedBindEntry re{};
    re.binding = e.binding;
    re.array_element = e.array_element;
    re.type = e.type;

    if (e.type == DescriptorType::UniformBuffer || e.type == DescriptorType::StorageBuffer) {
      const BufferEntry* be = buffers_.get(e.buffer);
      if (!be || !be->buffer) {
        error = "create_bind_group: invalid BufferHandle";
        return {};
      }
      re.buffer = be->buffer;
      re.buffer_offset = e.buffer_offset;
    } else if (e.type == DescriptorType::Sampler) {
      const SamplerEntry* se = samplers_.get(e.sampler);
      if (!se || !se->sampler) {
        error = "create_bind_group: invalid SamplerHandle";
        return {};
      }
      re.sampler = se->sampler;
    } else if (e.type == DescriptorType::SampledTexture ||
               e.type == DescriptorType::StorageTexture ||
               e.type == DescriptorType::CombinedTextureSampler) {
      const TextureViewEntry* tv = texture_views_.get(e.texture_view);
      if (!tv || !tv->view) {
        error = "create_bind_group: invalid TextureViewHandle";
        return {};
      }
      re.texture = tv->view;
      if (e.type == DescriptorType::CombinedTextureSampler) {
        const SamplerEntry* se = samplers_.get(e.sampler);
        if (!se || !se->sampler) {
          error = "create_bind_group: invalid SamplerHandle for CombinedTextureSampler";
          return {};
        }
        re.sampler = se->sampler;
      }
    } else if (e.type == DescriptorType::AccelStruct) {
      const AccelStructEntry* as = accel_structs_.get(e.accel_struct);
      if (!as || !as->handle) {
        error = "create_bind_group: invalid AccelStructHandle";
        return {};
      }
      re.accel_struct = as->handle;
      if (as->instanced_blases) {
        for (id<MTLAccelerationStructure> b in as->instanced_blases) {
          re.tlas_blases.push_back(b);
        }
      }
    }
    entry.entries.push_back(std::move(re));
  }

  return bind_groups_.allocate(std::move(entry));
}

void MetalDeviceImpl::destroy_bind_group(BindGroupHandle handle) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  bind_groups_.remove(handle);
}

std::unique_ptr<BindlessTable> MetalDeviceImpl::create_bindless_table(const BindlessTableDesc& desc,
                                                                      std::string& error) {
  if (!caps_.bindless) {
    error = "Metal 3 Tier 2 argument buffers are not supported on this device";
    return nullptr;
  }
  const uint32_t max_tex = std::min(desc.max_textures, caps_.max_bindless_textures);
  if (max_tex == 0) {
    error = "create_bindless_table: max_textures must be > 0";
    return nullptr;
  }

  @autoreleasepool {
    std::vector<BindGroupLayoutBinding> bindings;
    bindings.push_back({0, DescriptorType::Sampler, 1, desc.stages, false, false});
    bindings.push_back({1, DescriptorType::SampledTexture, max_tex, desc.stages, true, true});
    if (desc.max_storage_buffers > 0) {
      bindings.push_back({2, DescriptorType::StorageBuffer, desc.max_storage_buffers, desc.stages, true, false});
    }

    BindGroupLayoutDesc ldesc{};
    ldesc.bindings = bindings;
    ldesc.update_after_bind_pool = true;
    BindGroupLayoutHandle layout_h = create_bind_group_layout(ldesc, error);
    if (!layout_h) return nullptr;

    // Allocate Metal 3 Tier 2 Argument Buffer in Unified Shared memory:
    //   [0]: MTLResourceID default_sampler
    //   [8 .. 8 + 8*max_tex]: MTLResourceID textures[max_tex]
    //   [8*(1+max_tex) .. ]: uint64_t storage_buffer_addrs[max_storage_buffers]
    const size_t arg_bytes =
        sizeof(MTLResourceID) * (1u + static_cast<size_t>(max_tex)) +
        sizeof(uint64_t) * static_cast<size_t>(desc.max_storage_buffers);
    id<MTLBuffer> arg_buf =
        [device_ newBufferWithLength:arg_bytes options:MTLResourceStorageModeShared];
    if (!arg_buf) {
      destroy_bind_group_layout(layout_h);
      error = "create_bindless_table: failed to allocate Metal 3 argument buffer";
      return nullptr;
    }
    arg_buf.label = @"shift_rhi_bindless_argbuf";
    std::memset([arg_buf contents], 0, arg_bytes);

    BindGroupEntry bge{};
    bge.layout = layout_h;
    BindGroupHandle group_h;
    {
      std::lock_guard<std::mutex> lock(resource_mutex_);
      group_h = bind_groups_.allocate(std::move(bge));
    }

    auto table = std::make_unique<MetalBindlessTableImpl>(
        *this, layout_h, group_h, arg_buf, max_tex, desc.max_storage_buffers);

    // Link BindGroupEntry back to MetalBindlessTableImpl so bind_group(set, table->bind_group()) works too.
    {
      std::lock_guard<std::mutex> lock(resource_mutex_);
      if (BindGroupEntry* ge = bind_groups_.get(group_h)) {
        ge->bindless_table = table.get();
      }
    }

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
    table->commit_residency_if_needed();
    return table;
  }
}

// -----------------------------------------------------------------------------
// Shader compilation (MSL 3.1 source OR precompiled .metallib) & Pipelines
// -----------------------------------------------------------------------------
id<MTLFunction> MetalDeviceImpl::compile_shader_function(const ShaderModuleDesc& sm,
                                                         MTLSize* out_compute_tg,
                                                         std::string& error) {
  const uint8_t* bytes = nullptr;
  size_t byte_len = 0;
  if (!sm.msl.empty()) {
    bytes = sm.msl.data();
    byte_len = sm.msl.size();
  } else if (!sm.spirv.empty()) {
    bytes = reinterpret_cast<const uint8_t*>(sm.spirv.data());
    byte_len = sm.spirv.size_bytes();
  } else {
    error = "Metal shader payload is empty";
    return nil;
  }

  id<MTLLibrary> lib = nil;
  NSError* ns_err = nil;

  // Detect precompiled .metallib ("MTLB" magic) vs MSL 3.1 UTF-8 source text.
  const bool is_metallib =
      (byte_len >= 4 && bytes[0] == 'M' && bytes[1] == 'T' && bytes[2] == 'L' && bytes[3] == 'B');
  if (is_metallib) {
    dispatch_data_t data =
        dispatch_data_create(bytes, byte_len, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    lib = [device_ newLibraryWithData:data error:&ns_err];
    if (!lib) {
      error = ns_err_str("MTLDevice newLibraryWithData(.metallib)", ns_err);
      return nil;
    }
  } else {
    while (byte_len > 0 && bytes[byte_len - 1] == '\0') --byte_len;
    if (byte_len == 0) {
      error = "Metal shader MSL text is empty";
      return nil;
    }
    if (out_compute_tg) {
      parse_local_size_hint(reinterpret_cast<const char*>(bytes), byte_len, *out_compute_tg);
    }
    NSString* src = [[NSString alloc] initWithBytes:bytes
                                             length:byte_len
                                           encoding:NSUTF8StringEncoding];
    if (!src) {
      error = "Metal shader payload is neither valid .metallib nor UTF-8 MSL source";
      return nil;
    }
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.languageVersion = MTLLanguageVersion3_1;
    opts.mathMode = MTLMathModeFast;
    lib = [device_ newLibraryWithSource:src options:opts error:&ns_err];
    if (!lib) {
      error = ns_err_str("MTLDevice newLibraryWithSource(MSL 3.1)", ns_err);
      return nil;
    }
  }

  const char* entry = (sm.entry_point && *sm.entry_point) ? sm.entry_point : "main";
  NSString* ns_entry = [NSString stringWithUTF8String:entry];

  // Resolve function name (try exact entry_point, then "main0" SPIRV-Cross convention, then sole function).
  id<MTLFunction> base_fn = [lib newFunctionWithName:ns_entry];
  if (!base_fn && std::strcmp(entry, "main") == 0) {
    ns_entry = @"main0";
    base_fn = [lib newFunctionWithName:ns_entry];
  }
  if (!base_fn) {
    NSArray<NSString*>* names = [lib functionNames];
    if ([names count] == 1) {
      ns_entry = names[0];
      base_fn = [lib newFunctionWithName:ns_entry];
    }
  }
  if (!base_fn) {
    error = std::string("Metal library does not contain function '") + entry + "'";
    return nil;
  }

  if (sm.spec_constants.empty()) {
    return base_fn;
  }

  // Apply specialization constants via MTLFunctionConstantValues, matching exact declared MTLDataType.
  MTLFunctionConstantValues* fcv = [MTLFunctionConstantValues new];
  NSDictionary<NSString*, MTLFunctionConstant*>* fc_dict = [base_fn functionConstantsDictionary];
  for (const SpecConstantEntry& sc : sm.spec_constants) {
    MTLDataType target_type = MTLDataTypeUInt;
    for (MTLFunctionConstant* fc in [fc_dict allValues]) {
      if (static_cast<uint32_t>([fc index]) == sc.constant_id) {
        target_type = [fc type];
        break;
      }
    }
    if (target_type == MTLDataTypeBool) {
      bool bval = (sc.value != 0u);
      [fcv setConstantValue:&bval type:MTLDataTypeBool atIndex:sc.constant_id];
    } else if (target_type == MTLDataTypeFloat) {
      float fval = 0.0f;
      std::memcpy(&fval, &sc.value, sizeof(float));
      [fcv setConstantValue:&fval type:MTLDataTypeFloat atIndex:sc.constant_id];
    } else if (target_type == MTLDataTypeInt) {
      int32_t ival = static_cast<int32_t>(sc.value);
      [fcv setConstantValue:&ival type:MTLDataTypeInt atIndex:sc.constant_id];
    } else {
      uint32_t uval = sc.value;
      [fcv setConstantValue:&uval type:MTLDataTypeUInt atIndex:sc.constant_id];
    }
  }

  id<MTLFunction> spec_fn = [lib newFunctionWithName:ns_entry constantValues:fcv error:&ns_err];
  if (!spec_fn) {
    error = ns_err_str("MTLLibrary newFunctionWithName:constantValues", ns_err);
    return nil;
  }
  return spec_fn;
}

PipelineLayoutHandle MetalDeviceImpl::create_pipeline_layout(const PipelineLayoutDesc& desc,
                                                             std::string& error) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  for (BindGroupLayoutHandle h : desc.bind_group_layouts) {
    if (!bind_group_layouts_.get(h)) {
      error = "create_pipeline_layout: invalid BindGroupLayoutHandle";
      return {};
    }
  }
  PipelineLayoutEntry entry{};
  entry.bind_group_layouts.assign(desc.bind_group_layouts.begin(), desc.bind_group_layouts.end());
  entry.push_constants.assign(desc.push_constants.begin(), desc.push_constants.end());
  return pipeline_layouts_.allocate(std::move(entry));
}

void MetalDeviceImpl::destroy_pipeline_layout(PipelineLayoutHandle handle) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  pipeline_layouts_.remove(handle);
}

PipelineHandle MetalDeviceImpl::create_graphics_pipeline(const GraphicsPipelineDesc& desc,
                                                         std::string& error) {
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!pipeline_layouts_.get(desc.layout)) {
      error = "create_graphics_pipeline: invalid PipelineLayoutHandle";
      return {};
    }
  }

  @autoreleasepool {
    id<MTLFunction> vert_fn = compile_shader_function(desc.vertex_shader, nullptr, error);
    if (!vert_fn) return {};
    id<MTLFunction> frag_fn = nil;
    if (desc.fragment_shader.has_payload()) {
      frag_fn = compile_shader_function(desc.fragment_shader, nullptr, error);
      if (!frag_fn) return {};
    }

    MTLRenderPipelineDescriptor* rpd = [MTLRenderPipelineDescriptor new];
    rpd.vertexFunction = vert_fn;
    rpd.fragmentFunction = frag_fn;
    rpd.rasterSampleCount = std::max(desc.raster.sample_count, 1u);
    rpd.alphaToCoverageEnabled = desc.raster.alpha_to_coverage ? YES : NO;
    rpd.supportIndirectCommandBuffers = caps_.indirect_count ? YES : NO;

    // Vertex descriptor (mapped to buffer indices kVertexBufferBaseIndex + binding)
    if (!desc.vertex_bindings.empty() || !desc.vertex_attributes.empty()) {
      MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
      for (const VertexBindingDesc& vb : desc.vertex_bindings) {
        const NSUInteger slot = kVertexBufferBaseIndex + vb.binding;
        vd.layouts[slot].stride = vb.stride;
        vd.layouts[slot].stepFunction =
            vb.per_instance ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
        vd.layouts[slot].stepRate = 1;
      }
      for (const VertexAttributeDesc& va : desc.vertex_attributes) {
        vd.attributes[va.location].format = to_mtl_vertex_format(va.format);
        vd.attributes[va.location].offset = va.offset;
        vd.attributes[va.location].bufferIndex = kVertexBufferBaseIndex + va.binding;
      }
      rpd.vertexDescriptor = vd;
    }

    // Color attachments & blend states
    for (size_t i = 0; i < desc.color_formats.size(); ++i) {
      const MTLPixelFormat cf = to_mtl_pixel_format(desc.color_formats[i], supports_bc_textures_);
      if (cf == MTLPixelFormatInvalid) {
        error = "create_graphics_pipeline: unsupported color attachment format";
        return {};
      }
      MTLRenderPipelineColorAttachmentDescriptor* att = rpd.colorAttachments[i];
      att.pixelFormat = cf;
      const BlendState bs = (i < desc.color_blends.size()) ? desc.color_blends[i] : BlendState::opaque();
      att.blendingEnabled = bs.enabled ? YES : NO;
      att.sourceRGBBlendFactor = to_mtl_blend_factor(bs.src_color);
      att.destinationRGBBlendFactor = to_mtl_blend_factor(bs.dst_color);
      att.rgbBlendOperation = to_mtl_blend_op(bs.color_op);
      att.sourceAlphaBlendFactor = to_mtl_blend_factor(bs.src_alpha);
      att.destinationAlphaBlendFactor = to_mtl_blend_factor(bs.dst_alpha);
      att.alphaBlendOperation = to_mtl_blend_op(bs.alpha_op);
      att.writeMask = to_mtl_write_mask(bs.write_mask);
    }

    if (desc.depth_format != Format::Undefined) {
      rpd.depthAttachmentPixelFormat = to_mtl_pixel_format(desc.depth_format, supports_bc_textures_);
    }

    if (binary_archive_) {
      rpd.binaryArchives = @[ binary_archive_ ];
    }

    NSError* ns_err = nil;
    id<MTLRenderPipelineState> pso = [device_ newRenderPipelineStateWithDescriptor:rpd error:&ns_err];
    if (!pso) {
      error = ns_err_str("MTLDevice newRenderPipelineStateWithDescriptor", ns_err);
      return {};
    }
    if (binary_archive_) {
      NSError* arch_err = nil;
      if ([binary_archive_ addRenderPipelineFunctionsWithDescriptor:rpd error:&arch_err]) {
        pipeline_cache_dirty_ = true;
      }
    }

    MTLDepthStencilDescriptor* dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction =
        desc.depth.test_enabled ? to_mtl_compare_func(desc.depth.compare_op) : MTLCompareFunctionAlways;
    dsd.depthWriteEnabled = (desc.depth.test_enabled && desc.depth.write_enabled) ? YES : NO;
    id<MTLDepthStencilState> ds_state = [device_ newDepthStencilStateWithDescriptor:dsd];

    PipelineEntry entry{};
    entry.is_compute = false;
    entry.layout = desc.layout;
    entry.render_pso = pso;
    entry.depth_stencil = ds_state;
    entry.cull_mode = to_mtl_cull_mode(desc.raster.cull_mode);
    entry.front_face = to_mtl_front_face(desc.raster.front_face);
    entry.primitive_type = to_mtl_primitive_type(desc.raster.topology);
    entry.depth_clip_mode = desc.raster.depth_clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip;
    entry.depth_bias_enabled = desc.raster.depth_bias_enabled || desc.raster.dynamic_depth_bias;

    std::lock_guard<std::mutex> lock(resource_mutex_);
    return pipelines_.allocate(std::move(entry));
  }
}

PipelineHandle MetalDeviceImpl::create_compute_pipeline(const ComputePipelineDesc& desc,
                                                        std::string& error) {
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!pipeline_layouts_.get(desc.layout)) {
      error = "create_compute_pipeline: invalid PipelineLayoutHandle";
      return {};
    }
  }

  @autoreleasepool {
    MTLSize hint_tg = MTLSizeMake(0, 0, 0);
    id<MTLFunction> comp_fn = compile_shader_function(desc.compute_shader, &hint_tg, error);
    if (!comp_fn) return {};

    MTLComputePipelineDescriptor* cpd = [MTLComputePipelineDescriptor new];
    cpd.computeFunction = comp_fn;
    if (binary_archive_) {
      cpd.binaryArchives = @[ binary_archive_ ];
    }

    NSError* ns_err = nil;
    id<MTLComputePipelineState> pso =
        [device_ newComputePipelineStateWithDescriptor:cpd
                                               options:MTLPipelineOptionNone
                                            reflection:nil
                                                 error:&ns_err];
    if (!pso) {
      error = ns_err_str("MTLDevice newComputePipelineStateWithDescriptor", ns_err);
      return {};
    }
    if (binary_archive_) {
      NSError* arch_err = nil;
      if ([binary_archive_ addComputePipelineFunctionsWithDescriptor:cpd error:&arch_err]) {
        pipeline_cache_dirty_ = true;
      }
    }

    // Determine threadgroup size: explicit shift_local_size(x,y,z) hint, else shader
    // [[max_total_threads_per_threadgroup(N)]] / default <= 64.
    MTLSize tg = hint_tg;
    if (tg.width == 0 || tg.height == 0 || tg.depth == 0) {
      const NSUInteger max_t = std::max<NSUInteger>(1, [pso maxTotalThreadsPerThreadgroup]);
      tg = MTLSizeMake(std::min<NSUInteger>(max_t, 64), 1, 1);
    }

    PipelineEntry entry{};
    entry.is_compute = true;
    entry.layout = desc.layout;
    entry.compute_pso = pso;
    entry.compute_threads_per_group = tg;

    std::lock_guard<std::mutex> lock(resource_mutex_);
    return pipelines_.allocate(std::move(entry));
  }
}

void MetalDeviceImpl::destroy_pipeline(PipelineHandle handle) {
  PipelineEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!pipelines_.remove(handle, &entry)) return;
  }
  entry.render_pso = nil;
  entry.depth_stencil = nil;
  entry.compute_pso = nil;
}

// -----------------------------------------------------------------------------
// Query Pools (GPU Timestamps via MTLCounterSampleBuffer + RT Compacted Size)
// -----------------------------------------------------------------------------
QueryPoolHandle MetalDeviceImpl::create_query_pool(const QueryPoolDesc& desc, std::string& error) {
  if (desc.count == 0) {
    error = "create_query_pool: count must be > 0";
    return {};
  }

  @autoreleasepool {
    QueryPoolEntry entry{};
    entry.type = desc.type;
    entry.count = desc.count;

    if (desc.type == QueryType::AccelStructCompactedSize) {
      if (!caps_.ray_query) {
        error = "create_query_pool: AccelStructCompactedSize requires Metal ray tracing";
        return {};
      }
      id<MTLBuffer> buf = [device_ newBufferWithLength:static_cast<NSUInteger>(desc.count) * sizeof(uint64_t)
                                               options:MTLResourceStorageModeShared];
      if (!buf) {
        error = "create_query_pool: failed to allocate compacted-size query buffer";
        return {};
      }
      std::memset([buf contents], 0, desc.count * sizeof(uint64_t));
      entry.result_buffer = buf;
    } else {
      if (!caps_.timestamps || !timestamp_counter_set_) {
        error = "create_query_pool: GPU timestamp counter sampling is not supported";
        return {};
      }
      MTLCounterSampleBufferDescriptor* csd = [MTLCounterSampleBufferDescriptor new];
      csd.counterSet = timestamp_counter_set_;
      csd.sampleCount = desc.count;
      csd.storageMode = MTLStorageModeShared;
      csd.label = @"shift_rhi_timestamps";
      NSError* ns_err = nil;
      id<MTLCounterSampleBuffer> csb =
          [device_ newCounterSampleBufferWithDescriptor:csd error:&ns_err];
      if (!csb) {
        error = ns_err_str("MTLDevice newCounterSampleBufferWithDescriptor", ns_err);
        return {};
      }
      entry.counter_buffer = csb;
    }

    std::lock_guard<std::mutex> lock(resource_mutex_);
    return query_pools_.allocate(std::move(entry));
  }
}

void MetalDeviceImpl::destroy_query_pool(QueryPoolHandle handle) {
  QueryPoolEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!query_pools_.remove(handle, &entry)) return;
  }
  entry.counter_buffer = nil;
  entry.result_buffer = nil;
}

bool MetalDeviceImpl::get_query_pool_results(QueryPoolHandle pool, uint32_t first, uint32_t count,
                                             std::span<uint64_t> out_values, bool /*wait*/) {
  QueryPoolEntry qe{};
  if (!lookup_query_pool_copy(pool, qe)) return false;
  if (count == 0 || first + count > qe.count || out_values.size() < count) return false;

  @autoreleasepool {
    if (qe.type == QueryType::AccelStructCompactedSize) {
      if (!qe.result_buffer) return false;
      const auto* ptr = static_cast<const uint64_t*>([qe.result_buffer contents]);
      for (uint32_t i = 0; i < count; ++i) out_values[i] = ptr[first + i];
      return true;
    }

    if (!qe.counter_buffer) return false;
    NSData* data = [qe.counter_buffer resolveCounterRange:NSMakeRange(first, count)];
    if (!data || [data length] < count * sizeof(MTLCounterResultTimestamp)) return false;
    const auto* res = static_cast<const MTLCounterResultTimestamp*>([data bytes]);
    for (uint32_t i = 0; i < count; ++i) {
      out_values[i] = res[i].timestamp;
    }
    return true;
  }
}

// -----------------------------------------------------------------------------
// Internal blit pipeline cache for scaled/filtered cmd.blit()
// -----------------------------------------------------------------------------
id<MTLRenderPipelineState> MetalDeviceImpl::get_or_create_blit_pipeline(MTLPixelFormat dst_fmt) {
  std::lock_guard<std::mutex> lock(blit_mutex_);
  const uint32_t key = static_cast<uint32_t>(dst_fmt);
  auto it = blit_pipelines_.find(key);
  if (it != blit_pipelines_.end()) return it->second;

  @autoreleasepool {
    NSError* ns_err = nil;
    if (!blit_lib_) {
      MTLCompileOptions* opts = [MTLCompileOptions new];
      opts.languageVersion = MTLLanguageVersion3_1;
      blit_lib_ = [device_ newLibraryWithSource:[NSString stringWithUTF8String:kInternalBlitMsl]
                                        options:opts
                                          error:&ns_err];
      if (!blit_lib_) return nil;
    }
    MTLRenderPipelineDescriptor* rpd = [MTLRenderPipelineDescriptor new];
    rpd.vertexFunction = [blit_lib_ newFunctionWithName:@"shift_blit_vs"];
    rpd.fragmentFunction = [blit_lib_ newFunctionWithName:@"shift_blit_fs"];
    rpd.colorAttachments[0].pixelFormat = dst_fmt;
    id<MTLRenderPipelineState> pso = [device_ newRenderPipelineStateWithDescriptor:rpd error:&ns_err];
    if (!pso) return nil;
    blit_pipelines_[key] = pso;
    return pso;
  }
}

id<MTLSamplerState> MetalDeviceImpl::get_or_create_blit_sampler(Filter filter) {
  std::lock_guard<std::mutex> lock(blit_mutex_);
  if (filter == Filter::Nearest) {
    if (!blit_sampler_nearest_) {
      MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
      sd.minFilter = MTLSamplerMinMagFilterNearest;
      sd.magFilter = MTLSamplerMinMagFilterNearest;
      sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
      sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
      blit_sampler_nearest_ = [device_ newSamplerStateWithDescriptor:sd];
    }
    return blit_sampler_nearest_;
  }
  if (!blit_sampler_linear_) {
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    blit_sampler_linear_ = [device_ newSamplerStateWithDescriptor:sd];
  }
  return blit_sampler_linear_;
}

id<MTLIndirectCommandBuffer> MetalDeviceImpl::allocate_icb(uint32_t max_draws) {
  if (!device_ || max_draws == 0) return nil;
  MTLIndirectCommandBufferDescriptor* desc = [MTLIndirectCommandBufferDescriptor new];
  desc.commandTypes = MTLIndirectCommandTypeDrawIndexed;
  desc.inheritBuffers = YES;
  desc.inheritPipelineState = YES;
  desc.maxVertexBufferBindCount = 0;
  desc.maxFragmentBufferBindCount = 0;
  return [device_ newIndirectCommandBufferWithDescriptor:desc
                                         maxCommandCount:max_draws
                                                 options:MTLResourceStorageModePrivate];
}

id<MTLComputePipelineState> MetalDeviceImpl::get_or_create_icb_encode_pipeline() {
  std::lock_guard<std::mutex> lock(blit_mutex_);
  if (icb_encode_pso_) return icb_encode_pso_;
  static NSString* const kIcbMsl = @R"(
#include <metal_stdlib>
using namespace metal;
struct DrawIndexedArgs {
  uint index_count;
  uint instance_count;
  uint first_index;
  int base_vertex;
  uint base_instance;
};
struct IcbArg {
  command_buffer cmd_buf [[id(0)]];
};
struct IcbParams {
  uint max_draws;
  uint stride_words;
  uint is_u16;
  uint index_base_bytes;
};
kernel void shift_encode_icb_indexed(constant IcbArg& icb [[buffer(0)]],
                                     device const uint* raw_args [[buffer(1)]],
                                     device const uint* count_buf [[buffer(2)]],
                                     device const uint* idx32 [[buffer(3)]],
                                     device const ushort* idx16 [[buffer(4)]],
                                     constant IcbParams& pc [[buffer(5)]],
                                     uint tid [[thread_position_in_grid]]) {
  if (tid >= pc.max_draws) return;
  render_command cmd(icb.cmd_buf, tid);
  const uint active_count = min(count_buf[0], pc.max_draws);
  if (tid >= active_count) {
    cmd.reset();
    return;
  }
  const uint off = tid * pc.stride_words;
  const uint index_count = raw_args[off + 0u];
  const uint instance_count = raw_args[off + 1u];
  const uint first_index = raw_args[off + 2u];
  const int base_vertex = as_type<int>(raw_args[off + 3u]);
  const uint base_instance = raw_args[off + 4u];
  if (index_count == 0u || instance_count == 0u) {
    cmd.reset();
    return;
  }
  if (pc.is_u16 != 0u) {
    cmd.draw_indexed_primitives(primitive_type::triangle, index_count,
                                idx16 + (pc.index_base_bytes >> 1u) + first_index,
                                instance_count, base_vertex, base_instance);
  } else {
    cmd.draw_indexed_primitives(primitive_type::triangle, index_count,
                                idx32 + (pc.index_base_bytes >> 2u) + first_index,
                                instance_count, base_vertex, base_instance);
  }
}
)";
  NSError* err = nil;
  id<MTLLibrary> lib = [device_ newLibraryWithSource:kIcbMsl options:nil error:&err];
  if (!lib) return nil;
  id<MTLFunction> fn = [lib newFunctionWithName:@"shift_encode_icb_indexed"];
  if (!fn) return nil;
  icb_encode_pso_ = [device_ newComputePipelineStateWithFunction:fn error:&err];
  return icb_encode_pso_;
}

// -----------------------------------------------------------------------------
// Timeline Semaphores (MTLSharedEvent) & Multi-Queue Batch Submission
// -----------------------------------------------------------------------------
TimelineSemaphoreHandle MetalDeviceImpl::create_timeline_semaphore(uint64_t initial_value,
                                                                   std::string& error) {
  @autoreleasepool {
    id<MTLSharedEvent> ev = [device_ newSharedEvent];
    if (!ev) {
      error = "create_timeline_semaphore: MTLDevice newSharedEvent returned nil";
      return {};
    }
    ev.signaledValue = initial_value;
    TimelineSemaphoreEntry entry{};
    entry.event = ev;
    std::lock_guard<std::mutex> lock(resource_mutex_);
    return timeline_semaphores_.allocate(std::move(entry));
  }
}

void MetalDeviceImpl::destroy_timeline_semaphore(TimelineSemaphoreHandle handle) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  timeline_semaphores_.remove(handle);
}

uint64_t MetalDeviceImpl::timeline_semaphore_value(TimelineSemaphoreHandle handle) const {
  id<MTLSharedEvent> ev = nil;
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const TimelineSemaphoreEntry* e = timeline_semaphores_.get(handle);
    if (!e || !e->event) return 0ull;
    ev = e->event;
  }
  return static_cast<uint64_t>(ev.signaledValue);
}

bool MetalDeviceImpl::wait_timeline_semaphore(TimelineSemaphoreHandle handle, uint64_t value,
                                              uint64_t timeout_ns) {
  id<MTLSharedEvent> ev = nil;
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const TimelineSemaphoreEntry* e = timeline_semaphores_.get(handle);
    if (!e || !e->event) return false;
    ev = e->event;
  }
  if (ev.signaledValue >= value) return true;
  if (!shared_event_listener_) return false;
  @autoreleasepool {
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [ev notifyListener:shared_event_listener_
               atValue:value
                 block:^(id<MTLSharedEvent>, uint64_t) {
                   dispatch_semaphore_signal(sem);
                 }];
    const dispatch_time_t wait_t =
        (timeout_ns == UINT64_MAX) ? DISPATCH_TIME_FOREVER
                                   : dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(timeout_ns));
    return dispatch_semaphore_wait(sem, wait_t) == 0;
  }
}

bool MetalDeviceImpl::signal_timeline_semaphore(TimelineSemaphoreHandle handle, uint64_t value) {
  id<MTLSharedEvent> ev = nil;
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const TimelineSemaphoreEntry* e = timeline_semaphores_.get(handle);
    if (!e || !e->event) return false;
    ev = e->event;
  }
  ev.signaledValue = value;
  return true;
}

std::unique_ptr<CommandList> MetalDeviceImpl::create_command_list(QueueKind queue,
                                                                  std::string& error) {
  if (!mtl_queue(queue)) {
    error = "create_command_list: Metal command queue is null";
    return nullptr;
  }
  return std::make_unique<MetalCommandListImpl>(*this, nil, queue, true);
}

bool MetalDeviceImpl::submit(QueueKind queue,
                             std::span<CommandList* const> lists,
                             std::span<const TimelineWait> waits,
                             std::span<const TimelineSignal> signals,
                             std::string& error) {
  id<MTLCommandQueue> target_queue = mtl_queue(queue);
  if (!target_queue) {
    error = "submit: Metal command queue is null";
    return false;
  }

  std::vector<std::pair<id<MTLSharedEvent>, uint64_t>> wait_events;
  wait_events.reserve(waits.size());
  std::vector<std::pair<id<MTLSharedEvent>, uint64_t>> signal_events;
  signal_events.reserve(signals.size());
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    for (const TimelineWait& w : waits) {
      const TimelineSemaphoreEntry* e = timeline_semaphores_.get(w.semaphore);
      if (!e || !e->event) {
        error = "submit: invalid TimelineWait semaphore handle";
        return false;
      }
      wait_events.emplace_back(e->event, w.value);
    }
    for (const TimelineSignal& s : signals) {
      const TimelineSemaphoreEntry* e = timeline_semaphores_.get(s.semaphore);
      if (!e || !e->event) {
        error = "submit: invalid TimelineSignal semaphore handle";
        return false;
      }
      signal_events.emplace_back(e->event, s.value);
    }
  }

  @autoreleasepool {
    // Encode waits on a dedicated leading command buffer so they execute strictly
    // before any encoders already recorded on the submitted command buffers.
    if (!wait_events.empty()) {
      id<MTLCommandBuffer> wait_cb = [target_queue commandBuffer];
      if (!wait_cb) {
        error = "submit: failed to allocate Metal wait command buffer";
        return false;
      }
      for (const auto& [ev, val] : wait_events) {
        [wait_cb encodeWaitForEvent:ev value:val];
      }
      [wait_cb commit];
    }

    id<MTLCommandBuffer> last_cb = nil;
    for (CommandList* cl : lists) {
      if (!cl) continue;
      auto* mcl = static_cast<MetalCommandListImpl*>(cl);
      mcl->end_active_encoder();
      id<MTLCommandBuffer> cb = mcl->native_cmd_buf();
      if (!cb) continue;
      if (last_cb) [last_cb commit];
      last_cb = cb;
    }

    if (last_cb) {
      for (const auto& [ev, val] : signal_events) {
        [last_cb encodeSignalEvent:ev value:val];
      }
      [last_cb commit];
    } else if (!signal_events.empty()) {
      id<MTLCommandBuffer> sig_cb = [target_queue commandBuffer];
      if (!sig_cb) {
        error = "submit: failed to allocate Metal signal command buffer";
        return false;
      }
      for (const auto& [ev, val] : signal_events) {
        [sig_cb encodeSignalEvent:ev value:val];
      }
      [sig_cb commit];
    }
  }
  return true;
}

// -----------------------------------------------------------------------------
// Immediate submission (submit_once) & Swapchain factory
// -----------------------------------------------------------------------------
bool MetalDeviceImpl::submit_once(const std::function<void(CommandList&)>& record_fn,
                                  std::string& error, bool prefer_transfer_queue) {
  const QueueKind qk = prefer_transfer_queue ? QueueKind::Transfer : QueueKind::Graphics;
  id<MTLCommandQueue> target_queue = mtl_queue(qk);
  if (!target_queue) {
    error = "submit_once: Metal command queue is null";
    return false;
  }
  @autoreleasepool {
    MTLCommandBufferDescriptor* cb_desc = [MTLCommandBufferDescriptor new];
    cb_desc.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
    id<MTLCommandBuffer> cb = [target_queue commandBufferWithDescriptor:cb_desc];
    if (!cb) {
      error = "submit_once: MTLCommandQueue commandBuffer returned nil";
      return false;
    }

    MetalCommandListImpl cmd_list(*this, cb, qk, false);
    cmd_list.begin();
    record_fn(cmd_list);
    cmd_list.end();

    [cb commit];
    [cb waitUntilCompleted];
    cmd_list.release_transients();

    if ([cb status] == MTLCommandBufferStatusError) {
      error = ns_err_str("MTLCommandBuffer submit_once", [cb error]);
      NSArray<id<MTLCommandBufferEncoderInfo>>* infos =
          [[cb error] userInfo][MTLCommandBufferEncoderInfoErrorKey];
      for (id<MTLCommandBufferEncoderInfo> info in infos) {
        std::fprintf(stderr, "[rhi:metal] encoder '%s' status=%ld\n",
                     info.label ? [info.label UTF8String] : "(unnamed)",
                     static_cast<long>(info.errorState));
      }
      std::fprintf(stderr, "[rhi:metal] %s\n", error.c_str());
      record_validation_message();
      return false;
    }
    return true;
  }
}

std::unique_ptr<Swapchain> MetalDeviceImpl::create_swapchain(const SwapchainDesc& desc,
                                                             std::string& error) {
  auto sw = std::make_unique<MetalSwapchainImpl>(*this);
  if (!sw->init(desc, error)) return nullptr;
  return sw;
}

}  // namespace mtl

// -----------------------------------------------------------------------------
// Entry points called by rhi_factory.cpp
// -----------------------------------------------------------------------------
std::unique_ptr<Device> create_metal_device(const DeviceDesc& desc, std::string& error) {
  auto dev = std::make_unique<mtl::MetalDeviceImpl>();
  if (!dev->init(desc, error)) return nullptr;
  return dev;
}

void enumerate_metal_adapters(std::vector<AdapterInfo>& out) {
  @autoreleasepool {
    auto push_device = [&](id<MTLDevice> dev) {
      if (!dev) return;
      AdapterInfo info{};
      info.backend = BackendKind::Metal;
      const char* name = [[dev name] UTF8String];
      info.name = name ? name : "Apple Metal GPU";
      info.vendor_id = 0x106Bu;
      const uint64_t reg_id = [dev registryID];
      info.device_id = static_cast<uint32_t>((reg_id ^ (reg_id >> 32)) & 0xFFFFFFFFull);
      info.device_local_bytes = [dev recommendedMaxWorkingSetSize];
      info.is_integrated = [dev hasUnifiedMemory];
      info.is_discrete = ![dev hasUnifiedMemory];
      info.is_software = false;
      info.supports_ray_query = [dev supportsRaytracing];
      out.push_back(std::move(info));
    };

#if TARGET_OS_OSX
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    for (id<MTLDevice> dev in devices) {
      push_device(dev);
    }
#else
    push_device(MTLCreateSystemDefaultDevice());
#endif
  }
}

}  // namespace shift::rhi
