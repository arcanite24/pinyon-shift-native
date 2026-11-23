#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4324 4505)
#endif
#define VMA_IMPLEMENTATION
#include "rhi_vk_internal.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#if defined(__APPLE__) || defined(__ANDROID__)
#include <dlfcn.h>
#endif

#ifdef SHIFT_RHI_HAS_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

namespace shift::rhi {
namespace vk {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

std::string vk_err(const char* what, VkResult r) {
  return std::string(what) + " failed (" + std::to_string(static_cast<int>(r)) + ")";
}

bool contains_ci(const std::string& hay, const char* needle) {
  std::string h = hay;
  std::string n = needle;
  for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return h.find(n) != std::string::npos;
}

VKAPI_ATTR VkBool32 VKAPI_CALL rhi_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT,
                                                  const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                  void* user) {
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    if (data && data->pMessage) {
      std::fprintf(stderr, "[rhi:vulkan] %s\n", data->pMessage);
    }
    if (user) static_cast<VkDeviceImpl*>(user)->record_validation_message();
  }
  return VK_FALSE;
}

VkSampleCountFlagBits to_vk_sample_count(uint32_t samples) noexcept {
  switch (samples) {
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    default: return VK_SAMPLE_COUNT_1_BIT;
  }
}

}  // namespace

// -----------------------------------------------------------------------------
// Volk bootstrap (singleton guarded)
// -----------------------------------------------------------------------------
bool ensure_volk_initialized(std::string& error) {
  static std::mutex s_volk_mutex;
  static bool s_initialized = false;
  std::lock_guard<std::mutex> lock(s_volk_mutex);
  if (s_initialized) return true;

  bool volk_ok = false;
#if defined(__ANDROID__)
  if (const char* drv = std::getenv("SHIFT_ANDROID_GPU_DRIVER");
      drv && *drv && std::strcmp(drv, "system") != 0) {
    if (void* lib = dlopen(drv, RTLD_NOW | RTLD_LOCAL)) {
      if (auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"))) {
        volkInitializeCustom(gipa);
        volk_ok = true;
      }
    }
  }
#elif defined(__APPLE__)
  const char* candidates[] = {
      "/opt/homebrew/lib/libvulkan.1.dylib",
      "/opt/homebrew/lib/libMoltenVK.dylib",
      "/usr/local/lib/libvulkan.1.dylib",
  };
  for (const char* c : candidates) {
    if (void* lib = dlopen(c, RTLD_NOW | RTLD_LOCAL)) {
      if (auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"))) {
        volkInitializeCustom(gipa);
        volk_ok = true;
        break;
      }
    }
  }
#endif
  if (!volk_ok) {
    const VkResult r = volkInitialize();
    if (r != VK_SUCCESS) {
      error = "no Vulkan loader found (vulkan-1.dll / libvulkan.so.1 / libMoltenVK.dylib)";
      return false;
    }
  }
  s_initialized = true;
  return true;
}

// -----------------------------------------------------------------------------
// Format & flag translators
// -----------------------------------------------------------------------------
VkFormat to_vk_format(Format fmt) noexcept {
  switch (fmt) {
    case Format::Undefined:    return VK_FORMAT_UNDEFINED;
    case Format::R8Unorm:      return VK_FORMAT_R8_UNORM;
    case Format::Rgba8Unorm:   return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::Rgba8Srgb:    return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::Rgba8Snorm:   return VK_FORMAT_R8G8B8A8_SNORM;
    case Format::Bgra8Unorm:   return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::Bgra8Srgb:    return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::Rgb10A2Unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case Format::Rg16Unorm:    return VK_FORMAT_R16G16_UNORM;
    case Format::Rg16Sfloat:   return VK_FORMAT_R16G16_SFLOAT;
    case Format::Rgba16Sfloat: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::Rgba16Snorm:  return VK_FORMAT_R16G16B16A16_SNORM;
    case Format::Rgba16Sint:   return VK_FORMAT_R16G16B16A16_SINT;
    case Format::R32Sfloat:    return VK_FORMAT_R32_SFLOAT;
    case Format::R32Uint:      return VK_FORMAT_R32_UINT;
    case Format::Rg32Sfloat:   return VK_FORMAT_R32G32_SFLOAT;
    case Format::Rgb32Sfloat:  return VK_FORMAT_R32G32B32_SFLOAT;
    case Format::Rgba32Sfloat: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::Rgba32Uint:   return VK_FORMAT_R32G32B32A32_UINT;
    case Format::D32Sfloat:    return VK_FORMAT_D32_SFLOAT;
    case Format::D16Unorm:     return VK_FORMAT_D16_UNORM;
    case Format::Bc1RgbaUnorm: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case Format::Bc1RgbaSrgb:  return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case Format::Bc2Unorm:     return VK_FORMAT_BC2_UNORM_BLOCK;
    case Format::Bc2Srgb:      return VK_FORMAT_BC2_SRGB_BLOCK;
    case Format::Bc3Unorm:     return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::Bc3Srgb:      return VK_FORMAT_BC3_SRGB_BLOCK;
    case Format::Bc4Unorm:     return VK_FORMAT_BC4_UNORM_BLOCK;
    case Format::Bc5Unorm:     return VK_FORMAT_BC5_UNORM_BLOCK;
    case Format::Count:        return VK_FORMAT_UNDEFINED;
  }
  return VK_FORMAT_UNDEFINED;
}

Format from_vk_format(VkFormat fmt) noexcept {
  switch (fmt) {
    case VK_FORMAT_R8_UNORM:                 return Format::R8Unorm;
    case VK_FORMAT_R8G8B8A8_UNORM:           return Format::Rgba8Unorm;
    case VK_FORMAT_R8G8B8A8_SRGB:            return Format::Rgba8Srgb;
    case VK_FORMAT_R8G8B8A8_SNORM:           return Format::Rgba8Snorm;
    case VK_FORMAT_B8G8R8A8_UNORM:           return Format::Bgra8Unorm;
    case VK_FORMAT_B8G8R8A8_SRGB:            return Format::Bgra8Srgb;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return Format::Rgb10A2Unorm;
    case VK_FORMAT_R16G16_UNORM:             return Format::Rg16Unorm;
    case VK_FORMAT_R16G16_SFLOAT:            return Format::Rg16Sfloat;
    case VK_FORMAT_R16G16B16A16_SFLOAT:      return Format::Rgba16Sfloat;
    case VK_FORMAT_R16G16B16A16_SNORM:       return Format::Rgba16Snorm;
    case VK_FORMAT_R16G16B16A16_SINT:        return Format::Rgba16Sint;
    case VK_FORMAT_R32_SFLOAT:               return Format::R32Sfloat;
    case VK_FORMAT_R32_UINT:                 return Format::R32Uint;
    case VK_FORMAT_R32G32_SFLOAT:            return Format::Rg32Sfloat;
    case VK_FORMAT_R32G32B32_SFLOAT:         return Format::Rgb32Sfloat;
    case VK_FORMAT_R32G32B32A32_SFLOAT:      return Format::Rgba32Sfloat;
    case VK_FORMAT_R32G32B32A32_UINT:        return Format::Rgba32Uint;
    case VK_FORMAT_D32_SFLOAT:               return Format::D32Sfloat;
    case VK_FORMAT_D16_UNORM:                return Format::D16Unorm;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:     return Format::Bc1RgbaUnorm;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:      return Format::Bc1RgbaSrgb;
    case VK_FORMAT_BC2_UNORM_BLOCK:          return Format::Bc2Unorm;
    case VK_FORMAT_BC2_SRGB_BLOCK:           return Format::Bc2Srgb;
    case VK_FORMAT_BC3_UNORM_BLOCK:          return Format::Bc3Unorm;
    case VK_FORMAT_BC3_SRGB_BLOCK:           return Format::Bc3Srgb;
    case VK_FORMAT_BC4_UNORM_BLOCK:          return Format::Bc4Unorm;
    case VK_FORMAT_BC5_UNORM_BLOCK:          return Format::Bc5Unorm;
    default:                                 return Format::Undefined;
  }
}

VkImageType to_vk_image_type(TextureType type) noexcept {
  switch (type) {
    case TextureType::Tex3D: return VK_IMAGE_TYPE_3D;
    case TextureType::TexCube:
    case TextureType::Tex2D:
    default: return VK_IMAGE_TYPE_2D;
  }
}

VkImageViewType to_vk_view_type(TextureType type) noexcept {
  switch (type) {
    case TextureType::TexCube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TextureType::Tex3D:   return VK_IMAGE_VIEW_TYPE_3D;
    case TextureType::Tex2D:
    default:                   return VK_IMAGE_VIEW_TYPE_2D;
  }
}

VkComponentSwizzle to_vk_swizzle(ComponentSwizzle s) noexcept {
  switch (s) {
    case ComponentSwizzle::Zero: return VK_COMPONENT_SWIZZLE_ZERO;
    case ComponentSwizzle::One:  return VK_COMPONENT_SWIZZLE_ONE;
    case ComponentSwizzle::R:    return VK_COMPONENT_SWIZZLE_R;
    case ComponentSwizzle::G:    return VK_COMPONENT_SWIZZLE_G;
    case ComponentSwizzle::B:    return VK_COMPONENT_SWIZZLE_B;
    case ComponentSwizzle::A:    return VK_COMPONENT_SWIZZLE_A;
    case ComponentSwizzle::Identity:
    default:                     return VK_COMPONENT_SWIZZLE_IDENTITY;
  }
}

