// Comprehensive cross-platform hardware telemetry & deterministic benchmark
// suite for shift-engine (JSON schema v1 + per-frame CSV). Plain C++ with no
// Vulkan dependency so it is unit-tested on every platform.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carview::bench {

inline constexpr int kSchemaVersion = 1;

enum class Scenario : uint8_t {
  kCustom = 0,
  kNoonCruise,
  kMidnightLights,
  kCockpitMirrors,
  kRacePack,
  kAutoshowStudio,
  kUi4Heavy,
  kColdBootSwitch,
};

bool parse_scenario(std::string_view name, Scenario& out);
std::string_view scenario_name(Scenario s);
std::string_view scenario_description(Scenario s);
std::vector<Scenario> all_scenarios();

// Cross-platform process resident memory (RSS current & peak in bytes).
struct ProcessMemory {
  uint64_t rss_bytes = 0;
  uint64_t peak_rss_bytes = 0;
};
ProcessMemory query_process_memory();

// Cross-platform host OS/CPU/RAM fingerprint.
struct SystemHardware {
  std::string os_name;
  std::string os_version;
  std::string cpu_arch;
  uint32_t cpu_logical_cores = 1;
  std::string cpu_brand;
  double system_ram_mib = 0.0;
};
SystemHardware probe_system_hardware();

std::string format_vulkan_version(uint32_t api_version);
std::string utc_timestamp_iso8601();

struct GpuInfo {
  std::string device_name;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  std::string device_type = "unknown";
  uint32_t api_version_raw = 0;
  std::string api_version;
  uint32_t driver_version_raw = 0;
  std::string driver_name;
  std::string driver_info;
  bool is_moltenvk = false;
  bool is_turnip = false;
  bool portability_subset = false;
  bool has_timestamps = false;
  double vram_total_mib = 0.0;
  double vram_budget_mib = 0.0;
};

struct FeatureFlags {
  bool gpu_driven = false;
  bool hiz = false;
  bool bindless = false;
  bool vrs_supported = false;
  bool vrs_active = false;
  bool hdr10_supported = false;
  bool scrgb_supported = false;
  std::string hdr_mode = "off";
  std::string upscaler = "bilinear";
  std::string frame_gen = "off";
  std::string aa_mode = "off";
  std::string ssao = "off";
  std::string shadows = "high";
  std::string vsync = "fifo";
  uint32_t msaa = 1;
  float anisotropy = 1.0f;
  int render_scale = 0;
  bool base_540p = false;
  bool z_prepass = false;
  bool reverse_z = true;
  bool bloom = true;
  bool full_fx = false;
  bool bench_audio = false;
};

struct BuildInfo {
  std::string git_commit;
  std::string build_config;
  std::string preset = "high";
  std::string device_label;
  std::string scenario = "custom";
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  uint32_t render_width = 0;
  uint32_t render_height = 0;
  double target_seconds = 0.0;
  double warmup_seconds = 0.0;
};

struct CarSwitchSample {
  std::string car;
  double load_ms = 0.0;
  double upload_ms = 0.0;
  double total_ms = 0.0;
};

struct StartupMetrics {
  double vulkan_init_ms = 0.0;
  double pipeline_compile_ms = 0.0;
  double db_catalog_load_ms = 0.0;
  double audio_load_ms = 0.0;
  double car_bake_load_ms = 0.0;
  double world_load_ms = 0.0;
  double ttff_ms = 0.0;
  std::vector<CarSwitchSample> car_switches;
};

struct FrameSample {
  uint32_t frame_index = 0;
  bool is_warmup = false;
  double sim_sec = 0.0;
  double wall_sec = 0.0;
  double frame_ms = 0.0;
  double cpu_ms = 0.0;
  double gpu_ms = -1.0;

  // Per-phase CPU breakdown (ms).
  double physics_ms = 0.0;
  double ai_traffic_ms = 0.0;
  double world_stream_ms = 0.0;
  double audio_push_ms = 0.0;
  double ui_build_ms = 0.0;
  double pose_cull_ms = 0.0;
  double cmd_record_ms = 0.0;
  double submit_wait_ms = 0.0;

