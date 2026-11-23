#include "setup/stfs.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_set>

#include "cars/sources.h"
#include "fh1data/bytes.h"

namespace setup::stfs {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kBlocksPerL1 = kBlocksPerHashTable * kBlocksPerHashTable;  // 28900 (0x70E4)
constexpr uint32_t kEndBlock = 0xFFFFFFu;
constexpr uint32_t kHashEntrySize = 24;
constexpr size_t kFileEntrySize = 64;

uint32_t load_le24(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
}

uint32_t load_be24(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | static_cast<uint32_t>(p[2]);
}

uint64_t load_be64_local(const uint8_t* p) {
  return (static_cast<uint64_t>(fh1::load_be32(p)) << 32) | static_cast<uint64_t>(fh1::load_be32(p + 4));
}

std::string hex_upper(const uint8_t* bytes, size_t len) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kHex[(bytes[i] >> 4) & 0x0Fu]);
    out.push_back(kHex[bytes[i] & 0x0Fu]);
  }
  return out;
}

std::string decode_utf16be(const uint8_t* bytes, size_t max_bytes) {
  std::string out;
  for (size_t i = 0; i + 1 < max_bytes; i += 2) {
    const uint16_t cp = fh1::load_be16(bytes + i);
    if (cp == 0) break;
    if (cp < 0x80u) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
      out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
  }
  return out;
}

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20u) out.push_back(c);
        break;
    }
  }
  return out;
}

// Converts logical STFS data block number to backing block index (0-based from
// Header::data_offset).
uint32_t data_block_to_backing(const Header& h, uint32_t block) {
  const uint32_t shift = h.read_only ? 0u : 1u;
  uint32_t backing = block + (((block + kBlocksPerHashTable) / kBlocksPerHashTable) << shift);
  if (block < kBlocksPerHashTable) return backing;
  backing += ((block / kBlocksPerL1) + 1u) << shift;
  if (block < kBlocksPerL1) return backing;
  backing += 1u << shift;
  return backing;
}

// Backing block index of the L0 hash table covering `data_block`.
uint32_t l0_table_backing(const Header& h, uint32_t data_block) {
  const uint32_t shift = h.read_only ? 0u : 1u;
  const uint32_t group_start = (data_block / kBlocksPerHashTable) * kBlocksPerHashTable;
  return data_block_to_backing(h, group_start) - (1u << shift);
}

// Reads the 24-byte hash entry for `data_block` from its L0 hash table to
// obtain the next block in a non-contiguous block chain.
bool read_next_block(const StreamReader& reader, const Header& h, uint32_t data_block, uint32_t& next_block,
                     std::string* error) {
  uint32_t table_backing = l0_table_backing(h, data_block);
  if (!h.read_only && h.allocated_block_count <= kBlocksPerHashTable && h.root_active_index) {
    table_backing += 1u;
  }
  const uint32_t entry_idx = data_block % kBlocksPerHashTable;
  const uint64_t entry_off =
      h.data_offset + static_cast<uint64_t>(table_backing) * kBlockSize + static_cast<uint64_t>(entry_idx) * kHashEntrySize;
  uint8_t entry[kHashEntrySize]{};
  if (!reader.read_at(entry_off, entry, sizeof(entry), error)) return false;
  next_block = load_be24(entry + 21);
  return true;
}

bool read_block_chain(const StreamReader& reader, const Header& h, uint32_t start_block, uint32_t block_count,
                      bool contiguous, uint64_t byte_limit, std::vector<uint8_t>& out, std::string* error) {
  out.clear();
  if (byte_limit == 0 || block_count == 0) return true;
  out.reserve(static_cast<size_t>(byte_limit));

  uint32_t cur = start_block;
  uint64_t remaining = byte_limit;
  std::unordered_set<uint32_t> visited;
  std::array<uint8_t, kBlockSize> block_buf{};

  for (uint32_t i = 0; i < block_count && remaining > 0; ++i) {
    if (cur >= kEndBlock) {
      if (error) *error = "STFS block chain ended early";
      return false;
    }
    if (!visited.insert(cur).second) {
      if (error) *error = "STFS block chain contains a cycle";
      return false;
    }
    const uint32_t backing = data_block_to_backing(h, cur);
    const uint64_t off = h.data_offset + static_cast<uint64_t>(backing) * kBlockSize;
    const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, kBlockSize));
    if (!reader.read_at(off, block_buf.data(), want, error)) return false;
    out.insert(out.end(), block_buf.begin(), block_buf.begin() + static_cast<std::ptrdiff_t>(want));
    remaining -= want;
    if (remaining == 0 || i + 1 >= block_count) break;
    if (contiguous) {
      ++cur;
    } else {
      if (!read_next_block(reader, h, cur, cur, error)) return false;
    }
  }
  return true;
}

