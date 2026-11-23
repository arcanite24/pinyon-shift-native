// Unit tests for the cross-device hardware benchmark & telemetry suite:
// percentile math, 1%/0.1% lows, hitch/stutter attribution, warmup exclusion,
// cross-platform RSS & hardware probing, JSON schema v1 & CSV serialization,
// and scenario parsing. Plain C++ (no Vulkan).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "benchmark.h"
#include "perf_overlay.h"

using namespace carview::bench;

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

void test_scenarios() {
  Scenario s = Scenario::kCustom;
  CHECK(parse_scenario("noon_cruise", s) && s == Scenario::kNoonCruise);
  CHECK(parse_scenario("MIDNIGHT_LIGHTS", s) && s == Scenario::kMidnightLights);
  CHECK(parse_scenario(" cockpit_mirrors ", s) && s == Scenario::kCockpitMirrors);
  CHECK(parse_scenario("race_pack", s) && s == Scenario::kRacePack);
  CHECK(parse_scenario("autoshow_studio", s) && s == Scenario::kAutoshowStudio);
  CHECK(parse_scenario("ui4_heavy", s) && s == Scenario::kUi4Heavy);
  CHECK(parse_scenario("cold_boot_switch", s) && s == Scenario::kColdBootSwitch);
  CHECK(parse_scenario("custom", s) && s == Scenario::kCustom);
  CHECK(!parse_scenario("nonexistent_scenario", s));
  CHECK(!parse_scenario("", s));

  const auto list = all_scenarios();
  CHECK(list.size() == 7);
  for (Scenario sc : list) {
    CHECK(!scenario_name(sc).empty());
    CHECK(!scenario_description(sc).empty());
    Scenario parsed = Scenario::kCustom;
    CHECK(parse_scenario(scenario_name(sc), parsed) && parsed == sc);
  }
}

void test_percentiles_and_lows() {
  // Empty and single-element edge cases.
  CHECK(near(compute_percentile({}, 0.5), 0.0));
  CHECK(near(compute_percentile({16.0}, 0.99), 16.0));
  CHECK(near(compute_low_fps({}, 0.01), 0.0));

  // 1000 values: 1.0 .. 1000.0 ms.
  std::vector<double> vals;
  vals.reserve(1000);
  for (int i = 1000; i >= 1; --i) vals.push_back(double(i));  // unsorted input

  const Distribution d = compute_distribution(vals);
  CHECK(near(d.min, 1.0, 1e-6));
  CHECK(near(d.max, 1000.0, 1e-6));
  CHECK(near(d.avg, 500.5, 1e-6));
  CHECK(near(d.p50, 500.5, 1e-3));
  CHECK(near(d.p90, 900.1, 1e-2));
  CHECK(near(d.p95, 950.05, 1e-2));
  CHECK(near(d.p99, 990.01, 1e-2));
  CHECK(near(d.p99_9, 999.001, 1e-2));

  // 1% low of 1000 frames: mean of slowest 10 frames (991..1000 -> mean = 995.5 ms).
  const double low1 = compute_low_fps(vals, 0.01);
  CHECK(near(low1, 1000.0 / 995.5, 1e-4));

  // 0.1% low of 1000 frames: mean of slowest 1 frame (1000 ms -> 1.0 FPS).
  const double low01 = compute_low_fps(vals, 0.001);
  CHECK(near(low01, 1.0, 1e-4));

  // FrameStats::ms_low_fps consistency.
  carview::FrameStats fs(10.0, 1024);
  double t = 0.0;
  fs.frame(t);
  for (int i = 0; i < 99; ++i) {
    t += 0.010;  // 10 ms -> 100 FPS
    fs.frame(t);
  }
  t += 0.050;  // 1 hitch frame at 50 ms -> 20 FPS
  fs.frame(t);
  CHECK(near(fs.ms_low_fps(0.01), 20.0, 0.2));
}

void test_hardware_and_memory_probing() {
  const SystemHardware hw = probe_system_hardware();
  CHECK(!hw.os_name.empty());
  CHECK(!hw.os_version.empty());
  CHECK(!hw.cpu_arch.empty());
  CHECK(hw.cpu_logical_cores >= 1);
  CHECK(!hw.cpu_brand.empty());
  CHECK(hw.system_ram_mib > 64.0);

  const ProcessMemory pm = query_process_memory();
  CHECK(pm.rss_bytes > 0);
  CHECK(pm.peak_rss_bytes >= pm.rss_bytes);

  // Vulkan version encoding: 1.3.296.
  const uint32_t v = (1u << 22) | (3u << 12) | 296u;
  CHECK(format_vulkan_version(v) == "1.3.296");
  CHECK(!utc_timestamp_iso8601().empty());
}