VkFilter to_vk_filter(Filter f) noexcept {
  return f == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerMipmapMode to_vk_mipmap_mode(Filter f) noexcept {
  return f == Filter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

VkSamplerAddressMode to_vk_address_mode(AddressMode m) noexcept {
  switch (m) {
    case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case AddressMode::ClampToBorder:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case AddressMode::Repeat:
    default:                          return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
}

VkCompareOp to_vk_compare_op(CompareOp op) noexcept {
  switch (op) {
    case CompareOp::Never:          return VK_COMPARE_OP_NEVER;
    case CompareOp::Less:           return VK_COMPARE_OP_LESS;
    case CompareOp::Equal:          return VK_COMPARE_OP_EQUAL;
    case CompareOp::LessOrEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater:        return VK_COMPARE_OP_GREATER;
    case CompareOp::NotEqual:       return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Always:         return VK_COMPARE_OP_ALWAYS;
  }
  return VK_COMPARE_OP_ALWAYS;
}

VkBorderColor to_vk_border_color(BorderColor c) noexcept {
  switch (c) {
    case BorderColor::TransparentBlack: return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    case BorderColor::OpaqueWhite:      return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    case BorderColor::OpaqueBlack:
    default:                            return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  }
}

VkShaderStageFlags to_vk_shader_stages(ShaderStage stages) noexcept {
  VkShaderStageFlags out = 0;
  if (has_flag(stages, ShaderStage::Vertex))   out |= VK_SHADER_STAGE_VERTEX_BIT;
  if (has_flag(stages, ShaderStage::Fragment)) out |= VK_SHADER_STAGE_FRAGMENT_BIT;
  if (has_flag(stages, ShaderStage::Compute))  out |= VK_SHADER_STAGE_COMPUTE_BIT;
  return out;
}

VkDescriptorType to_vk_descriptor_type(DescriptorType t) noexcept {
  switch (t) {
    case DescriptorType::UniformBuffer:          return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case DescriptorType::StorageBuffer:          return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case DescriptorType::SampledTexture:         return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case DescriptorType::StorageTexture:         return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case DescriptorType::Sampler:                return VK_DESCRIPTOR_TYPE_SAMPLER;
    case DescriptorType::CombinedTextureSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case DescriptorType::AccelStruct:            return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  }
  return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

VkBlendFactor to_vk_blend_factor(BlendFactor f) noexcept {
  switch (f) {
    case BlendFactor::Zero:                  return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One:                   return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcColor:              return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::DstColor:              return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::OneMinusDstColor:      return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::SrcAlpha:              return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha:              return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha:      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::ConstantColor:         return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case BlendFactor::OneMinusConstantColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
  }
  return VK_BLEND_FACTOR_ONE;
}

VkBlendOp to_vk_blend_op(BlendOp op) noexcept {
  switch (op) {
    case BlendOp::Add:             return VK_BLEND_OP_ADD;
    case BlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
    case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::Min:             return VK_BLEND_OP_MIN;
    case BlendOp::Max:             return VK_BLEND_OP_MAX;
  }
  return VK_BLEND_OP_ADD;
}

VkCullModeFlags to_vk_cull_mode(CullMode c) noexcept {
  switch (c) {
    case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    case CullMode::Back:  return VK_CULL_MODE_BACK_BIT;
    case CullMode::None:
    default:              return VK_CULL_MODE_NONE;
  }
}

VkFrontFace to_vk_front_face(FrontFace f) noexcept {
  return f == FrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

VkPrimitiveTopology to_vk_topology(PrimitiveTopology t) noexcept {
  switch (t) {
    case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::TriangleList:
    default:                               return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  }
}

VkAttachmentLoadOp to_vk_load_op(LoadOp op) noexcept {
  switch (op) {
    case LoadOp::Load:     return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::Clear:    return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  }
  return VK_ATTACHMENT_LOAD_OP_CLEAR;
}

VkAttachmentStoreOp to_vk_store_op(StoreOp op) noexcept {
  switch (op) {
    case StoreOp::DontCare:           return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    case StoreOp::MultisampleResolve: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    case StoreOp::StoreAndResolve:
    case StoreOp::Store:
    default:                          return VK_ATTACHMENT_STORE_OP_STORE;
  }
}

VkResolveModeFlagBits to_vk_resolve_mode(ResolveMode m) noexcept {
  switch (m) {
    case ResolveMode::Average:    return VK_RESOLVE_MODE_AVERAGE_BIT;
    case ResolveMode::SampleZero: return VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    case ResolveMode::None:
    default:                      return VK_RESOLVE_MODE_NONE;
  }
}

VkPipelineStageFlags2 to_vk_stage2(PipelineStage stages) noexcept {
  VkPipelineStageFlags2 out = VK_PIPELINE_STAGE_2_NONE;
  if (has_flag(stages, PipelineStage::TopOfPipe))          out |= VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
  if (has_flag(stages, PipelineStage::DrawIndirect))       out |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
  if (has_flag(stages, PipelineStage::VertexInput))        out |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
  if (has_flag(stages, PipelineStage::VertexShader))       out |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
  if (has_flag(stages, PipelineStage::FragmentShader))     out |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  if (has_flag(stages, PipelineStage::EarlyFragmentTests)) out |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
  if (has_flag(stages, PipelineStage::LateFragmentTests))  out |= VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  if (has_flag(stages, PipelineStage::ColorAttachmentOut)) out |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  if (has_flag(stages, PipelineStage::ComputeShader))      out |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  if (has_flag(stages, PipelineStage::Transfer))           out |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  if (has_flag(stages, PipelineStage::Copy))               out |= VK_PIPELINE_STAGE_2_COPY_BIT;
  if (has_flag(stages, PipelineStage::Blit))               out |= VK_PIPELINE_STAGE_2_BLIT_BIT;
  if (has_flag(stages, PipelineStage::Clear))              out |= VK_PIPELINE_STAGE_2_CLEAR_BIT;
  if (has_flag(stages, PipelineStage::AccelStructBuild))   out |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  if (has_flag(stages, PipelineStage::AccelStructCopy))    out |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR;
  if (has_flag(stages, PipelineStage::Host))               out |= VK_PIPELINE_STAGE_2_HOST_BIT;
  if (has_flag(stages, PipelineStage::BottomOfPipe))       out |= VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
  if (has_flag(stages, PipelineStage::AllGraphics))        out |= VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
  if (has_flag(stages, PipelineStage::AllCommands))        out |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  return out;
}

VkAccessFlags2 to_vk_access2(AccessFlags access) noexcept {
  VkAccessFlags2 out = VK_ACCESS_2_NONE;
  if (has_flag(access, AccessFlags::IndirectCommandRead))  out |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
  if (has_flag(access, AccessFlags::IndexRead))            out |= VK_ACCESS_2_INDEX_READ_BIT;
  if (has_flag(access, AccessFlags::VertexAttributeRead))  out |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
  if (has_flag(access, AccessFlags::UniformRead))          out |= VK_ACCESS_2_UNIFORM_READ_BIT;
  if (has_flag(access, AccessFlags::ShaderRead))           out |= VK_ACCESS_2_SHADER_READ_BIT;
  if (has_flag(access, AccessFlags::ShaderWrite))          out |= VK_ACCESS_2_SHADER_WRITE_BIT;
  if (has_flag(access, AccessFlags::ColorAttachmentRead))  out |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
  if (has_flag(access, AccessFlags::ColorAttachmentWrite)) out |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  if (has_flag(access, AccessFlags::DepthAttachmentRead))  out |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
  if (has_flag(access, AccessFlags::DepthAttachmentWrite)) out |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  if (has_flag(access, AccessFlags::TransferRead))         out |= VK_ACCESS_2_TRANSFER_READ_BIT;
  if (has_flag(access, AccessFlags::TransferWrite))        out |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
  if (has_flag(access, AccessFlags::HostRead))             out |= VK_ACCESS_2_HOST_READ_BIT;
  if (has_flag(access, AccessFlags::HostWrite))            out |= VK_ACCESS_2_HOST_WRITE_BIT;
  if (has_flag(access, AccessFlags::AccelStructRead))      out |= VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
  if (has_flag(access, AccessFlags::AccelStructWrite))     out |= VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  if (has_flag(access, AccessFlags::MemoryRead))           out |= VK_ACCESS_2_MEMORY_READ_BIT;
  if (has_flag(access, AccessFlags::MemoryWrite))          out |= VK_ACCESS_2_MEMORY_WRITE_BIT;
  return out;
}

VkBufferUsageFlags to_vk_buffer_usage(BufferUsage usage, bool bda_enabled, bool rt_enabled) noexcept {
  VkBufferUsageFlags out = 0;
  if (has_flag(usage, BufferUsage::TransferSrc)) out |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (has_flag(usage, BufferUsage::TransferDst)) out |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (has_flag(usage, BufferUsage::Uniform))     out |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if (has_flag(usage, BufferUsage::Storage))     out |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (has_flag(usage, BufferUsage::Index))       out |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (has_flag(usage, BufferUsage::Vertex))      out |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (has_flag(usage, BufferUsage::Indirect))    out |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  if (bda_enabled && has_flag(usage, BufferUsage::DeviceAddress)) {
    out |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  }
  if (rt_enabled && has_flag(usage, BufferUsage::AccelStructInput)) {
    out |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    if (bda_enabled) out |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  }
  if (rt_enabled && has_flag(usage, BufferUsage::AccelStructStorage)) {
    out |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
    if (bda_enabled) out |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  }
  return out;
}

VkImageUsageFlags to_vk_image_usage(TextureUsage usage) noexcept {
  VkImageUsageFlags out = 0;
  if (has_flag(usage, TextureUsage::TransferSrc))     out |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (has_flag(usage, TextureUsage::TransferDst))     out |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (has_flag(usage, TextureUsage::Sampled))         out |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if (has_flag(usage, TextureUsage::Storage))         out |= VK_IMAGE_USAGE_STORAGE_BIT;
  if (has_flag(usage, TextureUsage::ColorAttachment)) out |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (has_flag(usage, TextureUsage::DepthAttachment)) out |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (has_flag(usage, TextureUsage::Transient))       out |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  return out;
}

VkBuildAccelerationStructureFlagsKHR to_vk_as_build_flags(AccelStructBuildFlags flags) noexcept {
  VkBuildAccelerationStructureFlagsKHR out = 0;
  if (has_flag(flags, AccelStructBuildFlags::PreferFastTrace))
    out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  if (has_flag(flags, AccelStructBuildFlags::PreferFastBuild))
    out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
  if (has_flag(flags, AccelStructBuildFlags::AllowCompaction))
    out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
  if (has_flag(flags, AccelStructBuildFlags::AllowUpdate))
    out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
  return out;
}

VkGeometryFlagsKHR to_vk_geometry_flags(GeometryFlags flags) noexcept {
  VkGeometryFlagsKHR out = 0;
  if (has_flag(flags, GeometryFlags::Opaque))
    out |= VK_GEOMETRY_OPAQUE_BIT_KHR;
  if (has_flag(flags, GeometryFlags::NoDuplicateAnyHitInvocation))
    out |= VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
  return out;
}

StateTransitionInfo translate_resource_state(ResourceState state, bool is_depth, bool is_dst) noexcept {
  switch (state) {
    case ResourceState::Undefined:
      return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
    case ResourceState::Common:
      return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    case ResourceState::VertexBuffer:
      return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
              VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT};
    case ResourceState::IndexBuffer:
      return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
              VK_ACCESS_2_INDEX_READ_BIT};
    case ResourceState::UniformBuffer:
      return {VK_IMAGE_LAYOUT_UNDEFINED,
              VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_UNIFORM_READ_BIT};
    case ResourceState::ShaderRead:
      return {is_depth ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    case ResourceState::StorageReadWrite:
      return {VK_IMAGE_LAYOUT_GENERAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    case ResourceState::RenderTarget:
      return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    case ResourceState::DepthWrite:
      return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case ResourceState::DepthRead:
      return {VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case ResourceState::IndirectArg:
      return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
              VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
    case ResourceState::CopySrc:
      return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
              VK_ACCESS_2_TRANSFER_READ_BIT};
    case ResourceState::CopyDst:
      return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case ResourceState::AccelStructRead:
      return {VK_IMAGE_LAYOUT_UNDEFINED,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
              VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR};
    case ResourceState::AccelStructWrite:
      return {VK_IMAGE_LAYOUT_UNDEFINED,
              VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                  VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR,
              VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR};
    case ResourceState::Present:
      return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
              is_dst ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_NONE};
    case ResourceState::HostRead:
      return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
  }
  return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
}

// -----------------------------------------------------------------------------
// VkBindlessTableImpl
// -----------------------------------------------------------------------------
VkBindlessTableImpl::VkBindlessTableImpl(VkDeviceImpl& device, BindGroupLayoutHandle layout_handle,
                                         BindGroupHandle group_handle, VkDescriptorSet set,
                                         uint32_t max_textures, uint32_t max_storage_buffers)
    : device_(device),
      layout_handle_(layout_handle),
      group_handle_(group_handle),
      set_(set),
      max_textures_(max_textures),
      max_storage_buffers_(max_storage_buffers) {}

VkBindlessTableImpl::~VkBindlessTableImpl() {
  device_.destroy_bind_group(group_handle_);
  device_.destroy_bind_group_layout(layout_handle_);
}

void VkBindlessTableImpl::set_sampler(SamplerHandle sampler) {
  const SamplerEntry* se = device_.lookup_sampler(sampler);
  if (!se || !set_) return;
  VkDescriptorImageInfo info{};
  info.sampler = se->sampler;
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set_;
  write.dstBinding = 0;
  write.dstArrayElement = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  write.pImageInfo = &info;
  vkUpdateDescriptorSets(device_.vk_device(), 1, &write, 0, nullptr);
}

void VkBindlessTableImpl::set_texture(uint32_t slot, TextureViewHandle view) {
  if (slot >= max_textures_ || !set_) return;
  const TextureViewEntry* tv = device_.lookup_texture_view(view);
  if (!tv) return;
  VkDescriptorImageInfo info{};
  info.imageView = tv->view;
  info.imageLayout =
      tv->is_depth ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set_;
  write.dstBinding = 1;
  write.dstArrayElement = slot;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.pImageInfo = &info;
  vkUpdateDescriptorSets(device_.vk_device(), 1, &write, 0, nullptr);
}

void VkBindlessTableImpl::set_storage_buffer(uint32_t slot, BufferHandle buffer,
                                             uint64_t offset, uint64_t size) {
  if (slot >= max_storage_buffers_ || !set_) return;
  const BufferEntry* be = device_.lookup_buffer(buffer);
  if (!be) return;
  VkDescriptorBufferInfo info{};
  info.buffer = be->buffer;
  info.offset = offset;
  info.range = (size == UINT64_MAX) ? VK_WHOLE_SIZE : size;
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set_;
  write.dstBinding = 2;
  write.dstArrayElement = slot;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &info;
  vkUpdateDescriptorSets(device_.vk_device(), 1, &write, 0, nullptr);
}

// -----------------------------------------------------------------------------
// VkDeviceImpl lifecycle
// -----------------------------------------------------------------------------
VkDeviceImpl::~VkDeviceImpl() {
  if (device_) {
    wait_idle();

    // Clean up any remaining live handles in reverse dependency order.
    {
      std::vector<AccelStructHandle> leftover_as;
      accel_structs_.for_each_alive([&](AccelStructHandle h, AccelStructEntry&) { leftover_as.push_back(h); });
      for (AccelStructHandle h : leftover_as) destroy_accel_struct(h);

      std::vector<PipelineHandle> leftover_pipes;
      pipelines_.for_each_alive([&](PipelineHandle h, PipelineEntry&) { leftover_pipes.push_back(h); });
      for (PipelineHandle h : leftover_pipes) destroy_pipeline(h);

      std::vector<PipelineLayoutHandle> leftover_pl;
      pipeline_layouts_.for_each_alive([&](PipelineLayoutHandle h, PipelineLayoutEntry&) { leftover_pl.push_back(h); });
      for (PipelineLayoutHandle h : leftover_pl) destroy_pipeline_layout(h);

      std::vector<BindGroupHandle> leftover_bg;
      bind_groups_.for_each_alive([&](BindGroupHandle h, BindGroupEntry&) { leftover_bg.push_back(h); });
      for (BindGroupHandle h : leftover_bg) destroy_bind_group(h);

      std::vector<BindGroupLayoutHandle> leftover_bgl;
      bind_group_layouts_.for_each_alive([&](BindGroupLayoutHandle h, BindGroupLayoutEntry&) { leftover_bgl.push_back(h); });
      for (BindGroupLayoutHandle h : leftover_bgl) destroy_bind_group_layout(h);

      std::vector<SamplerHandle> leftover_samp;
      samplers_.for_each_alive([&](SamplerHandle h, SamplerEntry&) { leftover_samp.push_back(h); });
      for (SamplerHandle h : leftover_samp) destroy_sampler(h);

      std::vector<TextureViewHandle> leftover_tv;
      texture_views_.for_each_alive([&](TextureViewHandle h, TextureViewEntry& e) {
        if (e.owned) leftover_tv.push_back(h);
      });
      for (TextureViewHandle h : leftover_tv) destroy_texture_view(h);

      std::vector<TextureHandle> leftover_tex;
      textures_.for_each_alive([&](TextureHandle h, TextureEntry& e) {
        if (e.owned) leftover_tex.push_back(h);
      });
      for (TextureHandle h : leftover_tex) destroy_texture(h);

      std::vector<BufferHandle> leftover_buf;
      buffers_.for_each_alive([&](BufferHandle h, BufferEntry&) { leftover_buf.push_back(h); });
      for (BufferHandle h : leftover_buf) destroy_buffer(h);

      std::vector<QueryPoolHandle> leftover_qp;
      query_pools_.for_each_alive([&](QueryPoolHandle h, QueryPoolEntry&) { leftover_qp.push_back(h); });
      for (QueryPoolHandle h : leftover_qp) destroy_query_pool(h);
    }

    std::string cache_err;
    save_pipeline_cache(cache_err);
    if (pipeline_cache_) {
      vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
      pipeline_cache_ = VK_NULL_HANDLE;
    }

    for (VkCommandPool p : all_cmd_pools_) {
      if (p) vkDestroyCommandPool(device_, p, nullptr);
    }
    all_cmd_pools_.clear();
    free_cmd_pools_.clear();

    if (allocator_) {
      vmaDestroyAllocator(allocator_);
      allocator_ = VK_NULL_HANDLE;
    }

    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
  }

  if (instance_) {
    if (probe_surface_) {
      vkDestroySurfaceKHR(instance_, probe_surface_, nullptr);
      probe_surface_ = VK_NULL_HANDLE;
    }
    if (messenger_) {
      vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
      messenger_ = VK_NULL_HANDLE;
    }
    vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }
}

bool VkDeviceImpl::init(const DeviceDesc& desc, std::string& error) {
  if (!ensure_volk_initialized(error)) return false;

  const uint32_t loader_ver = volkGetInstanceVersion();
#if defined(__ANDROID__) || defined(__APPLE__)
  if (loader_ver < VK_API_VERSION_1_1) {
    error = "Vulkan loader is older than 1.1";
    return false;
  }
#else
  if (loader_ver < VK_API_VERSION_1_3) {
    error = "Vulkan loader is older than 1.3";
    return false;
  }
#endif

  pipeline_cache_path_ = desc.pipeline_cache_path;
  if (!create_instance(desc, error)) return false;
  if (!pick_physical_device(desc, error)) return false;
  if (!create_logical_device(desc, error)) return false;
  return true;
}

bool VkDeviceImpl::create_instance(const DeviceDesc& desc, std::string& error) {
  std::vector<const char*> extensions;
  uint32_t avail_ext_count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &avail_ext_count, nullptr);
  std::vector<VkExtensionProperties> avail_exts(avail_ext_count);
  if (avail_ext_count > 0) {
    vkEnumerateInstanceExtensionProperties(nullptr, &avail_ext_count, avail_exts.data());
  }
  auto has_inst_ext = [&](const char* name) {
    return std::any_of(avail_exts.begin(), avail_exts.end(), [&](const VkExtensionProperties& e) {
      return std::strcmp(e.extensionName, name) == 0;
    });
  };
  auto push_ext_unique = [&](const char* name) {
    if (std::none_of(extensions.begin(), extensions.end(), [&](const char* n) {
          return std::strcmp(n, name) == 0;
        })) {
      extensions.push_back(name);
    }
  };

#ifdef SHIFT_RHI_HAS_SDL3
  if (desc.sdl_window) {
    Uint32 sdl_ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    if (!sdl_exts) {
      error = std::string("SDL_Vulkan_GetInstanceExtensions: ") + SDL_GetError();
      return false;
    }
    for (Uint32 i = 0; i < sdl_ext_count; ++i) push_ext_unique(sdl_exts[i]);
    if (has_inst_ext(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
      push_ext_unique(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
      hdr_colorspace_ext_ = true;
    }
  }
#endif

  std::vector<const char*> layers;
  validation_enabled_ = false;
  if (desc.enable_validation) {
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> avail_layers(layer_count);
    if (layer_count > 0) {
      vkEnumerateInstanceLayerProperties(&layer_count, avail_layers.data());
    }
    const bool has_val = std::any_of(avail_layers.begin(), avail_layers.end(), [](const VkLayerProperties& l) {
      return std::strcmp(l.layerName, kValidationLayer) == 0;
    });
    if (has_val && has_inst_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
      layers.push_back(kValidationLayer);
      push_ext_unique(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
      validation_enabled_ = true;
    }
  }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = desc.app_name ? desc.app_name : "shift-engine";
  app.pEngineName = "shift-engine";
  app.apiVersion = volkGetInstanceVersion() >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;

  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &app;
  if (has_inst_ext(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
    push_ext_unique(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  } else {
    extensions.erase(
        std::remove_if(extensions.begin(), extensions.end(), [](const char* n) {
          return std::strcmp(n, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0;
        }),
        extensions.end());
  }
  info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  info.ppEnabledExtensionNames = extensions.data();
  info.enabledLayerCount = static_cast<uint32_t>(layers.size());
  info.ppEnabledLayerNames = layers.data();

  const VkResult r = vkCreateInstance(&info, nullptr, &instance_);
  if (r != VK_SUCCESS) {
    error = vk_err("vkCreateInstance", r);
    return false;
  }
  volkLoadInstance(instance_);

  if (validation_enabled_ && vkCreateDebugUtilsMessengerEXT) {
    VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mci.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mci.pfnUserCallback = rhi_debug_callback;
    mci.pUserData = this;
    vkCreateDebugUtilsMessengerEXT(instance_, &mci, nullptr, &messenger_);
  }

#ifdef SHIFT_RHI_HAS_SDL3
  if (desc.sdl_window) {
    if (!SDL_Vulkan_CreateSurface(desc.sdl_window, instance_, nullptr, &probe_surface_)) {
      error = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
      return false;
    }
  }
#endif
  return true;
}

bool VkDeviceImpl::pick_physical_device(const DeviceDesc&, std::string& error) {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  if (count == 0) {
    error = "no Vulkan physical devices found";
    return false;
  }
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance_, &count, devices.data());

  int best_score = -1;
  for (VkPhysicalDevice candidate : devices) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(candidate, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) continue;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(candidate, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2) continue;

    uint32_t fam_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &fam_count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(fam_count);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &fam_count, fams.data());

    for (uint32_t i = 0; i < fam_count; ++i) {
      if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !(fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        continue;
      }
      if (probe_surface_) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, probe_surface_, &present);
        if (!present) continue;
      }
      const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 0
                        : 1;
      if (score > best_score) {
        best_score = score;
        physical_ = candidate;
        queue_family_ = i;
      }
      break;
    }
  }

  if (!physical_) {
    error = "no Vulkan 1.3 device with dynamicRendering and synchronization2 found";
    return false;
  }

  vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props_);

  VkPhysicalDeviceDepthStencilResolveProperties ds_resolve{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
  VkPhysicalDeviceDriverProperties drv_props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  ds_resolve.pNext = &drv_props;
  VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  props2.pNext = &ds_resolve;
  vkGetPhysicalDeviceProperties2(physical_, &props2);

  const VkPhysicalDeviceProperties& p = props2.properties;
  caps_ = BackendCaps{};
  caps_.backend = BackendKind::Vulkan;
  caps_.device_name = p.deviceName;
  caps_.driver_name = drv_props.driverName;
  caps_.driver_info = drv_props.driverInfo;
  caps_.vendor_id = p.vendorID;
  caps_.device_id = p.deviceID;
  caps_.api_version = p.apiVersion;
  caps_.driver_version = p.driverVersion;
  std::memcpy(caps_.pipeline_cache_uuid.data(), p.pipelineCacheUUID, sizeof(caps_.pipeline_cache_uuid));

  caps_.is_discrete = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
  caps_.is_integrated = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
  caps_.is_software = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU);
  caps_.is_moltenvk = (drv_props.driverID == VK_DRIVER_ID_MOLTENVK) ||
                      contains_ci(caps_.driver_name, "moltenvk") ||
                      contains_ci(caps_.driver_info, "moltenvk");
  caps_.is_turnip = (drv_props.driverID == VK_DRIVER_ID_MESA_TURNIP) ||
                    contains_ci(caps_.driver_name, "turnip") ||
                    contains_ci(caps_.driver_info, "turnip");

  uint64_t local_bytes = 0;
  for (uint32_t i = 0; i < mem_props_.memoryHeapCount; ++i) {
    if (mem_props_.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
      local_bytes += mem_props_.memoryHeaps[i].size;
    }
  }
  caps_.device_local_bytes = local_bytes;

  caps_.lazy_memory = false;
  for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
    if (mem_props_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) {
      caps_.lazy_memory = true;
      break;
    }
  }
  caps_.is_mobile_tbdr = caps_.lazy_memory && !caps_.is_discrete;

  caps_.dynamic_rendering = true;
  caps_.synchronization2 = true;
  caps_.depth_resolve_sample_zero =
      (ds_resolve.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) != 0;
  caps_.supported_msaa_mask =
      static_cast<uint32_t>(p.limits.framebufferColorSampleCounts & p.limits.framebufferDepthSampleCounts);
  if (caps_.supported_msaa_mask == 0) caps_.supported_msaa_mask = 1u;

  uint32_t fam_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &fam_count, nullptr);
  std::vector<VkQueueFamilyProperties> fams(fam_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &fam_count, fams.data());
  if (queue_family_ < fam_count && fams[queue_family_].timestampValidBits > 0 && p.limits.timestampPeriod > 0.0f) {
    caps_.timestamps = true;
    caps_.timestamp_period_ns = static_cast<double>(p.limits.timestampPeriod);
  }

  cache_key_.backend = BackendKind::Vulkan;
  cache_key_.vendor_id = caps_.vendor_id;
  cache_key_.device_id = caps_.device_id;
  cache_key_.uuid = caps_.pipeline_cache_uuid;
  return true;
}

bool VkDeviceImpl::create_logical_device(const DeviceDesc& desc, std::string& error) {
  uint32_t fam_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &fam_count, nullptr);
  std::vector<VkQueueFamilyProperties> fams(fam_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &fam_count, fams.data());

  uint32_t queue_count = 1;
  if (desc.enable_transfer_queue && queue_family_ < fam_count && fams[queue_family_].queueCount >= 2) {
    queue_count = 2;
  }
  const float priorities[2] = {1.0f, 0.5f};
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queue_family_;
  qci.queueCount = queue_count;
  qci.pQueuePriorities = priorities;

  // Query core Vulkan 1.0 / 1.2 / 1.3 offered features & descriptor indexing limits.
  VkPhysicalDeviceVulkan13Features o13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features o12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  o13.pNext = &o12;
  VkPhysicalDeviceFeatures2 offered2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  offered2.pNext = &o13;
  vkGetPhysicalDeviceFeatures2(physical_, &offered2);

  VkPhysicalDeviceDescriptorIndexingProperties di_props{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
  VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  p2.pNext = &di_props;
  vkGetPhysicalDeviceProperties2(physical_, &p2);

  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  f13.shaderDemoteToHelperInvocation = o13.shaderDemoteToHelperInvocation;
  f13.maintenance4 = o13.maintenance4;

  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  f13.pNext = &f12;

  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f13;

  if (offered2.features.samplerAnisotropy && desc.max_anisotropy > 1.0f) {
    f2.features.samplerAnisotropy = VK_TRUE;
    caps_.max_anisotropy = std::min(desc.max_anisotropy, p2.properties.limits.maxSamplerAnisotropy);
  } else {
    caps_.max_anisotropy = 1.0f;
  }

  if (offered2.features.multiDrawIndirect) {
    f2.features.multiDrawIndirect = VK_TRUE;
    caps_.multi_draw_indirect = true;
  }
  if (offered2.features.drawIndirectFirstInstance) {
    f2.features.drawIndirectFirstInstance = VK_TRUE;
  }
  if (o12.drawIndirectCount) {
    f12.drawIndirectCount = VK_TRUE;
    caps_.indirect_count = true;
  }

  const uint32_t max_uab_imgs = std::min(di_props.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                         di_props.maxDescriptorSetUpdateAfterBindSampledImages);
  const bool bindless_ok = desc.enable_bindless &&
                           o12.descriptorIndexing &&
                           o12.runtimeDescriptorArray &&
                           o12.shaderSampledImageArrayNonUniformIndexing &&
                           o12.descriptorBindingPartiallyBound &&
                           o12.descriptorBindingSampledImageUpdateAfterBind &&
                           o12.descriptorBindingUpdateUnusedWhilePending &&
                           di_props.maxPerStageDescriptorUpdateAfterBindSamplers >= 1 &&
                           max_uab_imgs >= 4096;
  if (bindless_ok) {
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    if (o12.descriptorBindingStorageBufferUpdateAfterBind) {
      f12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    }
    if (o12.shaderStorageBufferArrayNonUniformIndexing) {
      f12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    }
    caps_.bindless = true;
    caps_.max_bindless_textures = max_uab_imgs;
  }

  if (o12.bufferDeviceAddress) {
    f12.bufferDeviceAddress = VK_TRUE;
    caps_.buffer_device_address = true;
  }
  if (o12.scalarBlockLayout) {
    f12.scalarBlockLayout = VK_TRUE;
  }

  // Enumerate device extensions.
  uint32_t dev_ext_count = 0;
  vkEnumerateDeviceExtensionProperties(physical_, nullptr, &dev_ext_count, nullptr);
  std::vector<VkExtensionProperties> dev_exts(dev_ext_count);
  if (dev_ext_count > 0) {
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &dev_ext_count, dev_exts.data());
  }
  auto has_dev_ext = [&](const char* name) {
    return std::any_of(dev_exts.begin(), dev_exts.end(), [&](const VkExtensionProperties& e) {
      return std::strcmp(e.extensionName, name) == 0;
    });
  };

  std::vector<const char*> extensions;
  if (probe_surface_ && has_dev_ext(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
    extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  } else if (has_dev_ext(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
    // Enable swapchain extension proactively when available so create_swapchain()
    // can be called on an already-created device.
    extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }
  if (has_dev_ext("VK_KHR_portability_subset")) {
    extensions.push_back("VK_KHR_portability_subset");
  }

  VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrs_feat{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
  if (has_dev_ext(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) {
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR offered_vrs{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 qvrs{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    qvrs.pNext = &offered_vrs;
    vkGetPhysicalDeviceFeatures2(physical_, &qvrs);
    if (offered_vrs.pipelineFragmentShadingRate) {
      vrs_feat.pipelineFragmentShadingRate = VK_TRUE;
      vrs_feat.pNext = f2.pNext;
      f2.pNext = &vrs_feat;
      extensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
      caps_.fragment_shading_rate = true;
    }
  }

#ifdef SHIFT_ENABLE_RAY_TRACING
  VkPhysicalDeviceAccelerationStructureFeaturesKHR as_feat{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  VkPhysicalDeviceRayQueryFeaturesKHR rq_feat{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtp_feat{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
  VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR rtm1_feat{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR};
  VkPhysicalDeviceOpacityMicromapFeaturesEXT omm_feat{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT};

  const bool has_as = has_dev_ext(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
  const bool has_rq = has_dev_ext(VK_KHR_RAY_QUERY_EXTENSION_NAME);
  const bool has_dho = has_dev_ext(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
  const bool has_rtp = has_dev_ext(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
  const bool has_rtm1 = has_dev_ext(VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME);
  const bool has_omm = has_dev_ext(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);

  if (desc.enable_ray_tracing && has_as && has_rq && has_dho &&
      caps_.buffer_device_address && o12.scalarBlockLayout && offered2.features.shaderInt64) {
    VkPhysicalDeviceAccelerationStructureFeaturesKHR o_as{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR o_rq{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR o_rtp{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR o_rtm1{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR};
    VkPhysicalDeviceOpacityMicromapFeaturesEXT o_omm{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT};

    VkPhysicalDeviceFeatures2 qrt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    qrt.pNext = &o_as;
    void** tail = &o_as.pNext;
    *tail = &o_rq;
    tail = &o_rq.pNext;
    if (has_rtp) { *tail = &o_rtp; tail = &o_rtp.pNext; }
    if (has_rtm1) { *tail = &o_rtm1; tail = &o_rtm1.pNext; }
    if (has_omm) { *tail = &o_omm; tail = &o_omm.pNext; }
    vkGetPhysicalDeviceFeatures2(physical_, &qrt);

    if (o_as.accelerationStructure && o_rq.rayQuery) {
      f2.features.shaderInt64 = VK_TRUE;
      as_feat.accelerationStructure = VK_TRUE;
      as_feat.pNext = f2.pNext;
      f2.pNext = &as_feat;
      rq_feat.rayQuery = VK_TRUE;
      rq_feat.pNext = f2.pNext;
      f2.pNext = &rq_feat;
      extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
      extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
      extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
      caps_.ray_query = true;

      if (has_rtp && o_rtp.rayTracingPipeline && !caps_.is_moltenvk && !caps_.is_turnip) {
        rtp_feat.rayTracingPipeline = VK_TRUE;
        rtp_feat.pNext = f2.pNext;
        f2.pNext = &rtp_feat;
        extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        caps_.rt_pipeline = true;
      }
      if (has_rtm1 && o_rtm1.rayTracingMaintenance1) {
        rtm1_feat.rayTracingMaintenance1 = VK_TRUE;
        rtm1_feat.pNext = f2.pNext;
        f2.pNext = &rtm1_feat;
        extensions.push_back(VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME);
        caps_.rt_maintenance1 = true;
      }
      if (has_omm && o_omm.micromap && !caps_.is_moltenvk && !caps_.is_turnip) {
        omm_feat.micromap = VK_TRUE;
        omm_feat.pNext = f2.pNext;
        f2.pNext = &omm_feat;
        extensions.push_back(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
        caps_.opacity_micromap = true;
      }

      VkPhysicalDeviceAccelerationStructurePropertiesKHR as_props{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
      VkPhysicalDeviceProperties2 prt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      prt.pNext = &as_props;
      vkGetPhysicalDeviceProperties2(physical_, &prt);
      caps_.min_as_scratch_alignment = std::max(as_props.minAccelerationStructureScratchOffsetAlignment, 256u);
    }
  }
#endif

  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &f2;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  dci.ppEnabledExtensionNames = extensions.data();

  const VkResult r = vkCreateDevice(physical_, &dci, nullptr, &device_);
  if (r != VK_SUCCESS) {
    error = vk_err("vkCreateDevice", r);
    return false;
  }
  volkLoadDevice(device_);

  vkGetDeviceQueue(device_, queue_family_, 0, &graphics_queue_);
  if (queue_count > 1) {
    vkGetDeviceQueue(device_, queue_family_, 1, &transfer_queue_);
  }

  // VMA allocator (with BDA enabled whenever bufferDeviceAddress is active).
  VmaVulkanFunctions vma_funcs{};
  vma_funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  vma_funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
  VmaAllocatorCreateInfo aci{};
  aci.flags = caps_.buffer_device_address ? VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT : 0u;
  aci.physicalDevice = physical_;
  aci.device = device_;
  aci.pVulkanFunctions = &vma_funcs;
  aci.instance = instance_;
  aci.vulkanApiVersion = VK_API_VERSION_1_3;
  const VkResult vr = vmaCreateAllocator(&aci, &allocator_);
  if (vr != VK_SUCCESS) {
    error = vk_err("vmaCreateAllocator", vr);
    return false;
  }

  load_pipeline_cache();
  return true;
}

void VkDeviceImpl::load_pipeline_cache() {
  std::vector<uint8_t> initial_data;
  pipeline_cache_loaded_ = false;
  pipeline_cache_loaded_bytes_ = 0;

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
            if (decode_pipeline_cache_envelope(raw.data(), raw.size(), cache_key_, initial_data)) {
              pipeline_cache_loaded_ = true;
              pipeline_cache_loaded_bytes_ = initial_data.size();
            }
          }
        }
      }
    }
  }

  VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  pci.initialDataSize = initial_data.size();
  pci.pInitialData = initial_data.empty() ? nullptr : initial_data.data();
  if (vkCreatePipelineCache(device_, &pci, nullptr, &pipeline_cache_) != VK_SUCCESS && !initial_data.empty()) {
    pipeline_cache_loaded_ = false;
    pipeline_cache_loaded_bytes_ = 0;
    pci.initialDataSize = 0;
    pci.pInitialData = nullptr;
    vkCreatePipelineCache(device_, &pci, nullptr, &pipeline_cache_);
  }
}

bool VkDeviceImpl::save_pipeline_cache(std::string& error) {
  if (!device_ || !pipeline_cache_ || pipeline_cache_path_.empty()) return true;
  size_t sz = 0;
  VkResult r = vkGetPipelineCacheData(device_, pipeline_cache_, &sz, nullptr);
  if (r != VK_SUCCESS) {
    error = vk_err("vkGetPipelineCacheData(size)", r);
    return false;
  }
  if (sz == 0) return true;
  if (pipeline_cache_loaded_ && sz == pipeline_cache_loaded_bytes_) return true;

  std::vector<uint8_t> payload(sz);
  r = vkGetPipelineCacheData(device_, pipeline_cache_, &sz, payload.data());
  if (r != VK_SUCCESS) {
    error = vk_err("vkGetPipelineCacheData", r);
    return false;
  }
  payload.resize(sz);

  const std::vector<uint8_t> encoded = encode_pipeline_cache_envelope(cache_key_, payload.data(), payload.size());
  std::error_code ec;
  if (pipeline_cache_path_.has_parent_path()) {
    std::filesystem::create_directories(pipeline_cache_path_.parent_path(), ec);
  }
  const std::filesystem::path tmp_path = pipeline_cache_path_.string() + ".tmp";
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "failed to open temporary pipeline cache file for writing";
      return false;
    }
    out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    if (!out.good()) {
      error = "failed writing pipeline cache payload";
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
      error = "failed renaming pipeline cache into place";
      return false;
    }
  }
  pipeline_cache_loaded_ = true;
  pipeline_cache_loaded_bytes_ = payload.size();
  return true;
}

VramStats VkDeviceImpl::vram_stats() const {
  VramStats out{};
  if (!allocator_) return out;
  VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
  vmaGetHeapBudgets(allocator_, budgets);
  for (uint32_t i = 0; i < mem_props_.memoryHeapCount; ++i) {
    out.block_count += budgets[i].statistics.blockCount;
    out.allocation_count += budgets[i].statistics.allocationCount;
    out.block_bytes += budgets[i].statistics.blockBytes;
    out.allocation_bytes += budgets[i].statistics.allocationBytes;
    if (mem_props_.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
      out.device_local_usage += budgets[i].usage;
      out.device_local_budget += budgets[i].budget;
    }
  }
  return out;
}

void VkDeviceImpl::wait_idle() {
  if (!device_) return;
  std::lock_guard<std::mutex> lock(queue_mutex_);
  vkDeviceWaitIdle(device_);
}

// -----------------------------------------------------------------------------
// Lookup helpers
// -----------------------------------------------------------------------------
const BufferEntry* VkDeviceImpl::lookup_buffer(BufferHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return buffers_.get(h);
}
const TextureEntry* VkDeviceImpl::lookup_texture(TextureHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return textures_.get(h);
}
const TextureViewEntry* VkDeviceImpl::lookup_texture_view(TextureViewHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return texture_views_.get(h);
}
const SamplerEntry* VkDeviceImpl::lookup_sampler(SamplerHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return samplers_.get(h);
}
const BindGroupLayoutEntry* VkDeviceImpl::lookup_bind_group_layout(BindGroupLayoutHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return bind_group_layouts_.get(h);
}
const BindGroupEntry* VkDeviceImpl::lookup_bind_group(BindGroupHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return bind_groups_.get(h);
}
const PipelineLayoutEntry* VkDeviceImpl::lookup_pipeline_layout(PipelineLayoutHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return pipeline_layouts_.get(h);
}
const PipelineEntry* VkDeviceImpl::lookup_pipeline(PipelineHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return pipelines_.get(h);
}
const QueryPoolEntry* VkDeviceImpl::lookup_query_pool(QueryPoolHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return query_pools_.get(h);
}
const AccelStructEntry* VkDeviceImpl::lookup_accel_struct(AccelStructHandle h) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return accel_structs_.get(h);
}

// -----------------------------------------------------------------------------
// Buffers
// -----------------------------------------------------------------------------
BufferHandle VkDeviceImpl::create_buffer(const BufferDesc& desc, std::string& error) {
  if (desc.size == 0) {
    error = "create_buffer: size must be > 0";
    return {};
  }
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = desc.size;
  bci.usage = to_vk_buffer_usage(desc.usage, caps_.buffer_device_address, caps_.ray_query);
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo aci{};
  switch (desc.memory) {
    case MemoryHint::CpuToGpu:
      aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
      aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
      if (desc.persistently_mapped) aci.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
      break;
    case MemoryHint::GpuToCpu:
      aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
      aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
      if (desc.persistently_mapped) aci.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
      break;
    case MemoryHint::Transient:
    case MemoryHint::GpuOnly:
    default:
      aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
      break;
  }

  BufferEntry entry{};
  entry.size = desc.size;
  entry.usage = desc.usage;
  entry.memory = desc.memory;
  VmaAllocationInfo alloc_info{};
  const VkResult r = vmaCreateBuffer(allocator_, &bci, &aci, &entry.buffer, &entry.allocation, &alloc_info);
  if (r != VK_SUCCESS) {
    error = vk_err("vmaCreateBuffer", r);
    return {};
  }
  if (desc.persistently_mapped && alloc_info.pMappedData) {
    entry.persistent_map = alloc_info.pMappedData;
  }
  if ((bci.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) && vkGetBufferDeviceAddress) {
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = entry.buffer;
    entry.device_address = vkGetBufferDeviceAddress(device_, &ai);
  }

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return buffers_.allocate(std::move(entry));
}

void VkDeviceImpl::destroy_buffer(BufferHandle handle) {
  BufferEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!buffers_.remove(handle, &entry)) return;
  }
  if (entry.buffer && entry.allocation) {
    vmaDestroyBuffer(allocator_, entry.buffer, entry.allocation);
  }
}

void* VkDeviceImpl::map_buffer(BufferHandle handle) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  BufferEntry* e = buffers_.get(handle);
  if (!e || !e->allocation) return nullptr;
  if (e->persistent_map) return e->persistent_map;
  void* ptr = nullptr;
  if (vmaMapMemory(allocator_, e->allocation, &ptr) != VK_SUCCESS) return nullptr;
  return ptr;
}

void VkDeviceImpl::unmap_buffer(BufferHandle handle) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  BufferEntry* e = buffers_.get(handle);
  if (!e || !e->allocation || e->persistent_map) return;
  vmaUnmapMemory(allocator_, e->allocation);
}

uint64_t VkDeviceImpl::buffer_device_address(BufferHandle handle) const {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  const BufferEntry* e = buffers_.get(handle);
  return e ? e->device_address : 0ull;
}

// -----------------------------------------------------------------------------
// Textures & TextureViews
// -----------------------------------------------------------------------------
TextureHandle VkDeviceImpl::create_texture(const TextureDesc& desc, std::string& error) {
  const VkFormat vk_fmt = to_vk_format(desc.format);
  if (vk_fmt == VK_FORMAT_UNDEFINED || desc.width == 0 || desc.height == 0 || desc.depth == 0) {
    error = "create_texture: invalid format or zero dimension";
    return {};
  }
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  if (desc.type == TextureType::TexCube) {
    ici.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  }
  ici.imageType = to_vk_image_type(desc.type);
  ici.format = vk_fmt;
  ici.extent = {desc.width, desc.height, desc.type == TextureType::Tex3D ? desc.depth : 1u};
  ici.mipLevels = std::max(desc.mip_levels, 1u);
  ici.arrayLayers = desc.type == TextureType::TexCube ? std::max(desc.array_layers, 6u)
                                                      : std::max(desc.array_layers, 1u);
  ici.samples = to_vk_sample_count(desc.sample_count);
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = to_vk_image_usage(desc.usage);
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (desc.memory == MemoryHint::Transient && caps_.lazy_memory &&
      has_flag(desc.usage, TextureUsage::Transient)) {
    aci.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
  }

  TextureEntry entry{};
  entry.vk_format = vk_fmt;
  entry.format = desc.format;
  entry.type = desc.type;
  entry.width = desc.width;
  entry.height = desc.height;
  entry.depth = ici.extent.depth;
  entry.mip_levels = ici.mipLevels;
  entry.array_layers = ici.arrayLayers;
  entry.sample_count = std::max(desc.sample_count, 1u);
  entry.owned = true;

  const VkResult r = vmaCreateImage(allocator_, &ici, &aci, &entry.image, &entry.allocation, nullptr);
  if (r != VK_SUCCESS) {
    error = vk_err("vmaCreateImage", r);
    return {};
  }

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return textures_.allocate(std::move(entry));
}

void VkDeviceImpl::destroy_texture(TextureHandle handle) {
  TextureEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!textures_.remove(handle, &entry)) return;
  }
  if (entry.owned && entry.image && entry.allocation) {
    vmaDestroyImage(allocator_, entry.image, entry.allocation);
  }
}

TextureViewHandle VkDeviceImpl::create_texture_view(const TextureViewDesc& desc, std::string& error) {
  VkImage image = VK_NULL_HANDLE;
  VkFormat fmt = VK_FORMAT_UNDEFINED;
  Format rhi_fmt = desc.format;
  TextureType view_type = desc.type;
  uint32_t max_mips = 1;
  uint32_t max_layers = 1;
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const TextureEntry* tex = textures_.get(desc.texture);
    if (!tex) {
      error = "create_texture_view: invalid TextureHandle";
      return {};
    }
    image = tex->image;
    fmt = (desc.format == Format::Undefined) ? tex->vk_format : to_vk_format(desc.format);
    rhi_fmt = (desc.format == Format::Undefined) ? tex->format : desc.format;
    max_mips = tex->mip_levels;
    max_layers = tex->array_layers;
  }

  const bool is_depth = format_is_depth(rhi_fmt);
  VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  ivci.image = image;
  ivci.viewType = to_vk_view_type(view_type);
  ivci.format = fmt;
  ivci.components = {
      to_vk_swizzle(desc.swizzle.r),
      to_vk_swizzle(desc.swizzle.g),
      to_vk_swizzle(desc.swizzle.b),
      to_vk_swizzle(desc.swizzle.a),
  };
  ivci.subresourceRange.aspectMask = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  ivci.subresourceRange.baseMipLevel = desc.base_mip;
  ivci.subresourceRange.levelCount = std::min(desc.mip_count, max_mips - std::min(desc.base_mip, max_mips));
  ivci.subresourceRange.baseArrayLayer = desc.base_layer;
  ivci.subresourceRange.layerCount =
      std::min(desc.layer_count, max_layers - std::min(desc.base_layer, max_layers));

  TextureViewEntry entry{};
  entry.texture = desc.texture;
  entry.vk_format = fmt;
  entry.is_depth = is_depth;
  entry.owned = true;

  const VkResult r = vkCreateImageView(device_, &ivci, nullptr, &entry.view);
  if (r != VK_SUCCESS) {
    error = vk_err("vkCreateImageView", r);
    return {};
  }

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return texture_views_.allocate(std::move(entry));
}

void VkDeviceImpl::destroy_texture_view(TextureViewHandle handle) {
  TextureViewEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!texture_views_.remove(handle, &entry)) return;
  }
  if (entry.owned && entry.view) {
    vkDestroyImageView(device_, entry.view, nullptr);
  }
}

