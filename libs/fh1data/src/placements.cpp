#include "fh1data/placements.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "fh1data/bytes.h"

namespace fh1 {
namespace {

constexpr size_t kMaximumString = 4096;
constexpr size_t kTableSearchLimit = 1u << 20;  // where the 28-byte table may start

size_t align(size_t at, size_t to) { return (at + to - 1) / to * to; }

std::string strip(const uint8_t* p, size_t n) {
  std::string s(reinterpret_cast<const char*>(p), n);
  while (!s.empty() && s.back() == '\0') s.pop_back();
  return s;
}

std::array<float, 3> vec3_pad(BeReader& r) {
  std::array<float, 3> v{r.f32(), r.f32(), r.f32()};
  r.skip(4);
  return v;
}

bool fail(std::string* error, const std::string& why) {
  if (error) *error = why;
  return false;
}

// The 28-byte record k at p: (u32, record number k, 1.0, 1.0, ten zero
// bytes, flag byte, type byte).
bool record28_ok(std::span<const uint8_t> d, size_t p, uint32_t k) {
  if (p + 28 > d.size()) return false;
  if (load_be32(&d[p + 4]) != k) return false;
  if (load_bef32(&d[p + 8]) != 1.0f || load_bef32(&d[p + 12]) != 1.0f) return false;
  for (size_t i = 16; i < 26; ++i)
    if (d[p + i] != 0) return false;
  return true;
}

// A counted list of u32-length-prefixed printable names at p; returns the
// offset after it, or 0.
size_t names_end(std::span<const uint8_t> d, size_t p, std::vector<std::string>* out) {
  if (p + 4 > d.size()) return 0;
  const uint32_t n = load_be32(&d[p]);
  p += 4;
  if (n > 65536) return 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (p + 4 > d.size()) return 0;
    const uint32_t len = load_be32(&d[p]);
    p += 4;
    if (len == 0 || len > kMaximumString || len > d.size() - p) return 0;
    const std::string s = strip(&d[p], len);
    if (s.empty()) return 0;
    for (char c : s)
      if (c < 32 || c > 126) return 0;
    if (out) out->push_back(s);
    p += len;
  }
  return p;
}

}  // namespace

