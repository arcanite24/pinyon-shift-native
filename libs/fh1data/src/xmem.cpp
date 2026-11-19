#include "fh1data/xmem.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "fh1data/archive.h"

extern "C" {
#include <lzx.h>
#include <mspack.h>
}

namespace fh1 {
namespace {

// libmspack reads and writes through an mspack_system; these route it to
// memory. Each "file" is a span with a cursor.
struct MemoryFile {
  uint8_t* data;
  size_t size;
  size_t at;
};

int memory_read(mspack_file* file, void* buffer, int bytes) {
  auto* f = reinterpret_cast<MemoryFile*>(file);
  const size_t n = std::min<size_t>(size_t(std::max(bytes, 0)), f->size - f->at);
  std::memcpy(buffer, f->data + f->at, n);
  f->at += n;
  return int(n);
}

int memory_write(mspack_file* file, void* buffer, int bytes) {
  auto* f = reinterpret_cast<MemoryFile*>(file);
  const size_t n = std::min<size_t>(size_t(std::max(bytes, 0)), f->size - f->at);
  std::memcpy(f->data + f->at, buffer, n);
  f->at += n;
  // A short write tells lzxd the output is full, which it treats as an error.
  return int(n);
}

void* memory_alloc(mspack_system*, size_t bytes) { return std::calloc(bytes, 1); }
void memory_free(void* pointer) { std::free(pointer); }
void memory_copy(void* source, void* destination, size_t bytes) {
  std::memmove(destination, source, bytes);
}

mspack_system make_system() {
  mspack_system system{};
  system.read = memory_read;
  system.write = memory_write;
  system.alloc = memory_alloc;
  system.free = memory_free;
  system.copy = memory_copy;
  return system;
}

}  // namespace

bool xmem_unframe(std::span<const uint8_t> compressed, size_t uncompressed_size,
                  std::vector<uint8_t>& lzx) {
  lzx.clear();
  lzx.reserve(compressed.size());
  size_t at = 0;
  size_t produced = 0;
  while (produced < uncompressed_size) {
    size_t frame_compressed = 0;
    size_t frame_output = 0;
    if (at < compressed.size() && compressed[at] == 0xFF) {
      if (compressed.size() - at < 5) return false;
      frame_output = size_t(compressed[at + 1]) << 8 | compressed[at + 2];
      frame_compressed = size_t(compressed[at + 3]) << 8 | compressed[at + 4];
      at += 5;
    } else {
      if (compressed.size() - at < 2) return false;
      frame_output = std::min<size_t>(0x8000, uncompressed_size - produced);
      frame_compressed = size_t(compressed[at]) << 8 | compressed[at + 1];
      at += 2;
    }
    if (!frame_compressed || !frame_output || frame_compressed > compressed.size() - at ||
        frame_output > uncompressed_size - produced) {
      return false;
    }
    lzx.insert(lzx.end(), compressed.begin() + at, compressed.begin() + at + frame_compressed);
    at += frame_compressed;
    produced += frame_output;
  }
  return true;
}

bool lzx_decode(std::span<const uint8_t> lzx, std::span<uint8_t> output, int window_bits) {
  mspack_system system = make_system();
  MemoryFile input{const_cast<uint8_t*>(lzx.data()), lzx.size(), 0};
  MemoryFile sink{output.data(), output.size(), 0};
  lzxd_stream* stream =
      lzxd_init(&system, reinterpret_cast<mspack_file*>(&input),
                reinterpret_cast<mspack_file*>(&sink), window_bits, 0, 0x8000,
                off_t(output.size()), 0);
  if (!stream) return false;
  const int result = lzxd_decompress(stream, off_t(output.size()));
  lzxd_free(stream);
  return result == MSPACK_ERR_OK && sink.at == output.size();
}

std::optional<std::vector<uint8_t>> xmem_decompress(std::span<const uint8_t> compressed,
                                                    size_t uncompressed_size,
                                                    std::optional<uint32_t> expected_crc,
                                                    int* window_bits_used) {
  std::vector<uint8_t> lzx;
  if (!xmem_unframe(compressed, uncompressed_size, lzx)) return std::nullopt;
  std::vector<uint8_t> output(uncompressed_size);
  for (int window_bits : {17, 16, 15}) {
    if (!lzx_decode(lzx, output, window_bits)) continue;
    if (expected_crc && crc32(output) != *expected_crc) continue;
    if (window_bits_used) *window_bits_used = window_bits;
    return output;
  }
  return std::nullopt;
}

}  // namespace fh1
