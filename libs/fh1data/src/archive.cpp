#include "fh1data/archive.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <mutex>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

#include "fh1data/bytes.h"
#include "fh1data/inflate.h"
#include "fh1data/xmem.h"

namespace fh1 {
namespace {

constexpr uint32_t kLocalHeaderSignature = 0x04034B50;
constexpr uint32_t kCentralHeaderSignature = 0x02014B50;
constexpr uint32_t kEndOfDirectorySignature = 0x06054B50;
constexpr size_t kEndOfDirectorySize = 22;
constexpr size_t kLocalHeaderSize = 30;
constexpr size_t kCentralHeaderSize = 46;
constexpr uint16_t kExtraPayloadOffset = 0x1123;
constexpr uint16_t kExtraZip64 = 0x0001;

// The installed mod override: a flag for the common no-mods path, the
// pointer under a mutex (read() copies it, so a swap mid-read is safe).
std::atomic<bool> g_has_override{false};
std::mutex g_override_mutex;
std::shared_ptr<const MemberOverride> g_override;

}  // namespace

void set_member_override(std::shared_ptr<const MemberOverride> override_hooks) {
  std::lock_guard<std::mutex> lock(g_override_mutex);
  g_override = std::move(override_hooks);
  g_has_override.store(g_override != nullptr);
}

std::shared_ptr<const MemberOverride> member_override() {
  if (!g_has_override.load(std::memory_order_relaxed)) return nullptr;
  std::lock_guard<std::mutex> lock(g_override_mutex);
  return g_override;
}

std::string normalize_member_name(std::string_view name) {
  std::string out(name);
  for (char& c : out) {
    c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

uint32_t crc32(std::span<const uint8_t> data) {
#if defined(__ARM_FEATURE_CRC32)
  uint32_t crc = 0xFFFFFFFFu;
  const uint8_t* p = data.data();
  size_t n = data.size();
  while (n >= 8) {
    uint64_t v = 0;
    std::memcpy(&v, p, 8);
    crc = __crc32d(crc, v);
    p += 8;
    n -= 8;
  }
  if (n >= 4) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    crc = __crc32w(crc, v);
    p += 4;
    n -= 4;
  }
  if (n >= 2) {
    uint16_t v = 0;
    std::memcpy(&v, p, 2);
    crc = __crc32h(crc, v);
    p += 2;
    n -= 2;
  }
  if (n) crc = __crc32b(crc, *p);
  return crc ^ 0xFFFFFFFFu;
#else
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> values{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit) value = (value & 1) ? 0xEDB88320u ^ (value >> 1) : value >> 1;
      values[i] = value;
    }
    return values;
  }();
  uint32_t crc = 0xFFFFFFFFu;
  for (uint8_t byte : data) crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
#endif
}

Archive::~Archive() {
#if !defined(_WIN32)
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
#endif
}

bool Archive::read_range(uint64_t offset, size_t size, std::vector<uint8_t>& out) {
  if (offset > file_size_ || size > file_size_ - offset) return false;
  out.resize(size);
  if (size == 0) return true;
#if !defined(_WIN32)
  if (fd_ >= 0) {
    size_t done = 0;
    while (done < size) {
      const ssize_t n = ::pread(fd_, out.data() + done, size - done, static_cast<off_t>(offset + done));
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (n == 0) return false;
      done += size_t(n);
    }
    return true;
  }
#endif
  std::lock_guard<std::mutex> lock(io_mutex_);
  stream_.clear();
  stream_.seekg(std::streamoff(offset));
  return bool(stream_.read(reinterpret_cast<char*>(out.data()), std::streamsize(size)));
}

