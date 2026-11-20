#include "cars/bake_format.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace cars::bake {
namespace {

constexpr char kMagic[8] = {'S', 'H', 'F', 'T', 'B', 'A', 'K', 'E'};
constexpr char kEnd[8] = {'E', 'N', 'D', 'B', 'A', 'K', 'E', '\0'};

class Writer {
 public:
  std::vector<uint8_t> out;
  void u8(uint8_t v) { out.push_back(v); }
  void u16(uint16_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
  }
  void u32(uint32_t v) {
    for (int s = 0; s < 32; s += 8) out.push_back(uint8_t(v >> s));
  }
  void i64(int64_t v) {
    for (int s = 0; s < 64; s += 8) out.push_back(uint8_t(uint64_t(v) >> s));
  }
  void f32(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    u32(b);
  }
  void str(const std::string& s) {
    u32(uint32_t(s.size()));
    out.insert(out.end(), s.begin(), s.end());
  }
  void bytes(const std::vector<uint8_t>& b) {
    u32(uint32_t(b.size()));
    out.insert(out.end(), b.begin(), b.end());
  }
  void raw(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
  }
};

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
  for (const Vertex& v : vs) {
    for (float f : v.position) w.f32(f);
    for (int16_t c : v.frame) w.u16(uint16_t(c));
    for (uint16_t c : v.uv0) w.u16(c);
    for (uint16_t c : v.uv1) w.u16(c);
    w.raw(v.lighting.data(), 4);
  }
}

std::vector<Vertex> read_vertices(Reader& r) {
  std::vector<Vertex> vs(r.count(32));
  for (Vertex& v : vs) {
    for (float& f : v.position) f = r.f32();
    for (int16_t& c : v.frame) c = int16_t(r.u16());
    for (uint16_t& c : v.uv0) c = r.u16();
    for (uint16_t& c : v.uv1) c = r.u16();
    r.raw(v.lighting.data(), 4);
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
          for (const auto& v : t)
            for (float f : v) w.f32(f);
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
        for (uint32_t i : s.indices) w.u32(i);
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
            for (auto& v : t)
              for (float& f : v) f = r.f32();
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
          for (uint32_t& i : s.indices) i = r.u32();
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
  std::filesystem::create_directories(path.parent_path(), ec);
  const auto temp = path.string() + ".partial";
  {
    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()))) {
      if (error) *error = "cannot write " + temp;
      return false;
    }
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    if (error) *error = "cannot rename to " + path.string() + ": " + ec.message();
    return false;
  }
  return true;
}

bool read_file(const std::filesystem::path& path, Bake& out, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = "cannot open " + path.string();
    return false;
  }
  std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(f), {});
  if (!decode(bytes, out, error)) {
    if (error) *error = path.string() + ": " + *error;
    return false;
  }
  return true;
}

}  // namespace cars::bake
