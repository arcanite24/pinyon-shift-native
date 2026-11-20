#include "fh1data/carbin.h"

#include <cfloat>
#include <cmath>
#include <cstring>

#include "fh1data/bytes.h"

namespace fh1 {
namespace {

constexpr uint32_t kPartMarker = 5;
constexpr uint32_t kVertexStride = 28;
constexpr size_t kFooterSize = 24;
constexpr uint32_t kMaxListEntries = 4096;

template <size_t N>
void take(BeReader& r, std::array<uint8_t, N>& out) {
  auto bytes = r.raw(N);
  std::memcpy(out.data(), bytes.data(), N);
}

std::vector<uint8_t> take_vector(BeReader& r, size_t n) {
  auto bytes = r.raw(n);
  return {bytes.begin(), bytes.end()};
}

bool floats_sane(std::span<const uint8_t> data, size_t at, int count) {
  for (int i = 0; i < count; ++i) {
    const float v = load_bef32(&data[at + 4 * i]);
    // An empty part's bounds are +/-FLT_MAX (an empty box).
    if (std::isnan(v) || (std::fabs(v) >= 1e5f && std::fabs(v) != FLT_MAX)) return false;
  }
  return true;
}

CarbinPool read_pool(BeReader& r) {
  CarbinPool pool;
  pool.count = r.i32();
  pool.stride = r.u32();
  pool.buffer_flag = r.u32();
  if (pool.buffer_flag) pool.buffer_id = r.u32();
  if (pool.stride > 256) throw FormatError("pool stride " + std::to_string(pool.stride));
  const size_t count = pool.vertex_count();
  auto body = r.raw(count * pool.stride);
  pool.raw_bytes = body.size();
  if (pool.stride == kVertexStride) {
    pool.vertices.resize(count);
    for (size_t i = 0; i < count; ++i) {
      const uint8_t* p = body.data() + i * kVertexStride;
      CarbinVertex& v = pool.vertices[i];
      for (int k = 0; k < 4; ++k) v.position[k] = int16_t(load_be16(p + 2 * k));
      v.uv0 = {load_be16(p + 8), load_be16(p + 10)};
      v.uv1 = {load_be16(p + 12), load_be16(p + 14)};
      for (int k = 0; k < 4; ++k) v.frame[k] = int16_t(load_be16(p + 16 + 2 * k));
      for (int k = 0; k < 4; ++k) v.lighting[k] = p[24 + k];
    }
  } else {
    pool.raw.assign(body.begin(), body.end());
  }
  return pool;
}

CarbinSubsection read_subsection(BeReader& r) {
  CarbinSubsection s;
  take(r, s.head);
  for (float& f : s.uv_transform) f = r.f32();
  take(r, s.unknown1);
  const uint32_t name_size = r.u32();
  if (name_size > 256) throw FormatError("sub-section name of " + std::to_string(name_size) + " bytes");
  auto name = r.raw(name_size);
  s.material.assign(name.begin(), name.end());
  s.lod = r.i32();
  s.index_type = r.u32();
  take(r, s.unknown2);
  s.version_words = {r.u32(), r.u32()};
  const uint32_t count = r.u32();
  s.index_size = r.u32();
  if (s.index_size != 2 && s.index_size != 4)
    throw FormatError("index size " + std::to_string(s.index_size) + " in " + s.material);
  auto bytes = r.raw(size_t(count) * s.index_size);
  s.indices.resize(count);
  for (uint32_t i = 0; i < count; ++i)
    s.indices[i] = s.index_size == 2 ? load_be16(&bytes[2 * i]) : load_be32(&bytes[4 * i]);
  take(r, s.end);
  return s;
}

CarbinPart read_part(BeReader& r) {
  if (r.u32() != kPartMarker) throw FormatError("no part marker at " + std::to_string(r.at() - 4));
  CarbinPart part;
  for (float& f : part.bounds) f = r.f32();
  take(r, part.unknown1);
  {
    const size_t start = r.at();
    const uint32_t n = r.u32();
    if (n > (1u << 20)) throw FormatError("record count " + std::to_string(n));
    r.skip(size_t(n) * 16);
    r.seek(start);
    part.records = take_vector(r, 4 + size_t(n) * 16);
  }
  take(r, part.unknown2);
  {
    const size_t start = r.at();
    const uint32_t n = r.u32();
    if (n > (1u << 20)) throw FormatError("short count " + std::to_string(n));
    r.skip(size_t(n) * 2);
    r.seek(start);
    part.shorts = take_vector(r, 4 + size_t(n) * 2);
  }
  take(r, part.unknown3);
  auto name = r.raw(r.u8());
  part.name.assign(name.begin(), name.end());
  part.version = r.u32();
  part.lod_pool = read_pool(r);
  take(r, part.unknown4);
  const uint32_t count = r.u32();
  if (count > 4096) throw FormatError(std::to_string(count) + " sub-sections in " + part.name);
  part.subsections.reserve(count);
  for (uint32_t i = 0; i < count; ++i) part.subsections.push_back(read_subsection(r));
  take(r, part.unknown5);
  part.lod0_pool = read_pool(r);
  part.tail_word = r.u32();
  part.damage_flag = r.u8();
  for (int block = 0; block < 2; ++block) {  // LOD, then LOD0
    CarbinDamageBlock d;
    d.kind = r.u32();
    d.count = r.u32();
    d.stride = r.u32();
    d.extra = r.u32();
    if (d.count > (1u << 22) || d.stride > 64 || d.extra > (1u << 16))
      throw FormatError("damage block " + std::to_string(d.count) + "x" + std::to_string(d.stride) +
                        " in " + part.name);
    d.table = take_vector(r, size_t(d.count) * d.stride);
    d.words = take_vector(r, size_t(d.extra) * 4);
    part.damage.push_back(std::move(d));
  }
  return part;
}

bool plausible_part(std::span<const uint8_t> data, size_t at) {
  if (at + 4 + 36 + 28 > data.size() || load_be32(&data[at]) != kPartMarker) return false;
  if (!floats_sane(data, at + 4, 9)) return false;
  try {
    BeReader r(data, at);
    CarbinPart part = read_part(r);
    if (part.name.empty() || part.name.size() > 64) return false;
    for (char c : part.name)
      if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return false;
    return part.version == 2 || part.version == 3;
  } catch (const FormatError&) {
    return false;
  }
}

// After the lists of a draw-list block starting at `at`: the offset of the
// word 1, or SIZE_MAX when the lists do not parse.
size_t lists_end(std::span<const uint8_t> data, size_t at, uint32_t lods) {
  size_t p = at + 12;
  for (uint32_t i = 0; i < 2 * lods; ++i) {
    if (p + 4 > data.size()) return SIZE_MAX;
    const uint32_t count = load_be32(&data[p]);
    if (count > kMaxListEntries || p + 4 + size_t(count) * 4 > data.size()) return SIZE_MAX;
    p += 4 + size_t(count) * 4;
  }
  return p;
}

class BeWriter {
 public:
  std::vector<uint8_t> out;
  void u8(uint8_t v) { out.push_back(v); }
  void u16(uint16_t v) {
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
  }
  void u32(uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
  }
  void f32(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    u32(bits);
  }
  void bytes(std::span<const uint8_t> b) { out.insert(out.end(), b.begin(), b.end()); }
};

void write_pool(BeWriter& w, const CarbinPool& pool) {
  w.u32(uint32_t(pool.count));
  w.u32(pool.stride);
  w.u32(pool.buffer_flag);
  if (pool.buffer_flag) w.u32(pool.buffer_id.value_or(0));
  if (pool.stride == kVertexStride) {
    for (const CarbinVertex& v : pool.vertices) {
      for (int16_t c : v.position) w.u16(uint16_t(c));
      w.u16(v.uv0[0]);
      w.u16(v.uv0[1]);
      w.u16(v.uv1[0]);
      w.u16(v.uv1[1]);
      for (int16_t c : v.frame) w.u16(uint16_t(c));
      w.bytes(v.lighting);
    }
  } else {
    w.bytes(pool.raw);
  }
}

void write_part(BeWriter& w, const CarbinPart& part) {
  w.u32(kPartMarker);
  for (float f : part.bounds) w.f32(f);
  w.bytes(part.unknown1);
  w.bytes(part.records);
  w.bytes(part.unknown2);
  w.bytes(part.shorts);
  w.bytes(part.unknown3);
  w.u8(uint8_t(part.name.size()));
  w.bytes({reinterpret_cast<const uint8_t*>(part.name.data()), part.name.size()});
  w.u32(part.version);
  write_pool(w, part.lod_pool);
  w.bytes(part.unknown4);
  w.u32(uint32_t(part.subsections.size()));
  for (const CarbinSubsection& s : part.subsections) {
    w.bytes(s.head);
    for (float f : s.uv_transform) w.f32(f);
    w.bytes(s.unknown1);
    w.u32(uint32_t(s.material.size()));
    w.bytes({reinterpret_cast<const uint8_t*>(s.material.data()), s.material.size()});
    w.u32(uint32_t(s.lod));
    w.u32(s.index_type);
    w.bytes(s.unknown2);
    w.u32(s.version_words[0]);
    w.u32(s.version_words[1]);
    w.u32(uint32_t(s.indices.size()));
    w.u32(s.index_size);
    for (uint32_t i : s.indices) s.index_size == 2 ? w.u16(uint16_t(i)) : w.u32(i);
    w.bytes(s.end);
  }
  w.bytes(part.unknown5);
  write_pool(w, part.lod0_pool);
  w.u32(part.tail_word);
  w.u8(part.damage_flag);
  for (const CarbinDamageBlock& d : part.damage) {
    w.u32(d.kind);
    w.u32(d.count);
    w.u32(d.stride);
    w.u32(d.extra);
    w.bytes(d.table);
    w.bytes(d.words);
  }
}

}  // namespace

bool CarbinPart::empty_box() const {
  for (float f : bounds)
    if (std::fabs(f) == FLT_MAX) return true;
  return false;
}

std::vector<size_t> carbin_scan_part_count(std::span<const uint8_t> data) {
  std::vector<size_t> found;
  for (size_t at = 0; at + 12 <= data.size(); at += 4) {
    if (load_be32(&data[at]) != 4 || load_be32(&data[at + 4]) != 4) continue;
    const uint32_t lods = load_be32(&data[at + 8]);
    if (lods > 8) continue;
    const size_t p = lists_end(data, at, lods);
    if (p == SIZE_MAX || p + 8 > data.size() || load_be32(&data[p]) != 1) continue;
    const uint32_t count = load_be32(&data[p + 4]);
    if ((count == 0 && data.size() - (p + 8) == kFooterSize) ||
        (count > 0 && count <= 4096 && plausible_part(data, p + 8)))
      found.push_back(p + 4);
  }
  return found;
}

bool parse_carbin(std::span<const uint8_t> data, Carbin& out, std::string* error) {
  out = Carbin{};
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (data.size() < 4) return fail("shorter than a header");
  const auto found = carbin_scan_part_count(data);
  if (found.size() != 1)
    return fail("the draw-list scan finds " + std::to_string(found.size()) + " positions");
  const size_t count_at = found[0];
  out.kind = load_be32(data.data());
  out.head.assign(data.begin(), data.begin() + std::ptrdiff_t(count_at));
  out.head_size = count_at;
  // Walk the draw-list block again from its start, which ends at count_at.
  for (size_t at = 0; at + 12 <= count_at; at += 4) {
    if (load_be32(&data[at]) != 4 || load_be32(&data[at + 4]) != 4) continue;
    const uint32_t lods = load_be32(&data[at + 8]);
    if (lods > 8 || lists_end(data, at, lods) + 4 != count_at) continue;
    out.draw_list_lods = lods;
    BeReader r(data, at + 12);
    for (uint32_t i = 0; i < 2 * lods; ++i) {
      std::vector<uint32_t> list(r.u32());
      for (uint32_t& e : list) e = r.u32();
      (i < lods ? out.transparent_lists : out.opaque_lists).push_back(std::move(list));
    }
    break;
  }
  const uint32_t count = load_be32(&data[count_at]);
  try {
    BeReader r(data, count_at + 4);
    out.parts.reserve(count);
    for (uint32_t i = 0; i < count; ++i) out.parts.push_back(read_part(r));
    out.footer = take_vector(r, r.remaining());
  } catch (const FormatError& e) {
    return fail("part " + std::to_string(out.parts.size()) + ": " + e.what());
  }
  out.footer_size = out.footer.size();
  return true;
}

std::vector<uint8_t> encode_carbin(const Carbin& carbin) {
  BeWriter w;
  w.bytes(carbin.head);
  w.u32(uint32_t(carbin.parts.size()));
  for (const CarbinPart& part : carbin.parts) write_part(w, part);
  w.bytes(carbin.footer);
  return std::move(w.out);
}

size_t carbin_bad_indices(const Carbin& carbin) {
  size_t bad = 0;
  for (const CarbinPart& part : carbin.parts) {
    for (const CarbinSubsection& s : part.subsections) {
      const CarbinPool& pool = s.lod == 0 ? part.lod0_pool : part.lod_pool;
      const uint32_t restart = s.index_size == 2 ? 0xFFFFu : 0x00FFFFFFu;
      const uint32_t limit = uint32_t(pool.vertex_count());
      for (uint32_t i : s.indices) {
        if (s.index_type == uint32_t(CarbinIndexType::kTriangleStrip) && i == restart) continue;
        if (i >= limit) ++bad;
      }
    }
  }
  return bad;
}

bool carbin_draw_lists_cover(const Carbin& carbin) {
  std::vector<std::vector<int>> seen(carbin.parts.size());
  for (size_t p = 0; p < carbin.parts.size(); ++p) seen[p].assign(carbin.parts[p].subsections.size(), 0);
  const uint32_t n = carbin.draw_list_lods;
  const int lod_base = n == 5 ? 1 : 0;
  for (int kind = 0; kind < 2; ++kind) {
    const auto& lists = kind == 0 ? carbin.transparent_lists : carbin.opaque_lists;
    for (size_t k = 0; k < lists.size(); ++k) {
      for (uint32_t e : lists[k]) {
        const uint32_t part = e >> 16, sub = e & 0xFFFF;
        if (part >= carbin.parts.size() || sub >= carbin.parts[part].subsections.size()) return false;
        if (carbin.parts[part].subsections[sub].lod != int32_t(k) + lod_base) return false;
        ++seen[part][sub];
      }
    }
  }
  for (const auto& part : seen)
    for (int s : part)
      if (s != 1) return false;
  return true;
}

std::vector<std::vector<bool>> carbin_transparent_flags(const Carbin& carbin) {
  std::vector<std::vector<bool>> flags(carbin.parts.size());
  for (size_t p = 0; p < carbin.parts.size(); ++p) flags[p].assign(carbin.parts[p].subsections.size(), false);
  for (const auto& list : carbin.transparent_lists)
    for (uint32_t e : list) {
      const uint32_t part = e >> 16, sub = e & 0xFFFF;
      if (part < flags.size() && sub < flags[part].size()) flags[part][sub] = true;
    }
  return flags;
}

std::array<float, 3> carbin_position(const CarbinPart& part, const CarbinVertex& v) {
  const float ex = part.bounds[6], ey = part.bounds[7], ez = part.bounds[8];
  const float scale = std::sqrt(ex * ex + ey * ey + ez * ez);
  const float length = float(v.position[3]) / 32767.0f * scale;
  std::array<float, 3> p;
  for (int a = 0; a < 3; ++a) p[a] = float(v.position[a]) / 32767.0f * length + part.bounds[a];
  return p;
}

std::array<float, 3> carbin_normal(const CarbinVertex& v) {
  const float x = v.frame[0] / 32767.0f, y = v.frame[1] / 32767.0f, z = v.frame[2] / 32767.0f,
              w = v.frame[3] / 32767.0f;
  // The rotated +X axis of the quaternion (x, y, z, w).
  std::array<float, 3> n = {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)};
  const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  if (len > 0)
    for (float& c : n) c /= len;
  return n;
}

