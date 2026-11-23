#include "rhi/rhi.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace shift::rhi {

#ifdef SHIFT_RHI_VULKAN
std::unique_ptr<Device> create_vulkan_device(const DeviceDesc& desc, std::string& error);
void enumerate_vulkan_adapters(std::vector<AdapterInfo>& out);
#endif

// -----------------------------------------------------------------------------
// Backend kind helpers
// -----------------------------------------------------------------------------
const char* backend_kind_name(BackendKind kind) noexcept {
  switch (kind) {
    case BackendKind::Auto: return "auto";
    case BackendKind::Vulkan: return "vulkan";
    case BackendKind::Metal: return "metal";
    case BackendKind::D3D12: return "d3d12";
  }
  return "auto";
}

bool try_parse_backend_kind(std::string_view text, BackendKind& out) noexcept {
  // Trim leading/trailing ASCII whitespace.
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  if (text.empty()) return false;

  std::string lower;
  lower.reserve(text.size());
  for (char c : text) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

  if (lower == "auto" || lower == "default") {
    out = BackendKind::Auto;
    return true;
  }
  if (lower == "vulkan" || lower == "vk") {
    out = BackendKind::Vulkan;
    return true;
  }
  if (lower == "metal" || lower == "mtl") {
    out = BackendKind::Metal;
    return true;
  }
  if (lower == "d3d12" || lower == "dx12" || lower == "directx12" || lower == "direct3d12") {
    out = BackendKind::D3D12;
    return true;
  }
  return false;
}

BackendKind parse_backend_kind(std::string_view text, BackendKind fallback) noexcept {
  BackendKind parsed = fallback;
  if (try_parse_backend_kind(text, parsed)) return parsed;
  return fallback;
}

bool is_backend_compiled(BackendKind kind) noexcept {
  switch (kind) {
    case BackendKind::Vulkan:
#ifdef SHIFT_RHI_VULKAN
      return true;
#else
      return false;
#endif
    case BackendKind::Metal:
#ifdef SHIFT_RHI_METAL
      return true;
#else
      return false;
#endif
    case BackendKind::D3D12:
#ifdef SHIFT_RHI_D3D12
      return true;
#else
      return false;
#endif
    case BackendKind::Auto:
      return is_backend_compiled(BackendKind::Vulkan) ||
             is_backend_compiled(BackendKind::Metal) ||
             is_backend_compiled(BackendKind::D3D12);
  }
  return false;
}

std::vector<BackendKind> resolve_backend_order(BackendKind requested) {
  std::vector<BackendKind> order;
  auto push_if_compiled = [&](BackendKind k) {
    if (k == BackendKind::Auto) return;
    if (!is_backend_compiled(k)) return;
    if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
  };

  if (requested != BackendKind::Auto) {
    push_if_compiled(requested);
    return order;
  }

  // Platform preference order matching the RHI architecture specification:
  // macOS/iOS prefer Metal when compiled, then Vulkan.
  // Windows/Linux/Android currently prefer Vulkan (reference backend) in RHI-1,
  // with D3D12 added as fallback/peer once RHI-4 lands.
#if defined(__APPLE__)
  push_if_compiled(BackendKind::Metal);
  push_if_compiled(BackendKind::Vulkan);
#elif defined(_WIN32)
  push_if_compiled(BackendKind::Vulkan);
  push_if_compiled(BackendKind::D3D12);
#else
  push_if_compiled(BackendKind::Vulkan);
#endif
  return order;
}

// -----------------------------------------------------------------------------
// Format metadata tables (all 28 concrete engine formats + Undefined)
// -----------------------------------------------------------------------------
struct FormatInfo {
  const char* name;
  uint32_t bytes_per_block;
  uint32_t block_extent;
  bool is_depth;
  bool is_srgb;
  bool is_compressed;
};

