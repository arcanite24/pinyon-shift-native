// Thin Modern RHI (libs/rhi/) core value types, enums, capability structs,
// format helpers, and typed generational handles. Zero GPU API headers leak
// into this header.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace shift::rhi {

// -----------------------------------------------------------------------------
// Bitmask enum utilities
// -----------------------------------------------------------------------------
template <typename E>
struct EnableBitmask : std::false_type {};

template <typename E>
inline constexpr bool kEnableBitmask = EnableBitmask<E>::value;

template <typename E>
requires kEnableBitmask<E>
constexpr E operator|(E a, E b) noexcept {
  using U = std::underlying_type_t<E>;
  return static_cast<E>(static_cast<U>(a) | static_cast<U>(b));
}

template <typename E>
requires kEnableBitmask<E>
constexpr E operator&(E a, E b) noexcept {
  using U = std::underlying_type_t<E>;
  return static_cast<E>(static_cast<U>(a) & static_cast<U>(b));
}

template <typename E>
requires kEnableBitmask<E>
constexpr E operator^(E a, E b) noexcept {
  using U = std::underlying_type_t<E>;
  return static_cast<E>(static_cast<U>(a) ^ static_cast<U>(b));
}

template <typename E>
requires kEnableBitmask<E>
constexpr E operator~(E a) noexcept {
  using U = std::underlying_type_t<E>;
  return static_cast<E>(~static_cast<U>(a));
}

template <typename E>
requires kEnableBitmask<E>
constexpr E& operator|=(E& a, E b) noexcept {
  a = a | b;
  return a;
}

template <typename E>
requires kEnableBitmask<E>
constexpr E& operator&=(E& a, E b) noexcept {
  a = a & b;
  return a;
}

template <typename E>
requires kEnableBitmask<E>
constexpr bool has_flag(E value, E flag) noexcept {
  using U = std::underlying_type_t<E>;
  return (static_cast<U>(value) & static_cast<U>(flag)) != 0;
}

template <typename E>
requires kEnableBitmask<E>
constexpr bool any_flag(E value) noexcept {
  using U = std::underlying_type_t<E>;
  return static_cast<U>(value) != 0;
}

// -----------------------------------------------------------------------------
// Backend selection
// -----------------------------------------------------------------------------
enum class BackendKind : uint8_t {
  Auto = 0,
  Vulkan = 1,
  Metal = 2,
  D3D12 = 3,
};

const char* backend_kind_name(BackendKind kind) noexcept;
// Case-insensitive parser accepting "auto", "vulkan"/"vk", "metal"/"mtl",
// "d3d12"/"dx12"/"directx12". Unknown tokens return fallback (default Auto).
BackendKind parse_backend_kind(std::string_view text, BackendKind fallback = BackendKind::Auto) noexcept;
// Returns true if text matched a recognized backend token.
bool try_parse_backend_kind(std::string_view text, BackendKind& out) noexcept;
// Compile-time / runtime availability check for a concrete backend.
bool is_backend_compiled(BackendKind kind) noexcept;
// Resolves Auto (or falls back from an unavailable requested backend) to the
// concrete backend order to attempt on the current platform.
std::vector<BackendKind> resolve_backend_order(BackendKind requested);

// -----------------------------------------------------------------------------
// Typed generational handles (64-bit, O(1) dense slot-map lookup, stale-safe)
// -----------------------------------------------------------------------------
template <typename Tag>
struct Handle {
  uint32_t index = 0;       // 0 == null / invalid
  uint32_t generation = 0;  // monotonically incremented on slot reuse

  constexpr bool valid() const noexcept { return index != 0; }
  explicit constexpr operator bool() const noexcept { return valid(); }
  constexpr bool operator==(const Handle&) const noexcept = default;
  constexpr uint64_t raw() const noexcept {
    return (static_cast<uint64_t>(generation) << 32) | static_cast<uint64_t>(index);
  }
  static constexpr Handle from_raw(uint64_t bits) noexcept {
    return Handle{static_cast<uint32_t>(bits & 0xFFFFFFFFull), static_cast<uint32_t>(bits >> 32)};
  }
};

