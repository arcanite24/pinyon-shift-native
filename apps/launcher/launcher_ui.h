// GUI state and UI builder for shift-launcher.
// Renders identically through Vulkan (carview::Renderer) and CPU rasterization
// (carview::ui::rasterize) for headless screenshot verification.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "launcher_core.h"
#include "ui.h"

namespace launcher {

enum class LauncherTab : int {
  kPlay = 0,
  kSetup = 1,
  kSaves = 2,
  kDlc = 3,
  kMods = 4,
  kRadio = 5,
  kWheel = 6,
  kUpdate = 7,
  kAndroid = 8,
  kCount = 9,
};

const char* tab_name(LauncherTab tab);
bool parse_tab_name(std::string_view name, LauncherTab& out);

struct HitBox {
  std::string id;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool focusable = true;
  bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};

struct LauncherUiState {
  LauncherTab active_tab = LauncherTab::kPlay;
  LauncherPaths paths;
  PlayConfig play_cfg;
  HardwareInfo hw_info;
  HardwareRecommendation hw_rec;

  // Play tab state
  int selected_preset = 1;      // 0: ultra, 1: high, 2: low, 3: smooth-60, 4: quality-30, 5: low-power
  int selected_resolution = 0;  // 0..4 into kResolutionPresets
  int selected_backend = 0;     // 0..N into available_backend_choices()
  int selected_vsync = 0;       // 0..2 into kVsyncChoices
  int selected_fps_cap = 0;     // 0..6 into kFpsCapChoices
  bool launch_fullscreen = false;
  std::string launch_car = "NIS_SkylineGTRVSpecII_02";
  std::string extra_args;

  // Saves tab state
  std::vector<SaveProfileEntry> save_profiles;
  int selected_save_profile = 0;
  int save_profiles_scroll = 0;
  std::string new_profile_gamertag;
  std::vector<PinyonSaveSource> pinyon_sources;
  int selected_pinyon_source = 0;
  int pinyon_sources_scroll = 0;
  std::string saves_status;

  // Setup tab state
  std::string disc_path_input;
  std::string state_path_input;
  std::string save_import_path;
  DiscVerification disc_check;
  bool launcher_sounds_enabled = true;
  bool setup_all_sounds = false;
  bool setup_build_index = true;
  bool setup_log_expanded = true;
  int setup_log_scroll = 0;
  SetupProgress setup_progress;

  // Content (DLC & TU) tab state
  ContentStatus content_status;
  ContentInstallProgress content_install;
  int content_scroll = 0;

  // Mods tab state
  std::vector<ModEntry> mods;
  ModSaveIsolation save_iso;
  int selected_mod = 0;
  int mods_scroll = 0;

  // Custom Radio tab state
  RadioBuilderConfig radio_cfg;
  std::vector<RadioTrackInfo> radio_tracks;
  LogoValidation radio_logo_check;
  std::string radio_status;
  int radio_scroll = 0;

  // Controls (Wheel & Controller) tab state
  std::vector<WheelDeviceInfo> devices;
  int selected_device = 0;
  int devices_scroll = 0;
  WheelBindingConfig wheel_cfg;
  float live_steer = 0.0f;
  float live_throttle = 0.0f;
  float live_brake = 0.0f;
  float live_clutch = 0.0f;
  float live_handbrake = 0.0f;
  int live_gear = 0;  // -1=R, 0=N, 1..6
  std::string wheel_status;

  // Updates tab state
  std::string current_version = "0.1.0";
  std::string update_channel = "stable";
  std::optional<AvailableUpdate> available_update;
  std::filesystem::path staged_apply_script;
  int staged_file_count = 0;
  std::string update_status = "No update check run yet.";

  // Android Wi-Fi Sync tab state
  bool sync_include_play = true;
  bool sync_include_dlc = true;
  bool sync_include_mods = true;
  bool sync_include_radio = true;
  bool sync_active = false;
  std::string sync_lan_ip = "192.168.1.42";
  uint16_t sync_port = WifiSyncServer::kPreferredPort;
  WifiSyncManifest sync_manifest;
  QrMatrix sync_qr;
  std::vector<std::string> sync_activity;
  int sync_log_scroll = 0;

  // Global status notice & interaction state
  std::string banner_notice;
  float mouse_x = -1.0f;
  float mouse_y = -1.0f;
  std::string hovered_id;
  std::string pressed_id;
  std::string focused_id;
};

// Populates realistic showcase data for headless --demo screenshot verification.
void populate_demo_state(LauncherUiState& st);

// Builds the launcher frame into `list` and fills `hitboxes` for click/keyboard handling.
// `width` and `height` are in physical pixels; `dpi_scale` is the display pixel ratio (1.0 at 96 DPI, 2.0 on Retina).
void build_launcher_ui(carview::ui::List& list, const LauncherUiState& st, float width, float height,
                       std::vector<HitBox>& hitboxes, float dpi_scale = 1.0f);

}  // namespace launcher
