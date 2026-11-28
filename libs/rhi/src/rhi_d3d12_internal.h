// Internal DirectX 12 RHI structures shared across rhi_d3d12_device.cpp,
// rhi_d3d12_cmd.cpp, and rhi_d3d12_rt.cpp. Never exposed outside libs/rhi/src/.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rhi/rhi.h"

namespace shift::rhi::d3d12 {

using Microsoft::WRL::ComPtr;

// -----------------------------------------------------------------------------
// Descriptor Heap Allocator (contiguous range allocator with simple free list)
// -----------------------------------------------------------------------------
class DescriptorAllocator {
 public:
  bool init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
            uint32_t capacity, bool shader_visible, std::string& error);
  void shutdown();

  // Returns base index, or UINT32_MAX on failure.
  uint32_t allocate(uint32_t count);
  void free(uint32_t base, uint32_t count);

  D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(uint32_t index) const noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu_start_;
    h.ptr += static_cast<SIZE_T>(index) * static_cast<SIZE_T>(stride_);
    return h;
  }

  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(uint32_t index) const noexcept {
    D3D12_GPU_DESCRIPTOR_HANDLE h = gpu_start_;
    h.ptr += static_cast<UINT64>(index) * static_cast<UINT64>(stride_);
    return h;
  }

  ID3D12DescriptorHeap* heap() const noexcept { return heap_.Get(); }
  uint32_t stride() const noexcept { return stride_; }
  uint32_t capacity() const noexcept { return capacity_; }

 private:
  struct Range {
    uint32_t start = 0;
    uint32_t count = 0;
  };
  mutable std::mutex mutex_;
  ComPtr<ID3D12DescriptorHeap> heap_;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu_start_{};
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_start_{};
  uint32_t stride_ = 0;
  uint32_t capacity_ = 0;
  uint32_t bump_ = 0;
  std::vector<Range> free_ranges_;
};

// -----------------------------------------------------------------------------
// Placed-Heap Transient Sub-Allocator (ID3D12Heap + CreatePlacedResource)
// Avoids per-resource CreateCommittedResource kernel calls for transient buffers
// and render targets while respecting D3D12 Heap Tier 1/2 alignment rules.
// -----------------------------------------------------------------------------
class PlacedHeapPool {
 public:
  struct Allocation {
    uint32_t heap_index = UINT32_MAX;
    uint64_t offset = 0;
    uint64_t size = 0;
    ID3D12Heap* heap = nullptr;
    bool valid() const noexcept { return heap != nullptr; }
  };

  void shutdown();
  Allocation allocate(ID3D12Device* device, D3D12_HEAP_FLAGS flags,
                      uint64_t size, uint64_t alignment);
  void free(const Allocation& alloc);

  uint32_t heap_count() const noexcept;

 private:
  struct FreeBlock {
    uint64_t offset = 0;
    uint64_t size = 0;
  };
  struct HeapBlock {
    ComPtr<ID3D12Heap> heap;
    D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE;
    uint64_t capacity = 0;
    std::vector<FreeBlock> free_list;
  };
  mutable std::mutex mutex_;
  std::vector<HeapBlock> heaps_;
};

// -----------------------------------------------------------------------------
// Resource pool entries
// -----------------------------------------------------------------------------
struct BufferEntry {
  ComPtr<ID3D12Resource> resource;
  PlacedHeapPool::Allocation placed_alloc{};
  uint64_t size = 0;            // logical size requested by caller
  uint64_t allocated_size = 0;  // aligned size allocated in D3D12
  BufferUsage usage = BufferUsage::None;
  MemoryHint memory = MemoryHint::GpuOnly;
  D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_STATES current_state = D3D12_RESOURCE_STATE_COMMON;
  bool allow_uav = false;
  bool fixed_state = false;  // UPLOAD (GENERIC_READ), READBACK (COPY_DEST), or AS storage
  void* persistent_map = nullptr;
  uint64_t gpu_va = 0;
};

