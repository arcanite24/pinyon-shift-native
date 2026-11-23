// Clean-room Xbox 360 STFS (CON / LIVE / PIRS) package reader and on-device
// installer for Title Update v4, Rally Expansion, 1000 Club, and DLC packs.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "setup/stream_reader.h"

namespace setup::stfs {

inline constexpr uint32_t kBlockSize = 0x1000;
inline constexpr uint32_t kBlocksPerHashTable = 0xAA;  // 170
inline constexpr uint32_t kContentTypeMarketplace = 0x00000002u;
inline constexpr uint32_t kContentTypeTitleUpdate = 0x000B0000u;
inline constexpr uint32_t kContentTypeSavedGame = 0x00000001u;
inline constexpr uint32_t kFh1TitleId = 0x4D5309C9u;

struct Header {
  std::string magic;         // "CON ", "LIVE", "PIRS"
  uint32_t header_size = 0;
  uint64_t data_offset = 0;  // page-aligned start of STFS backing blocks
  uint32_t content_type = 0;
  uint32_t metadata_version = 0;
  uint64_t content_size = 0;
  uint32_t title_id = 0;
  uint32_t version = 0;
  uint32_t base_version = 0;
  bool read_only = true;
  bool root_active_index = false;
  uint16_t file_table_block_count = 0;
  uint32_t file_table_block_number = 0;
  uint32_t allocated_block_count = 0;
  uint32_t unallocated_block_count = 0;
  std::string content_id_hex;  // 40-char uppercase hex
  std::string display_name;
  std::string title_name;
};

struct FileEntry {
  std::string path;  // normalized relative path using '/'
  std::string name;
  uint32_t starting_block = 0;
  uint32_t valid_blocks = 0;
  uint32_t file_size = 0;
  int16_t parent_index = -1;
  bool is_directory = false;
  bool contiguous = false;
};

struct Progress {
  uint64_t bytes_done = 0;
  uint64_t bytes_total = 0;
  size_t files_done = 0;
  size_t files_total = 0;
  std::string current_file;
};

using ProgressCallback = std::function<void(const Progress&)>;

struct InstallTargets {
  std::filesystem::path play_dir;   // writes summary records in play/dlc or play/updates
  std::filesystem::path state_dir;  // writes title-update-v4/ and user/.../00000002/<id>/
  std::string suggested_name;       // fallback package id if header content_id is empty
};

enum class PackageKind { kUnknown, kTitleUpdate, kDlc, kRallyExpansion, kClub1000 };

struct InstallReport {
  bool ok = false;
  bool cancelled = false;
  Header header;
  PackageKind kind = PackageKind::kUnknown;
  std::string package_id;
  std::string display_name;
  std::filesystem::path installed_dir;
  size_t files_extracted = 0;
  uint64_t bytes_written = 0;
  std::string error;
};

struct InstalledPackage {
  std::string package_id;
  std::string display_name;
  PackageKind kind = PackageKind::kUnknown;
  bool enabled = true;
  bool has_header = false;
  int64_t puboffer = -1;
  std::filesystem::path path;
};

bool is_stfs_magic(const uint8_t bytes[4]);
bool parse_header(const StreamReader& reader, Header& out, std::string* error = nullptr);
bool list_files(const StreamReader& reader, const Header& header, std::vector<FileEntry>& out,
                std::string* error = nullptr);
bool extract_file(const StreamReader& reader, const Header& header, const FileEntry& entry,
                  std::vector<uint8_t>& out, std::string* error = nullptr);

bool install_package(const StreamReader& reader, const InstallTargets& targets, const ProgressCallback& progress,
                     const std::atomic<bool>* cancel, InstallReport& report);

bool install_package_path(const std::filesystem::path& path, const InstallTargets& targets,
                          const ProgressCallback& progress, const std::atomic<bool>* cancel, InstallReport& report);

bool install_package_fd(int fd, bool take_ownership, const InstallTargets& targets, const ProgressCallback& progress,
                        const std::atomic<bool>* cancel, InstallReport& report);

std::vector<InstalledPackage> list_installed_packages(const std::filesystem::path& state_dir,
                                                      const std::filesystem::path& play_dir = {});

const char* package_kind_name(PackageKind kind);

}  // namespace setup::stfs
