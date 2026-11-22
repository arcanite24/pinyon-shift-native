// Real-time-safe hand-off primitives between the main thread and the audio
// thread (ours; nothing here allocates, locks or waits after construction).
//   SpscQueue<T>   one producer thread, one consumer thread, fixed capacity:
//                  discrete commands, and objects handed over by pointer.
//   TripleBuffer<T> the producer's latest value for the consumer, wait-free
//                  on both sides: the engine's inputs each frame, and the
//                  audio thread's published state (radio popup, meters).
#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace audio {

template <class T>
class SpscQueue {
 public:
  // `capacity` is rounded up to a power of two (at least 2).
  explicit SpscQueue(size_t capacity = 256) {
    size_t n = 2;
    while (n < capacity) n <<= 1;
    slots_.resize(n);
    mask_ = n - 1;
  }
  // Producer: false (nothing queued) when full.
  bool push(const T& v) {
    const size_t w = write_.load(std::memory_order_relaxed);
    if (w - read_.load(std::memory_order_acquire) > mask_) return false;
    slots_[w & mask_] = v;
    write_.store(w + 1, std::memory_order_release);
    return true;
  }
  // Consumer: false when empty.
  bool pop(T& out) {
    const size_t r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire)) return false;
    out = slots_[r & mask_];
    read_.store(r + 1, std::memory_order_release);
    return true;
  }
  size_t size_approx() const { return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire); }
  size_t capacity() const { return mask_ + 1; }

 private:
  std::vector<T> slots_;
  size_t mask_ = 1;
  alignas(64) std::atomic<size_t> write_{0};
  alignas(64) std::atomic<size_t> read_{0};
};

// The classic three-slot exchange: the writer fills its private slot and
// swaps it with the middle one; the reader swaps the middle one for its own
// when the middle holds something new. T is copied whole; keep it small.
template <class T>
class TripleBuffer {
  static_assert(std::is_copy_assignable_v<T>);

 public:
  TripleBuffer() = default;
  explicit TripleBuffer(const T& initial) { buf_[0] = buf_[1] = buf_[2] = initial; }
  // Writer thread only.
  void write(const T& v) {
    buf_[back_] = v;
    back_ = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel) & 3;
  }
  // Reader thread only: the newest value. True when it is newer than the
  // last read.
  bool read(T& out) {
    bool fresh = false;
    if (middle_.load(std::memory_order_relaxed) & kDirty) {
      front_ = middle_.exchange(front_, std::memory_order_acq_rel) & 3;
      fresh = true;
    }
    out = buf_[front_];
    return fresh;
  }

 private:
  static constexpr int kDirty = 4;
  T buf_[3]{};
  int back_ = 0, front_ = 2;
  std::atomic<int> middle_{1};
};

}  // namespace audio
