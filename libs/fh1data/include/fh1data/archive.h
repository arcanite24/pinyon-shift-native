// FH1 ZIP archives (media/**/*.zip on the disc).
//
// An ordinary ZIP central directory whose members are stored (method 0),
// deflated (8) or XMem LZX compressed (21). Every member carries extra field
// 0x1123 in the central directory: the absolute offset of its payload. Some
// archives have no local headers at all, so the payload is located through
// that field (or, without it, the local header when one is present).
// Spec: site/findings/files.toml (FH1 ZIP records) in horizon1-recomp.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fh1 {

enum class ZipMethod : uint16_t { kStored = 0, kDeflate = 8, kXmem = 21 };

struct ArchiveMember {
  std::string name;              // as the directory spells it
  uint16_t method = 0;
  uint32_t crc32 = 0;            // of the decompressed bytes
  uint64_t compressed_size = 0;
  uint64_t size = 0;
  uint64_t header_offset = 0;    // the directory's local-header offset
  std::optional<uint64_t> payload_offset;  // extra field 0x1123, when present
};

struct ReadResult {
  std::vector<uint8_t> data;
  std::string error;  // empty on success
  int lzx_window_bits = 0;  // XMem members: the window that decoded it
  bool ok() const { return error.empty(); }
};

class Archive {
 public:
  static constexpr uint64_t kMaximumMemberSize = 256ull * 1024 * 1024;

  // Opens the archive and reads its central directory. The directory is
  // walked by its byte size: the end record's 16-bit member count wraps on
  // the largest archives (Colorado's bin.zip has 230,057 members).
  static std::unique_ptr<Archive> open(const std::filesystem::path& path, std::string* error);

  const std::vector<ArchiveMember>& members() const { return members_; }
  // Case-insensitive lookup, '/' and '\\' treated alike. Names can repeat
  // within an archive; this returns the first member with the name.
  const ArchiveMember* find(std::string_view name) const;

  // Reads, decompresses and CRC-checks one member. Not thread-safe.
  ReadResult read(const ArchiveMember& member);
  ReadResult read(std::string_view name);

  // Whether a local file header sits at the member's directory offset.
  bool has_local_header(const ArchiveMember& member);

  // Where the member's payload starts, as read() resolves it, or an error.
  std::optional<uint64_t> resolve_payload(const ArchiveMember& member, std::string* error);

  const std::filesystem::path& path() const { return path_; }
  uint64_t file_size() const { return file_size_; }

 private:
  bool read_range(uint64_t offset, size_t size, std::vector<uint8_t>& out);

  std::filesystem::path path_;
  std::ifstream stream_;
  uint64_t file_size_ = 0;
  std::vector<ArchiveMember> members_;
  std::unordered_map<std::string, size_t> index_;
};

uint32_t crc32(std::span<const uint8_t> data);
std::string normalize_member_name(std::string_view name);

// Mod overrides of archive members (cars::mod_merge installs one; none by
// default). `replace` supplies a member's whole bytes (a replacement, or a
// member the archive lacks: Archive::read(name) asks it before failing);
// `patch` edits the bytes read() decoded (XML/INI merges). Both are called
// with the archive's path and the member's name as the directory spells it,
// from any thread, so they must be thread-safe.
struct MemberOverride {
  std::function<bool(const std::filesystem::path& archive, std::string_view member, std::vector<uint8_t>& out)>
      replace;
  std::function<void(const std::filesystem::path& archive, std::string_view member, std::vector<uint8_t>& data)>
      patch;
};
// Installs (or, with null, removes) the override every Archive consults.
void set_member_override(std::shared_ptr<const MemberOverride> override_hooks);
std::shared_ptr<const MemberOverride> member_override();

}  // namespace fh1
