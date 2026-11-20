// Small helpers for the car tools: a work-stealing parallel loop and SHA-256.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace cars {

// Runs fn(i) for i in [0, n) on up to `threads` threads (0: hardware
// concurrency). Items are taken in index order; results must be stored by
// index for a deterministic outcome.
void parallel_for(size_t n, unsigned threads, const std::function<void(size_t)>& fn);

class Sha256 {
 public:
  Sha256();
  void update(std::span<const uint8_t> data);
  std::array<uint8_t, 32> finish();
  static std::string hex(const std::array<uint8_t, 32>& digest);

 private:
  void block(const uint8_t* p);
  std::array<uint32_t, 8> h_;
  std::array<uint8_t, 64> buffer_{};
  size_t used_ = 0;
  uint64_t length_ = 0;
};

}  // namespace cars