TextureHandle VkDeviceImpl::register_external_texture(VkImage image, VkFormat fmt, uint32_t w, uint32_t h) {
  TextureEntry entry{};
  entry.image = image;
  entry.vk_format = fmt;
  entry.format = from_vk_format(fmt);
  entry.type = TextureType::Tex2D;
  entry.width = w;
  entry.height = h;
  entry.owned = false;
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return textures_.allocate(std::move(entry));
}

TextureViewHandle VkDeviceImpl::register_external_texture_view(TextureHandle tex, VkImageView view, VkFormat fmt) {
  TextureViewEntry entry{};
  entry.view = view;
  entry.texture = tex;
  entry.vk_format = fmt;
  entry.is_depth = format_is_depth(from_vk_format(fmt));
  entry.owned = false;
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return texture_views_.allocate(std::move(entry));
}

void VkDeviceImpl::unregister_external_texture_view(TextureViewHandle h) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  texture_views_.remove(h);
}

void VkDeviceImpl::unregister_external_texture(TextureHandle h) {
  std::lock_guard<std::mutex> lock(resource_mutex_);
  textures_.remove(h);
}

// -----------------------------------------------------------------------------
// Samplers
// -----------------------------------------------------------------------------
SamplerHandle VkDeviceImpl::create_sampler(const SamplerDesc& desc, std::string& error) {
  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = to_vk_filter(desc.mag_filter);
  sci.minFilter = to_vk_filter(desc.min_filter);
  sci.mipmapMode = to_vk_mipmap_mode(desc.mip_filter);
  sci.addressModeU = to_vk_address_mode(desc.address_u);
  sci.addressModeV = to_vk_address_mode(desc.address_v);
  sci.addressModeW = to_vk_address_mode(desc.address_w);
  sci.mipLodBias = desc.mip_lod_bias;
  if (desc.max_anisotropy > 1.0f && caps_.max_anisotropy > 1.0f) {
    sci.anisotropyEnable = VK_TRUE;
    sci.maxAnisotropy = std::min(desc.max_anisotropy, caps_.max_anisotropy);
  }
  sci.compareEnable = desc.compare_enabled ? VK_TRUE : VK_FALSE;
  sci.compareOp = to_vk_compare_op(desc.compare_op);
  sci.minLod = desc.min_lod;
  sci.maxLod = desc.max_lod;
  sci.borderColor = to_vk_border_color(desc.border_color);

  SamplerEntry entry{};
  const VkResult r = vkCreateSampler(device_, &sci, nullptr, &entry.sampler);
  if (r != VK_SUCCESS) {
    error = vk_err("vkCreateSampler", r);
    return {};
  }
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return samplers_.allocate(std::move(entry));
}

