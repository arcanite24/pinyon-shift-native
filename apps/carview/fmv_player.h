// FFmpeg-backed WMV FMV player (Slice 9): plays Forza Horizon's disc movies
// (media/ui/videos/*.wmv & splash_intros/*.wmv) with companion .def language
// stream selection, timed subtitle XMLs, and audio-synced RGBA frames rendered
// into the UI atlas / Vulkan pass.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audio/rt.h"
#include "ui.h"

namespace carview {

enum class FmvMovieId {
  kT10MsCombined,
  kMsgsLogo,
  kDolbyCorona,
  kForzaIntro,
  kForzaTone,
  kPressStart,
  kFmv01Opening,
  kFmv02Ceremony,
  kFmv04Credits,
  kWristbandGet,
  kRallyBgLoop,
  kRallyForzaTone,
  kClubIntro,
};

struct FmvCatalogEntry {
  FmvMovieId id = FmvMovieId::kFmv01Opening;
  std::string name;
  std::filesystem::path path;
  bool is_boot_splash = false;
  bool exists = false;
};

struct FmvStreamDef {
  int video_id = -1;
  struct AudioTrack {
    int id = -1;
    std::string lang;
  };
  std::vector<AudioTrack> audio;
  // Returns the retail ASF stream `id` attribute matching `language` (with EN
  // fallback, or first valid id, or -1 if no .def audio entries carry an id).
  int audio_stream_id_for(std::string_view language) const;
  // Returns the retail ASF stream `id` matching `language` when present,
  // falling back to the 0-based XML element index only if `id < 0`.
  int audio_index_for(std::string_view language) const;
};

// Downmixes interleaved multichannel PCM (mono, stereo, quad, 5.1 WMA Pro,
// 7.1) into interleaved stereo float PCM using ITU/FMOD 0.7071 fold weights.
void downmix_interleaved_to_stereo(const float* src, int src_channels, size_t frames, float* dst_stereo);

struct FmvSubtitleCue {
  int index = 0;
  double start_s = 0.0;
  double end_s = -1.0;       // explicit End attribute (or start_s + Duration); < 0 means implicit window
  double duration_s = -1.0;  // explicit Duration attribute when present; < 0 means unspecified
};

// Word-wraps an FMV subtitle string cleanly into lines that fit within `max_width_px`.
std::vector<std::string> wrap_fmv_subtitle(std::string_view text, float max_width_px, const ui::TextStyle& ts);

// Gentle +/-40 ms deadband audio-clock PLL video clock step (zero jitter within +/-40 ms,
// smooth proportional drift correction outside +/-40 ms, never steps backwards).
double fmv_pll_step_clock(double video_clock_s, double dt_s, double audio_clock_s);

struct FmvProbeInfo {
  int width = 0;
  int height = 0;
  double fps = 24.0;
  double duration_s = 0.0;
  bool has_audio = false;
  int audio_streams = 0;
};

// Parses a companion `.def` XML file (`<StreamDefinition>`).
bool parse_fmv_def(std::string_view xml, FmvStreamDef& out);
// Parses a companion `fmv_XX_subtitles/<LANG>.xml` file (`<Subtitles>`).
bool parse_fmv_subtitles(std::string_view xml, std::vector<FmvSubtitleCue>& out);

class FmvPlayer {
 public:
  static constexpr int kAudioRate = 48000;
  static constexpr double kHoldToSkipSeconds = 0.75;

  FmvPlayer() = default;
  ~FmvPlayer();
  FmvPlayer(const FmvPlayer&) = delete;
  FmvPlayer& operator=(const FmvPlayer&) = delete;

  // Discovers the disc's movies under `game_root/media/ui/videos/` and optional DLC packages under `state_root`.
  static std::vector<FmvCatalogEntry> discover_movies(const std::filesystem::path& game_root,
                                                      const std::filesystem::path& state_root = {});
  // Resolves a specific movie ID to its file on disk (handling fallbacks such
  // as WristbandGet.wmv -> FMV_02.wmv on base discs and DLCZips movies under `state_root`).
  static std::filesystem::path resolve_movie(const std::filesystem::path& game_root, FmvMovieId id,
                                             const std::filesystem::path& state_root = {});
  // Sets a global `_&<id>` text resolver (e.g. `carview::game_text`) so FMV
  // subtitles pull authentic localized strings from `Subtitles.str`.
  using SubtitleResolver = std::function<std::string(const std::string&)>;
  static void set_subtitle_resolver(SubtitleResolver fn);
  // True when an in-process platform WMV decoder (Windows Media Foundation) is available.
  static bool has_native_wmv_decoder();
  // Probes video dimensions, frame rate, and duration (in-process MF on Windows for .wmv, else ffprobe).
  static bool probe(const std::filesystem::path& wmv_path, FmvProbeInfo& out);
  // Synchronously decodes one RGBA frame at `time_s` (in-process MF on Windows for .wmv, else FFmpeg).
  static bool decode_frame_at(const std::filesystem::path& wmv_path, double time_s, int target_w, int target_h,
                              std::vector<uint8_t>& out_rgba, std::string* error = nullptr);

