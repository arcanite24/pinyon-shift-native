#include "rhi_backend_bridge.h"

#include <algorithm>
#include <cstring>

namespace carview {

ModernRhiRuntime::~ModernRhiRuntime() {
  shutdown();
}

bool ModernRhiRuntime::init(const ModernRuntimeConfig& cfg, std::string& error) {
  shutdown();
  cfg_ = cfg;

  shift::rhi::DeviceDesc dd{};
  dd.backend = cfg.backend;
  dd.enable_validation = cfg.enable_validation;
  dd.enable_async_compute = true;
  dd.enable_transfer_queue = true;
  dd.enable_bindless = true;
  dd.app_name = "shift-carview-modern";

  device_ = shift::rhi::create_device(dd, error);
  if (!device_) return false;

  vis_ = std::make_unique<shift::rhi::VisBufferPipeline>(*device_);
  if (!vis_->init(cfg.max_width, cfg.max_height, error, cfg.max_draws)) {
    shutdown();
    return false;
  }

  post_ = std::make_unique<shift::rhi::ComputePostFxPipeline>(*device_);
  if (!post_->init(cfg.max_width, cfg.max_height, error)) {
    shutdown();
    return false;
  }

  detiler_ = std::make_unique<shift::rhi::XenosDetileGpu>(*device_);
  if (!detiler_->init(error)) {
    shutdown();
    return false;
  }

  if (!cfg.native_cache_dir.empty()) {
    cache_ = std::make_unique<fh1::NativeCache>(cfg.native_cache_dir);
    if (cfg.native_cache_budget_bytes > 0) {
      cache_->trim_to_budget(cfg.native_cache_budget_bytes);
    }
  }

  render_graph_ = std::make_unique<shift::rhi::RenderGraph>(*device_);

  render_thread_.start(
      [this](const RenderSnapshot& snap) {
        static_assert(sizeof(RenderSnapshot::LightData) == sizeof(shift::rhi::ClusterLightGpu));
        std::span<const shift::rhi::ClusterLightGpu> snap_lights;
        if (!snap.lights.empty()) {
          snap_lights = std::span<const shift::rhi::ClusterLightGpu>(
              reinterpret_cast<const shift::rhi::ClusterLightGpu*>(snap.lights.data()),
              snap.lights.size());
        }
        std::string err;
        if (!render_snapshot_sync(snap, snap_lights, err)) {
          std::lock_guard<std::mutex> lk(error_mu_);
          last_render_error_ = std::move(err);
        }
      },
      cfg.enable_render_thread);

  ready_ = true;
  return true;
}

void ModernRhiRuntime::shutdown() {
  render_thread_.stop();
  streamer_.reset();
  if (render_graph_) {
    render_graph_->release_pool();
    render_graph_.reset();
  }
  if (device_) device_->wait_idle();
  if (detiler_) { detiler_->shutdown(); detiler_.reset(); }
  if (post_) { post_->shutdown(); post_.reset(); }
  if (vis_) { vis_->shutdown(); vis_.reset(); }
  device_.reset();
  cache_.reset();
  car_pool_.collect_unused();
  ready_ = false;
}

bool ModernRhiRuntime::upload_scene(const shift::rhi::VisSceneUpload& scene, std::string& error) {
  if (!ready_ || !vis_) {
    error = "ModernRhiRuntime::upload_scene: runtime not initialized";
    return false;
  }
  render_thread_.flush();
  return vis_->upload_scene(scene, error);
}

bool ModernRhiRuntime::render_snapshot_sync(const RenderSnapshot& snap,
                                            std::span<const shift::rhi::ClusterLightGpu> lights,
                                            std::string& error) {
  if (!ready_ || !device_ || !vis_ || !post_) {
    error = "ModernRhiRuntime::render_snapshot_sync: runtime not initialized";
    return false;
  }

  shift::rhi::VisFrameView view{};
  std::memcpy(view.view, snap.view, sizeof(view.view));
  std::memcpy(view.view_proj, snap.view_proj, sizeof(view.view_proj));
  std::memcpy(view.prev_view_proj, snap.prev_view_proj, sizeof(view.prev_view_proj));
  std::memcpy(view.inv_proj, snap.inv_proj, sizeof(view.inv_proj));
  for (int i = 0; i < 3; ++i) {
    view.camera_pos[i] = snap.camera_pos[i];
    view.sun_dir[i] = snap.sun_dir[i];
    view.sun_color[i] = snap.sun_color[i];
  }
  view.sun_intensity = snap.sun_intensity;
  view.width = std::clamp(snap.viewport_width, 1u, cfg_.max_width);
  view.height = std::clamp(snap.viewport_height, 1u, cfg_.max_height);

  shift::rhi::PostFxConfig pcfg{};
  pcfg.width = view.width;
  pcfg.height = view.height;
  pcfg.manual_exposure = snap.use_manual_exposure ? snap.exposure : 0.0f;

  if (!render_graph_) {
    render_graph_ = std::make_unique<shift::rhi::RenderGraph>(*device_);
  }
  shift::rhi::RenderGraph& rg = *render_graph_;
  rg.reset();
  shift::rhi::VisFrameOutputs vo = vis_->add_frame_passes(rg, view, lights, error);
  if (!vo.hdr_color.valid()) return false;

  shift::rhi::PostFxOutputs po = post_->add_frame_passes(
      rg, vo.hdr_color, vis_->hdr_view(), vo.depth_buffer, vis_->depth_view(),
      vo.velocity, vis_->velocity_view(), pcfg, snap.dt_seconds, error);
  if (!po.ldr_output.valid()) return false;

  // Reuse persistent RenderGraph timeline semaphores & command lists instead of
  // stalling the whole device every frame with wait_idle().
  if (!rg.compile(error) || !rg.execute(error, /*wait_for_completion=*/false)) return false;
  return true;
}

RenderSnapshot& ModernRhiRuntime::begin_frame() {
  return render_thread_.begin_sim_snapshot();
}

void ModernRhiRuntime::submit_frame(std::span<const shift::rhi::ClusterLightGpu> lights) {
  RenderSnapshot& snap = render_thread_.current_sim_snapshot();
  snap.lights.resize(lights.size());
  if (!lights.empty()) {
    static_assert(sizeof(RenderSnapshot::LightData) == sizeof(shift::rhi::ClusterLightGpu));
    std::memcpy(snap.lights.data(), lights.data(), lights.size_bytes());
  }
  render_thread_.publish_snapshot();
}

void ModernRhiRuntime::flush() {
  render_thread_.flush();
  if (render_graph_) render_graph_->wait_for_last_execution();
  if (streamer_) streamer_->wait_idle();
  if (device_) device_->wait_idle();
}

ModernRuntimeTelemetry ModernRhiRuntime::telemetry(std::string* error) {
  ModernRuntimeTelemetry out{};
  if (!ready_ || !device_) return out;
  render_thread_.flush();
  if (render_graph_) render_graph_->wait_for_last_execution();
  out.active_backend = device_->backend();
  out.adapter_name = device_->caps().device_name;
  out.validation_messages = device_->validation_message_count();
  std::string err;
  {
    std::lock_guard<std::mutex> lk(error_mu_);
    if (!last_render_error_.empty()) err = last_render_error_;
  }
  if (vis_) vis_->readback_telemetry(out.vis, err);
  if (post_) post_->readback_exposure(out.exposure, err);
  out.render_thread = render_thread_.stats();
  if (cache_) out.native_cache = cache_->stats();
  if (streamer_) out.resident_cells = static_cast<uint32_t>(streamer_->resident_count());
  out.shared_car_saved_vram = car_pool_.saved_vram_bytes();
  if (error && !err.empty()) *error = err;
  return out;
}

}  // namespace carview
