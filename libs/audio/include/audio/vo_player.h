// Character, festival, outpost, and barn-find Voice-Over player (Slice 9):
// Parses media/audio/vo/DialogueScript_<LANG>.xml (EN/BR/FR/MX, falling back
// to EN) and streams clips from VO_<LANG>_Stream.fsb onto Bus::kVo via a
// background StreamPool, driving AudioMixerSnapshots VOPlaying ducking and
// supporting Pinyon Shift's "Skip Voice-Overs with A" setting.
#pragma once

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

  // Registers an in-memory PCM clip for a trigger ID (unit tests / fallback).
  void set_clip(std::string_view trigger_id, PcmSample sample);

  const VoScript& script() const { return script_; }
  const std::string& language() const { return script_.language; }
  bool has_trigger(std::string_view trigger_id) const;

  // Queues (or immediately replaces if `interrupt`) a VO trigger.
  bool trigger(std::string_view trigger_id, bool interrupt = false);
  // Pinyon Shift "Skip Voice-Overs with A": skips the active line immediately.
  void skip_current();
  void clear();

  // Renders active VO audio into interleaved stereo `out` (additive) on Bus::kVo.
  // Returns true while any VO line is actively playing (drives VOPlaying snapshot).
  bool render(float* out, int frames);

  bool playing() const { return active_; }
  const std::string& current_trigger() const { return active_trigger_; }
  size_t queued_count() const { return queue_.size(); }

 private:
  struct QueuedLine {
    std::string trigger_id;
    int64_t sample_index = -1;  // >= 0: FSB stream; -1: in-memory PcmSample
  };

  void start_next();
  void stop_active();

  int rate_ = 48000;
  bool non_blocking_ = true;
  VoScript script_;
  std::filesystem::path fsb_path_;
  std::vector<FsbSample> fsb_samples_;
  std::unique_ptr<StreamPool> pool_;
  std::unordered_map<std::string, PcmSample> mem_clips_;

  std::deque<QueuedLine> queue_;
  bool active_ = false;
  std::string active_trigger_;
  int active_slot_ = -1;
  const PcmSample* active_mem_ = nullptr;
  double pos_ = 0.0;
  uint32_t variant_rr_ = 0;
};

}  // namespace audio