struct TextureEntry {
  ComPtr<ID3D12Resource> resource;
  PlacedHeapPool::Allocation placed_alloc{};
  DXGI_FORMAT resource_format = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT copy_format = DXGI_FORMAT_UNKNOWN;
  Format format = Format::Undefined;
  TextureType type = TextureType::Tex2D;
  TextureUsage usage = TextureUsage::None;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 1;
  uint32_t mip_levels = 1;
  uint32_t array_layers = 1;
  uint32_t sample_count = 1;
  uint64_t allocated_size = 0;
  D3D12_RESOURCE_STATES current_state = D3D12_RESOURCE_STATE_COMMON;
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
  D3D12_BARRIER_LAYOUT current_layout = D3D12_BARRIER_LAYOUT_COMMON;
#endif
  bool enhanced_barrier_compatible = false;
  bool owned = true;
};

struct TextureViewEntry {
  TextureHandle texture;
  Format format = Format::Undefined;
  TextureType type = TextureType::Tex2D;
  uint32_t base_mip = 0;
  uint32_t mip_count = 1;
  uint32_t base_layer = 0;
  uint32_t layer_count = 1;
  SwizzleMapping swizzle{};
  bool is_depth = false;
  bool has_rtv = false;
  bool has_dsv = false;
  uint32_t rtv_slot = UINT32_MAX;
  uint32_t dsv_slot = UINT32_MAX;
};

struct SamplerEntry {
  D3D12_SAMPLER_DESC desc{};
};

struct BindGroupLayoutSlotInfo {
  uint32_t binding = 0;
  DescriptorType type = DescriptorType::UniformBuffer;
  uint32_t count = 1;
  ShaderStage stages = ShaderStage::All;
  bool partially_bound = false;
  bool update_after_bind = false;
  // Offsets within the BindGroup's contiguous CBV/SRV/UAV or Sampler heap blocks.
  uint32_t srv_cbv_offset = UINT32_MAX;
  uint32_t uav_offset = UINT32_MAX;      // StorageBuffer (count==1) also exposes a UAV slot
  uint32_t sampler_offset = UINT32_MAX;
  uint32_t srv_cbv_shader_reg = 0;
  uint32_t srv_cbv_space_offset = 0;     // 0 normally; >0 if an array collided in spaceS
  uint32_t uav_shader_reg = 0;
  uint32_t sampler_shader_reg = 0;
};

struct BindGroupLayoutEntry {
  std::vector<BindGroupLayoutSlotInfo> slots;
  uint32_t cbv_srv_uav_count = 0;
  uint32_t sampler_count = 0;
  bool update_after_bind = false;
};

struct BindGroupEntry {
  BindGroupLayoutHandle layout;
  uint32_t cbv_srv_uav_base = UINT32_MAX;
  uint32_t cbv_srv_uav_count = 0;
  uint32_t sampler_base = UINT32_MAX;
  uint32_t sampler_count = 0;
};

struct PipelineLayoutSetMapping {
  uint32_t cbv_srv_uav_root_param = UINT32_MAX;
  uint32_t sampler_root_param = UINT32_MAX;
};

struct PipelineLayoutEntry {
  ComPtr<ID3D12RootSignature> root_signature;
  uint32_t push_const_root_param = UINT32_MAX;
  uint32_t push_const_dwords = 0;
  std::vector<PipelineLayoutSetMapping> set_mappings;
};

struct PipelineEntry {
  ComPtr<ID3D12PipelineState> pso;
  ID3D12RootSignature* root_signature = nullptr;
  PipelineLayoutHandle layout_handle;
  bool is_compute = false;
  D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  std::array<uint32_t, 16> vertex_strides{};
  bool dynamic_depth_bias = false;
  // Cached parameters for creating dynamic depth-bias PSO variants on demand.
  std::vector<uint8_t> vs_bytecode;
  std::vector<uint8_t> ps_bytecode;
  std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
  std::vector<std::string> semantic_names;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC base_graphics_desc{};
  std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> bias_variants;
};

struct QueryPoolEntry {
  ComPtr<ID3D12QueryHeap> heap;         // used for Timestamp
  ComPtr<ID3D12Resource> gpu_buffer;    // used for AccelStructCompactedSize (DEFAULT+UAV)
  ComPtr<ID3D12Resource> readback;      // READBACK buffer of count * sizeof(uint64_t)
  QueryType type = QueryType::Timestamp;
  uint32_t count = 0;
};

