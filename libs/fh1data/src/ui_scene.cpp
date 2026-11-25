#include "fh1data/ui_scene.h"

#include "fh1data/archive.h"

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

const UiGeometry* UiScene::geometry_of(int node_index) const {
  if (node_index < 0 || size_t(node_index) >= nodes.size()) return nullptr;
  const int oi = nodes[size_t(node_index)].object_index;
  if (oi < 0 || size_t(oi) >= objects.size()) return nullptr;
  const UiObject& o = objects[size_t(oi)];
  if (o.kind != UiObjectKind::kModel || o.geometry >= geometries.size()) return nullptr;
  return &geometries[o.geometry];
}

bool UiBgfDocument::has_contract(std::string_view type) const {
  for (const auto& c : contracts)
    if (c.type == type) return true;
  return false;
}

std::vector<const UiContract*> UiBgfDocument::find_contracts(std::string_view type) const {
  std::vector<const UiContract*> out;
  for (const auto& c : contracts)
    if (c.type == type) out.push_back(&c);
  return out;
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
        o.text_valign = w[2], o.text_mode = w[3];
        std::memcpy(&o.text_spacing[0], &w[4], 4);
        std::memcpy(&o.text_spacing[1], &w[5], 4);
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
        // 52-byte sampler tail: 7 floats + flag + addressing mode + 4 small enums.
        o.tex_rotation = r.f32();
        o.tex_u_offset = r.f32();
        o.tex_v_offset = r.f32();
        o.tex_u_scale = r.f32();
        o.tex_v_scale = r.f32();
        o.tex_pivot_u = r.f32();
        o.tex_pivot_v = r.f32();
        o.tex_flag = r.u32();
        o.tex_address_mode = r.u32();
        for (auto& e : o.tex_sampler_enums) e = r.u32();
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
  out.geometries.clear();
  if (r.p + 12 <= data.size()) {
    const uint32_t gcount = r.u32();
    const uint32_t extra = r.u32();
    const uint32_t gpu = r.u32();
    const size_t gbase = r.p;
    const size_t extra_base = gbase + size_t(gcount) * 96;
    const size_t gpu_base = extra_base + size_t(extra);
    if (!r.bad && gcount <= 200000 && gpu_base <= data.size()) {
      out.geometries.reserve(gcount);
      for (uint32_t gi = 0; gi < gcount; ++gi) {
        Reader gr{data.subspan(gbase + size_t(gi) * 96, 96)};
        UiGeometry geo;
        geo.index = gi;
        gr.skip(16);  // 4 header words
        geo.bounds_min[0] = gr.f32();
        geo.bounds_min[1] = gr.f32();
        geo.bounds_min[2] = gr.f32();
        gr.f32();
        geo.bounds_max[0] = gr.f32();
        geo.bounds_max[1] = gr.f32();
        geo.bounds_max[2] = gr.f32();
        gr.f32();
        gr.skip(24);  // words 12..17
        const uint32_t fetch0 = gr.u32();
        const uint32_t fetch1 = gr.u32();
        geo.gpu_offset = fetch0 & ~3u;
        geo.gpu_fetch_word = fetch1;
        geo.extra_offset = gr.u32();
        auto close = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
        geo.is_unit_quad = close(geo.bounds_min[0], -50.0f) && close(geo.bounds_min[1], -50.0f) &&
                           close(geo.bounds_max[0], 50.0f) && close(geo.bounds_max[1], 50.0f);
        if (!geo.is_unit_quad && gpu > 0 && gpu_base + geo.gpu_offset + 24 <= data.size()) {
          // Extract bounded 2D mesh vertices from the GPU vertex buffer (stride 36 bytes or 24 bytes: XYZ floats first).
          const uint32_t avail = uint32_t(std::min<size_t>(size_t(gpu) - geo.gpu_offset, 2048));
          const uint32_t stride = 36;
          const uint32_t vmax = std::min<uint32_t>(avail / stride, 48);
          for (uint32_t vi = 0; vi < vmax; ++vi) {
            Reader vr{data.subspan(gpu_base + geo.gpu_offset + size_t(vi) * stride, 12)};
            const float vx = vr.f32(), vy = vr.f32();
            if (!std::isfinite(vx) || !std::isfinite(vy)) break;
            if (vx < geo.bounds_min[0] - 2.0f || vx > geo.bounds_max[0] + 2.0f ||
                vy < geo.bounds_min[1] - 2.0f || vy > geo.bounds_max[1] + 2.0f)
              break;
            geo.vertices.push_back({vx, vy});
          }
        }
        out.geometries.push_back(std::move(geo));
      }
    }
  }
  return true;
}

