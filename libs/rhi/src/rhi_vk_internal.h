// Internal Vulkan 1.3 RHI structures shared across rhi_vk_device.cpp,
// rhi_vk_cmd.cpp, and rhi_vk_rt.cpp. Never exposed outside libs/rhi/src/.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <volk.h>
#include <vk_mem_alloc.h>

#include "rhi/rhi.h"

namespace shift::rhi::vk {

// -----------------------------------------------------------------------------
// Resource pool entries
// -----------------------------------------------------------------------------
struct BufferEntry {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  uint64_t size = 0;
  BufferUsage usage = BufferUsage::None;
  MemoryHint memory = MemoryHint::GpuOnly;
  void* persistent_map = nullptr;
  uint64_t device_address = 0;
};

struct TextureEntry {
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;  // null for swapchain-owned images
  VkFormat vk_format = VK_FORMAT_UNDEFINED;
  Format format = Format::Undefined;
  TextureType type = TextureType::Tex2D;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 1;
  uint32_t mip_levels = 1;
  uint32_t array_layers = 1;
  uint32_t sample_count = 1;
  bool owned = true;
};

struct TextureViewEntry {
  VkImageView view = VK_NULL_HANDLE;
  TextureHandle texture;
  VkFormat vk_format = VK_FORMAT_UNDEFINED;
  bool is_depth = false;
  bool owned = true;
};

struct SamplerEntry {
  VkSampler sampler = VK_NULL_HANDLE;
};

struct BindGroupLayoutEntry {
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  std::vector<VkDescriptorPoolSize> pool_sizes;
  bool update_after_bind = false;
};

struct BindGroupEntry {
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;  // dedicated small pool or shared pool
  bool owns_pool = false;
};

struct PipelineLayoutEntry {
  VkPipelineLayout layout = VK_NULL_HANDLE;
};

struct PipelineEntry {
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

struct QueryPoolEntry {
  VkQueryPool pool = VK_NULL_HANDLE;
  QueryType type = QueryType::Timestamp;
  uint32_t count = 0;
};

struct AccelStructEntry {
  VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
  BufferHandle storage_buffer;
  uint64_t size = 0;
  uint64_t device_address = 0;
  AccelStructKind kind = AccelStructKind::BottomLevel;
};

// -----------------------------------------------------------------------------
// Format & flag translation helpers
// -----------------------------------------------------------------------------
VkFormat to_vk_format(Format fmt) noexcept;
Format from_vk_format(VkFormat fmt) noexcept;
VkImageType to_vk_image_type(TextureType type) noexcept;
VkImageViewType to_vk_view_type(TextureType type) noexcept;
VkComponentSwizzle to_vk_swizzle(ComponentSwizzle s) noexcept;
VkFilter to_vk_filter(Filter f) noexcept;
VkSamplerMipmapMode to_vk_mipmap_mode(Filter f) noexcept;
VkSamplerAddressMode to_vk_address_mode(AddressMode m) noexcept;
VkCompareOp to_vk_compare_op(CompareOp op) noexcept;
VkBorderColor to_vk_border_color(BorderColor c) noexcept;
VkShaderStageFlags to_vk_shader_stages(ShaderStage stages) noexcept;
VkDescriptorType to_vk_descriptor_type(DescriptorType t) noexcept;
VkBlendFactor to_vk_blend_factor(BlendFactor f) noexcept;
VkBlendOp to_vk_blend_op(BlendOp op) noexcept;
VkCullModeFlags to_vk_cull_mode(CullMode c) noexcept;
VkFrontFace to_vk_front_face(FrontFace f) noexcept;
VkPrimitiveTopology to_vk_topology(PrimitiveTopology t) noexcept;
VkAttachmentLoadOp to_vk_load_op(LoadOp op) noexcept;
VkAttachmentStoreOp to_vk_store_op(StoreOp op) noexcept;
VkResolveModeFlagBits to_vk_resolve_mode(ResolveMode m) noexcept;
VkPipelineStageFlags2 to_vk_stage2(PipelineStage stages) noexcept;
VkAccessFlags2 to_vk_access2(AccessFlags access) noexcept;
VkBufferUsageFlags to_vk_buffer_usage(BufferUsage usage, bool bda_enabled, bool rt_enabled) noexcept;
VkImageUsageFlags to_vk_image_usage(TextureUsage usage) noexcept;
VkBuildAccelerationStructureFlagsKHR to_vk_as_build_flags(AccelStructBuildFlags flags) noexcept;
VkGeometryFlagsKHR to_vk_geometry_flags(GeometryFlags flags) noexcept;

struct StateTransitionInfo {
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
  VkAccessFlags2 access = VK_ACCESS_2_NONE;
};
StateTransitionInfo translate_resource_state(ResourceState state, bool is_depth, bool is_dst) noexcept;

// -----------------------------------------------------------------------------
// Forward declarations of Vulkan backend implementations
// -----------------------------------------------------------------------------
class VkDeviceImpl;

class VkBindlessTableImpl final : public BindlessTable {
 public:
  VkBindlessTableImpl(VkDeviceImpl& device, BindGroupLayoutHandle layout_handle,
                      BindGroupHandle group_handle, VkDescriptorSet set,
                      uint32_t max_textures, uint32_t max_storage_buffers);
  ~VkBindlessTableImpl() override;

