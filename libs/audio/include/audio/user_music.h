// Pinyon Shift "Your Music" folder player (ours): scans <state>/music
// recursively for MP3 and WAV files, streams them off-thread via StreamPool
// (prefetching the next track), supports ordered or shuffled playback and
// F9 / Shift+F9 next/previous skipping, and mutes the game's radio while
// active.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace audio {

class StreamPool;

class UserMusicPlayer {
 public:
  UserMusicPlayer(const std::filesystem::path& folder, int out_rate, uint64_t seed = 1, bool non_blocking = true);
  ~UserMusicPlayer();

  // Rescans `folder` (call on the main thread before handing to the audio
  // thread, or while stopped).
  int scan(const std::filesystem::path& folder);
  int track_count() const { return int(tracks_.size()); }
  bool has_tracks() const { return !tracks_.empty(); }

  void set_enabled(bool on);
  bool enabled() const { return enabled_; }
  bool active() const { return enabled_ && !tracks_.empty(); }

  void set_shuffle(bool on);
  bool shuffle() const { return shuffle_; }

  void set_volume(float v) { volume_ = v < 0.0f ? 0.0f : (v > 2.0f ? 2.0f : v); }
  float volume() const { return volume_; }

  void set_non_blocking(bool on) { non_blocking_ = on; }
  bool non_blocking() const { return non_blocking_; }

  void next();
  void prev();

  int current_index() const;  // into tracks_, or -1
  std::string current_title() const;
  std::string_view current_title_view() const;  // allocation-free for audio thread

  // Adds `frames` stereo frames into `out` when active().
  void render(float* out, int frames);

 private:
  void rebuild_order(int keep_track);
  void start_current();
  void ensure_prefetch();
  void stop_streams();
  int next_order_pos(int pos) const;

  std::filesystem::path folder_;
  int rate_ = 48000;
  bool non_blocking_ = true;
  bool enabled_ = false;
  bool shuffle_ = false;
  float volume_ = 1.0f;
  std::mt19937_64 rng_;

  std::vector<std::filesystem::path> tracks_;
  std::vector<std::string> titles_;
  std::vector<int> order_;
  int order_pos_ = 0;

  std::unique_ptr<StreamPool> pool_;
  int cur_slot_ = -1;
  int next_slot_ = -1;
  int next_track_ = -1;
  double pos_ = 0.0;
  float fade_ = 0.0f;
};

}  // namespace audio
