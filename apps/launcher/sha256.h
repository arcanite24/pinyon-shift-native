#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace launcher {

class Sha256 {
 public:
  Sha256();
  void update(const uint8_t* data, size_t len);
  void update(std::span<const uint8_t> data) { update(data.data(), data.size()); }
  void update(std::string_view s) {
    update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }
  std::array<uint8_t, 32> finish();

 private:
  void transform(const uint8_t block[64]);
  uint32_t state_[8];
  uint64_t bit_len_ = 0;
  uint8_t buffer_[64]{};
  size_t buffer_len_ = 0;
};

std::string to_hex_lower(const std::array<uint8_t, 32>& digest);
std::string sha256_bytes(std::span<const uint8_t> data);
std::string sha256_string(std::string_view text);
bool sha256_file(const std::filesystem::path& path, std::string& out_hex, std::string* error = nullptr);

// IEEE 802.3 CRC-32 (used by Steam non-Steam shortcut AppID calculation).
uint32_t crc32_bytes(std::span<const uint8_t> data);
uint32_t crc32_string(std::string_view text);

}  // namespace launcher
