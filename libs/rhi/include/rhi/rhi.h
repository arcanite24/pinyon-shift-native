// Thin Modern RHI (libs/rhi/) public interfaces:
// Device, Swapchain, CommandList, BindlessTable, RenderPassDesc, BarrierDesc,
// Pipeline / Resource / Ray-Tracing descriptors, and create_device() factory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rhi/rhi_types.h"

struct SDL_Window;

namespace shift::rhi {

// -----------------------------------------------------------------------------
// Device & Swapchain descriptors
// -----------------------------------------------------------------------------
struct DeviceDesc {
  BackendKind backend = BackendKind::Auto;
  bool enable_validation = false;
  bool enable_ray_tracing = true;
  bool enable_bindless = true;
  bool enable_transfer_queue = true;
  float max_anisotropy = 16.0f;
  // Optional SDL3 window used when probing surface/present support on the device.
  SDL_Window* sdl_window = nullptr;
  // Optional path for persistent pipeline cache load/save.
  std::filesystem::path pipeline_cache_path;
  const char* app_name = "shift-engine";
};

struct SwapchainDesc {
  SDL_Window* sdl_window = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  PresentMode present_mode = PresentMode::Fifo;
  bool want_hdr = false;
  uint32_t frames_in_flight = 2;
};

struct SwapchainFrame {
  uint32_t image_index = 0;
  uint32_t frame_slot = 0;
  TextureHandle texture;
  TextureViewHandle view;
  Format format = Format::Undefined;
  uint32_t width = 0;
  uint32_t height = 0;
  bool suboptimal = false;
};

// -----------------------------------------------------------------------------
// Resource descriptors
// -----------------------------------------------------------------------------
struct BufferDesc {
  uint64_t size = 0;
  BufferUsage usage = BufferUsage::None;
  MemoryHint memory = MemoryHint::GpuOnly;
  bool persistently_mapped = false;
  const char* debug_name = nullptr;
};

struct TextureDesc {
  TextureType type = TextureType::Tex2D;
  Format format = Format::Rgba8Unorm;
  uint32_t width = 1;
  uint32_t height = 1;
  uint32_t depth = 1;
  uint32_t mip_levels = 1;
  uint32_t array_layers = 1;
  uint32_t sample_count = 1;
  TextureUsage usage = TextureUsage::Sampled;
  MemoryHint memory = MemoryHint::GpuOnly;
  const char* debug_name = nullptr;
};

struct TextureViewDesc {
  TextureHandle texture;
  TextureType type = TextureType::Tex2D;
  Format format = Format::Undefined;  // Undefined inherits texture format
  uint32_t base_mip = 0;
  uint32_t mip_count = 1;
  uint32_t base_layer = 0;
  uint32_t layer_count = 1;
  SwizzleMapping swizzle{};
};

struct SamplerDesc {
  Filter min_filter = Filter::Linear;
  Filter mag_filter = Filter::Linear;
  Filter mip_filter = Filter::Linear;
  AddressMode address_u = AddressMode::Repeat;
  AddressMode address_v = AddressMode::Repeat;
  AddressMode address_w = AddressMode::Repeat;
  float mip_lod_bias = 0.0f;
  float min_lod = 0.0f;
  float max_lod = 1000.0f;
  float max_anisotropy = 1.0f;
  bool compare_enabled = false;
  CompareOp compare_op = CompareOp::LessOrEqual;
  BorderColor border_color = BorderColor::OpaqueBlack;
};

struct QueryPoolDesc {
  QueryType type = QueryType::Timestamp;
  uint32_t count = 64;
};

// -----------------------------------------------------------------------------
// Bind groups, bindless table, shaders, and pipelines
// -----------------------------------------------------------------------------
enum class DescriptorType : uint8_t {
  UniformBuffer = 0,
  StorageBuffer,
  SampledTexture,
  StorageTexture,
  Sampler,
  CombinedTextureSampler,
  AccelStruct,
};

struct BindGroupLayoutBinding {
  uint32_t binding = 0;
  DescriptorType type = DescriptorType::UniformBuffer;
  uint32_t count = 1;
  ShaderStage stages = ShaderStage::All;
  bool partially_bound = false;
  bool update_after_bind = false;
};

struct BindGroupLayoutDesc {
  std::span<const BindGroupLayoutBinding> bindings;
  bool update_after_bind_pool = false;
};

struct BindGroupEntry {
  uint32_t binding = 0;
  uint32_t array_element = 0;
  DescriptorType type = DescriptorType::UniformBuffer;
  BufferHandle buffer;
  uint64_t buffer_offset = 0;
  uint64_t buffer_range = UINT64_MAX;
  TextureViewHandle texture_view;
  SamplerHandle sampler;
  AccelStructHandle accel_struct;
};

struct BindGroupDesc {
  BindGroupLayoutHandle layout;
  std::span<const BindGroupEntry> entries;
};

struct PushConstantRange {
  ShaderStage stages = ShaderStage::All;
  uint32_t offset = 0;
  uint32_t size = 0;
};

struct PipelineLayoutDesc {
  std::span<const BindGroupLayoutHandle> bind_group_layouts;
  std::span<const PushConstantRange> push_constants;
};

struct SpecConstantEntry {
  uint32_t constant_id = 0;
  uint32_t value = 0;  // 32-bit scalar / VkBool32 / float bit-cast
};

struct ShaderModuleDesc {
  ShaderStage stage = ShaderStage::Vertex;
  // SPIR-V words (Vulkan), MSL text/metallib (Metal), or DXIL bytes (D3D12).
  std::span<const uint32_t> spirv;
  const char* entry_point = "main";
  std::span<const SpecConstantEntry> spec_constants;
};

struct VertexBindingDesc {
  uint32_t binding = 0;
  uint32_t stride = 0;
  bool per_instance = false;
};

struct VertexAttributeDesc {
  uint32_t location = 0;
  uint32_t binding = 0;
  Format format = Format::Rgb32Sfloat;
  uint32_t offset = 0;
};

struct GraphicsPipelineDesc {
  PipelineLayoutHandle layout;
  ShaderModuleDesc vertex_shader;
  ShaderModuleDesc fragment_shader;  // empty spirv allowed for depth-only pass
  std::span<const VertexBindingDesc> vertex_bindings;
  std::span<const VertexAttributeDesc> vertex_attributes;
  std::span<const Format> color_formats;
  std::span<const BlendState> color_blends;  // empty -> opaque for every attachment
  Format depth_format = Format::Undefined;
  DepthState depth{};
  RasterState raster{};
  bool dynamic_blend_constants = false;
  bool dynamic_shading_rate = false;
};

struct ComputePipelineDesc {
  PipelineLayoutHandle layout;
  ShaderModuleDesc compute_shader;
};

// -----------------------------------------------------------------------------
// Render pass (Dynamic Rendering / Metal RenderPass / DX12 RenderPass)
// -----------------------------------------------------------------------------
struct ColorAttachmentDesc {
  TextureViewHandle view;
  TextureViewHandle resolve_view;
  LoadOp load_op = LoadOp::Clear;
  StoreOp store_op = StoreOp::Store;
  ResolveMode resolve_mode = ResolveMode::None;
  float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct DepthAttachmentDesc {
  TextureViewHandle view;
  TextureViewHandle resolve_view;
  LoadOp load_op = LoadOp::Clear;
  StoreOp store_op = StoreOp::Store;
  ResolveMode resolve_mode = ResolveMode::None;
  float clear_depth = 0.0f;
};

struct RenderPassDesc {
  uint32_t width = 0;
  uint32_t height = 0;
  std::span<const ColorAttachmentDesc> color_attachments;
  const DepthAttachmentDesc* depth_attachment = nullptr;
  const char* debug_label = nullptr;
};

// -----------------------------------------------------------------------------
// Barriers (synchronization2 / Metal fences / DX12 Enhanced Barriers)
// -----------------------------------------------------------------------------
struct TextureBarrier {
  TextureHandle texture;
  ResourceState before = ResourceState::Undefined;
  ResourceState after = ResourceState::Common;
  // Optional explicit stage/access overrides (None derives from ResourceState).
  PipelineStage src_stage = PipelineStage::None;
  AccessFlags src_access = AccessFlags::None;
  PipelineStage dst_stage = PipelineStage::None;
  AccessFlags dst_access = AccessFlags::None;
  uint32_t base_mip = 0;
  uint32_t mip_count = 1;
  uint32_t base_layer = 0;
  uint32_t layer_count = 1;
  bool is_depth = false;
};

struct BufferBarrier {
  BufferHandle buffer;
  ResourceState before = ResourceState::Common;
  ResourceState after = ResourceState::Common;
  PipelineStage src_stage = PipelineStage::None;
  AccessFlags src_access = AccessFlags::None;
  PipelineStage dst_stage = PipelineStage::None;
  AccessFlags dst_access = AccessFlags::None;
  uint64_t offset = 0;
  uint64_t size = UINT64_MAX;
};

struct BarrierDesc {
  std::span<const TextureBarrier> textures;
  std::span<const BufferBarrier> buffers;
  bool global_compute_uav = false;  // Compute write -> Compute read/write
  bool global_as_build_to_read = false;  // AS build -> Compute/AS read
  bool global_host_read = false;    // Transfer write -> Host read
};

// -----------------------------------------------------------------------------
// Copy / Blit regions
// -----------------------------------------------------------------------------
struct BufferTextureCopy {
  uint64_t buffer_offset = 0;
  uint32_t buffer_row_length = 0;   // 0 == tightly packed
  uint32_t buffer_image_height = 0; // 0 == tightly packed
  uint32_t mip_level = 0;
  uint32_t base_layer = 0;
  uint32_t layer_count = 1;
  int32_t tex_x = 0;
  int32_t tex_y = 0;
  int32_t tex_z = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 1;
  bool is_depth = false;
};

struct TextureCopyRegion {
  uint32_t src_mip = 0;
  uint32_t src_layer = 0;
  int32_t src_x = 0;
  int32_t src_y = 0;
  uint32_t dst_mip = 0;
  uint32_t dst_layer = 0;
  int32_t dst_x = 0;
  int32_t dst_y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t layer_count = 1;
  bool is_depth = false;
};

struct TextureBlitRegion {
  uint32_t src_mip = 0;
  uint32_t src_layer = 0;
  int32_t src_x0 = 0;
  int32_t src_y0 = 0;
  int32_t src_x1 = 0;
  int32_t src_y1 = 0;
  uint32_t dst_mip = 0;
  uint32_t dst_layer = 0;
  int32_t dst_x0 = 0;
  int32_t dst_y0 = 0;
  int32_t dst_x1 = 0;
  int32_t dst_y1 = 0;
  uint32_t layer_count = 1;
  bool is_depth = false;
};

// -----------------------------------------------------------------------------
// Hardware Ray Tracing descriptors (BLAS / TLAS / Compaction)
// -----------------------------------------------------------------------------
enum class AccelStructKind : uint8_t {
  BottomLevel = 0,
  TopLevel = 1,
};

enum class AccelStructBuildFlags : uint32_t {
  None            = 0,
  PreferFastTrace = 1u << 0,
  PreferFastBuild = 1u << 1,
  AllowCompaction = 1u << 2,
  AllowUpdate     = 1u << 3,
};
template <> struct EnableBitmask<AccelStructBuildFlags> : std::true_type {};

enum class GeometryFlags : uint32_t {
  None                        = 0,
  Opaque                      = 1u << 0,
  NoDuplicateAnyHitInvocation = 1u << 1,
};
template <> struct EnableBitmask<GeometryFlags> : std::true_type {};

struct BlasTriangleGeometryDesc {
  BufferHandle vertex_buffer;
  uint64_t vertex_offset = 0;
  uint32_t vertex_stride = 0;
  uint32_t max_vertex = 0;
  Format vertex_format = Format::Rgb32Sfloat;
  BufferHandle index_buffer;
  uint64_t index_offset = 0;
  IndexType index_type = IndexType::Uint32;
  uint32_t primitive_count = 0;
  GeometryFlags flags = GeometryFlags::Opaque;
};

struct AccelStructDesc {
  AccelStructKind kind = AccelStructKind::BottomLevel;
  uint64_t size_bytes = 0;
  const char* debug_name = nullptr;
};

struct AccelStructBuildSizes {
  uint64_t accel_struct_size = 0;
  uint64_t update_scratch_size = 0;
  uint64_t build_scratch_size = 0;
};

struct BlasBuildDesc {
  AccelStructHandle dst;
  BufferHandle scratch_buffer;
  uint64_t scratch_offset = 0;
  std::span<const BlasTriangleGeometryDesc> geometries;
  AccelStructBuildFlags flags = AccelStructBuildFlags::PreferFastTrace;
};

// Binary layout matches VkAccelerationStructureInstanceKHR / D3D12_RAYTRACING_INSTANCE_DESC (64 B).
struct TlasInstanceDesc {
  float transform[3][4] = {
      {1.0f, 0.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 1.0f, 0.0f},
  };
  uint32_t custom_index_24 : 24 = 0;
  uint32_t mask_8 : 8 = 0xFFu;
  uint32_t sbt_record_offset_24 : 24 = 0;
  uint32_t flags_8 : 8 = 0;
  uint64_t blas_device_address = 0;
};
static_assert(sizeof(TlasInstanceDesc) == 64, "TlasInstanceDesc must match 64-byte hardware instance layout");

struct TlasBuildDesc {
  AccelStructHandle dst;
  BufferHandle instance_buffer;
  uint64_t instance_offset = 0;
  uint32_t instance_count = 0;
  BufferHandle scratch_buffer;
  uint64_t scratch_offset = 0;
  AccelStructBuildFlags flags = AccelStructBuildFlags::PreferFastTrace;
};

// -----------------------------------------------------------------------------
// Bindless Table (1x SAMPLER + SAMPLED_IMAGE[4096] + Storage Buffer[256])
// -----------------------------------------------------------------------------
struct BindlessTableDesc {
  SamplerHandle default_sampler;
  TextureViewHandle fallback_texture;
  BufferHandle fallback_buffer;
  uint32_t max_textures = 4096;
  uint32_t max_storage_buffers = 256;
  ShaderStage stages = ShaderStage::Fragment | ShaderStage::Compute;
};

class BindlessTable {
 public:
  virtual ~BindlessTable() = default;
  virtual BindGroupLayoutHandle layout() const = 0;
  virtual BindGroupHandle bind_group() const = 0;
  virtual uint32_t max_textures() const = 0;
  virtual uint32_t max_storage_buffers() const = 0;
  virtual void set_sampler(SamplerHandle sampler) = 0;
  virtual void set_texture(uint32_t slot, TextureViewHandle view) = 0;
  virtual void set_storage_buffer(uint32_t slot, BufferHandle buffer,
                                  uint64_t offset = 0, uint64_t size = UINT64_MAX) = 0;
};

// -----------------------------------------------------------------------------
// Command List
// -----------------------------------------------------------------------------
class CommandList {
 public:
  virtual ~CommandList() = default;