static constexpr FormatInfo kFormatTable[kFormatCount] = {
    {"Undefined",    0,  1, false, false, false},  // Undefined
    {"R8Unorm",      1,  1, false, false, false},  // R8Unorm
    {"Rgba8Unorm",   4,  1, false, false, false},  // Rgba8Unorm
    {"Rgba8Srgb",    4,  1, false, true,  false},  // Rgba8Srgb
    {"Rgba8Snorm",   4,  1, false, false, false},  // Rgba8Snorm
    {"Bgra8Unorm",   4,  1, false, false, false},  // Bgra8Unorm
    {"Bgra8Srgb",    4,  1, false, true,  false},  // Bgra8Srgb
    {"Rgb10A2Unorm", 4,  1, false, false, false},  // Rgb10A2Unorm
    {"Rg16Unorm",    4,  1, false, false, false},  // Rg16Unorm
    {"Rg16Sfloat",   4,  1, false, false, false},  // Rg16Sfloat
    {"Rgba16Sfloat", 8,  1, false, false, false},  // Rgba16Sfloat
    {"Rgba16Snorm",  8,  1, false, false, false},  // Rgba16Snorm
    {"Rgba16Sint",   8,  1, false, false, false},  // Rgba16Sint
    {"R32Sfloat",    4,  1, false, false, false},  // R32Sfloat
    {"R32Uint",      4,  1, false, false, false},  // R32Uint
    {"Rg32Sfloat",   8,  1, false, false, false},  // Rg32Sfloat
    {"Rgb32Sfloat",  12, 1, false, false, false},  // Rgb32Sfloat
    {"Rgba32Sfloat", 16, 1, false, false, false},  // Rgba32Sfloat
    {"Rgba32Uint",   16, 1, false, false, false},  // Rgba32Uint
    {"D32Sfloat",    4,  1, true,  false, false},  // D32Sfloat
    {"D16Unorm",     2,  1, true,  false, false},  // D16Unorm
    {"Bc1RgbaUnorm", 8,  4, false, false, true},   // Bc1RgbaUnorm
    {"Bc1RgbaSrgb",  8,  4, false, true,  true},   // Bc1RgbaSrgb
    {"Bc2Unorm",     16, 4, false, false, true},   // Bc2Unorm
    {"Bc2Srgb",      16, 4, false, true,  true},   // Bc2Srgb
    {"Bc3Unorm",     16, 4, false, false, true},   // Bc3Unorm
    {"Bc3Srgb",      16, 4, false, true,  true},   // Bc3Srgb
    {"Bc4Unorm",     8,  4, false, false, true},   // Bc4Unorm
    {"Bc5Unorm",     16, 4, false, false, true},   // Bc5Unorm
};

static const FormatInfo& format_info(Format fmt) noexcept {
  const uint32_t idx = static_cast<uint32_t>(fmt);
  return idx < kFormatCount ? kFormatTable[idx] : kFormatTable[0];
}

const char* format_name(Format fmt) noexcept { return format_info(fmt).name; }
uint32_t format_bytes_per_block(Format fmt) noexcept { return format_info(fmt).bytes_per_block; }
uint32_t format_block_extent(Format fmt) noexcept { return format_info(fmt).block_extent; }
bool format_is_depth(Format fmt) noexcept { return format_info(fmt).is_depth; }
bool format_is_srgb(Format fmt) noexcept { return format_info(fmt).is_srgb; }
bool format_is_compressed(Format fmt) noexcept { return format_info(fmt).is_compressed; }

uint64_t format_image_bytes(Format fmt, uint32_t width, uint32_t height, uint32_t depth) noexcept {
  const FormatInfo& info = format_info(fmt);
  if (info.bytes_per_block == 0 || width == 0 || height == 0 || depth == 0) return 0;
  const uint64_t bw = (static_cast<uint64_t>(width) + info.block_extent - 1ull) / info.block_extent;
  const uint64_t bh = (static_cast<uint64_t>(height) + info.block_extent - 1ull) / info.block_extent;
  return bw * bh * static_cast<uint64_t>(depth) * static_cast<uint64_t>(info.bytes_per_block);
}

// -----------------------------------------------------------------------------
// Pipeline cache binary envelope
// -----------------------------------------------------------------------------
namespace {

constexpr uint32_t kCacheMagic = 0x43504853u;  // "SHPC" little-endian
constexpr uint32_t kCacheVersion = 1u;

struct PipelineCacheHeader {
  uint32_t magic = kCacheMagic;
  uint32_t version = kCacheVersion;
  uint32_t backend = 0;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  uint32_t reserved = 0;
  uint8_t uuid[16]{};
  uint64_t payload_bytes = 0;
  uint64_t payload_hash = 0;
};
static_assert(sizeof(PipelineCacheHeader) == 56);

uint64_t fnv1a_64(const uint8_t* data, size_t size) noexcept {
  uint64_t h = 14695981039346656037ull;
  for (size_t i = 0; i < size; ++i) {
    h ^= static_cast<uint64_t>(data[i]);
    h *= 1099511628211ull;
  }
  return h;
}

}  // namespace

