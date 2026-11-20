// Unit tests for fh1data on synthetic bytes only (no game data).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <span>
#include <utility>
#include <string>
#include <vector>

#include "cars/bake.h"
#include "cars/bake_format.h"
#include "cars/sources.h"
#include "fh1data/archive.h"
#include "fh1data/carbin.h"
#include "fh1data/inflate.h"
#include "fh1data/placements.h"
#include "fh1data/rmb.h"
#include "fh1data/xds.h"
#include "fh1data/xmem.h"
#include "reference.h"

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

// --- placements -------------------------------------------------------------

void pad_to(BeWriter& w, size_t alignment) {
  while (w.bytes.size() % alignment) w.bytes.push_back(0xEE);  // filler, not zero
}

void put_be32_at(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int s = 0; s < 4; ++s) b[at + s] = uint8_t(v >> (24 - 8 * s));
}

void test_half() {
  CHECK(fh1::half_to_float(0x3C00) == 1.0f);
  CHECK(fh1::half_to_float(0xC000) == -2.0f);
  CHECK(fh1::half_to_float(0x4900) == 10.0f);
  CHECK(fh1::half_to_float(0x0000) == 0.0f);
  CHECK(fh1::half_to_float(0x0001) == std::ldexp(1.0f, -24));
  CHECK(std::isinf(fh1::half_to_float(0x7C00)));
  CHECK(std::isnan(fh1::half_to_float(0x7E00)));
}

// An FPVS file: magic, version, a header of odd length, then the 28-byte
// records, the shader names and the object table.
std::vector<uint8_t> synthetic_pvs(uint32_t objects) {
  BeWriter w;
  w.bytes = {'F', 'P', 'V', 'S'};
  w.u32(50);
  for (int i = 0; i < 7; ++i) w.bytes.push_back(uint8_t(0x30 + i));  // undescribed header bytes
  w.u32(3);
  for (uint32_t k = 0; k < 3; ++k) {
    w.u32(10 + 5 * k);
    w.u32(k);
    w.f32(1.0f);
    w.f32(1.0f);
    for (int i = 0; i < 10; ++i) w.bytes.push_back(0);
    w.bytes.push_back(1);
    w.bytes.push_back(uint8_t(k % 2 ? 4 : 0));
  }
  w.u32(2);
  w.name(std::string("shaders\\track\\one") + '\0');
  w.name("shaders\\track\\two");
  w.u32(objects);
  for (uint32_t i = 0; i < objects; ++i) {
    w.u16(100 + i);
    w.u16(0xFFFE);
    w.u16(0x0005);
    w.u16(0x0007);
    w.u32(i);
    w.u32(0xFFFFFFFF);
    w.u16(0xFFFF);
  }
  for (int i = 0; i < 9; ++i) w.u32(0xABCD0000u + i);  // what follows the table
  return w.bytes;
}

void test_object_table() {
  const auto bytes = synthetic_pvs(4);
  fh1::PvsObjectTable t;
  std::string error;
  CHECK(fh1::parse_object_table(bytes, t, &error));
  CHECK(t.version == 50 && t.record_table_offset == 15 && t.record_count == 3);
  CHECK(t.shaders.size() == 2 && t.shaders[0] == "shaders\\track\\one");
  CHECK(t.objects.size() == 4);
  if (t.objects.size() == 4) {
    CHECK(t.objects[2].member == 102 && t.objects[2].field_2 == -2 && t.objects[2].field_8 == 2);
    CHECK(t.objects[3].field_16 == 0xFFFF);
  }
  auto bad = bytes;
  bad[0] = 'X';
  CHECK(!fh1::parse_object_table(bad, t, &error) && !error.empty());
  // A record numbered out of order: no table is found.
  bad = bytes;
  bad[15 + 4 + 28 + 7] = 9;
  error.clear();
  CHECK(!fh1::parse_object_table(bad, t, &error) && !error.empty());
  // Truncated inside the object records.
  error.clear();
  CHECK(!fh1::parse_object_table(std::span(bytes).first(bytes.size() - 9 * 4 - 5), t, &error));
  CHECK(!error.empty());
}

struct SyntheticInstance {
  float rows[3][3];
  float t[3];
};