  virtual void begin() = 0;
  virtual void end() = 0;

  // Synchronization & queries
  virtual void barrier(const BarrierDesc& desc) = 0;
  virtual void reset_query_pool(QueryPoolHandle pool, uint32_t first, uint32_t count) = 0;
  virtual void write_timestamp(QueryPoolHandle pool, uint32_t index,
                               PipelineStage stage = PipelineStage::BottomOfPipe) = 0;

  // Dynamic rendering pass
  virtual void begin_render_pass(const RenderPassDesc& desc) = 0;
  virtual void end_render_pass() = 0;

  // Dynamic state
  virtual void set_viewport(float x, float y, float width, float height,
                            float min_depth = 0.0f, float max_depth = 1.0f) = 0;
  virtual void set_scissor(int32_t x, int32_t y, uint32_t width, uint32_t height) = 0;
  virtual void set_depth_bias(float constant_factor, float clamp, float slope_factor) = 0;
  virtual void set_blend_constants(const float rgba[4]) = 0;
  virtual void set_shading_rate(uint32_t width, uint32_t height) = 0;

  // Pipeline, bindings, vertex/index buffers
  virtual void bind_pipeline(PipelineHandle pipeline) = 0;
  virtual void bind_group(uint32_t set_index, BindGroupHandle group) = 0;
  virtual void bind_bindless(uint32_t set_index, const BindlessTable& table) = 0;
  virtual void push_constants(ShaderStage stages, uint32_t offset, uint32_t size, const void* data) = 0;
  virtual void bind_vertex_buffers(uint32_t first_binding, std::span<const BufferHandle> buffers,
                                   std::span<const uint64_t> offsets) = 0;
  virtual void bind_index_buffer(BufferHandle buffer, uint64_t offset = 0,
                                 IndexType type = IndexType::Uint32) = 0;

