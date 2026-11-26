// Character, festival, outpost, and barn-find Voice-Over player (Slice 9):
// Parses media/audio/vo/DialogueScript_<LANG>.xml (EN/BR/FR/MX, falling back
// to EN) and streams clips from VO_<LANG>_Stream.fsb onto Bus::kVo via a
// background StreamPool, driving AudioMixerSnapshots VOPlaying ducking and
// supporting Pinyon Shift's "Skip Voice-Overs with A" setting.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "audio/fsb4.h"
#include "audio/sfx_engine.h"
#include "audio/stream.h"

namespace audio {

struct VoScriptEntry {
  std::string filename_id;
  uint32_t soundbank_index = 0;
  uint8_t bank_id = 0;  // 0 = base VO_<LANG>_Stream.fsb, 1 = Rally_VO bank
};

struct VoScript {
  std::string language = "EN";
  // Lowercase trigger id -> one or more candidate clips in VO_<LANG>_Stream.fsb.
  std::unordered_map<std::string, std::vector<VoScriptEntry>> triggers;

  const std::vector<VoScriptEntry>* find(std::string_view trigger_id) const;
  size_t trigger_count() const { return triggers.size(); }
};

bool parse_dialogue_script(std::string_view xml, VoScript& out, std::string* error = nullptr);

class VoPlayer {
 public:
  explicit VoPlayer(int out_rate = 48000, bool non_blocking = true);
  ~VoPlayer();
  VoPlayer(const VoPlayer&) = delete;
  VoPlayer& operator=(const VoPlayer&) = delete;

  // Loads DialogueScript_<LANG>.xml and VO_<LANG>_Stream.fsb headers from
  // `game_root` (honouring cars::mods::resolve and EN fallback).
  bool load(const std::filesystem::path& game_root, std::string_view language = "EN", std::string* note = nullptr);

  // Discovers and layers Horizon Rally VO (`Rally_VO_<LANG>_Stream.fsb` /
  // `Rally_DialogueScript_<LANG>.xml` or `Rally_VO[_<LANG>].fev/.fsb`) from a
  // Rally package dir, direct VO dir, or launcher `state_root`.
  bool load_rally_vo(const std::filesystem::path& rally_root_or_state_root, std::string_view language = "EN",
                     std::string* note = nullptr);
  bool has_rally_vo() const { return has_rally_vo_; }

  // Registers an in-memory PCM clip for a trigger ID (unit tests / fallback).
  void set_clip(std::string_view trigger_id, PcmSample sample);

  const VoScript& script() const { return script_; }
  const std::string& language() const { return script_.language; }
  bool has_trigger(std::string_view trigger_id) const;

  // Queues (or immediately replaces if `interrupt`) a VO trigger.
  // `variant` >= 0 plays that Filename row of the trigger (a behaviour that
  // forces one); else the rows take turns.
  bool trigger(std::string_view trigger_id, bool interrupt = false, int variant = -1);
  // Pinyon Shift "Skip Voice-Overs with A": skips the active line immediately.
  void skip_current();
  void clear();

  // Renders active VO audio into interleaved stereo `out` (additive) on Bus::kVo.
  // Returns true while any VO line is actively playing (drives VOPlaying snapshot).
  bool render(float* out, int frames);

  bool playing() const { return active_.load(std::memory_order_acquire); }
  const std::string& current_trigger() const { return active_trigger_; }
  // Lock-free / atomic-safe live VO telemetry accessors for AudioEngine & UI subtitle sync.
  float elapsed_s() const { return elapsed_s_.load(std::memory_order_acquire); }
  float duration_s() const { return duration_s_.load(std::memory_order_acquire); }
  float current_rms() const { return current_rms_.load(std::memory_order_acquire); }
  uint64_t active_trigger_hash() const { return active_trigger_hash_.load(std::memory_order_acquire); }
  std::string active_trigger() const;
  size_t queued_count() const { return queue_.size(); }

 private:
  struct QueuedLine {
    std::string trigger_id;
    int64_t sample_index = -1;  // >= 0: FSB stream; -1: in-memory PcmSample
    uint8_t bank_id = 0;        // 0 = base VO, 1 = Rally_VO
  };

  void ensure_pool();
  void start_next();
  void stop_active();
  void begin_fade_out();
  void clear_fade_out();
  bool render_fade_out(float* out, int frames, double& sum_sq);
  void publish_trigger_snapshot(std::string_view trig);
  void update_rms(double sum_sq, int frames);

  int rate_ = 48000;
  bool non_blocking_ = true;
  VoScript script_;
  std::filesystem::path fsb_path_;
  std::vector<FsbSample> fsb_samples_;
  std::filesystem::path rally_fsb_path_;
  std::vector<FsbSample> rally_fsb_samples_;
  bool has_rally_vo_ = false;
  std::unique_ptr<StreamPool> pool_;
  std::unordered_map<std::string, PcmSample> mem_clips_;

  std::deque<QueuedLine> queue_;
  std::atomic<bool> active_{false};
  std::string active_trigger_;
  std::atomic<float> elapsed_s_{0.0f};
  std::atomic<float> duration_s_{0.0f};
  std::atomic<float> current_rms_{0.0f};
  std::atomic<uint64_t> active_trigger_hash_{0};
  mutable std::atomic<uint32_t> trigger_seq_{0};
  std::array<char, 64> trigger_buf_{};
  float rms_smooth_ = 0.0f;

  int active_slot_ = -1;
  const PcmSample* active_mem_ = nullptr;
  double pos_ = 0.0;
  int stalled_frames_ = 0;
  int seek_retries_ = 0;
  uint32_t variant_rr_ = 0;
  float last_l_ = 0.0f;
  float last_r_ = 0.0f;

  // 10 ms smooth fade-out tail for click-free skip_current() / interrupt.
  int fade_rem_ = 0;
  int fade_total_ = 0;
  int fade_slot_ = -1;
  const PcmSample* fade_mem_ = nullptr;
  double fade_pos_ = 0.0;
  float fade_last_l_ = 0.0f;
  float fade_last_r_ = 0.0f;
};

}  // namespace audio
