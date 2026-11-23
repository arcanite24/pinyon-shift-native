// Internal Native Metal 3 RHI structures shared across rhi_metal_device.mm,
// rhi_metal_cmd.mm, and rhi_metal_rt.mm. Never exposed outside libs/rhi/src/.
#pragma once

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#ifdef SHIFT_RHI_HAS_METALFX
#import <MetalFX/MetalFX.h>
#endif

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rhi/rhi.h"

namespace shift::rhi::mtl {

// Reserved Metal buffer index for <= 128 B push constants (matches SPIRV-Cross).
inline constexpr uint32_t kPushConstantBufferIndex = 30u;
inline constexpr uint32_t kMaxPushConstantBytes = 128u;
// Vertex buffers occupy slots [20..29] so bind groups [0..19] and push constants [30] never collide.
inline constexpr uint32_t kVertexBufferBaseIndex = 20u;

// -----------------------------------------------------------------------------
// Resource pool entries (ARC-managed Objective-C++ handles inside C++ structs)
// -----------------------------------------------------------------------------
struct BufferEntry {
  id<MTLBuffer> buffer = nil;
  uint64_t size = 0;
  BufferUsage usage = BufferUsage::None;
  MemoryHint memory = MemoryHint::GpuOnly;
  uint64_t device_address = 0;
  bool tracked_alloc = false;
};

struct TextureEntry {
  id<MTLTexture> texture = nil;
  MTLPixelFormat mtl_format = MTLPixelFormatInvalid;
  Format format = Format::Undefined;
  TextureType type = TextureType::Tex2D;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 1;
  uint32_t mip_levels = 1;
  uint32_t array_layers = 1;
  uint32_t sample_count = 1;
  bool owned = true;
  bool is_memoryless = false;
  uint64_t approx_bytes = 0;
};

struct TextureViewEntry {
  id<MTLTexture> view = nil;
  TextureHandle texture;
  MTLPixelFormat mtl_format = MTLPixelFormatInvalid;
  Format format = Format::Undefined;
  bool is_depth = false;
  bool owned = true;
};

struct SamplerEntry {
  id<MTLSamplerState> sampler = nil;
};

struct BindGroupLayoutEntry {
  std::vector<BindGroupLayoutBinding> bindings;
  bool update_after_bind = false;
};

struct ResolvedBindEntry {
  uint32_t binding = 0;
  uint32_t array_element = 0;
  DescriptorType type = DescriptorType::UniformBuffer;
  id<MTLBuffer> buffer = nil;
  uint64_t buffer_offset = 0;
  id<MTLTexture> texture = nil;
  id<MTLSamplerState> sampler = nil;
  id<MTLAccelerationStructure> accel_struct = nil;
  std::vector<id<MTLAccelerationStructure>> tlas_blases;
};

class MetalBindlessTableImpl;

struct BindGroupEntry {
  BindGroupLayoutHandle layout;
  std::vector<ResolvedBindEntry> entries;
  // Non-null when this bind group backs a Metal 3 Tier 2 Argument Buffer bindless table.
  MetalBindlessTableImpl* bindless_table = nullptr;
};

struct PipelineLayoutEntry {
  std::vector<BindGroupLayoutHandle> bind_group_layouts;
  std::vector<PushConstantRange> push_constants;
};

struct PipelineEntry {
  bool is_compute = false;
  PipelineLayoutHandle layout;
  id<MTLRenderPipelineState> render_pso = nil;
  id<MTLDepthStencilState> depth_stencil = nil;
  id<MTLComputePipelineState> compute_pso = nil;
  MTLSize compute_threads_per_group = MTLSizeMake(1, 1, 1);
  MTLCullMode cull_mode = MTLCullModeBack;
  MTLWinding front_face = MTLWindingCounterClockwise;
  MTLPrimitiveType primitive_type = MTLPrimitiveTypeTriangle;
  MTLDepthClipMode depth_clip_mode = MTLDepthClipModeClip;
  bool depth_bias_enabled = false;
};

struct QueryPoolEntry {
  QueryType type = QueryType::Timestamp;
  uint32_t count = 0;
  // Used for QueryType::Timestamp
  id<MTLCounterSampleBuffer> counter_buffer = nil;
  // Used for QueryType::AccelStructCompactedSize (and CPU timestamp fallback if needed)
  id<MTLBuffer> result_buffer = nil;
};

struct AccelStructEntry {
  id<MTLAccelerationStructure> handle = nil;
  uint64_t size = 0;
  uint64_t device_address = 0;
  AccelStructKind kind = AccelStructKind::BottomLevel;
  // Referenced BLAS objects for TLAS builds (kept alive + used for residency).
  NSArray<id<MTLAccelerationStructure>>* instanced_blases = nil;
};

// -----------------------------------------------------------------------------
// Format & flag translation helpers
// -----------------------------------------------------------------------------
MTLPixelFormat to_mtl_pixel_format(Format fmt, bool bc_supported) noexcept;
Format from_mtl_pixel_format(MTLPixelFormat fmt) noexcept;
MTLVertexFormat to_mtl_vertex_format(Format fmt) noexcept;
MTLAttributeFormat to_mtl_attribute_format(Format fmt) noexcept;
MTLTextureSwizzle to_mtl_swizzle(ComponentSwizzle s, MTLTextureSwizzle identity_channel) noexcept;
MTLTextureSwizzleChannels to_mtl_swizzle_channels(const SwizzleMapping& m) noexcept;
bool is_identity_swizzle(const SwizzleMapping& m) noexcept;
MTLSamplerMinMagFilter to_mtl_filter(Filter f) noexcept;
MTLSamplerMipFilter to_mtl_mip_filter(Filter f) noexcept;
MTLSamplerAddressMode to_mtl_address_mode(AddressMode m) noexcept;
MTLCompareFunction to_mtl_compare_func(CompareOp op) noexcept;
MTLSamplerBorderColor to_mtl_border_color(BorderColor c) noexcept;
MTLBlendFactor to_mtl_blend_factor(BlendFactor f) noexcept;
MTLBlendOperation to_mtl_blend_op(BlendOp op) noexcept;
MTLColorWriteMask to_mtl_write_mask(ColorWriteMask m) noexcept;
MTLCullMode to_mtl_cull_mode(CullMode c) noexcept;
MTLWinding to_mtl_front_face(FrontFace f) noexcept;
MTLPrimitiveType to_mtl_primitive_type(PrimitiveTopology t) noexcept;
MTLLoadAction to_mtl_load_action(LoadOp op) noexcept;
MTLStoreAction to_mtl_store_action(StoreOp op, bool has_resolve) noexcept;
MTLMultisampleDepthResolveFilter to_mtl_depth_resolve_filter(ResolveMode mode) noexcept;
MTLAccelerationStructureUsage to_mtl_as_usage(AccelStructBuildFlags flags) noexcept;

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
class MetalDeviceImpl;

// -----------------------------------------------------------------------------
// Metal 3 Tier 2 Argument Buffer Bindless Table
// Layout: [MTLResourceID sampler] [MTLResourceID textures[max_tex]] [uint64_t buffers[max_buf]]
// -----------------------------------------------------------------------------
class MetalBindlessTableImpl final : public BindlessTable {
 public:
  MetalBindlessTableImpl(MetalDeviceImpl& device,
                         BindGroupLayoutHandle layout_handle,
                         BindGroupHandle group_handle,
                         id<MTLBuffer> arg_buffer,
                         uint32_t max_textures,
                         uint32_t max_storage_buffers);
  ~MetalBindlessTableImpl() override;