void VkDeviceImpl::destroy_sampler(SamplerHandle handle) {
  SamplerEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!samplers_.remove(handle, &entry)) return;
  }
  if (entry.sampler) vkDestroySampler(device_, entry.sampler, nullptr);
}

// -----------------------------------------------------------------------------
// Bind Group Layouts, Bind Groups, and Split Bindless Table
// -----------------------------------------------------------------------------
BindGroupLayoutHandle VkDeviceImpl::create_bind_group_layout(const BindGroupLayoutDesc& desc, std::string& error) {
  std::vector<VkDescriptorSetLayoutBinding> vk_bindings;
  std::vector<VkDescriptorBindingFlags> binding_flags;
  std::vector<VkDescriptorPoolSize> pool_sizes;
  vk_bindings.reserve(desc.bindings.size());
  binding_flags.reserve(desc.bindings.size());

  bool any_flags = false;
  for (const BindGroupLayoutBinding& b : desc.bindings) {
    VkDescriptorSetLayoutBinding vb{};
    vb.binding = b.binding;
    vb.descriptorType = to_vk_descriptor_type(b.type);
    vb.descriptorCount = std::max(b.count, 1u);
    vb.stageFlags = to_vk_shader_stages(b.stages);
    vk_bindings.push_back(vb);

    VkDescriptorBindingFlags bf = 0;
    if (b.partially_bound && caps_.bindless) bf |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    if (b.update_after_bind && caps_.bindless) {
      bf |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }
    if (bf != 0) any_flags = true;
    binding_flags.push_back(bf);

    auto it = std::find_if(pool_sizes.begin(), pool_sizes.end(), [&](const VkDescriptorPoolSize& ps) {
      return ps.type == vb.descriptorType;
    });
    if (it != pool_sizes.end()) {
      it->descriptorCount += vb.descriptorCount;
    } else {
      pool_sizes.push_back({vb.descriptorType, vb.descriptorCount});
    }
  }

  VkDescriptorSetLayoutBindingFlagsCreateInfo bfci{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  bfci.bindingCount = static_cast<uint32_t>(binding_flags.size());
  bfci.pBindingFlags = binding_flags.data();

  VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  if (any_flags) lci.pNext = &bfci;
  if (desc.update_after_bind_pool && caps_.bindless) {
    lci.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  }
  lci.bindingCount = static_cast<uint32_t>(vk_bindings.size());
  lci.pBindings = vk_bindings.data();

  BindGroupLayoutEntry entry{};
  entry.pool_sizes = std::move(pool_sizes);
  entry.update_after_bind = (desc.update_after_bind_pool && caps_.bindless);
  const VkResult r = vkCreateDescriptorSetLayout(device_, &lci, nullptr, &entry.layout);
  if (r != VK_SUCCESS) {
    error = vk_err("vkCreateDescriptorSetLayout", r);
    return {};
  }

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return bind_group_layouts_.allocate(std::move(entry));
}

void VkDeviceImpl::destroy_bind_group_layout(BindGroupLayoutHandle handle) {
  BindGroupLayoutEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!bind_group_layouts_.remove(handle, &entry)) return;
  }
  if (entry.layout) vkDestroyDescriptorSetLayout(device_, entry.layout, nullptr);
}

