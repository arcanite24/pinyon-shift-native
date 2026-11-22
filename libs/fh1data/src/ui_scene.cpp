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
  const uint32_t style_count = u32_at(0x44);
  const uint32_t track_count = u32_at(0x4C);
  const uint32_t anim_count = u32_at(0x5C);

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

  // 4. Styles / States section (12 + child_count * 24 bytes per style).
  if (cursor + 4 <= data.size()) {
    const uint32_t st_declared = u32_at(cursor);
    cursor += 4;
    const size_t st_end = cursor + size_t(st_declared);
    if (st_end <= data.size()) {
      out.styles.reserve(style_count);
      for (uint32_t i = 0; i < style_count && cursor + 12 <= st_end; ++i) {
        UiBgfStyle st;
        st.owner_item = u32_at(cursor);
        st.state_count = u16_at(cursor + 10);
        cursor += 12 + size_t(st.state_count) * 24;
        if (cursor > st_end) break;
        out.styles.push_back(st);
      }
      cursor = st_end;
    }
  }

  // 5. Tracks / Keyframes section.
  if (cursor + 4 <= data.size()) {
    const uint32_t tr_declared = u32_at(cursor);
    cursor += 4;
    const size_t tr_end = cursor + size_t(tr_declared);
    if (tr_end <= data.size()) {
      out.tracks.reserve(track_count);
      for (uint32_t i = 0; i < track_count && cursor + 8 <= tr_end; ++i) {
        UiBgfTrack tr;
        tr.owner_item = u32_at(cursor);
        const uint32_t child_count = u32_at(cursor + 4);
        cursor += 8;
        for (uint32_t ch = 0; ch < child_count && cursor + 8 <= tr_end; ++ch) {
          const uint32_t key_count = u32_at(cursor + 4);
          cursor += 8;
          for (uint32_t k = 0; k < key_count && cursor + 20 <= tr_end; ++k) {
            UiBgfKeyframe kf;
            kf.target_item = u32_at(cursor);
            kf.attr_hash = u32_at(cursor + 4);
            kf.time = f32_at(cursor + 8);
            kf.raw_tag = u32_at(cursor + 12);
            kf.raw_value = u32_at(cursor + 16);
            kf.value = f32_at(cursor + 16);
            tr.keys.push_back(kf);
            cursor += 20;
          }
        }
        out.tracks.push_back(std::move(tr));
      }
      cursor = tr_end;
    }
  }

  // 6. Empty companion table + 7. Animations section.
  if (cursor + 4 <= data.size()) {
    const uint32_t empty_decl = u32_at(cursor);
    cursor += 4 + size_t(empty_decl);
  }
  if (cursor + 4 <= data.size()) {
    const uint32_t anim_declared = u32_at(cursor);
    cursor += 4;
    const size_t anim_end = cursor + size_t(anim_declared);
    if (anim_end <= data.size()) {
      out.animations.reserve(anim_count);
      for (uint32_t i = 0; i < anim_count && cursor + 33 <= anim_end; ++i) {
        UiBgfAnimation an;
        an.owner_item = u32_at(cursor + 16);
        an.track_count = u32_at(cursor + 29);
        cursor += 33;
        for (uint32_t t = 0; t < an.track_count && cursor + 9 <= anim_end; ++t) {
          cursor += 5;
          const uint32_t kc = u32_at(cursor);
          cursor += 4 + size_t(kc) * 8;
          if (cursor + 4 > anim_end) break;
          const uint32_t ec = u32_at(cursor);
          cursor += 4 + size_t(ec) * 4;
          an.key_count += kc;
          an.event_count += ec;
        }
        out.animations.push_back(an);
      }
    }
  }
  return true;
}

float evaluate_bgf_track(const UiBgfTrack& track, float time_s, float fallback) {
  if (track.keys.empty()) return fallback;
  if (time_s <= track.keys.front().time) return track.keys.front().value;
  if (time_s >= track.keys.back().time) return track.keys.back().value;
  for (size_t i = 0; i + 1 < track.keys.size(); ++i) {
    const auto& a = track.keys[i];
    const auto& b = track.keys[i + 1];
    if (time_s >= a.time && time_s <= b.time) {
      const float span = b.time - a.time;
      const float t = span > 1e-6f ? (time_s - a.time) / span : 0.0f;
      return a.value + (b.value - a.value) * t;
    }
  }
  return track.keys.back().value;
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

}  // namespace fh1
