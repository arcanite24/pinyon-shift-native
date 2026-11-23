// Core logic for Shift Engine Launcher.
// Pure C++20 + optional volk Vulkan probe + POSIX/Winsock LAN sync so every
// subsystem (disc verification, portable paths, hardware detection, Steam VDF,
// DLC/v4/UDLC/XE scanner, Mod Manager, Custom Radio Builder, Wheel settings,
// Self-Updater, and Android Wi-Fi Sync server) is deterministic and testable
// under ctest with zero GUI or network dependency.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace launcher {

// ---- 1. Portable Mode & Cross-Platform Paths -------------------------------

struct LauncherPaths {
  bool portable = false;
  std::filesystem::path launcher_dir;
  std::filesystem::path data_root;
  std::filesystem::path play_dir;
  std::filesystem::path drive_dir;
  std::filesystem::path mods_dir;
  std::filesystem::path state_dir;
  std::filesystem::path settings_file;
  std::filesystem::path logs_dir;
  std::filesystem::path carview_bin;
  std::filesystem::path carbake_bin;
  std::filesystem::path soundbake_bin;
  std::filesystem::path ffmpeg_bin;
  std::string path_length_warning;
};

// Resolves standard OS user-data paths (%APPDATA%/shift-engine on Windows,
// ~/Library/Application Support/shift-engine on macOS, ~/.local/share/shift-engine
// on Linux) or self-contained <launcher_dir>/data when portable.txt exists or
// --portable is passed.
LauncherPaths resolve_launcher_paths(const std::filesystem::path& launcher_dir,
                                     bool force_portable = false,
                                     const std::string& os_override = {});

// Checks whether a portable data root exceeds the safe Windows tool path budget.
std::string check_portable_path_length(const std::filesystem::path& data_root, int max_chars = 70);

// Reads/writes play.txt ("game <path>\n" + optional "state <path>\n").
struct PlayConfig {
  std::filesystem::path game_root;
  std::filesystem::path state_root;
  int baked_car_count = 0;
  bool has_colorado_index = false;
  bool has_sound_dir = false;
};
bool read_play_config(const std::filesystem::path& play_dir, PlayConfig& out);
bool write_play_config(const std::filesystem::path& play_dir, const std::filesystem::path& game_root,
                       const std::filesystem::path& state_root = {}, std::string* error = nullptr);

// ---- 2. Setup & Disc Verification ------------------------------------------

struct DiscCheckItem {
  std::string label;
  std::string relative_path;
  bool required = true;
  bool present = false;
  uint64_t bytes = 0;
  std::string sha256_prefix;  // first 16 hex chars when computed
};

struct DiscVerification {
  bool valid = false;
  bool looks_like_iso = false;
  std::string iso_format;              // "XISO", "XGD3", "XGD2", "XGD1" when looks_like_iso
  int iso_file_count = 0;              // files to extract from ISO (media/ tree)
  uint64_t iso_extract_bytes = 0;      // total bytes to extract from ISO
  uint64_t required_space_bytes = 0;   // free space required for extraction + bake
  std::filesystem::path resolved_root;
  int car_archive_count = 0;
  uint64_t free_space_bytes = 0;
  bool sufficient_space = true;
  std::string summary;
  std::string db_sha256;
  std::vector<DiscCheckItem> items;
};

// Returns true if the path has an Xbox 360 disc image extension (.iso / .xiso).
bool is_iso_image_path(const std::filesystem::path& path);

DiscVerification verify_disc_folder(const std::filesystem::path& selected_path,
                                    const std::filesystem::path& target_play_dir = {});

struct IsoExtractProgress {
  uint64_t bytes_done = 0;
  uint64_t bytes_total = 0;
  size_t files_done = 0;
  size_t files_total = 0;
  std::string current_file;
};

struct IsoExtractResult {
  bool ok = false;
  bool cancelled = false;
  std::string format_label;
  size_t files_extracted = 0;
  size_t files_skipped = 0;
  uint64_t bytes_written = 0;
  std::filesystem::path extracted_game_root;
  std::string error;
};

// Extracts media/ from an Xbox 360 GDFX/XISO/XGD2/XGD3 .iso image into `target_game_dir`.
IsoExtractResult extract_disc_iso(
    const std::filesystem::path& iso_path,
    const std::filesystem::path& target_game_dir,
    const std::function<void(const IsoExtractProgress&)>& progress_cb = {},
    const std::atomic<bool>* cancel = nullptr);