struct BufferTag {};
struct TextureTag {};
struct TextureViewTag {};
struct SamplerTag {};
struct BindGroupLayoutTag {};
struct BindGroupTag {};
struct PipelineLayoutTag {};
struct PipelineTag {};
struct QueryPoolTag {};
struct AccelStructTag {};
struct FenceTag {};
struct TimelineSemaphoreTag {};

using BufferHandle            = Handle<BufferTag>;
using TextureHandle           = Handle<TextureTag>;
using TextureViewHandle       = Handle<TextureViewTag>;
using SamplerHandle           = Handle<SamplerTag>;
using BindGroupLayoutHandle   = Handle<BindGroupLayoutTag>;
using BindGroupHandle         = Handle<BindGroupTag>;
using PipelineLayoutHandle    = Handle<PipelineLayoutTag>;
using PipelineHandle          = Handle<PipelineTag>;
using QueryPoolHandle         = Handle<QueryPoolTag>;
using AccelStructHandle       = Handle<AccelStructTag>;
using FenceHandle             = Handle<FenceTag>;
using TimelineSemaphoreHandle = Handle<TimelineSemaphoreTag>;

// -----------------------------------------------------------------------------
// Hardware command queue classes
// -----------------------------------------------------------------------------
enum class QueueKind : uint8_t {
  Graphics = 0,      // Direct / Graphics + Compute + Transfer + Present
  AsyncCompute = 1,  // Dedicated async compute queue (falls back to Graphics)
  Transfer = 2,      // Dedicated DMA copy queue (falls back to Graphics)
};

const char* queue_kind_name(QueueKind kind) noexcept;

// Reusable dense generational slot pool (header-only so unit tests and all
// backends share identical handle generation semantics).
template <typename HandleT, typename EntryT>
class HandlePool {
 public:
  HandleT allocate(EntryT entry) {
    if (!free_slots_.empty()) {
      const uint32_t idx = free_slots_.back();
      free_slots_.pop_back();
      Slot& s = slots_[idx - 1u];
      s.alive = true;
      s.entry = std::move(entry);
      ++live_count_;
      return HandleT{idx, s.generation};
    }
    const uint32_t idx = static_cast<uint32_t>(slots_.size()) + 1u;
    slots_.push_back(Slot{1u, true, std::move(entry)});
    ++live_count_;
    return HandleT{idx, 1u};
  }

  EntryT* get(HandleT h) noexcept {
    if (!h.valid() || h.index > slots_.size()) return nullptr;
    Slot& s = slots_[h.index - 1u];
    if (!s.alive || s.generation != h.generation) return nullptr;
    return &s.entry;
  }

  const EntryT* get(HandleT h) const noexcept {
    if (!h.valid() || h.index > slots_.size()) return nullptr;
    const Slot& s = slots_[h.index - 1u];
    if (!s.alive || s.generation != h.generation) return nullptr;
    return &s.entry;
  }

  bool remove(HandleT h, EntryT* out_entry = nullptr) {
    if (!h.valid() || h.index > slots_.size()) return false;
    Slot& s = slots_[h.index - 1u];
    if (!s.alive || s.generation != h.generation) return false;
    if (out_entry) *out_entry = std::move(s.entry);
    s.entry = EntryT{};
    s.alive = false;
    ++s.generation;
    if (s.generation == 0u) s.generation = 1u;  // never collide with 0
    free_slots_.push_back(h.index);
    --live_count_;
    return true;
  }

