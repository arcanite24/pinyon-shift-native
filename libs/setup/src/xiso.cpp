#include "setup/xiso.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

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

namespace {

// Direct, low-overhead file writer bypassing std::ofstream's small default stdio buffer.
struct FastOutFile {
#ifdef _WIN32
  std::FILE* fp = nullptr;
#else
  int fd = -1;
#endif
  uint64_t bytes_written = 0;

  bool open(const fs::path& p, std::string* err) {
    close(false);
#ifdef _WIN32
    if (_wfopen_s(&fp, p.c_str(), L"wb") != 0 || !fp) {
      fp = nullptr;
      if (err) *err = "cannot open for writing: " + p.string();
      return false;
    }
    std::setvbuf(fp, nullptr, _IONBF, 0);
    return true;
#else
    fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      if (err) *err = "cannot open for writing: " + p.string() + " (" + std::strerror(errno) + ")";
      return false;
    }
    return true;
#endif
  }

  bool write(const uint8_t* data, size_t len, std::string* err) {
    if (len == 0) return true;
#ifdef _WIN32
    if (!fp) return false;
    if (std::fwrite(data, 1, len, fp) != len) {
      if (err) *err = "write error (storage full?)";
      return false;
    }
    bytes_written += len;
    return true;
#else
    if (fd < 0) return false;
    size_t off = 0;
    while (off < len) {
      const ssize_t n = ::write(fd, data + off, len - off);
      if (n < 0) {
        if (errno == EINTR) continue;
        if (err) *err = std::string("write error (storage full?): ") + std::strerror(errno);
        return false;
      }
      if (n == 0) {
        if (err) *err = "short write (storage full?)";
        return false;
      }
      off += static_cast<size_t>(n);
    }
    bytes_written += len;
    return true;
#endif
  }

