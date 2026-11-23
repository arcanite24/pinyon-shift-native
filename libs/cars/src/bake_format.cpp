#include "cars/bake_format.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cars::bake {
namespace {

constexpr char kMagic[8] = {'S', 'H', 'F', 'T', 'B', 'A', 'K', 'E'};
constexpr char kEnd[8] = {'E', 'N', 'D', 'B', 'A', 'K', 'E', '\0'};

#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
inline constexpr bool kLittleEndian = true;
#else
inline constexpr bool kLittleEndian = false;
#endif

static_assert(sizeof(Vertex) == 32 && offsetof(Vertex, position) == 0 && offsetof(Vertex, frame) == 12 &&
                  offsetof(Vertex, uv0) == 20 && offsetof(Vertex, uv1) == 24 && offsetof(Vertex, lighting) == 28,
              "Vertex layout must be 32 packed bytes");
static_assert(sizeof(std::array<float, 3>) == 12, "MorphTarget vertex must be 12 packed bytes");

class Writer {
 public:
  std::vector<uint8_t> out;
  void raw(const void* p, size_t n) {
    if (n == 0) return;
    const size_t off = out.size();
    out.resize(off + n);
    std::memcpy(out.data() + off, p, n);
  }
  void u8(uint8_t v) { out.push_back(v); }
  void u16(uint16_t v) {
    if constexpr (kLittleEndian) {
      raw(&v, 2);
    } else {
      out.push_back(uint8_t(v));
      out.push_back(uint8_t(v >> 8));
    }
  }
  void u32(uint32_t v) {
    if constexpr (kLittleEndian) {
      raw(&v, 4);
    } else {
      for (int s = 0; s < 32; s += 8) out.push_back(uint8_t(v >> s));
    }
  }
  void i64(int64_t v) {
    if constexpr (kLittleEndian) {
      raw(&v, 8);
    } else {
      for (int s = 0; s < 64; s += 8) out.push_back(uint8_t(uint64_t(v) >> s));
    }
  }
  void f32(float f) {
    if constexpr (kLittleEndian) {
      raw(&f, 4);
    } else {
      uint32_t b;
      std::memcpy(&b, &f, 4);
      u32(b);
    }
  }
  void str(const std::string& s) {
    u32(uint32_t(s.size()));
    raw(s.data(), s.size());
  }
  void bytes(const std::vector<uint8_t>& b) {
    u32(uint32_t(b.size()));
    raw(b.data(), b.size());
  }
};

size_t estimate_encoded_size(const Bake& b) {
  size_t total = 8 + 4 + 4 + 8;  // magic, version, kind, end
  if (b.kind == Kind::kCar) {
    const CarInfo& c = b.car;
    total += 4 + c.media_name.size() + 4 + c.display_name.size() + 4 + c.origin.size() + 8;
    total += 4;
    for (const Colour& col : c.colours) {
      total += 8 + 8 + 1 + 4 + 4 + col.atlas_lod0.size() + 4 + col.atlas_lod.size() + 1 + 4;
    }
    total += 4 + c.wheel_media.size() + 4 + c.tyre_media.size() + 4 + c.tyre_texture_media.size();
    total += 2 * (8 + 8 + 8 + 4 + 4 + 4);
    total += 2 * 4 + 4 * 4 + 1 + 1 + 4 * 12 + 4 * 12 + 10 * 12 + 1 + 8;
    total += 4;
    for (const auto& s : c.stock_rows) {
      total += 8 + 1 + 1 + 4 + 4;
      for (const auto& n : s.parts) total += 4 + n.size();
      for (const auto& n : s.removed) total += 4 + n.size();
    }
    total += 4;
    for (const auto& n : c.notes) total += 4 + n.size();
  }
  total += 4;
  for (const Model& m : b.models) {
    total += 4 + m.name.size() + 4 + 4 + 4 + 16 + 4;
    for (const Part& p : m.parts) {
      total += 4 + p.name.size() + 36;
      total += 4 + p.lod_vertices.size() * 32;
      total += 4 + p.lod0_vertices.size() * 32;
      for (const auto* morphs : {&p.lod_morphs, &p.lod0_morphs}) {
        total += 4;
        for (const MorphTarget& t : *morphs) total += 4 + t.size() * 12;
      }
      total += 4;
      for (const Subsection& s : p.subsections) {
        total += 4 + s.technique.size() + 4 + 1 + 4 + 32;
        total += 4 + s.slots[0].size() + 4 + s.slots[1].size();
        total += 4 + s.indices.size() * 4;
      }
    }
  }
  total += 4;
  for (const Texture& t : b.textures) {
    total += 4 + t.name.size() + 4 + 4 + 4;
    for (const TextureLevel& l : t.levels) total += 4 + 4 + 4 + l.data.size();
  }
  total += 4;
  for (const LiveryPlane& p : b.livery) {
    total += 4 + p.name.size() + 1 + 9 * 4 + 4 + p.x_axis.size() + 4 + p.y_axis.size();
  }
  return total;
}

class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& d) : d_(d) {}
  void need(size_t n) {
    if (n > d_.size() - at_) throw std::runtime_error("truncated at " + std::to_string(at_));
  }
  uint8_t u8() {
    need(1);
    return d_[at_++];
  }
  uint16_t u16() {
    need(2);
    const uint16_t v = uint16_t(d_[at_] | d_[at_ + 1] << 8);
    at_ += 2;
    return v;
  }
  uint32_t u32() {
    need(4);
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(d_[at_ + size_t(i)]) << (8 * i);
    at_ += 4;
    return v;
  }
  int64_t i64() {
    need(8);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(d_[at_ + size_t(i)]) << (8 * i);
    at_ += 8;
    return int64_t(v);
  }
  float f32() {
    const uint32_t b = u32();
    float f;
    std::memcpy(&f, &b, 4);
    return f;
  }
  std::string str() {
    const uint32_t n = u32();
    need(n);
    std::string s(reinterpret_cast<const char*>(&d_[at_]), n);
    at_ += n;
    return s;
  }
  std::vector<uint8_t> bytes() {
    const uint32_t n = u32();
    need(n);
    std::vector<uint8_t> b(d_.begin() + std::ptrdiff_t(at_), d_.begin() + std::ptrdiff_t(at_ + n));
    at_ += n;
    return b;
  }
  uint32_t count(size_t item_bytes) {
    const uint32_t n = u32();
    if (item_bytes && size_t(n) * item_bytes > d_.size() - at_) throw std::runtime_error("bad count");
    return n;
  }
  void raw(void* p, size_t n) {
    need(n);
    std::memcpy(p, &d_[at_], n);
    at_ += n;
  }
  size_t at() const { return at_; }

 private:
  const std::vector<uint8_t>& d_;
  size_t at_ = 0;
};