void test_recorder_hitches_and_serialization() {
  BenchmarkRecorder rec;
  // 5 warmup frames with massive synthetic spikes (must be excluded from aggregates).
  for (uint32_t i = 0; i < 5; ++i) {
    FrameSample f{};
    f.frame_index = i;
    f.is_warmup = true;
    f.sim_sec = double(i) / 60.0;
    f.wall_sec = double(i) * 0.1;
    f.frame_ms = 250.0;
    f.cpu_ms = 200.0;
    f.gpu_ms = 50.0;
    f.world_stream_ms = 190.0;
    f.gpu_passes = {{"shadows", 20.0}, {"post", 30.0}};
    rec.add_frame(f);
  }
  // 100 measured frames: 96 fast (~10 ms), 2 over 16.67ms (20 ms), 1 over 33.33ms (40 ms), 1 over 50ms (60 ms).
  for (uint32_t i = 0; i < 100; ++i) {
    FrameSample f{};
    f.frame_index = 5 + i;
    f.is_warmup = false;
    f.sim_sec = double(5 + i) / 60.0;
    f.wall_sec = 0.5 + double(i) * 0.011;
    if (i == 10 || i == 20) {
      f.frame_ms = 20.0;
      f.physics_ms = 2.0;
      f.ai_traffic_ms = 3.0;
      f.cmd_record_ms = 12.0;
      f.submit_wait_ms = 3.0;
    } else if (i == 50) {
      f.frame_ms = 40.0;
      f.world_stream_ms = 32.0;
      f.cmd_record_ms = 5.0;
      f.submit_wait_ms = 3.0;
    } else if (i == 80) {
      f.frame_ms = 60.0;
      f.ui_build_ms = 48.0;
      f.cmd_record_ms = 7.0;
      f.submit_wait_ms = 5.0;
    } else {
      f.frame_ms = 10.0;
      f.physics_ms = 1.0;
      f.ai_traffic_ms = 1.0;
      f.world_stream_ms = 0.5;
      f.audio_push_ms = 0.2;
      f.ui_build_ms = 0.8;
      f.pose_cull_ms = 1.5;
      f.cmd_record_ms = 2.0;
      f.submit_wait_ms = 3.0;
    }
    f.cpu_ms = f.physics_ms + f.ai_traffic_ms + f.world_stream_ms + f.audio_push_ms +
               f.ui_build_ms + f.pose_cull_ms + f.cmd_record_ms;
    f.gpu_ms = 6.5;
    f.gpu_passes = {{"shadows", 1.2}, {"world gpu", 2.8}, {"post", 1.5}, {"ui", 0.5}};
    f.world_draws = 300;
    f.world_culled = 150;
    f.ui4_shapes = 120;
    f.triangles_drawn = 500000;
    f.triangles_total = 1200000;
    f.rss_mib = 512.0;
    rec.add_frame(f);
  }

  SystemHardware hw = probe_system_hardware();
  GpuInfo gpu{};
  gpu.device_name = "Test \"GPU\" \\ Alpha";
  gpu.vendor_id = 0x10de;
  gpu.device_id = 0x2684;
  gpu.device_type = "discrete";
  gpu.api_version = "1.3.296";
  gpu.has_timestamps = true;
  gpu.vram_total_mib = 8192.0;

  FeatureFlags feat{};
  feat.gpu_driven = true;
  feat.hiz = true;
  feat.bindless = true;
  feat.msaa = 4;
  feat.full_fx = true;
  feat.bench_audio = true;

  BuildInfo binfo{};
  binfo.git_commit = "deadbeef";
  binfo.build_config = "Release";
  binfo.preset = "high";
  binfo.device_label = "UnitTest Rig";
  binfo.scenario = "noon_cruise";
  binfo.output_width = 1920;
  binfo.output_height = 1080;
  binfo.render_width = 1920;
  binfo.render_height = 1080;

  StartupMetrics st{};
  st.vulkan_init_ms = 42.0;
  st.pipeline_compile_ms = 120.0;
  st.ttff_ms = 650.0;
  st.car_switches.push_back({"NIS_SkylineGTRVSpecII_02", 18.0, 22.0, 42.5});

  AudioMetrics aud{};
  aud.enabled = true;
  aud.blocks_rendered = 120;
  aud.avg_block_ms = 0.85;
  aud.p95_block_ms = 1.4;
  aud.worst_block_ms = 2.1;
  aud.stalls = 0;

  Ui4AtlasMetrics ui_m{};
  ui_m.utilization_pct = 37.5;
  ui_m.transient_evictions = 3;

  MemoryMetrics mem{};
  mem.rss_current_mib = 512.0;
  mem.rss_peak_mib = 600.0;
  mem.vma_blocks = 12;
  mem.vma_allocs = 240;
  mem.vma_used_mib = 420.0;
  mem.vma_reserved_mib = 512.0;

  StreamingMetrics str{};
  str.resident_textures = 480;
  str.resident_textures_mib = 210.0;
  str.staging_upload_bytes = 64ull * 1024ull * 1024ull;

  const BenchmarkReport rep = rec.finalize(hw, gpu, feat, binfo, st, aud, ui_m, mem, str, 2.0, 3.0);

  CHECK(rep.pacing.total_frames == 100);
  CHECK(rep.pacing.warmup_frames == 5);
  CHECK(near(rep.pacing.frame_ms.p50, 10.0, 1e-3));
  CHECK(near(rep.pacing.frame_ms.max, 60.0, 1e-3));
  CHECK(rep.stutter.frames_over_16_67ms == 4);  // 20, 20, 40, 60
  CHECK(rep.stutter.frames_over_33_33ms == 2);  // 40, 60
  CHECK(rep.stutter.frames_over_50ms == 1);     // 60
  CHECK(near(rep.stutter.hitch_threshold_ms, 15.0, 1e-3));
  CHECK(near(rep.stutter.hitch_free_pct, 96.0, 1e-3));
  CHECK(near(rep.stutter.worst_hitch_ms, 60.0, 1e-3));
  CHECK(rep.stutter.worst_hitch_frame == 85);
  CHECK(rep.stutter.top5_slowest.size() == 5);
  CHECK(rep.stutter.top5_slowest[0].dominant_phase == "ui_build");
  CHECK(rep.stutter.top5_slowest[1].dominant_phase == "world_stream");
  CHECK(rep.stutter.top5_slowest[2].dominant_phase == "cmd_record");
  CHECK(rep.gpu_passes.size() == 4);
  CHECK(near(rep.cpu_util.pct_of_one_core, 150.0, 1e-2));
  CHECK(near(rep.streaming.staging_upload_mib_per_sec, 32.0, 1e-2));

  // Verify JSON output & escaping.
  const std::string json = to_json(rep);
  CHECK(json.find("\"schema_version\": 1") != std::string::npos);
  CHECK(json.find("Test \\\"GPU\\\" \\\\ Alpha") != std::string::npos);
  CHECK(json.find("\"startup_ms\"") != std::string::npos);
  CHECK(json.find("\"pacing\"") != std::string::npos);
  CHECK(json.find("\"cpu_phases_ms\"") != std::string::npos);
  CHECK(json.find("\"gpu_passes_ms\"") != std::string::npos);
  CHECK(json.find("\"stutter\"") != std::string::npos);
  CHECK(json.find("\"top5_slowest_frames\"") != std::string::npos);
  CHECK(json.find("\"ui4_atlas\"") != std::string::npos);
  CHECK(json.find("\"car_switches\"") != std::string::npos);
  CHECK(json.find("\"fps_1pct_low\"") != std::string::npos);
  CHECK(json.find("\"fps_0_1pct_low\"") != std::string::npos);

  // Verify CSV output (header + 105 frames including 5 warmup rows).
  const std::string csv = to_csv(rec.frames());
  CHECK(csv.find("frame,warmup,sim_sec,wall_sec,frame_ms,cpu_ms,gpu_ms") == 0);
  CHECK(csv.find("gpu_world_gpu_ms") != std::string::npos);
  size_t line_count = 0;
  for (char c : csv) if (c == '\n') ++line_count;
  CHECK(line_count == 106);

  // Verify write_json & write_csv to temp files.
  const auto tmp = std::filesystem::temp_directory_path() / "shift_bench_unit_test";
  std::string err;
  CHECK(write_json(tmp / "test.json", rep, err));
  CHECK(write_csv(tmp / "test.csv", rec.frames(), err));
  CHECK(std::filesystem::exists(tmp / "test.json"));
  CHECK(std::filesystem::exists(tmp / "test.csv"));
  std::error_code ec;
  std::filesystem::remove_all(tmp, ec);

  const std::string summary = format_console_summary(rep);
  CHECK(summary.find("bench[noon_cruise]:") != std::string::npos);
  CHECK(summary.find("1% low") != std::string::npos);
}

}  // namespace

int main() {
  test_scenarios();
  test_percentiles_and_lows();
  test_hardware_and_memory_probing();
  test_recorder_hitches_and_serialization();
  if (failures) {
    std::printf("benchmark_tests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("benchmark_tests: ok\n");
  return 0;
}
