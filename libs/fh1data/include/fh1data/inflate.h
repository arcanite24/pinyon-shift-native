// Raw deflate (RFC 1951) decoder for ZIP method 8 members: stored, fixed
// and dynamic Huffman blocks. Written from the RFC.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fh1 {

// Decodes a raw deflate stream that must produce exactly `size` bytes.
std::optional<std::vector<uint8_t>> inflate_raw(std::span<const uint8_t> data, size_t size);

}  // namespace fh1