struct AccelStructEntry {
  ComPtr<ID3D12Resource> resource;
  uint64_t size = 0;
  uint64_t gpu_va = 0;
  AccelStructKind kind = AccelStructKind::BottomLevel;
};

struct TimelineSemaphoreEntry {
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
};

// -----------------------------------------------------------------------------
// Format & flag translation helpers
// -----------------------------------------------------------------------------
DXGI_FORMAT to_dxgi_format(Format fmt) noexcept;
DXGI_FORMAT to_dxgi_resource_format(Format fmt, TextureUsage usage) noexcept;
DXGI_FORMAT to_dxgi_srv_format(Format fmt) noexcept;
DXGI_FORMAT to_dxgi_dsv_format(Format fmt) noexcept;
Format from_dxgi_format(DXGI_FORMAT fmt) noexcept;
UINT to_dxgi_swizzle_mapping(const SwizzleMapping& s) noexcept;
D3D12_FILTER to_d3d12_filter(Filter min_f, Filter mag_f, Filter mip_f,
                             bool compare, bool aniso) noexcept;
D3D12_TEXTURE_ADDRESS_MODE to_d3d12_address_mode(AddressMode m) noexcept;
D3D12_COMPARISON_FUNC to_d3d12_compare_op(CompareOp op) noexcept;
D3D12_BLEND to_d3d12_blend_factor(BlendFactor f) noexcept;
D3D12_BLEND_OP to_d3d12_blend_op(BlendOp op) noexcept;
D3D12_CULL_MODE to_d3d12_cull_mode(CullMode c) noexcept;
D3D12_PRIMITIVE_TOPOLOGY_TYPE to_d3d12_topology_type(PrimitiveTopology t) noexcept;
D3D12_PRIMITIVE_TOPOLOGY to_d3d12_topology(PrimitiveTopology t) noexcept;
D3D12_RESOURCE_STATES to_d3d12_resource_state(ResourceState state, bool is_depth) noexcept;
D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS to_d3d12_as_build_flags(
    AccelStructBuildFlags flags) noexcept;
D3D12_RAYTRACING_GEOMETRY_FLAGS to_d3d12_geometry_flags(GeometryFlags flags) noexcept;

#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
D3D12_BARRIER_LAYOUT to_d3d12_barrier_layout(ResourceState state, bool is_depth) noexcept;
D3D12_BARRIER_SYNC to_d3d12_barrier_sync(ResourceState state, PipelineStage override_stage) noexcept;
D3D12_BARRIER_ACCESS to_d3d12_barrier_access(ResourceState state, AccessFlags override_access,
                                             bool is_depth) noexcept;
#endif

// Utility: align up to power of two.
inline uint64_t align_up(uint64_t v, uint64_t a) noexcept {
  return (v + a - 1ull) & ~(a - 1ull);
}

// -----------------------------------------------------------------------------
// Forward declarations of DirectX 12 backend classes
// -----------------------------------------------------------------------------
class D3D12DeviceImpl;

class D3D12BindlessTableImpl final : public BindlessTable {
 public:
  D3D12BindlessTableImpl(D3D12DeviceImpl& device, BindGroupLayoutHandle layout_handle,
                         BindGroupHandle group_handle, uint32_t max_textures,
                         uint32_t max_storage_buffers);
  ~D3D12BindlessTableImpl() override;

  BindGroupLayoutHandle layout() const override { return layout_handle_; }
  BindGroupHandle bind_group() const override { return group_handle_; }
  uint32_t max_textures() const override { return max_textures_; }
  uint32_t max_storage_buffers() const override { return max_storage_buffers_; }
  void set_sampler(SamplerHandle sampler) override;
  void set_texture(uint32_t slot, TextureViewHandle view) override;
  void set_storage_buffer(uint32_t slot, BufferHandle buffer,
                          uint64_t offset, uint64_t size) override;

 private:
  D3D12DeviceImpl& device_;
  BindGroupLayoutHandle layout_handle_;
  BindGroupHandle group_handle_;
  uint32_t max_textures_ = 0;
  uint32_t max_storage_buffers_ = 0;
};

