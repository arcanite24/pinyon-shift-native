// Declarative Frame Render Graph for shift::rhi.
// Manages pass dependency ordering, dead-pass culling, batched resource barriers,
// cross-queue TimelineSemaphore synchronization (Graphics / AsyncCompute / Transfer),
// and transient texture/buffer aliasing across non-overlapping pass lifetimes.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rhi/rhi.h"

namespace shift::rhi {

struct RGTextureHandle {
  uint32_t index = UINT32_MAX;

  constexpr bool valid() const noexcept { return index != UINT32_MAX; }
  explicit constexpr operator bool() const noexcept { return valid(); }
  constexpr bool operator==(const RGTextureHandle&) const noexcept = default;
};

struct RGBufferHandle {
  uint32_t index = UINT32_MAX;

  constexpr bool valid() const noexcept { return index != UINT32_MAX; }
  explicit constexpr operator bool() const noexcept { return valid(); }
  constexpr bool operator==(const RGBufferHandle&) const noexcept = default;
};

struct RenderExtent {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t max_width = 0;
  uint32_t max_height = 0;

  float uv_scale_x() const noexcept {
    return max_width > 0 ? static_cast<float>(width) / static_cast<float>(max_width) : 1.0f;
  }
  float uv_scale_y() const noexcept {
    return max_height > 0 ? static_cast<float>(height) / static_cast<float>(max_height) : 1.0f;
  }
};

struct RenderGraphStats {
  uint32_t total_passes = 0;
  uint32_t executed_passes = 0;
  uint32_t culled_passes = 0;
  uint32_t transient_textures_declared = 0;
  uint32_t physical_textures_allocated = 0;
  uint32_t aliased_textures = 0;
  uint32_t transient_buffers_declared = 0;
  uint32_t physical_buffers_allocated = 0;
  uint32_t aliased_buffers = 0;
  uint32_t queue_submissions = 0;
  uint32_t cross_queue_syncs = 0;
};

class RenderGraph;

// Builder passed to pass setup callbacks to declare resource reads/writes.
class RenderGraphBuilder {
 public:
  void read_texture(RGTextureHandle handle, ResourceState state = ResourceState::ShaderRead);
  void write_texture(RGTextureHandle handle, ResourceState state = ResourceState::RenderTarget);
  void read_buffer(RGBufferHandle handle, ResourceState state = ResourceState::ShaderRead);
  void write_buffer(RGBufferHandle handle, ResourceState state = ResourceState::StorageReadWrite);
  void has_side_effects();

 private:
  friend class RenderGraph;
  RenderGraphBuilder(RenderGraph& graph, uint32_t pass_index)
      : graph_(graph), pass_index_(pass_index) {}

  RenderGraph& graph_;
  uint32_t pass_index_ = 0;
};

// Context passed to pass execute callbacks to resolve logical handles to live RHI handles.
class RenderGraphContext {
 public:
  TextureHandle texture(RGTextureHandle handle) const noexcept;
  TextureViewHandle texture_view(RGTextureHandle handle) const noexcept;
  BufferHandle buffer(RGBufferHandle handle) const noexcept;
  const RenderExtent& render_extent() const noexcept;

 private:
  friend class RenderGraph;
  explicit RenderGraphContext(const RenderGraph& graph) : graph_(graph) {}

  const RenderGraph& graph_;
};

class RenderGraph {
 public:
  explicit RenderGraph(Device& device);
  ~RenderGraph();

  RenderGraph(const RenderGraph&) = delete;
  RenderGraph& operator=(const RenderGraph&) = delete;

  // Resets per-frame pass and logical resource declarations while keeping the
  // pooled physical transient resources and timeline semaphore warm for reuse.
  void reset();

  // Releases all cached physical transient resources, command lists, and timeline
  // semaphores (call before destroying the Device if RenderGraph outlives a scene).
  void release_pool();

  // Enable or disable dead-pass culling during compile() (enabled by default).
  void set_culling_enabled(bool enabled) noexcept { culling_enabled_ = enabled; }

  // Stall-free Dynamic Resolution Scaling (DRS): sets active render viewport
  // extent within fixed max-resolution transient textures (zero device wait_idle).
  void set_render_extent(uint32_t width, uint32_t height,
                         uint32_t max_width = 0, uint32_t max_height = 0) noexcept {
    render_extent_.width = width;
    render_extent_.height = height;
    render_extent_.max_width = max_width > 0 ? max_width : width;
    render_extent_.max_height = max_height > 0 ? max_height : height;
  }
  const RenderExtent& render_extent() const noexcept { return render_extent_; }

  // Declare transient resources (automatically allocated and aliased by lifetime).
  RGTextureHandle create_texture(const char* name, const TextureDesc& desc);
  RGBufferHandle create_buffer(const char* name, const BufferDesc& desc);

  // Import persistent/external resources (e.g., swapchain backbuffers, world buffers).
  RGTextureHandle import_texture(const char* name,
                                 TextureHandle texture,
                                 TextureViewHandle view,
                                 ResourceState initial_state,
                                 ResourceState final_state = ResourceState::Undefined,
                                 Format format = Format::Undefined);
  RGBufferHandle import_buffer(const char* name,
                               BufferHandle buffer,
                               ResourceState initial_state,
                               ResourceState final_state = ResourceState::Undefined);