  BindGroupLayoutHandle layout() const override { return layout_handle_; }
  BindGroupHandle bind_group() const override { return group_handle_; }
  uint32_t max_textures() const override { return max_textures_; }
  uint32_t max_storage_buffers() const override { return max_storage_buffers_; }

  void set_sampler(SamplerHandle sampler) override;
  void set_texture(uint32_t slot, TextureViewHandle view) override;
  void set_storage_buffer(uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t size) override;

  id<MTLBuffer> arg_buffer() const noexcept { return arg_buffer_; }
  void commit_residency_if_needed();
  void encode_use_resources_render(id<MTLRenderCommandEncoder> enc) const;
  void encode_use_resources_compute(id<MTLComputeCommandEncoder> enc) const;

 private:
  void mark_residency_dirty_locked();

  MetalDeviceImpl& device_;
  BindGroupLayoutHandle layout_handle_;
  BindGroupHandle group_handle_;
  id<MTLBuffer> arg_buffer_ = nil;
  uint32_t max_textures_ = 0;
  uint32_t max_storage_buffers_ = 0;

  mutable std::mutex table_mutex_;
  std::vector<id<MTLTexture>> resident_textures_;
  std::vector<id<MTLBuffer>> resident_buffers_;
  id<MTLResidencySet> residency_set_ API_AVAILABLE(macos(15.0), ios(18.0)) = nil;
  bool residency_dirty_ = false;
  bool has_residency_set_ = false;
};

// -----------------------------------------------------------------------------
// Metal 3 CommandList
// -----------------------------------------------------------------------------
class MetalCommandListImpl final : public CommandList {
 public:
  MetalCommandListImpl(MetalDeviceImpl& device, id<MTLCommandBuffer> cmd_buf);
  ~MetalCommandListImpl() override;