struct SetupOptions {
  std::filesystem::path game_root;  // extracted disc folder OR .iso/.xiso image
  std::filesystem::path state_root;
  std::filesystem::path play_dir;
  bool all_sounds = false;
  bool build_index = true;
  bool skip_bake = false;  // extract + verify + write play.txt only (for unit tests)
  std::string cars_filter;  // optional comma-separated car media names
};

struct SetupProgress {
  bool running = false;
  bool finished = false;
  bool success = false;
  int stage_index = 0;   // 0..4
  int stage_count = 4;
  float fraction = 0.0f; // 0..1
  std::string stage_label;
  std::string error_text;
  std::vector<std::string> log_lines;
};

class SetupRunner {
 public:
  SetupRunner() = default;
  ~SetupRunner();
  SetupRunner(const SetupRunner&) = delete;
  SetupRunner& operator=(const SetupRunner&) = delete;

  bool start(const LauncherPaths& paths, const SetupOptions& opts);
  void cancel();
  SetupProgress snapshot() const;

 private:
  void append_log(const std::string& line);
  void set_stage(int idx, const std::string& label, float frac);
  int run_subprocess_logged(const std::vector<std::string>& args);

  mutable std::mutex mu_;
  SetupProgress state_;
  std::atomic<bool> cancel_{false};
  std::thread worker_;
};

// ---- 3. Hardware Detection & Preset Recommendation -------------------------

struct GpuInfo {
  std::string name;
  bool discrete = false;
  bool software = false;
  uint64_t vram_bytes = 0;
  uint32_t api_version = 0;  // VK_MAKE_API_VERSION
  uint32_t vendor_id = 0;

  std::string vulkan_version_string() const;
  bool supports_vulkan_1_3() const;
  bool supports_vulkan_1_1() const;
};

struct HardwareInfo {
  std::optional<GpuInfo> gpu;
  int physical_cores = 0;
  int logical_threads = 0;
  int refresh_hz = 60;
};

struct HardwareRecommendation {
  std::string preset_id;      // "high", "low", "balanced", "ultra_low"
  std::string carview_preset; // "high" or "low" (passed to --preset)
  std::string label;          // "Performance 120", "Quality 60", "Low-spec 60", "Balanced 40", "Ultra Low 30"
  std::string reason;
  std::string warning;        // empty when driver/GPU is healthy
  std::string driver_url;
};

HardwareInfo probe_hardware(int refresh_hz = 60);
HardwareRecommendation recommend_preset(const HardwareInfo& info);
std::string describe_hardware(const HardwareInfo& info);
std::string vendor_driver_url(uint32_t vendor_id);

struct BackendChoice {
  const char* id;     // "auto", "vulkan", "dx12", "metal"
  const char* label;  // "Auto", "Vulkan", "DirectX 12", "Metal 3"
};
std::vector<BackendChoice> available_backend_choices(const std::string& os_override = {});
std::string load_graphics_backend_setting(const std::filesystem::path& settings_file);
bool save_graphics_backend_setting(const std::filesystem::path& settings_file,
                                   std::string_view backend_id,
                                   std::string* error = nullptr);

// ---- 4. Add to Steam Shortcut Generator (Binary VDF) -----------------------

struct SteamShortcutEntry {
  uint32_t app_id = 0;
  std::string app_name;
  std::string exe;
  std::string start_dir;
  std::string icon;
  std::string launch_options;
  bool allow_desktop_config = true;
  bool allow_overlay = true;
};

uint32_t steam_shortcut_app_id(std::string_view quoted_exe, std::string_view app_name);
std::vector<uint8_t> encode_shortcuts_vdf(const std::vector<SteamShortcutEntry>& entries);
bool parse_shortcuts_vdf(std::span<const uint8_t> bytes, std::vector<SteamShortcutEntry>& out,
                         std::string* error = nullptr);
std::vector<uint8_t> upsert_steam_shortcut_vdf(std::span<const uint8_t> existing_vdf,
                                               const SteamShortcutEntry& entry, bool* updated_existing = nullptr);

struct SteamInstallResult {
  bool ok = false;
  int steam_users_updated = 0;
  bool desktop_entry_written = false;
  std::string message;
};
SteamInstallResult add_to_steam(const LauncherPaths& paths, const std::string& app_name = "Shift Engine");

