#include "upscaler.h"

#include <algorithm>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace carview {
namespace {

constexpr uint32_t kNvidiaVendorId = 0x10DE;

bool try_load_lib(const std::filesystem::path& p) {
  if (p.empty() || !std::filesystem::is_regular_file(p)) return false;
#ifdef _WIN32
  HMODULE h = LoadLibraryW(p.c_str());
  if (!h) return false;
  FreeLibrary(h);
  return true;
#else
  void* h = dlopen(p.c_str(), RTLD_LAZY | RTLD_LOCAL);
  if (!h) return false;
  dlclose(h);
  return true;
#endif
}

std::filesystem::path find_versioned_dll(const std::vector<std::filesystem::path>& roots, const char* subdir,
                                         const char* filename) {
  std::vector<std::pair<std::string, std::filesystem::path>> candidates;
  std::error_code ec;
  for (const auto& root : roots) {
    if (root.empty()) continue;
    const std::filesystem::path base = root / "upscalers" / subdir;
    if (!std::filesystem::is_directory(base, ec)) continue;
    const std::filesystem::path direct = base / filename;
    if (std::filesystem::is_regular_file(direct, ec)) candidates.emplace_back("", direct);
    for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
      if (!entry.is_directory(ec)) continue;
      const std::filesystem::path cand = entry.path() / filename;
      if (std::filesystem::is_regular_file(cand, ec)) {
        candidates.emplace_back(entry.path().filename().string(), cand);
      }
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  return candidates.empty() ? std::filesystem::path{} : candidates.front().second;
}

}  // namespace

UpscalerRuntimeInfo probe_upscaler_runtime(uint32_t gpu_vendor_id, const std::filesystem::path& exe_dir,
                                           const std::filesystem::path& state_dir) {
  UpscalerRuntimeInfo info;
  info.support.temporal = false;
  info.support.rtx = (gpu_vendor_id == kNvidiaVendorId);

  const std::vector<std::filesystem::path> roots = {exe_dir, state_dir, std::filesystem::current_path()};
#ifdef _WIN32
  const char* ffx_name = "amd_fidelityfx_vk.dll";
  const char* dlss_name = "nvngx_dlss.dll";
  const char* dlssg_name = "nvngx_dlssg.dll";
#else
  const char* ffx_name = "libamd_fidelityfx_vk.so";
  const char* dlss_name = "libnvidia-ngx-dlss.so";
  const char* dlssg_name = "libnvidia-ngx-dlssg.so";
#endif

  const std::filesystem::path ffx_path = find_versioned_dll(roots, "fidelityfx", ffx_name);
  if (try_load_lib(ffx_path)) {
    info.ffx_dll_path = ffx_path.string();
    info.support.temporal = true;
    info.support.frame_gen = true;
  }

  const std::filesystem::path dlss_path = find_versioned_dll(roots, "dlss", dlss_name);
  if (info.support.rtx && try_load_lib(dlss_path)) {
    info.dlss_dll_path = dlss_path.string();
    info.support.dlss_runtime = true;
    info.support.temporal = true;
  }

  const std::filesystem::path dlssg_path = find_versioned_dll(roots, "dlss", dlssg_name);
  if (info.support.dlss_runtime && try_load_lib(dlssg_path)) {
    info.dlssg_dll_path = dlssg_path.string();
    info.support.frame_gen = true;
  }
  return info;
}

}  // namespace carview
