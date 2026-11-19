// Unit tests for fh1data on synthetic bytes only (no game data).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "fh1data/archive.h"
#include "fh1data/carbin.h"
#include "fh1data/rmb.h"
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

// --- archives ---------------------------------------------------------------

void put_le16(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
}
void put_le32(std::vector<uint8_t>& out, uint32_t v) {
  put_le16(out, v & 0xFFFF);
  put_le16(out, v >> 16);
}

// A stored ZIP with more members than the end record's 16-bit count holds:
// the count wraps, the directory size and offset stay right.
void test_archive_wrapped_count() {
  constexpr uint32_t kMembers = 65536 + 70;
  std::vector<uint8_t> zip, directory;
  const std::string last_payload = "last member";
  for (uint32_t i = 0; i < kMembers; ++i) {
    char name[16];
    std::snprintf(name, sizeof name, "m%06u.bin", i);
    const uint32_t name_size = uint32_t(std::strlen(name));
    const std::string payload = i + 1 == kMembers ? last_payload : "";
    const uint32_t crc =
        fh1::crc32({reinterpret_cast<const uint8_t*>(payload.data()), payload.size()});
    const uint32_t size = uint32_t(payload.size());
    const uint32_t offset = uint32_t(zip.size());
    put_le32(zip, 0x04034B50);
    for (uint32_t v : {20u, 0u, 0u, 0u, 0u}) put_le16(zip, v);
    put_le32(zip, crc);
    put_le32(zip, size);
    put_le32(zip, size);
    put_le16(zip, name_size);
    put_le16(zip, 0);
    zip.insert(zip.end(), name, name + name_size);
    zip.insert(zip.end(), payload.begin(), payload.end());
    put_le32(directory, 0x02014B50);
    for (uint32_t v : {20u, 20u, 0u, 0u, 0u, 0u}) put_le16(directory, v);
    put_le32(directory, crc);
    put_le32(directory, size);
    put_le32(directory, size);
    for (uint32_t v : {name_size, 0u, 0u, 0u, 0u}) put_le16(directory, v);
    put_le32(directory, 0);
    put_le32(directory, offset);
    directory.insert(directory.end(), name, name + name_size);
  }
  const uint32_t directory_offset = uint32_t(zip.size());
  zip.insert(zip.end(), directory.begin(), directory.end());
  put_le32(zip, 0x06054B50);
  put_le16(zip, 0);
  put_le16(zip, 0);
  put_le16(zip, kMembers & 0xFFFF);  // wrapped: 70
  put_le16(zip, kMembers & 0xFFFF);
  put_le32(zip, uint32_t(directory.size()));
  put_le32(zip, directory_offset);
  put_le16(zip, 0);

  const auto path = std::filesystem::temp_directory_path() / "shift-engine-wrapped-count.zip";
  {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(zip.data()), std::streamsize(zip.size()));
  }
  std::string error;
  auto archive = fh1::Archive::open(path, &error);
  CHECK(archive != nullptr);
  if (archive) {
    CHECK(archive->members().size() == kMembers);
    std::set<std::string> names;
    for (const auto& m : archive->members()) names.insert(fh1::normalize_member_name(m.name));
    CHECK(names.size() == kMembers);
    char last[16];
    std::snprintf(last, sizeof last, "M%06u.BIN", kMembers - 1);  // lookup ignores case
    const auto read = archive->read(last);
    CHECK(read.ok());
    CHECK(std::string(read.data.begin(), read.data.end()) == last_payload);
  }
  archive.reset();
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

// --- render models ----------------------------------------------------------

struct BeWriter {
  std::vector<uint8_t> bytes;
  void u32(uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) bytes.push_back(uint8_t(v >> s));
  }
  void u16(uint32_t v) {
    bytes.push_back(uint8_t(v >> 8));
    bytes.push_back(uint8_t(v));
  }
  void f32(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    u32(v);
  }
  void vec3_pad(float x, float y, float z) {
    f32(x);
    f32(y);
    f32(z);
    u32(0);
  }
  void name(const std::string& s) {
    u32(uint32_t(s.size()));
    bytes.insert(bytes.end(), s.begin(), s.end());
  }
};

struct SyntheticSubmesh {
  uint32_t primitive;
  uint32_t index_size;
  std::vector<uint32_t> indices;
};