// ---- 5. DLC, Title Update v4, UDLC v1.2 & Forza Horizon XE -----------------

enum class ContentKind { kTitleUpdateV4, kDlcPack, kRallyExpansion, kClub1000, kUdlcV12, kHorizonXe };

struct ContentPackage {
  std::string id;
  std::string title;
  ContentKind kind = ContentKind::kDlcPack;
  std::filesystem::path path;
  bool installed = false;
  bool enabled = false;
  bool verified = false;
  uint64_t bytes = 0;
  std::string detail;
};

struct ContentStatus {
  bool has_v4 = false;
  bool v4_enabled = false;
  bool has_rally = false;
  bool has_1000_club = false;
  bool has_udlc = false;
  bool has_xe = false;
  int dlc_installed_count = 0;
  int dlc_total_known = 21;
  std::string notice;
  std::vector<ContentPackage> packages;
};

// Checks if a file begins with Xbox 360 STFS container magic ("CON ", "LIVE", "PIRS").
bool is_stfs_package_file(const std::filesystem::path& file);
ContentStatus scan_content_packages(const std::filesystem::path& state_root,
                                    const std::filesystem::path& mods_root,
                                    const std::filesystem::path& play_root = {});
bool import_content_package(
    const std::filesystem::path& source_path,
    const std::filesystem::path& state_root,
    const std::filesystem::path& mods_root,
    std::string& out_message,
    const std::filesystem::path& play_root = {},
    const std::function<void(uint64_t bytes_done, uint64_t bytes_total, const std::string& current_file)>& progress_cb =
        {},
    const std::atomic<bool>* cancel = nullptr);

struct ContentInstallProgress {
  bool running = false;
  bool finished = false;
  bool success = false;
  int packages_done = 0;
  int packages_total = 0;
  float fraction = 0.0f;
  std::string status_label;
  std::string summary;
};

class ContentInstallRunner {
 public:
  ContentInstallRunner() = default;
  ~ContentInstallRunner();
  ContentInstallRunner(const ContentInstallRunner&) = delete;
  ContentInstallRunner& operator=(const ContentInstallRunner&) = delete;

  bool start(const std::vector<std::filesystem::path>& sources,
             const std::filesystem::path& state_root,
             const std::filesystem::path& mods_root,
             const std::filesystem::path& play_root = {});
  void cancel();
  ContentInstallProgress snapshot() const;

 private:
  mutable std::mutex mu_;
  ContentInstallProgress state_;
  std::atomic<bool> cancel_{false};
  std::thread worker_;
};

// ---- 6. Mod Manager (mod.toml + mod.txt compatible with Slice 13A) ---------

struct ModOptionChoice {
  std::string value;
  std::string label;
};

struct ModOption {
  std::string key;
  std::string title;
  std::string description;
  std::string value;
  std::vector<ModOptionChoice> choices;
};

struct ModEntry {
  std::string name;
  std::string title;
  std::string version;
  std::string author;
  std::string description;
  std::string url;
  std::string profile;       // non-empty -> own isolated save (e.g. "user-xe")
  std::filesystem::path dir;
  std::filesystem::path preview_image;
  bool enabled = true;
  bool shares_save = true;
  bool has_mod_toml = false;
  bool has_database_patch = false;
  int priority = 0;
  int file_count = 0;
  std::vector<std::string> contents;   // "cars", "sounds", "database", "textures", "radio", "shaders"
  std::vector<std::string> requires_mods;
  std::vector<std::string> conflicts;
  std::vector<std::string> problems;
  std::vector<ModOption> options;
};

struct ModSaveIsolation {
  std::string kind = "own";       // "own", "modded", "own-mod-save"
  std::string profile = "user";   // "user", "user-modded", or mod.profile
  std::string owning_mod;
  std::string warning_banner;
};

std::vector<ModEntry> scan_mods(const std::filesystem::path& mods_root);
ModSaveIsolation evaluate_save_isolation(const std::vector<ModEntry>& mods);
bool write_mod_state(const ModEntry& mod, std::string* error = nullptr);
bool toggle_mod_enabled(std::vector<ModEntry>& mods, size_t index, std::string* error = nullptr);
bool move_mod_order(std::vector<ModEntry>& mods, size_t index, int delta, std::string* error = nullptr);
bool set_mod_option_value(ModEntry& mod, const std::string& key, const std::string& value,
                          std::string* error = nullptr);
