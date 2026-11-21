#include "fh1data/ui_scene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace fh1 {
namespace {

// A big-endian reader that fails (and stays failed) past the end.
struct Reader {
  std::span<const uint8_t> d;
  size_t p = 0;
  bool bad = false;
  bool need(size_t n) {
    if (bad || p + n > d.size()) return bad = true, false;
    return true;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    const uint32_t v = uint32_t(d[p]) << 24 | uint32_t(d[p + 1]) << 16 | uint32_t(d[p + 2]) << 8 | d[p + 3];
    p += 4;
    return v;
  }
  uint8_t u8() { return need(1) ? d[p++] : 0; }
  float f32() {
    const uint32_t v = u32();
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  std::string str() {
    const uint32_t n = u32();
    if (!need(n)) return {};
    std::string s(reinterpret_cast<const char*>(d.data() + p), n);
    p += n;
    return s;
  }
  void skip(size_t n) {
    if (need(n)) p += n;
  }
};

}  // namespace

int UiScene::find_object(uint32_t id) const {
  for (size_t i = 0; i < objects.size(); ++i)
    if (objects[i].id == id) return int(i);
  return -1;
}

bool parse_ui_fbf(std::span<const uint8_t> data, UiScene& out, std::string* error) {
  Reader r{data};
  out.version = r.u32();
  const uint32_t n = r.u32();
  if (r.bad || out.version < 1006 || out.version > 1008 || n > 1000000) {
    if (error) *error = "not an FBF (version " + std::to_string(out.version) + ")";
    return false;
  }
  out.objects.clear();
  out.objects.reserve(n);
  for (uint32_t i = 0; i < n && !r.bad; ++i) {
    UiObject o;
    o.kind = UiObjectKind(r.u32());
    o.id = r.u32();
    o.name = r.str();
    switch (o.kind) {
      case UiObjectKind::kGroup: break;
      case UiObjectKind::kText: {
        r.u32();
        r.str(), r.str();
        r.f32(), r.f32();
        o.font = r.str();
        uint32_t w[8];
        for (auto& v : w) v = r.u32();
        o.text_size = w[0], o.text_align = w[1];
        r.u8();
        if (out.version == 1008) o.text_width = r.f32();
        if (out.version >= 1007 && r.u8()) r.skip(59);
        break;
      }
      case UiObjectKind::kCamera: r.skip(64); break;
      case UiObjectKind::kLayer: r.skip(24); break;
      case UiObjectKind::kModel: o.geometry = r.u32(), r.f32(); break;
      case UiObjectKind::kMaterial:
        for (auto& c : o.colour) c = r.f32();
        r.skip(104 - 16);
        break;
      case UiObjectKind::kLight: r.skip(104); break;
      case UiObjectKind::kTexture:
        o.owner = r.u32();
        o.path = r.str();
        for (auto& m : o.matrix) m = r.f32();
        r.skip(52);
        break;
      default:
        if (error) *error = "object kind " + std::to_string(uint32_t(o.kind)) + " at " + std::to_string(r.p);
        return false;
    }
    out.objects.push_back(std::move(o));
  }
  if (r.bad) {
    if (error) *error = "FBF ends inside its objects";
    return false;
  }
  return true;
}

bool parse_ui_bsg(std::span<const uint8_t> data, UiScene& out, std::string* error) {
  Reader r{data};
  static const uint8_t kTag[8] = {'F', 'G', 'B', 'k', 'r', 'a', 'n', 'A'};
  if (data.size() < 48 || std::memcmp(data.data() + 8, kTag, 8) != 0) {
    if (error) *error = "not a BSG";
    return false;
  }
  r.p = 36;
  const uint32_t nodes = r.u32(), pool = r.u32(), node_bytes = r.u32();
  if (node_bytes != 96ull * nodes || 48ull + node_bytes > data.size()) {
    if (error) *error = "BSG node table does not fit";
    return false;
  }
  r.p = 48;
  out.nodes.assign(nodes, {});
  for (UiNode& n : out.nodes) {
    const uint32_t parent = r.u32();
    n.parent = parent == 0xFFFFFFFFu ? -1 : int(parent);
    n.object = r.u32();
    r.u32();
    for (auto& v : n.position) v = r.f32();
    for (auto& v : n.rotation) v = r.f32();
    for (auto& v : n.scale) v = r.f32();
    r.skip(12);
    for (auto& v : n.bounds_min) v = r.f32();
    for (auto& v : n.bounds_max) v = r.f32();
    n.opacity = r.f32();
    r.u32();
    n.drawable = r.u32() != 0;
  }
  const uint32_t pool_bytes = r.u32();
  if (r.bad || pool_bytes != 5ull * pool) {
    if (error) *error = "BSG pool does not fit";
    return false;
  }
  out.pool.clear();
  for (uint32_t i = 0; i < pool && !r.bad; ++i) out.pool.push_back(r.u32()), r.u8();
  if (r.bad) {
    if (error) *error = "BSG ends inside its pool";
    return false;
  }
  // Link and resolve: position rotated by the parent's angle and scaled by
  // its scale (no shear), angles add, scales multiply.
  std::unordered_map<uint32_t, int> index;
  for (size_t i = 0; i < out.objects.size(); ++i) index.emplace(out.objects[i].id, int(i));
  for (size_t i = 0; i < out.nodes.size(); ++i) {
    UiNode& n = out.nodes[i];
    if (auto it = index.find(n.object); it != index.end()) n.object_index = it->second;
    const float lx = n.position[0], ly = n.position[1];
    if (n.parent >= 0 && size_t(n.parent) < i) {
      const UiNode& p = out.nodes[size_t(n.parent)];
      const float c = std::cos(p.angle), s = std::sin(p.angle);
      const float ux = lx * p.sx, uy = ly * p.sy;
      n.x = p.x + c * ux - s * uy;
      n.y = p.y + s * ux + c * uy;
      n.angle = p.angle + n.rotation[2];
      n.sx = p.sx * n.scale[0], n.sy = p.sy * n.scale[1];
      n.alpha = p.alpha * n.opacity;
    } else {
      n.x = lx, n.y = ly, n.angle = n.rotation[2];
      n.sx = n.scale[0], n.sy = n.scale[1];
      n.alpha = n.opacity;
    }
  }
  return true;
}

std::string ui_texture_member(const std::string& path, bool& horizon) {
  std::string p;
  for (char c : path) p += c == '/' ? '\\' : char(std::toupper(static_cast<unsigned char>(c)));
  for (const char* prefix : {"GAME:\\MEDIA\\UI\\TEXTURES\\", "MEDIA\\UI\\TEXTURES\\"})
    if (p.rfind(prefix, 0) == 0) {
      p = p.substr(std::strlen(prefix));
      break;
    }
  horizon = p.rfind("HORIZON\\", 0) == 0;
  if (horizon) p = p.substr(8);
  if (p.size() > 4 && p.compare(p.size() - 4, 4, ".TGA") == 0) p.resize(p.size() - 4);
  return p + ".xds";
}

}  // namespace fh1