  void reset_command_buffer(id<MTLCommandBuffer> cmd_buf);
  id<MTLCommandBuffer> native_cmd_buf() const noexcept { return cmd_buf_; }
  void release_transients() { transient_buffers_.clear(); transient_objects_.clear(); }
  void keep_transient_buffer(id<MTLBuffer> buf) {
    if (buf) transient_buffers_.push_back(buf);
  }
  void keep_transient_object(id obj) {
    if (obj) transient_objects_.push_back(obj);
  }

  void begin() override;
  void end() override;
  void barrier(const BarrierDesc& desc) override;
  void reset_query_pool(QueryPoolHandle pool, uint32_t first, uint32_t count) override;
  void write_timestamp(QueryPoolHandle pool, uint32_t index, PipelineStage stage) override;

  void begin_render_pass(const RenderPassDesc& desc) override;
  void end_render_pass() override;

  void set_viewport(float x, float y, float width, float height, float min_depth, float max_depth) override;
  void set_scissor(int32_t x, int32_t y, uint32_t width, uint32_t height) override;
  void set_depth_bias(float constant_factor, float clamp, float slope_factor) override;
  void set_blend_constants(const float rgba[4]) override;
  void set_shading_rate(uint32_t width, uint32_t height) override;

  void bind_pipeline(PipelineHandle pipeline) override;
  void bind_group(uint32_t set_index, BindGroupHandle group) override;
  void bind_bindless(uint32_t set_index, const BindlessTable& table) override;
  void push_constants(ShaderStage stages, uint32_t offset, uint32_t size, const void* data) override;
  void bind_vertex_buffers(uint32_t first_binding, std::span<const BufferHandle> buffers,
                           std::span<const uint64_t> offsets) override;
  void bind_index_buffer(BufferHandle buffer, uint64_t offset, IndexType type) override;

  void draw(uint32_t vertex_count, uint32_t instance_count,
            uint32_t first_vertex, uint32_t first_instance) override;
  void draw_indexed(uint32_t index_count, uint32_t instance_count,
                    uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) override;
  void draw_indirect(BufferHandle arg_buffer, uint64_t arg_offset,
                     uint32_t draw_count, uint32_t stride) override;
  void draw_indexed_indirect_count(BufferHandle arg_buffer, uint64_t arg_offset,
                                   BufferHandle count_buffer, uint64_t count_offset,
                                   uint32_t max_draw_count, uint32_t stride) override;

  void dispatch(uint32_t group_x, uint32_t group_y, uint32_t group_z) override;

