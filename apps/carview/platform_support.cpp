#include "platform_support.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <system_error>

namespace carview {
namespace {

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace

bool resolve_graphics_preset(std::string_view name, GraphicsPresetSpec& out) {
  const std::string p = lower_ascii(name);
  if (p == "ultra") {
    // Tier 3 Enthusiast 4K: 4x MSAA, Full-Res XeGTAO + Bent Normals, Full-Res 12-tap SSCS,
    // Hex-Tiling terrain, 13-tap McGuire Velocity Motion Blur, 3-Cascade D32 PCSS Ultra,
    // Full-Res Hi-Z SSR, Half-Res Volumetric Scattering, Close-Up Car Detail.
    out = {"ultra", 4, false, "high", true, 16.0f, 120, 1.0f,
           modern::SsaoQuality::kUltra, modern::ContactShadows::kUltra, true, modern::VelocityBlur::kHigh,
           modern::ShadowPcss::kUltra, modern::SsrQuality::kFull, modern::VolumetricFog::kHalf, true};
    return true;
  }
  if (p == "high" || p == "quality-60" || p == "performance-120") {
    // Tier 2 Mainstream: 4x MSAA, Half-Res 3-slice XeGTAO + Bent Normals, Half-Res 10-tap SSCS,
    // Hex-Tiling terrain, 9-tap McGuire Velocity Motion Blur, 3-Cascade D32 PCSS High,
    // Half-Res Hi-Z SSR, Quarter-Res Volumetric Scattering, Close-Up Car Detail.
    out = {p, 4, false, "high", true, 16.0f, p == "performance-120" ? 120 : 60, 1.0f,
           modern::SsaoQuality::kHigh, modern::ContactShadows::kHigh, true, modern::VelocityBlur::kLow,
           modern::ShadowPcss::kHigh, modern::SsrQuality::kHalf, modern::VolumetricFog::kQuarter, true};
    return true;
  }
  if (p == "low" || p == "low-spec-60" || p == "balanced-40") {
    // Tier 1 Deck / iGPU / Low: 1x MSAA, Half-Res 2-slice XeGTAO, Half-Res 8-tap SSCS,
    // fast 2-cascade PCF shadows, SSR/Volumetric/Hex-Tiling/VelBlur off.
    out = {p, 1, true, "low", false, 2.0f, p == "balanced-40" ? 40 : 60, 0.5f,
           modern::SsaoQuality::kLow, modern::ContactShadows::kLow, false, modern::VelocityBlur::kOff,
           modern::ShadowPcss::kLow, modern::SsrQuality::kOff, modern::VolumetricFog::kOff, false};
    return true;
  }
  if (p == "smooth-60" || p == "smooth_60") {
    // Android Smooth 60: 2x MSAA, shadows off, quarter-rate reflections, Half-Res 2-slice XeGTAO & 8-tap SSCS.
    out = {"smooth-60", 2, true, "off", false, 4.0f, 60, 0.25f,
           modern::SsaoQuality::kLow, modern::ContactShadows::kLow, false, modern::VelocityBlur::kOff,
           modern::ShadowPcss::kOff, modern::SsrQuality::kOff, modern::VolumetricFog::kOff, false};
    return true;
  }
  if (p == "quality-30" || p == "quality_30") {
    // Android Quality 30: 4x MSAA, shadows low, bloom on, quarter-rate reflections, Quarter-Res Volumetrics.
    out = {"quality-30", 4, false, "low", true, 8.0f, 30, 0.25f,
           modern::SsaoQuality::kLow, modern::ContactShadows::kLow, false, modern::VelocityBlur::kLow,
           modern::ShadowPcss::kLow, modern::SsrQuality::kOff, modern::VolumetricFog::kQuarter, false};
    return true;
  }
  if (p == "low-power" || p == "low_power" || p == "ultra-low-30") {
    // Tier 0 Mobile / Deck Low Power: 1x MSAA, shadows off, bloom off, all Phase A/B heavy passes off.
    out = {"low-power", 1, true, "off", false, 2.0f, 30, 0.25f,
           modern::SsaoQuality::kOff, modern::ContactShadows::kOff, false, modern::VelocityBlur::kOff,
           modern::ShadowPcss::kOff, modern::SsrQuality::kOff, modern::VolumetricFog::kOff, false};
    return true;
  }
  return false;
}

AndroidDriverSelection resolve_android_gpu_driver(std::string_view preference,
                                                  const std::filesystem::path& drivers_dir,
                                                  std::string_view gpu_vendor_hint) {
  AndroidDriverSelection sel;
  const std::string pref = lower_ascii(preference.empty() ? "auto" : preference);
  sel.mode = pref;
  if (pref == "system") {
    sel.summary = "Using Android system Vulkan driver.";
    return sel;
  }

  std::error_code ec;
  auto find_so_in = [&](const std::filesystem::path& dir) -> std::filesystem::path {
    if (!std::filesystem::is_directory(dir, ec)) return {};
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
      if (!e.is_regular_file(ec)) continue;
      if (e.path().extension() == ".so") return e.path();
    }
    return {};
  };

