// Double-Buffered Frame N+1 / Frame N Dedicated Render Thread for Shift Engine.
//
// Decouples 120 Hz vehicle/AI/traffic simulation (Main/Sim thread) from RHI
// RenderGraph recording and GPU submission (RenderThread) via a ping-pong pair
// of RenderSnapshots, achieving full CPU/GPU frame overlap with deterministic
// synchronous bypass for headless/replay runs.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace carview {

struct RenderVehiclePose {
  uint32_t actor_id = 0;
  float model[16] = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1,
  };
  float prev_model[16] = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1,
  };
  float wheel_spin[4] = {0, 0, 0, 0};
  float wheel_steer[2] = {0, 0};
  float brake_intensity = 0.0f;
  bool headlights_on = false;
};

struct RenderSnapshot {
  uint64_t frame_id = 0;
  float dt_seconds = 1.0f / 60.0f;
  uint32_t viewport_width = 1280;
  uint32_t viewport_height = 720;
  float view[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float view_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float prev_view_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float inv_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float camera_pos[3] = {0, 2, 0};
  float camera_vel[3] = {0, 0, 0};
  float sun_dir[3] = {0.38f, 0.87f, 0.29f};
  float sun_color[3] = {1.0f, 0.95f, 0.88f};
  float sun_intensity = 3.2f;
  float exposure = 1.0f;
  bool use_manual_exposure = false;
  std::vector<RenderVehiclePose> vehicles;
  // Snapshot-owned lights (64-byte ClusterLightGpu layout: pos_radius, color_intensity, dir_cos_outer, extra).
  struct LightData {
    float pos_radius[4] = {0, 0, 0, 10.0f};
    float color_intensity[4] = {1, 1, 1, 1.0f};
    float dir_cos_outer[4] = {0, -1, 0, 0.5f};
    float cos_inner = 0.8f;
    uint32_t type = 0;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
  };
  static_assert(sizeof(LightData) == 64);
  std::vector<LightData> lights;
};

struct RenderThreadStats {
  uint64_t frames_submitted = 0;
  uint64_t frames_completed = 0;
  double last_sim_wait_ms = 0.0;
  double last_render_ms = 0.0;
  bool threaded_active = false;
};

class RenderThread {
 public:
  using RenderFn = std::function<void(const RenderSnapshot&)>;

  RenderThread() = default;
  ~RenderThread();

  RenderThread(const RenderThread&) = delete;
  RenderThread& operator=(const RenderThread&) = delete;

  // Starts the dedicated render worker thread (when `threaded == true`).
  // When `threaded == false`, publish() executes `render_fn` synchronously inline.
  void start(RenderFn render_fn, bool threaded = true);
  void stop();

  bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }
  bool is_threaded() const noexcept { return threaded_.load(std::memory_order_acquire); }

  // Acquires the writable simulation snapshot buffer (waits only if the previous
  // render of this ping-pong slot is still in flight — Frame N+1 overlaps Frame N!).
  RenderSnapshot& begin_sim_snapshot();

  // Returns a reference to the current writable sim snapshot (valid between begin/publish).
  RenderSnapshot& current_sim_snapshot() noexcept {
    return slots_[threaded_.load(std::memory_order_acquire) ? sim_slot_ : 0u];
  }

  // Publishes the filled snapshot to the render thread and immediately returns so
  // the simulation thread can start Frame N+1 concurrently.
  void publish_snapshot();

  // Waits until all published snapshots have finished rendering.
  void flush();

  RenderThreadStats stats() const;

 private:
  void worker_loop();

  RenderFn render_fn_;
  std::atomic<bool> running_{false};
  std::atomic<bool> threaded_{false};
  std::thread worker_;

  mutable std::mutex mu_;
  std::condition_variable cv_worker_;
  std::condition_variable cv_sim_;

  RenderSnapshot slots_[2]{};
  uint32_t sim_slot_ = 0;
  bool slot_busy_[2] = {false, false};
  uint32_t pending_slot_ = 0;
  bool has_pending_ = false;
  bool stop_requested_ = false;

  uint64_t frames_submitted_ = 0;
  uint64_t frames_completed_ = 0;
  double last_sim_wait_ms_ = 0.0;
  double last_render_ms_ = 0.0;
};

}  // namespace carview
