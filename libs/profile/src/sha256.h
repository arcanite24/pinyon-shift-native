// SHA-256 (FIPS 180-4), for save hashes and sync archives. Internal to
// shift_profile.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace profile::detail {

class Sha256 {
 public:
  Sha256();
  void update(std::span<const uint8_t> data);
  void update(const std::string& s) { update(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size())); }
  std::array<uint8_t, 32> finish();
  // Upper-case hex of the digest.
  std::string hex();

 private:
  void block(const uint8_t* p);
  std::array<uint32_t, 8> h_;
  std::array<uint8_t, 64> buf_{};
  size_t used_ = 0;
  uint64_t bytes_ = 0;
};

std::string sha256_hex(std::span<const uint8_t> data);

}  // namespace profile::detail