PackageKind classify_package(const Header& h, const std::vector<FileEntry>& files) {
  if (h.content_type == kContentTypeTitleUpdate) return PackageKind::kTitleUpdate;
  const std::string name_l = lower_ascii(h.display_name);
  if (name_l.find("rally") != std::string::npos) return PackageKind::kRallyExpansion;
  if (name_l.find("1000") != std::string::npos || name_l.find("club") != std::string::npos) {
    return PackageKind::kClub1000;
  }
  for (const auto& f : files) {
    const std::string p = lower_ascii(f.path);
    if (p.find("1600.puboffer") != std::string::npos || p.find("coloradodirt") != std::string::npos) {
      return PackageKind::kRallyExpansion;
    }
    if (p.find("club") != std::string::npos && p.find(".xml") != std::string::npos) {
      return PackageKind::kClub1000;
    }
  }
  if (h.content_type == kContentTypeMarketplace) return PackageKind::kDlc;
  return PackageKind::kDlc;
}

std::string sanitize_package_id(std::string_view raw) {
  std::string out;
  for (char c : raw) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') out.push_back(c);
  }
  return out;
}

bool is_all_zero_hex(std::string_view s) {
  for (char c : s) {
    if (c != '0') return false;
  }
  return true;
}

}  // namespace

const char* package_kind_name(PackageKind kind) {
  switch (kind) {
    case PackageKind::kTitleUpdate: return "title_update";
    case PackageKind::kDlc: return "dlc";
    case PackageKind::kRallyExpansion: return "rally_expansion";
    case PackageKind::kClub1000: return "club_1000";
    case PackageKind::kUnknown: return "unknown";
  }
  return "unknown";
}

bool is_stfs_magic(const uint8_t bytes[4]) {
  return std::memcmp(bytes, "CON ", 4) == 0 || std::memcmp(bytes, "LIVE", 4) == 0 ||
         std::memcmp(bytes, "PIRS", 4) == 0;
}

bool parse_header(const StreamReader& reader, Header& out, std::string* error) {
  out = Header{};
  if (reader.size() < 0x1711u) {
    if (error) *error = "file too small for an STFS container";
    return false;
  }

  std::vector<uint8_t> hdr;
  if (!reader.read_at(0, 0x1711u, hdr, error)) return false;
  if (!is_stfs_magic(hdr.data())) {
    if (error) *error = "not an STFS package (expected CON, LIVE, or PIRS magic)";
    return false;
  }

  out.magic.assign(reinterpret_cast<const char*>(hdr.data()), 4);
  out.content_id_hex = hex_upper(hdr.data() + 0x032C, 20);
  out.header_size = fh1::load_be32(hdr.data() + 0x0340);
  out.content_type = fh1::load_be32(hdr.data() + 0x0344);
  out.metadata_version = fh1::load_be32(hdr.data() + 0x0348);
  out.content_size = load_be64_local(hdr.data() + 0x034C);
  out.version = fh1::load_be32(hdr.data() + 0x0358);
  out.base_version = fh1::load_be32(hdr.data() + 0x035C);
  out.title_id = fh1::load_be32(hdr.data() + 0x0360);

  // STFS volume descriptor at 0x0379 (0x24 bytes).
  const uint8_t block_sep = hdr[0x037B];
  out.read_only = (block_sep & 0x01u) != 0;
  out.root_active_index = (block_sep & 0x02u) != 0;
  out.file_table_block_count = fh1::load_le16(hdr.data() + 0x037C);
  out.file_table_block_number = load_le24(hdr.data() + 0x037E);
  out.allocated_block_count = fh1::load_be32(hdr.data() + 0x0395);
  out.unallocated_block_count = fh1::load_be32(hdr.data() + 0x0399);

  const uint32_t descriptor_type = fh1::load_be32(hdr.data() + 0x03A9);
  if (descriptor_type != 0u) {
    if (error) *error = "SVOD containers are not supported (expected STFS volume descriptor)";
    return false;
  }

  // Round header_size up to a 0x1000 page boundary (standard 0x971A -> 0xA000, 0xAD0E -> 0xB000).
  uint32_t aligned = (out.header_size + 0xFFFu) & ~0xFFFu;
  if (aligned < 0xA000u) aligned = 0xA000u;
  out.data_offset = aligned;
  if (out.data_offset > reader.size()) {
    if (error) *error = "STFS header size exceeds container length";
    return false;
  }

  out.display_name = decode_utf16be(hdr.data() + 0x0411, 0x80u);
  out.title_name = decode_utf16be(hdr.data() + 0x1691, 0x80u);
  return true;
}