// A kind-7 tile with two model records (the second with a string and no
// IDs), two references and two groups: the instances of record 0 and one
// 32-byte record, then no instances of record 1. `long_header` uses the
// 0x98-byte header.
std::vector<uint8_t> synthetic_pgeo(bool long_header, const std::vector<SyntheticInstance>& in) {
  BeWriter w;
  w.bytes = {'O', 'E', 'G', 'P'};
  w.u32(1);
  w.f32(64);
  w.f32(64);
  w.vec3_pad(-10, 0, 20);
  w.vec3_pad(10, 5, 40);
  const std::string name = std::string("Models_Synthetic_1") + '\0';
  w.u32(7);
  w.f32(-1);
  w.f32(2500);
  w.u32(uint32_t(name.size()));
  w.u32(2);   // references
  w.u32(0);   // string records
  w.u32(0);   // total size, patched below
  w.u32(0);
  w.u32(80);  // trailing block: two 40-byte records
  w.u32(2);   // model records
  w.u32(long_header ? 0x12345678 : 0);
  w.u32(0x44444444);
  while (w.bytes.size() < (long_header ? 0x98u : 0x60u)) w.u32(0xFFFFFFFF);
  w.bytes.insert(w.bytes.end(), name.begin(), name.end());
  pad_to(w, 16);
  // 28 characters and a NUL: the ID lists and strings end off a 4-byte boundary.
  const std::string text = "Tracks\\Synthetic\\Obj_001.max";
  for (int r = 0; r < 2; ++r) {
    w.vec3_pad(-1, 0, -1);
    w.vec3_pad(1, 2, 1);
    w.u32(r == 0 ? 0 : uint32_t(text.size() + 2));
    const uint32_t counts[2][3] = {{1, 2, 0}, {0, 0, 0}};
    for (int k = 0; k < 3; ++k) {
      w.u32(counts[r][k]);
      w.u32(0x11111111);  // slot
    }
    w.u32(0x33333333);
  }
  w.u32(61000);
  w.u32(61001);
  w.u32(61002);
  w.bytes.insert(w.bytes.end(), text.begin(), text.end());
  w.bytes.push_back(0);
  w.bytes.push_back(0);
  pad_to(w, 4);
  w.u32(0x0101);
  w.u32(0);
  w.u32(0x0102);
  w.u32(1);
  // The class part: a 0x14-byte header with the group count at +0xC.
  w.u32(0x22222222);
  w.f32(100);
  w.u32(0);
  w.u32(2);
  w.u32(0);
  for (uint32_t g = 0; g < 2; ++g) {
    const size_t at = w.bytes.size();
    w.bytes.resize(at + 0x5C, 0);
    put_be32_at(w.bytes, at + 0x00, g);
    put_be32_at(w.bytes, at + 0x2C, g);
    put_be32_at(w.bytes, at + 0x30, g == 0 ? uint32_t(in.size()) : 0);
    put_be32_at(w.bytes, at + 0x34, 1);
    put_be32_at(w.bytes, at + 0x38, g == 0 ? 1 : 0);
    for (int k = 0; k < 4; ++k) {
      uint32_t v;
      const float d = 100.0f * float(k + 1);
      std::memcpy(&v, &d, 4);
      put_be32_at(w.bytes, at + 0x3C + 4 * size_t(k), v);
    }
  }
  pad_to(w, 16);
  for (const auto& i : in) {
    for (const auto& row : i.rows) w.vec3_pad(row[0], row[1], row[2]);
    w.f32(i.t[0]);
    w.f32(i.t[1]);
    w.f32(i.t[2]);
    w.f32(1);
    for (int k = 0; k < 4; ++k) w.f32(0.5f);
    w.vec3_pad(0, 1, 0);
  }
  for (int k = 0; k < 32; ++k) w.bytes.push_back(uint8_t(k));
  for (int k = 0; k < 80; ++k) w.bytes.push_back(uint8_t(0xA0 + k));
  put_be32_at(w.bytes, 0x48, uint32_t(w.bytes.size()));
  return w.bytes;
}

bool near3(const std::array<double, 3>& a, double x, double y, double z) {
  return std::fabs(a[0] - x) < 1e-9 && std::fabs(a[1] - y) < 1e-9 && std::fabs(a[2] - z) < 1e-9;
}