  void update_buffer(BufferHandle dst, uint64_t offset, uint64_t size, const void* data) override;
  void fill_buffer(BufferHandle dst, uint64_t offset, uint64_t size, uint32_t value) override;
  void copy_buffer(BufferHandle src, uint64_t src_offset,
                   BufferHandle dst, uint64_t dst_offset, uint64_t size) override;
  void copy_buffer_to_texture(BufferHandle src, TextureHandle dst,
                              std::span<const BufferTextureCopy> regions) override;
  void copy_texture_to_buffer(TextureHandle src, BufferHandle dst,
                              std::span<const BufferTextureCopy> regions) override;
  void copy_texture(TextureHandle src, TextureHandle dst, const TextureCopyRegion& region) override;
  void blit(TextureHandle src, TextureHandle dst, const TextureBlitRegion& region, Filter filter) override;
  void clear_color_texture(TextureHandle dst, const float rgba[4]) override;
  void clear_depth_texture(TextureHandle dst, float depth) override;
  void resolve_depth(TextureViewHandle msaa_depth, TextureViewHandle resolved_depth,
                     uint32_t width, uint32_t height, ResolveMode mode) override;

  void cmd_build_blas(const BlasBuildDesc& desc) override;
  void cmd_write_blas_compacted_size(AccelStructHandle blas, QueryPoolHandle pool, uint32_t query_index) override;
  void cmd_copy_compact_blas(AccelStructHandle src, AccelStructHandle dst) override;
  void cmd_build_tlas(const TlasBuildDesc& desc) override;

  id<MTLAccelerationStructureCommandEncoder> ensure_accel_encoder();
  void end_active_encoder();

 private:
  enum class EncoderKind : uint8_t { None = 0, Render, Compute, Blit, AccelStruct };

  id<MTLComputeCommandEncoder> ensure_compute_encoder();
  id<MTLBlitCommandEncoder> ensure_blit_encoder();
  void apply_render_pipeline_state(const PipelineEntry& pe);
  void apply_bind_group_to_render(uint32_t set_index, const BindGroupEntry& ge);
  void apply_bind_group_to_compute(uint32_t set_index, const BindGroupEntry& ge);
  void flush_pending_compute_state();

  MetalDeviceImpl& device_;
  id<MTLCommandBuffer> cmd_buf_ = nil;

  EncoderKind active_encoder_ = EncoderKind::None;
  id<MTLRenderCommandEncoder> render_enc_ = nil;
  id<MTLComputeCommandEncoder> compute_enc_ = nil;
  id<MTLBlitCommandEncoder> blit_enc_ = nil;
  id<MTLAccelerationStructureCommandEncoder> accel_enc_ = nil;

  // Bound pipeline & state tracking
  PipelineHandle current_pipeline_{};
  bool current_pipeline_is_compute_ = false;
  MTLPrimitiveType current_primitive_type_ = MTLPrimitiveTypeTriangle;
  MTLSize current_compute_tg_ = MTLSizeMake(1, 1, 1);

  id<MTLBuffer> bound_index_buffer_ = nil;
  uint64_t bound_index_offset_ = 0;
  MTLIndexType bound_index_type_ = MTLIndexTypeUInt32;

  // Cached push constants (<= 128 B)
  uint8_t push_const_data_[kMaxPushConstantBytes]{};
  uint32_t push_const_size_ = 0;
  bool push_const_compute_dirty_ = false;

  // Deferred compute bindings (applied when compute encoder is opened on dispatch)
  static constexpr uint32_t kMaxBindSets = 4u;
  BindGroupHandle pending_compute_groups_[kMaxBindSets]{};
  bool compute_pipeline_dirty_ = false;

  // Render pass dimensions for clamping scissor rects
  uint32_t rp_width_ = 0;
  uint32_t rp_height_ = 0;

  // Cross-encoder barrier synchronization (required for bindless/MTLResidencySet
  // resources that bypass Metal's automatic hazard tracking across encoders).
  id<MTLFence> barrier_fence_ = nil;
  bool fence_updated_ = false;
  bool fence_wait_pending_ = false;

