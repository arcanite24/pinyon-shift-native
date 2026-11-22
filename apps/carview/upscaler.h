#pragma once

#include <filesystem>
#include <string>

#include "render_modern.h"

namespace carview {

// Probes dynamic vendor upscaler runtimes under `upscalers/` (AMD FidelityFX
// and NVIDIA NGX DLSS / DLSS-G) and GPU capabilities without bundling binaries.
struct UpscalerRuntimeInfo {
  modern::UpscalerSupport support;
  std::string ffx_dll_path;
  std::string dlss_dll_path;
  std::string dlssg_dll_path;
};

UpscalerRuntimeInfo probe_upscaler_runtime(uint32_t gpu_vendor_id, const std::filesystem::path& exe_dir,
                                           const std::filesystem::path& state_dir = {});

}  // namespace carview