class D3D12CommandListImpl final : public CommandList {
 public:
  D3D12CommandListImpl(D3D12DeviceImpl& device, ComPtr<ID3D12CommandAllocator> allocator,
                       ComPtr<ID3D12GraphicsCommandList4> cmd,
                       QueueKind queue_kind = QueueKind::Graphics);
  ~D3D12CommandListImpl() override = default;

  QueueKind queue_kind() const override { return queue_kind_; }

  void reset_for_recording();
  void release_transients() { transient_resources_.clear(); }
  ID3D12GraphicsCommandList4* native_cmd() const noexcept { return cmd_.Get(); }
  ID3D12CommandAllocator* native_allocator() const noexcept { return allocator_.Get(); }
  HRESULT last_close_hr() const noexcept { return close_hr_; }

  void begin() override;
  void end() override;
  void barrier(const BarrierDesc& desc) override;
  void reset_query_pool(QueryPoolHandle pool, uint32_t first, uint32_t count) override;
  void write_timestamp(QueryPoolHandle pool, uint32_t index, PipelineStage stage) override;

  void begin_render_pass(const RenderPassDesc& desc) override;
  void end_render_pass() override;

  void set_viewport(float x, float y, float width, float height,
                    float min_depth, float max_depth) override;
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
  void dispatch_indirect(BufferHandle arg_buffer, uint64_t arg_offset) override;

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
  void cmd_write_blas_compacted_size(AccelStructHandle blas, QueryPoolHandle pool,
                                     uint32_t query_index) override;
  void cmd_copy_compact_blas(AccelStructHandle src, AccelStructHandle dst) override;
  void cmd_build_tlas(const TlasBuildDesc& desc) override;

 private:
  struct PendingResolve {
    TextureHandle src_tex;
    TextureHandle dst_tex;
    uint32_t src_subresource = 0;
    uint32_t dst_subresource = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ResolveMode mode = ResolveMode::Average;
    bool is_depth = false;
  };
  struct BoundVertexBuffer {
    BufferHandle handle;
    uint64_t offset = 0;
  };

  void bind_global_heaps();
  void flush_vertex_buffers();
  void transition_resource_legacy(ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after);
  ComPtr<ID3D12Resource> allocate_transient_buffer(uint64_t size, D3D12_HEAP_TYPE heap_type,
                                                   D3D12_RESOURCE_STATES init_state);

  D3D12DeviceImpl& device_;
  QueueKind queue_kind_ = QueueKind::Graphics;
  ComPtr<ID3D12CommandAllocator> allocator_;
  ComPtr<ID3D12GraphicsCommandList4> cmd_;
  ComPtr<ID3D12GraphicsCommandList5> cmd5_;
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
  ComPtr<ID3D12GraphicsCommandList7> cmd7_;
#endif
  bool recording_ = false;
  HRESULT close_hr_ = S_OK;
  bool heaps_bound_ = false;
  PipelineHandle current_pipeline_;
  PipelineLayoutHandle current_layout_;
  bool current_is_compute_ = false;
  std::array<BoundVertexBuffer, 16> bound_vbs_{};
  uint32_t bound_vb_mask_ = 0;
  bool vb_dirty_ = false;
  std::vector<PendingResolve> active_resolves_;
  std::vector<ComPtr<ID3D12Resource>> transient_resources_;
};

class D3D12SwapchainImpl final : public Swapchain {
 public:
  explicit D3D12SwapchainImpl(D3D12DeviceImpl& device);
  ~D3D12SwapchainImpl() override;

  bool init(const SwapchainDesc& desc, std::string& error);

  bool resize(uint32_t width, uint32_t height, PresentMode mode,
              bool want_hdr, std::string& error) override;
  bool acquire_next_frame(SwapchainFrame& out_frame, uint64_t timeout_ns,
                          bool& out_of_date, std::string& error) override;
  CommandList& frame_command_list(uint32_t frame_slot) override;
  bool submit_and_present(const SwapchainFrame& frame, bool& out_of_date,
                          std::string& error) override;