bool list_files(const StreamReader& reader, const Header& header, std::vector<FileEntry>& out, std::string* error) {
  out.clear();
  if (header.file_table_block_count == 0) return true;

  std::vector<uint8_t> table;
  const uint64_t table_bytes = static_cast<uint64_t>(header.file_table_block_count) * kBlockSize;
  // File table blocks can be chained via L0 hash table (not necessarily contiguous).
  if (!read_block_chain(reader, header, header.file_table_block_number, header.file_table_block_count, false,
                        table_bytes, table, error)) {
    return false;
  }

  struct RawEntry {
    std::string name;
    uint32_t starting_block = 0;
    uint32_t valid_blocks = 0;
    uint32_t file_size = 0;
    int16_t parent_index = -1;
    bool is_directory = false;
    bool contiguous = false;
    bool valid = false;
  };

  const size_t count = table.size() / kFileEntrySize;
  std::vector<RawEntry> raw(count);

  for (size_t i = 0; i < count; ++i) {
    const uint8_t* p = table.data() + i * kFileEntrySize;
    const uint8_t flags = p[0x28];
    const uint8_t name_len = flags & 0x3Fu;
    if (name_len == 0 || name_len > 40u) continue;
    RawEntry e;
    e.name.assign(reinterpret_cast<const char*>(p), name_len);
    e.valid_blocks = load_le24(p + 0x29);
    e.starting_block = load_le24(p + 0x2F);
    e.parent_index = static_cast<int16_t>(fh1::load_be16(p + 0x32));
    e.file_size = fh1::load_be32(p + 0x34);
    e.contiguous = (flags & 0x40u) != 0;
    e.is_directory = (flags & 0x80u) != 0;
    e.valid = true;
    raw[i] = std::move(e);
  }

  for (size_t i = 0; i < raw.size(); ++i) {
    if (!raw[i].valid) continue;
    std::vector<std::string> parts;
    parts.push_back(raw[i].name);
    int16_t parent = raw[i].parent_index;
    int guard = 0;
    while (parent != -1 && guard < 64) {
      if (parent < 0 || static_cast<size_t>(parent) >= raw.size() || !raw[static_cast<size_t>(parent)].valid) {
        if (error) *error = "STFS file entry has invalid parent directory index";
        return false;
      }
      parts.push_back(raw[static_cast<size_t>(parent)].name);
      parent = raw[static_cast<size_t>(parent)].parent_index;
      ++guard;
    }
    if (guard >= 64) {
      if (error) *error = "STFS directory hierarchy contains a cycle";
      return false;
    }
    std::reverse(parts.begin(), parts.end());
    std::string joined;
    for (size_t k = 0; k < parts.size(); ++k) {
      if (k > 0) joined.push_back('/');
      joined += parts[k];
    }
    const std::string clean = sanitize_relative_path(joined);
    if (clean.empty()) {
      if (error) *error = "STFS contains unsafe entry path: " + joined;
      return false;
    }

    FileEntry fe;
    fe.path = clean;
    fe.name = raw[i].name;
    fe.starting_block = raw[i].starting_block;
    fe.valid_blocks = raw[i].valid_blocks;
    fe.file_size = raw[i].file_size;
    fe.parent_index = raw[i].parent_index;
    fe.is_directory = raw[i].is_directory;
    fe.contiguous = raw[i].contiguous;
    out.push_back(std::move(fe));
  }

  return true;
}

bool extract_file(const StreamReader& reader, const Header& header, const FileEntry& entry, std::vector<uint8_t>& out,
                  std::string* error) {
  if (entry.is_directory) {
    out.clear();
    return true;
  }
  const uint32_t needed_blocks = entry.valid_blocks > 0
                                     ? entry.valid_blocks
                                     : static_cast<uint32_t>((static_cast<uint64_t>(entry.file_size) + kBlockSize - 1u) / kBlockSize);
  return read_block_chain(reader, header, entry.starting_block, needed_blocks, entry.contiguous, entry.file_size, out,
                          error);
}