  template <typename Fn>
  void for_each_alive(Fn&& fn) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(slots_.size()); ++i) {
      if (slots_[i].alive) {
        fn(HandleT{i + 1u, slots_[i].generation}, slots_[i].entry);
      }
    }
  }

  void clear() {
    slots_.clear();
    free_slots_.clear();
    live_count_ = 0;
  }

  uint32_t live_count() const noexcept { return live_count_; }
  uint32_t capacity() const noexcept { return static_cast<uint32_t>(slots_.size()); }

 private:
  struct Slot {
    uint32_t generation = 1u;
    bool alive = false;
    EntryT entry{};
  };
  std::vector<Slot> slots_;
  std::vector<uint32_t> free_slots_;
  uint32_t live_count_ = 0;
};

// -----------------------------------------------------------------------------
// Engine formats (all 28 concrete engine formats + Undefined)
// -----------------------------------------------------------------------------
enum class Format : uint8_t {
  Undefined = 0,
  // Color (18)
  R8Unorm,
  Rgba8Unorm,
  Rgba8Srgb,
  Rgba8Snorm,
  Bgra8Unorm,
  Bgra8Srgb,
  Rgb10A2Unorm,
  Rg16Unorm,
  Rg16Sfloat,
  Rgba16Sfloat,
  Rgba16Snorm,
  Rgba16Sint,
  R32Sfloat,
  R32Uint,
  Rg32Sfloat,
  Rg32Uint,
  Rgb32Sfloat,
  Rgba32Sfloat,
  Rgba32Uint,
  // Depth (2)
  D32Sfloat,
  D16Unorm,
  // Block-compressed (8)
  Bc1RgbaUnorm,
  Bc1RgbaSrgb,
  Bc2Unorm,
  Bc2Srgb,
  Bc3Unorm,
  Bc3Srgb,
  Bc4Unorm,
  Bc5Unorm,

  Count
};

constexpr uint32_t kFormatCount = static_cast<uint32_t>(Format::Count);

const char* format_name(Format fmt) noexcept;
uint32_t format_bytes_per_block(Format fmt) noexcept;
uint32_t format_block_extent(Format fmt) noexcept;
bool format_is_depth(Format fmt) noexcept;
bool format_is_srgb(Format fmt) noexcept;
bool format_is_compressed(Format fmt) noexcept;
uint64_t format_image_bytes(Format fmt, uint32_t width, uint32_t height, uint32_t depth = 1) noexcept;

// -----------------------------------------------------------------------------
// Resource usage, memory hints, texture/view/sampler enums
// -----------------------------------------------------------------------------
enum class BufferUsage : uint32_t {
  None               = 0,
  TransferSrc        = 1u << 0,
  TransferDst        = 1u << 1,
  Uniform            = 1u << 2,
  Storage            = 1u << 3,
  Index              = 1u << 4,
  Vertex             = 1u << 5,
  Indirect           = 1u << 6,
  DeviceAddress      = 1u << 7,
  AccelStructInput   = 1u << 8,
  AccelStructStorage = 1u << 9,
};
template <> struct EnableBitmask<BufferUsage> : std::true_type {};

enum class TextureUsage : uint32_t {
  None            = 0,
  TransferSrc     = 1u << 0,
  TransferDst     = 1u << 1,
  Sampled         = 1u << 2,
  Storage         = 1u << 3,
  ColorAttachment = 1u << 4,
  DepthAttachment = 1u << 5,
  Transient       = 1u << 6,
};
template <> struct EnableBitmask<TextureUsage> : std::true_type {};

enum class MemoryHint : uint8_t {
  GpuOnly = 0,   // VMA AUTO_PREFER_DEVICE / MTLStorageModePrivate / D3D12 DEFAULT
  CpuToGpu = 1,  // VMA HOST_SEQUENTIAL_WRITE / MTLStorageModeShared / D3D12 UPLOAD
  GpuToCpu = 2,  // VMA HOST_RANDOM / MTLStorageModeShared / D3D12 READBACK
  Transient = 3, // VMA LAZILY_ALLOCATED / MTLStorageModeMemoryless / D3D12 DEFAULT
};

enum class TextureType : uint8_t {
  Tex2D = 0,
  TexCube = 1,
  Tex3D = 2,
};