void test_pgeo() {
  const std::vector<SyntheticInstance> in = {
      {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {1, 2, 30}},
      {{{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}, {2, 3, 31}},  // a quarter turn about y
      {{{2, 0, 0}, {0, 2, 0}, {0, 0, 2}}, {3, 4, 32}},   // scale 2
  };
  for (bool long_header : {false, true}) {
    const auto bytes = synthetic_pgeo(long_header, in);
    fh1::PgeoTile t;
    std::string error;
    CHECK(fh1::pgeo_kind(bytes, &error) == 7);
    CHECK(fh1::parse_pgeo(bytes, t, &error));
    if (!error.empty()) std::printf("  pgeo: %s\n", error.c_str());
    CHECK(t.header_size == (long_header ? 0x98u : 0x60u));
    CHECK(t.name == "Models_Synthetic_1" && t.distance == 2500.0f && t.box_max[2] == 40.0f);
    CHECK(t.references.size() == 2 && t.references[1] == std::make_pair(0x0102u, 1u));
    CHECK(t.models.size() == 2 && t.groups.size() == 2 && t.trailing_size == 80);
    if (t.models.size() == 2 && t.groups.size() == 2) {
      CHECK(t.models[0].ids[0] == std::vector<uint32_t>{61000});
      CHECK((t.models[0].ids[1] == std::vector<uint32_t>{61001, 61002}));
      CHECK(t.models[0].ids[2].empty() && t.models[1].ids[0].empty());
      CHECK(t.models[1].text == "Tracks\\Synthetic\\Obj_001.max");
      CHECK(t.groups[0].instances.size() == 3 && t.groups[1].instances.empty());
      CHECK(t.groups[0].record32_count == 1 && t.groups[1].model_record == 1);
      CHECK(t.groups[0].distances[3] == 400.0f);
      if (t.groups[0].instances.size() == 3) {
        const auto& i1 = t.groups[0].instances[1];
        CHECK(i1.rows[0][2] == -1.0f && i1.translation[2] == 31.0f && i1.w == 1.0f);
        // F R^T F v + F t. Local +x maps to the first row (0, 0, -1) in the
        // game frame, (0, 0, 1) in the file frame, added to F t = (2, 3, -31).
        const auto p = fh1::pgeo_placement(i1);
        CHECK(near3(p.apply({1, 0, 0}), 2, 3, -30));
        // File-frame local +z is game-frame -z, whose image is -(row 2) =
        // (-1, 0, 0) in both frames.
        CHECK(near3(p.apply({0, 0, 1}), 1, 3, -31));
        CHECK(near3(fh1::pgeo_placement(t.groups[0].instances[2]).apply({1, 1, 1}), 5, 6, -30));
      }
    }
    // Truncations are errors with a reason.
    size_t tried = 0, rejected = 0;
    for (size_t n = 0; n < bytes.size(); n += 7) {
      fh1::PgeoTile x;
      std::string why;
      ++tried;
      if (!fh1::parse_pgeo(std::span(bytes).first(n), x, &why) && !why.empty()) ++rejected;
    }
    CHECK(rejected == tried);
    auto bad = bytes;
    bad[0x30 + 3] = 3;
    CHECK(!fh1::parse_pgeo(bad, t, &error) && t.kind == 3);
    bad = bytes;
    bad[0x50 + 3] = 40;  // the groups no longer end where the trailing block starts
    error.clear();
    CHECK(!fh1::parse_pgeo(bad, t, &error) && !error.empty());
  }
}

void put_zone_record(BeWriter& w, const std::array<float, 3>& pos, const uint16_t rot[9],
                     bool extended, const std::string& name) {
  for (uint16_t d : {0x3C00, 0x4000, 0x4200}) w.u16(d);  // 1, 2, 3
  for (float p : pos) w.f32(p);
  for (int k = 0; k < 9; ++k) w.u16(rot[k]);
  for (int k = 0; k < 16; ++k) w.bytes.push_back(0);
  w.bytes.push_back(extended ? 1 : 0);
  if (extended) {
    w.u32(0xFFFFFFFF);
    w.bytes.push_back(3);
    w.bytes.insert(w.bytes.end(), {7, 8, 9});
    std::string n = name;
    n.resize(32, '\0');
    w.bytes.insert(w.bytes.end(), n.begin(), n.end());
  }
}

void test_pvsz() {
  const uint16_t identity[9] = {0x3C00, 0, 0, 0, 0x3C00, 0, 0, 0, 0x3C00};
  // A quarter turn: rows (0, 0, 1), (0, 1, 0), (-1, 0, 0).
  const uint16_t quarter[9] = {0, 0, 0x3C00, 0, 0x3C00, 0, 0xBC00, 0, 0};
  BeWriter w;
  w.u32(3);
  w.u32(5);
  w.u32(0x80000006);
  w.u32(7);
  // The undecoded block: 64 bytes that hold no rigid record.
  for (int k = 0; k < 64; ++k) w.bytes.push_back(uint8_t(k % 3 == 0 ? 0x11 : 0));
  const size_t start = w.bytes.size();
  // The first record's z (bytes 14-17) holds 0x01 at byte 15 and its
  // rotation's second half is 0, so a bogus extended record 37 bytes
  // earlier ends exactly where the second record starts: two starts fit.
  float z;
  const uint32_t zbits = 0x42010000;
  std::memcpy(&z, &zbits, 4);
  put_zone_record(w, {10, 20, z}, identity, false, "");
  put_zone_record(w, {-5, 1, 2}, quarter, true, "ZONE_A");
  put_zone_record(w, {7, 8, 9}, identity, false, "");
  fh1::PvszZone zone;
  std::string error;
  CHECK(fh1::parse_pvsz(w.bytes, zone, &error));
  CHECK(zone.ids.size() == 3 && fh1::pvsz_object_id(zone.ids[1]) == 6 && (zone.ids[1] >> 31) == 1);
  CHECK(zone.starts.size() >= 2 && zone.starts.front() == start - 37);
  CHECK(zone.start == start && zone.rigid_records == 3);
  CHECK(zone.records.size() == 3);
  if (zone.records.size() == 3) {
    const auto& r = zone.records[1];
    CHECK(r.extended && r.word == 0xFFFFFFFFu && r.extra == std::vector<uint8_t>({7, 8, 9}));
    CHECK(r.name == "ZONE_A" && r.position[0] == -5.0f && r.distances[1] == 2.0f);
    CHECK(r.rotation[2] == 1.0f && r.rotation[6] == -1.0f);
    // R[r][c] = h[3c + r] F[r]: local +x goes to (h0, h1, -h2) = (0, 0, -1).
    CHECK(near3(fh1::pvsz_placement(r).apply({1, 0, 0}), -5, 1, -3));
    CHECK(!zone.records[0].extended && zone.records[2].position[2] == 9.0f);
  }
  fh1::PvszZone first;
  CHECK(fh1::parse_pvsz(w.bytes, first, &error, fh1::PvszStartRule::kFirst));
  CHECK(first.start == start - 37 && first.rigid_records == 2);
  // One byte more at the end: no start fits.
  auto longer = w.bytes;
  longer.push_back(0);
  error.clear();
  CHECK(!fh1::parse_pvsz(longer, zone, &error) && !error.empty());
  CHECK(!fh1::parse_pvsz(std::span(w.bytes).first(3), zone, &error));
}

// A tilted rotation (no axis upright), where reading the halves as rows
// instead of columns gives another placement.
void test_pvsz_tilted() {
  fh1::PvszRecord r;
  // Stored columns (0, 1, 0), (0, 0, -1), (1, 0, 0); negating each third
  // component gives R = [[0, 0, 1], [1, 0, 0], [0, 1, 0]], a proper rotation.
  r.rotation = {0, 1, 0, 0, 0, -1, 1, 0, 0};
  r.position = {10, 20, 30};
  CHECK(fh1::pvsz_record_rigid(r));
  const auto p = fh1::pvsz_placement(r);
  CHECK(near3(p.apply({1, 0, 0}), 10, 21, -30));
  CHECK(near3(p.apply({0, 1, 0}), 10, 20, -29));
  CHECK(near3(p.apply({0, 0, 1}), 11, 20, -30));
  CHECK(near3(p.apply({0, 0, 0}), 10, 20, -30));
  double det = 0;
  const auto& m = p.linear;
  det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
        m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  CHECK(std::fabs(det - 1) < 1e-12);
}

void test_lod_part() {
  std::string family;
  int level = 0;
  CHECK(fh1::rmb_lod_part("Synthetic_Rail_LOD02_", family, level));
  CHECK(level == 2 && family == "synthetic_rail_lod**_");
  CHECK(!fh1::rmb_lod_part("Tree_NoLOD", family, level) && level == -1);
  CHECK(!fh1::rmb_lod_part("TERR_LOD1", family, level));
}

}  // namespace

