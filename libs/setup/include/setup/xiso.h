// Clean-room Xbox 360 GDFX / XISO disc image reader and streaming extractor.
// Supports trimmed XISO (0x0), XGD3 (0x02080000), XGD2 (0x0FD90000), and XGD1
// (0x18300000) game partition offsets directly from file paths or POSIX file
// descriptors (Android SAF).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "setup/stream_reader.h"

namespace setup::xiso {

inline constexpr uint32_t kSectorSize = 2048;
inline constexpr uint32_t kVolumeDescriptorSector = 32;
inline constexpr const char* kGdfxMagic = "MICROSOFT*XBOX*MEDIA";

struct PartitionInfo {
  uint64_t partition_offset = 0;
  uint32_t root_sector = 0;
  uint32_t root_size = 0;
  uint64_t creation_filetime = 0;
  std::string format_label;  // "XISO", "XGD3", "XGD2", "XGD1"
};

struct Entry {
  std::string path;        // normalized relative path using '/'
  uint64_t data_offset = 0;
  uint64_t size = 0;
  uint8_t attributes = 0;
  bool is_directory = false;
};

struct ExtractOptions {
  bool media_only = true;          // extract media/ tree (plus root fallback if no media/ dir exists)
  bool skip_xex = true;            // skip Xbox 360 executables (.xex / .xexp)
  bool overwrite_existing = false; // false = resume by skipping existing files of identical size
  size_t chunk_bytes = 1024 * 1024;
};

struct Progress {
  uint64_t bytes_done = 0;
  uint64_t bytes_total = 0;
  size_t files_done = 0;
  size_t files_total = 0;
  std::string current_file;
};

using ProgressCallback = std::function<void(const Progress&)>;

struct ExtractReport {
  bool ok = false;
  bool cancelled = false;
  PartitionInfo partition;
  size_t files_extracted = 0;
  size_t files_skipped = 0;
  uint64_t bytes_written = 0;
  std::string error;
};

struct DiscCheck {
  bool valid = false;
  std::filesystem::path resolved_root;
  bool has_colorado_bin = false;
  bool has_gamedb = false;
  size_t car_zip_count = 0;
  std::string summary;
};

bool detect_partition(const StreamReader& reader, PartitionInfo& out, std::string* error = nullptr);
bool list_entries(const StreamReader& reader, const PartitionInfo& partition, std::vector<Entry>& out,
                  std::string* error = nullptr);

bool extract_iso(const StreamReader& reader, const std::filesystem::path& out_dir, const ExtractOptions& opts,
                 const ProgressCallback& progress, const std::atomic<bool>* cancel, ExtractReport& report);

bool extract_iso_path(const std::filesystem::path& iso_path, const std::filesystem::path& out_dir,
                      const ExtractOptions& opts, const ProgressCallback& progress,
                      const std::atomic<bool>* cancel, ExtractReport& report);

bool extract_iso_fd(int fd, bool take_ownership, const std::filesystem::path& out_dir, const ExtractOptions& opts,
                    const ProgressCallback& progress, const std::atomic<bool>* cancel, ExtractReport& report);

// Verifies an extracted game folder (either parent of media/ or media/ itself).
DiscCheck inspect_disc_directory(const std::filesystem::path& game_root);

}  // namespace setup::xiso
