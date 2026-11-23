// On-device mod installer (.zip or directory, POSIX fd or path), load-order /
// enable-disable management (compatible with cars::modmgr), and Mesa Turnip
// custom Vulkan driver installer (.so or .zip).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "setup/stream_reader.h"

namespace setup::mods {

struct Progress {
  size_t files_done = 0;
  size_t files_total = 0;
  uint64_t bytes_done = 0;
  uint64_t bytes_total = 0;
  std::string current_file;
};

using ProgressCallback = std::function<void(const Progress&)>;

struct InstalledMod {
  std::string name;
  std::string title;
  std::string version;
  std::string author;
  std::string summary;
  bool enabled = true;
  int priority = 0;
  size_t order_index = 0;
  std::filesystem::path dir;
};

struct InstallModReport {
  bool ok = false;
  bool cancelled = false;
  std::string mod_name;
  std::filesystem::path mod_dir;
  size_t files_extracted = 0;
  uint64_t bytes_written = 0;
  std::string error;
};

struct TurnipInstallReport {
  bool ok = false;
  std::filesystem::path driver_path;
  std::string driver_name;
  uint64_t bytes_written = 0;
  std::string error;
};

bool install_mod_zip(const StreamReader& reader, const std::string& suggested_name,
                     const std::filesystem::path& mods_root, const ProgressCallback& progress,
                     const std::atomic<bool>* cancel, InstallModReport& report);

bool install_mod_zip_path(const std::filesystem::path& zip_path, const std::filesystem::path& mods_root,
                          const ProgressCallback& progress, const std::atomic<bool>* cancel, InstallModReport& report);

bool install_mod_zip_fd(int fd, bool take_ownership, const std::string& suggested_name,
                        const std::filesystem::path& mods_root, const ProgressCallback& progress,
                        const std::atomic<bool>* cancel, InstallModReport& report);

bool install_mod_directory(const std::filesystem::path& src_dir, const std::filesystem::path& mods_root,
                           InstallModReport& report);

std::vector<InstalledMod> list_mods(const std::filesystem::path& mods_root);
bool set_mod_enabled(const std::filesystem::path& mods_root, const std::string& mod_name, bool enabled,
                     std::string* error = nullptr);
// Moves a mod in mods.toml's load order (delta < 0 moves earlier/higher priority,
// delta > 0 moves later/lower priority).
bool move_mod(const std::filesystem::path& mods_root, const std::string& mod_name, int delta,
              std::string* error = nullptr);
bool remove_mod(const std::filesystem::path& mods_root, const std::string& mod_name, std::string* error = nullptr);

// Installs a Mesa Turnip Vulkan driver from a raw .so or a .zip release archive
// into <drivers_root>/turnip/libvulkan_freedreno.so.
bool install_turnip_driver(const StreamReader& reader, const std::string& filename,
                           const std::filesystem::path& drivers_root, TurnipInstallReport& report);
bool install_turnip_driver_path(const std::filesystem::path& path, const std::filesystem::path& drivers_root,
                                TurnipInstallReport& report);
bool install_turnip_driver_fd(int fd, bool take_ownership, const std::string& filename,
                              const std::filesystem::path& drivers_root, TurnipInstallReport& report);
std::filesystem::path find_installed_turnip_driver(const std::filesystem::path& drivers_root);

}  // namespace setup::mods
