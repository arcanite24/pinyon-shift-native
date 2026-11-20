#include "fh1data/rmb.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "fh1data/bytes.h"

namespace fh1 {
namespace {

constexpr uint32_t kModelKind = 6;
constexpr size_t kMaximumName = 4096;
constexpr uint32_t kMaximumObjects = 4096;
constexpr uint32_t kMaximumSubmeshes = 65536;

std::string read_name(BeReader& r, const char* what) {
  const uint32_t size = r.u32();
  if (size > kMaximumName) throw FormatError(std::string(what) + " of " + std::to_string(size) + " bytes");
  const auto bytes = r.raw(size);
  std::string name(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  while (!name.empty() && name.back() == '\0') name.pop_back();
  return name;
}

void expect(BeReader& r, uint32_t value, const char* what) {
  const size_t at = r.at();
  const uint32_t got = r.u32();
  if (got != value) {
    throw FormatError(std::string(what) + " is " + std::to_string(got) + ", expected " +
                      std::to_string(value) + " (at " + std::to_string(at) + ")");
  }
}

std::array<float, 3> vec3_pad(BeReader& r) {
  std::array<float, 3> v{r.f32(), r.f32(), r.f32()};
  r.skip(4);
  return v;
}

void parse_submesh(BeReader& r, const RmbObject& object, RmbSubmesh& s) {
  expect(r, 1, "submesh tag");
  expect(r, 2, "submesh tag word 2");
  s.material = read_name(r, "material name");
  expect(r, 0, "submesh header word 0");
  s.primitive = r.u32();
  if (s.primitive != uint32_t(RmbPrimitive::kTriangleList) &&
      s.primitive != uint32_t(RmbPrimitive::kTriangleStrip))
    throw FormatError("primitive type " + std::to_string(s.primitive) + " in " + s.material);
  s.material_index = r.u32();
  expect(r, 1, "submesh header word 3");
  r.skip(16);  // reserved, zero
  for (float& c : s.colour) c = r.f32();
  for (float& c : s.sphere) c = r.f32();
  r.skip(4);  // the fourth word, 4 in every member; meaning unknown
  r.skip(4);  // zero
  const uint32_t count = r.u32();
  s.index_size = r.u32();
  if (s.index_size != 2 && s.index_size != 4)
    throw FormatError("index size " + std::to_string(s.index_size) + " in " + s.material);
  if (count > r.remaining() / s.index_size)
    throw FormatError(std::to_string(count) + " indices run past the end in " + s.material);
  const auto bytes = r.raw(size_t(count) * s.index_size);
  s.indices.resize(count);
  const uint32_t restart = s.index_size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
  const bool strip = s.primitive == uint32_t(RmbPrimitive::kTriangleStrip);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t v =
        s.index_size == 2 ? load_be16(&bytes[size_t(i) * 2]) : load_be32(&bytes[size_t(i) * 4]);
    if (v >= object.vertex_count && !(strip && v == restart)) {
      throw FormatError("index " + std::to_string(v) + " of " + std::to_string(object.vertex_count) +
                        " vertices in " + s.material);
    }
    s.indices[i] = v;
  }
  s.trailer = r.u32();
}

void parse_object(BeReader& r, RmbObject& o) {
  expect(r, 1, "object tag");
  o.centre = vec3_pad(r);
  vec3_pad(r);  // minus the half extent
  o.half_extent = vec3_pad(r);
  expect(r, 1, "object count word");
  o.name = read_name(r, "object name");
  o.vertex_format = r.u32();
  o.vertex_count = r.u32();
  o.stride = r.u32();
  if (o.stride < 12 || o.stride > 256)
    throw FormatError("vertex stride " + std::to_string(o.stride) + " in " + o.name);
  expect(r, 0, "word after the stride");
  if (o.vertex_count > r.remaining() / o.stride)
    throw FormatError(std::to_string(o.vertex_count) + " vertices run past the end in " + o.name);
  const auto bytes = r.raw(size_t(o.vertex_count) * o.stride);
  o.vertices.assign(bytes.begin(), bytes.end());
  expect(r, 1, "word after the vertices");
  const uint32_t submeshes = r.u32();
  if (submeshes > kMaximumSubmeshes)
    throw FormatError(std::to_string(submeshes) + " submeshes in " + o.name);
  o.submeshes.resize(submeshes);
  for (auto& s : o.submeshes) parse_submesh(r, o, s);
}

}  // namespace

std::array<float, 3> RmbObject::position(uint32_t i) const {
  const uint8_t* p = &vertices[size_t(i) * stride];
  return {load_bef32(p), load_bef32(p + 4), load_bef32(p + 8)};
}

bool parse_render_model(std::span<const uint8_t> data, RenderModel& out, std::string* error) {
  out = RenderModel{};
  try {
    BeReader r(data);
    out.kind = r.u32();
    if (out.kind != kModelKind) throw FormatError("model kind " + std::to_string(out.kind));
    out.box_min = vec3_pad(r);
    out.box_max = vec3_pad(r);
    for (float& m : out.matrix) m = r.f32();
    for (uint32_t& w : out.header_words) w = r.u32();
    const uint32_t objects = r.u32();
    if (objects > kMaximumObjects) throw FormatError(std::to_string(objects) + " mesh objects");
    out.objects.resize(objects);
    for (auto& o : out.objects) parse_object(r, o);
    out.geometry_end = r.at();
  } catch (const FormatError& e) {
    if (error) *error = e.what();
    out = RenderModel{};
    return false;
  }
  return true;
}

bool rmb_is_world_space(const RenderModel& model) {
  for (int axis : {0, 2}) {
    if (std::fabs(model.box_min[axis]) > kRmbLocalRadius ||
        std::fabs(model.box_max[axis]) > kRmbLocalRadius)
      return true;
  }
  return false;
}

std::vector<uint32_t> rmb_triangle_list(const RmbSubmesh& s) {
  std::vector<uint32_t> out;
  const auto& idx = s.indices;
  if (s.primitive != uint32_t(RmbPrimitive::kTriangleStrip)) {
    out.assign(idx.begin(), idx.end() - (idx.size() % 3));
    return out;
  }
  const uint32_t restart = s.index_size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
  out.reserve(idx.size() * 3);
  size_t run = 0;  // first index of the current strip
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

namespace {

std::vector<std::array<float, 4>> read_constants(BeReader& r, const char* what) {
  expect(r, 1, what);
  const uint32_t n = r.u32();
  if (n > 256) throw FormatError(std::string(what) + ": " + std::to_string(n) + " constants");
  std::vector<std::array<float, 4>> out(n);
  for (auto& c : out)
    for (float& v : c) v = r.f32();
  return out;
}

std::string effect_name(const std::string& path) {
  size_t start = path.find_last_of("\\/");
  start = start == std::string::npos ? 0 : start + 1;
  std::string name = path.substr(start);
  for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
  if (name.size() > 3 && name.ends_with(".FX")) name.resize(name.size() - 3);
  return name;
}

}  // namespace

bool parse_render_materials(std::span<const uint8_t> data, RenderModel& m, std::string* error) {
  m.materials.clear();
  m.effects.clear();
  m.uv_offset_scale.clear();
  try {
    BeReader r(data, m.geometry_end);
    for (int i = 0; i < 4; ++i) expect(r, 1, "material section tag");
    const uint32_t count = r.u32();
    if (count > kMaximumSubmeshes) throw FormatError(std::to_string(count) + " materials");
    m.materials.resize(count);
    for (auto& mat : m.materials) {
      expect(r, 3, "material tag");
      mat.effect = r.u32();
      expect(r, 0, "material word 2");
      mat.vertex_constants = read_constants(r, "vertex constant tag");
      mat.pixel_constants = read_constants(r, "pixel constant tag");
      expect(r, 1, "texture slot tag");
      const uint32_t slots = r.u32();
      if (slots > 32) throw FormatError(std::to_string(slots) + " texture slots");
      mat.slots.resize(slots);
      for (auto& v : mat.slots) v = int32_t(r.u32());
    }
    expect(r, 1, "effect list tag");
    const uint32_t effects = r.u32();
    if (effects > 4096) throw FormatError(std::to_string(effects) + " effects");
    for (uint32_t i = 0; i < effects; ++i) m.effects.push_back(effect_name(read_name(r, "effect path")));
    for (const auto& mat : m.materials)
      if (mat.effect >= m.effects.size())
        throw FormatError("effect " + std::to_string(mat.effect) + " of " + std::to_string(m.effects.size()));
    // The command section: the uvOffsetScale writes.
    size_t submeshes = 0;
    for (const auto& o : m.objects) submeshes += o.submeshes.size();
    const size_t start = r.at();
    for (size_t at = start; at + 36 <= data.size() && m.uv_offset_scale.size() < submeshes; ++at) {
      const uint32_t header = load_be32(&data[at]);
      if (header != 0x000F4000u && header != 0x001F4000u) continue;
      bool ones = true;
      for (int k = 0; k < 4; ++k) ones = ones && load_bef32(&data[at + 20 + 4 * size_t(k)]) == 1.0f;
      if (!ones) continue;
      m.uv_offset_scale.push_back({load_bef32(&data[at + 4]), load_bef32(&data[at + 8]), load_bef32(&data[at + 12]),
                                   load_bef32(&data[at + 16])});
      at += 35;
    }
    if (m.uv_offset_scale.size() < submeshes) m.uv_offset_scale.clear();
  } catch (const FormatError& e) {
    if (error) *error = e.what();
    m.materials.clear();
    m.effects.clear();
    m.uv_offset_scale.clear();
    return false;
  }
  return true;
}

RmbVertexLayout rmb_vertex_layout(const std::string& e, uint32_t stride) {
  RmbVertexLayout l;
  l.known = true;
  auto starts = [&](const char* prefix) { return e.rfind(prefix, 0) == 0; };
  if (stride == 24 && e == "ADD_DIFF_OPAC_RGBA") {
    l.colour = 12, l.texcoord = {16, 20, -1};
  } else if (stride == 24 && starts("BUNTING_")) {
    l.normal = 12, l.texcoord = {16, -1, -1}, l.colour = 20;
  } else if (stride == 24 && e == "TREE_DIFF_OPAC_DIRLIGHT_2_2SD") {
    l.texcoord = {12, -1, -1}, l.normal = 16, l.colour = 20;
  } else if (stride == 32 && starts("ANIM_FLAG_")) {
    l.texcoord = {12, 24, 28}, l.colour = 16, l.tangent = 20;
  } else if (stride == 32 && starts("LAKE_")) {
    l.normal = 12, l.texcoord = {24, -1, 16}, l.colour = 20, l.tangent = 28;
  } else if (stride == 28 && e == "DIFF_GLOW_1") {
    l.normal = 12, l.texcoord = {16, -1, 20}, l.colour = 24;
  } else {
    switch (stride) {
      case 16: l.texcoord = {12, -1, -1}; break;
      case 20: l.normal = 12, l.texcoord = {16, -1, -1}; break;
      case 24: l.normal = 12, l.texcoord = {16, 20, -1}; break;
      case 28: l.normal = 12, l.texcoord = {16, 20, -1}, l.colour = 24; break;
      case 32: l.normal = 12, l.texcoord = {16, 20, 24}, l.colour = 28; break;
      case 36: l.normal = 12, l.texcoord = {16, 20, 24}, l.tangent = 28, l.colour = 32; break;
      default: l.known = false;
    }
  }
  return l;
}

std::array<float, 3> rmb_normal(const uint8_t* p) {
  const uint32_t w = load_be32(p);
  auto component = [&](int shift) {
    int32_t v = int32_t((w >> shift) & 0x3FF);
    if (v & 0x200) v -= 0x400;
    return std::max(float(v) / 511.0f, -1.0f);
  };
  return {component(0), component(10), component(20)};
}

std::array<float, 2> rmb_texcoord(const uint8_t* p) {
  return {float(load_be16(p)) / 65535.0f, float(load_be16(p + 2)) / 65535.0f};
}

std::array<uint8_t, 4> rmb_colour(const uint8_t* p) { return {p[1], p[2], p[3], p[0]}; }

}  // namespace fh1