  BindGroupLayoutHandle layout() const override { return layout_handle_; }
  BindGroupHandle bind_group() const override { return group_handle_; }
  uint32_t max_textures() const override { return max_textures_; }
  uint32_t max_storage_buffers() const override { return max_storage_buffers_; }
  void set_sampler(SamplerHandle sampler) override;
  void set_texture(uint32_t slot, TextureViewHandle view) override;
  void set_storage_buffer(uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t size) override;

 private:
  VkDeviceImpl& device_;
  BindGroupLayoutHandle layout_handle_;
  BindGroupHandle group_handle_;
  VkDescriptorSet set_ = VK_NULL_HANDLE;
  uint32_t max_textures_ = 0;
  uint32_t max_storage_buffers_ = 0;
};

class VkCommandListImpl final : public CommandList {
 public:
  VkCommandListImpl(VkDeviceImpl& device, VkCommandBuffer cmd);
  ~VkCommandListImpl() override = default;

  void reset_handle(VkCommandBuffer cmd) {
    cmd_ = cmd;
    current_layout_ = VK_NULL_HANDLE;
    current_bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
  }
  VkCommandBuffer native_cmd() const noexcept { return cmd_; }

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

 private:
  VkDeviceImpl& device_;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkPipelineLayout current_layout_ = VK_NULL_HANDLE;
  VkPipelineBindPoint current_bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

class VkSwapchainImpl final : public Swapchain {
 public:
  explicit VkSwapchainImpl(VkDeviceImpl& device);
  ~VkSwapchainImpl() override;

  bool init(const SwapchainDesc& desc, std::string& error);

  bool resize(uint32_t width, uint32_t height, PresentMode mode,
              bool want_hdr, std::string& error) override;
  bool acquire_next_frame(SwapchainFrame& out_frame, uint64_t timeout_ns,
                          bool& out_of_date, std::string& error) override;
  CommandList& frame_command_list(uint32_t frame_slot) override;
  bool submit_and_present(const SwapchainFrame& frame, bool& out_of_date, std::string& error) override;

  Format format() const override { return format_; }
  uint32_t width() const override { return extent_.width; }
  uint32_t height() const override { return extent_.height; }
  PresentMode active_present_mode() const override { return active_present_mode_; }
  HdrMode active_hdr_mode() const override { return active_hdr_mode_; }
  uint32_t image_count() const override { return static_cast<uint32_t>(images_.size()); }

 private:
  struct FrameSync {
    VkSemaphore image_ready = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::unique_ptr<VkCommandListImpl> wrapper;
  };

  bool rebuild_swapchain(uint32_t width, uint32_t height, PresentMode mode,
                         bool want_hdr, std::string& error);
  void destroy_images_and_views();

  VkDeviceImpl& device_;
  SDL_Window* sdl_window_ = nullptr;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  bool owns_surface_ = false;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkExtent2D extent_{0, 0};
  VkFormat vk_format_ = VK_FORMAT_UNDEFINED;
  Format format_ = Format::Undefined;
  VkColorSpaceKHR colorspace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  PresentMode active_present_mode_ = PresentMode::Fifo;
  HdrMode active_hdr_mode_ = HdrMode::Off;

  std::vector<VkImage> images_;
  std::vector<TextureHandle> texture_handles_;
  std::vector<TextureViewHandle> view_handles_;
  std::vector<VkSemaphore> render_done_;
  VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
  std::vector<FrameSync> frames_;
  uint32_t current_slot_ = 0;
};

class VkDeviceImpl final : public Device {
 public:
  VkDeviceImpl() = default;
  ~VkDeviceImpl() override;

