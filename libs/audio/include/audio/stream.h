// Streaming decode for music and voice clips (ours): sources decode to
// stereo float PCM on a worker thread into a lock-free ring each, and the
// audio thread only reads from the rings. File reads, MP3/WAV decode and
// seeking never run on the audio thread; a stream that is still loading or
// seeking outputs silence for that block without blocking.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "audio/rt.h"

namespace audio {

class PcmSource {
 public:
  virtual ~PcmSource() = default;
  virtual bool open() = 0;  // worker thread: reads files and initializes decoder
  virtual int rate() const = 0;
  virtual int64_t length() const = 0;  // frames, -1 if unknown
  // Positions so the next read() returns `frame` exactly.
  virtual bool seek(int64_t frame) = 0;
  // Decodes up to `frames` interleaved stereo float frames; 0 at EOF.
  virtual int64_t read(float* stereo, int64_t frames) = 0;
};

// An FSB4 MPEG sample read lazily from `bank` at `offset` (`bytes` long)
// when open() runs on the worker thread.
std::unique_ptr<PcmSource> make_fsb_mpeg_source(const std::filesystem::path& bank, uint64_t offset, uint32_t bytes,
                                                int fallback_rate = 48000);
// An MP3 or WAV file (by extension) streamed from disk.
std::unique_ptr<PcmSource> make_file_source(const std::filesystem::path& file);
bool is_music_file(const std::filesystem::path& file);  // .mp3 or .wav (case-insensitive)

class PcmStream {
 public:
  enum State : int { kEmpty, kLoading, kReady, kFailed };

  // Worker thread.
  void load(std::unique_ptr<PcmSource> src, size_t ring_frames);
  bool service();  // processes seeks and fills the ring; true if work was done
  void reset();

  // Audio thread (lock-free, never blocks).
  State state() const { return State(state_.load(std::memory_order_acquire)); }
  int rate() const { return rate_; }
  int64_t length() const { return length_; }
  // Requests a seek so that frame `first` becomes the ring's start;
  // positioned() becomes false immediately and true again once served.
  void request(int64_t first);
  bool positioned() const { return epoch_.load(std::memory_order_acquire) == requested_gen_; }
  // Audio thread tells the worker it will no longer read frames < `lowest`.
  void hint(int64_t lowest) { want_.store(lowest, std::memory_order_release); }
  // Valid frames currently in the ring: [from(), end()). Only read while
  // state() == kReady && positioned().
  int64_t from() const { return from_.load(std::memory_order_acquire); }
  int64_t end() const { return end_.load(std::memory_order_acquire); }
  bool source_done() const { return done_.load(std::memory_order_acquire); }
  // Copies `n` stereo frames starting at `first` (must lie in [from(), end())).
  void copy(int64_t first, int64_t n, float* dst) const;
  // Reads one stereo frame at `frame` (must lie in [from(), end())).
  void sample(int64_t frame, float& l, float& r) const;

 private:
  void seek_and_prefill(int64_t target);
  bool fill_chunk(int64_t max_frames);

  std::atomic<int> state_{kEmpty};
  int rate_ = 48000;
  int64_t length_ = -1;
  std::unique_ptr<PcmSource> src_;
  std::vector<float> ring_;
  size_t cap_ = 0;
  std::atomic<int64_t> from_{0}, end_{0}, want_{0};
  std::atomic<bool> done_{false};
  std::atomic<int64_t> seek_to_{0};
  std::atomic<uint32_t> seek_gen_{0}, epoch_{0};
  uint32_t served_gen_ = 0;     // worker thread
  uint32_t requested_gen_ = 0;  // audio thread
  friend class StreamPool;
};

class StreamPool {
 public:
  // Called on the worker thread to create the PcmSource for `key`.
  using Factory = std::function<std::unique_ptr<PcmSource>(uint64_t key)>;
  StreamPool(int slots, size_t ring_frames, Factory factory);
  ~StreamPool();

  // Audio thread (lock-free). Returns a slot id, or -1 if none free.
  int acquire(uint64_t key, int64_t initial_frame = 0);
  void release(int slot);
  PcmStream* stream(int slot) {
    return slot >= 0 && size_t(slot) < slots_.size() ? &slots_[size_t(slot)].stream : nullptr;
  }
  int slots() const { return int(slots_.size()); }
  int in_use() const;
  // Offline / test helper: sleeps until the slot is no longer loading and
  // any pending seek has been served.
  bool wait_ready(int slot, double timeout_s = 5.0);

 private:
  enum Slot : int { kFree, kActive, kReleasing };
  struct Entry {
    PcmStream stream;
    std::atomic<int> st{kFree};
    uint64_t key = 0;
  };
  struct Request {
    int slot = -1;
    bool release = false;
    uint64_t key = 0;
  };
  void run();

  Factory factory_;
  size_t ring_ = 0;
  std::vector<Entry> slots_;
  SpscQueue<Request> requests_{256};
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

}  // namespace audio
