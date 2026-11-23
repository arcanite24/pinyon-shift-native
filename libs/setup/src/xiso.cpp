#include "setup/xiso.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <unordered_set>

#include "fh1data/bytes.h"

namespace setup::xiso {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMagicLen = 20;
constexpr uint8_t kAttrDirectory = 0x10;
constexpr uint32_t kMaxDirectoryBytes = 64u * 1024u * 1024u;
constexpr int kMaxTreeDepth = 64;

struct CandidateOffset {
  uint64_t offset;
  const char* label;
};

constexpr std::array<CandidateOffset, 4> kStandardPartitions = {{
    {0x00000000ULL, "XISO"},
    {0x02080000ULL, "XGD3"},
    {0x0FD90000ULL, "XGD2"},
    {0x18300000ULL, "XGD1"},
}};

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
  if (s.size() < suffix.size()) return false;
  const size_t off = s.size() - suffix.size();
  for (size_t i = 0; i < suffix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[off + i])) !=
        std::tolower(static_cast<unsigned char>(suffix[i]))) {
      return false;
    }
  }
  return true;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

struct DirTask {
  std::string prefix;
  uint32_t sector = 0;
  uint32_t size = 0;
  int depth = 0;
};

bool walk_directory_buffer(const std::vector<uint8_t>& buf, const std::string& prefix, uint64_t partition_offset,
                           uint64_t stream_size, int depth, std::vector<Entry>& out, std::vector<DirTask>& subdirs,
                           std::string* error) {
  if (buf.empty()) return true;
  if (depth > kMaxTreeDepth) {
    if (error) *error = "XISO directory nesting exceeded safety limit";
    return false;
  }

  std::vector<uint32_t> stack;
  std::unordered_set<uint32_t> visited;
  stack.push_back(0);

  while (!stack.empty()) {
    const uint32_t pos = stack.back();
    stack.pop_back();
    if (!visited.insert(pos).second) {
      if (error) *error = "XISO directory tree contains a cycle";
      return false;
    }
    if (pos + 14u > buf.size()) {
      if (error) *error = "XISO directory entry truncated";
      return false;
    }

    const uint8_t* p = buf.data() + pos;
    const uint16_t left_word = fh1::load_le16(p + 0);
    const uint16_t right_word = fh1::load_le16(p + 2);
    const uint32_t first_sector = fh1::load_le32(p + 4);
    const uint32_t file_size = fh1::load_le32(p + 8);
    const uint8_t attrs = p[12];
    const uint8_t name_len = p[13];

    if (pos + 14u + name_len > buf.size()) {
      if (error) *error = "XISO directory entry name extends past directory table";
      return false;
    }

    // Push right then left so traversal visits left-subtree before right-subtree.
    if (right_word != 0xFFFFu && right_word != 0u) {
      const uint32_t right_off = static_cast<uint32_t>(right_word) * 4u;
      if (right_off < buf.size()) stack.push_back(right_off);
    }
    if (left_word != 0xFFFFu && left_word != 0u) {
      const uint32_t left_off = static_cast<uint32_t>(left_word) * 4u;
      if (left_off < buf.size()) stack.push_back(left_off);
    }

    if (name_len == 0) continue;
    const std::string raw_name(reinterpret_cast<const char*>(p + 14), name_len);
    const std::string combined = prefix.empty() ? raw_name : (prefix + "/" + raw_name);
    const std::string clean = sanitize_relative_path(combined);
    if (clean.empty()) {
      if (error) *error = "XISO contains unsafe path: " + raw_name;
      return false;
    }

    const uint64_t data_offset = partition_offset + static_cast<uint64_t>(first_sector) * kSectorSize;
    const bool is_dir = (attrs & kAttrDirectory) != 0;

    if (!is_dir && file_size > 0) {
      if (data_offset > stream_size || file_size > stream_size - data_offset) {
        if (error) *error = "XISO file '" + clean + "' extends past image end";
        return false;
      }
    }

    Entry e;
    e.path = clean;
    e.data_offset = data_offset;
    e.size = file_size;
    e.attributes = attrs;
    e.is_directory = is_dir;
    out.push_back(std::move(e));

    if (is_dir && file_size > 0) {
      if (file_size > kMaxDirectoryBytes) {
        if (error) *error = "XISO subdirectory table too large: " + clean;
        return false;
      }
      subdirs.push_back({clean, first_sector, file_size, depth + 1});
    }
  }
  return true;
}

}  // namespace

