// Unit tests for PlaytestRecorder: 4 Hz JSONL telemetry recording, >25 ms hitch
// detection and top-20 ranking, ground/void/non-finite/audio-glitch anomaly
// detection, summary.json percentile accuracy, 0.5 s bug bookmark debounce,
// PNG + bug_00N.json + repro_cmd formatting, and <0.1 ms/frame overhead.
// Plain C++ (no Vulkan or SDL).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "playtest_recorder.h"

using carview::PlaytestFrameSample;
using carview::PlaytestRecorder;
using carview::PlaytestSummary;

namespace {

int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

std::string read_all_text(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::vector<std::string> read_lines(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(std::move(line));
  }
  return lines;
}

std::filesystem::path make_temp_session_dir(const std::string& tag) {
  const auto base = std::filesystem::temp_directory_path() / ("shift_playtest_test_" + tag);
  std::error_code ec;
  std::filesystem::remove_all(base, ec);
  std::filesystem::create_directories(base, ec);
  return base;
}

PlaytestFrameSample make_base_sample(double t, double frame_ms = 16.6) {
  PlaytestFrameSample s;
  s.t = t;
  s.frame_ms = frame_ms;
  s.cpu_ms = 4.2;
  s.gpu_ms = 8.1;
  s.x = 1999.0;
  s.y = 145.40;
  s.z = 962.90;
  s.road_y = 145.38;
  s.has_road_y = true;
  s.heading = 2.1990;
  s.speed_kmh = 112.5;
  s.gear = 3;
  s.rpm = 5400.0;
  s.car = "NIS_SkylineGTRVSpecII_02";
  s.tod_hour = 14.50;
  s.chunks_loaded = 38;
  s.paused = false;
  s.in_race = false;
  s.audio_stalls = 0;
  s.camera_mode = "chase";
  return s;
}

void test_jsonl_periodic_and_fields() {
  const auto dir = make_temp_session_dir("periodic");
  PlaytestRecorder rec;
  std::string err;
  CHECK(rec.start(dir, &err));
  CHECK(rec.active());

  // Feed 60 frames over 1.0 s (every 1/60 s): should log at 4 Hz (t=0.0, ~0.25, ~0.50, ~0.75, ~1.0 -> 4 or 5 lines).
  for (int i = 0; i <= 60; ++i) {
    rec.record_frame(make_base_sample(double(i) / 60.0, 16.667));
  }
  CHECK(rec.finish());
  CHECK(!rec.active());

  const auto lines = read_lines(dir / "telemetry.jsonl");
  CHECK(lines.size() == 5);  // t = 0.00, 0.25, 0.50, 0.75, 1.00
  for (const auto& l : lines) {
    CHECK(l.find("\"t\":") != std::string::npos);
    CHECK(l.find("\"frame_ms\":") != std::string::npos);
    CHECK(l.find("\"cpu_ms\":") != std::string::npos);
    CHECK(l.find("\"gpu_ms\":") != std::string::npos);
    CHECK(l.find("\"x\":") != std::string::npos);
    CHECK(l.find("\"y\":") != std::string::npos);
    CHECK(l.find("\"z\":") != std::string::npos);
    CHECK(l.find("\"road_y\":") != std::string::npos);
    CHECK(l.find("\"heading\":") != std::string::npos);
    CHECK(l.find("\"speed_kmh\":") != std::string::npos);
    CHECK(l.find("\"gear\":3") != std::string::npos);
    CHECK(l.find("\"rpm\":") != std::string::npos);
    CHECK(l.find("\"car\":\"NIS_SkylineGTRVSpecII_02\"") != std::string::npos);
    CHECK(l.find("\"tod_hour\":") != std::string::npos);
    CHECK(l.find("\"chunks_loaded\":38") != std::string::npos);
    CHECK(l.find("\"paused\":false") != std::string::npos);
    CHECK(l.find("\"in_race\":false") != std::string::npos);
    CHECK(l.find("\"reason\":\"periodic\"") != std::string::npos);
  }

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

void test_hitches_and_top20() {
  const auto dir = make_temp_session_dir("hitches");
  PlaytestRecorder rec;
  CHECK(rec.start(dir));

  // First periodic frame at t=0.0
  rec.record_frame(make_base_sample(0.0, 16.0));

  // Inject 25 hitches (> 25 ms) packed tightly within 0.01..0.20 s (all inside a single periodic window!).
  // Every single one MUST be logged immediately to telemetry.jsonl, and summary must keep top 20 sorted desc.
  for (int i = 1; i <= 25; ++i) {
    auto s = make_base_sample(0.005 * i, 26.0 + double(i) * 2.5);
    s.x = 100.0 + double(i);
    s.z = 200.0 + double(i);
    rec.record_frame(s);
  }
  // One non-hitch boundary frame at exactly 25.0 ms (must NOT count as hitch).
  rec.record_frame(make_base_sample(0.18, 25.0));

  CHECK(rec.finish());

  const PlaytestSummary& sum = rec.last_summary();
  CHECK(sum.total_frames == 27);
  CHECK(sum.hitch_count == 25);
  CHECK(sum.top_hitches.size() == 20);
  CHECK(near(sum.top_hitches.front().frame_ms, 26.0 + 25.0 * 2.5, 1e-3));  // 88.5 ms
  CHECK(near(sum.top_hitches.front().x, 125.0, 1e-3));
  CHECK(near(sum.top_hitches.back().frame_ms, 26.0 + 6.0 * 2.5, 1e-3));    // 41.0 ms
  for (size_t i = 1; i < sum.top_hitches.size(); ++i) {
    CHECK(sum.top_hitches[i - 1].frame_ms >= sum.top_hitches[i].frame_ms);
  }

  const auto lines = read_lines(dir / "telemetry.jsonl");
  CHECK(lines.size() == 26);  // 1 periodic + 25 hitches
  size_t hitch_lines = 0;
  for (const auto& l : lines) {
    if (l.find("\"reason\":\"hitch\"") != std::string::npos) ++hitch_lines;
  }
  CHECK(hitch_lines == 25);

  const std::string summary_json = read_all_text(dir / "summary.json");
  CHECK(summary_json.find("\"hitch_count\": 25") != std::string::npos);
  CHECK(summary_json.find("\"top_hitches\": [") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

void test_ground_nonfinite_and_audio_anomalies() {
  const auto dir = make_temp_session_dir("anomalies");
  PlaytestRecorder rec;
  CHECK(rec.start(dir));

  // Frame 0: normal baseline (periodic).
  rec.record_frame(make_base_sample(0.00, 16.0));

  // Frame 1: normal clearance (y - road_y = -0.2 m, within [-0.5, 4.0]). Not logged.
  {
    auto s = make_base_sample(0.02, 16.0);
    s.y = 145.18;
    s.road_y = 145.38;
    rec.record_frame(s);
  }
  // Frame 2: sunken below road (y - road_y = -0.8 m < -0.5 m) -> ground_anomaly.
  {
    auto s = make_base_sample(0.04, 16.0);
    s.y = 144.58;
    s.road_y = 145.38;
    rec.record_frame(s);
  }
  // Frame 3: launched high above road (y - road_y = +5.2 m > 4.0 m) -> ground_anomaly.
  {
    auto s = make_base_sample(0.06, 16.0);
    s.y = 150.58;
    s.road_y = 145.38;
    rec.record_frame(s);
  }
  // Frame 4: void (no road hit while unpaused) -> ground_anomaly.
  {
    auto s = make_base_sample(0.08, 16.0);
    s.has_road_y = false;
    s.paused = false;
    rec.record_frame(s);
  }
  // Frame 5: non-finite X coordinate -> logged with null and non_finite flag.
  {
    auto s = make_base_sample(0.10, 16.0);
    s.x = std::numeric_limits<double>::quiet_NaN();
    rec.record_frame(s);
  }
  // Frame 6: audio stall increment -> logged as audio_glitch.
  {
    auto s = make_base_sample(0.12, 16.0);
    s.audio_stalls = 2;
    rec.record_frame(s);
  }

  CHECK(rec.finish());

  const PlaytestSummary& sum = rec.last_summary();
  CHECK(sum.total_frames == 7);
  CHECK(sum.ground_anomaly_count == 3);
  CHECK(sum.non_finite_count == 1);
  CHECK(sum.audio_glitch_count == 1);

  const auto lines = read_lines(dir / "telemetry.jsonl");
  CHECK(lines.size() == 6);  // Frame 0 + Frames 2, 3, 4, 5, 6
  CHECK(lines[1].find("\"reason\":\"ground_anomaly\"") != std::string::npos);
  CHECK(lines[2].find("\"reason\":\"ground_anomaly\"") != std::string::npos);
  CHECK(lines[3].find("\"road_y\":null") != std::string::npos);
  CHECK(lines[3].find("\"reason\":\"ground_anomaly\"") != std::string::npos);
  CHECK(lines[4].find("\"x\":null") != std::string::npos);
  CHECK(lines[4].find("\"non_finite\":true") != std::string::npos);
  CHECK(lines[5].find("\"audio_stalls\":2") != std::string::npos);
  CHECK(lines[5].find("\"reason\":\"audio_glitch\"") != std::string::npos);

  const std::string summary_json = read_all_text(dir / "summary.json");
  CHECK(summary_json.find("\"ground_anomaly_count\": 3") != std::string::npos);
  CHECK(summary_json.find("\"non_finite_count\": 1") != std::string::npos);
  CHECK(summary_json.find("\"audio_glitch_count\": 1") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

void test_summary_percentiles() {
  const auto dir = make_temp_session_dir("percentiles");
  PlaytestRecorder rec;
  CHECK(rec.start(dir));

  // Feed 100 frames with frame_ms = 1.0, 2.0, ..., 100.0 (unsorted order).
  for (int i = 100; i >= 1; --i) {
    rec.record_frame(make_base_sample(double(100 - i) * 0.01, double(i)));
  }
  CHECK(rec.finish());

  const PlaytestSummary& sum = rec.last_summary();
  CHECK(sum.total_frames == 100);
  CHECK(near(sum.duration_s, 0.99, 1e-3));
  CHECK(near(sum.p50_ms, 50.5, 1e-3));
  CHECK(near(sum.p95_ms, 95.05, 1e-3));
  CHECK(near(sum.p99_ms, 99.01, 1e-3));
  CHECK(near(sum.max_ms, 100.0, 1e-3));
  CHECK(sum.hitch_count == 75);  // 26..100 > 25.0

  const std::string summary_json = read_all_text(dir / "summary.json");
  CHECK(summary_json.find("\"total_frames\": 100") != std::string::npos);
  CHECK(summary_json.find("\"p50_ms\": 50.500") != std::string::npos);
  CHECK(summary_json.find("\"p95_ms\": 95.050") != std::string::npos);
  CHECK(summary_json.find("\"p99_ms\": 99.010") != std::string::npos);
  CHECK(summary_json.find("\"max_ms\": 100.000") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

void test_bug_bookmark_repro_cmd_and_debounce() {
  const auto dir = make_temp_session_dir("bugs");
  PlaytestRecorder rec;
  CHECK(rec.start(dir));

  auto s1 = make_base_sample(1.0, 16.5);
  s1.car = "VIP_Viper_13";
  s1.x = -1360.25;
  s1.y = 142.80;
  s1.z = -210.50;
  s1.heading = 1.5708;
  s1.tod_hour = 19.25;
  s1.in_race = true;

  // Verify standalone repro_cmd formatting.
  const std::string expected_repro =
      "carview.exe --drive --car VIP_Viper_13 --spawn -1360.25 142.80 -210.50 1.5708 --hour 19.25";
  CHECK(PlaytestRecorder::format_repro_cmd(s1) == expected_repro);

  // Tiny 4x4 RGBA test image for bug_001.png.
  std::vector<uint8_t> rgba(4u * 4u * 4u, 200u);
  CHECK(rec.can_trigger_bug_mark(1.0));
  const std::string banner1 = rec.save_bug_bookmark(s1, rgba, 4, 4);
  CHECK(banner1 == "BUG #1 SAVED -> bug_001.png");
  CHECK(rec.bug_count() == 1);

  // Within 0.5 s debounce window -> rejected.
  auto s_debounced = s1;
  s_debounced.t = 1.3;
  CHECK(!rec.can_trigger_bug_mark(1.3));
  CHECK(rec.save_bug_bookmark(s_debounced, rgba, 4, 4).empty());
  CHECK(rec.bug_count() == 1);

  // After 0.5 s debounce window -> accepted as bug_002 (no screenshot pixels -> fallback json banner).
  auto s2 = make_base_sample(1.6, 17.0);
  s2.car = "FER_F40_87";
  CHECK(rec.can_trigger_bug_mark(1.6));
  const std::string banner2 = rec.save_bug_bookmark(s2, {}, 0, 0);
  CHECK(banner2 == "BUG #2 SAVED -> bug_002.json");
  CHECK(rec.bug_count() == 2);

  CHECK(rec.finish());

  // Verify bug_001.png exists and starts with valid PNG signature.
  const auto png1 = dir / "bug_001.png";
  CHECK(std::filesystem::exists(png1));
  {
    std::ifstream pin(png1, std::ios::binary);
    unsigned char sig[8] = {};
    pin.read(reinterpret_cast<char*>(sig), 8);
    CHECK(sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G');
  }

  // Verify bug_001.json contains repro_cmd and state snapshot.
  const std::string json1 = read_all_text(dir / "bug_001.json");
  CHECK(json1.find("\"bug_index\": 1") != std::string::npos);
  CHECK(json1.find("\"screenshot\": \"bug_001.png\"") != std::string::npos);
  CHECK(json1.find("\"screenshot_saved\": true") != std::string::npos);
  CHECK(json1.find("\"repro_cmd\": \"" + expected_repro + "\"") != std::string::npos);
  CHECK(json1.find("\"in_race\": true") != std::string::npos);
  CHECK(json1.find("\"camera_mode\": \"chase\"") != std::string::npos);

  // Verify telemetry.jsonl logged both user_mark records.
  const auto lines = read_lines(dir / "telemetry.jsonl");
  size_t marks = 0;
  for (const auto& l : lines) {
    if (l.find("\"reason\":\"user_mark\"") != std::string::npos) ++marks;
  }
  CHECK(marks == 2);

  // Verify summary.json lists both bugs.
  const std::string summary_json = read_all_text(dir / "summary.json");
  CHECK(summary_json.find("\"png\": \"bug_001.png\"") != std::string::npos);
  CHECK(summary_json.find("\"json\": \"bug_001.json\"") != std::string::npos);
  CHECK(summary_json.find("\"json\": \"bug_002.json\"") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

void test_zero_hitch_overhead() {
  const auto dir = make_temp_session_dir("perf");
  PlaytestRecorder rec;
  CHECK(rec.start(dir));

  auto s = make_base_sample(0.0, 16.6);
  const auto t0 = std::chrono::steady_clock::now();
  constexpr int kFrames = 10000;
  for (int i = 0; i < kFrames; ++i) {
    s.t = double(i) / 60.0;
    rec.record_frame(s);
  }
  const double total_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const double per_frame_ms = total_ms / double(kFrames);
  std::printf("playtest_recorder_tests: %d frames recorded in %.3f ms (%.5f ms/frame)\n", kFrames, total_ms,
              per_frame_ms);
  CHECK(per_frame_ms < 0.1);
  CHECK(rec.finish());

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

void test_ground_anomaly_false_positive_filters() {
  const auto dir = make_temp_session_dir("ga_filters");
  PlaytestRecorder rec;
  CHECK(rec.start(dir));

  // 1. Paused pre-race-grid frame before physics road snap (PT-22 Cluster 1): NOT an anomaly.
  {
    auto s = make_base_sample(0.00, 16.0);
    s.x = -950.95;
    s.y = 1.60;
    s.z = -519.02;
    s.road_y = -4.135;
    s.paused = true;
    s.in_race = true;
    CHECK(!PlaytestRecorder::is_ground_anomaly(s));
    rec.record_frame(s);
  }
  // Paused frame with non-finite Y MUST still be flagged as an anomaly / non-finite.
  {
    auto s = make_base_sample(0.01, 16.0);
    s.y = std::numeric_limits<double>::quiet_NaN();
    s.paused = true;
    CHECK(PlaytestRecorder::is_ground_anomaly(s));
  }

  // 2. Overhead canopy / bridge / leaning-wall raycast artifact (PT-22 Cluster 2):
  //    car drives smoothly at y=137.0..137.5 on road_y=136.93 while +2.2 m downward probe
  //    temporarily strikes an overhead structure at road_y=139.35 (dy=-2.05) -> NOT an anomaly!
  {
    auto s0 = make_base_sample(1.00, 8.3);
    s0.x = -5830.55;
    s0.y = 136.98;
    s0.z = -2951.72;
    s0.road_y = 136.925;
    s0.in_race = true;
    rec.record_frame(s0);

    for (int i = 1; i <= 10; ++i) {
      auto ov = make_base_sample(1.00 + 0.02 * i, 8.3);
      ov.x = -5830.0;
      ov.y = 137.30;
      ov.z = -2951.72 - 1.5 * i;
      ov.road_y = 139.35;  // +2.42 m upward jump into +2.2 m probe headroom!
      ov.in_race = true;
      rec.record_frame(ov);
    }

    // Genuine sink below the continuous road (road_y stays 136.925, car drops to 135.8) MUST still trigger!
    auto sink = make_base_sample(1.30, 8.3);
    sink.x = -5830.0;
    sink.y = 135.80;
    sink.z = -2968.0;
    sink.road_y = 136.925;
    sink.in_race = true;
    rec.record_frame(sink);
  }

  CHECK(rec.finish());
  const PlaytestSummary& sum = rec.last_summary();
  CHECK(sum.ground_anomaly_count == 1);  // only the genuine sink frame, 0 false positives!

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace

int main() {
  test_jsonl_periodic_and_fields();
  test_hitches_and_top20();
  test_ground_nonfinite_and_audio_anomalies();
  test_summary_percentiles();
  test_bug_bookmark_repro_cmd_and_debounce();
  test_zero_hitch_overhead();
  test_ground_anomaly_false_positive_filters();

  if (failures == 0) {
    std::printf("playtest_recorder_tests: all checks passed\n");
    return 0;
  }
  std::printf("playtest_recorder_tests: %d failure(s)\n", failures);
  return 1;
}