bool install_package(const StreamReader& reader, const InstallTargets& targets, const ProgressCallback& progress,
                     const std::atomic<bool>* cancel, InstallReport& report) {
  report = InstallReport{};
  std::string err;
  if (!parse_header(reader, report.header, &err)) {
    report.error = err;
    return false;
  }

  std::vector<FileEntry> entries;
  if (!list_files(reader, report.header, entries, &err)) {
    report.error = err;
    return false;
  }

  report.kind = classify_package(report.header, entries);
  report.display_name = !report.header.display_name.empty() ? report.header.display_name : targets.suggested_name;

  std::string pkg_id;
  if (!report.header.content_id_hex.empty() && !is_all_zero_hex(report.header.content_id_hex)) {
    pkg_id = report.header.content_id_hex;
  } else {
    pkg_id = sanitize_package_id(targets.suggested_name);
    if (pkg_id.empty()) pkg_id = "STFS_PACKAGE";
  }
  report.package_id = pkg_id;
  if (report.display_name.empty()) report.display_name = pkg_id;

  const fs::path state_root = !targets.state_dir.empty() ? targets.state_dir : targets.play_dir;
  const bool is_tu = (report.kind == PackageKind::kTitleUpdate);

  fs::path dest_dir;
  if (is_tu) {
    dest_dir = !targets.state_dir.empty() ? (targets.state_dir / "title-update-v4")
                                          : (targets.play_dir / "updates" / pkg_id);
  } else {
    dest_dir = !targets.state_dir.empty()
                   ? (targets.state_dir / "user" / "0000000000000000" / "4D5309C9" / "00000002" / pkg_id)
                   : (targets.play_dir / "dlc" / pkg_id);
  }
  report.installed_dir = dest_dir;

  std::error_code ec;
  fs::create_directories(dest_dir, ec);
  if (ec) {
    report.error = "cannot create package directory: " + ec.message();
    return false;
  }

  std::vector<const FileEntry*> files;
  uint64_t total_bytes = 0;
  for (const auto& e : entries) {
    if (e.is_directory) continue;
    files.push_back(&e);
    total_bytes += e.file_size;
  }

  uint64_t bytes_done = 0;
  size_t files_done = 0;
  std::vector<uint8_t> data;

  for (const FileEntry* e : files) {
    if (cancel && cancel->load(std::memory_order_relaxed)) {
      report.cancelled = true;
      report.error = "STFS install cancelled";
      return false;
    }

    if (!extract_file(reader, report.header, *e, data, &err)) {
      report.error = "failed extracting " + e->path + ": " + err;
      return false;
    }

    // For Title Update v4, normalize any media.zip at root or Media/media.zip so
    // cars::discover_install finds <state_root>/title-update-v4/media.zip directly.
    fs::path out_path = dest_dir / fs::path(e->path);
    if (is_tu && (lower_ascii(e->path) == "media.zip" || lower_ascii(e->path) == "media/media.zip")) {
      out_path = dest_dir / "media.zip";
    }

    fs::create_directories(out_path.parent_path(), ec);
    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) {
      report.error = "cannot write " + out_path.string();
      return false;
    }
    if (!data.empty()) {
      f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
      if (!f) {
        report.error = "write error on " + out_path.string();
        return false;
      }
    }
    f.close();

    bytes_done += data.size();
    report.bytes_written += data.size();
    ++files_done;
    ++report.files_extracted;
    if (progress) progress({bytes_done, total_bytes, files_done, files.size(), e->path});
  }

  // Write STFS header file and metadata JSON so cars::discover_install and
  // cars::list_packs recognize ownership, display_name, and license_mask.
  const std::string meta_json =
      "{\n"
      "  \"package_id\": \"" + json_escape(pkg_id) + "\",\n"
      "  \"display_name\": \"" + json_escape(report.display_name) + "\",\n"
      "  \"kind\": \"" + std::string(package_kind_name(report.kind)) + "\",\n"
      "  \"license_mask\": \"FFFFFFFF\",\n"
      "  \"files\": " + std::to_string(report.files_extracted) + "\n"
      "}\n";

  if (!targets.state_dir.empty()) {
    if (!is_tu) {
      const fs::path headers_dir =
          targets.state_dir / "user" / "0000000000000000" / "4D5309C9" / "Headers" / "00000002";
      fs::create_directories(headers_dir, ec);
      std::vector<uint8_t> raw_header;
      const size_t hdr_len = static_cast<size_t>(std::min<uint64_t>(report.header.data_offset, reader.size()));
      if (reader.read_at(0, hdr_len, raw_header, nullptr)) {
        std::ofstream hf(headers_dir / (pkg_id + ".header"), std::ios::binary | std::ios::trunc);
        hf.write(reinterpret_cast<const char*>(raw_header.data()), static_cast<std::streamsize>(raw_header.size()));
      }
      const fs::path dlc_meta_dir = targets.state_dir / "dlc";
      fs::create_directories(dlc_meta_dir, ec);
      std::ofstream mf(dlc_meta_dir / (pkg_id + ".json"), std::ios::binary | std::ios::trunc);
      mf << meta_json;
    } else {
      std::ofstream mf(dest_dir / "update.json", std::ios::binary | std::ios::trunc);
      mf << meta_json;
    }
  }

  // Mirror compact summary record into play/updates or play/dlc so the play
  // folder also records installed add-ons cleanly.
  if (!targets.play_dir.empty()) {
    const fs::path play_sub = targets.play_dir / (is_tu ? "updates" : "dlc");
    fs::create_directories(play_sub, ec);
    std::ofstream pf(play_sub / (pkg_id + ".json"), std::ios::binary | std::ios::trunc);
    pf << meta_json;
  }

  report.ok = true;
  return true;
}