  // Raster draws & indirect draws
  virtual void draw(uint32_t vertex_count, uint32_t instance_count = 1,
                    uint32_t first_vertex = 0, uint32_t first_instance = 0) = 0;
  virtual void draw_indexed(uint32_t index_count, uint32_t instance_count = 1,
                            uint32_t first_index = 0, int32_t vertex_offset = 0,
                            uint32_t first_instance = 0) = 0;
  virtual void draw_indirect(BufferHandle arg_buffer, uint64_t arg_offset,
                             uint32_t draw_count, uint32_t stride) = 0;
  virtual void draw_indexed_indirect_count(BufferHandle arg_buffer, uint64_t arg_offset,
                                           BufferHandle count_buffer, uint64_t count_offset,
                                           uint32_t max_draw_count, uint32_t stride) = 0;

  // Compute
  virtual void dispatch(uint32_t group_x, uint32_t group_y = 1, uint32_t group_z = 1) = 0;

  // Transfers, clears, blits, and explicit depth resolve
  virtual void update_buffer(BufferHandle dst, uint64_t offset, uint64_t size, const void* data) = 0;
  virtual void fill_buffer(BufferHandle dst, uint64_t offset, uint64_t size, uint32_t value) = 0;
  virtual void copy_buffer(BufferHandle src, uint64_t src_offset,
                           BufferHandle dst, uint64_t dst_offset, uint64_t size) = 0;
  virtual void copy_buffer_to_texture(BufferHandle src, TextureHandle dst,
                                      std::span<const BufferTextureCopy> regions) = 0;
  virtual void copy_texture_to_buffer(TextureHandle src, BufferHandle dst,
                                      std::span<const BufferTextureCopy> regions) = 0;
  virtual void copy_texture(TextureHandle src, TextureHandle dst, const TextureCopyRegion& region) = 0;
  virtual void blit(TextureHandle src, TextureHandle dst,
                    const TextureBlitRegion& region, Filter filter = Filter::Linear) = 0;
  virtual void clear_color_texture(TextureHandle dst, const float rgba[4]) = 0;
  virtual void clear_depth_texture(TextureHandle dst, float depth) = 0;
  virtual void resolve_depth(TextureViewHandle msaa_depth, TextureViewHandle resolved_depth,
                             uint32_t width, uint32_t height,
                             ResolveMode mode = ResolveMode::SampleZero) = 0;

