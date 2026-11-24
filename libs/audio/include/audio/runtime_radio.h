// Runtime Custom Radio Station Manager (ours, LAB-87):
// Zero-repackaging custom radio stations configured via JSON
// (<user>/radio_stations.json and <play>/radio_stations.json), supporting
// unlimited user stations with mixed sources (local folder, single file,
// M3U/M3U8/PLS playlist, live HTTP/HTTPS stream, and YouTube video/playlist
// resolved asynchronously via yt-dlp + cached in <user>/radio_cache/), live
// mtime hot-reload, and lock-free 48 kHz stereo PCM playback via StreamPool.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace audio {

class PcmSource;
class StreamPool;

enum class RadioSourceType : uint8_t {
  kFolder = 0,
  kFile,
  kPlaylist,
  kStream,
  kYoutube,
};

const char* radio_source_type_name(RadioSourceType type);
bool parse_radio_source_type(std::string_view text, RadioSourceType& out);

struct RadioSourceSpec {
  RadioSourceType type = RadioSourceType::kFolder;
  std::string uri;     // directory path, file path, playlist path, stream URL, or YouTube URL
  std::string title;   // optional override for single file / stream / youtube
  std::string artist;  // optional override
};

struct RuntimeStationConfig {
  std::string id;
  std::string name;
  std::string genre;
  std::string accent_color = "#FF6A00";
  std::string logo_png;
  bool shuffle = true;
  bool enabled = true;
  float volume = 1.0f;
  std::vector<RadioSourceSpec> sources;
};

struct RuntimeRadioConfig {
  int version = 1;
  std::vector<RuntimeStationConfig> stations;
};

enum class RuntimeTrackKind : uint8_t {
  kLocalFile = 0,
  kStream,
  kYoutube,
};

struct RuntimeTrack {
  RuntimeTrackKind kind = RuntimeTrackKind::kLocalFile;
  std::string uri;       // resolved filesystem path or URL
  std::string title;
  std::string artist;
  std::string cache_id;  // YouTube video id or deterministic hash
};

struct RuntimeStation {
  RuntimeStationConfig config;
  std::vector<RuntimeTrack> tracks;
  bool youtube_pending = false;
};

// JSON serialization / deserialization (zero external dependencies).
bool parse_runtime_radio_json(std::string_view json, RuntimeRadioConfig& out, std::string* error = nullptr);
std::string serialize_runtime_radio_json(const RuntimeRadioConfig& cfg);
bool load_runtime_radio_config(const std::filesystem::path& path, RuntimeRadioConfig& out, std::string* error = nullptr);
bool save_runtime_radio_config(const std::filesystem::path& path, const RuntimeRadioConfig& cfg,
                               std::string* error = nullptr);

// Supported local audio extensions: .mp3, .wav, .ogg, .flac, .m4a, .opus.
bool is_runtime_audio_file(const std::filesystem::path& path);

// Scans a folder recursively (sorted deterministically) for supported audio files.
std::vector<RuntimeTrack> scan_audio_folder(const std::filesystem::path& folder);

// Parses .m3u / .m3u8 / .pls playlist files (resolving relative paths against `base_dir`).
std::vector<RuntimeTrack> parse_playlist_text(std::string_view text, const std::filesystem::path& base_dir,
                                              bool is_pls = false);
std::vector<RuntimeTrack> parse_playlist_file(const std::filesystem::path& playlist_path);

// Parses `yt-dlp --flat-playlist --dump-single-json` output (single video or playlist).
bool parse_ytdlp_flat_playlist_json(std::string_view json, std::vector<RuntimeTrack>& out,
                                    std::string* error = nullptr);

// Locates `yt-dlp` on SHIFT_YTDLP, PATH, or common install locations.
std::filesystem::path find_ytdlp();

// Creates a PcmSource for any RuntimeTrack (native dr_mp3/dr_wav for .mp3/.wav,
// ffmpeg pipe decode with CREATE_NO_WINDOW for .ogg/.flac/.m4a/.opus/stream/youtube).
std::unique_ptr<PcmSource> make_runtime_track_source(const RuntimeTrack& track, const std::filesystem::path& cache_dir,
                                                     int out_rate = 48000);

// Manages config discovery (<play>/radio_stations.json + <user>/radio_stations.json),
// hot-reload mtime tracking, mixed-source resolution, and async YouTube playlist expansion.
class RuntimeRadioManager {
 public:
  RuntimeRadioManager();
  ~RuntimeRadioManager();

  RuntimeRadioManager(const RuntimeRadioManager&) = delete;
  RuntimeRadioManager& operator=(const RuntimeRadioManager&) = delete;

  void configure(const std::filesystem::path& user_config_path, const std::filesystem::path& play_config_path,
                 const std::filesystem::path& cache_dir);

  const std::filesystem::path& user_config_path() const { return user_config_path_; }
  const std::filesystem::path& play_config_path() const { return play_config_path_; }
  const std::filesystem::path& cache_dir() const { return cache_dir_; }