bool install_package_path(const fs::path& path, const InstallTargets& targets, const ProgressCallback& progress,
                          const std::atomic<bool>* cancel, InstallReport& report) {
  std::string err;
  auto reader = StreamReader::open_path(path, &err);
  if (!reader) {
    report = InstallReport{};
    report.error = err;
    return false;
  }
  InstallTargets t = targets;
  if (t.suggested_name.empty()) t.suggested_name = path.stem().string();
  return install_package(*reader, t, progress, cancel, report);
}

bool install_package_fd(int fd, bool take_ownership, const InstallTargets& targets, const ProgressCallback& progress,
                        const std::atomic<bool>* cancel, InstallReport& report) {
  std::string err;
  auto reader = StreamReader::from_fd(fd, take_ownership, &err);
  if (!reader) {
    report = InstallReport{};
    report.error = err;
    return false;
  }
  return install_package(*reader, targets, progress, cancel, report);
}

std::vector<InstalledPackage> list_installed_packages(const fs::path& state_dir, const fs::path& play_dir) {
  std::vector<InstalledPackage> out;
  std::error_code ec;

  // Title Update v4
  if (!state_dir.empty() && fs::is_regular_file(state_dir / "title-update-v4" / "media.zip", ec)) {
    InstalledPackage tu;
    tu.package_id = "title-update-v4";
    tu.display_name = "Title Update v4";
    tu.kind = PackageKind::kTitleUpdate;
    tu.enabled = true;
    tu.has_header = true;
    tu.path = state_dir / "title-update-v4";
    out.push_back(std::move(tu));
  } else if (!play_dir.empty() && fs::is_directory(play_dir / "updates", ec)) {
    for (const auto& e : fs::directory_iterator(play_dir / "updates", ec)) {
      if (!e.is_regular_file(ec) && !e.is_directory(ec)) continue;
      InstalledPackage tu;
      tu.package_id = e.path().stem().string();
      tu.display_name = "Title Update (" + tu.package_id + ")";
      tu.kind = PackageKind::kTitleUpdate;
      tu.enabled = true;
      tu.has_header = true;
      tu.path = e.path();
      out.push_back(std::move(tu));
      break;
    }
  }

  // DLC packs via cars::list_packs (keeps 100% parity with runtime discovery)
  if (!state_dir.empty()) {
    for (const auto& p : cars::list_packs(state_dir)) {
      InstalledPackage ip;
      ip.package_id = p.id;
      ip.display_name = p.name;
      ip.enabled = p.enabled;
      ip.has_header = p.header;
      ip.puboffer = p.puboffer;
      ip.path = state_dir / "user" / "0000000000000000" / "4D5309C9" / "00000002" / p.id;
      const std::string nl = lower_ascii(p.name);
      if (p.puboffer == 1600 || nl.find("rally") != std::string::npos) {
        ip.kind = PackageKind::kRallyExpansion;
      } else if (nl.find("1000") != std::string::npos || nl.find("club") != std::string::npos) {
        ip.kind = PackageKind::kClub1000;
      } else {
        ip.kind = PackageKind::kDlc;
      }
      out.push_back(std::move(ip));
    }
  }
  return out;
}

}  // namespace setup::stfs
