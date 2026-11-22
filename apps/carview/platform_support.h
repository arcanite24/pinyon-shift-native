// Cross-platform helpers for 4-platform parity (Windows, Linux/Steam Deck,
// macOS, Android):
// - Android graphics presets (smooth-60, quality-30, low-power)
// - Android Turnip custom Vulkan driver discovery & Vulkan 1.1 capability report
// - macOS Cmd keyboard shortcuts (Cmd+,, Ctrl+Cmd+F, Cmd+Shift+P, Cmd+T, Cmd+Q)
// - Portable-mode root resolution (portable.txt beside binary or macOS .app)
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace carview {

// ---- Graphics Presets (Desktop + Android) ----------------------------------

struct GraphicsPresetSpec {
  std::string name;
  uint32_t msaa = 4;
  bool low_preset = false;
  std::string shadows = "high";  // "off", "low", "high"
  bool bloom = true;
  float anisotropy = 16.0f;
  int target_fps = 60;
  float env_map_rate = 1.0f;     // 0.25 on Android presets
};

bool resolve_graphics_preset(std::string_view name, GraphicsPresetSpec& out);

// ---- Android Turnip Loader & Vulkan Capability Check -----------------------

struct AndroidDriverSelection {
  std::string mode;  // "auto", "system", or folder name
  std::filesystem::path driver_so;
  bool using_custom_driver = false;
  std::string summary;
};

AndroidDriverSelection resolve_android_gpu_driver(std::string_view preference,
                                                  const std::filesystem::path& drivers_dir,
                                                  std::string_view gpu_vendor_hint = {});

struct VulkanCapabilitySummary {
  uint32_t api_version = 0;
  bool has_vulkan_1_1 = false;
  bool has_vulkan_1_3 = false;
  bool has_dynamic_rendering = false;
  bool has_sync2 = false;
  bool has_portability_subset = false;
  bool eligible = false;
  std::string report_line;
};

VulkanCapabilitySummary evaluate_vulkan_capabilities(std::string_view device_name,
                                                     uint32_t api_version,
                                                     bool dynamic_rendering,
                                                     bool sync2,
                                                     bool portability_subset = false);

// ---- macOS Cmd Keyboard Shortcuts ------------------------------------------

enum class PlatformShortcut {
  kNone = 0,
  kSettings,          // Cmd + ,
  kToggleFullscreen,  // Ctrl + Cmd + F
  kPhotoMode,         // Cmd + Shift + P
  kTrainerOrTime,     // Cmd + T
  kQuit,              // Cmd + Q
};

// Matches macOS standard Command shortcuts using SDL keycode + SDL_Keymod bits.
// When `is_macos` is false, always returns kNone so desktop Ctrl/Win keys never collide.
PlatformShortcut match_macos_shortcut(uint32_t sdl_keycode, uint16_t sdl_keymod, bool is_macos);

// ---- Portable Mode Resolution for carview ----------------------------------

// Checks for portable.txt beside `exe_dir` (or beside the `.app` bundle on macOS).
// Returns `<anchor>/data` when portable mode is active, or empty path otherwise.
std::filesystem::path resolve_portable_data_root(const std::filesystem::path& exe_dir,
                                                 bool is_macos = false);

}  // namespace carview