  // Hardware ray tracing commands
  virtual void cmd_build_blas(const BlasBuildDesc& desc) = 0;
  virtual void cmd_write_blas_compacted_size(AccelStructHandle blas,
                                             QueryPoolHandle pool, uint32_t query_index) = 0;
  virtual void cmd_copy_compact_blas(AccelStructHandle src, AccelStructHandle dst) = 0;
  virtual void cmd_build_tlas(const TlasBuildDesc& desc) = 0;
};

// -----------------------------------------------------------------------------
// Swapchain
// -----------------------------------------------------------------------------
class Swapchain {
 public:
  virtual ~Swapchain() = default;

  virtual bool resize(uint32_t width, uint32_t height, PresentMode mode,
                      bool want_hdr, std::string& error) = 0;
  virtual bool acquire_next_frame(SwapchainFrame& out_frame, uint64_t timeout_ns,
                                  bool& out_of_date, std::string& error) = 0;
  virtual CommandList& frame_command_list(uint32_t frame_slot) = 0;
  virtual bool submit_and_present(const SwapchainFrame& frame, bool& out_of_date,
                                  std::string& error) = 0;

  virtual Format format() const = 0;
  virtual uint32_t width() const = 0;
  virtual uint32_t height() const = 0;
  virtual PresentMode active_present_mode() const = 0;
  virtual HdrMode active_hdr_mode() const = 0;
  virtual uint32_t image_count() const = 0;
};

// -----------------------------------------------------------------------------
// Device
// -----------------------------------------------------------------------------
class Device {
 public:
  virtual ~Device() = default;