std::unique_ptr<Archive> Archive::open(const std::filesystem::path& path, std::string* error) {
  auto fail = [&](const char* why) -> std::unique_ptr<Archive> {
    if (error) *error = why;
    return nullptr;
  };
  auto archive = std::unique_ptr<Archive>(new Archive());
  archive->path_ = path;
#if !defined(_WIN32)
  archive->fd_ = ::open(path.c_str(), O_RDONLY);
  if (archive->fd_ < 0) return fail("cannot open file");
  struct stat st {};
  if (::fstat(archive->fd_, &st) != 0 || st.st_size < 0) return fail("cannot open file");
  archive->file_size_ = uint64_t(st.st_size);
#else
  archive->stream_.open(path, std::ios::binary);
  if (!archive->stream_) return fail("cannot open file");
  archive->stream_.seekg(0, std::ios::end);
  archive->file_size_ = uint64_t(archive->stream_.tellg());
#endif
  if (archive->file_size_ < kEndOfDirectorySize) return fail("file too small for a ZIP");

  // The end record is followed by at most a 64 KiB comment.
  const size_t tail_size =
      size_t(std::min<uint64_t>(archive->file_size_, kEndOfDirectorySize + 0xFFFF));
  std::vector<uint8_t> tail;
  if (!archive->read_range(archive->file_size_ - tail_size, tail_size, tail))
    return fail("cannot read end of directory");
  size_t end_record = SIZE_MAX;
  for (size_t at = tail_size - kEndOfDirectorySize + 1; at-- > 0;) {
    if (load_le32(&tail[at]) == kEndOfDirectorySignature) {
      end_record = at;
      break;
    }
  }
  if (end_record == SIZE_MAX) return fail("no end-of-directory record");
  const uint32_t directory_size = load_le32(&tail[end_record + 12]);
  const uint32_t directory_offset = load_le32(&tail[end_record + 16]);
  if (directory_offset == 0xFFFFFFFFu) return fail("ZIP64 archives are not supported");
  std::vector<uint8_t> directory;
  if (!archive->read_range(directory_offset, directory_size, directory))
    return fail("central directory outside the file");

  const size_t est_members = std::max<size_t>(16, directory.size() / 96);
  archive->members_.reserve(est_members);
  archive->index_.reserve(std::max<size_t>(16, est_members / 4));

  size_t at = 0;
  while (at < directory.size()) {
    if (directory.size() - at < kCentralHeaderSize ||
        load_le32(&directory[at]) != kCentralHeaderSignature)
      return fail("bad central directory entry");
    const uint8_t* h = &directory[at];
    const size_t name_size = load_le16(h + 28);
    const size_t extra_size = load_le16(h + 30);
    const size_t comment_size = load_le16(h + 32);
    if (directory.size() - at - kCentralHeaderSize < name_size + extra_size + comment_size)
      return fail("truncated central directory entry");
    ArchiveMember m;
    m.method = load_le16(h + 10);
    m.crc32 = load_le32(h + 16);
    m.compressed_size = load_le32(h + 20);
    m.size = load_le32(h + 24);
    m.header_offset = load_le32(h + 42);
    m.name.assign(reinterpret_cast<const char*>(h + kCentralHeaderSize), name_size);
    const uint8_t* extra = h + kCentralHeaderSize + name_size;
    for (size_t e = 0; e + 4 <= extra_size;) {
      const uint16_t tag = load_le16(extra + e);
      const uint16_t length = load_le16(extra + e + 2);
      if (e + 4 + length > extra_size) break;
      const uint8_t* body = extra + e + 4;
      if (tag == kExtraPayloadOffset && length == 4) {
        m.payload_offset = load_le32(body);
      } else if (tag == kExtraZip64) {
        // Only the fields whose 32-bit value is saturated are present, in order.
        size_t z = 0;
        auto take = [&](uint64_t& field) {
          if (field == 0xFFFFFFFFu && z + 8 <= length) {
            field = load_le64(body + z);
            z += 8;
          }
        };
        take(m.size);
        take(m.compressed_size);
        take(m.header_offset);
      }
      e += 4 + length;
    }
    // Names repeat in some archives (bin.zip: 230,057 members, 43,546
    // distinct names); every member is kept, lookup finds the first.
    archive->index_.emplace(normalize_member_name(m.name), archive->members_.size());
    archive->members_.push_back(std::move(m));
    at += kCentralHeaderSize + name_size + extra_size + comment_size;
  }
  return archive;
}

std::shared_ptr<Archive> Archive::open_shared(const std::filesystem::path& path, std::string* error) {
  struct CacheEntry {
    uint64_t size = 0;
    int64_t mtime = 0;
    std::weak_ptr<Archive> archive;
  };
  static std::mutex cache_mutex;
  static std::unordered_map<std::string, CacheEntry> cache;

  std::error_code ec;
  const std::filesystem::path norm = std::filesystem::weakly_canonical(path, ec);
  const std::string key = normalize_member_name((ec ? path : norm).string());
  const uint64_t fsize = std::filesystem::file_size(path, ec);
  const bool have_size = !ec;
  const auto ftime = std::filesystem::last_write_time(path, ec);
  const int64_t mtime = ec ? 0 : static_cast<int64_t>(ftime.time_since_epoch().count());

  if (have_size) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (auto it = cache.find(key); it != cache.end()) {
      if (it->second.size == fsize && it->second.mtime == mtime) {
        if (auto sp = it->second.archive.lock()) return sp;
      }
    }
  }

  auto opened = Archive::open(path, error);
  if (!opened) return nullptr;
  std::shared_ptr<Archive> shared(std::move(opened));
  if (have_size) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    cache[key] = CacheEntry{fsize, mtime, shared};
  }
  return shared;
}

