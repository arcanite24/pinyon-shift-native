// Persistent bounded low-priority worker pool for parallel archive/mesh/texture
// loading so streaming gathers never spawn fresh OS threads or preempt the
// render and audio threads.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__ANDROID__)
#include <sys/resource.h>
#endif

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace fh1 {

namespace detail {

#if defined(_WIN32)
void lower_worker_thread_priority_win32();
#endif

inline void configure_worker_thread() {
#if defined(_WIN32)
  lower_worker_thread_priority_win32();
#elif defined(__ANDROID__)
  setpriority(PRIO_PROCESS, 0, 8);
#endif
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
  // Flush denormals to zero (FTZ + DAZ) on streaming workers.
  _mm_setcsr(_mm_getcsr() | 0x8040u);
#endif
}

inline thread_local bool g_in_worker_pool = false;

class WorkerPool {
 public:
  static WorkerPool& instance() {
    static WorkerPool pool;
    return pool;
  }

  size_t worker_count() const { return workers_.size(); }

  // Runs `fn(size_t i)` for i in [0, count). Safe against nested/re-entrant calls:
  // if invoked from a pool worker or when another batch is already running, falls
  // back to inline execution on the calling thread without deadlocking.
  void run(size_t count, const std::function<void(size_t)>& fn) {
    if (count == 0) return;
    if (count < 4 || workers_.empty() || g_in_worker_pool) {
      for (size_t i = 0; i < count; ++i) fn(i);
      return;
    }

    std::unique_lock<std::mutex> call_lk(call_mutex_, std::try_to_lock);
    if (!call_lk.owns_lock()) {
      for (size_t i = 0; i < count; ++i) fn(i);
      return;
    }

    const size_t active = std::min<size_t>(workers_.size(), count - 1);
    {
      std::lock_guard<std::mutex> lk(mu_);
      job_fn_ = &fn;
      job_count_ = count;
      job_next_.store(0, std::memory_order_relaxed);
      active_workers_.store(active, std::memory_order_relaxed);
      ++generation_;
    }
    cv_work_.notify_all();

    // Calling thread participates in stealing work so latency stays minimal.
    for (;;) {
      const size_t i = job_next_.fetch_add(1, std::memory_order_relaxed);
      if (i >= count) break;
      fn(i);
    }

    std::unique_lock<std::mutex> lk(mu_);
    cv_done_.wait(lk, [this] { return active_workers_.load(std::memory_order_acquire) == 0; });
    job_fn_ = nullptr;
    job_count_ = 0;
  }

 private:
  WorkerPool() {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    // Leave at least 2 logical cores free for render + audio on desktop, cap at 8
    // to keep L3 cache & archive I/O contention low.
    const size_t target = (hw <= 2u) ? 1u : std::clamp<size_t>(size_t(hw) - 2u, 2u, 8u);
    workers_.reserve(target);
    for (size_t t = 0; t < target; ++t) {
      workers_.emplace_back([this, t] { worker_loop(t); });
    }
  }

  ~WorkerPool() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
      ++generation_;
    }
    cv_work_.notify_all();
    for (auto& w : workers_) {
      if (w.joinable()) w.join();
    }
  }

  void worker_loop(size_t worker_idx) {
    g_in_worker_pool = true;
    configure_worker_thread();
    uint64_t seen_gen = 0;
    for (;;) {
      const std::function<void(size_t)>* fn = nullptr;
      size_t count = 0;
      bool participate = false;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_work_.wait(lk, [this, seen_gen] { return stop_ || generation_ != seen_gen; });
        if (stop_) return;
        seen_gen = generation_;
        count = job_count_;
        fn = job_fn_;
        participate = (fn != nullptr && worker_idx + 1 < count);
      }
      if (participate) {
        for (;;) {
          const size_t i = job_next_.fetch_add(1, std::memory_order_relaxed);
          if (i >= count) break;
          (*fn)(i);
        }
        if (active_workers_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          std::lock_guard<std::mutex> lk(mu_);
          cv_done_.notify_one();
        }
      }
    }
  }

  std::mutex call_mutex_;
  std::mutex mu_;
  std::condition_variable cv_work_;
  std::condition_variable cv_done_;
  bool stop_ = false;
  uint64_t generation_ = 0;
  const std::function<void(size_t)>* job_fn_ = nullptr;
  size_t job_count_ = 0;
  std::atomic<size_t> job_next_{0};
  std::atomic<size_t> active_workers_{0};
  std::vector<std::thread> workers_;
};

}  // namespace detail

template <typename Fn>
inline void parallel_for(size_t count, Fn&& fn) {
  if (count == 0) return;
  if (count < 4) {
    for (size_t i = 0; i < count; ++i) fn(i);
    return;
  }
  const std::function<void(size_t)> bound = [&](size_t i) { fn(i); };
  detail::WorkerPool::instance().run(count, bound);
}

}  // namespace fh1