enum class ComponentSwizzle : uint8_t {
  Identity = 0,
  Zero,
  One,
  R,
  G,
  B,
  A,
};

struct SwizzleMapping {
  ComponentSwizzle r = ComponentSwizzle::Identity;
  ComponentSwizzle g = ComponentSwizzle::Identity;
  ComponentSwizzle b = ComponentSwizzle::Identity;
  ComponentSwizzle a = ComponentSwizzle::Identity;
};

enum class Filter : uint8_t {
  Nearest = 0,
  Linear = 1,
};

enum class AddressMode : uint8_t {
  Repeat = 0,
  MirroredRepeat = 1,
  ClampToEdge = 2,
  ClampToBorder = 3,
};

enum class BorderColor : uint8_t {
  TransparentBlack = 0,
  OpaqueBlack = 1,
  OpaqueWhite = 2,
};

enum class CompareOp : uint8_t {
  Never = 0,
  Less,
  Equal,
  LessOrEqual,
  Greater,
  NotEqual,
  GreaterOrEqual,
  Always,
};

enum class IndexType : uint8_t {
  Uint16 = 0,
  Uint32 = 1,
};

// -----------------------------------------------------------------------------
// Synchronization: PipelineStage, AccessFlags, ResourceState
// -----------------------------------------------------------------------------
enum class PipelineStage : uint32_t {
  None                = 0,
  TopOfPipe           = 1u << 0,
  DrawIndirect        = 1u << 1,
  VertexInput         = 1u << 2,
  VertexShader        = 1u << 3,
  FragmentShader      = 1u << 4,
  EarlyFragmentTests  = 1u << 5,
  LateFragmentTests   = 1u << 6,
  ColorAttachmentOut  = 1u << 7,
  ComputeShader       = 1u << 8,
  Transfer            = 1u << 9,
  Copy                = 1u << 10,
  Blit                = 1u << 11,
  Clear               = 1u << 12,
  AccelStructBuild    = 1u << 13,
  AccelStructCopy     = 1u << 14,
  Host                = 1u << 15,
  BottomOfPipe        = 1u << 16,
  AllGraphics         = 1u << 17,
  AllCommands         = 1u << 18,
};
template <> struct EnableBitmask<PipelineStage> : std::true_type {};

enum class AccessFlags : uint32_t {
  None                = 0,
  IndirectCommandRead = 1u << 0,
  IndexRead           = 1u << 1,
  VertexAttributeRead = 1u << 2,
  UniformRead         = 1u << 3,
  ShaderRead          = 1u << 4,
  ShaderWrite         = 1u << 5,
  ColorAttachmentRead = 1u << 6,
  ColorAttachmentWrite = 1u << 7,
  DepthAttachmentRead = 1u << 8,
  DepthAttachmentWrite = 1u << 9,
  TransferRead        = 1u << 10,
  TransferWrite       = 1u << 11,
  HostRead            = 1u << 12,
  HostWrite           = 1u << 13,
  AccelStructRead     = 1u << 14,
  AccelStructWrite    = 1u << 15,
  MemoryRead          = 1u << 16,
  MemoryWrite         = 1u << 17,
};
template <> struct EnableBitmask<AccessFlags> : std::true_type {};

enum class ResourceState : uint32_t {
  Undefined         = 0,
  Common            = 1u << 0,
  VertexBuffer      = 1u << 1,
  IndexBuffer       = 1u << 2,
  UniformBuffer     = 1u << 3,
  ShaderRead        = 1u << 4,
  StorageReadWrite  = 1u << 5,
  RenderTarget      = 1u << 6,
  DepthWrite        = 1u << 7,
  DepthRead         = 1u << 8,
  IndirectArg       = 1u << 9,
  CopySrc           = 1u << 10,
  CopyDst           = 1u << 11,
  AccelStructRead   = 1u << 12,
  AccelStructWrite  = 1u << 13,
  Present           = 1u << 14,
  HostRead          = 1u << 15,
};
template <> struct EnableBitmask<ResourceState> : std::true_type {};