void write_vertices(Writer& w, const std::vector<Vertex>& vs) {
  w.u32(uint32_t(vs.size()));
  if constexpr (kLittleEndian) {
    w.raw(vs.data(), vs.size() * sizeof(Vertex));
  } else {
    for (const Vertex& v : vs) {
      for (float f : v.position) w.f32(f);
      for (int16_t c : v.frame) w.u16(uint16_t(c));
      for (uint16_t c : v.uv0) w.u16(c);
      for (uint16_t c : v.uv1) w.u16(c);
      w.raw(v.lighting.data(), 4);
    }
  }
}

std::vector<Vertex> read_vertices(Reader& r) {
  std::vector<Vertex> vs(r.count(32));
  if constexpr (kLittleEndian) {
    if (!vs.empty()) r.raw(vs.data(), vs.size() * sizeof(Vertex));
  } else {
    for (Vertex& v : vs) {
      for (float& f : v.position) f = r.f32();
      for (int16_t& c : v.frame) c = int16_t(r.u16());
      for (uint16_t& c : v.uv0) c = r.u16();
      for (uint16_t& c : v.uv1) c = r.u16();
      r.raw(v.lighting.data(), 4);
    }
  }
  return vs;
}

}  // namespace

const char* material_class_name(MaterialClass c) {
  switch (c) {
    case MaterialClass::kOpaque: return "opaque";
    case MaterialClass::kPaint: return "paint";
    case MaterialClass::kGlass: return "glass";
    case MaterialClass::kLamp: return "lamp";
    case MaterialClass::kInterior: return "interior";
    case MaterialClass::kGauge: return "gauge";
    case MaterialClass::kTyre: return "tyre";
    case MaterialClass::kRim: return "rim";
    case MaterialClass::kCaliper: return "caliper";
    case MaterialClass::kChrome: return "chrome";
    case MaterialClass::kCarbon: return "carbon";
    case MaterialClass::kHidden: return "hidden";
  }
  return "?";
}

