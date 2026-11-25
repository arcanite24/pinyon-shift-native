// Anna Sat-Nav GPS voice-over player: resolves SatNav_<lang>.fev/.fsb clips
// from media/audio/vo, decodes XMA2 subsounds asynchronously on a background
// worker with on-disk PCM cache, and renders lock-freely onto Bus::kVo so
// AudioEngine can drive the SatNavPlaying / SatNavNotPlaying mixer snapshot.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audio/engine_player.h"
#include "audio/rt.h"

namespace audio {

enum class SatNavCue : uint8_t {
  kDestGeneric = 0,
  kDestFestival,
  kDestFestivalRace,
  kDestShowcase,
  kDestOutpost,
  kDestWorkshop,
  kDestAutoshow,
  kDestPaintShop,
  kDestRaceCentral,
  kDestStreetRace,
  kDestReset,
  kDestCannotBeSet,
  kRecalculating,
  kUTurn,
  kTurnLeft,
  kTurnRight,
  kBearLeft,
  kBearRight,
  kSharpLeft,
  kSharpRight,
  kNearLeft,
  kNearRight,
  kNearBearLeft,
  kNearBearRight,
  kNearSharpLeft,
  kNearSharpRight,
  kFarLeft,
  kFarRight,
  kFarBearLeft,
  kFarBearRight,
  kFarSharpLeft,
  kFarSharpRight,
  kExtendedLeft,
  kExtendedRight,
  kExtendedBearLeft,
  kExtendedBearRight,
  kExtendedSharpLeft,
  kExtendedSharpRight,
  kNextJuncLeft,
  kNextJuncRight,
  kNextJuncBearLeft,
  kNextJuncBearRight,
  kNextJuncSharpLeft,
  kNextJuncSharpRight,
  kNearDestination,
  kFarDestination,
  kExtendedDestination,
  kArrived,
  kRoundaboutExit1,
  kRoundaboutExit2,
  kRoundaboutExit3,
  kRoundaboutExit4,
  kKeepLeft,
  kKeepRight,
  kCount
};

// Returns the FSB fallback substring needle for `cue` (guaranteed distinct per cue).
const char* satnav_cue_fsb_fallback(SatNavCue cue);

class SatNavVoPlayer {
 public:
  explicit SatNavVoPlayer(int out_rate = 48000);
  ~SatNavVoPlayer();

  // Loads SatNav_<lang>.fev / SatNav_<lang>.fsb (falling back to EN) under
  // `game_root/media/audio/vo` and begins background XMA2 -> PCM decoding
  // into `cache_dir/satnav`.
  bool load(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir,
            std::string_view language = "EN", bool metric = true, std::string* note = nullptr);

  // Injects a PCM clip directly (for unit tests without game audio files).
  void set_clip(SatNavCue cue, PcmSample sample);

  // Audio-thread controls (lock-free, no allocation).
  void set_enabled(bool on);
  bool enabled() const { return enabled_; }
  void trigger(SatNavCue cue);
  void stop();
  bool playing() const { return active_; }

  // Mixes active Anna VO into interleaved stereo `vo_bus` (`frames` frames).
  // Returns true while audio is actively playing.
  bool render(float* vo_bus, int frames);

  int loaded_clip_count() const;

 private:
  struct DecodeJob {
    SatNavCue cue = SatNavCue::kDestGeneric;
    std::filesystem::path bank_path;
    uint32_t subsound = 0;
  };
  struct LoadedClip {
    SatNavCue cue = SatNavCue::kDestGeneric;
    PcmSample* pcm = nullptr;
  };

  void start_worker(std::vector<DecodeJob> jobs, std::filesystem::path cache_dir);
  void stop_worker();
  void drain_loaded();
  bool start_next_pending();

  int rate_ = 48000;
  bool enabled_ = true;
  bool active_ = false;
  SatNavCue active_cue_ = SatNavCue::kDestGeneric;
  double phase_ = 0.0;
  float gain_ = 1.0f;
  float last_sample_ = 0.0f;

  // 8 ms smooth fade-out / crossfade state on high-priority SatNav interrupts.
  SatNavCue fade_cue_ = SatNavCue::kDestGeneric;
  double fade_phase_ = 0.0;
  int fade_rem_ = 0;
  int fade_total_ = 0;
  float fade_last_ = 0.0f;

  static constexpr size_t kMaxPending = 8;
  std::array<SatNavCue, kMaxPending> pending_{};
  size_t pending_head_ = 0;
  size_t pending_tail_ = 0;

  std::array<PcmSample*, int(SatNavCue::kCount)> clips_{};
  SpscQueue<LoadedClip> loaded_q_{128};
  std::atomic<bool> stop_worker_{false};
  std::thread worker_;
};

}  // namespace audio