  Format format() const override { return format_; }
  uint32_t width() const override { return width_; }
  uint32_t height() const override { return height_; }
  PresentMode active_present_mode() const override { return active_present_mode_; }
  HdrMode active_hdr_mode() const override { return active_hdr_mode_; }
  uint32_t image_count() const override { return static_cast<uint32_t>(texture_handles_.size()); }

 private:
  struct FrameSlot {
    std::unique_ptr<D3D12CommandListImpl> cmd_list;
    uint64_t fence_value = 0;
  };

  bool create_backbuffer_handles(std::string& error);
  void release_backbuffer_handles();
  void configure_colorspace(bool want_hdr);

  D3D12DeviceImpl& device_;
  SDL_Window* sdl_window_ = nullptr;
  HWND hwnd_ = nullptr;
  ComPtr<IDXGISwapChain4> swapchain_;
  HANDLE frame_latency_waitable_ = nullptr;
  ComPtr<ID3D12Fence> frame_fence_;
  HANDLE fence_event_ = nullptr;
  uint64_t next_fence_value_ = 1;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t buffer_count_ = 2;
  DXGI_FORMAT dxgi_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
  Format format_ = Format::Bgra8Unorm;
  PresentMode active_present_mode_ = PresentMode::Fifo;
  HdrMode active_hdr_mode_ = HdrMode::Off;
  bool allow_tearing_ = false;

  std::vector<TextureHandle> texture_handles_;
  std::vector<TextureViewHandle> view_handles_;
  std::vector<FrameSlot> frames_;
  uint32_t current_slot_ = 0;
};

class D3D12DeviceImpl final : public Device {
 public:
  D3D12DeviceImpl() = default;
  ~D3D12DeviceImpl() override;

  bool init(const DeviceDesc& desc, std::string& error);

  BackendKind backend() const override { return BackendKind::D3D12; }
  const BackendCaps& caps() const override { return caps_; }
  VramStats vram_stats() const override;
  uint32_t validation_message_count() const override;
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

  BindGroupLayoutHandle create_bind_group_layout(const BindGroupLayoutDesc& desc,
                                                 std::string& error) override;
  void destroy_bind_group_layout(BindGroupLayoutHandle handle) override;
  BindGroupHandle create_bind_group(const BindGroupDesc& desc, std::string& error) override;
  void destroy_bind_group(BindGroupHandle handle) override;
  std::unique_ptr<BindlessTable> create_bindless_table(const BindlessTableDesc& desc,
                                                       std::string& error) override;

  PipelineLayoutHandle create_pipeline_layout(const PipelineLayoutDesc& desc,
                                              std::string& error) override;
  void destroy_pipeline_layout(PipelineLayoutHandle handle) override;
  PipelineHandle create_graphics_pipeline(const GraphicsPipelineDesc& desc,
                                          std::string& error) override;
  PipelineHandle create_compute_pipeline(const ComputePipelineDesc& desc,
                                         std::string& error) override;
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

  TimelineSemaphoreHandle create_timeline_semaphore(uint64_t initial_value,
                                                    std::string& error) override;
  void destroy_timeline_semaphore(TimelineSemaphoreHandle handle) override;
  uint64_t timeline_semaphore_value(TimelineSemaphoreHandle handle) const override;
  bool wait_timeline_semaphore(TimelineSemaphoreHandle handle, uint64_t value,
                               uint64_t timeout_ns) override;
  bool signal_timeline_semaphore(TimelineSemaphoreHandle handle, uint64_t value) override;

  std::unique_ptr<CommandList> create_command_list(QueueKind queue,
                                                   std::string& error) override;
  bool submit(QueueKind queue,
              std::span<CommandList* const> lists,
              std::span<const TimelineWait> waits,
              std::span<const TimelineSignal> signals,
              std::string& error) override;

  bool submit_once(const std::function<void(CommandList&)>& record_fn,
                   std::string& error, bool prefer_transfer_queue) override;
  std::unique_ptr<Swapchain> create_swapchain(const SwapchainDesc& desc,
                                              std::string& error) override;