bool detect_partition(const StreamReader& reader, PartitionInfo& out, std::string* error) {
  out = PartitionInfo{};
  const uint64_t total = reader.size();
  const uint64_t vd_rel = static_cast<uint64_t>(kVolumeDescriptorSector) * kSectorSize;

  std::array<uint8_t, kSectorSize> sector{};
  for (const auto& cand : kStandardPartitions) {
    if (total < cand.offset + vd_rel + kSectorSize) continue;
    if (!reader.read_at(cand.offset + vd_rel, sector.data(), sector.size(), nullptr)) continue;
    if (std::memcmp(sector.data(), kGdfxMagic, kMagicLen) != 0) continue;

    const uint32_t root_sector = fh1::load_le32(sector.data() + 0x14);
    const uint32_t root_size = fh1::load_le32(sector.data() + 0x18);
    const uint64_t root_off = cand.offset + static_cast<uint64_t>(root_sector) * kSectorSize;
    if (root_size > kMaxDirectoryBytes || root_off > total || root_size > total - root_off) continue;

    out.partition_offset = cand.offset;
    out.root_sector = root_sector;
    out.root_size = root_size;
    out.creation_filetime = fh1::load_le64(sector.data() + 0x1C);
    out.format_label = cand.label;
    return true;
  }

  if (error) *error = "no valid GDFX (MICROSOFT*XBOX*MEDIA) game partition found in ISO";
  return false;
}

bool list_entries(const StreamReader& reader, const PartitionInfo& partition, std::vector<Entry>& out,
                  std::string* error) {
  out.clear();
  if (partition.root_size == 0) return true;
  if (partition.root_size > kMaxDirectoryBytes) {
    if (error) *error = "XISO root directory exceeds safety limit";
    return false;
  }

  std::vector<DirTask> queue;
  queue.push_back({"", partition.root_sector, partition.root_size, 0});

  size_t cursor = 0;
  std::vector<uint8_t> dir_buf;
  while (cursor < queue.size()) {
    const DirTask task = queue[cursor++];
    const uint64_t dir_off = partition.partition_offset + static_cast<uint64_t>(task.sector) * kSectorSize;
    if (!reader.read_at(dir_off, task.size, dir_buf, error)) return false;
    if (!walk_directory_buffer(dir_buf, task.prefix, partition.partition_offset, reader.size(), task.depth, out, queue,
                               error)) {
      return false;
    }
  }

  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
  return true;
}