// Raw deflate streams of two synthetic inputs (made with zlib at level 9):
// a short text (fixed Huffman) and repeated byte runs (dynamic Huffman).
void test_inflate() {
  const std::string text = "hello hello hello hello, deflate";
  const std::vector<uint8_t> a = {203,72,205,201,201,87,200,64,39,117,20,82,82,211,114,18,75,82,1};
  auto out = fh1::inflate_raw(a, text.size());
  CHECK(out && std::string(out->begin(), out->end()) == text);
  std::vector<uint8_t> expect;
  for (int r = 0; r < 20; ++r)
    for (int i = 0; i < 256; ++i) expect.push_back(uint8_t(i));
  for (int r = 0; r < 50; ++r) expect.insert(expect.end(), {'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c'});
  const std::vector<uint8_t> b = {99,96,100,98,102,97,101,99,231,224,228,226,230,225,229,227,23,16,20,18,22,17,21,19,151,144,148,146,150,145,149,147,87,80,84,82,86,81,85,83,215,208,212,210,214,209,213,211,55,48,52,50,54,49,53,51,183,176,180,178,182,177,181,179,119,112,116,114,118,113,117,115,247,240,244,242,246,241,245,243,15,8,12,10,14,9,13,11,143,136,140,138,142,137,141,139,79,72,76,74,78,73,77,75,207,200,204,202,206,201,205,203,47,40,44,42,46,41,45,43,175,168,172,170,174,169,173,171,111,104,108,106,110,105,109,107,239,232,236,234,238,233,237,235,159,48,113,210,228,41,83,167,77,159,49,115,214,236,57,115,231,205,95,176,112,209,226,37,75,151,45,95,177,114,213,234,53,107,215,173,223,176,113,211,230,45,91,183,109,223,177,115,215,238,61,123,247,237,63,112,240,208,225,35,71,143,29,63,113,242,212,233,51,103,207,157,191,112,241,210,229,43,87,175,93,191,113,243,214,237,59,119,239,221,127,240,240,209,227,39,79,159,61,127,241,242,213,235,55,111,223,189,255,240,241,211,231,47,95,191,125,255,241,243,215,239,63,127,255,253,103,24,245,255,168,255,71,253,63,234,255,81,255,143,250,127,212,255,163,254,31,245,255,168,255,71,253,63,234,255,81,255,143,250,127,212,255,163,254,31,245,255,168,255,135,177,255,129,206,27,69,67,26,1,0};
  out = fh1::inflate_raw(b, expect.size());
  CHECK(out && *out == expect);
  CHECK(!fh1::inflate_raw(b, expect.size() - 1));  // wrong size is an error
  // A stored block.
  const std::vector<uint8_t> stored = {1, 3, 0, 0xFC, 0xFF, 'x', 'y', 'z'};
  out = fh1::inflate_raw(stored, 3);
  CHECK(out && std::string(out->begin(), out->end()) == "xyz");
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}

