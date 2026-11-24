// Lightweight playtest session & bug recorder (ours): zero-hitch buffered
// telemetry.jsonl writer (4 Hz periodic + immediate logging on >25 ms hitches,
// ground/void anomalies, non-finite values, audio underruns, and user marks),
// instant F11 / Back+RB bug bookmark capture (bug_00N.png + bug_00N.json with
// copy-pasteable repro_cmd), and automatic exit summary.json.
// Plain C++ + stb_image_write (no Vulkan or SDL) for fast standalone testing.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace carview {

struct PlaytestFrameSample {
  double t = 0.0;              // elapsed session time (seconds)
  double frame_ms = 0.0;       // wall-clock frame interval (ms)
  double cpu_ms = 0.0;         // renderer CPU record/submit time (ms)
  double gpu_ms = -1.0;        // renderer GPU timestamp time (ms, -1 if unavailable)
  double x = 0.0;              // car world X (m)
  double y = 0.0;              // car world Y (m)
  double z = 0.0;              // car world Z (m)
  double road_y = 0.0;         // road/ground surface Y beneath car (m)
  bool has_road_y = true;      // false if no ground/road surface found below car
  double heading = 0.0;        // car heading (radians, 0 = +Z, + = +X)
  double speed_kmh = 0.0;      // vehicle speed (km/h)
  int gear = 0;                // -1 reverse, 0 neutral, 1..N
  double rpm = 0.0;            // engine RPM
  std::string car;             // car media name
  double tod_hour = 12.0;      // time of day in hours [0, 24)
  uint32_t chunks_loaded = 0;  // loaded world chunks / models
  bool paused = false;         // simulation or menu paused
  bool in_race = false;        // currently inside a race event
  uint64_t audio_stalls = 0;   // monotonic audio engine stall/underrun counter
  bool audio_underrun = false; // explicit per-frame audio underrun/glitch flag
  std::string camera_mode;     // active camera mode label (optional)
};

struct PlaytestHitchEntry {
  double t = 0.0;
  double frame_ms = 0.0;
  double cpu_ms = 0.0;
  double gpu_ms = -1.0;
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double speed_kmh = 0.0;
  std::string car;
  uint32_t chunks_loaded = 0;
  bool paused = false;
  bool in_race = false;
};

struct PlaytestBugEntry {
  uint32_t index = 0;
  double t = 0.0;
  std::string png_file;   // e.g. "bug_001.png"
  std::string json_file;  // e.g. "bug_001.json"
  bool screenshot_saved = false;
  std::string repro_cmd;
  PlaytestFrameSample snapshot;
};

struct PlaytestSummary {
  uint64_t total_frames = 0;
  double duration_s = 0.0;
  double p50_ms = 0.0;
  double p95_ms = 0.0;
  double p99_ms = 0.0;
  double max_ms = 0.0;
  uint64_t hitch_count = 0;
  std::vector<PlaytestHitchEntry> top_hitches;  // up to 20 worst hitches (> 25 ms), descending
  uint64_t ground_anomaly_count = 0;
  uint64_t non_finite_count = 0;
  uint64_t audio_glitch_count = 0;
  uint64_t telemetry_lines_written = 0;
  std::vector<PlaytestBugEntry> bugs;
};

class PlaytestRecorder {
 public:
  static constexpr double kPeriodicIntervalSec = 0.25;     // 4 Hz baseline sampling
  static constexpr double kHitchThresholdMs = 25.0;        // every frame > 25 ms logged
  static constexpr double kGroundBelowThresholdM = -0.5;   // y - road_y < -0.5 m
  static constexpr double kGroundAboveThresholdM = 4.0;    // y - road_y > 4.0 m
  static constexpr double kBugDebounceSec = 0.5;           // 0.5 s debounce between bug bookmarks
  static constexpr size_t kTopHitchesMax = 20;             // top 20 worst hitches in summary.json
  static constexpr size_t kFlushBytesThreshold = 16384;    // 16 KiB batch handoff to writer thread

  PlaytestRecorder() = default;
  PlaytestRecorder(const PlaytestRecorder&) = delete;
  PlaytestRecorder& operator=(const PlaytestRecorder&) = delete;
  ~PlaytestRecorder();

  // Opens `<dir>/telemetry.jsonl` and starts the background writer thread.
  bool start(const std::filesystem::path& dir, std::string* error = nullptr);
  bool active() const { return active_; }
  const std::filesystem::path& session_dir() const { return dir_; }