float half_to_float(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31;
  const uint32_t exponent = (h >> 10) & 0x1F;
  const uint32_t mantissa = h & 0x3FF;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {  // subnormal: mantissa * 2^-24
      const float v = std::ldexp(float(mantissa), -24);
      std::memcpy(&bits, &v, 4);
      bits |= sign;
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  float out;
  std::memcpy(&out, &bits, 4);
  return out;
}

// --- object table ---------------------------------------------------------------

bool parse_object_table(std::span<const uint8_t> d, PvsObjectTable& out, std::string* error) {
  out = PvsObjectTable{};
  if (d.size() < 12 || std::memcmp(d.data(), "FPVS", 4) != 0)
    return fail(error, "not an FPVS file (no magic)");
  out.version = load_be32(&d[4]);
  const size_t limit = std::min(d.size(), kTableSearchLimit);
  for (size_t p = 8; p + 4 <= limit; ++p) {
    const uint32_t n = load_be32(&d[p]);
    if (n == 0 || n > (d.size() - p - 4) / 28) continue;
    if (!record28_ok(d, p + 4, 0) || !record28_ok(d, p + 4 + size_t(n - 1) * 28, n - 1)) continue;
    bool all = true;
    for (uint32_t k = 1; k + 1 < n && all; ++k) all = record28_ok(d, p + 4 + size_t(k) * 28, k);
    if (!all) continue;
    std::vector<std::string> shaders;
    const size_t after_names = names_end(d, p + 4 + size_t(n) * 28, &shaders);
    if (!after_names || after_names + 4 > d.size()) continue;
    const uint32_t objects = load_be32(&d[after_names]);
    if (objects == 0 || objects > (d.size() - after_names - 4) / 18) continue;
    out.record_table_offset = p;
    out.record_count = n;
    out.shaders = std::move(shaders);
    out.object_table_offset = after_names;
    out.objects.resize(objects);
    const uint8_t* q = &d[after_names + 4];
    for (auto& o : out.objects) {
      o.member = load_be16(q);
      o.field_2 = int16_t(load_be16(q + 2));
      o.field_4 = load_be16(q + 4);
      o.field_6 = load_be16(q + 6);
      o.field_8 = load_be32(q + 8);
      o.field_12 = load_be32(q + 12);
      o.field_16 = load_be16(q + 16);
      q += 18;
    }
    out.textures.resize(n);
    for (uint32_t k = 0; k < n; ++k) {
      const uint8_t* t = &d[p + 4 + size_t(k) * 28];
      out.textures[k].id = load_be32(t);
      out.textures[k].flag = t[26];
      out.textures[k].type = t[27];
    }
    try {
      BeReader r(d, size_t(q - d.data()));
      const uint32_t lists = r.u32();
      if (lists > 1u << 20) throw FormatError(std::to_string(lists) + " model lists");
      for (uint32_t want : {2u, 0u, 1u})
        if (r.u32() != want) throw FormatError("model list header is not 2, 0, 1");
      out.model_textures.resize(lists);
      for (auto& list : out.model_textures) {
        const uint32_t k = r.u32();
        if (k > r.remaining() / 4) throw FormatError("effect indices run past the end");
        r.skip(size_t(k) * 4 + 15 * 4);
        const uint32_t t = r.u32();
        if (t > r.remaining() / 4) throw FormatError("texture indices run past the end");
        list.resize(t);
        for (auto& v : list) v = r.u32();
      }
    } catch (const FormatError& e) {
      out.model_textures.clear();
      out.texture_list_error = e.what();
    }
    return true;
  }
  return fail(error, "no 28-byte record table followed by shader names and an object table "
                     "within the first " + std::to_string(limit) + " bytes");
}

// --- .pgeo ------------------------------------------------------------------------

uint32_t pgeo_kind(std::span<const uint8_t> d, std::string* error) {
  if (d.size() < 0x34 || std::memcmp(d.data(), "OEGP", 4) != 0) {
    fail(error, "not a .pgeo tile (no OEGP magic)");
    return 0;
  }
  const uint32_t kind = load_be32(&d[0x30]);
  if (kind == 0) fail(error, "kind word 0");
  return kind;
}

PgeoBillboard decode_pgeo_billboard(const uint8_t* p, const std::array<float, 3>& size) {
  PgeoBillboard b;
  auto bits = [](uint32_t w, int at, int n) { return (w >> at) & ((1u << n) - 1); };
  auto sbits = [&](uint32_t w, int at, int n) {
    const int32_t v = int32_t(bits(w, at, n));
    return v >= (1 << (n - 1)) ? v - (1 << n) : v;
  };
  // +0: 10_11_11 unsigned, stored X, Z, height.
  const uint32_t pos = load_be32(p);
  b.position_word = pos;
  b.base = {float(bits(pos, 0, 11) / 2047.0 * size[0]), float(bits(pos, 22, 10) / 1023.0 * size[1]),
            float(bits(pos, 11, 11) / 2047.0 * size[2])};
  // +4: 10_11_11 signed, x, y, z.
  const uint32_t ax = load_be32(p + 4);
  b.axis = {float(sbits(ax, 0, 11) / 1023.0), float(sbits(ax, 11, 11) / 1023.0), float(sbits(ax, 22, 10) / 511.0)};
  // +12: 2_10_10_10 signed, as NORMAL.
  const uint32_t nw = load_be32(p + 12);
  b.normal = {float(std::max(-1.0, sbits(nw, 0, 10) / 511.0)), float(std::max(-1.0, sbits(nw, 10, 10) / 511.0)),
              float(std::max(-1.0, sbits(nw, 20, 10) / 511.0))};
  const uint32_t bottom = load_be32(p + 16), top = load_be32(p + 20);
  b.u1 = float(bits(bottom, 11, 11) / 2047.0);
  b.v1 = float(bits(bottom, 22, 10) / 1023.0);
  b.u0 = float(bits(top, 0, 11) / 2047.0);
  b.v0 = float(bits(top, 11, 11) / 2047.0);
  b.half_width = half_to_float(load_be16(p + 24));
  b.height = half_to_float(load_be16(p + 26));
  b.near = half_to_float(load_be16(p + 28));
  b.far = half_to_float(load_be16(p + 30));
  b.tint = {p[33], p[34], p[35], p[32]};
  b.extra = {p[37], p[38], p[39], p[36]};
  return b;
}

bool parse_pgeo(std::span<const uint8_t> d, PgeoTile& out, std::string* error) {
  out = PgeoTile{};
  try {
    out.kind = pgeo_kind(d, error);
    if (out.kind == 0) return false;
    if (out.kind != 7) return fail(error, "tile kind " + std::to_string(out.kind) + " is not decoded");
    if (d.size() < 0x60) throw FormatError("kind-7 header runs past the end");
    BeReader r(d, 0x10);
    out.box_min = vec3_pad(r);
    out.box_max = vec3_pad(r);
    out.distance = load_bef32(&d[0x38]);
    const uint32_t name_size = load_be32(&d[0x3C]);
    const uint32_t references = load_be32(&d[0x40]);
    out.string_records = load_be32(&d[0x44]);
    const uint32_t total = load_be32(&d[0x48]);
    out.trailing_size = load_be32(&d[0x50]);
    const uint32_t models = load_be32(&d[0x54]);
    if (total != d.size())
      throw FormatError("stated size " + std::to_string(total) + ", file " + std::to_string(d.size()));
    if (out.trailing_size > d.size()) throw FormatError("trailing block larger than the file");
    if (name_size > kMaximumString) throw FormatError("name of " + std::to_string(name_size) + " bytes");
    // Our reading: the header is 0x60 bytes, or 0x98 when the word at
    // +0x58 is set (the name then follows a longer header).
    out.header_size = load_be32(&d[0x58]) ? 0x98 : 0x60;
    if (out.header_size == 0x98) {
      if (d.size() < 0x98) throw FormatError("kind-7 long header runs past the end");
      const uint8_t n = d[0x84];
      if (0x85 + size_t(n) > 0x98) throw FormatError("route list of " + std::to_string(n) + " bytes");
      out.has_routes = true;
      out.routes.assign(d.begin() + 0x85, d.begin() + 0x85 + n);
    }
    r.seek(out.header_size);
    const auto name = r.raw(name_size);
    out.name = strip(name.data(), name.size());

    r.seek(align(r.at(), 16));
    if (models > r.remaining() / 0x40) throw FormatError(std::to_string(models) + " model records run past the end");
    out.models.resize(models);
    std::vector<uint32_t> text_sizes(models);
    std::vector<std::array<uint32_t, 3>> counts(models);
    for (uint32_t i = 0; i < models; ++i) {
      auto& m = out.models[i];
      m.box_min = vec3_pad(r);
      m.box_max = vec3_pad(r);
      text_sizes[i] = r.u32();
      if (text_sizes[i] > kMaximumString) throw FormatError("model record string of " + std::to_string(text_sizes[i]) + " bytes");
      for (int k = 0; k < 3; ++k) {
        counts[i][k] = r.u32();
        r.skip(4);  // slot: a runtime value
      }
      m.word = r.u32();
    }
    r.seek(align(r.at(), 4));
    for (uint32_t i = 0; i < models; ++i)
      for (int k = 0; k < 3; ++k) {
        if (counts[i][k] > r.remaining() / 4) throw FormatError("object ID list runs past the end");
        auto& ids = out.models[i].ids[k];
        ids.resize(counts[i][k]);
        for (auto& id : ids) id = r.u32();
      }
    for (uint32_t i = 0; i < models; ++i) {
      const auto t = r.raw(text_sizes[i]);
      out.models[i].text = strip(t.data(), t.size());
    }
    r.seek(align(r.at(), 4));
    if (references > r.remaining() / 8) throw FormatError("references run past the end");
    out.references.resize(references);
    for (auto& [value, flag] : out.references) {
      value = r.u32();
      flag = r.u32();
    }
    if (out.string_records > r.remaining() / 8) throw FormatError("string records run past the end");
    r.skip(size_t(out.string_records) * 8);

    // The class part: a 0x14-byte header (group count at +0xC), then the
    // 0x5C-byte group records.
    const size_t class_part = r.at();
    r.need(0x14);
    const uint32_t groups = load_be32(&d[class_part + 0xC]);
    r.skip(0x14);
    if (groups > r.remaining() / 0x5C) throw FormatError(std::to_string(groups) + " groups run past the end");
    out.groups.resize(groups);
    std::vector<uint32_t> instance_counts(groups);
    for (uint32_t g = 0; g < groups; ++g) {
      const size_t at = r.at();
      auto& gr = out.groups[g];
      gr.key = load_be32(&d[at]);
      gr.model_record = load_be32(&d[at + 0x2C]);
      instance_counts[g] = load_be32(&d[at + 0x30]);
      gr.packed_count = load_be32(&d[at + 0x34]);
      gr.record32_count = load_be32(&d[at + 0x38]);
      for (int k = 0; k < 4; ++k) gr.distances[k] = load_bef32(&d[at + 0x3C + 4 * k]);
      for (int k = 0; k < 4; ++k) gr.atlas_rect[size_t(k)] = load_bef32(&d[at + 0x08 + 4 * k]);
      gr.sprite_size = {load_bef32(&d[at + 0x18]), load_bef32(&d[at + 0x1C])};
      if (gr.model_record >= models)
        throw FormatError("group " + std::to_string(g) + " names model record " +
                          std::to_string(gr.model_record) + " of " + std::to_string(models));
      r.skip(0x5C);
    }
    for (uint32_t g = 0; g < groups; ++g) {
      auto& gr = out.groups[g];
      if (instance_counts[g]) r.seek(align(r.at(), 16));
      if (instance_counts[g] > r.remaining() / 96) throw FormatError("instances run past the end");
      gr.instances.resize(instance_counts[g]);
      for (auto& in : gr.instances) {
        for (auto& row : in.rows) row = vec3_pad(r);
        in.translation = {r.f32(), r.f32(), r.f32()};
        in.w = r.f32();
        for (float& c : in.colour) c = r.f32();
        in.normal = {r.f32(), r.f32(), r.f32()};
        in.word = r.u32();
      }
      if (gr.record32_count > r.remaining() / 32) throw FormatError("32-byte records run past the end");
      r.skip(size_t(gr.record32_count) * 32);
    }
    if (r.at() != d.size() - out.trailing_size)
      throw FormatError("groups end at " + std::to_string(r.at()) + ", the trailing block starts at " +
                        std::to_string(d.size() - out.trailing_size));
    // The trailing block: one 40-byte billboard record per sprite, in group
    // order. Left empty when the groups' counts do not cover it.
    uint64_t packed = 0;
    for (const auto& g : out.groups) packed += g.packed_count;
    if (packed * 40 == out.trailing_size) {
      const std::array<float, 3> size{out.box_max[0] - out.box_min[0], out.box_max[1] - out.box_min[1],
                                      out.box_max[2] - out.box_min[2]};
      const uint8_t* p = d.data() + (d.size() - out.trailing_size);
      out.billboards.reserve(size_t(packed));
      for (uint32_t g = 0; g < groups; ++g)
        for (uint32_t k = 0; k < out.groups[g].packed_count; ++k, p += 40) {
          out.billboards.push_back(decode_pgeo_billboard(p, size));
          out.billboards.back().group = g;
        }
    }
  } catch (const FormatError& e) {
    const uint32_t kind = out.kind;
    out = PgeoTile{};
    out.kind = kind;
    return fail(error, e.what());
  }
  return true;
}

// --- .pvsz --------------------------------------------------------------------------

namespace {

constexpr size_t kPvszRecord = 53;  // up to and including the flag byte

// Length of the record at o, or 0 when no record fits there.
size_t pvsz_record_size(std::span<const uint8_t> d, size_t o) {
  if (o + kPvszRecord > d.size()) return 0;
  const uint8_t flag = d[o + kPvszRecord - 1];
  if (flag == 0) return kPvszRecord;
  if (flag != 1 || o + kPvszRecord + 5 > d.size()) return 0;
  return kPvszRecord + 4 + 1 + d[o + kPvszRecord + 4] + 32;
}

PvszRecord read_pvsz_record(std::span<const uint8_t> d, size_t o) {
  PvszRecord rec;
  rec.offset = o;
  for (int k = 0; k < 3; ++k) rec.distances[k] = half_to_float(load_be16(&d[o + 2 * k]));
  for (int k = 0; k < 3; ++k) rec.position[k] = load_bef32(&d[o + 6 + 4 * k]);
  for (int k = 0; k < 9; ++k) rec.rotation[k] = half_to_float(load_be16(&d[o + 18 + 2 * k]));
  std::memcpy(rec.unknown.data(), &d[o + 36], 16);
  rec.extended = d[o + 52] == 1;
  if (rec.extended) {
    rec.word = load_be32(&d[o + 53]);
    const uint8_t c = d[o + 57];
    rec.extra.assign(d.begin() + o + 58, d.begin() + o + 58 + c);
    rec.name = strip(&d[o + 58 + c], 32);
  }
  return rec;
}

}  // namespace

bool pvsz_record_rigid(const PvszRecord& r) {
  for (float p : r.position)
    if (!std::isfinite(p)) return false;
  for (float h : r.rotation)
    if (!std::isfinite(h)) return false;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double dot = 0;
      for (int k = 0; k < 3; ++k) dot += double(r.rotation[3 * i + k]) * r.rotation[3 * j + k];
      if (std::fabs(dot - (i == j ? 1.0 : 0.0)) >= 0.01) return false;
    }
  return true;
}

