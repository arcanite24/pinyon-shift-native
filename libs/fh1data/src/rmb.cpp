#include "fh1data/rmb.h"

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

}  // namespace fh1