  void close(bool drop_page_cache = false) {
#ifdef _WIN32
    (void)drop_page_cache;
    if (fp) {
      std::fclose(fp);
      fp = nullptr;
    }
#else
    if (fd >= 0) {
#if defined(__linux__)
      if (drop_page_cache && bytes_written >= 16u * 1024u * 1024u) {
        (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
      }
#else
      (void)drop_page_cache;
#endif
      ::close(fd);
      fd = -1;
    }
#endif
    bytes_written = 0;
  }

  ~FastOutFile() { close(false); }
};

bool write_whole_file_direct(const fs::path& dest, const uint8_t* data, size_t len, std::string* err) {
  FastOutFile out;
  if (!out.open(dest, err)) return false;
  if (len > 0 && !out.write(data, len, err)) {
    out.close(false);
    std::error_code ec;
    fs::remove(dest, ec);
    return false;
  }
  out.close(false);
  return true;
}

}  // namespace

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
  files.reserve(all_entries.size());
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

  // Sort by ascending ISO data_offset so reads stream strictly forward sequentially.
  std::sort(files.begin(), files.end(), [](const Entry* a, const Entry* b) {
    if (a->data_offset != b->data_offset) return a->data_offset < b->data_offset;
    return a->path < b->path;
  });

  std::error_code ec;
  fs::create_directories(out_dir, ec);
  if (ec) {
    report.error = "cannot create output directory " + out_dir.string() + ": " + ec.message();
    return false;
  }

  // Filter already-extracted files (resume) and pre-create every needed parent directory once.
  struct ExtractItem {
    const Entry* entry = nullptr;
    fs::path dest;
  };
  std::vector<ExtractItem> todo;
  todo.reserve(files.size());
  std::set<fs::path> parent_dirs;
  std::atomic<uint64_t> bytes_done{0};
  std::atomic<size_t> files_done{0};
  std::atomic<size_t> files_extracted{0};
  std::atomic<uint64_t> bytes_written{0};

  for (const Entry* e : files) {
    if (cancel && cancel->load(std::memory_order_relaxed)) {
      report.cancelled = true;
      report.error = "ISO extraction cancelled";
      return false;
    }
    fs::path dest = out_dir / fs::path(e->path);
    if (!opts.overwrite_existing && fs::is_regular_file(dest, ec) && fs::file_size(dest, ec) == e->size) {
      bytes_done.fetch_add(e->size, std::memory_order_relaxed);
      files_done.fetch_add(1, std::memory_order_relaxed);
      ++report.files_skipped;
      continue;
    }
    if (dest.has_parent_path()) parent_dirs.insert(dest.parent_path());
    todo.push_back({e, std::move(dest)});
  }

  for (const fs::path& dir : parent_dirs) {
    ec.clear();
    fs::create_directories(dir, ec);
    if (ec) {
      report.error = "cannot create directory " + dir.string() + ": " + ec.message();
      return false;
    }
  }

  const size_t total_files = files.size();
  std::mutex progress_mu;
  auto last_progress_tp = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  auto emit_progress = [&](const std::string& cur_path, bool force) {
    if (!progress) return;
    std::lock_guard<std::mutex> lk(progress_mu);
    const auto now = std::chrono::steady_clock::now();
    if (!force && (now - last_progress_tp) < std::chrono::milliseconds(80)) return;
    last_progress_tp = now;
    progress({bytes_done.load(std::memory_order_relaxed), total_bytes, files_done.load(std::memory_order_relaxed),
              total_files, cur_path});
  };

  if (todo.empty()) {
    emit_progress(files.empty() ? std::string() : files.back()->path, true);
    report.ok = true;
    return true;
  }

  const size_t chunk_sz = std::max<size_t>(64u * 1024u, std::min<size_t>(opts.chunk_bytes, 8u * 1024u * 1024u));
  constexpr size_t kSmallQueueByteCap = 24u * 1024u * 1024u;
  constexpr size_t kLargeChunkSlots = 6;

  std::atomic<bool> failed{false};
  std::atomic<bool> was_cancelled{false};
  std::mutex err_mu;
  std::string first_error;
  auto record_error = [&](const std::string& msg, bool is_cancel = false) {
    if (is_cancel) was_cancelled.store(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(err_mu);
    if (first_error.empty()) first_error = msg;
    failed.store(true, std::memory_order_release);
  };
  auto is_stopped = [&]() -> bool {
    if (failed.load(std::memory_order_acquire)) return true;
    if (cancel && cancel->load(std::memory_order_relaxed)) {
      record_error("ISO extraction cancelled", true);
      return true;
    }
    return false;
  };

  // ---- Small/medium file parallel writer pool (<= chunk_sz) ----
  // Hides FUSE open/write/close latency on Android while keeping ISO reads 100% sequential.
  struct SmallFileJob {
    const Entry* entry = nullptr;
    fs::path dest;
    std::vector<uint8_t> data;
  };
  std::mutex small_mu;
  std::condition_variable small_cv;
  std::deque<SmallFileJob> small_queue;
  size_t small_queued_bytes = 0;
  bool small_done = false;

  size_t num_small_writers = opts.writer_threads;
  if (num_small_writers == 0) {
    const unsigned hw = std::thread::hardware_concurrency();
    num_small_writers = std::clamp<size_t>(hw >= 4 ? hw / 2 : 2, 2, 4);
  }
  num_small_writers = std::clamp<size_t>(num_small_writers, 1, 8);
  size_t small_item_count = 0;
  size_t large_item_count = 0;
  for (const auto& item : todo) {
    if (item.entry->size <= chunk_sz) ++small_item_count;
    else ++large_item_count;
  }
  if (small_item_count == 0) num_small_writers = 0;
  else num_small_writers = std::min(num_small_writers, small_item_count);

  std::vector<std::thread> small_workers;
  small_workers.reserve(num_small_writers);
  for (size_t wi = 0; wi < num_small_writers; ++wi) {
    small_workers.emplace_back([&]() {
      for (;;) {
        SmallFileJob job;
        {
          std::unique_lock<std::mutex> lk(small_mu);
          small_cv.wait(lk, [&]() { return !small_queue.empty() || small_done || failed.load(std::memory_order_acquire); });
          if (failed.load(std::memory_order_acquire) || (small_queue.empty() && small_done)) break;
          job = std::move(small_queue.front());
          small_queue.pop_front();
          small_queued_bytes -= job.data.size();
        }
        small_cv.notify_all();

        if (is_stopped()) break;

        std::string werr;
        if (!write_whole_file_direct(job.dest, job.data.data(), job.data.size(), &werr)) {
          record_error("write error on " + job.entry->path + ": " + werr);
          small_cv.notify_all();
          break;
        }
        bytes_done.fetch_add(job.entry->size, std::memory_order_relaxed);
        bytes_written.fetch_add(job.entry->size, std::memory_order_relaxed);
        files_done.fetch_add(1, std::memory_order_relaxed);
        files_extracted.fetch_add(1, std::memory_order_relaxed);
        emit_progress(job.entry->path, true);
      }
    });
  }

  // ---- Large file async streaming writer (> chunk_sz) ----
  // Overlaps sequential 4 MiB ISO reads with 4 MiB direct writes using recycled buffers.
  struct LargeMsg {
    enum class Kind { kOpen, kChunk, kClose } kind = Kind::kChunk;
    const Entry* entry = nullptr;
    fs::path dest;
    std::vector<uint8_t> buf;
    size_t len = 0;
  };
  std::mutex large_mu;
  std::condition_variable large_cv;
  std::deque<LargeMsg> large_queue;
  std::vector<std::vector<uint8_t>> free_chunk_bufs;
  bool large_done = false;

  std::thread large_worker;
  if (large_item_count > 0) {
    free_chunk_bufs.reserve(kLargeChunkSlots);
    for (size_t i = 0; i < kLargeChunkSlots; ++i) {
      free_chunk_bufs.emplace_back(chunk_sz);
    }
    large_worker = std::thread([&]() {
      FastOutFile active_file;
      fs::path active_dest;
      const Entry* active_entry = nullptr;
      auto abort_active = [&]() {
        if (active_entry) {
          active_file.close(false);
          std::error_code rec;
          fs::remove(active_dest, rec);
          active_entry = nullptr;
          active_dest.clear();
        }
      };
      for (;;) {
        LargeMsg msg;
        {
          std::unique_lock<std::mutex> lk(large_mu);
          large_cv.wait(lk, [&]() { return !large_queue.empty() || large_done || failed.load(std::memory_order_acquire); });
          if (failed.load(std::memory_order_acquire)) {
            abort_active();
            break;
          }
          if (large_queue.empty() && large_done) break;
          msg = std::move(large_queue.front());
          large_queue.pop_front();
        }

        if (is_stopped()) {
          if (!msg.buf.empty()) {
            std::lock_guard<std::mutex> lk(large_mu);
            free_chunk_bufs.push_back(std::move(msg.buf));
            large_cv.notify_all();
          }
          abort_active();
          break;
        }

        if (msg.kind == LargeMsg::Kind::kOpen) {
          std::string werr;
          if (!active_file.open(msg.dest, &werr)) {
            record_error("cannot open " + msg.entry->path + ": " + werr);
            large_cv.notify_all();
            break;
          }
          active_dest = std::move(msg.dest);
          active_entry = msg.entry;
        } else if (msg.kind == LargeMsg::Kind::kChunk) {
          std::string werr;
          const bool ok = active_file.write(msg.buf.data(), msg.len, &werr);
          {
            std::lock_guard<std::mutex> lk(large_mu);
            free_chunk_bufs.push_back(std::move(msg.buf));
          }
          large_cv.notify_all();
          if (!ok) {
            abort_active();
            record_error("write error on " + (msg.entry ? msg.entry->path : std::string()) + ": " + werr);
            large_cv.notify_all();
            break;
          }
          bytes_done.fetch_add(msg.len, std::memory_order_relaxed);
          bytes_written.fetch_add(msg.len, std::memory_order_relaxed);
          if (active_entry) emit_progress(active_entry->path, false);
        } else if (msg.kind == LargeMsg::Kind::kClose) {
          active_file.close(true);
          if (active_entry) {
            files_done.fetch_add(1, std::memory_order_relaxed);
            files_extracted.fetch_add(1, std::memory_order_relaxed);
            emit_progress(active_entry->path, true);
          }
          active_entry = nullptr;
          active_dest.clear();
        }
      }
      abort_active();
    });
  }

  // ---- Sequential Producer Loop (ascending data_offset order) ----
  for (const auto& item : todo) {
    if (is_stopped()) break;
    const Entry* e = item.entry;

    if (e->size <= chunk_sz) {
      // Wait for bounded small-file memory budget before reading.
      {
        std::unique_lock<std::mutex> lk(small_mu);
        small_cv.wait(lk, [&]() {
          return small_queued_bytes < kSmallQueueByteCap || failed.load(std::memory_order_acquire) ||
                 (cancel && cancel->load(std::memory_order_relaxed));
        });
      }
      if (is_stopped()) break;

      std::vector<uint8_t> data(static_cast<size_t>(e->size));
      if (!data.empty()) {
        std::string rerr;
        if (!reader.read_at(e->data_offset, data.data(), data.size(), &rerr)) {
          record_error("read error on " + e->path + ": " + rerr);
          break;
        }
      }
      {
        std::lock_guard<std::mutex> lk(small_mu);
        small_queued_bytes += data.size();
        small_queue.push_back({e, item.dest, std::move(data)});
      }
      small_cv.notify_one();
    } else {
      // Large file: stream recycled chunk_sz buffers to dedicated async writer.
      {
        std::lock_guard<std::mutex> lk(large_mu);
        large_queue.push_back({LargeMsg::Kind::kOpen, e, item.dest, {}, 0});
      }
      large_cv.notify_one();

      uint64_t remaining = e->size;
      uint64_t src_off = e->data_offset;
      while (remaining > 0) {
        if (is_stopped()) break;
        std::vector<uint8_t> buf;
        {
          std::unique_lock<std::mutex> lk(large_mu);
          large_cv.wait(lk, [&]() {
            return !free_chunk_bufs.empty() || failed.load(std::memory_order_acquire) ||
                   (cancel && cancel->load(std::memory_order_relaxed));
          });
          if (failed.load(std::memory_order_acquire) || (cancel && cancel->load(std::memory_order_relaxed))) break;
          buf = std::move(free_chunk_bufs.back());
          free_chunk_bufs.pop_back();
        }
        if (is_stopped()) {
          std::lock_guard<std::mutex> lk(large_mu);
          free_chunk_bufs.push_back(std::move(buf));
          break;
        }
        const size_t n = static_cast<size_t>(std::min<uint64_t>(remaining, buf.size()));
        std::string rerr;
        if (!reader.read_at(src_off, buf.data(), n, &rerr)) {
          {
            std::lock_guard<std::mutex> lk(large_mu);
            free_chunk_bufs.push_back(std::move(buf));
          }
          record_error("read error on " + e->path + ": " + rerr);
          break;
        }
        {
          std::lock_guard<std::mutex> lk(large_mu);
          large_queue.push_back({LargeMsg::Kind::kChunk, e, {}, std::move(buf), n});
        }
        large_cv.notify_one();
        src_off += n;
        remaining -= n;
      }

      if (!is_stopped()) {
        std::lock_guard<std::mutex> lk(large_mu);
        large_queue.push_back({LargeMsg::Kind::kClose, e, {}, {}, 0});
        large_cv.notify_one();
      }
    }
  }

  // Signal completion and join all writer threads cleanly.
  {
    std::lock_guard<std::mutex> lk(small_mu);
    small_done = true;
  }
  small_cv.notify_all();
  {
    std::lock_guard<std::mutex> lk(large_mu);
    large_done = true;
  }
  large_cv.notify_all();

  for (auto& t : small_workers) {
    if (t.joinable()) t.join();
  }
  if (large_worker.joinable()) large_worker.join();

  report.files_extracted = files_extracted.load(std::memory_order_relaxed);
  report.bytes_written = bytes_written.load(std::memory_order_relaxed);

  if (failed.load(std::memory_order_acquire)) {
    report.cancelled = was_cancelled.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(err_mu);
    report.error = first_error.empty() ? "ISO extraction failed" : first_error;
    return false;
  }

  emit_progress(todo.back().entry->path, true);
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