std::array<float, 2> carbin_uv(const CarbinSubsection& s, const CarbinVertex& v, int set) {
  const auto& uv = set == 0 ? v.uv0 : v.uv1;
  const float* t = &s.uv_transform[set == 0 ? 0 : 4];
  return {uv[0] / 65535.0f * t[1] + t[0], uv[1] / 65535.0f * t[3] + t[2]};
}

std::vector<uint32_t> carbin_triangle_list(const CarbinSubsection& s) {
  std::vector<uint32_t> out;
  const auto& idx = s.indices;
  if (s.index_type != uint32_t(CarbinIndexType::kTriangleStrip)) {
    out.assign(idx.begin(), idx.end() - std::ptrdiff_t(idx.size() % 3));
    return out;
  }
  const uint32_t restart = s.index_size == 2 ? 0xFFFFu : 0x00FFFFFFu;
  out.reserve(idx.size() * 3);
  size_t run = 0;  // index of the first vertex of the current strip
  for (size_t i = 0; i < idx.size(); ++i) {
    if (idx[i] == restart) {
      run = i + 1;
      continue;
    }
    if (i - run < 2) continue;
    const uint32_t a = idx[i - 2], b = idx[i - 1], c = idx[i];
    if (a == b || b == c || a == c) continue;
    if ((i - run) % 2 == 0) {
      out.insert(out.end(), {a, b, c});
    } else {
      out.insert(out.end(), {b, a, c});
    }
  }
  return out;
}

}  // namespace fh1