  // Forces a full reload from disk. Returns true if at least one config file was loaded.
  bool reload();

  // Checks config file mtimes/sizes and reloads if either changed. Also drains any
  // completed async YouTube playlist resolutions. Returns true if station/track state changed.
  bool poll_updates();

  // Returns a snapshot of all enabled stations (thread-safe).
  std::vector<RuntimeStation> enabled_stations() const;
  // Returns all configured stations (including disabled).
  std::vector<RuntimeStation> all_stations() const;
  uint64_t generation() const { return generation_.load(std::memory_order_acquire); }

  // Runtime CRUD helpers (mutates user config and saves to user_config_path_).
  bool upsert_station(const RuntimeStationConfig& station, std::string* error = nullptr);
  bool remove_station(std::string_view station_id, std::string* error = nullptr);

 private:
  struct FileStamp {
    bool exists = false;
    uint64_t size = 0;
    int64_t mtime_ns = 0;
    bool operator==(const FileStamp& o) const {
      return exists == o.exists && size == o.size && mtime_ns == o.mtime_ns;
    }
    bool operator!=(const FileStamp& o) const { return !(*this == o); }
  };

  struct YoutubeJob {
    std::string station_id;
    std::string url;
    std::string fallback_title;
    std::string fallback_artist;
    uint64_t generation = 0;
  };

  struct YoutubeResult {
    std::string station_id;
    uint64_t generation = 0;
    std::vector<RuntimeTrack> tracks;
  };

  static FileStamp stat_file(const std::filesystem::path& p);
  void start_yt_worker();
  void stop_yt_worker();
  void yt_worker_main();
  void queue_youtube_job(YoutubeJob job);

  std::filesystem::path user_config_path_;
  std::filesystem::path play_config_path_;
  std::filesystem::path cache_dir_;
  FileStamp user_stamp_{};
  FileStamp play_stamp_{};

  mutable std::mutex mu_;
  RuntimeRadioConfig user_cfg_;
  RuntimeRadioConfig play_cfg_;
  std::vector<RuntimeStation> stations_;
  std::atomic<uint64_t> generation_{0};

  std::mutex yt_mu_;
  std::vector<YoutubeJob> yt_queue_;
  std::vector<YoutubeResult> yt_results_;
  std::atomic<bool> yt_stop_{false};
  std::thread yt_thread_;
};

// Audio-thread playback engine for enabled custom radio stations.
// All render() calls are lock-free and never block when non_blocking == true.
class RuntimeRadioPlayer {
 public:
  struct Snapshot {
    std::vector<RuntimeStation> stations;
    std::filesystem::path cache_dir;
  };

  struct NowPlaying {
    bool active = false;
    int station_index = -1;
    std::string station_id;
    std::string station_name;
    std::string title;
    std::string artist;
    uint32_t track_seq = 0;
  };

  RuntimeRadioPlayer(int out_rate, uint64_t seed = 1, bool non_blocking = true);
  ~RuntimeRadioPlayer();

  // Replaces station list (call before audio thread starts, or via swap).
  void set_stations(std::vector<RuntimeStation> stations, const std::filesystem::path& cache_dir);

  int station_count() const { return int(stations_.size()); }
  int selected_station() const { return selected_; }
  bool active() const { return selected_ >= 0 && size_t(selected_) < stations_.size() && !current_tracks().empty(); }

  void select_station(int index);  // -1 to deactivate
  void set_volume(float v) { volume_ = v < 0.0f ? 0.0f : (v > 2.0f ? 2.0f : v); }
  float volume() const { return volume_; }
  void set_non_blocking(bool on) { non_blocking_ = on; }
  bool non_blocking() const { return non_blocking_; }

  void next_track();
  void prev_track();

  // Allocation-free views for AudioEngine::publish_telemetry (audio thread only).
  std::string_view current_title_view() const;
  std::string_view current_artist_view() const;
  std::string_view current_station_name_view() const;
  uint32_t track_seq() const { return track_seq_; }
  NowPlaying now_playing() const;

  // Adds `frames` stereo frames into `out` (interleaved) when active().
  void render(float* out, int frames);

 private:
  const std::vector<RuntimeTrack>& current_tracks() const;
  void rebuild_order(int station_idx);
  void start_current();
  void ensure_prefetch();
  void stop_streams();
  int current_track_index() const;
  int next_order_pos(int pos) const;

  int rate_ = 48000;
  bool non_blocking_ = true;
  float volume_ = 1.0f;
  std::mt19937_64 rng_;

  std::filesystem::path cache_dir_;
  std::vector<RuntimeStation> stations_;
  std::vector<std::vector<int>> orders_;
  std::vector<int> order_pos_;
  int selected_ = -1;
  uint32_t track_seq_ = 0;

  std::unique_ptr<StreamPool> pool_;
  int cur_slot_ = -1;
  int next_slot_ = -1;
  uint64_t next_key_ = ~0ull;
  double pos_ = 0.0;
  float fade_ = 0.0f;
};

}  // namespace audio
