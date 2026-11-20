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

Placement pvsz_placement(const PvszRecord& rec, bool transposed) {
  // The stored H = F * R_game^T, so the file-frame placement is H * F.
  Placement p;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      const double h = transposed ? rec.rotation[3 * j + i] : rec.rotation[3 * i + j];
      p.linear[i][j] = h * kF[j];
    }
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