bool install_mod_source(const std::filesystem::path& source_folder_or_zip,
                        const std::filesystem::path& mods_root,
                        std::string& out_message);

// ---- 7. Custom Radio Station Builder ---------------------------------------

struct RadioTrackInfo {
  std::filesystem::path path;
  std::string title;
  std::string artist;
  uint64_t bytes = 0;
};

struct RadioBuilderConfig {
  std::string station_slot = "Radio1";    // "Radio1" (Bass Arena), "Radio2" (Pulse), "Radio3" (Rocks)
  std::string mod_name = "custom_radio";
  std::string custom_station_name;        // optional display name string override
  bool replace_playlist = true;           // true = replace, false = extend
  std::filesystem::path music_folder;
  std::filesystem::path logo_png_path;    // optional 210x100 PNG station logo
};

struct LogoValidation {
  bool valid = false;
  uint32_t width = 0;
  uint32_t height = 0;
  std::string message;
};

std::vector<RadioTrackInfo> scan_radio_music_folder(const std::filesystem::path& folder);
LogoValidation validate_station_logo_png(const std::filesystem::path& png_path);
std::string merge_radio_system_xml(const std::string& xml, const std::string& station_slot,
                                   const std::vector<std::tuple<std::string, std::string, std::string>>& tracks,
                                   bool replace, std::string* error = nullptr);
std::string merge_radio_soundbank_xml(const std::string& xml,
                                      const std::vector<std::tuple<std::string, int, uint32_t, uint32_t>>& entries,
                                      std::string* error = nullptr);

struct RadioBuildResult {
  bool ok = false;
  int tracks_encoded = 0;
  std::filesystem::path mod_dir;
  std::string message;
};
RadioBuildResult build_custom_radio_mod(const RadioBuilderConfig& cfg,
                                        const std::filesystem::path& game_root,
                                        const std::filesystem::path& mods_root,
                                        const std::filesystem::path& ffmpeg_bin,
                                        const std::function<void(const std::string&)>& log_cb = {});

// ---- 8. Wheel & Controller Configurator ------------------------------------

struct WheelDeviceInfo {
  int instance_id = 0;
  std::string name;
  int num_axes = 0;
  int num_buttons = 0;
  bool is_gamepad = false;
  bool has_ffb = false;
  std::vector<float> axis_values;  // -1..+1
};

struct WheelAxisBinding {
  std::string device_name;
  int axis_index = -1;
  bool inverted = false;
  float deadzone_in = 0.05f;
  float deadzone_out = 0.95f;
};

struct WheelBindingConfig {
  bool separate_devices = false;
  int steering_degrees = 900;
  WheelAxisBinding steer;
  WheelAxisBinding throttle;
  WheelAxisBinding brake;
  WheelAxisBinding clutch;
  WheelAxisBinding handbrake;
  int shift_up_button = -1;
  int shift_down_button = -1;
  int h_shifter_gears[7] = {-1, -1, -1, -1, -1, -1, -1};  // R, 1..6
  float ffb_gain = 0.85f;
  bool ffb_invert = false;
  float rumble_scale = 1.0f;
  int controller_layout = 0;
};

WheelBindingConfig load_wheel_settings(const std::filesystem::path& settings_file);
bool save_wheel_settings(const std::filesystem::path& settings_file, const WheelBindingConfig& cfg,
                         std::string* error = nullptr);

// ---- 9. Self-Updater (GitHub Releases + SHA-256 + Staged Swap) -------------

struct ReleaseVersion {
  int major = 0;
  int minor = 0;
  int patch = 0;
  std::string prerelease;
  std::string to_string() const;
};

bool parse_release_version(std::string_view text, ReleaseVersion& out);
int compare_release_version(const ReleaseVersion& a, const ReleaseVersion& b);

struct AvailableUpdate {
  std::string version;
  std::string tag;
  std::string release_url;
  std::string asset_name;
  std::string download_url;
  uint64_t size_bytes = 0;
  std::string sha256;
};

std::optional<AvailableUpdate> select_github_update(std::string_view releases_json,
                                                    std::string_view current_version,
                                                    std::string_view channel,
                                                    std::string_view platform_asset_name);

