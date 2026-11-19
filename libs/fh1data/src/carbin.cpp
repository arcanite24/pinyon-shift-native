#include "fh1data/carbin.h"

#include <cfloat>
#include <cmath>
#include <cstring>

#include "fh1data/bytes.h"

namespace fh1 {
namespace {

constexpr uint32_t kPartMarker = 5;
constexpr uint32_t kVertexStride = 28;

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
  pool.bone_weights = r.u32();
  if (pool.bone_weights) pool.part_id = r.u32();
  if (pool.stride > 256) throw FormatError("pool stride " + std::to_string(pool.stride));
  const size_t count = pool.count > 0 ? size_t(pool.count) : 0;
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
  }
  return pool;
}

CarbinSubsection read_subsection(BeReader& r) {
  CarbinSubsection s;
  r.skip(5);  // not decoded
  for (float& f : s.uv_transform) f = r.f32();
  r.skip(36);  // not decoded
  const uint32_t name_size = r.u32();
  if (name_size > 256) throw FormatError("sub-section name of " + std::to_string(name_size) + " bytes");
  auto name = r.raw(name_size);
  s.material.assign(name.begin(), name.end());
  s.lod = r.i32();
  s.index_type = r.u32();
  r.skip(56);  // 14 floats, not decoded
  s.version_words = {r.u32(), r.u32()};
  const uint32_t count = r.u32();
  s.index_size = r.u32();
  if (s.index_size != 2 && s.index_size != 4)
    throw FormatError("index size " + std::to_string(s.index_size) + " in " + s.material);
  auto bytes = r.raw(size_t(count) * s.index_size);
  s.indices.resize(count);
  for (uint32_t i = 0; i < count; ++i)
    s.indices[i] = s.index_size == 2 ? load_be16(&bytes[2 * i]) : load_be32(&bytes[4 * i]);
  r.skip(4);
  return s;
}

CarbinPart read_part(BeReader& r) {
  if (r.u32() != kPartMarker) throw FormatError("no part marker at " + std::to_string(r.at() - 4));
  CarbinPart part;
  for (float& f : part.bounds) f = r.f32();
  r.skip(28);                       // mass, centre of mass, damage fields: not decoded
  r.skip(size_t(r.u32()) * 16);     // 16-byte records (glass parts)
  r.skip(4);
  r.skip(size_t(r.u32()) * 2);      // shorts
  r.skip(12);
  auto name = r.raw(r.u8());
  part.name.assign(name.begin(), name.end());
  part.version = r.u32();
  part.lod_pool = read_pool(r);
  r.skip(4);
  const uint32_t count = r.u32();
  if (count > 4096) throw FormatError(std::to_string(count) + " sub-sections in " + part.name);
  part.subsections.reserve(count);
  for (uint32_t i = 0; i < count; ++i) part.subsections.push_back(read_subsection(r));
  r.skip(4);
  part.lod0_pool = read_pool(r);
  part.tail_word = r.u32();
  part.damage_flag = r.u8();
  for (int block = 0; block < 2; ++block) {  // LOD, then LOD0
    r.u32();  // kind
    const uint32_t n = r.u32(), stride = r.u32(), extra = r.u32();
    if (n > (1u << 22) || stride > 64 || extra > (1u << 16))
      throw FormatError("damage block " + std::to_string(n) + "x" + std::to_string(stride) +
                        " in " + part.name);
    r.skip(size_t(n) * stride + size_t(extra) * 4);
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

}  // namespace

bool CarbinPart::empty_box() const {
  for (float f : bounds)
    if (std::fabs(f) == FLT_MAX) return true;
  return false;
}

bool parse_carbin(std::span<const uint8_t> data, Carbin& out, std::string* error) {
  out = Carbin{};
  // The part count is the first u32 in 1..1024 followed by a part that
  // parses (the header's strings are not padded, so it can be unaligned).
  size_t count_at = SIZE_MAX;
  uint32_t count = 0;
  for (size_t at = 0; at + 48 < data.size(); ++at) {
    const uint32_t n = load_be32(&data[at]);
    if (n >= 1 && n <= 1024 && plausible_part(data, at + 4)) {
      count_at = at;
      count = n;
      break;
    }
  }
  if (count_at == SIZE_MAX) {
    // A header-only model (the F-350 Super Duty's lod0 has no parts).
    if (data.size() >= 4 && data.size() <= 4096 && load_be32(data.data()) == kPartMarker) {
      out.head_size = data.size();
      return true;
    }
    if (error) *error = "no parts found";
    return false;
  }
  out.head_size = count_at;
  try {
    BeReader r(data, count_at + 4);
    out.parts.reserve(count);
    for (uint32_t i = 0; i < count; ++i) out.parts.push_back(read_part(r));
    out.footer_size = r.remaining();
  } catch (const FormatError& e) {
    if (error) *error = std::string("part ") + std::to_string(out.parts.size()) + ": " + e.what();
    return false;
  }
  return true;
}

size_t carbin_bad_indices(const Carbin& carbin) {
  size_t bad = 0;
  for (const CarbinPart& part : carbin.parts) {
    for (const CarbinSubsection& s : part.subsections) {
      const CarbinPool& pool = s.lod == 0 ? part.lod0_pool : part.lod_pool;
      const uint32_t restart = s.index_size == 2 ? 0xFFFFu : 0x00FFFFFFu;
      const uint32_t limit = pool.count > 0 ? uint32_t(pool.count) : 0;
      for (uint32_t i : s.indices) {
        if (s.index_type == uint32_t(CarbinIndexType::kTriangleStrip) && i == restart) continue;
        if (i >= limit) ++bad;
      }
    }
  }
  return bad;
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
    out.assign(idx.begin(), idx.end() - (idx.size() % 3));
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