  bool init(const DeviceDesc& desc, std::string& error);

  BackendKind backend() const override { return BackendKind::Vulkan; }
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

  // Internal accessors for VkCommandListImpl / VkSwapchainImpl / VkBindlessTableImpl / RT
  VkInstance vk_instance() const noexcept { return instance_; }
  VkPhysicalDevice vk_physical() const noexcept { return physical_; }
  VkDevice vk_device() const noexcept { return device_; }
  VmaAllocator vma_allocator() const noexcept { return allocator_; }
  uint32_t queue_family() const noexcept { return queue_family_; }
  VkQueue graphics_queue() const noexcept { return graphics_queue_; }
  VkSurfaceKHR probe_surface() const noexcept { return probe_surface_; }
  bool hdr_colorspace_ext() const noexcept { return hdr_colorspace_ext_; }
  std::mutex& queue_mutex() noexcept { return queue_mutex_; }

  const BufferEntry* lookup_buffer(BufferHandle h) const;
  const TextureEntry* lookup_texture(TextureHandle h) const;
  const TextureViewEntry* lookup_texture_view(TextureViewHandle h) const;
  const SamplerEntry* lookup_sampler(SamplerHandle h) const;
  const BindGroupLayoutEntry* lookup_bind_group_layout(BindGroupLayoutHandle h) const;
  const BindGroupEntry* lookup_bind_group(BindGroupHandle h) const;
  const PipelineLayoutEntry* lookup_pipeline_layout(PipelineLayoutHandle h) const;
  const PipelineEntry* lookup_pipeline(PipelineHandle h) const;
  const QueryPoolEntry* lookup_query_pool(QueryPoolHandle h) const;
  const AccelStructEntry* lookup_accel_struct(AccelStructHandle h) const;

  // Swapchain external-image registration helpers (non-owning handles).
  TextureHandle register_external_texture(VkImage image, VkFormat fmt, uint32_t w, uint32_t h);
  TextureViewHandle register_external_texture_view(TextureHandle tex, VkImageView view, VkFormat fmt);
  void unregister_external_texture_view(TextureViewHandle h);
  void unregister_external_texture(TextureHandle h);

  // RT internal implementation methods (defined in rhi_vk_rt.cpp)
  AccelStructHandle create_accel_struct_impl(const AccelStructDesc& desc, std::string& error);
  void cmd_build_blas_impl(VkCommandBuffer cmd, const BlasBuildDesc& desc);
  void cmd_write_blas_compacted_size_impl(VkCommandBuffer cmd, AccelStructHandle blas,
                                          QueryPoolHandle pool, uint32_t query_index);
  void cmd_copy_compact_blas_impl(VkCommandBuffer cmd, AccelStructHandle src, AccelStructHandle dst);
  void cmd_build_tlas_impl(VkCommandBuffer cmd, const TlasBuildDesc& desc);

  void record_validation_message() noexcept { ++validation_messages_; }

 private:
  bool create_instance(const DeviceDesc& desc, std::string& error);
  bool pick_physical_device(const DeviceDesc& desc, std::string& error);
  bool create_logical_device(const DeviceDesc& desc, std::string& error);
  void load_pipeline_cache();

  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkSurfaceKHR probe_surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VmaAllocator allocator_ = VK_NULL_HANDLE;
  VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;

  uint32_t queue_family_ = 0;
  VkQueue graphics_queue_ = VK_NULL_HANDLE;
  VkQueue transfer_queue_ = VK_NULL_HANDLE;
  mutable std::mutex queue_mutex_;
  std::mutex pool_mutex_;
  std::vector<VkCommandPool> free_cmd_pools_;
  std::vector<VkCommandPool> all_cmd_pools_;

  VkPhysicalDeviceMemoryProperties mem_props_{};
  BackendCaps caps_{};
  PipelineCacheKey cache_key_{};
  std::filesystem::path pipeline_cache_path_;
  bool pipeline_cache_loaded_ = false;
  size_t pipeline_cache_loaded_bytes_ = 0;
  bool hdr_colorspace_ext_ = false;
  bool validation_enabled_ = false;
  std::atomic<uint32_t> validation_messages_{0};

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
};

// Shared Volk loader bootstrap helper (used by both create_device & enumerate_adapters).
bool ensure_volk_initialized(std::string& error);

}  // namespace shift::rhi::vk