bool extract_iso(const StreamReader& reader, const fs::path& out_dir, const ExtractOptions& opts,
                 const ProgressCallback& progress, const std::atomic<bool>* cancel, ExtractReport& report) {
  report = ExtractReport{};
  std::string err;
  if (!detect_partition(reader, report.partition, &err)) {
    report.error = err;
    return false;
  }

  std::vector<Entry> all_entries;
  if (!list_entries(reader, report.partition, all_entries, &err)) {
    report.error = err;
    return false;
  }

  bool has_media_dir = false;
  for (const auto& e : all_entries) {
    if (starts_with_ci(e.path, "media/")) {
      has_media_dir = true;
      break;
    }
  }

  std::vector<const Entry*> files;
  uint64_t total_bytes = 0;
  for (const auto& e : all_entries) {
    if (e.is_directory) continue;
    if (opts.skip_xex && (ends_with_ci(e.path, ".xex") || ends_with_ci(e.path, ".xexp"))) {
      ++report.files_skipped;
      continue;
    }
    if (opts.media_only && has_media_dir && !starts_with_ci(e.path, "media/")) {
      ++report.files_skipped;
      continue;
    }
    files.push_back(&e);
    total_bytes += e.size;
  }

  std::error_code ec;
  fs::create_directories(out_dir, ec);
  if (ec) {
    report.error = "cannot create output directory " + out_dir.string() + ": " + ec.message();
    return false;
  }

  const size_t chunk_sz = std::max<size_t>(64u * 1024u, std::min<size_t>(opts.chunk_bytes, 8u * 1024u * 1024u));
  std::vector<uint8_t> chunk(chunk_sz);

  uint64_t bytes_done = 0;
  size_t files_done = 0;

  for (const Entry* e : files) {
    if (cancel && cancel->load(std::memory_order_relaxed)) {
      report.cancelled = true;
      report.error = "ISO extraction cancelled";
      return false;
    }

    const fs::path dest = out_dir / fs::path(e->path);
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
      report.error = "cannot create directory for " + e->path + ": " + ec.message();
      return false;
    }

    if (!opts.overwrite_existing && fs::is_regular_file(dest, ec) && fs::file_size(dest, ec) == e->size) {
      bytes_done += e->size;
      ++files_done;
      ++report.files_skipped;
      if (progress) progress({bytes_done, total_bytes, files_done, files.size(), e->path});
      continue;
    }

    const fs::path tmp = dest.string() + ".part";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      report.error = "cannot open for writing: " + dest.string();
      return false;
    }

    uint64_t remaining = e->size;
    uint64_t src_off = e->data_offset;
    while (remaining > 0) {
      if (cancel && cancel->load(std::memory_order_relaxed)) {
        out.close();
        fs::remove(tmp, ec);
        report.cancelled = true;
        report.error = "ISO extraction cancelled";
        return false;
      }
      const size_t n = static_cast<size_t>(std::min<uint64_t>(remaining, chunk.size()));
      if (!reader.read_at(src_off, chunk.data(), n, &err)) {
        out.close();
        fs::remove(tmp, ec);
        report.error = "read error on " + e->path + ": " + err;
        return false;
      }
      out.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(n));
      if (!out) {
        out.close();
        fs::remove(tmp, ec);
        report.error = "write error (storage full?) on " + e->path;
        return false;
      }
      src_off += n;
      remaining -= n;
      bytes_done += n;
      report.bytes_written += n;
      if (progress) progress({bytes_done, total_bytes, files_done, files.size(), e->path});
    }

    out.close();
    fs::remove(dest, ec);
    ec.clear();
    fs::rename(tmp, dest, ec);
    if (ec) {
      fs::remove(tmp, ec);
      report.error = "failed to finalize " + e->path + ": " + ec.message();
      return false;
    }

    ++files_done;
    ++report.files_extracted;
    if (progress) progress({bytes_done, total_bytes, files_done, files.size(), e->path});
  }

  report.ok = true;
  return true;
}

bool extract_iso_path(const fs::path& iso_path, const fs::path& out_dir, const ExtractOptions& opts,
                      const ProgressCallback& progress, const std::atomic<bool>* cancel, ExtractReport& report) {
  std::string err;
  auto reader = StreamReader::open_path(iso_path, &err);
  if (!reader) {
    report = ExtractReport{};
    report.error = err;
    return false;
  }
  return extract_iso(*reader, out_dir, opts, progress, cancel, report);
}

bool extract_iso_fd(int fd, bool take_ownership, const fs::path& out_dir, const ExtractOptions& opts,
                    const ProgressCallback& progress, const std::atomic<bool>* cancel, ExtractReport& report) {
  std::string err;
  auto reader = StreamReader::from_fd(fd, take_ownership, &err);
  if (!reader) {
    report = ExtractReport{};
    report.error = err;
    return false;
  }
  return extract_iso(*reader, out_dir, opts, progress, cancel, report);
}

DiscCheck inspect_disc_directory(const fs::path& game_root) {
  DiscCheck c;
  std::error_code ec;
  if (game_root.empty() || !fs::exists(game_root, ec)) {
    c.summary = "No game disc configured";
    return c;
  }
  fs::path root = game_root;
  if (lower_ascii(root.filename().string()) == "media") root = root.parent_path();
  const fs::path media = fs::is_directory(root / "media", ec) ? (root / "media") : root;
  c.resolved_root = root;
  c.has_colorado_bin = fs::is_regular_file(media / "tracks" / "colorado" / "bin.zip", ec);
  c.has_gamedb = fs::is_regular_file(media / "db" / "gamedb.slt", ec);
  if (fs::is_directory(media / "cars", ec)) {
    for (const auto& e : fs::directory_iterator(media / "cars", ec)) {
      if (e.is_regular_file(ec) && lower_ascii(e.path().extension().string()) == ".zip") ++c.car_zip_count;
    }
  }
  c.valid = c.has_colorado_bin && c.has_gamedb && c.car_zip_count > 0;
  if (c.valid) {
    c.summary = "Verified disc (" + std::to_string(c.car_zip_count) + " car archives)";
  } else {
    c.summary = "Incomplete disc folder (missing media/db/gamedb.slt, media/cars/*.zip, or colorado/bin.zip)";
  }
  return c;
}

}  // namespace setup::xiso
