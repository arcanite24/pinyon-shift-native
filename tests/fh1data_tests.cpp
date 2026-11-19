// Unit tests for fh1data on synthetic bytes only (no game data).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "fh1data/archive.h"
#include "fh1data/carbin.h"
#include "fh1data/xds.h"
#include "fh1data/xmem.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

// Wraps bytes in LZX uncompressed (type 3) blocks inside XMem frames, one
// per 32 KiB, as an encoder that stores literally would.
std::vector<uint8_t> xmem_stored(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out;
  for (size_t offset = 0; offset < data.size(); offset += 0x8000) {
    const size_t n = std::min<size_t>(0x8000, data.size() - offset);
    std::string bits = offset == 0 ? "0" : "";  // no E8 translation (first frame only)
    bits += "011";                               // block type 3: uncompressed
    for (int b = 23; b >= 0; --b) bits += ((n >> b) & 1) ? '1' : '0';
    while (bits.size() % 16) bits += '0';
    std::vector<uint8_t> block;
    for (size_t i = 0; i < bits.size(); i += 16) {
      const uint16_t word = uint16_t(std::stoul(bits.substr(i, 16), nullptr, 2));
      block.push_back(uint8_t(word & 0xFF));
      block.push_back(uint8_t(word >> 8));
    }
    for (int r = 0; r < 3; ++r) block.insert(block.end(), {1, 0, 0, 0});  // R0-R2
    block.insert(block.end(), data.begin() + offset, data.begin() + offset + n);
    if (n % 2) block.push_back(0);
    if (n == 0x8000) {
      out.push_back(uint8_t(block.size() >> 8));
      out.push_back(uint8_t(block.size()));
    } else {
      out.insert(out.end(), {0xFF, uint8_t(n >> 8), uint8_t(n), uint8_t(block.size() >> 8),
                             uint8_t(block.size())});
    }
    out.insert(out.end(), block.begin(), block.end());
  }
  return out;
}

void test_xmem() {
  std::vector<uint8_t> data(70000);
  for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t((i * 7919) >> 3);
  const auto packed = xmem_stored(data);
  const auto out = fh1::xmem_decompress(packed, data.size(), fh1::crc32(data));
  CHECK(out.has_value());
  CHECK(out && *out == data);
  const auto bad = fh1::xmem_decompress(packed, data.size(), fh1::crc32(data) ^ 1);
  CHECK(!bad.has_value());
}

void test_crc() {
  const std::string s = "123456789";
  CHECK(fh1::crc32({reinterpret_cast<const uint8_t*>(s.data()), s.size()}) == 0xCBF43926u);
}

void test_strip() {
  fh1::CarbinSubsection s;
  s.index_type = 6;
  s.index_size = 2;
  s.indices = {0, 1, 2, 3, 0xFFFF, 4, 5, 6, 6, 7};
  const auto tri = fh1::carbin_triangle_list(s);
  // 0,1,2 / 2,1,3 (flipped) / 4,5,6 ; 5,6,6 and 6,6,7 are degenerate.
  CHECK((tri == std::vector<uint32_t>{0, 1, 2, 2, 1, 3, 4, 5, 6}));
  s.index_size = 4;
  s.indices = {0, 1, 2, 0x00FFFFFF, 3, 4, 5};
  CHECK(fh1::carbin_triangle_list(s).size() == 6);
}

void test_position() {
  fh1::CarbinPart part;
  part.bounds = {1, 2, 3, -3, -4, 0, 3, 4, 0};  // |half-extent| = 5
  fh1::CarbinVertex v{};
  v.position = {32767, 0, 0, 32767};
  auto p = fh1::carbin_position(part, v);
  CHECK(std::fabs(p[0] - 6) < 1e-4f && std::fabs(p[1] - 2) < 1e-4f && std::fabs(p[2] - 3) < 1e-4f);
  v.frame = {0, 0, 0, 32767};  // identity: normal = +X
  auto n = fh1::carbin_normal(v);
  CHECK(std::fabs(n[0] - 1) < 1e-4f);
}

void test_tiled_offset() {
  // Every block of a 64x64-block surface maps to a distinct offset.
  std::vector<bool> seen(64 * 64 * 16, false);
  bool unique = true;
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x) {
      const int32_t o = fh1::xenos_tiled_offset_2d(x, y, 64, 4);
      if (o < 0 || size_t(o) / 16 >= seen.size() / 16 || o % 16 || seen[size_t(o)]) unique = false;
      else seen[size_t(o)] = true;
    }
  CHECK(unique);
}

}  // namespace

int main() {
  test_crc();
  test_xmem();
  test_strip();
  test_position();
  test_tiled_offset();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all fh1data tests passed\n");
  return 0;
}