  // Starts streaming video and audio from `wmv_path` (in-process MF on Windows for `.wmv`, or FFmpeg).
  bool open(const std::filesystem::path& wmv_path, std::string_view language = "EN", int target_w = 1280,
            int target_h = 720, std::string* error = nullptr, bool loop = false);
  // Opens a canonical movie ID from `game_root` / `state_root`, or falls back
  // gracefully to a stylized synthetic cutscene when absent in minimal fixtures.
  bool open_or_fallback(const std::filesystem::path& game_root, FmvMovieId id, std::string_view language = "EN",
                        int target_w = 1280, int target_h = 720, double fallback_duration_s = 6.0,
                        const std::filesystem::path& state_root = {});
  // Opens a deterministic synthetic video/audio stream (for tests without game
  // assets or when FFmpeg is absent).
  void open_synthetic(int w, int h, double fps, double duration_s, std::string title = "FMV");
  void close();

  bool active() const { return active_.load(std::memory_order_acquire); }
  bool finished() const { return finished_; }
  bool paused() const { return paused_.load(std::memory_order_acquire); }
  bool is_synthetic() const { return synthetic_; }
  void set_paused(bool p) { paused_.store(p, std::memory_order_release); }
  void skip();

  int width() const { return width_; }
  int height() const { return height_; }
  double fps() const { return fps_; }
  double duration() const { return duration_s_; }
  double position() const { return clock_s_; }
  const std::string& title() const { return title_; }
  int current_subtitle_index() const;
  std::string current_subtitle_text() const;
  void set_subtitles(std::vector<FmvSubtitleCue> cues);
  const std::vector<uint8_t>& current_frame() const { return current_frame_; }

  // Hold-to-skip progress accumulator (0..1 over kHoldToSkipSeconds = 0.75 s).
  float update_hold_skip(double dt, bool holding);
  float hold_skip_progress() const { return hold_skip_progress_; }
  void reset_hold_skip() { hold_skip_progress_ = 0.0f; }

  // Advances playback by `dt` seconds (syncing to drained audio clock when
  // `audio_synced` is true and audio is streaming). Returns true while active.
  bool update(double dt, bool audio_synced = false);

  // Drains decoded stereo 48 kHz PCM into `out` (interleaved, added/overwritten).
  // Thread-safe for calling from AudioEngine's render thread.
  int read_audio(float* out, int frames);

  // Draws the current video frame letterboxed over a black backdrop into `l`,
  // along with fade-in/out, optional subtitle text, and a skip / hold-to-skip prompt.
  void draw(ui::List& l, float screen_w, float screen_h, std::string_view subtitle_text = {},
            bool show_skip_prompt = true, float hold_skip_progress = 0.0f) const;

 private:
  struct VideoFrame {
    double pts = 0.0;
    std::vector<uint8_t> rgba;
  };

  void push_audio_stereo(const float* stereo, size_t n_floats);
  void video_worker(std::string cmd, int w, int h, double fps);
  void audio_worker(std::string cmd);
#ifdef _WIN32
  void mf_worker(std::filesystem::path wmv_path, int video_stream_id, int audio_stream_id, int target_w, int target_h,
                 bool loop);
#elif defined(SHIFT_HAVE_FFMPEG_FMV)
  void ff_worker(std::filesystem::path wmv_path, int video_stream_id, int audio_stream_id, int target_w, int target_h,
                 bool loop);
#endif
  void render_synthetic_frame(double t, std::vector<uint8_t>& rgba) const;

  std::atomic<bool> active_{false};
  bool finished_ = false;
  std::atomic<bool> paused_{false};
  bool synthetic_ = false;
  int width_ = 0;
  int height_ = 0;
  double fps_ = 24.0;
  double duration_s_ = 0.0;
  double clock_s_ = 0.0;
  float hold_skip_progress_ = 0.0f;
  FmvMovieId movie_id_ = FmvMovieId::kFmv01Opening;
  std::string title_;
  std::vector<FmvSubtitleCue> subtitles_;
  std::vector<uint8_t> current_frame_;
  mutable uint64_t frame_serial_ = 0;
  mutable uint64_t uploaded_serial_ = 0;

  std::atomic<bool> stop_{false};
  std::atomic<bool> video_eof_{false};
  std::atomic<bool> audio_eof_{false};
  std::atomic<int64_t> audio_frames_played_{0};

  mutable std::mutex video_mu_;
  std::deque<VideoFrame> video_q_;

  // Lock-free SPSC ring buffer (~2.73 s stereo at 48 kHz) for RT audio_thread read_audio().
  audio::SpscRingBuffer<float> audio_ring_{262144};

  std::thread video_thread_;
  std::thread audio_thread_;
};

}  // namespace carview