  // Register a render, compute, or transfer pass.
  using SetupFn = std::function<void(RenderGraphBuilder&)>;
  using ExecuteFn = std::function<void(CommandList&, const RenderGraphContext&)>;
  uint32_t add_pass(const char* name, QueueKind queue, SetupFn setup_fn, ExecuteFn execute_fn);

  // Compiles pass DAG, culls unused passes, schedules execution order, aliases
  // transient resources, and plans intra-queue barriers + cross-queue timeline syncs.
  bool compile(std::string& error);

  // Executes the compiled graph across Graphics / AsyncCompute / Transfer queues
  // using batched Device::submit() calls with automatic TimelineSemaphore waits/signals.
  // Returns the final timeline value signaled (0 if no submissions were needed).
  bool execute(std::string& error,
               bool wait_for_completion = false,
               uint64_t* out_final_timeline_value = nullptr);

  // Executes a single-queue compiled graph into a caller-owned CommandList
  // (e.g., Swapchain::frame_command_list()). Fails if compiled passes require
  // multiple distinct queues.
  bool execute_into(CommandList& cmd, std::string& error);

  // Wait on CPU for the last executed frame's timeline value.
  bool wait_for_last_execution(uint64_t timeout_ns = UINT64_MAX);

  const RenderGraphStats& stats() const noexcept { return stats_; }
  const std::vector<uint32_t>& execution_order() const noexcept { return exec_order_; }

 private:
  friend class RenderGraphBuilder;
  friend class RenderGraphContext;

  struct ResourceAccess {
    uint32_t resource_index = 0;
    ResourceState state = ResourceState::Common;
    bool is_write = false;
  };

  struct TextureNode {
    std::string name;
    bool imported = false;
    TextureDesc desc{};
    TextureHandle handle{};
    TextureViewHandle view{};
    ResourceState initial_state = ResourceState::Undefined;
    ResourceState final_state = ResourceState::Undefined;
    Format format = Format::Undefined;
    uint32_t physical_slot = UINT32_MAX;
    uint32_t first_exec_step = UINT32_MAX;
    uint32_t last_exec_step = 0;
  };

  struct BufferNode {
    std::string name;
    bool imported = false;
    BufferDesc desc{};
    BufferHandle handle{};
    ResourceState initial_state = ResourceState::Common;
    ResourceState final_state = ResourceState::Undefined;
    uint32_t physical_slot = UINT32_MAX;
    uint32_t first_exec_step = UINT32_MAX;
    uint32_t last_exec_step = 0;
  };

  struct PassNode {
    std::string name;
    QueueKind queue = QueueKind::Graphics;
    ExecuteFn execute;
    std::vector<ResourceAccess> texture_accesses;
    std::vector<ResourceAccess> buffer_accesses;
    bool side_effects = false;
    bool culled = false;
  };

  struct PlannedBarrier {
    std::vector<TextureBarrier> texture_barriers;
    std::vector<BufferBarrier> buffer_barriers;
  };

  struct QueueBatch {
    QueueKind queue = QueueKind::Graphics;
    std::vector<uint32_t> exec_steps;  // indices into exec_order_
    std::vector<uint32_t> wait_batches;
    bool signals_timeline = false;
    uint64_t signal_value = 0;
  };

  struct PooledTexture {
    TextureDesc desc{};
    TextureHandle texture{};
    TextureViewHandle view{};
    uint32_t last_used_exec_step = UINT32_MAX;
    uint32_t last_owner_pass = UINT32_MAX;
    ResourceState current_state = ResourceState::Undefined;
    bool used_this_frame = false;
  };

  struct PooledBuffer {
    BufferDesc desc{};
    BufferHandle buffer{};
    uint32_t last_used_exec_step = UINT32_MAX;
    uint32_t last_owner_pass = UINT32_MAX;
    bool used_this_frame = false;
  };

  static bool texture_desc_compatible(const TextureDesc& a, const TextureDesc& b) noexcept;
  static bool buffer_desc_compatible(const BufferDesc& a, const BufferDesc& b) noexcept;

  bool materialize_transient_resources(std::string& error);
  bool ensure_sync_objects(std::string& error);

  Device& device_;
  bool culling_enabled_ = true;
  bool compiled_ = false;
  RenderExtent render_extent_{};

  std::vector<TextureNode> textures_;
  std::vector<BufferNode> buffers_;
  std::vector<PassNode> passes_;

  // Compiled schedule
  std::vector<uint32_t> exec_order_;
  std::vector<std::vector<uint32_t>> pass_deps_;  // pass_deps_[p] = predecessor pass indices
  std::vector<PlannedBarrier> pre_pass_barriers_;
  std::vector<PlannedBarrier> post_pass_barriers_;
  PlannedBarrier epilogue_barriers_;
  std::vector<QueueBatch> batches_;
  RenderGraphStats stats_{};

  // Persistent pools reused across frames
  std::vector<PooledTexture> texture_pool_;
  std::vector<PooledBuffer> buffer_pool_;
  std::vector<std::unique_ptr<CommandList>> cmd_lists_[3];
  uint32_t cmd_list_cursor_[3] = {0, 0, 0};

  TimelineSemaphoreHandle timeline_sems_[3]{};
  uint64_t timeline_counters_[3] = {0, 0, 0};
  uint64_t last_submitted_timelines_[3] = {0, 0, 0};
};

}  // namespace shift::rhi