  if (pref != "auto") {
    // Explicit driver folder name or .so path
    std::filesystem::path direct(preference);
    if (std::filesystem::is_regular_file(direct, ec)) {
      sel.driver_so = direct;
      sel.using_custom_driver = true;
      sel.summary = "Using custom Vulkan driver: " + direct.filename().string();
      return sel;
    }
    if (const auto so = find_so_in(drivers_dir / std::string(preference)); !so.empty()) {
      sel.driver_so = so;
      sel.using_custom_driver = true;
      sel.summary = "Using custom Vulkan driver: " + std::string(preference) + " (" + so.filename().string() + ")";
      return sel;
    }
    sel.summary = "Requested driver '" + std::string(preference) + "' not found; falling back to system Vulkan.";
    return sel;
  }

  // AUTO: prefer bundled Turnip on Adreno 7xx/8xx (or when hint mentions adreno/qualcomm)
  const std::string hint = lower_ascii(gpu_vendor_hint);
  const bool prefer_turnip = hint.empty() || hint.find("adreno") != std::string::npos ||
                             hint.find("qualcomm") != std::string::npos;
  if (prefer_turnip && std::filesystem::is_directory(drivers_dir, ec)) {
    for (const auto& e : std::filesystem::directory_iterator(drivers_dir, ec)) {
      if (!e.is_directory(ec)) continue;
      if (lower_ascii(e.path().filename().string()).find("turnip") == std::string::npos) continue;
      if (const auto so = find_so_in(e.path()); !so.empty()) {
        sel.driver_so = so;
        sel.using_custom_driver = true;
        sel.summary = "AUTO selected bundled Mesa Turnip (" + e.path().filename().string() + ").";
        return sel;
      }
    }
  }
  sel.summary = "AUTO selected system Vulkan driver.";
  return sel;
}

VulkanCapabilitySummary evaluate_vulkan_capabilities(std::string_view device_name,
                                                     uint32_t api_version,
                                                     bool dynamic_rendering,
                                                     bool sync2,
                                                     bool portability_subset) {
  VulkanCapabilitySummary s;
  s.api_version = api_version;
  const uint32_t maj = (api_version >> 22) & 0x7Fu;
  const uint32_t min = (api_version >> 12) & 0x3FFu;
  s.has_vulkan_1_1 = (maj > 1) || (maj == 1 && min >= 1);
  s.has_vulkan_1_3 = (maj > 1) || (maj == 1 && min >= 3);
  s.has_dynamic_rendering = dynamic_rendering;
  s.has_sync2 = sync2;
  s.has_portability_subset = portability_subset;
  s.eligible = s.has_vulkan_1_1 && dynamic_rendering && sync2;

  char buf[256];
  std::snprintf(buf, sizeof buf,
                "VULKAN_CAPABILITY_REPORT: device=%.*s api=%u.%u dyn_render=%d sync2=%d portability=%d eligible=%d",
                int(device_name.size()), device_name.data(), maj, min,
                dynamic_rendering ? 1 : 0, sync2 ? 1 : 0, portability_subset ? 1 : 0, s.eligible ? 1 : 0);
  s.report_line = buf;
  return s;
}

PlatformShortcut match_macos_shortcut(uint32_t sdl_keycode, uint16_t sdl_keymod, bool is_macos) {
  if (!is_macos) return PlatformShortcut::kNone;
  const bool cmd = (sdl_keymod & SDL_KMOD_GUI) != 0;
  if (!cmd) return PlatformShortcut::kNone;
  const bool ctrl = (sdl_keymod & SDL_KMOD_CTRL) != 0;
  const bool shift = (sdl_keymod & SDL_KMOD_SHIFT) != 0;

  if (sdl_keycode == SDLK_COMMA && !ctrl && !shift) return PlatformShortcut::kSettings;
  if (sdl_keycode == SDLK_F && ctrl && !shift) return PlatformShortcut::kToggleFullscreen;
  if (sdl_keycode == SDLK_P && shift && !ctrl) return PlatformShortcut::kPhotoMode;
  if (sdl_keycode == SDLK_B && shift && !ctrl) return PlatformShortcut::kBugBookmark;
  if (sdl_keycode == SDLK_T && !ctrl && !shift) return PlatformShortcut::kTrainerOrTime;
  if (sdl_keycode == SDLK_Q && !ctrl && !shift) return PlatformShortcut::kQuit;
  return PlatformShortcut::kNone;
}

std::filesystem::path resolve_portable_data_root(const std::filesystem::path& exe_dir, bool is_macos) {
  std::error_code ec;
  if (exe_dir.empty()) return {};
  std::filesystem::path anchor = exe_dir;
  if (is_macos && exe_dir.filename() == "MacOS" && exe_dir.parent_path().filename() == "Contents") {
    anchor = exe_dir.parent_path().parent_path().parent_path();
  }
  if (std::filesystem::exists(anchor / "portable.txt", ec)) return anchor / "data";
  if (std::filesystem::exists(exe_dir / "portable.txt", ec)) return exe_dir / "data";
  return {};
}

std::filesystem::path resolve_default_save_path(const std::filesystem::path& portable_data_root,
                                                const std::filesystem::path& pref_drive_dir) {
  if (!portable_data_root.empty()) return portable_data_root / "drive" / "save.txt";
  if (!pref_drive_dir.empty()) return pref_drive_dir / "save.txt";
  return {};
}

}  // namespace carview