// A model laid flat in its own XZ plane and stood up on the canvas by its
// node's rotation about x or y (947_HUD's red zone, HalfRectangle: bounds
// x -50..50, y 0, z -50..0, turned -90 degrees about x) has no height in its
// stored XY bounds. Its canvas bounds are its scaled box turned by those
// two rotations, stored back unscaled so the node's x and y scales apply.
// Nodes with a real XY extent are left as stored.
static void stand_up_flat_bounds(UiNode& n) {
  const float rx = n.rotation[0], ry = n.rotation[1];
  if (std::fabs(rx) < 1e-3f && std::fabs(ry) < 1e-3f) return;
  const float ex = n.bounds_max[0] - n.bounds_min[0], ey = n.bounds_max[1] - n.bounds_min[1];
  const float ez = n.bounds_max[2] - n.bounds_min[2];
  if (!(ez > 1e-3f && (ex <= 1e-3f || ey <= 1e-3f))) return;
  const float s0 = std::fabs(n.scale[0]) > 1e-6f ? n.scale[0] : 1.0f;
  const float s1 = std::fabs(n.scale[1]) > 1e-6f ? n.scale[1] : 1.0f;
  const float s2 = std::fabs(n.scale[2]) > 1e-6f ? n.scale[2] : 1.0f;
  const float cx = std::cos(rx), sx = std::sin(rx), cy = std::cos(ry), sy = std::sin(ry);
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (int c = 0; c < 8; ++c) {
    const float x = ((c & 1) ? n.bounds_max[0] : n.bounds_min[0]) * s0;
    const float y = ((c & 2) ? n.bounds_max[1] : n.bounds_min[1]) * s1;
    const float z = ((c & 4) ? n.bounds_max[2] : n.bounds_min[2]) * s2;
    // About x, then about y.
    const float y1 = y * cx - z * sx, z1 = y * sx + z * cx;
    const float x2 = x * cy + z1 * sy, z2 = -x * sy + z1 * cy;
    const float p[3] = {x2, y1, z2};
    for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
  }
  const float sc[3] = {s0, s1, s2};
  for (int k = 0; k < 3; ++k) {
    const float a = lo[k] / sc[k], b = hi[k] / sc[k];
    n.bounds_min[k] = std::min(a, b), n.bounds_max[k] = std::max(a, b);
  }
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
    stand_up_flat_bounds(n);
    if (auto it = index.find(n.object); it != index.end()) n.object_index = it->second;
    const float lx = n.position[0], ly = n.position[1], lz = n.position[2];
    if (n.parent >= 0 && size_t(n.parent) < i) {
      const UiNode& p = out.nodes[size_t(n.parent)];
      const float c = std::cos(p.angle), s = std::sin(p.angle);
      const float ux = lx * p.sx, uy = ly * p.sy;
      n.x = p.x + c * ux - s * uy;
      n.y = p.y + s * ux + c * uy;
      n.z = p.z + lz * p.sz;
      n.angle = p.angle + n.rotation[2];
      n.sx = p.sx * n.scale[0], n.sy = p.sy * n.scale[1], n.sz = p.sz * n.scale[2];
      n.alpha = p.alpha * n.opacity;
    } else {
      n.x = lx, n.y = ly, n.z = lz, n.angle = n.rotation[2];
      n.sx = n.scale[0], n.sy = n.scale[1], n.sz = n.scale[2];
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

bool load_ui_scene(Archive& ui_zip, const std::string& name, UiScene& out, std::string* error) {
  const std::string base = "Scenes/ui4/" + name;
  auto fbf = ui_zip.read(base + ".fbf");
  auto bsg = ui_zip.read(base + ".bsg");
  if (!fbf.ok() || !bsg.ok()) {
    if (error) *error = "missing " + base + (fbf.ok() ? ".bsg" : ".fbf");
    return false;
  }
  if (!parse_ui_fbf(fbf.data, out, error) || !parse_ui_bsg(bsg.data, out, error)) return false;
  auto bgf = ui_zip.read(base + ".bgf");
  out.has_bgf = bgf.ok() && parse_ui_bgf(bgf.data, out.bgf, nullptr);
  return true;
}

namespace {
struct BgfCounts {
  uint32_t curves = 0, action_owners = 0, slides = 0;
};
bool parse_bgf_timeline(std::span<const uint8_t> data, size_t cursor, const BgfCounts& n, UiBgfDocument& out);
}  // namespace

bool parse_ui_bgf(std::span<const uint8_t> data, UiBgfDocument& out, std::string* error) {
  out = UiBgfDocument{};
  static constexpr char kMarker[8] = {'A', 'n', 'a', 'r', 'k', 'B', 'G', 'F'};
  if (data.size() < 0x8C || std::memcmp(data.data() + 6, kMarker, 8) != 0) {
    if (error) *error = "not an AnarkBGF document";
    return false;
  }
  auto u32_at = [&](size_t off) -> uint32_t {
    if (off + 4 > data.size()) return 0;
    return uint32_t(data[off]) << 24 | uint32_t(data[off + 1]) << 16 | uint32_t(data[off + 2]) << 8 | data[off + 3];
  };
  auto u16_at = [&](size_t off) -> uint16_t {
    if (off + 2 > data.size()) return 0;
    return uint16_t(uint16_t(data[off]) << 8 | data[off + 1]);
  };
  auto f32_at = [&](size_t off) -> float {
    const uint32_t v = u32_at(off);
    float f = 0.0f;
    std::memcpy(&f, &v, 4);
    return f;
  };

  out.element_count = u32_at(0x24);
  out.wrapper_count = u32_at(0x28);
  out.property_count = u32_at(0x2C);
  const uint32_t relation_count = u32_at(0x30);
  const uint32_t identity_count = u32_at(0x3C);
  BgfCounts header_counts;
  header_counts.curves = u32_at(0x44);
  header_counts.action_owners = u32_at(0x4C);
  header_counts.slides = u32_at(0x5C);

  // 1. Identity string pool at 0x88.
  const uint32_t id_declared = u32_at(0x88);
  const size_t id_end = size_t(0x8C) + size_t(id_declared);
  if (id_end > data.size()) {
    if (error) *error = "BGF identity section exceeds file";
    return false;
  }
  size_t cursor = 0x8C;
  out.identities.reserve(identity_count);
  for (uint32_t i = 0; i < identity_count; ++i) {
    if (cursor + 4 > id_end) {
      if (error) *error = "BGF identity string truncated";
      return false;
    }
    const uint32_t len = u32_at(cursor);
    cursor += 4;
    if (cursor + len > id_end) {
      if (error) *error = "BGF identity string body exceeds section";
      return false;
    }
    // Strip trailing NULs if present.
    size_t trim = len;
    while (trim > 0 && data[cursor + trim - 1] == 0) --trim;
    out.identities.emplace_back(reinterpret_cast<const char*>(data.data() + cursor), trim);
    cursor += len;
  }
  cursor = id_end;

  // 2. Item section.
  if (cursor + 4 > data.size()) {
    if (error) *error = "BGF missing item section";
    return false;
  }
  const uint32_t item_declared = u32_at(cursor);
  cursor += 4;
  const size_t item_end = cursor + size_t(item_declared);
  if (item_end > data.size()) {
    if (error) *error = "BGF item section exceeds file";
    return false;
  }
  const uint32_t total_items = out.element_count + out.wrapper_count;
  out.items.reserve(total_items);
  for (uint32_t i = 0; i < total_items; ++i) {
    if (cursor + 14 > item_end) {
      if (error) *error = "BGF item header truncated";
      return false;
    }
    UiBgfItem item;
    item.name_hash = u32_at(cursor);
    item.parent_hash = u32_at(cursor + 4);
    item.value = u32_at(cursor + 8);
    item.kind = data[cursor + 12];
    item.flags = data[cursor + 13];
    item.is_wrapper = (item.flags & 0x04) != 0;
    cursor += 14;
    if (item.is_wrapper) cursor += 5;
    if (cursor + 4 > item_end) {
      if (error) *error = "BGF item property count truncated";
      return false;
    }
    const uint32_t pcount = u32_at(cursor);
    cursor += 4;
    if (pcount > 1024 || cursor + size_t(pcount) * 8 > item_end) {
      if (error) *error = "BGF item properties exceed section";
      return false;
    }
    item.properties.reserve(pcount);
    for (uint32_t p = 0; p < pcount; ++p) {
      item.properties.emplace_back(u32_at(cursor), u32_at(cursor + 4));
      cursor += 8;
    }
    // Extract contracts from kind == 8 items using the real BGF property attribute hashes:
    //   0xACA9EC63 -> contract type (e.g. "SUPER_STACKER", "PAUSE_MENU_BUTTON")
    //   0x083BF5F8 -> initial state (e.g. "SHOWN", "HIDDEN")
    //   0xA73F25FB / 0xADE4DC33 -> data/node bindings
    //   0xAE4CB58A -> wrapper tag ("contract" / "UIContract")
    if (item.kind == 8) {
      constexpr uint32_t kAttrContractType = 0xACA9EC63u;
      constexpr uint32_t kAttrInitialState = 0x083BF5F8u;
      constexpr uint32_t kAttrBindingA = 0xA73F25FBu;
      constexpr uint32_t kAttrBindingB = 0xADE4DC33u;
      constexpr uint32_t kAttrContractTag = 0xAE4CB58Au;
      UiContract c;
      c.item_index = int(i);
      c.owner_hash = item.parent_hash;
      auto push_unique = [](std::vector<std::string>& dst, const std::string& val) {
        if (!val.empty() && std::find(dst.begin(), dst.end(), val) == dst.end()) dst.push_back(val);
      };
      for (const auto& [attr, val] : item.properties) {
        if (val >= out.identities.size()) continue;
        const std::string& s = out.identities[val];
        if (s.empty()) continue;
        if (attr == kAttrContractType) {
          c.type = s;
        } else if (attr == kAttrInitialState) {
          c.initial_state = s;
        } else if (attr == kAttrBindingA || attr == kAttrBindingB) {
          if (s != "UIContract" && s != "contract") push_unique(c.bindings, s);
        }
      }
      for (const auto& [attr, val] : item.properties) {
        if (attr == kAttrContractType || attr == kAttrInitialState || attr == kAttrBindingA ||
            attr == kAttrBindingB || attr == kAttrContractTag)
          continue;
        if (val >= out.identities.size()) continue;
        const std::string& s = out.identities[val];
        if (s.empty() || s == "UIContract" || s == "contract") continue;
        if (s.find('.') != std::string::npos) {
          push_unique(c.bindings, s);
        } else if (c.type.empty() && s != "SHOWN" && s != "HIDDEN") {
          c.type = s;
        } else {
          push_unique(c.events, s);
        }
      }
      if (!c.type.empty()) out.contracts.push_back(std::move(c));
    }
    out.items.push_back(std::move(item));
  }
  cursor = item_end;

  // 3. Relations section.
  if (cursor + 4 <= data.size()) {
    const uint32_t rel_declared = u32_at(cursor);
    cursor += 4;
    const size_t rel_end = cursor + size_t(rel_declared);
    if (rel_end <= data.size()) {
      out.relations.reserve(relation_count);
      for (uint32_t i = 0; i < relation_count && cursor + 8 <= rel_end; ++i) {
        UiBgfRelation rel;
        rel.parent = u32_at(cursor);
        const uint32_t cc = u32_at(cursor + 4);
        cursor += 8;
        if (cursor + size_t(cc) * 4 > rel_end) break;
        rel.children.reserve(cc);
        for (uint32_t k = 0; k < cc; ++k) {
          rel.children.push_back(u32_at(cursor));
          cursor += 4;
        }
        out.relations.push_back(std::move(rel));
      }
      cursor = rel_end;
    }
  }

  // 4..7. The timeline: curves, actions, an empty table, slides. Each is a
  // length-prefixed section whose records must consume the declaration
  // exactly (the title checks the same sums: 82F26AC0, 82F26D90, 82F284A8);
  // on any mismatch the timeline is dropped and the layout kept.
  out.has_timeline = parse_bgf_timeline(data, cursor, header_counts, out);
  if (!out.has_timeline) {
    out.curves.clear();
    out.actions.clear();
    out.slides.clear();
  }
  return true;
}

namespace {
bool parse_bgf_timeline(std::span<const uint8_t> data, size_t cursor, const BgfCounts& n, UiBgfDocument& out) {
  Reader r{data, cursor};
  auto section = [&](size_t& end) {
    const uint32_t declared = r.u32();
    end = r.p + declared;
    return !r.bad && end <= data.size();
  };
  auto ftime = [](uint32_t v) { return float(v); };  // times are stored as integer milliseconds
  // Curves (82F26AC0): item, attribute, two flag bytes, a key count, then
  // keys of (time ms int, value, c1 time int, c1 value, c2 time int, c2 value).
  size_t end = 0;
  if (!section(end)) return false;
  out.curves.reserve(n.curves);
  for (uint32_t i = 0; i < n.curves; ++i) {
    UiBgfCurve c;
    c.item = r.u32();
    c.attr = r.u32() & 0x07FFFFFFu;
    c.flag = r.u8() == 1;
    c.dynamic = r.u8() == 1;
    const uint16_t keys = uint16_t(r.u8() << 8 | r.u8());
    if (r.bad || r.p + size_t(keys) * 24 > end) return false;
    c.keys.reserve(keys);
    for (uint16_t k = 0; k < keys; ++k) {
      UiBgfCurveKey key;
      key.time_ms = ftime(r.u32());
      key.value = r.f32();
      key.c1_time = ftime(r.u32());
      key.c1_value = r.f32();
      key.c2_time = ftime(r.u32());
      key.c2_value = r.f32();
      c.keys.push_back(key);
    }
    out.curves.push_back(std::move(c));
  }
  if (r.bad || r.p != end) return false;
  // Actions (82F26D90): per owner item, events of (name hash, count) and
  // their actions of (source, target, handler, argument tag, argument).
  if (!section(end)) return false;
  for (uint32_t i = 0; i < n.action_owners; ++i) {
    const uint32_t owner = r.u32();
    const uint32_t events = r.u32();
    for (uint32_t e = 0; e < events && !r.bad; ++e) {
      const uint32_t event = r.u32();
      const uint32_t count = r.u32();
      if (r.bad || r.p + size_t(count) * 20 > end) return false;
      for (uint32_t a = 0; a < count; ++a) {
        UiBgfAction act;
        act.owner = owner;
        act.event = event;
        act.source = r.u32();
        act.target = r.u32();
        act.handler = r.u32();
        act.arg_tag = r.u32();
        act.arg = r.u32();
        out.actions.push_back(act);
      }
    }
  }
  if (r.bad || r.p != end) return false;
  // The companion table between actions and slides is empty in every scene.
  if (!section(end) || end != r.p) return false;
  // Slides (82F284A8): a 16-byte name, the component's item, the flag byte,
  // start and end ms, then per element (item, active byte, values of
  // (tag, value), references of (kind byte: 0 curve, else action; enabled
  // byte; u16 index)).
  if (!section(end)) return false;
  out.slides.reserve(n.slides);
  for (uint32_t i = 0; i < n.slides; ++i) {
    UiBgfSlide s;
    if (!r.need(16)) return false;
    const char* name = reinterpret_cast<const char*>(data.data() + r.p);
    s.name.assign(name, strnlen(name, 16));
    r.p += 16;
    s.component = r.u32();
    s.flags = r.u8();
    s.start_ms = r.u32();
    s.end_ms = r.u32();
    const uint32_t tracks = r.u32();
    if (r.bad || tracks > 65536) return false;
    s.tracks.reserve(tracks);
    for (uint32_t t = 0; t < tracks; ++t) {
      UiBgfSlideTrack tr;
      tr.item = r.u32();
      tr.active = r.u8() != 0;
      const uint32_t values = r.u32();
      if (r.bad || r.p + size_t(values) * 8 > end) return false;
      tr.values.reserve(values);
      for (uint32_t v = 0; v < values; ++v) {
        const uint32_t tag = r.u32();
        tr.values.push_back({tag & 0x07FFFFFFu, uint8_t(tag >> 27), r.u32()});
      }
      const uint32_t refs = r.u32();
      if (r.bad || r.p + size_t(refs) * 4 > end) return false;
      tr.refs.reserve(refs);
      for (uint32_t k = 0; k < refs; ++k) {
        UiBgfSlideRef ref;
        ref.action = r.u8() != 0;
        ref.enabled = r.u8() != 0;
        ref.index = uint16_t((r.u8() << 8 | r.u8()) & 0x3FFF);
        tr.refs.push_back(ref);
      }
      s.tracks.push_back(std::move(tr));
    }
    out.slides.push_back(std::move(s));
  }
  if (r.bad || r.p != end) return false;
  for (const auto& s : out.slides)
    for (const auto& t : s.tracks)
      for (const auto& ref : t.refs)
        if (ref.index >= (ref.action ? out.actions.size() : out.curves.size())) return false;
  return true;
}
}  // namespace

float UiBgfValue::f() const {
  float v = 0;
  std::memcpy(&v, &raw, 4);
  return v;
}

float UiBgfValue::number() const { return ui_bgf_number(uint32_t(type) << 27, raw); }

float ui_bgf_number(uint32_t tag, uint32_t raw) {
  if (((tag >> 27) & 7u) == 1u) return float(int32_t(raw));
  float v = 0;
  std::memcpy(&v, &raw, 4);
  return v;
}

std::vector<int> UiBgfDocument::slides_of(uint32_t component) const {
  std::vector<int> out;
  for (size_t i = 0; i < slides.size(); ++i)
    if (slides[i].component == component) out.push_back(int(i));
  return out;
}

int UiBgfDocument::find_slide(uint32_t component, std::string_view name) const {
  for (size_t i = 0; i < slides.size(); ++i)
    if (slides[i].component == component && slides[i].name == name) return int(i);
  return -1;
}

int UiBgfDocument::item_of_object(uint32_t object) const {
  for (size_t i = 0; i < items.size(); ++i)
    if (items[i].kind >= 1 && items[i].kind <= 7 && items[i].value == object) return int(i);
  return -1;
}

float evaluate_bgf_curve(const UiBgfCurve& c, float t) {
  const auto& k = c.keys;
  if (k.empty()) return 0.0f;
  if (k.size() < 2) return k[0].value;
  // The first key later than t (82F2EF08 walks from the first).
  size_t i = 0;
  while (i < k.size() && !(k[i].time_ms > t)) ++i;
  if (i == 0) return k[0].value;
  if (i == k.size()) return k.back().value;
  const UiBgfCurveKey& a = k[i - 1];
  const UiBgfCurveKey& b = k[i];
  if (b.time_ms == a.time_ms) return b.value;
  // With control times (none in the retail scenes) the title solves the
  // time cubic for s (82F2E3B8); taken as linear here.
  const float s = (t - a.time_ms) / (b.time_ms - a.time_ms);
  const float u = 1.0f - s;
  return u * u * u * a.value + 3.0f * a.c1_value * s * u * u + 3.0f * a.c2_value * s * s * u + s * s * s * b.value;
}

void restart_bgf_curve(UiBgfCurve& c, float start) {
  if (c.keys.empty()) return;
  UiBgfCurveKey& a = c.keys[0];
  if (c.keys.size() < 2) {
    a.value = start;
    return;
  }
  const float v1 = c.keys[1].value;
  const float third = (v1 - a.value) * (1.0f / 3.0f);
  float k1 = 0.0f, k2 = 0.0f;
  if (third != 0.0f) {
    k2 = (v1 - a.c2_value) / third;
    k1 = (a.c1_value - a.value) / third;
  }
  const float nt = (v1 - start) * (1.0f / 3.0f);
  a.c1_value = nt * k1 + start;
  a.c2_value = v1 - nt * k2;
  a.value = start;
}

std::vector<float> reflow_stacker(float origin, float step, int count) {
  std::vector<float> out;
  if (count <= 0) return out;
  out.reserve(size_t(count));
  for (int i = 0; i < count; ++i) out.push_back(origin + float(i) * step);
  return out;
}

namespace {
std::string to_lower_ascii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) out.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

std::string to_upper_ascii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) out.push_back(char(std::toupper(static_cast<unsigned char>(c))));
  return out;
}