bool parse_pvsz(std::span<const uint8_t> d, PvszZone& out, std::string* error, PvszStartRule rule) {
  out = PvszZone{};
  if (d.size() < 4) return fail(error, "zone shorter than its count");
  const uint32_t n = load_be32(d.data());
  if (n > (d.size() - 4) / 4) return fail(error, std::to_string(n) + " IDs run past the end");
  out.ids.resize(n);
  for (uint32_t i = 0; i < n; ++i) out.ids[i] = load_be32(&d[4 + 4 * size_t(i)]);
  const size_t first = 4 + 4 * size_t(n);
  // records[o - first]: how many records run from o to exactly the end of
  // the file, or -1. One backward pass.
  const size_t span = d.size() - first;
  std::vector<int32_t> count(span + 1, -1);
  count[span] = 0;
  for (size_t o = d.size(); o-- > first;) {
    const size_t len = pvsz_record_size(d, o);
    if (len && o + len <= d.size() && count[o + len - first] >= 0)
      count[o - first] = count[o + len - first] + 1;
  }
  for (size_t o = first; o <= d.size(); ++o)
    if (count[o - first] == int32_t(n)) out.starts.push_back(o);
  if (out.starts.empty())
    return fail(error, "no start offset from which " + std::to_string(n) +
                           " records end exactly at the end of the file");
  size_t best = 0, best_rigid = 0;
  std::vector<PvszRecord> best_records;
  for (size_t c = 0; c < out.starts.size(); ++c) {
    std::vector<PvszRecord> records;
    records.reserve(n);
    size_t rigid = 0;
    for (size_t o = out.starts[c], k = 0; k < n; ++k) {
      records.push_back(read_pvsz_record(d, o));
      rigid += pvsz_record_rigid(records.back());
      o += pvsz_record_size(d, o);
    }
    if (c == 0 || rigid > best_rigid) {
      best = c;
      best_rigid = rigid;
      best_records = std::move(records);
    }
    if (rule == PvszStartRule::kFirst) break;
  }
  out.start = out.starts[best];
  out.records = std::move(best_records);
  out.rigid_records = best_rigid;
  return true;
}

