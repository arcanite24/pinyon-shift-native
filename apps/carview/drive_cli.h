// Parsed CLI + Settings configuration for `carview --drive` and headless menu
// audit image dumps.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "audio_host.h"
#include "benchmark.h"
#include "frame_inspector.h"
#include "perf_overlay.h"
#include "race_view.h"
#include "render_modern.h"
#include "touch_controls.h"
#include "world/world.h"

namespace carview {

struct UiFontCache;

struct DriveConfig {
  std::chrono::steady_clock::time_point drive_start_tp = std::chrono::steady_clock::now();
  bench::StartupMetrics bench_startup{};

  bool post_on = true;
  int bloom_flag = -1;
  int motion_blur_flag = -1;
  int dof_flag = -1;
  double exposure_given = 0;
  bool low_preset = false;
  std::string shadows_flag;
  std::filesystem::path bake_dir, script_path, world_dir, cache_path;
  double near_x = world::kDumpCameraX;
  double near_z = world::kDumpCameraZ;
  double near_heading = world::kDumpCameraHeading;
  double cruise_kmh = 40;
  double spawn[4] = {0, 0, 0, 0};
  std::filesystem::path game_root_given;
  bool near_given = false, have_spawn = false, cli_spawn_given = false, fast_travel = false, render_ground = false,
       async_stream = false, flat_world = false, no_props = false, no_far = false, no_trees = false;
  bool cli_title_screen = false, cli_title_menu = false;
  bool bench = false;
  bench::Scenario bench_scenario = bench::Scenario::kCustom;
  bool bench_scenario_set = false;
  std::filesystem::path bench_json_path, bench_csv_path;
  bool bench_full_fx = false;
  bool bench_audio = false;
  double bench_warmup_sec = 0.0;
  bool bench_warmup_explicit = false;
  std::string preset_name = "high";
  bool cruise_kmh_explicit = false;
  uint32_t window_frames_limit = 0;
  FrameStats frame_stats;
  uint32_t msaa = 4;
  std::filesystem::path world_reference_path;
  std::string car, out, run = "slalom", view = "chase";
  uint32_t width = 960, height = 540;
  bool size_given = false;
  double seconds = 6, every = 1;
  bool headless = false;
  bool hud = false;
  std::filesystem::path save_path, menu_png;
  std::filesystem::path import_dir;
  bool import_again = false;
  bool no_save = false;
  bool validation = std::getenv("SHIFT_VALIDATION") != nullptr;
  std::filesystem::path sound_dir = "out/sound", audio_out;
  bool sound = true, sound_given = false;
  bool radio_on = true;
  double radio_press_every = 0;
  AudioConfig audio_cfg;
  std::string race_event;
  RaceView::Options race_options;
  int race_auto = -1;
  std::filesystem::path play_dir;
  int traffic_flag = -1;
  double hour = 12;
  double day_length_scale = 1.0;
  bool hour_explicit = false;
  uint64_t traffic_seed = 1;
  std::filesystem::path contacts_log_path;
  bool texture_census = false;
  bool collectibles_on = true, reveal = false, treasure_map_setting = false;
  bool has_cli_gps = false;
  double cli_gps_x = 0.0, cli_gps_z = 0.0;
  std::string cli_pr_stunt, cli_outpost_hub;
  bool demo_speed_trap = false, demo_h2h = false;
  bool demo_rally_stage = false, demo_rally_service = false, demo_club = false;
  std::filesystem::path pause_map_png;
  double cli_map_zoom = 4.5;
  double seed_distance = 0;
  std::filesystem::path map_png, career_png;
  int gather_route = 0;
  bool gather_route_given = false;
  int camera_mode = 0;
  std::string language = "EN";
  bool reverse_z = true;
  bool backface_cull = (std::getenv("SHIFT_NO_CULL") == nullptr);
  bool z_prepass = (std::getenv("SHIFT_Z_PREPASS") != nullptr);
  float preset_reflection_rate = 1.0f;
  float preset_anisotropy = -1.0f;
  modern::GraphicsSettings cli_graphics;
  bool cli_ssao_explicit = false;
  bool cli_sscs_explicit = false;
  bool cli_hextile_explicit = false;
  bool cli_velblur_explicit = false;
  bool cli_pcss_explicit = false;
  bool cli_ssr_explicit = false;
  bool cli_volfog_explicit = false;
  bool cli_cardetail_explicit = false;
  bool cli_preset_explicit = false;
  bool cli_msaa_explicit = false;
  bool cli_aa_explicit = false;
  bool cli_texcap_explicit = false;
  bool cli_fpscap_explicit = false;
  bool cli_scale_explicit = false;
  bool cli_output_explicit = false;
  uint32_t cli_hdr_pattern = 0;
  float cli_hdr_pattern_nits = 1000.0f;
  bool cli_perf_overlay = false;
  bool cli_frame_inspector = false;
  bool debug_title = false;  // R3-UI-18: show live telemetry in window title only when explicitly requested
  InspectorView cli_inspector_view = InspectorView::kColor;
  std::vector<std::string> args;
  bool photo_through_walls_cli = false;
  bool photo_wider_limits_cli = false;
  bool timed_autosave = false;  // --timed-autosave: the engine's 30 s save (an extra; the original saves at checkpoints)
  std::filesystem::path demo_photo_png;
  std::filesystem::path demo_replay_png;
  std::filesystem::path demo_achievements_png;
  std::filesystem::path demo_touch_hud_png;
  std::filesystem::path demo_upgrade_shop_png;
  TouchControls touch_controls;
  std::string android_gpu_driver = "auto";
  std::string graphics_backend = "auto";
  bool list_backends = false;
  bool save_settings = false;
  bool no_title = false;
  bool skip_intro_movies = false;
  bool start_in_single_player = false;
  bool skip_vo_with_a = true;
  bool metric_units = true;
  bool story_intro_cli = false;
  std::string cli_campaign_stage;
  // Capture entry point: `--cutscene <name> [--cutscene-time <s>]` plays a
  // disc cutscene frozen at that time.
  std::string cli_cutscene;
  double cli_cutscene_time = 0.0;
  std::filesystem::path telemetry_json_path;
  std::filesystem::path record_session_dir;
  std::string capture_times_csv;
  std::filesystem::path cli_fmv_path;
  std::filesystem::path mods_root;
  bool mods_off = false;
  bool mods_native = false;
  bool cli_dev_hints = (std::getenv("SHIFT_DEV_HINTS") != nullptr && std::string(std::getenv("SHIFT_DEV_HINTS")) == "1");
  std::filesystem::path sync_folder;
  std::string device_name, profile_name;
  std::string bench_switch_car;

  // Parses settings file + CLI arguments; returns false if the app should exit with `exit_code`.
  bool parse(int argc, char** argv, int& exit_code);
};

void drive_usage();
void print_rhi_backends();

// Handles `--menu-png` headless UI audit image dumps. Returns exit status (0 ok, 1 error).
int run_headless_menu_dumps(const DriveConfig& cfg, UiFontCache& font_cache);

}  // namespace carview