// A kind-5 model: a header with a draw-list block of one LOD (one opaque
// sub-section of part 0, one transparent of part 1), a part with a 16-bit
// strip, a glass part with a 32-bit strip, an empty part, and the footer.
fh1::Carbin synthetic_carbin() {
  fh1::Carbin c;
  std::vector<uint8_t> head;
  put32(head, 5);
  for (int i = 0; i < 9; ++i) put32(head, 0x3F800000u + i);  // filler words
  put32(head, 4); put32(head, 4); put32(head, 1);
  put32(head, 1); put32(head, 0x00010000u);  // transparent list: part 1, sub 0
  put32(head, 1); put32(head, 0x00000000u);  // opaque list: part 0, sub 0
  put32(head, 1);
  c.head = head;
  c.head_size = head.size();
  auto make_part = [](const char* name, uint32_t index_size, std::vector<uint32_t> indices, const char* material) {
    fh1::CarbinPart p;
    p.bounds = {0, 0.5f, 0, -1, -0.5f, -2, 1, 0.5f, 2};
    p.records = {0, 0, 0, 0};
    p.shorts = {0, 0, 0, 1, 0, 7};
    p.name = name;
    p.version = 3;
    p.lod_pool.stride = 0;
    fh1::CarbinSubsection s;
    s.uv_transform = {0, 1, 0, 1, 0, 1, 0, 1};
    s.material = material;
    s.lod = 0;
    s.index_type = 6;
    s.version_words = {4, 0};
    s.index_size = index_size;
    s.indices = std::move(indices);
    p.subsections.push_back(s);
    p.lod0_pool.count = 5;
    p.lod0_pool.stride = 28;
    p.lod0_pool.buffer_flag = 1;
    p.lod0_pool.buffer_id = 9;
    for (int i = 0; i < 5; ++i) {
      fh1::CarbinVertex v{};
      v.position = {int16_t(i * 1000), 0, 32767, int16_t(10000 + i)};
      v.frame = {0, 0, 0, 32767};
      v.lighting = {100, 1, 2, 3};
      p.lod0_pool.vertices.push_back(v);
    }
    p.tail_word = 1;
    for (int b = 0; b < 2; ++b) {
      fh1::CarbinDamageBlock d;
      d.kind = 3;
      p.damage.push_back(d);
    }
    return p;
  };
  // 16-bit strip with a restart and an odd number of triangles; 32-bit strip
  // with the 0x00FFFFFF restart.
  c.parts.push_back(make_part("body", 2, {0, 1, 2, 3, 0xFFFF, 2, 3, 4}, "body"));
  c.parts.push_back(make_part("glassF", 4, {0, 1, 2, 0x00FFFFFF, 2, 3, 4}, "window"));
  fh1::CarbinPart empty;
  const float m = 3.402823466e38f;
  empty.bounds = {m, m, m, -m, -m, -m, m, m, m};
  empty.records = {0, 0, 0, 0};
  empty.shorts = {0, 0, 0, 0};
  empty.name = "empty";
  empty.version = 3;
  empty.tail_word = 1;
  for (int b = 0; b < 2; ++b) empty.damage.push_back({});
  c.parts.push_back(empty);
  // The empty part has no sub-sections, so the lists still cover every one.
  c.footer = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
  return c;
}

void test_carbin_round_trip() {
  const fh1::Carbin model = synthetic_carbin();
  const auto bytes = fh1::encode_carbin(model);
  CHECK(fh1::carbin_scan_part_count(bytes).size() == 1);
  fh1::Carbin back;
  std::string error;
  CHECK(fh1::parse_carbin(bytes, back, &error));
  CHECK(back.parts.size() == 3);
  CHECK(back.kind == 5);
  CHECK(back.draw_list_lods == 1);
  CHECK(fh1::encode_carbin(back) == bytes);
  CHECK(fh1::carbin_draw_lists_cover(back));
  const auto transparent = fh1::carbin_transparent_flags(back);
  CHECK(!transparent[0][0] && transparent[1][0]);
  CHECK(back.parts[2].empty_box());
  CHECK(fh1::carbin_bad_indices(back) == 0);
  // 16-bit strip: 0,1,2 / 2,1,3 / restart / 2,3,4 (an odd count of three).
  CHECK((fh1::carbin_triangle_list(back.parts[0].subsections[0]) ==
         std::vector<uint32_t>{0, 1, 2, 2, 1, 3, 2, 3, 4}));
  CHECK(fh1::carbin_triangle_list(back.parts[1].subsections[0]).size() == 6);

  // A model with no parts: the n = 0 form (4, 4, 0, 1, 0 and the footer).
  std::vector<uint8_t> empty;
  put32(empty, 1);
  put32(empty, 4); put32(empty, 4); put32(empty, 0); put32(empty, 1); put32(empty, 0);
  empty.insert(empty.end(), model.footer.begin(), model.footer.end());
  fh1::Carbin none;
  CHECK(fh1::parse_carbin(empty, none, &error));
  CHECK(none.parts.empty() && none.kind == 1);
  CHECK(fh1::encode_carbin(none) == empty);
}

// A synthetic .xds header: format, size, header pitch (texels), levels,
// tiled and packed-tail flags, identity swizzle, endian mode 1.
std::vector<uint8_t> xds_header(uint32_t format, uint32_t w, uint32_t h, uint32_t pitch, uint32_t levels) {
  std::vector<uint8_t> v;
  put32(v, 3);
  for (uint32_t x : {1u, 0u, 0u, 0u, 0xFFFF0000u, 0xFFFF0000u}) put32(v, x);
  put32(v, 2u | ((pitch / 32) << 22) | (1u << 31));
  put32(v, format | (1u << 6));
  put32(v, (w - 1) | ((h - 1) << 13));
  put32(v, (0u | 1u << 3 | 2u << 6 | 3u << 9) << 1);
  put32(v, (levels - 1) << 6);
  put32(v, (1u << 9) | (1u << 11));
  return v;
}