  // Staging buffers & arrays kept alive for the lifetime of this command buffer
  std::vector<id<MTLBuffer>> transient_buffers_;
  std::vector<id> transient_objects_;
};

// -----------------------------------------------------------------------------
// Metal 3 Swapchain (CAMetalLayer via SDL3 + Apple XDR EDR HDR)
// -----------------------------------------------------------------------------
class MetalSwapchainImpl final : public Swapchain {
 public:
  explicit MetalSwapchainImpl(MetalDeviceImpl& device);
  ~MetalSwapchainImpl() override;

  bool init(const SwapchainDesc& desc, std::string& error);

  bool resize(uint32_t width, uint32_t height, PresentMode mode,
              bool want_hdr, std::string& error) override;
  bool acquire_next_frame(SwapchainFrame& out_frame, uint64_t timeout_ns,
                          bool& out_of_date, std::string& error) override;
  CommandList& frame_command_list(uint32_t frame_slot) override;
  bool submit_and_present(const SwapchainFrame& frame, bool& out_of_date, std::string& error) override;

  Format format() const override { return format_; }
  uint32_t width() const override { return width_; }
  uint32_t height() const override { return height_; }
  PresentMode active_present_mode() const override { return active_present_mode_; }
  HdrMode active_hdr_mode() const override { return active_hdr_mode_; }
  uint32_t image_count() const override { return drawable_count_; }

 private:
  struct FrameSlot {
    id<CAMetalDrawable> drawable = nil;
    TextureHandle tex_handle{};
    TextureViewHandle view_handle{};
    std::unique_ptr<MetalCommandListImpl> cmd_list;
  };

  bool configure_layer(uint32_t width, uint32_t height, PresentMode mode,
                       bool want_hdr, std::string& error);
  void release_slot_handles(FrameSlot& slot);

  MetalDeviceImpl& device_;
  SDL_Window* sdl_window_ = nullptr;
  void* metal_view_ = nullptr;  // SDL_MetalView
  CAMetalLayer* layer_ = nil;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  Format format_ = Format::Bgra8Unorm;
  MTLPixelFormat mtl_format_ = MTLPixelFormatBGRA8Unorm;
  PresentMode active_present_mode_ = PresentMode::Fifo;
  HdrMode active_hdr_mode_ = HdrMode::Off;
  uint32_t drawable_count_ = 3;

  dispatch_semaphore_t in_flight_sem_ = nil;
  std::vector<FrameSlot> slots_;
  uint32_t current_slot_ = 0;
};

// -----------------------------------------------------------------------------
// Metal 3 Device
// -----------------------------------------------------------------------------
class MetalDeviceImpl final : public Device {
 public:
  MetalDeviceImpl() = default;
  ~MetalDeviceImpl() override;

  bool init(const DeviceDesc& desc, std::string& error);

  BackendKind backend() const override { return BackendKind::Metal; }
  const BackendCaps& caps() const override { return caps_; }
  VramStats vram_stats() const override;
  uint32_t validation_message_count() const override { return validation_messages_.load(); }
  bool pipeline_cache_loaded() const override { return pipeline_cache_loaded_; }
  bool save_pipeline_cache(std::string& error) override;
  void wait_idle() override;

  BufferHandle create_buffer(const BufferDesc& desc, std::string& error) override;
  void destroy_buffer(BufferHandle handle) override;
  void* map_buffer(BufferHandle handle) override;
  void unmap_buffer(BufferHandle handle) override;
  uint64_t buffer_device_address(BufferHandle handle) const override;

  TextureHandle create_texture(const TextureDesc& desc, std::string& error) override;
  void destroy_texture(TextureHandle handle) override;
  TextureViewHandle create_texture_view(const TextureViewDesc& desc, std::string& error) override;
  void destroy_texture_view(TextureViewHandle handle) override;

