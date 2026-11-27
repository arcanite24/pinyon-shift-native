#include "setup/stream_reader.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace setup {
namespace {

class FilePathReader final : public StreamReader {
 public:
  FilePathReader(std::FILE* fp, uint64_t sz) : fp_(fp), size_(sz) {
#if defined(__linux__)
    if (fp_) {
      const int fd = ::fileno(fp_);
      if (fd >= 0) (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    }
#endif
  }
  ~FilePathReader() override {
    if (fp_) std::fclose(fp_);
  }

  uint64_t size() const override { return size_; }

  bool read_at(uint64_t offset, void* dst, size_t len, std::string* error) const override {
    if (len == 0) return true;
    if (offset > size_ || len > size_ - offset) {
      if (error) *error = "read past end of file";
      return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
#ifdef _WIN32
    if (_fseeki64(fp_, static_cast<long long>(offset), SEEK_SET) != 0) {
#else
    if (fseeko(fp_, static_cast<off_t>(offset), SEEK_SET) != 0) {
#endif
      if (error) *error = "seek failed";
      return false;
    }
    const size_t got = std::fread(dst, 1, len, fp_);
    if (got != len) {
      if (error) *error = "short read from file";
      return false;
    }
    return true;
  }

 private:
  mutable std::mutex mu_;
  std::FILE* fp_ = nullptr;
  uint64_t size_ = 0;
};

class FdReader final : public StreamReader {
 public:
  FdReader(int fd, bool owned, uint64_t sz) : fd_(fd), owned_(owned), size_(sz) {
#if defined(__linux__)
    if (fd_ >= 0) (void)::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  }
  ~FdReader() override {
    if (owned_ && fd_ >= 0) {
#ifdef _WIN32
      _close(fd_);
#else
      ::close(fd_);
#endif
    }
  }

  uint64_t size() const override { return size_; }

  bool read_at(uint64_t offset, void* dst, size_t len, std::string* error) const override {
    if (len == 0) return true;
    if (offset > size_ || len > size_ - offset) {
      if (error) *error = "read past end of file descriptor";
      return false;
    }
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t remaining = len;
    uint64_t pos = offset;
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(mu_);
    if (_lseeki64(fd_, static_cast<__int64>(pos), SEEK_SET) < 0) {
      if (error) *error = "fd seek failed";
      return false;
    }
    while (remaining > 0) {
      const unsigned int chunk = static_cast<unsigned int>(std::min<size_t>(remaining, 0x40000000u));
      const int n = _read(fd_, out, chunk);
      if (n <= 0) {
        if (error) *error = "short read from file descriptor";
        return false;
      }
      out += n;
      remaining -= static_cast<size_t>(n);
    }
    return true;
#else
    while (remaining > 0) {
      const ssize_t n = ::pread(fd_, out, remaining, static_cast<off_t>(pos));
      if (n < 0) {
        if (errno == EINTR) continue;
        if (error) *error = std::string("pread failed: ") + std::strerror(errno);
        return false;
      }
      if (n == 0) {
        if (error) *error = "unexpected EOF on file descriptor";
        return false;
      }
      out += n;
      pos += static_cast<uint64_t>(n);
      remaining -= static_cast<size_t>(n);
    }
    return true;
#endif
  }

 private:
#ifdef _WIN32
  mutable std::mutex mu_;
#endif
  int fd_ = -1;
  bool owned_ = false;
  uint64_t size_ = 0;
};

class MemoryReader final : public StreamReader {
 public:
  explicit MemoryReader(std::span<const uint8_t> data) : data_(data) {}
  uint64_t size() const override { return static_cast<uint64_t>(data_.size()); }
  bool read_at(uint64_t offset, void* dst, size_t len, std::string* error) const override {
    if (len == 0) return true;
    if (offset > data_.size() || len > data_.size() - offset) {
      if (error) *error = "read past end of memory buffer";
      return false;
    }
    std::memcpy(dst, data_.data() + offset, len);
    return true;
  }

 private:
  std::span<const uint8_t> data_;
};

class CallbackReader final : public StreamReader {
 public:
  CallbackReader(uint64_t total_size, ReadFn fn) : size_(total_size), fn_(std::move(fn)) {}
  uint64_t size() const override { return size_; }
  bool read_at(uint64_t offset, void* dst, size_t len, std::string* error) const override {
    if (len == 0) return true;
    if (offset > size_ || len > size_ - offset) {
      if (error) *error = "read past end of virtual stream";
      return false;
    }
    return fn_(offset, dst, len, error);
  }

 private:
  uint64_t size_ = 0;
  ReadFn fn_;
};

}  // namespace

std::unique_ptr<StreamReader> StreamReader::open_path(const std::filesystem::path& path, std::string* error) {
#ifdef _WIN32
  std::FILE* fp = nullptr;
  if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) {
    if (error) *error = "cannot open file: " + path.string();
    return nullptr;
  }
  if (_fseeki64(fp, 0, SEEK_END) != 0) {
    std::fclose(fp);
    if (error) *error = "cannot seek to end of file: " + path.string();
    return nullptr;
  }
  const long long sz = _ftelli64(fp);
  if (sz < 0) {
    std::fclose(fp);
    if (error) *error = "cannot determine file size: " + path.string();
    return nullptr;
  }
  return std::make_unique<FilePathReader>(fp, static_cast<uint64_t>(sz));
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (error) *error = "cannot open file: " + path.string() + " (" + std::strerror(errno) + ")";
    return nullptr;
  }
  return from_fd(fd, true, error);
#endif
}

std::unique_ptr<StreamReader> StreamReader::from_fd(int fd, bool take_ownership, std::string* error) {
  if (fd < 0) {
    if (error) *error = "invalid file descriptor";
    return nullptr;
  }
#ifdef _WIN32
  struct __stat64 st {};
  if (_fstat64(fd, &st) != 0 || st.st_size < 0) {
    if (take_ownership) _close(fd);
    if (error) *error = "fstat failed on file descriptor";
    return nullptr;
  }
  const uint64_t sz = static_cast<uint64_t>(st.st_size);
#else
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size < 0) {
    // Fallback to lseek for SAF descriptors where fstat reports 0 or fails.
    const off_t end = ::lseek(fd, 0, SEEK_END);
    if (end < 0) {
      if (take_ownership) ::close(fd);
      if (error) *error = std::string("fstat/lseek failed on fd: ") + std::strerror(errno);
      return nullptr;
    }
    return std::make_unique<FdReader>(fd, take_ownership, static_cast<uint64_t>(end));
  }
  uint64_t sz = static_cast<uint64_t>(st.st_size);
  if (sz == 0) {
    const off_t end = ::lseek(fd, 0, SEEK_END);
    if (end > 0) sz = static_cast<uint64_t>(end);
  }
#endif
  return std::make_unique<FdReader>(fd, take_ownership, sz);
}

std::unique_ptr<StreamReader> StreamReader::from_memory(std::span<const uint8_t> data) {
  return std::make_unique<MemoryReader>(data);
}

std::unique_ptr<StreamReader> StreamReader::from_callback(uint64_t total_size, ReadFn fn) {
  return std::make_unique<CallbackReader>(total_size, std::move(fn));
}

std::string sanitize_relative_path(std::string_view raw) {
  if (raw.empty()) return {};
  std::string out;
  out.reserve(raw.size());
  size_t i = 0;
  while (i < raw.size()) {
    while (i < raw.size() && (raw[i] == '/' || raw[i] == '\\')) ++i;
    if (i >= raw.size()) break;
    const size_t start = i;
    while (i < raw.size() && raw[i] != '/' && raw[i] != '\\') ++i;
    const std::string_view part = raw.substr(start, i - start);
    if (part.empty() || part == ".") continue;
    if (part == "..") return {};
    for (char c : part) {
      const unsigned char uc = static_cast<unsigned char>(c);
      if (uc < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
        return {};
      }
    }
    if (!out.empty()) out.push_back('/');
    out.append(part.data(), part.size());
  }
  return out;
}

}  // namespace setup
