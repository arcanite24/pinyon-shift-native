// 64-bit random-access stream reader supporting file paths, POSIX file
// descriptors (Android SAF ParcelFileDescriptor.detachFd), in-memory buffers,
// and custom virtual readers (for sparse image unit tests).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace setup {

class StreamReader {
 public:
  using ReadFn = std::function<bool(uint64_t offset, void* dst, size_t len, std::string* error)>;

  virtual ~StreamReader() = default;

  virtual uint64_t size() const = 0;
  virtual bool read_at(uint64_t offset, void* dst, size_t len, std::string* error = nullptr) const = 0;

  bool read_at(uint64_t offset, size_t len, std::vector<uint8_t>& out, std::string* error = nullptr) const {
    out.resize(len);
    if (len == 0) return true;
    return read_at(offset, out.data(), len, error);
  }

  static std::unique_ptr<StreamReader> open_path(const std::filesystem::path& path, std::string* error = nullptr);
  static std::unique_ptr<StreamReader> from_fd(int fd, bool take_ownership = true, std::string* error = nullptr);
  static std::unique_ptr<StreamReader> from_memory(std::span<const uint8_t> data);
  static std::unique_ptr<StreamReader> from_callback(uint64_t total_size, ReadFn fn);
};

// Sanitizes a relative archive/container path, rejecting traversal (".."),
// drive letters, control characters, and leading separators. Returns empty
// string if the path is unsafe.
std::string sanitize_relative_path(std::string_view raw);

}  // namespace setup