// -----------------------------------------------------------------------------
// Pipeline state: ShaderStage, BlendState, DepthState, RasterState
// -----------------------------------------------------------------------------
enum class ShaderStage : uint32_t {
  None     = 0,
  Vertex   = 1u << 0,
  Fragment = 1u << 1,
  Compute  = 1u << 2,
  AllGraphics = Vertex | Fragment,
  All      = Vertex | Fragment | Compute,
};
template <> struct EnableBitmask<ShaderStage> : std::true_type {};

enum class BlendFactor : uint8_t {
  Zero = 0,
  One,
  SrcColor,
  OneMinusSrcColor,
  DstColor,
  OneMinusDstColor,
  SrcAlpha,
  OneMinusSrcAlpha,
  DstAlpha,
  OneMinusDstAlpha,
  ConstantColor,
  OneMinusConstantColor,
};

enum class BlendOp : uint8_t {
  Add = 0,
  Subtract,
  ReverseSubtract,
  Min,
  Max,
};

enum class ColorWriteMask : uint8_t {
  None = 0,
  R    = 1u << 0,
  G    = 1u << 1,
  B    = 1u << 2,
  A    = 1u << 3,
  All  = R | G | B | A,
};
template <> struct EnableBitmask<ColorWriteMask> : std::true_type {};

struct BlendState {
  bool enabled = false;
  BlendFactor src_color = BlendFactor::One;
  BlendFactor dst_color = BlendFactor::Zero;
  BlendOp color_op = BlendOp::Add;
  BlendFactor src_alpha = BlendFactor::One;
  BlendFactor dst_alpha = BlendFactor::Zero;
  BlendOp alpha_op = BlendOp::Add;
  ColorWriteMask write_mask = ColorWriteMask::All;

  static constexpr BlendState opaque() noexcept { return BlendState{}; }
  static constexpr BlendState alpha_blend() noexcept {
    BlendState b{};
    b.enabled = true;
    b.src_color = BlendFactor::SrcAlpha;
    b.dst_color = BlendFactor::OneMinusSrcAlpha;
    b.src_alpha = BlendFactor::One;
    b.dst_alpha = BlendFactor::OneMinusSrcAlpha;
    return b;
  }
  static constexpr BlendState premultiplied_alpha() noexcept {
    BlendState b{};
    b.enabled = true;
    b.src_color = BlendFactor::One;
    b.dst_color = BlendFactor::OneMinusSrcAlpha;
    b.src_alpha = BlendFactor::One;
    b.dst_alpha = BlendFactor::OneMinusSrcAlpha;
    return b;
  }
  static constexpr BlendState additive() noexcept {
    BlendState b{};
    b.enabled = true;
    b.src_color = BlendFactor::SrcAlpha;
    b.dst_color = BlendFactor::One;
    b.src_alpha = BlendFactor::Zero;
    b.dst_alpha = BlendFactor::One;
    return b;
  }
};

struct DepthState {
  bool test_enabled = false;
  bool write_enabled = false;
  CompareOp compare_op = CompareOp::GreaterOrEqual;  // Reversed-Z default
  bool bounds_test = false;
};

enum class CullMode : uint8_t {
  None = 0,
  Front = 1,
  Back = 2,
};

enum class FrontFace : uint8_t {
  CounterClockwise = 0,
  Clockwise = 1,
};

enum class PrimitiveTopology : uint8_t {
  TriangleList = 0,
  TriangleStrip = 1,
  LineList = 2,
};

struct RasterState {
  CullMode cull_mode = CullMode::Back;
  FrontFace front_face = FrontFace::CounterClockwise;
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;
  bool depth_clamp = false;
  bool depth_bias_enabled = false;
  bool dynamic_depth_bias = false;
  bool alpha_to_coverage = false;
  uint32_t sample_count = 1;
};

// -----------------------------------------------------------------------------
// Render pass & presentation enums
// -----------------------------------------------------------------------------
enum class LoadOp : uint8_t {
  Load = 0,
  Clear = 1,
  DontCare = 2,
};