  // Internal accessors
  IDXGIFactory6* dxgi_factory() const noexcept { return factory_.Get(); }
  IDXGIAdapter4* dxgi_adapter() const noexcept { return adapter_.Get(); }
  ID3D12Device5* d3d_device() const noexcept { return device_.Get(); }
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
  ID3D12Device10* d3d_device10() const noexcept { return device10_.Get(); }
#endif
  ID3D12CommandQueue* direct_queue() const noexcept { return direct_queue_.Get(); }
  ID3D12CommandQueue* copy_queue() const noexcept { return copy_queue_.Get(); }
  ID3D12CommandQueue* compute_queue() const noexcept { return compute_queue_.Get(); }
  ID3D12CommandQueue* queue_for(QueueKind kind) const noexcept;
  bool allow_tearing() const noexcept { return allow_tearing_; }
  bool enhanced_barriers_enabled() const noexcept { return enhanced_barriers_enabled_; }
  bool sm66_dynamic_resources() const noexcept { return sm66_dynamic_resources_; }
  bool dxcompiler_available() const noexcept { return dxcompiler_available_; }
  std::mutex& queue_mutex() noexcept { return direct_queue_mutex_; }
  std::mutex& queue_mutex_for(QueueKind kind) noexcept;

  DescriptorAllocator& rtv_heap() noexcept { return rtv_heap_; }
  DescriptorAllocator& dsv_heap() noexcept { return dsv_heap_; }
  DescriptorAllocator& cbv_srv_uav_heap() noexcept { return cbv_srv_uav_heap_; }
  DescriptorAllocator& sampler_heap() noexcept { return sampler_heap_; }

  BufferEntry* lookup_buffer(BufferHandle h);
  const BufferEntry* lookup_buffer(BufferHandle h) const;
  TextureEntry* lookup_texture(TextureHandle h);
  const TextureEntry* lookup_texture(TextureHandle h) const;
  const TextureViewEntry* lookup_texture_view(TextureViewHandle h) const;
  const SamplerEntry* lookup_sampler(SamplerHandle h) const;
  const BindGroupLayoutEntry* lookup_bind_group_layout(BindGroupLayoutHandle h) const;
  const BindGroupEntry* lookup_bind_group(BindGroupHandle h) const;
  const PipelineLayoutEntry* lookup_pipeline_layout(PipelineLayoutHandle h) const;
  PipelineEntry* lookup_pipeline(PipelineHandle h);
  const PipelineEntry* lookup_pipeline(PipelineHandle h) const;
  const QueryPoolEntry* lookup_query_pool(QueryPoolHandle h) const;
  const AccelStructEntry* lookup_accel_struct(AccelStructHandle h) const;

  // Descriptor writing helpers (used by BindGroup and BindlessTable)
  void write_texture_srv(uint32_t heap_slot, TextureViewHandle view_h);
  void write_texture_uav(uint32_t heap_slot, TextureViewHandle view_h);
  void write_null_texture_srv(uint32_t heap_slot);
  void write_buffer_srv(uint32_t heap_slot, BufferHandle buf_h, uint64_t offset, uint64_t range);
  void write_buffer_uav(uint32_t heap_slot, BufferHandle buf_h, uint64_t offset, uint64_t range);
  void write_null_buffer_srv(uint32_t heap_slot);
  void write_null_buffer_uav(uint32_t heap_slot);
  void write_cbv(uint32_t heap_slot, BufferHandle buf_h, uint64_t offset, uint64_t range);
  void write_accel_struct_srv(uint32_t heap_slot, AccelStructHandle as_h);
  void write_sampler(uint32_t sampler_slot, SamplerHandle samp_h);

  // Command signature cache for ExecuteIndirect (keyed by (kind << 32) | stride)
  ID3D12CommandSignature* get_or_create_indirect_signature(bool indexed, uint32_t stride);
  ID3D12CommandSignature* get_or_create_dispatch_indirect_signature();

  // Dynamic depth-bias PSO variant lookup/creation
  ID3D12PipelineState* get_or_create_bias_pso(PipelineEntry& pe, float constant_factor,
                                              float clamp, float slope_factor);

  // Swapchain external backbuffer registration
  TextureHandle register_external_texture(ComPtr<ID3D12Resource> res, DXGI_FORMAT fmt,
                                          uint32_t w, uint32_t h);
  TextureViewHandle register_external_texture_view(TextureHandle tex, DXGI_FORMAT fmt);
  void unregister_external_texture_view(TextureViewHandle h);
  void unregister_external_texture(TextureHandle h);

