#include "render_thread.h"

#include <chrono>

namespace carview {

RenderThread::~RenderThread() {
  stop();
}

void RenderThread::start(RenderFn render_fn, bool threaded) {
  stop();
  render_fn_ = std::move(render_fn);
  threaded_.store(threaded, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  sim_slot_ = 0;
  slot_busy_[0] = slot_busy_[1] = false;
  has_pending_ = false;
  stop_requested_ = false;
  frames_submitted_ = 0;
  frames_completed_ = 0;
  last_sim_wait_ms_ = 0.0;
  last_render_ms_ = 0.0;

  if (threaded) {
    worker_ = std::thread([this]() { worker_loop(); });
  }
}

void RenderThread::stop() {
  if (!running_.load(std::memory_order_acquire)) return;
  if (threaded_.load(std::memory_order_acquire)) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_requested_ = true;
    }
    cv_worker_.notify_all();
    cv_sim_.notify_all();
    if (worker_.joinable()) worker_.join();
  }
  running_.store(false, std::memory_order_release);
  threaded_.store(false, std::memory_order_release);
  render_fn_ = {};
}

RenderSnapshot& RenderThread::begin_sim_snapshot() {
  if (!threaded_.load(std::memory_order_acquire)) {
    return slots_[0];
  }
  const auto t0 = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lk(mu_);
  // Always wait until the worker finishes reading sim_slot_ (never return a slot
  // still being read by worker_loop(), even if stop_requested_ was set).
  cv_sim_.wait(lk, [this]() { return !slot_busy_[sim_slot_]; });
  const auto t1 = std::chrono::steady_clock::now();
  last_sim_wait_ms_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
  return slots_[sim_slot_];
}

void RenderThread::publish_snapshot() {
  if (!running_.load(std::memory_order_acquire)) return;
  if (!threaded_.load(std::memory_order_acquire)) {
    const auto t0 = std::chrono::steady_clock::now();
    if (render_fn_) render_fn_(slots_[0]);
    const auto t1 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mu_);
    ++frames_submitted_;
    ++frames_completed_;
    last_render_ms_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return;
  }

  {
    std::unique_lock<std::mutex> lk(mu_);
    // Wait only if the previous pending slot hasn't been picked up yet (back-pressure = 2 frames max).
    cv_sim_.wait(lk, [this]() { return !has_pending_ || stop_requested_; });
    if (stop_requested_) return;
    slot_busy_[sim_slot_] = true;
    pending_slot_ = sim_slot_;
    has_pending_ = true;
    ++frames_submitted_;
    sim_slot_ ^= 1u;
  }
  cv_worker_.notify_one();
}

void RenderThread::flush() {
  if (!threaded_.load(std::memory_order_acquire)) return;
  std::unique_lock<std::mutex> lk(mu_);
  cv_sim_.wait(lk, [this]() { return !has_pending_ && !slot_busy_[0] && !slot_busy_[1]; });
}

RenderThreadStats RenderThread::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  return {frames_submitted_, frames_completed_, last_sim_wait_ms_, last_render_ms_,
          threaded_.load(std::memory_order_acquire) && running_.load(std::memory_order_acquire)};
}

void RenderThread::worker_loop() {
  for (;;) {
    uint32_t work_slot = 0;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_worker_.wait(lk, [this]() { return has_pending_ || stop_requested_; });
      if (!has_pending_ && stop_requested_) return;
      work_slot = pending_slot_;
      has_pending_ = false;
    }
    // Wake sim thread (and any waiters) immediately so Frame N+1 can populate the other ping-pong slot.
    cv_sim_.notify_all();

    const auto t0 = std::chrono::steady_clock::now();
    if (render_fn_) render_fn_(slots_[work_slot]);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    {
      std::lock_guard<std::mutex> lk(mu_);
      slot_busy_[work_slot] = false;
      ++frames_completed_;
      last_render_ms_ = ms;
    }
    cv_sim_.notify_all();
  }
}

}  // namespace carview
