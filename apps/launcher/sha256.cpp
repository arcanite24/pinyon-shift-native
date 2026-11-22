#include "sha256.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace launcher {
namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffa,  0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32u - n)); }

}  // namespace

Sha256::Sha256() {
  state_[0] = 0x6a09e667u;
  state_[1] = 0xbb67ae85u;
  state_[2] = 0x3c6ef372u;
  state_[3] = 0xa54ff53au;
  state_[4] = 0x510e527fu;
  state_[5] = 0x9b05688cu;
  state_[6] = 0x1f83d9abu;
  state_[7] = 0x5be0cd19u;
}

void Sha256::transform(const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
           (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = h + S1 + ch + kK[i] + w[i];
    const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;
  bit_len_ += uint64_t(len) * 8u;
  size_t i = 0;
  if (buffer_len_ > 0) {
    const size_t take = std::min(len, size_t(64) - buffer_len_);
    std::memcpy(buffer_ + buffer_len_, data, take);
    buffer_len_ += take;
    i += take;
    if (buffer_len_ == 64) {
      transform(buffer_);
      buffer_len_ = 0;
    }
  }
  while (i + 64 <= len) {
    transform(data + i);
    i += 64;
  }
  if (i < len) {
    buffer_len_ = len - i;
    std::memcpy(buffer_, data + i, buffer_len_);
  }
}

std::array<uint8_t, 32> Sha256::finish() {
  uint8_t pad[64]{};
  pad[0] = 0x80u;
  const size_t pad_len = (buffer_len_ < 56) ? (56 - buffer_len_) : (120 - buffer_len_);
  const uint64_t total_bits = bit_len_;
  if (pad_len <= 64) {
    update(pad, pad_len);
  } else {
    update(pad, 64);
    uint8_t zeros[64]{};
    update(zeros, pad_len - 64);
  }
  uint8_t len_be[8];
  for (int i = 0; i < 8; ++i) len_be[i] = uint8_t((total_bits >> ((7 - i) * 8)) & 0xFFu);
  // Write length directly without altering total_bits.
  std::memcpy(buffer_ + buffer_len_, len_be, 8);
  transform(buffer_);
  buffer_len_ = 0;

  std::array<uint8_t, 32> out{};
  for (int i = 0; i < 8; ++i) {
    out[size_t(i * 4 + 0)] = uint8_t((state_[i] >> 24) & 0xFFu);
    out[size_t(i * 4 + 1)] = uint8_t((state_[i] >> 16) & 0xFFu);
    out[size_t(i * 4 + 2)] = uint8_t((state_[i] >> 8) & 0xFFu);
    out[size_t(i * 4 + 3)] = uint8_t(state_[i] & 0xFFu);
  }
  return out;
}

std::string to_hex_lower(const std::array<uint8_t, 32>& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(64, '0');
  for (size_t i = 0; i < 32; ++i) {
    out[i * 2 + 0] = kHex[(digest[i] >> 4) & 0xFu];
    out[i * 2 + 1] = kHex[digest[i] & 0xFu];
  }
  return out;
}

std::string sha256_bytes(std::span<const uint8_t> data) {
  Sha256 ctx;
  ctx.update(data);
  return to_hex_lower(ctx.finish());
}

std::string sha256_string(std::string_view text) {
  Sha256 ctx;
  ctx.update(text);
  return to_hex_lower(ctx.finish());
}

bool sha256_file(const std::filesystem::path& path, std::string& out_hex, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot open " + path.string();
    return false;
  }
  Sha256 ctx;
  std::vector<uint8_t> buf(1 << 16);
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
    const auto got = in.gcount();
    if (got > 0) ctx.update(buf.data(), size_t(got));
  }
  out_hex = to_hex_lower(ctx.finish());
  return true;
}

uint32_t crc32_bytes(std::span<const uint8_t> data) {
  uint32_t crc = 0xFFFFFFFFu;
  for (uint8_t b : data) {
    crc ^= uint32_t(b);
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = uint32_t(-(int32_t(crc & 1u)));
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

uint32_t crc32_string(std::string_view text) {
  return crc32_bytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

}  // namespace launcher
