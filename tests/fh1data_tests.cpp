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

#include "fh1data/archive.h"
#include "fh1data/carbin.h"
#include "fh1data/placements.h"
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
  CHECK(fh1::half_to_float(0x5640) == 100.0f);
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
    w.u16(0x20);
    w.u16(0x3F02);
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
  w.u32(0x2A);
  w.f32(150);
  w.f32(150);
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
  w.u32(0x43280080);
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
      w.u32(0xB087A232);
    }
    w.u32(0x78000040);
  }
  w.u32(61000);
  w.u32(61001);
  w.u32(61002);
  w.bytes.insert(w.bytes.end(), text.begin(), text.end());
  w.bytes.push_back(0);
  w.bytes.push_back(0);
  pad_to(w, 4);
  w.u32(0x4C8C);
  w.u32(0);
  w.u32(0x4C8D);
  w.u32(1);
  // The class part: a 0x14-byte header with the group count at +0xC.
  w.u32(0xD4516D37);
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
    CHECK(t.references.size() == 2 && t.references[1] == std::make_pair(0x4C8Du, 1u));
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
  for (uint16_t d : {0x5640, 0x5BD0, 0x5FD0}) w.u16(d);  // 100, 250, 500
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
  put_zone_record(w, {-5, 1, 2}, quarter, true, "FR06_13");
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
    CHECK(r.name == "FR06_13" && r.position[0] == -5.0f && r.distances[1] == 250.0f);
    CHECK(r.rotation[2] == 1.0f && r.rotation[6] == -1.0f);
    // H F v + F p with H row-major: local +x goes to H's first column.
    CHECK(near3(fh1::pvsz_placement(r).apply({1, 0, 0}), -5, 1, -3));
    CHECK(near3(fh1::pvsz_placement(r, true).apply({1, 0, 0}), -5, 1, -1));
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

void test_lod_part() {
  std::string family;
  int level = 0;
  CHECK(fh1::rmb_lod_part("OBJ_FEST_BarrierMetal_LOD02_", family, level));
  CHECK(level == 2 && family == "obj_fest_barriermetal_lod**_");
  CHECK(!fh1::rmb_lod_part("Tree_NoLOD", family, level) && level == -1);
  CHECK(!fh1::rmb_lod_part("TERR_LOD1", family, level));
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
  test_half();
  test_object_table();
  test_pgeo();
  test_pvsz();
  test_lod_part();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all fh1data tests passed\n");
  return 0;
}