bool extract_xml_attr(std::string_view tag, std::string_view attr_name, std::string& out) {
  size_t pos = 0;
  while ((pos = tag.find(attr_name, pos)) != std::string_view::npos) {
    if (pos > 0 && std::isalnum(static_cast<unsigned char>(tag[pos - 1]))) {
      pos += attr_name.size();
      continue;
    }
    size_t i = pos + attr_name.size();
    while (i < tag.size() && (tag[i] == ' ' || tag[i] == '\t')) ++i;
    if (i >= tag.size() || tag[i] != '=') {
      pos = i;
      continue;
    }
    ++i;
    while (i < tag.size() && (tag[i] == ' ' || tag[i] == '\t')) ++i;
    if (i >= tag.size() || (tag[i] != '"' && tag[i] != '\'')) return false;
    const char q = tag[i++];
    const size_t end = tag.find(q, i);
    if (end == std::string_view::npos) return false;
    out.assign(tag.substr(i, end - i));
    return true;
  }
  return false;
}
}  // namespace

bool parse_fontmap_xml(std::string_view xml, UiFontMap& out) {
  out.mappings.clear();
  size_t pos = 0;
  while ((pos = xml.find("<mapping", pos)) != std::string_view::npos) {
    const size_t end = xml.find('>', pos);
    if (end == std::string_view::npos) break;
    const std::string_view tag = xml.substr(pos, end - pos + 1);
    std::string fontname, target;
    if (extract_xml_attr(tag, "fontname", fontname) && extract_xml_attr(tag, "target", target)) {
      out.mappings[to_lower_ascii(fontname)] = to_upper_ascii(target);
    }
    pos = end + 1;
  }
  return !out.mappings.empty();
}