  // Per-pass Vulkan GPU timestamps (ms).
  std::vector<std::pair<std::string, double>> gpu_passes;

  // Geometry / draw counters.
  uint32_t world_draws = 0;
  uint32_t world_culled = 0;
  uint32_t world_hiz_culled = 0;
  uint32_t trees_drawn = 0;
  uint32_t shadow_world_draws = 0;
  uint32_t shadow_cars = 0;
  uint32_t env_draws = 0;
  uint32_t mirror_draws = 0;
  uint32_t car_draws = 0;
  uint32_t car_instances = 0;
  uint32_t particle_sprites = 0;
  uint32_t skid_tris = 0;
  uint32_t ui4_shapes = 0;
  uint64_t triangles_drawn = 0;
  uint64_t triangles_total = 0;

  // Process RSS snapshot (MiB).
  double rss_mib = 0.0;
};

struct Distribution {
  double avg = 0.0;
  double min = 0.0;
  double p50 = 0.0;
  double p90 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double p99_9 = 0.0;
  double max = 0.0;
};

double compute_percentile(std::vector<double> values, double q);
Distribution compute_distribution(const std::vector<double>& values);
// Standard gaming 1% / 0.1% low FPS: 1000 / mean(slowest max(1, ceil(N * fraction)) frames).
double compute_low_fps(const std::vector<double>& frame_times_ms, double fraction);

struct PacingMetrics {
  uint32_t total_frames = 0;
  uint32_t warmup_frames = 0;
  double wall_sec = 0.0;
  double sim_sec = 0.0;
  double fps_avg = 0.0;
  double fps_1pct_low = 0.0;
  double fps_0_1pct_low = 0.0;
  double fps_min = 0.0;
  double fps_max = 0.0;
  Distribution frame_ms;
  Distribution cpu_ms;
  bool has_gpu_timestamps = false;
  Distribution gpu_ms;
};

struct CpuPhasesMetrics {
  Distribution physics_ms;
  Distribution ai_traffic_ms;
  Distribution world_stream_ms;
  Distribution audio_push_ms;
  Distribution ui_build_ms;
  Distribution pose_cull_ms;
  Distribution cmd_record_ms;
  Distribution submit_wait_ms;
};

struct AudioMetrics {
  bool enabled = false;
  uint64_t blocks_rendered = 0;
  double avg_block_ms = 0.0;
  double p95_block_ms = 0.0;
  double worst_block_ms = 0.0;
  uint64_t stalls = 0;
  double main_push_avg_ms = 0.0;
  double main_push_worst_ms = 0.0;
};

struct SlowFrameEntry {
  uint32_t frame_index = 0;
  double sim_sec = 0.0;
  double frame_ms = 0.0;
  double cpu_ms = 0.0;
  double gpu_ms = -1.0;
  double physics_ms = 0.0;
  double ai_traffic_ms = 0.0;
  double world_stream_ms = 0.0;
  double audio_push_ms = 0.0;
  double ui_build_ms = 0.0;
  double pose_cull_ms = 0.0;
  double cmd_record_ms = 0.0;
  double submit_wait_ms = 0.0;
  std::string dominant_phase;
};

struct StutterMetrics {
  uint32_t frames_over_16_67ms = 0;
  uint32_t frames_over_33_33ms = 0;
  uint32_t frames_over_50ms = 0;
  double hitch_threshold_ms = 0.0;
  double hitch_free_pct = 100.0;
  double worst_hitch_ms = 0.0;
  double worst_hitch_sim_sec = 0.0;
  uint32_t worst_hitch_frame = 0;
  std::vector<SlowFrameEntry> top5_slowest;
};

struct DrawCounter {
  double avg = 0.0;
  uint64_t peak = 0;
};

