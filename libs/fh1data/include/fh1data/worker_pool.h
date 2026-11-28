// Unified multi-lane work-stealing job system for Shift Engine.
//
// Replaces ad-hoc std::thread bursts and fragmented per-subsystem pools with two
// persistent priority lanes that support concurrent AND nested parallel_for calls
// with zero call_mutex_ serialization and zero per-dispatch heap allocations:
//   - JobLane::kFrameCritical: normal-priority workers with short spin-before-park
//     for sub-10 us 120 Hz physics, AI, traffic, and interactive frame tasks.
//   - JobLane::kBackground: lowered-priority workers for archive/world/collision/
//     prop/smashable/car loading, texture decoding, and pipeline compilation.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(__ANDROID__)
#include <sys/resource.h>
#endif

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

namespace fh1 {

enum class JobLane : uint8_t {
  kBackground = 0,
  kFrameCritical = 1,
};

namespace detail {

#if defined(_WIN32)
void lower_worker_thread_priority_win32();
#endif

inline void cpu_pause() noexcept {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
  _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
  asm volatile("yield" ::: "memory");
#else
  std::this_thread::yield();
#endif
}

inline void configure_worker_thread(JobLane lane) {
  if (lane == JobLane::kBackground) {
#if defined(_WIN32)
    lower_worker_thread_priority_win32();
#elif defined(__ANDROID__)
    setpriority(PRIO_PROCESS, 0, 8);
#endif
  }
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
  // Flush denormals to zero (FTZ + DAZ) on all job workers for consistent FP throughput.
  _mm_setcsr(_mm_getcsr() | 0x8040u);
#endif
}

inline thread_local int g_worker_lane = -1;
inline thread_local size_t g_worker_index = 0;

struct ParallelBatch {
  void (*fn)(void*, size_t) = nullptr;
  void* ctx = nullptr;
  size_t count = 0;
  size_t chunk = 1;
  std::atomic<size_t> next{0};
  std::atomic<size_t> active_workers{0};

  void steal_all() {
    for (;;) {
      const size_t begin = next.fetch_add(chunk, std::memory_order_relaxed);
      if (begin >= count) break;
      const size_t end = std::min(begin + chunk, count);
      for (size_t i = begin; i < end; ++i) fn(ctx, i);
    }
  }
};

class LanePool {
 public:
  LanePool(JobLane lane, size_t num_workers, uint32_t spin_iters)
      : lane_(lane), spin_iters_(spin_iters) {
    workers_.reserve(num_workers);
    for (size_t t = 0; t < num_workers; ++t) {
      workers_.emplace_back([this, t] { worker_loop(t); });
    }
  }

  ~LanePool() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_work_.notify_all();
    for (auto& w : workers_) {
      if (w.joinable()) w.join();
    }
  }

  LanePool(const LanePool&) = delete;
  LanePool& operator=(const LanePool&) = delete;

  size_t worker_count() const noexcept { return workers_.size(); }