std::vector<uint8_t> encode(const Bake& b) {
  Writer w;
  w.out.reserve(estimate_encoded_size(b));
  w.raw(kMagic, 8);
  w.u32(kVersion);
  w.u32(uint32_t(b.kind));
  if (b.kind == Kind::kCar) {
    const CarInfo& c = b.car;
    w.str(c.media_name);
    w.str(c.display_name);
    w.str(c.origin);
    w.i64(c.car_id);
    w.u32(uint32_t(c.colours.size()));
    for (const Colour& col : c.colours) {
      w.i64(col.id);
      w.i64(col.sequence);
      w.u8(col.metallic ? 1 : 0);
      w.u32(col.rgb);
      w.str(col.atlas_lod0);
      w.str(col.atlas_lod);
      w.u8(col.has_stripe ? 1 : 0);
      w.u32(col.stripe_rgb);
    }
    w.str(c.wheel_media);
    w.str(c.tyre_media);
    w.str(c.tyre_texture_media);
    for (int a = 0; a < 2; ++a) {
      w.i64(c.tyre_width_mm[size_t(a)]);
      w.i64(c.tyre_aspect[size_t(a)]);
      w.i64(c.rim_inches[size_t(a)]);
      w.f32(c.ride_height[size_t(a)]);
      w.u32(c.caliper_rgb[size_t(a)]);
      w.f32(c.caliper_angle[size_t(a)]);
    }
    for (float f : c.track_outer) w.f32(f);
    for (float f : {c.wheelbase, c.body_bottom_y, c.lift, c.pitch}) w.f32(f);
    w.u8(c.has_body ? 1 : 0);
    w.u8(c.has_track_offsets ? 1 : 0);
    for (const auto& p : c.wheel_centres)
      for (float f : p) w.f32(f);
    for (const auto& p : c.wheel_centres_body)
      for (float f : p) w.f32(f);
    for (const auto& p : c.part_positions)
      for (float f : p) w.f32(f);
    w.u8(c.has_livery ? 1 : 0);
    w.i64(c.wing_mask);
    w.u32(uint32_t(c.stock_rows.size()));
    for (const auto& s : c.stock_rows) {
      w.i64(s.car_id);
      w.u8(s.race_livery ? 1 : 0);
      w.u8(s.known ? 1 : 0);
      w.u32(uint32_t(s.parts.size()));
      for (const auto& n : s.parts) w.str(n);
      w.u32(uint32_t(s.removed.size()));
      for (const auto& n : s.removed) w.str(n);
    }
    w.u32(uint32_t(c.notes.size()));
    for (const auto& n : c.notes) w.str(n);
  }
  w.u32(uint32_t(b.models.size()));
  for (const Model& m : b.models) {
    w.str(m.name);
    w.u32(uint32_t(m.role));
    w.u32(uint32_t(m.corner));
    w.u32(m.header_kind);
    for (float f : m.header_bounds) w.f32(f);
    w.u32(uint32_t(m.parts.size()));
    for (const Part& p : m.parts) {
      w.str(p.name);
      for (float f : p.bounds) w.f32(f);
      write_vertices(w, p.lod_vertices);
      write_vertices(w, p.lod0_vertices);
      for (const auto* morphs : {&p.lod_morphs, &p.lod0_morphs}) {
        w.u32(uint32_t(morphs->size()));
        for (const MorphTarget& t : *morphs) {
          w.u32(uint32_t(t.size()));
          if constexpr (kLittleEndian) {
            w.raw(t.data(), t.size() * 12);
          } else {
            for (const auto& v : t)
              for (float f : v) w.f32(f);
          }
        }
      }
      w.u32(uint32_t(p.subsections.size()));
      for (const Subsection& s : p.subsections) {
        w.str(s.technique);
        w.u32(uint32_t(s.material));
        w.u8(s.transparent ? 1 : 0);
        w.u32(uint32_t(s.lod));
        for (float f : s.uv_transform) w.f32(f);
        w.str(s.slots[0]);
        w.str(s.slots[1]);
        w.u32(uint32_t(s.indices.size()));
        if constexpr (kLittleEndian) {
          w.raw(s.indices.data(), s.indices.size() * sizeof(uint32_t));
        } else {
          for (uint32_t i : s.indices) w.u32(i);
        }
      }
    }
  }
  w.u32(uint32_t(b.textures.size()));
  for (const Texture& t : b.textures) {
    w.str(t.name);
    w.u32(uint32_t(t.format));
    w.raw(t.swizzle.data(), 4);
    w.u32(uint32_t(t.levels.size()));
    for (const TextureLevel& l : t.levels) {
      w.u32(l.width);
      w.u32(l.height);
      w.bytes(l.data);
    }
  }
  w.u32(uint32_t(b.livery.size()));
  for (const LiveryPlane& p : b.livery) {
    w.str(p.name);
    w.u8(p.valid ? 1 : 0);
    for (float f : {p.left, p.right, p.top, p.bottom, p.x_origin, p.y_origin, p.x_scale, p.y_scale, p.rotation})
      w.f32(f);
    w.str(p.x_axis);
    w.str(p.y_axis);
  }
  w.raw(kEnd, 8);
  return std::move(w.out);
}