  // RT internal implementation methods (defined in rhi_d3d12_rt.cpp)
  AccelStructHandle create_accel_struct_impl(const AccelStructDesc& desc, std::string& error);
  void cmd_build_blas_impl(ID3D12GraphicsCommandList4* cmd, const BlasBuildDesc& desc);
  void cmd_write_blas_compacted_size_impl(ID3D12GraphicsCommandList4* cmd, AccelStructHandle blas,
                                          QueryPoolHandle pool, uint32_t query_index);
  void cmd_copy_compact_blas_impl(ID3D12GraphicsCommandList4* cmd, AccelStructHandle src,
                                  AccelStructHandle dst);
  void cmd_build_tlas_impl(ID3D12GraphicsCommandList4* cmd, const TlasBuildDesc& desc);

  // Track validation errors from ID3D12InfoQueue1 callback
  void record_validation_error() noexcept { ++validation_errors_; }

  // Helper to resolve shader bytecode (raw DXIL/DXBC or optional HLSL compilation)
  bool extract_or_compile_shader(const ShaderModuleDesc& mod, std::vector<uint8_t>& out_bytecode,
                                 std::string& error) const;

 private:
  bool create_factory_and_adapter(const DeviceDesc& desc, std::string& error);
  bool probe_capabilities(const DeviceDesc& desc, std::string& error);
  void load_pipeline_library();
  void setup_info_queue();
  void teardown_info_queue();
  void signal_and_wait_queue(ID3D12CommandQueue* queue);

  ComPtr<IDXGIFactory6> factory_;
  ComPtr<IDXGIAdapter4> adapter_;
  ComPtr<ID3D12Device5> device_;
  ComPtr<ID3D12Device1> device1_;
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
  ComPtr<ID3D12Device10> device10_;
#endif
  ComPtr<ID3D12InfoQueue> info_queue_;
#ifdef __ID3D12InfoQueue1_INTERFACE_DEFINED__
  ComPtr<ID3D12InfoQueue1> info_queue1_;
  DWORD info_queue_cookie_ = 0;
#endif
  ComPtr<ID3D12PipelineLibrary> pipeline_library_;
  std::mutex pipeline_library_mutex_;

  ComPtr<ID3D12CommandQueue> direct_queue_;
  ComPtr<ID3D12CommandQueue> copy_queue_;
  ComPtr<ID3D12CommandQueue> compute_queue_;
  ComPtr<ID3D12Fence> idle_fence_;
  HANDLE idle_fence_event_ = nullptr;
  uint64_t idle_fence_value_ = 0;
  std::mutex idle_fence_mutex_;
  mutable std::mutex direct_queue_mutex_;
  mutable std::mutex compute_queue_mutex_;
  mutable std::mutex copy_queue_mutex_;

  DescriptorAllocator rtv_heap_;
  DescriptorAllocator dsv_heap_;
  DescriptorAllocator cbv_srv_uav_heap_;
  DescriptorAllocator sampler_heap_;
  PlacedHeapPool transient_heap_pool_;

  std::mutex sig_mutex_;
  std::unordered_map<uint64_t, ComPtr<ID3D12CommandSignature>> indirect_sigs_;

  BackendCaps caps_{};
  PipelineCacheKey cache_key_{};
  std::filesystem::path pipeline_cache_path_;
  bool pipeline_cache_loaded_ = false;
  bool validation_enabled_ = false;
  bool allow_tearing_ = false;
  bool enhanced_barriers_enabled_ = false;
  bool sm66_dynamic_resources_ = false;
  bool dxcompiler_available_ = false;
  HMODULE dxcompiler_module_ = nullptr;
  std::atomic<uint32_t> validation_errors_{0};
  std::atomic<uint32_t> live_alloc_count_{0};
  std::atomic<uint64_t> live_alloc_bytes_{0};

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
  HandlePool<TimelineSemaphoreHandle, TimelineSemaphoreEntry> timeline_semaphores_;
};

}  // namespace shift::rhi::d3d12