// One object of `count` vertices at (i, 2i, -i) with `stride` bytes each
// (the rest of a vertex is filler), the given submeshes, then four bytes
// standing in for the material section, which is not geometry.
std::vector<uint8_t> synthetic_model(uint32_t count, uint32_t stride,
                                     const std::vector<SyntheticSubmesh>& submeshes) {
  BeWriter w;
  w.u32(6);
  w.vec3_pad(0, 0, -float(count));
  w.vec3_pad(float(count), 2 * float(count), 0);
  for (int i = 0; i < 16; ++i) w.f32(i % 5 == 0 ? 1.0f : 0.0f);
  for (uint32_t v : {4u, 4u, 0u, 1u}) w.u32(v);
  w.u32(1);  // mesh objects
  w.u32(1);
  w.vec3_pad(1, 2, -1);
  w.vec3_pad(-1, -2, -1);
  w.vec3_pad(1, 2, 1);
  w.u32(1);
  w.name("Synthetic_Object_LOD01");
  for (uint32_t v : {3u, count, stride, 0u}) w.u32(v);
  for (uint32_t i = 0; i < count; ++i) {
    w.f32(float(i));
    w.f32(2.0f * float(i));
    w.f32(-float(i));
    for (uint32_t b = 12; b < stride; ++b) w.bytes.push_back(uint8_t(0xA0 + b));
  }
  w.u32(1);
  w.u32(uint32_t(submeshes.size()));
  uint32_t material = 0;
  for (const auto& s : submeshes) {
    w.u32(1);
    w.u32(2);
    w.name("material_" + std::to_string(material));
    for (uint32_t v : {0u, s.primitive, material++, 1u}) w.u32(v);
    for (int i = 0; i < 4; ++i) w.u32(0);
    for (int i = 0; i < 4; ++i) w.f32(1.0f);
    for (float f : {0.0f, 0.0f, 0.0f, 10.0f}) w.f32(f);
    w.u32(4);
    w.u32(0);
    w.u32(uint32_t(s.indices.size()));
    w.u32(s.index_size);
    for (uint32_t i : s.indices) {
      if (s.index_size == 2) w.u16(i);
      else w.u32(i);
    }
    w.u32(5);
  }
  w.u32(0xDEADBEEF);
  return w.bytes;
}

void test_render_model() {
  const std::vector<SyntheticSubmesh> subs = {
      {6, 2, {0, 1, 2, 3, 0xFFFF, 4, 5, 6}},
      {4, 4, {0, 2, 4, 1, 3, 5}},
  };
  const auto bytes = synthetic_model(7, 20, subs);
  fh1::RenderModel model;
  std::string error;
  CHECK(fh1::parse_render_model(bytes, model, &error));
  CHECK(error.empty());
  CHECK(model.kind == 6);
  CHECK(model.box_max[1] == 14.0f && model.box_min[2] == -7.0f);
  CHECK(model.objects.size() == 1);
  CHECK(model.geometry_end == bytes.size() - 4);
  if (model.objects.size() == 1) {
    const auto& o = model.objects[0];
    CHECK(o.name == "Synthetic_Object_LOD01");
    CHECK(o.vertex_count == 7 && o.stride == 20 && o.vertices.size() == 140);
    CHECK(o.vertices[12] == 0xAC);  // the undecoded bytes are kept
    const auto p = o.position(5);
    CHECK(p[0] == 5.0f && p[1] == 10.0f && p[2] == -5.0f);
    CHECK(o.submeshes.size() == 2);
    if (o.submeshes.size() == 2) {
      CHECK(o.submeshes[0].primitive == 6 && o.submeshes[0].index_size == 2);
      CHECK(o.submeshes[0].indices.size() == 8 && o.submeshes[0].indices[4] == 0xFFFF);
      CHECK(o.submeshes[0].trailer == 5);
      CHECK((fh1::rmb_triangle_list(o.submeshes[0]) ==
             std::vector<uint32_t>{0, 1, 2, 2, 1, 3, 4, 5, 6}));
      CHECK(o.submeshes[1].index_size == 4 && o.submeshes[1].material == "material_1");
      CHECK(fh1::rmb_triangle_list(o.submeshes[1]) == subs[1].indices);
    }
  }

  // Every truncation is an error with a reason, never a crash.
  size_t rejected = 0;
  for (size_t n = 0; n < model.geometry_end; ++n) {
    fh1::RenderModel m;
    std::string why;
    if (!fh1::parse_render_model(std::span(bytes).first(n), m, &why) && !why.empty()) ++rejected;
  }
  CHECK(rejected == model.geometry_end);

  // An index past the vertices, an index size of 3, an unknown primitive,
  // and a restart value in a list (only strips restart).
  const std::vector<std::vector<SyntheticSubmesh>> bad = {
      {{4, 2, {0, 1, 7}}}, {{4, 3, {0, 1, 2}}}, {{5, 2, {0, 1, 2}}}, {{4, 2, {0, 0xFFFF, 1}}}};
  for (const auto& b : bad) {
    fh1::RenderModel m;
    std::string why;
    CHECK(!fh1::parse_render_model(synthetic_model(7, 16, b), m, &why));
    CHECK(!why.empty());
  }
  auto wrong_kind = bytes;
  wrong_kind[3] = 7;
  fh1::RenderModel m;
  CHECK(!fh1::parse_render_model(wrong_kind, m, &error));
}

}  // namespace

int main() {
  test_crc();
  test_xmem();
  test_strip();
  test_position();
  test_tiled_offset();
  test_archive_wrapped_count();
  test_render_model();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all fh1data tests passed\n");
  return 0;
}