  // Monotonic elapsed seconds since start().
  double elapsed_seconds() const;

  // Per-frame telemetry ingestion (hot path: zero allocations on normal frames).
  void record_frame(const PlaytestFrameSample& sample);

  // Debounce check (0.5 s) before requesting a swapchain readback on F11 / Back+RB.
  bool can_trigger_bug_mark(double now_s) const;

  // Saves `<dir>/bug_00N.json` immediately, queues `<dir>/bug_00N.png` on the
  // background writer thread (when rgba is non-empty), logs a `user_mark` row
  // to telemetry.jsonl, and returns the HUD banner string
  // (e.g. "BUG #1 SAVED -> bug_001.png"), or empty string if debounced/inactive.
  std::string save_bug_bookmark(const PlaytestFrameSample& sample, std::vector<uint8_t> rgba, uint32_t width,
                                uint32_t height);

  // Flushes buffered telemetry to disk without closing the session.
  void flush();

  // Drains all queued telemetry and PNG captures, closes telemetry.jsonl, and
  // writes `<dir>/summary.json`. Idempotent; also called by ~PlaytestRecorder().
  bool finish();

  // Computes summary metrics from recorded state (also populated by finish()).
  PlaytestSummary compute_summary() const;
  const PlaytestSummary& last_summary() const { return last_summary_; }
  uint32_t bug_count() const { return bug_counter_; }

  // Pure static helpers (also exercised by unit tests).
  static bool is_hitch(double frame_ms);
  static bool has_non_finite(const PlaytestFrameSample& s);
  static bool is_ground_anomaly(const PlaytestFrameSample& s);
  static double compute_percentile(const std::vector<double>& sorted_vals, double q);
  static std::string format_repro_cmd(const PlaytestFrameSample& s);
  static std::string format_telemetry_line(const PlaytestFrameSample& s, const char* reason, uint64_t audio_stall_delta,
                                           bool non_finite);
  static std::string format_bug_json(const PlaytestBugEntry& entry, const std::string& utc_iso);
  static std::string format_summary_json(const PlaytestSummary& summary, const std::filesystem::path& dir,
                                         const std::string& started_utc, const std::string& finished_utc);
  static std::string json_escape(const std::string& s);
  static std::string utc_timestamp_iso8601();

 private:
  struct PendingPngJob {
    std::filesystem::path path;
    std::vector<uint8_t> rgba;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  void append_line(const std::string& line);
  void enqueue_active_buf(bool allow_blocking);
  void writer_main();
  void record_hitch(const PlaytestFrameSample& s);

  bool active_ = false;
  std::filesystem::path dir_;
  std::string started_utc_;
  std::chrono::steady_clock::time_point start_tp_{};

  std::FILE* telemetry_file_ = nullptr;
  std::string active_buf_;

  std::mutex io_mu_;
  std::condition_variable io_cv_;
  std::condition_variable flush_cv_;
  std::deque<std::string> write_queue_;
  std::deque<PendingPngJob> png_queue_;
  bool stop_writer_ = false;
  bool writer_busy_ = false;
  std::thread writer_thread_;

  uint64_t total_frames_ = 0;
  double first_sample_t_ = 0.0;
  double last_sample_t_ = 0.0;
  bool have_sample_t_ = false;
  double last_periodic_t_ = -1e9;
  bool have_audio_stalls_ = false;
  uint64_t last_audio_stalls_ = 0;

  // Continuous road-surface tracker used to distinguish overhead canopy/bridge/wall
  // raycast hits (road_y jumping sharply upward into the +2.2 m probe headroom while
  // the car remains at road level) from genuine car sink / fall-through anomalies.
  bool have_valid_road_y_ = false;
  double last_valid_road_y_ = 0.0;
  double last_valid_x_ = 0.0;
  double last_valid_z_ = 0.0;
  double last_valid_t_ = 0.0;

  std::vector<double> frame_ms_samples_;
  uint64_t hitch_count_ = 0;
  std::vector<PlaytestHitchEntry> top_hitches_;
  uint64_t ground_anomaly_count_ = 0;
  uint64_t non_finite_count_ = 0;
  uint64_t audio_glitch_count_ = 0;
  uint64_t telemetry_lines_written_ = 0;

  uint32_t bug_counter_ = 0;
  double last_bug_mark_t_ = -1e9;
  std::vector<PlaytestBugEntry> bugs_;
  PlaytestSummary last_summary_{};
};

}  // namespace carview