// Maps a recommended preset name ("high", "low", "smooth-60", "quality-30", "low-power")
// to the launcher preset selector index (0..4).
int preset_index_for_name(std::string_view preset);

struct UpdateCheckResult {
  bool ok = false;
  bool update_available = false;
  std::optional<AvailableUpdate> update;
  std::string status;
};

// Queries GitHub Releases API (with short timeout and safe offline fallback) and selects
// any matching release newer than `current_version`.
UpdateCheckResult check_github_updates(std::string_view current_version,
                                       std::string_view channel,
                                       std::string_view platform_asset_name,
                                       std::string_view repo = "shift-engine/shift-engine");

struct StagedUpdatePlan {
  std::filesystem::path staging_dir;
  std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moves;  // src -> dst
};
StagedUpdatePlan plan_staged_update(const std::filesystem::path& unpacked_dir,
                                    const std::filesystem::path& install_dir);
bool verify_update_sha256(const std::filesystem::path& file, std::string_view expected_hex,
                          std::string* actual_hex = nullptr);

// ---- 10. Android Wi-Fi Sync Server & QR Code -------------------------------

struct WifiSyncFile {
  std::filesystem::path source;
  std::string destination;  // relative to Android/data/net.shiftengine.drive/files/
  uint64_t size = 0;
  int64_t modified_ms = 0;
};

struct WifiSyncGroup {
  std::string id;
  std::string label;
  bool selected = true;
  uint64_t total_bytes = 0;
  std::vector<WifiSyncFile> files;
};

struct WifiSyncManifest {
  std::string host_name;
  std::string pairing_code;  // 6 digits
  std::string token;         // 16 hex chars
  std::filesystem::path apk_path;
  std::vector<WifiSyncGroup> groups;
  std::string to_json() const;
};

WifiSyncManifest build_wifi_sync_manifest(const LauncherPaths& paths,
                                          bool include_play = true,
                                          bool include_dlc = true,
                                          bool include_mods = true,
                                          bool include_radio = true,
                                          const std::filesystem::path& apk_path = {});

struct QrMatrix {
  int size = 0;
  std::vector<uint8_t> modules;  // size*size, 1 = dark, 0 = light
  bool at(int x, int y) const {
    return x >= 0 && y >= 0 && x < size && y < size && modules[size_t(y * size + x)] != 0;
  }
};

// Generates a real scannable QR code (Version 2-4, Byte mode, ECC Level L) for URLs up to 78 chars.
QrMatrix generate_qr_matrix(std::string_view text);

// Returns local private IPv4 LAN addresses (e.g. 192.168.x.x, 10.x.x.x).
std::vector<std::string> discover_local_lan_ips();

class WifiSyncServer {
 public:
  static constexpr uint16_t kPreferredPort = 47615;
  static constexpr uint16_t kDiscoveryPort = 47616;
  static constexpr int kMaxWrongCodes = 10;

  WifiSyncServer() = default;
  ~WifiSyncServer();
  WifiSyncServer(const WifiSyncServer&) = delete;
  WifiSyncServer& operator=(const WifiSyncServer&) = delete;

  bool start(const WifiSyncManifest& manifest, const std::string& bind_ip = "0.0.0.0",
             uint16_t port = kPreferredPort, bool enable_udp_discovery = true);
  void stop();

  bool running() const { return running_.load(); }
  uint16_t port() const { return port_.load(); }
  bool locked() const { return wrong_codes_.load() >= kMaxWrongCodes; }
  std::string pairing_code() const;
  std::string download_url(const std::string& host_ip) const;
  std::vector<std::string> recent_activity() const;

 private:
  void accept_loop();
  void discovery_loop();
  void handle_client(intptr_t client_sock, const std::string& remote_ip);
  void log_activity(const std::string& msg);

  WifiSyncManifest manifest_;
  std::vector<WifiSyncFile> flat_files_;
  std::atomic<bool> running_{false};
  std::atomic<uint16_t> port_{0};
  std::atomic<int> wrong_codes_{0};
  intptr_t listen_sock_ = -1;
  intptr_t udp_sock_ = -1;
  std::thread accept_thread_;
  std::thread discovery_thread_;
  mutable std::mutex log_mu_;
  std::vector<std::string> activity_;
};

}  // namespace launcher
