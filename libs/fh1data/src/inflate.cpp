#include "fh1data/inflate.h"

#include <array>

namespace fh1 {
namespace {

class Bits {
 public:
  explicit Bits(std::span<const uint8_t> d) : data_(d) {}
  bool get(uint32_t n, uint32_t& out) {
    while (count_ < n) {
      if (at_ >= data_.size()) return false;
      buffer_ |= uint64_t(data_[at_++]) << count_;
      count_ += 8;
    }
    out = uint32_t(buffer_ & ((uint64_t(1) << n) - 1));
    buffer_ >>= n;
    count_ -= n;
    return true;
  }
  void align() {
    const uint32_t drop = count_ % 8;
    buffer_ >>= drop;
    count_ -= drop;
  }
  // Byte-aligned read after align(): drains whole buffered bytes first.
  bool byte(uint8_t& out) {
    uint32_t v;
    if (!get(8, v)) return false;
    out = uint8_t(v);
    return true;
  }

 private:
  std::span<const uint8_t> data_;
  size_t at_ = 0;
  uint64_t buffer_ = 0;
  uint32_t count_ = 0;
};

// Canonical Huffman decoding table (counts per length, symbols by code).
struct Huffman {
  std::array<uint16_t, 16> counts{};
  std::vector<uint16_t> symbols;
  bool build(const uint8_t* lengths, size_t n) {
    counts.fill(0);
    for (size_t i = 0; i < n; ++i) ++counts[lengths[i]];
    counts[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
      left = left * 2 - counts[len];
      if (left < 0) return false;  // over-subscribed
    }
    std::array<uint16_t, 16> offsets{};
    for (int len = 1; len < 15; ++len) offsets[len + 1] = uint16_t(offsets[len] + counts[len]);
    symbols.assign(n, 0);
    for (size_t i = 0; i < n; ++i)
      if (lengths[i]) symbols[offsets[lengths[i]]++] = uint16_t(i);
    return true;
  }
  bool decode(Bits& bits, uint32_t& symbol) const {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      uint32_t b;
      if (!bits.get(1, b)) return false;
      code |= int(b);
      const int count = counts[len];
      if (code - count < first) {
        symbol = symbols[size_t(index + (code - first))];
        return true;
      }
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    return false;
  }
};

constexpr std::array<uint16_t, 29> kLengthBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                                  15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                                  67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<uint8_t, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<uint16_t, 30> kDistBase = {1,    2,    3,    4,    5,    7,     9,     13,
                                                17,   25,   33,   49,   65,   97,    129,   193,
                                                257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                                4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<uint8_t, 30> kDistExtra = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits& bits, const Huffman& lit, const Huffman& dist, std::vector<uint8_t>& out,
           size_t limit) {
  for (;;) {
    uint32_t sym;
    if (!lit.decode(bits, sym)) return false;
    if (sym < 256) {
      if (out.size() >= limit) return false;
      out.push_back(uint8_t(sym));
    } else if (sym == 256) {
      return true;
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      uint32_t extra;
      if (!bits.get(kLengthExtra[sym], extra)) return false;
      const size_t length = kLengthBase[sym] + extra;
      uint32_t ds;
      if (!dist.decode(bits, ds) || ds >= 30) return false;
      if (!bits.get(kDistExtra[ds], extra)) return false;
      const size_t distance = kDistBase[ds] + extra;
      if (distance > out.size() || out.size() + length > limit) return false;
      const size_t from = out.size() - distance;
      for (size_t i = 0; i < length; ++i) out.push_back(out[from + i]);
    }
  }
}

}  // namespace

std::optional<std::vector<uint8_t>> inflate_raw(std::span<const uint8_t> data, size_t size) {
  Bits bits(data);
  std::vector<uint8_t> out;
  out.reserve(size);
  static const auto fixed = [] {
    std::array<uint8_t, 288 + 30> lengths{};
    for (int i = 0; i < 144; ++i) lengths[size_t(i)] = 8;
    for (int i = 144; i < 256; ++i) lengths[size_t(i)] = 9;
    for (int i = 256; i < 280; ++i) lengths[size_t(i)] = 7;
    for (int i = 280; i < 288; ++i) lengths[size_t(i)] = 8;
    for (int i = 0; i < 30; ++i) lengths[288 + size_t(i)] = 5;
    std::pair<Huffman, Huffman> h;
    h.first.build(lengths.data(), 288);
    h.second.build(lengths.data() + 288, 30);
    return h;
  }();
  uint32_t last = 0;
  do {
    uint32_t type;
    if (!bits.get(1, last) || !bits.get(2, type)) return std::nullopt;
    if (type == 0) {
      bits.align();
      uint8_t b[4];
      for (uint8_t& x : b)
        if (!bits.byte(x)) return std::nullopt;
      const uint32_t len = uint32_t(b[0] | b[1] << 8), nlen = uint32_t(b[2] | b[3] << 8);
      if ((len ^ 0xFFFF) != nlen || out.size() + len > size) return std::nullopt;
      for (uint32_t i = 0; i < len; ++i) {
        uint8_t x;
        if (!bits.byte(x)) return std::nullopt;
        out.push_back(x);
      }
    } else if (type == 1) {
      if (!codes(bits, fixed.first, fixed.second, out, size)) return std::nullopt;
    } else if (type == 2) {
      uint32_t hlit, hdist, hclen;
      if (!bits.get(5, hlit) || !bits.get(5, hdist) || !bits.get(4, hclen)) return std::nullopt;
      hlit += 257;
      hdist += 1;
      hclen += 4;
      if (hlit > 286 || hdist > 30) return std::nullopt;
      static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                             11, 4,  12, 3, 13, 2, 14, 1, 15};
      uint8_t code_lengths[19] = {};
      for (uint32_t i = 0; i < hclen; ++i) {
        uint32_t v;
        if (!bits.get(3, v)) return std::nullopt;
        code_lengths[kOrder[i]] = uint8_t(v);
      }
      Huffman code;
      if (!code.build(code_lengths, 19)) return std::nullopt;
      std::array<uint8_t, 320> lengths{};
      for (uint32_t i = 0; i < hlit + hdist;) {
        uint32_t sym;
        if (!code.decode(bits, sym)) return std::nullopt;
        if (sym < 16) {
          lengths[i++] = uint8_t(sym);
          continue;
        }
        uint32_t repeat = 0, value = 0, extra;
        if (sym == 16) {
          if (i == 0 || !bits.get(2, extra)) return std::nullopt;
          value = lengths[i - 1];
          repeat = 3 + extra;
        } else if (sym == 17) {
          if (!bits.get(3, extra)) return std::nullopt;
          repeat = 3 + extra;
        } else {
          if (!bits.get(7, extra)) return std::nullopt;
          repeat = 11 + extra;
        }
        if (i + repeat > hlit + hdist) return std::nullopt;
        while (repeat--) lengths[i++] = uint8_t(value);
      }
      Huffman lit, dist;
      if (!lit.build(lengths.data(), hlit) || !dist.build(lengths.data() + hlit, hdist))
        return std::nullopt;
      if (!codes(bits, lit, dist, out, size)) return std::nullopt;
    } else {
      return std::nullopt;
    }
  } while (!last);
  if (out.size() != size) return std::nullopt;
  return out;
}

}  // namespace fh1
