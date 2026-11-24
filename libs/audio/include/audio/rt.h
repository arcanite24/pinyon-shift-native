// Real-time-safe hand-off primitives between the main thread and the audio
// thread (ours; nothing here allocates, locks or waits after construction).
//   SpscQueue<T>   one producer thread, one consumer thread, fixed capacity:
//                  discrete commands, and objects handed over by pointer.
//   TripleBuffer<T> the producer's latest value for the consumer, wait-free
//                  on both sides: the engine's inputs each frame, and the
//                  audio thread's published state (radio popup, meters).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>

namespace audio {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // intentional cache-line alignment padding
#endif

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

// Lock-free power-of-two SPSC bulk ring buffer for real-time PCM audio streaming
// (e.g. FMV decode worker -> TIME_CRITICAL audio render callback). Zero mutexes,
// zero heap allocations after construction, contiguous wrap-around memcpy.
template <class T>
class SpscRingBuffer {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  explicit SpscRingBuffer(size_t min_capacity = 65536) {
    size_t n = 2;
    while (n < min_capacity) n <<= 1;
    buf_.resize(n);
    mask_ = n - 1;
  }

  size_t capacity() const { return mask_ + 1; }

  size_t available_read() const {
    const size_t w = write_.load(std::memory_order_acquire);
    const size_t r = read_.load(std::memory_order_relaxed);
    return w - r;
  }

  size_t available_write() const {
    const size_t w = write_.load(std::memory_order_relaxed);
    const size_t r = read_.load(std::memory_order_acquire);
    return (mask_ + 1) - (w - r);
  }

  // Producer thread only: writes up to `count` elements; returns count written.
  size_t write(const T* src, size_t count) {
    if (!src || count == 0) return 0;
    const size_t w = write_.load(std::memory_order_relaxed);
    const size_t r = read_.load(std::memory_order_acquire);
    const size_t cap = mask_ + 1;
    const size_t avail = cap - (w - r);
    const size_t n = std::min(count, avail);
    if (n == 0) return 0;
    const size_t idx = w & mask_;
    const size_t first = std::min(n, cap - idx);
    std::memcpy(buf_.data() + idx, src, first * sizeof(T));
    if (n > first) {
      std::memcpy(buf_.data(), src + first, (n - first) * sizeof(T));
    }
    write_.store(w + n, std::memory_order_release);
    return n;
  }

  // Consumer thread only: reads up to `count` elements; returns count read.
  size_t read(T* dst, size_t count) {
    if (!dst || count == 0) return 0;
    const size_t r = read_.load(std::memory_order_relaxed);
    const size_t w = write_.load(std::memory_order_acquire);
    const size_t cap = mask_ + 1;
    const size_t avail = w - r;
    const size_t n = std::min(count, avail);
    if (n == 0) return 0;
    const size_t idx = r & mask_;
    const size_t first = std::min(n, cap - idx);
    std::memcpy(dst, buf_.data() + idx, first * sizeof(T));
    if (n > first) {
      std::memcpy(dst + first, buf_.data(), (n - first) * sizeof(T));
    }
    read_.store(r + n, std::memory_order_release);
    return n;
  }

  // Call only when producer & consumer are stopped/quiesced.
  void clear() {
    read_.store(0, std::memory_order_relaxed);
    write_.store(0, std::memory_order_release);
  }

 private:
  std::vector<T> buf_;
  size_t mask_ = 1;
  alignas(64) std::atomic<size_t> write_{0};
  alignas(64) std::atomic<size_t> read_{0};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

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