  virtual BackendKind backend() const = 0;
  virtual const BackendCaps& caps() const = 0;
  virtual VramStats vram_stats() const = 0;
  virtual uint32_t validation_message_count() const = 0;
  virtual bool pipeline_cache_loaded() const = 0;
  virtual bool save_pipeline_cache(std::string& error) = 0;
  virtual void wait_idle() = 0;

  // Buffers
  virtual BufferHandle create_buffer(const BufferDesc& desc, std::string& error) = 0;
  virtual void destroy_buffer(BufferHandle handle) = 0;
  virtual void* map_buffer(BufferHandle handle) = 0;
  virtual void unmap_buffer(BufferHandle handle) = 0;
  virtual uint64_t buffer_device_address(BufferHandle handle) const = 0;

  // Textures & Views
  virtual TextureHandle create_texture(const TextureDesc& desc, std::string& error) = 0;
  virtual void destroy_texture(TextureHandle handle) = 0;
  virtual TextureViewHandle create_texture_view(const TextureViewDesc& desc, std::string& error) = 0;
  virtual void destroy_texture_view(TextureViewHandle handle) = 0;

  // Samplers
  virtual SamplerHandle create_sampler(const SamplerDesc& desc, std::string& error) = 0;
  virtual void destroy_sampler(SamplerHandle handle) = 0;