void test_xds_layout() {
  fh1::XdsHeader h;
  std::string error;
  // All tail (16 x 8, shorter side 8): one region whose pitch and rows come
  // from the size rounded up to powers of two (4 x 2 blocks -> 32 x 32), not
  // from the header pitch of 256 texels: 32 x 32 x 16 bytes.
  auto small = xds_header(53, 16, 8, 256, 1);
  CHECK(fh1::parse_xds_header(small, h, &error));
  CHECK(fh1::xds_layout_bytes(h) == 16384);
  // Not a power of two: 100 x 60 with the full chain. The base level uses
  // the header pitch (128 texels = 32 blocks), 15 rows rounded to 32; level
  // 1 is 64 x 32 rounded down from 128 x 64 -> 16 x 8 blocks -> 32 x 32;
  // levels from max(S - 4, 0) = 2 on share the tail tile.
  auto npot = xds_header(53, 100, 60, 128, 7);
  CHECK(fh1::parse_xds_header(npot, h, &error));
  CHECK(fh1::xds_layout_bytes(h) == 3 * 16384);

  // Untiling: fill each block of a 32 x 32-block DXT1 level with its own
  // index at its tiled offset (bytes swapped in pairs, endian mode 1) and
  // read it back in row order.
  auto tex = xds_header(18, 128, 128, 128, 1);
  const size_t payload = 32 * 32 * 8;
  std::vector<uint8_t> data(tex.size() + payload, 0);
  std::copy(tex.begin(), tex.end(), data.begin());
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 32; ++x) {
      const size_t at = tex.size() + size_t(fh1::xenos_tiled_offset_2d(x, y, 32, 3));
      const uint16_t id = uint16_t(y * 32 + x);
      data[at] = uint8_t(id >> 8);  // stored swapped: host bytes are (lo, hi)
      data[at + 1] = uint8_t(id);
    }
  fh1::XdsBlocks blocks;
  CHECK(fh1::extract_xds_blocks(data, blocks, &error));
  CHECK(blocks.format == fh1::XdsBlockFormat::kBc1);
  CHECK(blocks.levels.size() == 1 && blocks.levels[0].blocks.size() == payload);
  bool ordered = true;
  for (int i = 0; i < 32 * 32; ++i) {
    const uint8_t* b = &blocks.levels[0].blocks[size_t(i) * 8];
    if (uint16_t(b[0] | b[1] << 8) != i) ordered = false;
  }
  CHECK(ordered);
  CHECK((blocks.swizzle == std::array<uint8_t, 4>{0, 1, 2, 3}));
}

// A package's offer ZIP with one car folder whose members are stored,
// deflated and LZX (XMem), found through the install layout and read back.
void test_dlc_folder() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "shift-engine-dlc-test";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "game" / "media" / "cars");
  const fs::path zips = root / "state" / "user" / "0000000000000000" / "4D5309C9" / "00000002" /
                        "ABCDEF" / "Media" / "DLCZips";
  fs::create_directories(zips);
  const std::string text = "hello hello hello hello, deflate";
  const std::vector<uint8_t> plain(text.begin(), text.end());
  const std::vector<uint8_t> deflated = {203,72,205,201,201,87,200,64,39,117,20,82,82,211,114,18,75,82,1};
  std::vector<uint8_t> big(40000);
  for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 31 >> 2);
  struct Member {
    std::string name;
    uint16_t method;
    std::vector<uint8_t> data, packed;
  };
  const std::vector<Member> members = {
      {"Media/cars/Test_Car/a.carbin", 0, plain, plain},
      {"Media/cars/Test_Car/b.xds", 8, plain, deflated},
      {"Media/cars/Test_Car/LiveryMasks/c.tga", 21, big, xmem_stored(big)},
      {"Media/wheels/Test_Wheel/w.carbin", 0, plain, plain},
  };
  std::vector<uint8_t> zip, directory;
  for (const Member& m : members) {
    const uint32_t crc = fh1::crc32(m.data), offset = uint32_t(zip.size());
    put_le32(zip, 0x04034B50);
    for (uint32_t v : {20u, 0u, uint32_t(m.method), 0u, 0u}) put_le16(zip, v);
    put_le32(zip, crc);
    put_le32(zip, uint32_t(m.packed.size()));
    put_le32(zip, uint32_t(m.data.size()));
    put_le16(zip, uint32_t(m.name.size()));
    put_le16(zip, 0);
    zip.insert(zip.end(), m.name.begin(), m.name.end());
    zip.insert(zip.end(), m.packed.begin(), m.packed.end());
    put_le32(directory, 0x02014B50);
    for (uint32_t v : {20u, 20u, 0u, uint32_t(m.method), 0u, 0u}) put_le16(directory, v);
    put_le32(directory, crc);
    put_le32(directory, uint32_t(m.packed.size()));
    put_le32(directory, uint32_t(m.data.size()));
    for (uint32_t v : {uint32_t(m.name.size()), 0u, 0u, 0u, 0u}) put_le16(directory, v);
    put_le32(directory, 0);
    put_le32(directory, offset);
    directory.insert(directory.end(), m.name.begin(), m.name.end());
  }
  const uint32_t directory_offset = uint32_t(zip.size());
  zip.insert(zip.end(), directory.begin(), directory.end());
  put_le32(zip, 0x06054B50);
  for (uint32_t v : {0u, 0u, uint32_t(members.size()), uint32_t(members.size())}) put_le16(zip, v);
  put_le32(zip, uint32_t(directory.size()));
  put_le32(zip, directory_offset);
  put_le16(zip, 0);
  {
    std::ofstream f(zips / "1000_pri_65.zip", std::ios::binary);
    f.write(reinterpret_cast<const char*>(zip.data()), std::streamsize(zip.size()));
    std::ofstream g(zips / "StringTables_pri_70.zip", std::ios::binary);  // not content
  }
  cars::Install install;
  std::string error;
  CHECK(cars::discover_install(root / "game", root / "state", {}, install, &error));
  CHECK(install.packages.size() == 1);
  if (install.packages.size() == 1) {
    CHECK(install.packages[0].priority == 65);
    CHECK(install.packages[0].archives.size() == 1);
  }
  const auto car = install.find_car("TEST_CAR");
  CHECK(car.has_value());
  CHECK(install.find_wheel("test_wheel").has_value());
  if (car) {
    cars::Folder folder;
    CHECK(folder.open(*car, &error));
    CHECK((folder.names() == std::vector<std::string>{"a.carbin", "b.xds", "liverymasks/c.tga"}));
    std::vector<uint8_t> out;
    CHECK(folder.read("a.carbin", out, &error) && out == plain);
    CHECK(folder.read("B.XDS", out, &error) && out == plain);
    CHECK(folder.read("LiveryMasks/c.tga", out, &error) && out == big);
  }
  fs::remove_all(root, ec);
}

