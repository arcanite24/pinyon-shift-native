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
  kDlc = 2,
  kMods = 3,
  kRadio = 4,
  kWheel = 5,
  kUpdate = 6,
  kAndroid = 7,
  kCount = 8,
};

const char* tab_name(LauncherTab tab);
bool parse_tab_name(std::string_view name, LauncherTab& out);

struct HitBox {
  std::string id;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool focusable = true;
  bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};

struct ResolutionPreset {
  const char* label;
  int width;
  int height;
};

inline constexpr ResolutionPreset kResolutionPresets[5] = {
    {"Auto", 0, 0},
    {"1280\xC3\x97" "720", 1280, 720},
    {"1920\xC3\x97" "1080", 1920, 1080},
    {"2560\xC3\x97" "1440", 2560, 1440},
    {"3840\xC3\x97" "2160", 3840, 2160},
};

struct LauncherUiState {
  LauncherTab active_tab = LauncherTab::kPlay;
  LauncherPaths paths;
  PlayConfig play_cfg;
  HardwareInfo hw_info;
  HardwareRecommendation hw_rec;

  // Play tab state
  int selected_preset = 0;      // 0: high, 1: low, 2: smooth-60, 3: quality-30, 4: low-power
  int selected_resolution = 0;  // 0..4 into kResolutionPresets
  int selected_backend = 0;     // 0..N into available_backend_choices()
  bool launch_fullscreen = false;
  std::string launch_car = "NIS_SkylineGTRVSpecII_02";
  std::string extra_args;

  // Setup tab state
  std::string disc_path_input;
  DiscVerification disc_check;
  bool setup_all_sounds = false;
  bool setup_build_index = true;
  bool setup_log_expanded = true;
  int setup_log_scroll = 0;
  SetupProgress setup_progress;

  // Content (DLC & TU) tab state
  ContentStatus content_status;
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
