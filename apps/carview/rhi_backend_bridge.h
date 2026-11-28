// Unified Modern Engine Runtime Bridge for apps/carview:
// Integrates libs/rhi (Vulkan 1.3 / DirectX 12 / Metal 3), rhi::RenderGraph,
// rhi::VisBufferPipeline + Clustered Forward+ Lights, rhi::ComputePostFxPipeline
// (SPD Hi-Z + 2-Phase Cull + Async Compute Bloom/Exposure/Mblur/Linear HDR TAA-U),
// rhi::XenosDetileGpu, fh1::NativeCache (SHNC0001), world::CellGridStreamer (128 m),
// world::CarAssetPool, and double-buffered N/N-1 RenderThread into carview_render.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "fh1data/native_cache.h"
#include "render_thread.h"
#include "rhi/post_fx_compute.h"
#include "rhi/render_graph.h"
#include "rhi/rhi.h"
#include "rhi/vis_buffer.h"
#include "rhi/xenos_detile_gpu.h"
#include "world/cell_streamer.h"

namespace carview {

struct ModernRuntimeConfig {
  shift::rhi::BackendKind backend = shift::rhi::BackendKind::Auto;
  uint32_t max_width = 1920;
  uint32_t max_height = 1080;
  uint32_t max_draws = 65536;
  bool enable_validation = false;
  bool enable_render_thread = true;
  std::filesystem::path native_cache_dir;
  uint64_t native_cache_budget_bytes = 2ull * 1024ull * 1024ull * 1024ull;  // 2 GiB LRU cap
};

struct ModernRuntimeTelemetry {
  shift::rhi::BackendKind active_backend = shift::rhi::BackendKind::Auto;
  std::string adapter_name;
  uint64_t validation_messages = 0;
  shift::rhi::VisTelemetry vis{};
  shift::rhi::ExposureTelemetry exposure{};
  RenderThreadStats render_thread{};
  fh1::NativeCacheStats native_cache{};
  uint32_t resident_cells = 0;
  uint64_t shared_car_saved_vram = 0;
};

class ModernRhiRuntime {
 public:
  ModernRhiRuntime() = default;
  ~ModernRhiRuntime();

  ModernRhiRuntime(const ModernRhiRuntime&) = delete;
  ModernRhiRuntime& operator=(const ModernRhiRuntime&) = delete;

  bool init(const ModernRuntimeConfig& cfg, std::string& error);
  void shutdown();
  bool is_ready() const noexcept { return ready_; }

  // Uploads scene meshlets & materials into the active VisBufferPipeline.
  bool upload_scene(const shift::rhi::VisSceneUpload& scene, std::string& error);

  // Executes one full modern frame (VisBuffer + Clustered Forward+ + SPD Hi-Z +
  // Async Compute Post-FX + Pre-Tonemap Linear HDR TAA-U) on the active RHI backend.
  bool render_snapshot_sync(const RenderSnapshot& snap,
                            std::span<const shift::rhi::ClusterLightGpu> lights,
                            std::string& error);

  // Submits a snapshot onto the double-buffered N/N-1 RenderThread.
  RenderSnapshot& begin_frame();
  void submit_frame(std::span<const shift::rhi::ClusterLightGpu> lights = {});
  void flush();

  // Direct subsystem accessors for live carview integration.
  shift::rhi::Device* device() const noexcept { return device_.get(); }
  shift::rhi::VisBufferPipeline* vis_buffer() const noexcept { return vis_.get(); }
  shift::rhi::ComputePostFxPipeline* post_fx() const noexcept { return post_.get(); }
  shift::rhi::XenosDetileGpu* xenos_detiler() const noexcept { return detiler_.get(); }
  fh1::NativeCache* native_cache() const noexcept { return cache_.get(); }
  world::CarAssetPool& car_pool() noexcept { return car_pool_; }
  world::CellGridStreamer* cell_streamer() const noexcept { return streamer_.get(); }
  void attach_cell_streamer(std::unique_ptr<world::CellGridStreamer> streamer) {
    streamer_ = std::move(streamer);
  }

  ModernRuntimeTelemetry telemetry(std::string* error = nullptr);

 private:
  bool ready_ = false;
  ModernRuntimeConfig cfg_{};
  std::unique_ptr<shift::rhi::Device> device_;
  std::unique_ptr<shift::rhi::VisBufferPipeline> vis_;
  std::unique_ptr<shift::rhi::ComputePostFxPipeline> post_;
  std::unique_ptr<shift::rhi::XenosDetileGpu> detiler_;
  std::unique_ptr<fh1::NativeCache> cache_;
  std::unique_ptr<world::CellGridStreamer> streamer_;
  world::CarAssetPool car_pool_;
  std::unique_ptr<shift::rhi::RenderGraph> render_graph_;
  RenderThread render_thread_;
  std::mutex error_mu_;
  std::string last_render_error_;
};

}  // namespace carview