  SamplerHandle create_sampler(const SamplerDesc& desc, std::string& error) override;
  void destroy_sampler(SamplerHandle handle) override;

  BindGroupLayoutHandle create_bind_group_layout(const BindGroupLayoutDesc& desc, std::string& error) override;
  void destroy_bind_group_layout(BindGroupLayoutHandle handle) override;
  BindGroupHandle create_bind_group(const BindGroupDesc& desc, std::string& error) override;
  void destroy_bind_group(BindGroupHandle handle) override;
  std::unique_ptr<BindlessTable> create_bindless_table(const BindlessTableDesc& desc, std::string& error) override;

  PipelineLayoutHandle create_pipeline_layout(const PipelineLayoutDesc& desc, std::string& error) override;
  void destroy_pipeline_layout(PipelineLayoutHandle handle) override;
  PipelineHandle create_graphics_pipeline(const GraphicsPipelineDesc& desc, std::string& error) override;
  PipelineHandle create_compute_pipeline(const ComputePipelineDesc& desc, std::string& error) override;
  void destroy_pipeline(PipelineHandle handle) override;

  QueryPoolHandle create_query_pool(const QueryPoolDesc& desc, std::string& error) override;
  void destroy_query_pool(QueryPoolHandle handle) override;
  bool get_query_pool_results(QueryPoolHandle pool, uint32_t first, uint32_t count,
                              std::span<uint64_t> out_values, bool wait) override;

  bool query_blas_sizes(std::span<const BlasTriangleGeometryDesc> geometries,
                        AccelStructBuildFlags flags,
                        AccelStructBuildSizes& out_sizes) const override;
  bool query_tlas_sizes(uint32_t max_instances, AccelStructBuildFlags flags,
                        AccelStructBuildSizes& out_sizes) const override;
  AccelStructHandle create_blas(const AccelStructDesc& desc, std::string& error) override;
  AccelStructHandle create_tlas(const AccelStructDesc& desc, std::string& error) override;
  void destroy_accel_struct(AccelStructHandle handle) override;
  uint64_t accel_struct_device_address(AccelStructHandle handle) const override;

  bool submit_once(const std::function<void(CommandList&)>& record_fn,
                   std::string& error, bool prefer_transfer_queue) override;
  std::unique_ptr<Swapchain> create_swapchain(const SwapchainDesc& desc, std::string& error) override;

  // Internal accessors
  id<MTLDevice> mtl_device() const noexcept { return device_; }
  id<MTLCommandQueue> mtl_queue() const noexcept { return queue_; }
  id<MTLCounterSet> timestamp_counter_set() const noexcept { return timestamp_counter_set_; }
  bool supports_stage_boundary_counters() const noexcept { return supports_stage_boundary_counters_; }
  bool supports_blit_boundary_counters() const noexcept { return supports_blit_boundary_counters_; }
  bool supports_bc_textures() const noexcept { return supports_bc_textures_; }
  bool supports_metalfx_spatial() const noexcept { return supports_metalfx_spatial_; }
  bool supports_metalfx_temporal() const noexcept { return supports_metalfx_temporal_; }

  bool lookup_buffer_copy(BufferHandle h, BufferEntry& out) const;
  bool lookup_texture_copy(TextureHandle h, TextureEntry& out) const;
  bool lookup_texture_view_copy(TextureViewHandle h, TextureViewEntry& out) const;
  bool lookup_sampler_copy(SamplerHandle h, SamplerEntry& out) const;
  bool lookup_bind_group_copy(BindGroupHandle h, BindGroupEntry& out) const;
  bool lookup_pipeline_copy(PipelineHandle h, PipelineEntry& out) const;
  bool lookup_query_pool_copy(QueryPoolHandle h, QueryPoolEntry& out) const;
  bool lookup_accel_struct_copy(AccelStructHandle h, AccelStructEntry& out) const;
  id<MTLAccelerationStructure> lookup_as_by_address(uint64_t gpu_addr) const;
  void set_tlas_instanced_blases(AccelStructHandle h, NSArray<id<MTLAccelerationStructure>>* blases);

