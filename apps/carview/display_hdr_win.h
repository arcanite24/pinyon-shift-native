#pragma once

#include <filesystem>

namespace carview {

// Windows Auto-HDR helper: tracks HDR enablement made for this process and restores
// displays cleanly on exit or settings toggle.
void set_display_hdr_restore_file(const std::filesystem::path& path);
void turn_on_display_hdr();
void restore_display_hdr();

}  // namespace carview