BindGroupHandle VkDeviceImpl::create_bind_group(const BindGroupDesc& desc, std::string& error) {
  VkDescriptorSetLayout vk_layout = VK_NULL_HANDLE;
  std::vector<VkDescriptorPoolSize> pool_sizes;
  bool uab = false;
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    const BindGroupLayoutEntry* le = bind_group_layouts_.get(desc.layout);
    if (!le) {
      error = "create_bind_group: invalid BindGroupLayoutHandle";
      return {};
    }
    vk_layout = le->layout;
    pool_sizes = le->pool_sizes;
    uab = le->update_after_bind;
  }

  if (pool_sizes.empty()) {
    pool_sizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1});
  }

  VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  if (uab) pci.flags |= VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  pci.maxSets = 1;
  pci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
  pci.pPoolSizes = pool_sizes.data();

  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkResult r = vkCreateDescriptorPool(device_, &pci, nullptr, &pool);
  if (r != VK_SUCCESS) {
    error = vk_err("vkCreateDescriptorPool", r);
    return {};
  }

  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &vk_layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  r = vkAllocateDescriptorSets(device_, &ai, &set);
  if (r != VK_SUCCESS) {
    vkDestroyDescriptorPool(device_, pool, nullptr);
    error = vk_err("vkAllocateDescriptorSets", r);
    return {};
  }

  // Build descriptor writes.
  std::vector<VkWriteDescriptorSet> writes;
  std::vector<VkDescriptorBufferInfo> buf_infos;
  std::vector<VkDescriptorImageInfo> img_infos;
  std::vector<VkWriteDescriptorSetAccelerationStructureKHR> as_writes;
  std::vector<VkAccelerationStructureKHR> as_handles;
  writes.reserve(desc.entries.size());
  buf_infos.reserve(desc.entries.size());
  img_infos.reserve(desc.entries.size());
  as_writes.reserve(desc.entries.size());
  as_handles.reserve(desc.entries.size());

  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    for (const shift::rhi::BindGroupEntry& e : desc.entries) {
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = set;
      w.dstBinding = e.binding;
      w.dstArrayElement = e.array_element;
      w.descriptorCount = 1;
      w.descriptorType = to_vk_descriptor_type(e.type);

      if (e.type == DescriptorType::UniformBuffer || e.type == DescriptorType::StorageBuffer) {
        const BufferEntry* be = buffers_.get(e.buffer);
        if (!be) {
          vkDestroyDescriptorPool(device_, pool, nullptr);
          error = "create_bind_group: invalid BufferHandle";
          return {};
        }
        buf_infos.push_back({be->buffer, e.buffer_offset,
                             e.buffer_range == UINT64_MAX ? VK_WHOLE_SIZE : e.buffer_range});
        w.pBufferInfo = &buf_infos.back();
        writes.push_back(w);
      } else if (e.type == DescriptorType::Sampler) {
        const SamplerEntry* se = samplers_.get(e.sampler);
        if (!se) {
          vkDestroyDescriptorPool(device_, pool, nullptr);
          error = "create_bind_group: invalid SamplerHandle";
          return {};
        }
        img_infos.push_back({se->sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
        w.pImageInfo = &img_infos.back();
        writes.push_back(w);
      } else if (e.type == DescriptorType::SampledTexture ||
                 e.type == DescriptorType::StorageTexture ||
                 e.type == DescriptorType::CombinedTextureSampler) {
        const TextureViewEntry* tv = texture_views_.get(e.texture_view);
        if (!tv) {
          vkDestroyDescriptorPool(device_, pool, nullptr);
          error = "create_bind_group: invalid TextureViewHandle";
          return {};
        }
        VkSampler samp = VK_NULL_HANDLE;
        if (e.type == DescriptorType::CombinedTextureSampler) {
          const SamplerEntry* se = samplers_.get(e.sampler);
          if (!se) {
            vkDestroyDescriptorPool(device_, pool, nullptr);
            error = "create_bind_group: invalid SamplerHandle for CombinedTextureSampler";
            return {};
          }
          samp = se->sampler;
        }
        const VkImageLayout layout =
            e.type == DescriptorType::StorageTexture
                ? VK_IMAGE_LAYOUT_GENERAL
                : (tv->is_depth ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        img_infos.push_back({samp, tv->view, layout});
        w.pImageInfo = &img_infos.back();
        writes.push_back(w);
      } else if (e.type == DescriptorType::AccelStruct) {
        const AccelStructEntry* as = accel_structs_.get(e.accel_struct);
        if (!as) {
          vkDestroyDescriptorPool(device_, pool, nullptr);
          error = "create_bind_group: invalid AccelStructHandle";
          return {};
        }
        as_handles.push_back(as->handle);
        VkWriteDescriptorSetAccelerationStructureKHR asw{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        asw.accelerationStructureCount = 1;
        asw.pAccelerationStructures = &as_handles.back();
        as_writes.push_back(asw);
        w.pNext = &as_writes.back();
        writes.push_back(w);
      }
    }
  }

  if (!writes.empty()) {
    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  }

  BindGroupEntry entry{};
  entry.set = set;
  entry.pool = pool;
  entry.owns_pool = true;

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return bind_groups_.allocate(std::move(entry));
}

void VkDeviceImpl::destroy_bind_group(BindGroupHandle handle) {
  BindGroupEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!bind_groups_.remove(handle, &entry)) return;
  }
  if (entry.owns_pool && entry.pool) {
    vkDestroyDescriptorPool(device_, entry.pool, nullptr);
  }
}

