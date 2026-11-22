// Interactive FH1-branded GUI state and UI builder for shift-launcher.
// Renders identically through Vulkan (carview::Renderer) and CPU rasterization
// (carview::ui::rasterize) for headless screenshot verification.
#pragma once

#include <cstdint>
#include <string>
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
  bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};

struct LauncherUiState {
  LauncherTab active_tab = LauncherTab::kPlay;
  LauncherPaths paths;
  PlayConfig play_cfg;
  HardwareInfo hw_info;
  HardwareRecommendation hw_rec;

  // Play tab state
  int selected_preset = 0;  // 0: high, 1: low, 2: smooth-60, 3: quality-30, 4: low-power
  std::string launch_car = "NIS_SkylineGTRVSpecII_02";
  std::string extra_args;

  // Setup tab state
  std::string disc_path_input;
  DiscVerification disc_check;
  bool setup_all_sounds = false;
  bool setup_build_index = true;
  SetupProgress setup_progress;

  // DLC & v4 tab state
  ContentStatus content_status;

  // Mods tab state
  std::vector<ModEntry> mods;
  ModSaveIsolation save_iso;
  int selected_mod = 0;

  // Custom Radio tab state
  RadioBuilderConfig radio_cfg;
  std::vector<RadioTrackInfo> radio_tracks;
  LogoValidation radio_logo_check;
  std::string radio_status;

  // Wheel & Controller tab state
  std::vector<WheelDeviceInfo> devices;
  int selected_device = 0;
  WheelBindingConfig wheel_cfg;
  float live_steer = 0.0f;
  float live_throttle = 0.0f;
  float live_brake = 0.0f;
  float live_clutch = 0.0f;
  float live_handbrake = 0.0f;
  int live_gear = 0;  // -1=R, 0=N, 1..6
  std::string wheel_status;

  // Self-Updater tab state
  std::string current_version = "0.1.0";
  std::string update_channel = "stable";
  std::optional<AvailableUpdate> available_update;
  std::string update_status = "Up to date (click Check for Updates to query GitHub Releases).";

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

  // Global status toast & pointer state
  std::string banner_notice;
  float mouse_x = -1.0f;
  float mouse_y = -1.0f;
  std::string hovered_id;
};

// Populates realistic showcase data for headless --demo screenshot verification.
void populate_demo_state(LauncherUiState& st);

// Builds the entire launcher frame into `list` and fills `hitboxes` for click handling.
void build_launcher_ui(carview::ui::List& list, const LauncherUiState& st, float width, float height,
                       std::vector<HitBox>& hitboxes);

}  // namespace launcher