std::string UiFontMap::resolve(std::string_view font_name) const {
  if (font_name.empty()) return "B";
  // Strip any weight/style suffix after the first space (e.g. "d bold" -> "d",
  // "b_swiss_721_medium medium" -> "b_swiss_721_medium"), per FORMAT-UISCENES.
  const size_t sp = font_name.find(' ');
  const std::string base = to_lower_ascii(sp == std::string_view::npos ? font_name : font_name.substr(0, sp));
  const std::string full = to_lower_ascii(font_name);
  if (auto it = mappings.find(base); it != mappings.end()) return it->second;
  if (auto it = mappings.find(full); it != mappings.end()) return it->second;
  // Direct Horizon_* / target names.
  if (base == "a" || base == "horizon_a" || base == "horizon_c") return "A";
  if (base == "b" || base == "horizon_b") return "B";
  if (base == "c") return "C";
  if (base == "d" || base == "horizon_d" || base == "dbold") return "D";
  if (base == "e" || base == "horizon_e") return "E";
  if (base == "sym" || base == "horizon_sym") return "SYM";
  if (base == "arial") return "DG1";
  for (int i = 1; i <= 5; ++i) {
    const std::string dg = "dg" + std::to_string(i);
    if (base == dg || base == "horizon_" + dg || base.rfind(dg + "_", 0) == 0) return "DG" + std::to_string(i);
  }
  // Leading letter before '_' (e.g. "b_swiss_721_medium", "a_helvetica53-extended", "d_swiss_721_roman_condensed").
  if (base.size() >= 2 && base[1] == '_') {
    switch (base[0]) {
      case 'a': return "A";
      case 'b': return "B";
      case 'c': return "C";
      case 'd': return "D";
      case 'e': return "E";
      default: break;
    }
  }
  return "B";
}