std::unique_ptr<BindlessTable> VkDeviceImpl::create_bindless_table(const BindlessTableDesc& desc,
                                                                   std::string& error) {
  if (!caps_.bindless) {
    error = "bindless descriptor indexing is not supported on this device";
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
    bindings.push_back({2, DescriptorType::StorageBuffer, desc.max_storage_buffers, desc.stages, true, false});
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

  const BindGroupEntry* ge = lookup_bind_group(group_h);
  VkDescriptorSet set = ge ? ge->set : VK_NULL_HANDLE;
  auto table = std::make_unique<VkBindlessTableImpl>(*this, layout_h, group_h, set, max_tex,
                                                     desc.max_storage_buffers);

  if (desc.default_sampler) table->set_sampler(desc.default_sampler);
  if (desc.fallback_texture) {
    const TextureViewEntry* tv = lookup_texture_view(desc.fallback_texture);
    if (tv && set) {
      std::vector<VkDescriptorImageInfo> infos(
          max_tex, VkDescriptorImageInfo{VK_NULL_HANDLE, tv->view,
                                         tv->is_depth ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                                                      : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = set;
      w.dstBinding = 1;
      w.dstArrayElement = 0;
      w.descriptorCount = max_tex;
      w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      w.pImageInfo = infos.data();
      vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
  }
  if (desc.fallback_buffer && desc.max_storage_buffers > 0) {
    const BufferEntry* be = lookup_buffer(desc.fallback_buffer);
    if (be && set) {
      std::vector<VkDescriptorBufferInfo> infos(
          desc.max_storage_buffers, VkDescriptorBufferInfo{be->buffer, 0, VK_WHOLE_SIZE});
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = set;
      w.dstBinding = 2;
      w.dstArrayElement = 0;
      w.descriptorCount = desc.max_storage_buffers;
      w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w.pBufferInfo = infos.data();
      vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
  }
  return table;
}

// -----------------------------------------------------------------------------
// Pipeline Layouts & Pipelines
// -----------------------------------------------------------------------------
PipelineLayoutHandle VkDeviceImpl::create_pipeline_layout(const PipelineLayoutDesc& desc, std::string& error) {
  std::vector<VkDescriptorSetLayout> set_layouts;
  set_layouts.reserve(desc.bind_group_layouts.size());
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    for (BindGroupLayoutHandle h : desc.bind_group_layouts) {
      const BindGroupLayoutEntry* le = bind_group_layouts_.get(h);
      if (!le) {
        error = "create_pipeline_layout: invalid BindGroupLayoutHandle";
        return {};
      }
      set_layouts.push_back(le->layout);
    }
  }

  std::vector<VkPushConstantRange> pc_ranges;
  pc_ranges.reserve(desc.push_constants.size());
  for (const PushConstantRange& p : desc.push_constants) {
    VkPushConstantRange r{};
    r.stageFlags = to_vk_shader_stages(p.stages);
    r.offset = p.offset;
    r.size = p.size;
    pc_ranges.push_back(r);
  }

  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
  plci.pSetLayouts = set_layouts.data();
  plci.pushConstantRangeCount = static_cast<uint32_t>(pc_ranges.size());
  plci.pPushConstantRanges = pc_ranges.data();

  PipelineLayoutEntry entry{};
  const VkResult r = vkCreatePipelineLayout(device_, &plci, nullptr, &entry.layout);
  if (r != VK_SUCCESS) {
    error = vk_err("vkCreatePipelineLayout", r);
    return {};
  }
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return pipeline_layouts_.allocate(std::move(entry));
}

void VkDeviceImpl::destroy_pipeline_layout(PipelineLayoutHandle handle) {
  PipelineLayoutEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!pipeline_layouts_.remove(handle, &entry)) return;
  }
  if (entry.layout) vkDestroyPipelineLayout(device_, entry.layout, nullptr);
}

namespace {

bool create_shader_module(VkDevice device, std::span<const uint32_t> spirv,
                          VkShaderModule& out_mod, std::string& error) {
  if (spirv.empty()) {
    error = "shader SPIR-V payload is empty";
    return false;
  }
  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = spirv.size_bytes();
  smci.pCode = spirv.data();
  const VkResult r = vkCreateShaderModule(device, &smci, nullptr, &out_mod);
  if (r != VK_SUCCESS) {
    error = vk_err("vkCreateShaderModule", r);
    return false;
  }
  return true;
}

struct SpecInfoStorage {
  std::vector<VkSpecializationMapEntry> entries;
  std::vector<uint32_t> data;
  VkSpecializationInfo info{};

  void build(std::span<const SpecConstantEntry> specs) {
    if (specs.empty()) return;
    entries.reserve(specs.size());
    data.reserve(specs.size());
    for (size_t i = 0; i < specs.size(); ++i) {
      VkSpecializationMapEntry me{};
      me.constantID = specs[i].constant_id;
      me.offset = static_cast<uint32_t>(i * sizeof(uint32_t));
      me.size = sizeof(uint32_t);
      entries.push_back(me);
      data.push_back(specs[i].value);
    }
    info.mapEntryCount = static_cast<uint32_t>(entries.size());
    info.pMapEntries = entries.data();
    info.dataSize = data.size() * sizeof(uint32_t);
    info.pData = data.data();
  }
};

}  // namespace

PipelineHandle VkDeviceImpl::create_graphics_pipeline(const GraphicsPipelineDesc& desc, std::string& error) {
  VkPipelineLayout vk_layout = VK_NULL_HANDLE;
  {
    const PipelineLayoutEntry* ple = lookup_pipeline_layout(desc.layout);
    if (!ple) {
      error = "create_graphics_pipeline: invalid PipelineLayoutHandle";
      return {};
    }
    vk_layout = ple->layout;
  }

  VkShaderModule vert_mod = VK_NULL_HANDLE;
  VkShaderModule frag_mod = VK_NULL_HANDLE;
  if (!create_shader_module(device_, desc.vertex_shader.spirv, vert_mod, error)) return {};
  if (!desc.fragment_shader.spirv.empty()) {
    if (!create_shader_module(device_, desc.fragment_shader.spirv, frag_mod, error)) {
      vkDestroyShaderModule(device_, vert_mod, nullptr);
      return {};
    }
  }

  SpecInfoStorage vert_spec, frag_spec;
  vert_spec.build(desc.vertex_shader.spec_constants);
  frag_spec.build(desc.fragment_shader.spec_constants);

  std::vector<VkPipelineShaderStageCreateInfo> stages;
  VkPipelineShaderStageCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vs.module = vert_mod;
  vs.pName = desc.vertex_shader.entry_point ? desc.vertex_shader.entry_point : "main";
  if (!vert_spec.entries.empty()) vs.pSpecializationInfo = &vert_spec.info;
  stages.push_back(vs);

  if (frag_mod) {
    VkPipelineShaderStageCreateInfo fs{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    fs.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fs.module = frag_mod;
    fs.pName = desc.fragment_shader.entry_point ? desc.fragment_shader.entry_point : "main";
    if (!frag_spec.entries.empty()) fs.pSpecializationInfo = &frag_spec.info;
    stages.push_back(fs);
  }

  std::vector<VkVertexInputBindingDescription> vbindings;
  vbindings.reserve(desc.vertex_bindings.size());
  for (const VertexBindingDesc& vb : desc.vertex_bindings) {
    vbindings.push_back({vb.binding, vb.stride,
                         vb.per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
  }
  std::vector<VkVertexInputAttributeDescription> vattrs;
  vattrs.reserve(desc.vertex_attributes.size());
  for (const VertexAttributeDesc& va : desc.vertex_attributes) {
    vattrs.push_back({va.location, va.binding, to_vk_format(va.format), va.offset});
  }

  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = static_cast<uint32_t>(vbindings.size());
  vi.pVertexBindingDescriptions = vbindings.data();
  vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(vattrs.size());
  vi.pVertexAttributeDescriptions = vattrs.data();

  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = to_vk_topology(desc.raster.topology);

  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.depthClampEnable = desc.raster.depth_clamp ? VK_TRUE : VK_FALSE;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = to_vk_cull_mode(desc.raster.cull_mode);
  rs.frontFace = to_vk_front_face(desc.raster.front_face);
  rs.depthBiasEnable = (desc.raster.depth_bias_enabled || desc.raster.dynamic_depth_bias) ? VK_TRUE : VK_FALSE;
  rs.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = to_vk_sample_count(desc.raster.sample_count);
  ms.alphaToCoverageEnable = desc.raster.alpha_to_coverage ? VK_TRUE : VK_FALSE;

  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = desc.depth.test_enabled ? VK_TRUE : VK_FALSE;
  ds.depthWriteEnable = desc.depth.write_enabled ? VK_TRUE : VK_FALSE;
  ds.depthCompareOp = to_vk_compare_op(desc.depth.compare_op);
  ds.depthBoundsTestEnable = desc.depth.bounds_test ? VK_TRUE : VK_FALSE;

  std::vector<VkFormat> color_vk_formats;
  color_vk_formats.reserve(desc.color_formats.size());
  for (Format f : desc.color_formats) color_vk_formats.push_back(to_vk_format(f));

  std::vector<VkPipelineColorBlendAttachmentState> blend_atts(color_vk_formats.size());
  for (size_t i = 0; i < color_vk_formats.size(); ++i) {
    const BlendState bs = (i < desc.color_blends.size()) ? desc.color_blends[i] : BlendState::opaque();
    VkPipelineColorBlendAttachmentState& a = blend_atts[i];
    a.blendEnable = bs.enabled ? VK_TRUE : VK_FALSE;
    a.srcColorBlendFactor = to_vk_blend_factor(bs.src_color);
    a.dstColorBlendFactor = to_vk_blend_factor(bs.dst_color);
    a.colorBlendOp = to_vk_blend_op(bs.color_op);
    a.srcAlphaBlendFactor = to_vk_blend_factor(bs.src_alpha);
    a.dstAlphaBlendFactor = to_vk_blend_factor(bs.dst_alpha);
    a.alphaBlendOp = to_vk_blend_op(bs.alpha_op);
    VkColorComponentFlags wm = 0;
    if (has_flag(bs.write_mask, ColorWriteMask::R)) wm |= VK_COLOR_COMPONENT_R_BIT;
    if (has_flag(bs.write_mask, ColorWriteMask::G)) wm |= VK_COLOR_COMPONENT_G_BIT;
    if (has_flag(bs.write_mask, ColorWriteMask::B)) wm |= VK_COLOR_COMPONENT_B_BIT;
    if (has_flag(bs.write_mask, ColorWriteMask::A)) wm |= VK_COLOR_COMPONENT_A_BIT;
    a.colorWriteMask = wm;
  }

  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = static_cast<uint32_t>(blend_atts.size());
  cb.pAttachments = blend_atts.data();

  std::vector<VkDynamicState> dyn_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  if (desc.raster.dynamic_depth_bias) dyn_states.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
  if (desc.dynamic_blend_constants) dyn_states.push_back(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
  if (desc.dynamic_shading_rate && caps_.fragment_shading_rate) {
    dyn_states.push_back(VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR);
  }
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = static_cast<uint32_t>(dyn_states.size());
  dyn.pDynamicStates = dyn_states.data();

  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = static_cast<uint32_t>(color_vk_formats.size());
  rendering.pColorAttachmentFormats = color_vk_formats.data();
  rendering.depthAttachmentFormat = to_vk_format(desc.depth_format);

  VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpci.pNext = &rendering;
  gpci.stageCount = static_cast<uint32_t>(stages.size());
  gpci.pStages = stages.data();
  gpci.pVertexInputState = &vi;
  gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vp;
  gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &ms;
  gpci.pDepthStencilState = &ds;
  gpci.pColorBlendState = &cb;
  gpci.pDynamicState = &dyn;
  gpci.layout = vk_layout;

  PipelineEntry entry{};
  entry.layout = vk_layout;
  entry.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
  const VkResult r = vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &gpci, nullptr, &entry.pipeline);

  vkDestroyShaderModule(device_, vert_mod, nullptr);
  if (frag_mod) vkDestroyShaderModule(device_, frag_mod, nullptr);

  if (r != VK_SUCCESS) {
    error = vk_err("vkCreateGraphicsPipelines", r);
    return {};
  }

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return pipelines_.allocate(std::move(entry));
}

PipelineHandle VkDeviceImpl::create_compute_pipeline(const ComputePipelineDesc& desc, std::string& error) {
  VkPipelineLayout vk_layout = VK_NULL_HANDLE;
  {
    const PipelineLayoutEntry* ple = lookup_pipeline_layout(desc.layout);
    if (!ple) {
      error = "create_compute_pipeline: invalid PipelineLayoutHandle";
      return {};
    }
    vk_layout = ple->layout;
  }

  VkShaderModule comp_mod = VK_NULL_HANDLE;
  if (!create_shader_module(device_, desc.compute_shader.spirv, comp_mod, error)) return {};

  SpecInfoStorage comp_spec;
  comp_spec.build(desc.compute_shader.spec_constants);

  VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpci.stage.module = comp_mod;
  cpci.stage.pName = desc.compute_shader.entry_point ? desc.compute_shader.entry_point : "main";
  if (!comp_spec.entries.empty()) cpci.stage.pSpecializationInfo = &comp_spec.info;
  cpci.layout = vk_layout;

  PipelineEntry entry{};
  entry.layout = vk_layout;
  entry.bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
  const VkResult r = vkCreateComputePipelines(device_, pipeline_cache_, 1, &cpci, nullptr, &entry.pipeline);
  vkDestroyShaderModule(device_, comp_mod, nullptr);

  if (r != VK_SUCCESS) {
    error = vk_err("vkCreateComputePipelines", r);
    return {};
  }

  std::lock_guard<std::mutex> lock(resource_mutex_);
  return pipelines_.allocate(std::move(entry));
}

void VkDeviceImpl::destroy_pipeline(PipelineHandle handle) {
  PipelineEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!pipelines_.remove(handle, &entry)) return;
  }
  if (entry.pipeline) vkDestroyPipeline(device_, entry.pipeline, nullptr);
}

// -----------------------------------------------------------------------------
// Query Pools
// -----------------------------------------------------------------------------
QueryPoolHandle VkDeviceImpl::create_query_pool(const QueryPoolDesc& desc, std::string& error) {
  if (desc.count == 0) {
    error = "create_query_pool: count must be > 0";
    return {};
  }
  VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  if (desc.type == QueryType::AccelStructCompactedSize) {
    if (!caps_.ray_query) {
      error = "create_query_pool: AccelStructCompactedSize requires ray tracing support";
      return {};
    }
    qpci.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
  } else {
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
  }
  qpci.queryCount = desc.count;

  QueryPoolEntry entry{};
  entry.type = desc.type;
  entry.count = desc.count;
  const VkResult r = vkCreateQueryPool(device_, &qpci, nullptr, &entry.pool);
  if (r != VK_SUCCESS) {
    error = vk_err("vkCreateQueryPool", r);
    return {};
  }
  std::lock_guard<std::mutex> lock(resource_mutex_);
  return query_pools_.allocate(std::move(entry));
}

void VkDeviceImpl::destroy_query_pool(QueryPoolHandle handle) {
  QueryPoolEntry entry{};
  {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    if (!query_pools_.remove(handle, &entry)) return;
  }
  if (entry.pool) vkDestroyQueryPool(device_, entry.pool, nullptr);
}

bool VkDeviceImpl::get_query_pool_results(QueryPoolHandle pool, uint32_t first, uint32_t count,
                                          std::span<uint64_t> out_values, bool wait) {
  const QueryPoolEntry* qe = lookup_query_pool(pool);
  if (!qe || count == 0 || first + count > qe->count || out_values.size() < count) return false;
  VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT;
  if (wait) flags |= VK_QUERY_RESULT_WAIT_BIT;
  const VkResult r = vkGetQueryPoolResults(device_, qe->pool, first, count,
                                           count * sizeof(uint64_t), out_values.data(),
                                           sizeof(uint64_t), flags);
  return r == VK_SUCCESS;
}

// -----------------------------------------------------------------------------
// Immediate submission (submit_once)
// -----------------------------------------------------------------------------
bool VkDeviceImpl::submit_once(const std::function<void(CommandList&)>& record_fn,
                               std::string& error, bool prefer_transfer_queue) {
  VkCommandPool pool = VK_NULL_HANDLE;
  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (!free_cmd_pools_.empty()) {
      pool = free_cmd_pools_.back();
      free_cmd_pools_.pop_back();
    }
  }
  if (!pool) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    const VkResult r = vkCreateCommandPool(device_, &pci, nullptr, &pool);
    if (r != VK_SUCCESS) {
      error = vk_err("vkCreateCommandPool(submit_once)", r);
      return false;
    }
    std::lock_guard<std::mutex> lock(pool_mutex_);
    all_cmd_pools_.push_back(pool);
  }

  auto recycle_pool = [&]() {
    vkResetCommandPool(device_, pool, 0);
    std::lock_guard<std::mutex> lock(pool_mutex_);
    free_cmd_pools_.push_back(pool);
  };

  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkResult r = vkAllocateCommandBuffers(device_, &cbai, &cmd);
  if (r != VK_SUCCESS) {
    recycle_pool();
    error = vk_err("vkAllocateCommandBuffers", r);
    return false;
  }

  VkCommandListImpl cmd_list(*this, cmd);
  cmd_list.begin();
  record_fn(cmd_list);
  cmd_list.end();

  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  r = vkCreateFence(device_, &fci, nullptr, &fence);
  if (r != VK_SUCCESS) {
    vkFreeCommandBuffers(device_, pool, 1, &cmd);
    recycle_pool();
    error = vk_err("vkCreateFence", r);
    return false;
  }

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;

  VkQueue target_queue = (prefer_transfer_queue && transfer_queue_) ? transfer_queue_ : graphics_queue_;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    r = vkQueueSubmit(target_queue, 1, &si, fence);
  }
  if (r == VK_SUCCESS) {
    r = vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
  }

  vkDestroyFence(device_, fence, nullptr);
  vkFreeCommandBuffers(device_, pool, 1, &cmd);
  recycle_pool();

  if (r != VK_SUCCESS) {
    error = vk_err("submit_once", r);
    return false;
  }
  return true;
}