  void run_batch(size_t count, size_t chunk, void (*trampoline)(void*, size_t), void* ctx) {
    if (count == 0) return;
    if (count == 1 || workers_.empty()) {
      for (size_t i = 0; i < count; ++i) trampoline(ctx, i);
      return;
    }

    ParallelBatch batch;
    batch.fn = trampoline;
    batch.ctx = ctx;
    batch.count = count;
    batch.chunk = std::clamp<size_t>(chunk, 1, count);

    {
      std::lock_guard<std::mutex> lk(mu_);
      batches_.push_back(&batch);
      pending_work_.fetch_add(1, std::memory_order_release);
    }
    const size_t chunks = (count + batch.chunk - 1) / batch.chunk;
    if (chunks <= 2) {
      cv_work_.notify_one();
    } else {
      cv_work_.notify_all();
    }

    // RAII guard guarantees batch unlink + worker drain even if `trampoline` throws,
    // preventing workers from dereferencing a stack-unwound ParallelBatch.
    struct BatchGuard {
      LanePool& pool;
      ParallelBatch& b;
      ~BatchGuard() {
        b.next.store(b.count, std::memory_order_relaxed);
        {
          std::lock_guard<std::mutex> lk(pool.mu_);
          auto it = std::find(pool.batches_.begin(), pool.batches_.end(), &b);
          if (it != pool.batches_.end()) {
            pool.batches_.erase(it);
            pool.pending_work_.fetch_sub(1, std::memory_order_release);
          }
        }
        if (b.active_workers.load(std::memory_order_acquire) != 0) {
          for (uint32_t s = 0; s < 256; ++s) {
            if (b.active_workers.load(std::memory_order_acquire) == 0) return;
            cpu_pause();
          }
          std::unique_lock<std::mutex> dlk(pool.done_mu_);
          pool.done_cv_.wait(dlk, [&] { return b.active_workers.load(std::memory_order_acquire) == 0; });
        }
      }
    } guard{*this, batch};

    // Calling thread always participates so nested calls never deadlock and
    // zero threads sit idle while waiting for peers.
    batch.steal_all();
  }

  void enqueue_detached(std::function<void()> task) {
    if (workers_.empty()) {
      task();
      return;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      detached_.push_back(std::move(task));
      pending_work_.fetch_add(1, std::memory_order_release);
    }
    cv_work_.notify_one();
  }

 private:
  void worker_loop(size_t worker_idx) {
    g_worker_lane = int(lane_);
    // Disambiguate indices across lanes so frame-critical and background workers never
    // alias the same per-worker slot (0 = main/external, 1..16 = frame, 17+ = background).
    g_worker_index = (lane_ == JobLane::kFrameCritical) ? (worker_idx + 1u) : (worker_idx + 17u);
    configure_worker_thread(lane_);

    for (;;) {
      ParallelBatch* batch = nullptr;
      std::function<void()> detached;

      // Fast spin on frame-critical lane to avoid OS park/wake latency between
      // consecutive 120 Hz physics substeps or tight frame job chains.
      if (spin_iters_ > 0 && pending_work_.load(std::memory_order_acquire) == 0) {
        for (uint32_t s = 0; s < spin_iters_; ++s) {
          if (pending_work_.load(std::memory_order_acquire) != 0) break;
          cpu_pause();
        }
      }

      {
        std::unique_lock<std::mutex> lk(mu_);
        if (batches_.empty() && detached_.empty()) {
          cv_work_.wait(lk, [this] { return stop_ || !batches_.empty() || !detached_.empty(); });
        }
        if (stop_ && batches_.empty() && detached_.empty()) return;

        // Prefer active parallel batches (LIFO so nested inner batches finish quickly).
        while (!batches_.empty()) {
          ParallelBatch* cand = batches_.back();
          if (cand->next.load(std::memory_order_relaxed) >= cand->count) {
            batches_.pop_back();
            pending_work_.fetch_sub(1, std::memory_order_release);
            continue;
          }
          cand->active_workers.fetch_add(1, std::memory_order_acq_rel);
          batch = cand;
          break;
        }
        if (!batch && !detached_.empty()) {
          detached = std::move(detached_.front());
          detached_.pop_front();
          pending_work_.fetch_sub(1, std::memory_order_release);
        }
      }

      if (batch) {
        struct ActiveGuard {
          LanePool& pool;
          ParallelBatch* b;
          ~ActiveGuard() {
            if (b->active_workers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
              std::lock_guard<std::mutex> dlk(pool.done_mu_);
              pool.done_cv_.notify_all();
            }
          }
        } active_guard{*this, batch};
        try {
          batch->steal_all();
        } catch (...) {
          // Swallow worker exception after decrementing active_workers so caller never deadlocks.
        }
      } else if (detached) {
        detached();
      }
    }
  }

