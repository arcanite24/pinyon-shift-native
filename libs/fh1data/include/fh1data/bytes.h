// Endian helpers and a bounds-checked big-endian reader.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace fh1 {

inline uint16_t load_le16(const uint8_t* p) {
#if defined(_M_X64) || defined(_M_IX86) || defined(_M_ARM64) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
  uint16_t v;
  std::memcpy(&v, p, sizeof v);
  return v;
#else
  return uint16_t(p[0] | p[1] << 8);
#endif
}
inline uint32_t load_le32(const uint8_t* p) {
#if defined(_M_X64) || defined(_M_IX86) || defined(_M_ARM64) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
  uint32_t v;
  std::memcpy(&v, p, sizeof v);
  return v;
#else
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
#endif
}
inline uint64_t load_le64(const uint8_t* p) {
#if defined(_M_X64) || defined(_M_IX86) || defined(_M_ARM64) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
  uint64_t v;
  std::memcpy(&v, p, sizeof v);
  return v;
#else
  return uint64_t(load_le32(p)) | uint64_t(load_le32(p + 4)) << 32;
#endif
}
inline uint16_t load_be16(const uint8_t* p) {
#if defined(_MSC_VER)
  uint16_t v;
  std::memcpy(&v, p, sizeof v);
  return _byteswap_ushort(v);
#elif (defined(__GNUC__) || defined(__clang__)) && \
    defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  uint16_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap16(v);
#else
  return uint16_t(p[0] << 8 | p[1]);
#endif
}
inline uint32_t load_be32(const uint8_t* p) {
#if defined(_MSC_VER)
  uint32_t v;
  std::memcpy(&v, p, sizeof v);
  return _byteswap_ulong(v);
#elif (defined(__GNUC__) || defined(__clang__)) && \
    defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  uint32_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap32(v);
#else
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
#endif
}
inline float load_bef32(const uint8_t* p) {
  const uint32_t bits = load_be32(p);
  float value;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

// Thrown by BeReader when a read runs past the end of its data.
class FormatError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Sequential big-endian reader over a byte span; every read is checked.
class BeReader {
 public:
  explicit BeReader(std::span<const uint8_t> data, size_t at = 0) : data_(data), at_(at) {}

  size_t at() const { return at_; }
  size_t size() const { return data_.size(); }
  size_t remaining() const { return at_ <= data_.size() ? data_.size() - at_ : 0; }
  void seek(size_t at) { at_ = at; }

  void need(size_t bytes) const {
    if (at_ > data_.size() || bytes > data_.size() - at_) {
      throw FormatError("read of " + std::to_string(bytes) + " bytes at " + std::to_string(at_) +
                        " runs past the end (" + std::to_string(data_.size()) + ")");
    }
  }
  uint8_t u8() {
    need(1);
    return data_[at_++];
  }
  uint16_t u16() {
    need(2);
    const uint16_t v = load_be16(&data_[at_]);
    at_ += 2;
    return v;
  }
  uint32_t u32() {
    need(4);
    const uint32_t v = load_be32(&data_[at_]);
    at_ += 4;
    return v;
  }
  int32_t i32() { return int32_t(u32()); }
  float f32() {
    need(4);
    const float v = load_bef32(&data_[at_]);
    at_ += 4;
    return v;
  }
  std::span<const uint8_t> raw(size_t bytes) {
    need(bytes);
    auto out = data_.subspan(at_, bytes);
    at_ += bytes;
    return out;
  }
  void skip(size_t bytes) { raw(bytes); }

 private:
  std::span<const uint8_t> data_;
  size_t at_;
};

}  // namespace fh1