std::unique_ptr<Swapchain> VkDeviceImpl::create_swapchain(const SwapchainDesc& desc, std::string& error) {
  auto sw = std::make_unique<VkSwapchainImpl>(*this);
  if (!sw->init(desc, error)) return nullptr;
  return sw;
}

}  // namespace vk

// -----------------------------------------------------------------------------
// Entry points called by rhi_factory.cpp
// -----------------------------------------------------------------------------
std::unique_ptr<Device> create_vulkan_device(const DeviceDesc& desc, std::string& error) {
  auto dev = std::make_unique<vk::VkDeviceImpl>();
  if (!dev->init(desc, error)) return nullptr;
  return dev;
}

void enumerate_vulkan_adapters(std::vector<AdapterInfo>& out) {
  std::string err;
  if (!vk::ensure_volk_initialized(err)) return;

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "shift-rhi-probe";
  app.pEngineName = "shift-engine";
  app.apiVersion = volkGetInstanceVersion() >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;

  std::vector<const char*> exts;
  uint32_t ext_count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
  std::vector<VkExtensionProperties> avail(ext_count);
  if (ext_count > 0) vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, avail.data());

  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  for (const auto& e : avail) {
    if (std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
      exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
      ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
      break;
    }
  }
  ici.enabledExtensionCount = static_cast<uint32_t>(exts.size());
  ici.ppEnabledExtensionNames = exts.data();

  VkInstance inst = VK_NULL_HANDLE;
  if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return;
  volkLoadInstance(inst);

  uint32_t count = 0;
  vkEnumeratePhysicalDevices(inst, &count, nullptr);
  std::vector<VkPhysicalDevice> phys(count);
  if (count > 0) vkEnumeratePhysicalDevices(inst, &count, phys.data());

  for (VkPhysicalDevice pd : phys) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint64_t vram = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; ++i) {
      if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += mp.memoryHeaps[i].size;
    }

    uint32_t dev_ext_count = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &dev_ext_count, nullptr);
    std::vector<VkExtensionProperties> dev_exts(dev_ext_count);
    if (dev_ext_count > 0) vkEnumerateDeviceExtensionProperties(pd, nullptr, &dev_ext_count, dev_exts.data());
    bool has_as = false, has_rq = false;
    for (const auto& e : dev_exts) {
      if (std::strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) has_as = true;
      if (std::strcmp(e.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0) has_rq = true;
    }

    AdapterInfo info{};
    info.backend = BackendKind::Vulkan;
    info.name = props.deviceName;
    info.vendor_id = props.vendorID;
    info.device_id = props.deviceID;
    info.device_local_bytes = vram;
    info.is_discrete = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
    info.is_integrated = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
    info.is_software = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU);
    info.supports_ray_query = has_as && has_rq;
    out.push_back(std::move(info));
  }

  vkDestroyInstance(inst, nullptr);
}

}  // namespace shift::rhi