// The baked format: a car bake encodes, decodes and encodes to the same
// bytes, and materials are classed from technique names and draw lists.
void test_bake_format() {
  using namespace cars::bake;
  Bake b;
  b.kind = Kind::kCar;
  b.car.media_name = "Test_Car";
  b.car.car_id = 7;
  b.car.colours.push_back({7001, 1, true, 0x123456, "nodamage_lod0.xds", "nodamage.xds"});
  b.car.wheel_centres[0] = {-0.8f, 0.3f, -1.2f};
  b.car.notes = {"a note"};
  Model m;
  m.name = "test_car_lod0.carbin";
  m.role = Role::kLod0;
  Part p;
  p.name = "body";
  p.lod0_vertices.push_back({{1, 2, 3}, {0, 0, 0, 32767}, {1, 2}, {3, 4}, {100, 1, 2, 3}});
  Subsection s;
  s.technique = "body";
  s.material = MaterialClass::kPaint;
  s.slots = {"@atlas", ""};
  s.indices = {0, 0, 0};
  p.subsections.push_back(s);
  m.parts.push_back(p);
  b.models.push_back(m);
  b.textures.push_back({"t.xds", TextureFormat::kBc3, {0, 1, 2, 3}, {{4, 4, std::vector<uint8_t>(16, 9)}}});
  b.livery.push_back({"Front", true, -1, 1, -2, 2, 0, 0, 1, 1, 0, "-x", "+y"});
  const auto bytes = encode(b);
  Bake back;
  std::string error;
  CHECK(decode(bytes, back, &error));
  CHECK(encode(back) == bytes);
  CHECK(back.car.colours.size() == 1 && back.car.colours[0].rgb == 0x123456);
  auto truncated = bytes;
  truncated.pop_back();
  CHECK(!decode(truncated, back, &error));

  CHECK(cars::classify("body", false, Role::kLod0) == MaterialClass::kPaint);
  CHECK(cars::classify("window", true, Role::kLod0) == MaterialClass::kGlass);
  CHECK(cars::classify("blur_rim", true, Role::kWheel) == MaterialClass::kHidden);
  CHECK(cars::classify("hub", false, Role::kWheel) == MaterialClass::kRim);
  CHECK(cars::classify("hub", false, Role::kRotor) == MaterialClass::kOpaque);
  CHECK(cars::classify("tail_light_lod0", false, Role::kLod0) == MaterialClass::kLamp);
  CHECK(cars::classify("leather", false, Role::kCockpit) == MaterialClass::kInterior);
  CHECK(cars::classify("plastic2", false, Role::kLod0) == MaterialClass::kOpaque);

  // TGA: a 2 x 2 RLE image stored bottom row first.
  const std::vector<uint8_t> tga = {0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 8,
                                    0x81, 1, 2, 3, 4,         // bottom row: two of (b1 g2 r3 a4)
                                    0x01, 5, 6, 7, 8, 9, 10, 11, 12};  // top row: raw pair
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;
  CHECK(cars::decode_tga(tga, w, h, rgba, &error));
  CHECK(w == 2 && h == 2 && rgba.size() == 16);
  CHECK(rgba[0] == 7 && rgba[3] == 8 && rgba[8] == 3 && rgba[11] == 4);
}

