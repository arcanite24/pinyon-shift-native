#include "display_hdr_win.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#endif

namespace carview {

#ifdef _WIN32
namespace {

constexpr UINT32 kGetAdvancedColorInfo = 9;
constexpr UINT32 kSetAdvancedColorState = 10;
constexpr UINT32 kGetAdvancedColorInfo2 = 15;
constexpr UINT32 kSetHdrState = 16;

struct AdvancedColorInfo {
  DISPLAYCONFIG_DEVICE_INFO_HEADER header;
  UINT32 value;
  UINT32 color_encoding;
  UINT32 bits_per_channel;
};

struct AdvancedColorInfo2 {
  DISPLAYCONFIG_DEVICE_INFO_HEADER header;
  UINT32 value;
  UINT32 color_encoding;
  UINT32 bits_per_channel;
  UINT32 active_color_mode;
};

struct SetAdvancedColorState {
  DISPLAYCONFIG_DEVICE_INFO_HEADER header;
  UINT32 enable_advanced_color;
};

struct TurnedOnTarget {
  LUID adapter;
  UINT32 id;
};

bool g_has_restore = false;
std::filesystem::path g_restore_file;
std::vector<TurnedOnTarget> g_turned_on;
bool g_exit_registered = false;

bool query_hdr(LUID adapter, UINT32 id, bool* supported, bool* enabled, bool* uses_2) {
  AdvancedColorInfo2 info2{};
  info2.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kGetAdvancedColorInfo2);
  info2.header.size = sizeof(info2);
  info2.header.adapterId = adapter;
  info2.header.id = id;
  if (DisplayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS) {
    *supported = (info2.value & 1u) != 0 || (info2.value & 16u) != 0;
    *enabled = (info2.value & 2u) != 0 || info2.active_color_mode == 2;
    *uses_2 = true;
    return true;
  }
  AdvancedColorInfo info{};
  info.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kGetAdvancedColorInfo);
  info.header.size = sizeof(info);
  info.header.adapterId = adapter;
  info.header.id = id;
  if (DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS) {
    *supported = (info.value & 1u) != 0;
    *enabled = (info.value & 2u) != 0;
    *uses_2 = false;
    return true;
  }
  return false;
}

bool set_hdr(LUID adapter, UINT32 id, bool enable, bool uses_2) {
  SetAdvancedColorState req{};
  req.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(uses_2 ? kSetHdrState : kSetAdvancedColorState);
  req.header.size = sizeof(req);
  req.header.adapterId = adapter;
  req.header.id = id;
  req.enable_advanced_color = enable ? 1u : 0u;
  if (DisplayConfigSetDeviceInfo(&req.header) == ERROR_SUCCESS) return true;
  if (uses_2) {
    req.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kSetAdvancedColorState);
    if (DisplayConfigSetDeviceInfo(&req.header) == ERROR_SUCCESS) return true;
  }
  return false;
}

void write_restore_file() {
  if (!g_has_restore || g_restore_file.empty()) return;
  if (g_turned_on.empty()) {
    std::error_code ec;
    std::filesystem::remove(g_restore_file, ec);
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(g_restore_file.parent_path(), ec);
  FILE* f = std::fopen(g_restore_file.string().c_str(), "wb");
  if (!f) return;
  std::fwrite(g_turned_on.data(), sizeof(TurnedOnTarget), g_turned_on.size(), f);
  std::fclose(f);
}

void on_exit_restore() { restore_display_hdr(); }

}  // namespace

void set_display_hdr_restore_file(const std::filesystem::path& path) {
  g_restore_file = path;
  g_has_restore = !path.empty();
  if (g_has_restore && std::filesystem::exists(g_restore_file)) {
    FILE* f = std::fopen(g_restore_file.string().c_str(), "rb");
    if (f) {
      TurnedOnTarget t{};
      while (std::fread(&t, sizeof(t), 1, f) == 1) g_turned_on.push_back(t);
      std::fclose(f);
    }
    restore_display_hdr();
  }
}

void turn_on_display_hdr() {
  if (!g_exit_registered) {
    std::atexit(on_exit_restore);
    g_exit_registered = true;
  }
  UINT32 path_count = 0, mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) !=
      ERROR_SUCCESS) {
    return;
  }
  for (UINT32 i = 0; i < path_count; ++i) {
    const LUID adapter = paths[i].targetInfo.adapterId;
    const UINT32 id = paths[i].targetInfo.id;
    bool supported = false, enabled = false, uses_2 = false;
    if (!query_hdr(adapter, id, &supported, &enabled, &uses_2) || !supported || enabled) continue;
    if (set_hdr(adapter, id, true, uses_2)) {
      g_turned_on.push_back({adapter, id});
    }
  }
  write_restore_file();
}

void restore_display_hdr() {
  if (g_turned_on.empty()) return;
  for (const auto& t : g_turned_on) {
    bool supported = false, enabled = false, uses_2 = false;
    if (query_hdr(t.adapter, t.id, &supported, &enabled, &uses_2) && enabled) {
      set_hdr(t.adapter, t.id, false, uses_2);
    }
  }
  g_turned_on.clear();
  write_restore_file();
}

#else
void set_display_hdr_restore_file(const std::filesystem::path&) {}
void turn_on_display_hdr() {}
void restore_display_hdr() {}
#endif

}  // namespace carview