  // Bind groups & Bindless table
  virtual BindGroupLayoutHandle create_bind_group_layout(const BindGroupLayoutDesc& desc,
                                                         std::string& error) = 0;
  virtual void destroy_bind_group_layout(BindGroupLayoutHandle handle) = 0;
  virtual BindGroupHandle create_bind_group(const BindGroupDesc& desc, std::string& error) = 0;
  virtual void destroy_bind_group(BindGroupHandle handle) = 0;
  virtual std::unique_ptr<BindlessTable> create_bindless_table(const BindlessTableDesc& desc,
                                                               std::string& error) = 0;

  // Pipelines
  virtual PipelineLayoutHandle create_pipeline_layout(const PipelineLayoutDesc& desc,
                                                      std::string& error) = 0;
  virtual void destroy_pipeline_layout(PipelineLayoutHandle handle) = 0;
  virtual PipelineHandle create_graphics_pipeline(const GraphicsPipelineDesc& desc,
                                                  std::string& error) = 0;
  virtual PipelineHandle create_compute_pipeline(const ComputePipelineDesc& desc,
                                                 std::string& error) = 0;
  virtual void destroy_pipeline(PipelineHandle handle) = 0;

  // Queries
  virtual QueryPoolHandle create_query_pool(const QueryPoolDesc& desc, std::string& error) = 0;
  virtual void destroy_query_pool(QueryPoolHandle handle) = 0;
  virtual bool get_query_pool_results(QueryPoolHandle pool, uint32_t first, uint32_t count,
                                      std::span<uint64_t> out_values, bool wait = true) = 0;

  // Hardware Ray Tracing (BLAS / TLAS)
  virtual bool query_blas_sizes(std::span<const BlasTriangleGeometryDesc> geometries,
                                AccelStructBuildFlags flags,
                                AccelStructBuildSizes& out_sizes) const = 0;
  virtual bool query_tlas_sizes(uint32_t max_instances, AccelStructBuildFlags flags,
                                AccelStructBuildSizes& out_sizes) const = 0;
  virtual AccelStructHandle create_blas(const AccelStructDesc& desc, std::string& error) = 0;
  virtual AccelStructHandle create_tlas(const AccelStructDesc& desc, std::string& error) = 0;
  virtual void destroy_accel_struct(AccelStructHandle handle) = 0;
  virtual uint64_t accel_struct_device_address(AccelStructHandle handle) const = 0;

  // Immediate command submission (thread-safe; supports offscreen & staging uploads)
  virtual bool submit_once(const std::function<void(CommandList&)>& record_fn,
                           std::string& error, bool prefer_transfer_queue = false) = 0;

  // Swapchain creation (requires an SDL3 window)
  virtual std::unique_ptr<Swapchain> create_swapchain(const SwapchainDesc& desc,
                                                      std::string& error) = 0;
};

// -----------------------------------------------------------------------------
// Factory & Adapter Enumeration
// -----------------------------------------------------------------------------
std::vector<AdapterInfo> enumerate_adapters(BackendKind filter = BackendKind::Auto);
std::unique_ptr<Device> create_device(const DeviceDesc& desc, std::string& error);

}  // namespace shift::rhi
