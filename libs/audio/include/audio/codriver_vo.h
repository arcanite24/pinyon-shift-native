// Horizon Rally Co-Driver pace-note voice-over player: resolves
// CoDriver_<LANG>.fev / CoDriver_<LANG>.fsb (EN, MX, and all 15 Rally VO
// languages with EN fallback) from the Rally package's Media/audio/VO folder,
// decodes XMA2 subsounds asynchronously on a background worker with on-disk
// PCM cache, and mixes sequentially onto Bus::kVo so AudioEngine drives
// VOPlaying mixer ducking cleanly.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "audio/engine_player.h"
#include "audio/rt.h"

namespace audio {

class CoDriverVoPlayer {
 public:
  explicit CoDriverVoPlayer(int out_rate = 48000);
  ~CoDriverVoPlayer();
  CoDriverVoPlayer(const CoDriverVoPlayer&) = delete;
  CoDriverVoPlayer& operator=(const CoDriverVoPlayer&) = delete;

  // Loads CoDriver_<LANG>.fev / CoDriver_<LANG>.fsb from a Rally package dir,
  // direct VO dir, or launcher `state_root` (scanning DLCZips/*_pri_*/Media/audio/VO,
  // falling back to EN) and begins asynchronous XMA2 -> PCM decoding into
  // `cache_dir/codriver_<LANG>`.
  bool load(const std::filesystem::path& rally_package_dir, const std::filesystem::path& cache_dir,
            std::string_view language = "EN", std::string* note = nullptr);

  // Registers an in-memory PCM clip for a cue key ("<group>/<cue>", lowercase)
  // for unit tests or synthetic verification.
  void set_clip(std::string_view cue_path, PcmSample sample);

  // Maps "<group>/<cue>" (case-insensitive) to a dense clip id (>= 0), or -1.
  int find_cue(std::string_view cue_path) const;
  int cue_count() const { return int(cue_paths_.size()); }
  int loaded_clip_count() const;
  const std::string& language() const { return language_; }

  // Audio-thread controls (lock-free, no heap allocation in steady state).
  void set_enabled(bool on);
  bool enabled() const { return enabled_; }
  void enqueue_cue(std::string_view cue_path);
  void enqueue_id(int clip_id);
  void clear();
  bool playing() const { return active_; }
  uint32_t completed_cue_count() const { return completed_cues_.load(std::memory_order_acquire); }

  // Mixes active Co-Driver call into interleaved stereo `vo_bus` (`frames`).
  // Returns true while any co-driver call is actively playing.
  bool render(float* vo_bus, int frames);

 private:
  struct DecodeJob {
    int clip_id = -1;
    std::filesystem::path bank_path;
    uint32_t subsound = 0;
  };
  struct LoadedClip {
    int clip_id = -1;
    PcmSample* pcm = nullptr;
  };

  void start_worker(std::vector<DecodeJob> jobs, std::filesystem::path cache_dir);
  void stop_worker();
  void drain_loaded();
  void start_next();

  int rate_ = 48000;
  std::string language_ = "EN";
  bool enabled_ = true;
  bool active_ = false;
  int active_id_ = -1;
  double phase_ = 0.0;
  float gain_ = 1.1f;

  std::vector<std::string> cue_paths_;
  std::unordered_map<std::string, int> path_to_id_;
  std::vector<std::vector<int>> cue_variants_;  // indexed by cue_id -> clip_ids in clips_
  std::vector<int> cue_last_variant_;           // last picked variant index per cue_id
  uint32_t rng_state_ = 0x51a7c0deu;
  std::vector<PcmSample*> clips_;  // indexed by clip_id
  std::atomic<uint32_t> completed_cues_{0};

  // Fixed-capacity queue of pending clip IDs (audio thread).
  static constexpr size_t kMaxPending = 64;
  int pending_[kMaxPending]{};
  size_t pending_head_ = 0;
  size_t pending_tail_ = 0;

  SpscQueue<LoadedClip> loaded_q_{512};
  std::atomic<bool> stop_worker_{false};
  std::thread worker_;
};

}  // namespace audio