std::shared_ptr<const Archive::BaseNameIndex> Archive::base_name_index() const {
  std::lock_guard<std::mutex> lock(base_index_mutex_);
  if (base_index_) return base_index_;
  auto idx = std::make_shared<BaseNameIndex>();
  idx->by_name.reserve(std::max<size_t>(16, index_.size()));
  for (const auto& m : members_) {
    const auto slash = m.name.find_last_of("/\\");
    std::string base = slash == std::string::npos ? m.name : m.name.substr(slash + 1);
    for (char& c : base) c = char(std::tolower(static_cast<unsigned char>(c)));
    idx->by_name.emplace(std::move(base), &m);
  }
  idx->names.reserve(idx->by_name.size());
  for (const auto& [name, _] : idx->by_name) idx->names.push_back(name);
  std::sort(idx->names.begin(), idx->names.end());
  base_index_ = idx;
  return base_index_;
}

const ArchiveMember* Archive::find(std::string_view name) const {
  auto it = index_.find(normalize_member_name(name));
  return it == index_.end() ? nullptr : &members_[it->second];
}

bool Archive::has_local_header(const ArchiveMember& member) {
  std::vector<uint8_t> local;
  return read_range(member.header_offset, 4, local) && load_le32(local.data()) == kLocalHeaderSignature;
}

std::optional<uint64_t> Archive::resolve_payload(const ArchiveMember& member, std::string* /*error*/) {
  // Extra field 0x1123 records the absolute payload offset in the central
  // directory, avoiding a redundant local-header seek+read on every extraction.
  if (member.payload_offset) return member.payload_offset;
  std::vector<uint8_t> local;
  const bool has_local = read_range(member.header_offset, kLocalHeaderSize, local) &&
                         load_le32(local.data()) == kLocalHeaderSignature;
  if (has_local) {
    return member.header_offset + kLocalHeaderSize + load_le16(&local[26]) +
           load_le16(&local[28]);
  }
  // Without the extra field or a local header, the directory offset is the payload itself.
  return std::optional<uint64_t>(member.header_offset);
}

ReadResult Archive::read(const ArchiveMember& member) {
  ReadResult result;
  const auto hooks = member_override();
  if (hooks && hooks->replace && hooks->replace(path_, member.name, result.data)) return result;
  if (member.size > kMaximumMemberSize || member.compressed_size > kMaximumMemberSize) {
    result.error = "member too large";
    return result;
  }
  const auto payload = resolve_payload(member, &result.error);
  if (!payload) return result;
  std::vector<uint8_t> compressed;
  if (!read_range(*payload, size_t(member.compressed_size), compressed)) {
    result.error = "payload outside the file";
    return result;
  }
  switch (ZipMethod(member.method)) {
    case ZipMethod::kStored:
      result.data = std::move(compressed);
      break;
    case ZipMethod::kXmem: {
      auto data = xmem_decompress(compressed, size_t(member.size), member.crc32,
                                  &result.lzx_window_bits);
      if (!data) {
        result.error = "XMem LZX decode failed";
        return result;
      }
      result.data = std::move(*data);
      break;
    }
    case ZipMethod::kDeflate: {
      auto data = inflate_raw(compressed, size_t(member.size));
      if (!data) {
        result.error = "deflate decode failed";
        return result;
      }
      result.data = std::move(*data);
      break;
    }
    default:
      result.error = "unknown method " + std::to_string(member.method);
      return result;
  }
  if (result.data.size() != member.size || crc32(result.data) != member.crc32) {
    result.error = "CRC mismatch";
    result.data.clear();
    return result;
  }
  if (hooks && hooks->patch) hooks->patch(path_, member.name, result.data);
  return result;
}

ReadResult Archive::read(std::string_view name) {
  const ArchiveMember* member = find(name);
  if (!member) {
    ReadResult added;
    if (const auto hooks = member_override(); hooks && hooks->replace && hooks->replace(path_, name, added.data))
      return added;
    return ReadResult{{}, "no member named " + std::string(name)};
  }
  return read(*member);
}

}  // namespace fh1