// --- placing --------------------------------------------------------------------------

std::array<double, 3> Placement::apply(const std::array<float, 3>& v) const {
  std::array<double, 3> out{};
  for (int i = 0; i < 3; ++i)
    out[i] = linear[i][0] * v[0] + linear[i][1] * v[1] + linear[i][2] * v[2] + offset[i];
  return out;
}

namespace {
constexpr double kF[3] = {1, 1, -1};  // the game frame is the file frame with z negated
}

Placement pgeo_placement(const PgeoInstance& in) {
  // F * R^T * F, with R's rows the stored rows: (R^T)[i][j] = rows[j][i].
  Placement p;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) p.linear[i][j] = kF[i] * kF[j] * double(in.rows[j][i]);
    p.offset[i] = kF[i] * double(in.translation[i]);
  }
  return p;
}

Placement pvsz_placement(const PvszRecord& rec) {
  // R[r][c] = h[3c + r] * F[r]: the halves are R's columns, third
  // component negated.
  Placement p;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) p.linear[i][j] = double(rec.rotation[3 * j + i]) * kF[i];
    p.offset[i] = kF[i] * double(rec.position[i]);
  }
  return p;
}

bool rmb_lod_part(const std::string& name, std::string& family, int& level) {
  std::string n = name;
  for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
  for (size_t at = n.find("_lod"); at != std::string::npos; at = n.find("_lod", at + 1)) {
    if (at + 5 < n.size() && std::isdigit(static_cast<unsigned char>(n[at + 4])) &&
        std::isdigit(static_cast<unsigned char>(n[at + 5]))) {
      level = (n[at + 4] - '0') * 10 + (n[at + 5] - '0');
      n[at + 4] = n[at + 5] = '*';
      family = n;
      return true;
    }
  }
  level = -1;
  family = n;
  return false;
}

}  // namespace fh1