struct GeometryMetrics {
  DrawCounter world_draws;
  DrawCounter world_culled;
  DrawCounter world_hiz_culled;
  DrawCounter trees_drawn;
  DrawCounter shadow_world_draws;
  DrawCounter shadow_cars;
  DrawCounter env_draws;
  DrawCounter mirror_draws;
  DrawCounter car_draws;
  DrawCounter car_instances;
  DrawCounter particle_sprites;
  DrawCounter skid_tris;
  DrawCounter ui4_shapes;
  DrawCounter triangles_drawn;
  DrawCounter triangles_total;
};

struct Ui4AtlasMetrics {
  uint32_t atlas_size = 4096;
  uint32_t glyph_rows_used = 0;
  uint32_t persistent_rows_used = 0;
  uint32_t transient_rows_used = 0;
  double utilization_pct = 0.0;
  uint64_t transient_evictions = 0;
  uint32_t glyph_count = 0;
  uint32_t image_count = 0;
};

struct MemoryMetrics {
  double rss_current_mib = 0.0;
  double rss_peak_mib = 0.0;
  uint32_t vma_blocks = 0;
  uint32_t vma_allocs = 0;
  double vma_used_mib = 0.0;
  double vma_reserved_mib = 0.0;
  double vram_usage_mib = 0.0;
  double vram_budget_mib = 0.0;
  double vram_total_mib = 0.0;
};

struct StreamingMetrics {
  size_t resident_textures = 0;
  double resident_textures_mib = 0.0;
  uint64_t texture_uploads = 0;
  uint64_t texture_evictions = 0;
  uint64_t texture_reuploads = 0;
  size_t retired_pending = 0;
  size_t descriptor_pools = 0;
  uint64_t staging_upload_bytes = 0;
  double staging_upload_mib_per_sec = 0.0;
};

struct CpuUtilMetrics {
  double process_cpu_sec = 0.0;
  double wall_sec = 0.0;
  double ms_per_frame = 0.0;
  double pct_of_one_core = 0.0;
  double pct_of_all_cores = 0.0;
  uint32_t logical_cores = 1;
};

struct BenchmarkReport {
  int schema_version = kSchemaVersion;
  std::string timestamp_utc;
  SystemHardware hardware;
  GpuInfo gpu;
  FeatureFlags features;
  BuildInfo build;
  StartupMetrics startup;
  PacingMetrics pacing;
  CpuPhasesMetrics cpu_phases;
  std::vector<std::pair<std::string, Distribution>> gpu_passes;
  AudioMetrics audio;
  StutterMetrics stutter;
  GeometryMetrics geometry;
  Ui4AtlasMetrics ui4_atlas;
  MemoryMetrics memory;
  StreamingMetrics streaming;
  CpuUtilMetrics cpu_util;
};

class BenchmarkRecorder {
 public:
  void clear() { frames_.clear(); }
  void reserve(size_t n) { frames_.reserve(n); }
  void add_frame(const FrameSample& sample) { frames_.push_back(sample); }
  const std::vector<FrameSample>& frames() const { return frames_; }

  BenchmarkReport finalize(const SystemHardware& hw, const GpuInfo& gpu, const FeatureFlags& features,
                           const BuildInfo& build, const StartupMetrics& startup, const AudioMetrics& audio_in,
                           const Ui4AtlasMetrics& ui4_atlas, const MemoryMetrics& memory,
                           const StreamingMetrics& streaming, double measured_wall_sec,
                           double measured_cpu_sec) const;

 private:
  std::vector<FrameSample> frames_;
};

std::string json_escape(std::string_view s);
std::string to_json(const BenchmarkReport& report);
bool write_json(const std::filesystem::path& path, const BenchmarkReport& report, std::string& error);
std::string to_csv(const std::vector<FrameSample>& frames);
bool write_csv(const std::filesystem::path& path, const std::vector<FrameSample>& frames, std::string& error);
std::string format_console_summary(const BenchmarkReport& report);

}  // namespace carview::bench