// carview's reference JSON (provisional schema), the eye of a clip matrix,
// and the M1 metric on small masks.
void test_reference() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "shift_reference_test";
  std::filesystem::create_directories(dir);
  // A look-at from (0, 0, 5) towards the origin times a perspective: the
  // eye must come back as (0, 0, 5).
  const float f = 2.0f, n = 0.5f, fa = 50.0f;
  // view: translate by -5 in z; projection (D3D, z 0..1, column vectors).
  const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -5, 1};
  const float proj[16] = {f, 0, 0, 0, 0, f, 0, 0, 0, 0, fa / (n - fa), -1, 0, 0, n * fa / (n - fa), 0};
  float m[16];
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += proj[k * 4 + r] * view[c * 4 + k];
      m[c * 4 + r] = sum;
    }
  {
    std::FILE* out = std::fopen((dir / "car.json").string().c_str(), "wb");
    std::fprintf(out, "{\"schema\": \"shift-engine.m1-reference.v1\", \"car_id\": 1001,\n"
                      " \"media_name\": \"X_Car_10\", \"image\": \"1001.png\", \"mask\": \"1001.mask.png\",\n"
                      " \"frame\": {\"after_a\": 390}, \"paint_rgb\": [141, 0, 7], \"half_pixel_removed\": true,\n"
                      " \"width\": 64, \"height\": 32, \"clip_from_car\": [");
    for (int i = 0; i < 16; ++i) std::fprintf(out, "%s%.9g", i ? ", " : "", double(m[i]));
    std::fprintf(out, "]}\n");
    std::fclose(out);
  }
  carview::Reference ref;
  std::string error;
  CHECK(carview::read_reference(dir / "car.json", ref, error));
  CHECK(ref.media_name == "X_Car_10" && ref.car_id == 1001 && ref.width == 64 && ref.height == 32);
  CHECK(ref.has_paint && ref.paint_rgb[0] == 141 && ref.paint_rgb[2] == 7);
  CHECK(ref.image == dir / "1001.png" && ref.mask == dir / "1001.mask.png");
  CHECK(std::fabs(ref.clip_from_car[14] - m[14]) < 1e-5f);
  float eye[3];
  CHECK(carview::clip_eye(ref.clip_from_car, eye));
  CHECK(std::fabs(eye[0]) < 1e-4f && std::fabs(eye[1]) < 1e-4f && std::fabs(eye[2] - 5) < 1e-4f);
  // Without checks.transparent_clip_from_car the glass uses the main matrix.
  CHECK(!ref.has_transparent && ref.transparent_clip_from_car[14] == ref.clip_from_car[14]);
  {
    // With it, the glass matrix is read from checks (here: m with x moved).
    std::FILE* out = std::fopen((dir / "glass.json").string().c_str(), "wb");
    std::fprintf(out, "{\"schema\": \"shift-engine.m1-reference.v1\", \"width\": 64, \"height\": 32,\n"
                      " \"checks\": {\"mask_threshold\": 6, \"transparent_clip_from_car\": [");
    for (int i = 0; i < 16; ++i) std::fprintf(out, "%s%.9g", i ? ", " : "", double(i == 12 ? m[i] + 0.25f : m[i]));
    std::fprintf(out, "]}, \"clip_from_car\": [");
    for (int i = 0; i < 16; ++i) std::fprintf(out, "%s%.9g", i ? ", " : "", double(m[i]));
    std::fprintf(out, "]}\n");
    std::fclose(out);
  }
  carview::Reference glass;
  CHECK(carview::read_reference(dir / "glass.json", glass, error));
  CHECK(glass.has_transparent && std::fabs(glass.transparent_clip_from_car[12] - (m[12] + 0.25f)) < 1e-5f &&
        std::fabs(glass.clip_from_car[12] - m[12]) < 1e-5f);
  {
    std::FILE* out = std::fopen((dir / "bad.json").string().c_str(), "wb");
    std::fprintf(out, "{\"schema\": \"other\", \"width\": 1, \"height\": 1, \"clip_from_car\": []}");
    std::fclose(out);
  }
  CHECK(!carview::read_reference(dir / "bad.json", ref, error));
  std::filesystem::remove_all(dir);

  // Metric: a 4x4 square against the same square shifted by one column.
  const uint32_t w = 8, h = 8;
  std::vector<uint8_t> a_mask(w * h, 0), b_mask(w * h, 0), a_rgba(w * h * 4, 100), b_rgba(w * h * 4, 100);
  for (uint32_t y = 2; y < 6; ++y)
    for (uint32_t x = 2; x < 6; ++x) {
      a_mask[y * w + x] = 255;
      b_mask[y * w + x + 1] = 255;
    }
  carview::Metric same = carview::compare(a_rgba, a_mask, a_rgba, a_mask, w, h);
  CHECK(same.iou == 1.0 && std::isinf(same.masked_psnr) && same.edge_overlap == 1.0);
  carview::Metric shifted = carview::compare(a_rgba, a_mask, b_rgba, b_mask, w, h);
  CHECK(std::fabs(shifted.iou - 12.0 / 20.0) < 1e-9);
  b_rgba[(3 * w + 3) * 4] = 110;  // one channel of one pixel off by 10
  carview::Metric noisy = carview::compare(a_rgba, a_mask, b_rgba, a_mask, w, h);
  CHECK(std::fabs(noisy.masked_psnr - 10 * std::log10(255.0 * 255.0 / (100.0 / 48.0))) < 1e-6);
}

int main() {
  test_reference();
  test_bake_format();
  test_dlc_folder();
  test_xds_layout();
  test_inflate();
  test_carbin_round_trip();
  test_crc();
  test_xmem();
  test_strip();
  test_position();
  test_tiled_offset();
  test_archive_wrapped_count();
  test_render_model();
  test_half();
  test_object_table();
  test_pgeo();
  test_pvsz();
  test_pvsz_tilted();
  test_lod_part();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all fh1data tests passed\n");
  return 0;
}