bool decode(const std::vector<uint8_t>& bytes, Bake& b, std::string* error) {
  b = Bake{};
  try {
    Reader r(bytes);
    char magic[8];
    r.raw(magic, 8);
    if (std::memcmp(magic, kMagic, 8) != 0) throw std::runtime_error("not a .shiftbake file");
    if (const uint32_t v = r.u32(); v != kVersion)
      throw std::runtime_error("version " + std::to_string(v) + ", expected " + std::to_string(kVersion));
    b.kind = Kind(r.u32());
    if (b.kind == Kind::kCar) {
      CarInfo& c = b.car;
      c.media_name = r.str();
      c.display_name = r.str();
      c.origin = r.str();
      c.car_id = r.i64();
      c.colours.resize(r.count(0));
      for (Colour& col : c.colours) {
        col.id = r.i64();
        col.sequence = r.i64();
        col.metallic = r.u8() != 0;
        col.rgb = r.u32();
        col.atlas_lod0 = r.str();
        col.atlas_lod = r.str();
        col.has_stripe = r.u8() != 0;
        col.stripe_rgb = r.u32();
      }
      c.wheel_media = r.str();
      c.tyre_media = r.str();
      c.tyre_texture_media = r.str();
      for (int a = 0; a < 2; ++a) {
        c.tyre_width_mm[size_t(a)] = r.i64();
        c.tyre_aspect[size_t(a)] = r.i64();
        c.rim_inches[size_t(a)] = r.i64();
        c.ride_height[size_t(a)] = r.f32();
        c.caliper_rgb[size_t(a)] = r.u32();
        c.caliper_angle[size_t(a)] = r.f32();
      }
      for (float& f : c.track_outer) f = r.f32();
      for (float* f : {&c.wheelbase, &c.body_bottom_y, &c.lift, &c.pitch}) *f = r.f32();
      c.has_body = r.u8() != 0;
      c.has_track_offsets = r.u8() != 0;
      for (auto& p : c.wheel_centres)
        for (float& f : p) f = r.f32();
      for (auto& p : c.wheel_centres_body)
        for (float& f : p) f = r.f32();
      for (auto& p : c.part_positions)
        for (float& f : p) f = r.f32();
      c.has_livery = r.u8() != 0;
      c.wing_mask = r.i64();
      c.stock_rows.resize(r.count(14));
      for (auto& s : c.stock_rows) {
        s.car_id = r.i64();
        s.race_livery = r.u8() != 0;
        s.known = r.u8() != 0;
        s.parts.resize(r.count(4));
        for (auto& n : s.parts) n = r.str();
        s.removed.resize(r.count(4));
        for (auto& n : s.removed) n = r.str();
      }
      c.notes.resize(r.count(4));
      for (auto& n : c.notes) n = r.str();
    }
    b.models.resize(r.count(12));
    for (Model& m : b.models) {
      m.name = r.str();
      m.role = Role(r.u32());
      m.corner = Corner(r.u32());
      m.header_kind = r.u32();
      for (float& f : m.header_bounds) f = r.f32();
      m.parts.resize(r.count(4));
      for (Part& p : m.parts) {
        p.name = r.str();
        for (float& f : p.bounds) f = r.f32();
        p.lod_vertices = read_vertices(r);
        p.lod0_vertices = read_vertices(r);
        for (auto* morphs : {&p.lod_morphs, &p.lod0_morphs}) {
          morphs->resize(r.count(4));
          for (MorphTarget& t : *morphs) {
            t.resize(r.count(12));
            if constexpr (kLittleEndian) {
              if (!t.empty()) r.raw(t.data(), t.size() * 12);
            } else {
              for (auto& v : t)
                for (float& f : v) f = r.f32();
            }
          }
        }
        p.subsections.resize(r.count(4));
        for (Subsection& s : p.subsections) {
          s.technique = r.str();
          s.material = MaterialClass(r.u32());
          s.transparent = r.u8() != 0;
          s.lod = int32_t(r.u32());
          for (float& f : s.uv_transform) f = r.f32();
          s.slots[0] = r.str();
          s.slots[1] = r.str();
          s.indices.resize(r.count(4));
          if constexpr (kLittleEndian) {
            if (!s.indices.empty()) r.raw(s.indices.data(), s.indices.size() * sizeof(uint32_t));
          } else {
            for (uint32_t& i : s.indices) i = r.u32();
          }
        }
      }
    }
    b.textures.resize(r.count(4));
    for (Texture& t : b.textures) {
      t.name = r.str();
      t.format = TextureFormat(r.u32());
      r.raw(t.swizzle.data(), 4);
      t.levels.resize(r.count(12));
      for (TextureLevel& l : t.levels) {
        l.width = r.u32();
        l.height = r.u32();
        l.data = r.bytes();
      }
    }
    b.livery.resize(r.count(4));
    for (LiveryPlane& p : b.livery) {
      p.name = r.str();
      p.valid = r.u8() != 0;
      for (float* f : {&p.left, &p.right, &p.top, &p.bottom, &p.x_origin, &p.y_origin, &p.x_scale,
                       &p.y_scale, &p.rotation})
        *f = r.f32();
      p.x_axis = r.str();
      p.y_axis = r.str();
    }
    char end[8];
    r.raw(end, 8);
    if (std::memcmp(end, kEnd, 8) != 0 || r.at() != bytes.size()) throw std::runtime_error("bad end marker");
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
  return true;
}

bool write_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, std::string* error) {
  std::error_code ec;
  const auto parent = path.parent_path();
  if (!parent.empty() && !std::filesystem::is_directory(parent, ec)) {
    std::filesystem::create_directories(parent, ec);
  }
  const std::filesystem::path temp = path.string() + ".partial";
#ifdef _WIN32
  HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (error) *error = "cannot write " + temp.string();
    return false;
  }
  if (!bytes.empty()) {
    FILE_END_OF_FILE_INFO eof_info{};
    eof_info.EndOfFile.QuadPart = static_cast<LONGLONG>(bytes.size());
    SetFileInformationByHandle(h, FileEndOfFileInfo, &eof_info, sizeof(eof_info));
  }
  size_t off = 0;
  constexpr DWORD kChunk = 16u * 1024u * 1024u;
  bool write_ok = true;
  while (off < bytes.size()) {
    const DWORD to_write = DWORD(std::min<size_t>(bytes.size() - off, kChunk));
    DWORD written = 0;
    if (!WriteFile(h, bytes.data() + off, to_write, &written, nullptr) || written != to_write) {
      write_ok = false;
      break;
    }
    off += written;
  }
  CloseHandle(h);
  if (!write_ok) {
    DeleteFileW(temp.c_str());
    if (error) *error = "cannot write " + temp.string();
    return false;
  }
  if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(temp.c_str());
    if (error) *error = "cannot rename to " + path.string();
    return false;
  }
  return true;
#else
  {
    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()))) {
      if (error) *error = "cannot write " + temp.string();
      return false;
    }
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    if (error) *error = "cannot rename to " + path.string() + ": " + ec.message();
    return false;
  }
  return true;
#endif
}

bool read_file(const std::filesystem::path& path, Bake& out, std::string* error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    if (error) *error = "cannot open " + path.string();
    return false;
  }
  const std::streamoff len = f.tellg();
  if (len < 0) {
    if (error) *error = "cannot size " + path.string();
    return false;
  }
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> bytes(static_cast<size_t>(len));
  if (!bytes.empty() && !f.read(reinterpret_cast<char*>(bytes.data()), len)) {
    if (error) *error = "cannot read " + path.string();
    return false;
  }
  if (!decode(bytes, out, error)) {
    if (error) *error = path.string() + ": " + *error;
    return false;
  }
  return true;
}

}  // namespace cars::bake