std::vector<uint8_t> encode_pipeline_cache_envelope(const PipelineCacheKey& key,
                                                    const uint8_t* payload, size_t payload_bytes) {
  PipelineCacheHeader hdr{};
  hdr.backend = static_cast<uint32_t>(key.backend);
  hdr.vendor_id = key.vendor_id;
  hdr.device_id = key.device_id;
  std::memcpy(hdr.uuid, key.uuid.data(), sizeof(hdr.uuid));
  hdr.payload_bytes = static_cast<uint64_t>(payload_bytes);
  hdr.payload_hash = (payload && payload_bytes > 0) ? fnv1a_64(payload, payload_bytes) : 0ull;

  std::vector<uint8_t> out(sizeof(PipelineCacheHeader) + payload_bytes);
  std::memcpy(out.data(), &hdr, sizeof(PipelineCacheHeader));
  if (payload && payload_bytes > 0) {
    std::memcpy(out.data() + sizeof(PipelineCacheHeader), payload, payload_bytes);
  }
  return out;
}

bool decode_pipeline_cache_envelope(const uint8_t* file_bytes, size_t file_size,
                                    const PipelineCacheKey& expected_key,
                                    std::vector<uint8_t>& out_payload) {
  out_payload.clear();
  if (!file_bytes || file_size < sizeof(PipelineCacheHeader)) return false;

  PipelineCacheHeader hdr{};
  std::memcpy(&hdr, file_bytes, sizeof(PipelineCacheHeader));
  if (hdr.magic != kCacheMagic || hdr.version != kCacheVersion) return false;
  if (hdr.backend != static_cast<uint32_t>(expected_key.backend)) return false;
  if (hdr.vendor_id != expected_key.vendor_id || hdr.device_id != expected_key.device_id) return false;
  if (std::memcmp(hdr.uuid, expected_key.uuid.data(), sizeof(hdr.uuid)) != 0) return false;
  if (hdr.payload_bytes == 0 || hdr.payload_bytes > file_size - sizeof(PipelineCacheHeader)) return false;

  const uint8_t* payload = file_bytes + sizeof(PipelineCacheHeader);
  const size_t n = static_cast<size_t>(hdr.payload_bytes);
  if (fnv1a_64(payload, n) != hdr.payload_hash) return false;

  out_payload.assign(payload, payload + n);
  return true;
}

// -----------------------------------------------------------------------------
// Adapter enumeration & Device factory
// -----------------------------------------------------------------------------
std::vector<AdapterInfo> enumerate_adapters(BackendKind filter) {
  std::vector<AdapterInfo> adapters;
  const bool want_vk = (filter == BackendKind::Auto || filter == BackendKind::Vulkan);
#ifdef SHIFT_RHI_VULKAN
  if (want_vk) enumerate_vulkan_adapters(adapters);
#else
  (void)want_vk;
#endif
  return adapters;
}

std::unique_ptr<Device> create_device(const DeviceDesc& desc, std::string& error) {
  error.clear();
  BackendKind effective = desc.backend;
  if (effective == BackendKind::Auto) {
    if (const char* env = std::getenv("SHIFT_BACKEND"); env && *env) {
      BackendKind parsed = BackendKind::Auto;
      if (try_parse_backend_kind(env, parsed)) effective = parsed;
    }
  }

  if (effective != BackendKind::Auto && !is_backend_compiled(effective)) {
    error = std::string("requested RHI backend '") + backend_kind_name(effective) +
            "' is not compiled into this build";
    return nullptr;
  }

  const std::vector<BackendKind> candidates = resolve_backend_order(effective);
  if (candidates.empty()) {
    error = "no RHI graphics backend is compiled into this build";
    return nullptr;
  }

  std::string errors;
  for (BackendKind candidate : candidates) {
    std::string attempt_err;
    std::unique_ptr<Device> dev;
    switch (candidate) {
      case BackendKind::Vulkan:
#ifdef SHIFT_RHI_VULKAN
        dev = create_vulkan_device(desc, attempt_err);
#else
        attempt_err = "vulkan backend not compiled";
#endif
        break;
      case BackendKind::Metal:
        attempt_err = "metal backend not compiled";
        break;
      case BackendKind::D3D12:
        attempt_err = "d3d12 backend not compiled";
        break;
      case BackendKind::Auto:
        break;
    }
    if (dev) return dev;
    if (!errors.empty()) errors += "; ";
    errors += std::string(backend_kind_name(candidate)) + ": " + attempt_err;
  }

  error = errors.empty() ? "failed to initialize any RHI backend" : errors;
  return nullptr;
}

}  // namespace shift::rhi