  // External swapchain drawable registration (non-owning).
  TextureHandle register_external_texture(id<MTLTexture> tex, Format fmt, uint32_t w, uint32_t h);
  TextureViewHandle register_external_texture_view(TextureHandle tex_h, id<MTLTexture> tex, Format fmt);
  void unregister_external_texture_view(TextureViewHandle h);
  void unregister_external_texture(TextureHandle h);

  // Internal blit pipeline cache (used by cmd.blit() for scaled/filtered texture blits).
  id<MTLRenderPipelineState> get_or_create_blit_pipeline(MTLPixelFormat dst_fmt);
  id<MTLSamplerState> get_or_create_blit_sampler(Filter filter);

  // RT command helpers (defined in rhi_metal_rt.mm).
  AccelStructHandle create_accel_struct_impl(const AccelStructDesc& desc, std::string& error);
  void cmd_build_blas_impl(MetalCommandListImpl& cmd, const BlasBuildDesc& desc);
  void cmd_write_blas_compacted_size_impl(MetalCommandListImpl& cmd, AccelStructHandle blas,
                                          QueryPoolHandle pool, uint32_t query_index);
  void cmd_copy_compact_blas_impl(MetalCommandListImpl& cmd, AccelStructHandle src, AccelStructHandle dst);
  void cmd_build_tlas_impl(MetalCommandListImpl& cmd, const TlasBuildDesc& desc);

  void record_validation_message() noexcept { ++validation_messages_; }

 private:
  void probe_capabilities();
  void load_pipeline_cache();
  id<MTLFunction> compile_shader_function(const ShaderModuleDesc& sm,
                                          MTLSize* out_compute_tg,
                                          std::string& error);

  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLBinaryArchive> binary_archive_ = nil;
  id<MTLCounterSet> timestamp_counter_set_ = nil;
  bool supports_stage_boundary_counters_ = false;
  bool supports_blit_boundary_counters_ = false;
  bool supports_bc_textures_ = false;
  bool supports_metalfx_spatial_ = false;
  bool supports_metalfx_temporal_ = false;
  bool supports_dynamic_libraries_ = false;

  BackendCaps caps_{};
  PipelineCacheKey cache_key_{};
  std::filesystem::path pipeline_cache_path_;
  bool pipeline_cache_loaded_ = false;
  bool pipeline_cache_dirty_ = false;
  std::atomic<uint32_t> validation_messages_{0};

  // Internal blit helpers
  std::mutex blit_mutex_;
  id<MTLLibrary> blit_lib_ = nil;
  std::unordered_map<uint32_t, id<MTLRenderPipelineState>> blit_pipelines_;
  id<MTLSamplerState> blit_sampler_nearest_ = nil;
  id<MTLSamplerState> blit_sampler_linear_ = nil;

  // Resource pools & allocation accounting
  mutable std::mutex resource_mutex_;
  HandlePool<BufferHandle, BufferEntry> buffers_;
  HandlePool<TextureHandle, TextureEntry> textures_;
  HandlePool<TextureViewHandle, TextureViewEntry> texture_views_;
  HandlePool<SamplerHandle, SamplerEntry> samplers_;
  HandlePool<BindGroupLayoutHandle, BindGroupLayoutEntry> bind_group_layouts_;
  HandlePool<BindGroupHandle, BindGroupEntry> bind_groups_;
  HandlePool<PipelineLayoutHandle, PipelineLayoutEntry> pipeline_layouts_;
  HandlePool<PipelineHandle, PipelineEntry> pipelines_;
  HandlePool<QueryPoolHandle, QueryPoolEntry> query_pools_;
  HandlePool<AccelStructHandle, AccelStructEntry> accel_structs_;
  std::unordered_map<uint64_t, AccelStructHandle> as_address_map_;

  uint32_t live_alloc_count_ = 0;
  uint64_t live_alloc_bytes_ = 0;
};

}  // namespace shift::rhi::mtl