  JobLane lane_;
  uint32_t spin_iters_ = 0;
  std::mutex mu_;
  std::condition_variable cv_work_;
  std::mutex done_mu_;
  std::condition_variable done_cv_;
  std::atomic<size_t> pending_work_{0};
  std::vector<ParallelBatch*> batches_;
  std::deque<std::function<void()>> detached_;
  bool stop_ = false;
  std::vector<std::thread> workers_;
};

class JobSystem {
 public:
  static JobSystem& instance() {
    static JobSystem sys;
    return sys;
  }

  LanePool& lane(JobLane l) noexcept { return (l == JobLane::kFrameCritical) ? frame_lane_ : bg_lane_; }
  size_t worker_count(JobLane l) const noexcept {
    return (l == JobLane::kFrameCritical) ? frame_lane_.worker_count() : bg_lane_.worker_count();
  }

 private:
  static size_t frame_worker_count() {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return (hw <= 1u) ? 0u : std::clamp<size_t>(size_t(hw) - 1u, 1u, 15u);
  }
  static size_t bg_worker_count() {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return (hw <= 2u) ? 1u : std::clamp<size_t>(size_t(hw) - 2u, 2u, 12u);
  }

  JobSystem()
      : frame_lane_(JobLane::kFrameCritical, frame_worker_count(), 1500u),
        bg_lane_(JobLane::kBackground, bg_worker_count(), 0u) {}

  LanePool frame_lane_;
  LanePool bg_lane_;
};

// Backwards-compatible alias for any callers referencing detail::WorkerPool.
class WorkerPool {
 public:
  static WorkerPool& instance() {
    static WorkerPool shim;
    return shim;
  }
  size_t worker_count() const { return JobSystem::instance().worker_count(JobLane::kBackground); }
  void run(size_t count, const std::function<void(size_t)>& fn) {
    auto trampoline = [](void* p, size_t idx) { (*static_cast<const std::function<void(size_t)>*>(p))(idx); };
    JobSystem::instance().lane(JobLane::kBackground)
        .run_batch(count, 1, trampoline, const_cast<std::function<void(size_t)>*>(&fn));
  }
};

}  // namespace detail

// Returns 0 on the main/external thread, or 1..N on a job worker thread.
inline size_t current_worker_index() noexcept { return detail::g_worker_index; }

inline size_t job_worker_count(JobLane lane = JobLane::kBackground) noexcept {
  return detail::JobSystem::instance().worker_count(lane);
}

// General parallel_for with explicit lane, chunk size, and serial threshold.
// Safe for concurrent multi-caller and nested invocation with zero heap allocation.
template <typename Fn>
inline void parallel_for_ex(size_t count, Fn&& fn, JobLane lane = JobLane::kBackground, size_t chunk = 1,
                            size_t min_parallel = 2) {
  if (count == 0) return;
  if (count < min_parallel) {
    for (size_t i = 0; i < count; ++i) fn(i);
    return;
  }
  auto trampoline = [](void* p, size_t idx) { (*static_cast<std::remove_reference_t<Fn>*>(p))(idx); };
  detail::JobSystem::instance().lane(lane).run_batch(count, chunk, trampoline, &fn);
}

// Background-lane parallel_for (streaming, decompression, asset/mesh/texture loading).
template <typename Fn>
inline void parallel_for(size_t count, Fn&& fn) {
  parallel_for_ex(count, std::forward<Fn>(fn), JobLane::kBackground, 1, 4);
}

// Low-latency frame-critical parallel_for (120 Hz physics, AI, traffic, interactive map).
template <typename Fn>
inline void parallel_for_frame(size_t count, Fn&& fn, size_t chunk = 1) {
  parallel_for_ex(count, std::forward<Fn>(fn), JobLane::kFrameCritical, chunk, 2);
}

// Fire-and-forget job submission onto the chosen lane.
inline void submit_job(std::function<void()> task, JobLane lane = JobLane::kBackground) {
  detail::JobSystem::instance().lane(lane).enqueue_detached(std::move(task));
}

}  // namespace fh1