enum class StoreOp : uint8_t {
  Store = 0,
  DontCare = 1,
  MultisampleResolve = 2,
  StoreAndResolve = 3,
};

enum class ResolveMode : uint8_t {
  None = 0,
  Average = 1,
  SampleZero = 2,
};

enum class PresentMode : uint8_t {
  Fifo = 0,
  Mailbox = 1,
  Immediate = 2,
};

enum class HdrMode : uint8_t {
  Off = 0,
  Hdr10 = 1,  // Rgb10A2Unorm + ST 2084 PQ (BT.2020)
  ScRgb = 2,  // Rgba16Sfloat + Extended Linear sRGB
  Edr = 3,    // Apple XDR EDR (Metal)
};

enum class QueryType : uint8_t {
  Timestamp = 0,
  AccelStructCompactedSize = 1,
};

// -----------------------------------------------------------------------------
// Capabilities, adapter info, VRAM telemetry, pipeline cache envelope
// -----------------------------------------------------------------------------
struct BackendCaps {
  BackendKind backend = BackendKind::Vulkan;
  std::string device_name;
  std::string driver_name;
  std::string driver_info;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  uint32_t api_version = 0;
  uint32_t driver_version = 0;
  std::array<uint8_t, 16> pipeline_cache_uuid{};
  uint64_t device_local_bytes = 0;

  bool is_discrete = false;
  bool is_integrated = false;
  bool is_software = false;
  bool is_mobile_tbdr = false;
  bool is_moltenvk = false;
  bool is_turnip = false;

  bool dynamic_rendering = false;
  bool synchronization2 = false;
  bool timeline_semaphores = false;
  bool dedicated_async_compute = false;
  bool dedicated_transfer_queue = false;
  bool subgroup_ops = false;
  bool mesh_shaders = false;
  bool bindless = false;
  uint32_t max_bindless_textures = 0;
  bool indirect_count = false;
  bool multi_draw_indirect = false;
  bool depth_resolve_sample_zero = false;
  bool fragment_shading_rate = false;
  bool timestamps = false;
  double timestamp_period_ns = 0.0;
  float max_anisotropy = 1.0f;
  uint32_t supported_msaa_mask = 1u;  // bitmask of sample counts (1, 2, 4, 8)
  bool lazy_memory = false;

  bool hdr10 = false;
  bool scrgb = false;
  bool edr = false;
  float edr_max_headroom = 1.0f;

  bool buffer_device_address = false;
  bool ray_query = false;
  bool rt_pipeline = false;
  bool rt_maintenance1 = false;
  bool opacity_micromap = false;
  bool invocation_reorder = false;
  uint32_t min_as_scratch_alignment = 256u;
};

struct AdapterInfo {
  BackendKind backend = BackendKind::Vulkan;
  std::string name;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  uint64_t device_local_bytes = 0;
  bool is_discrete = false;
  bool is_integrated = false;
  bool is_software = false;
  bool supports_ray_query = false;
};

struct VramStats {
  uint32_t block_count = 0;
  uint32_t allocation_count = 0;
  uint64_t block_bytes = 0;
  uint64_t allocation_bytes = 0;
  uint64_t device_local_usage = 0;
  uint64_t device_local_budget = 0;
};

struct PipelineCacheKey {
  BackendKind backend = BackendKind::Vulkan;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  std::array<uint8_t, 16> uuid{};
};

// Versioned binary envelope for persistent pipeline caches (validates backend,
// vendor/device IDs, driver UUID, and payload checksum before feeding to API).
std::vector<uint8_t> encode_pipeline_cache_envelope(const PipelineCacheKey& key,
                                                    const uint8_t* payload, size_t payload_bytes);
bool decode_pipeline_cache_envelope(const uint8_t* file_bytes, size_t file_size,
                                    const PipelineCacheKey& expected_key,
                                    std::vector<uint8_t>& out_payload);

}  // namespace shift::rhi