bool parse_loading_defs_xml(std::string_view xml, std::vector<UiLoadingTip>& out) {
  out.clear();
  std::string cur_group = "freeroam";
  int cur_count = 1;
  bool cur_sequential = false;
  size_t pos = 0;
  while (pos < xml.size()) {
    const size_t lt = xml.find('<', pos);
    if (lt == std::string_view::npos) break;
    if (xml.compare(lt, 4, "<!--") == 0) {
      const size_t cend = xml.find("-->", lt + 4);
      if (cend == std::string_view::npos) break;
      pos = cend + 3;
      continue;
    }
    const size_t gt = xml.find('>', lt + 1);
    if (gt == std::string_view::npos) break;
    const std::string_view tag = xml.substr(lt, gt - lt + 1);
    if (tag.rfind("<LoadingGroup", 0) == 0) {
      std::string nm;
      if (extract_xml_attr(tag, "name", nm) && !nm.empty()) cur_group = to_lower_ascii(nm);
      std::string count_s, order_s;
      cur_count = extract_xml_attr(tag, "tipcount", count_s) ? std::max(1, std::atoi(count_s.c_str())) : 1;
      cur_sequential = extract_xml_attr(tag, "order", order_s) && to_lower_ascii(order_s) == "sequential";
    } else if (tag.rfind("<Tip", 0) == 0 && (tag.size() == 4 || tag[4] == ' ' || tag[4] == '\t')) {
      UiLoadingTip tip;
      tip.group = cur_group;
      tip.group_tip_count = cur_count;
      tip.group_sequential = cur_sequential;
      extract_xml_attr(tag, "title", tip.title_key);
      extract_xml_attr(tag, "message", tip.message_key);
      std::string lay_s, dt_s;
      if (extract_xml_attr(tag, "layout", lay_s)) tip.layout = std::atoi(lay_s.c_str());
      if (extract_xml_attr(tag, "displaytime", dt_s)) tip.display_time = std::max(2.0f, float(std::atof(dt_s.c_str())));
      if (!tip.title_key.empty() || !tip.message_key.empty()) out.push_back(std::move(tip));
    }
    pos = gt + 1;
  }
  return !out.empty();
}

}  // namespace fh1
