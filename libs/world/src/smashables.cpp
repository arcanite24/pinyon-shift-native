#include "world/smashables.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "fh1data/archive.h"
#include "fh1data/bytes.h"

namespace world {
namespace {

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool read_attr(std::string_view tag, std::string_view name, std::string& out) {
  for (size_t p = tag.find(name); p != std::string_view::npos; p = tag.find(name, p + 1)) {
    if (p == 0 || (tag[p - 1] != ' ' && tag[p - 1] != '\t' && tag[p - 1] != '\n' && tag[p - 1] != '\r')) continue;
    size_t q = p + name.size();
    while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t')) ++q;
    if (q >= tag.size() || tag[q] != '=') continue;
    ++q;
    while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t')) ++q;
    if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) continue;
    const size_t close = tag.find(tag[q], q + 1);
    if (close == std::string_view::npos) return false;
    out = std::string(tag.substr(q + 1, close - q - 1));
    return true;
  }
  return false;
}

bool read_attr_f(std::string_view tag, std::string_view name, float& out) {
  std::string v;
  if (!read_attr(tag, name, v)) return false;
  out = float(std::atof(v.c_str()));
  return true;
}

bool read_attr_i(std::string_view tag, std::string_view name, int& out) {
  std::string v;
  if (!read_attr(tag, name, v)) return false;
  out = std::atoi(v.c_str());
  return true;
}

size_t find_tag(std::string_view xml, std::string_view name, size_t from, size_t to) {
  const std::string open = "<" + std::string(name);
  for (size_t p = xml.find(open, from); p != std::string_view::npos && p < to; p = xml.find(open, p + 1)) {
    const size_t q = p + open.size();
    if (q < xml.size() && (xml[q] == ' ' || xml[q] == '\t' || xml[q] == '/' || xml[q] == '>' || xml[q] == '\r' ||
                           xml[q] == '\n'))
      return p;
  }
  return std::string_view::npos;
}

std::string_view tag_slice(std::string_view xml, size_t start) {
  const size_t end = xml.find('>', start);
  return end == std::string_view::npos ? std::string_view{} : xml.substr(start, end - start + 1);
}

bool parse_definition(fh1::BeReader& r, PropPhysicsDef& d, std::string* error) {
  d = PropPhysicsDef{};
  d.version = int32_t(r.u32());
  if (d.version < 1 || d.version > 64) {
    if (error) *error = "unexpected PhysicsDefinitions version " + std::to_string(d.version);
    return false;
  }
  d.definition_type = (d.version >= 8) ? int32_t(r.u32()) : 0;
  d.mass = r.f32();
  for (float& v : d.inertia_tensor) v = r.f32();
  for (float& v : d.inv_inertia_tensor) v = r.f32();
  for (float& v : d.half_extents) v = r.f32();
  d.bounding_radius = r.f32();
  for (float& v : d.graphics_offset) v = r.f32();
  d.num_children = int32_t(r.u32());
  if (d.num_children < 0 || d.num_children > 256) {
    if (error) *error = "invalid NumChildren " + std::to_string(d.num_children);
    return false;
  }
  if (d.version >= 12) {
    for (float& v : d.aabb_centre_offset) v = r.f32();
  }

  if (d.definition_type == 0) {
    if (d.version < 13) d.legacy_render_model_index = int32_t(r.u32());
    d.visible_distance = r.f32();
    d.surface_type_index = int32_t(r.u32());
    d.break_speed_mph = r.f32();
    d.unique_index = int32_t(r.u32());
    d.object_name_hash = r.u32();
    if (d.version >= 9) {
      d.object_type_index = static_cast<int8_t>(r.u8());
    }
    const int32_t num_shapes = int32_t(r.u32());
    if (num_shapes < 0 || num_shapes > 1024) {
      if (error) *error = "invalid NumCollisionShapes " + std::to_string(num_shapes);
      return false;
    }
    d.shapes.resize(size_t(num_shapes));
    for (int32_t si = 0; si < num_shapes; ++si) {
      PropShape& s = d.shapes[size_t(si)];
      const int32_t stype = int32_t(r.u32());
      if (stype == 0) {
        s.kind = PropShapeKind::kSphere;
        if (d.version >= 10) {
          (void)r.u8();
          s.flags = r.u8();
        } else {
          (void)r.u32();
          s.flags = 3;
        }
        for (float& v : s.sphere) v = r.f32();
      } else if (stype == 1) {
        s.kind = PropShapeKind::kObb;
        if (d.version >= 10) {
          (void)r.u8();
          s.flags = r.u8();
        } else {
          (void)r.u32();
          s.flags = 3;
        }
        for (float& v : s.transform) v = r.f32();
        for (float& v : s.half_extents) v = r.f32();
      } else if (stype == 2) {
        s.kind = PropShapeKind::kConvexHull;
        const int32_t hull_size = int32_t(r.u32());
        if (hull_size < 104 || hull_size > 1024 * 1024) {
          if (error) *error = "invalid ConvexHullSize " + std::to_string(hull_size);
          return false;
        }
        const auto blob = r.raw(size_t(hull_size));
        fh1::BeReader hr(blob);
        (void)hr.u8();
        s.flags = hr.u8();
        hr.seek(16);
        for (float& v : s.transform) v = hr.f32();
        hr.seek(80);
        const uint32_t off_verts = hr.u32();
        (void)hr.u32();  // off_vert_adj
        const uint32_t off_edges = hr.u32();
        const uint32_t off_faces = hr.u32();
        const uint8_t num_verts = hr.u8();
        const uint8_t num_edges = hr.u8();
        const uint8_t num_faces = hr.u8();
        (void)hr.u8();
        const uint32_t total_size = hr.u32();
        if (total_size != uint32_t(hull_size)) {
          if (error) *error = "convex hull total_size mismatch";
          return false;
        }
        s.hull_verts.resize(num_verts);
        hr.seek(off_verts);
        for (auto& v : s.hull_verts) {
          v[0] = hr.f32();
          v[1] = hr.f32();
          v[2] = hr.f32();
          (void)hr.f32();
        }
        s.hull_edges.resize(num_edges);
        hr.seek(off_edges);
        for (auto& e : s.hull_edges) {
          e[0] = hr.u8();
          e[1] = hr.u8();
        }
        s.hull_faces.resize(num_faces);
        for (uint8_t fi = 0; fi < num_faces; ++fi) {
          hr.seek(off_faces + size_t(fi) * 8);
          const uint32_t off_fv = hr.u32();
          const uint8_t nfv = hr.u8();
          s.hull_faces[fi].resize(nfv);
          hr.seek(off_fv);
          for (uint8_t k = 0; k < nfv; ++k) s.hull_faces[fi][k] = hr.u8();
        }
      } else {
        if (error) *error = "unknown PropShapeType " + std::to_string(stype);
        return false;
      }
    }
    d.children.resize(size_t(d.num_children));
    for (int32_t ci = 0; ci < d.num_children; ++ci) {
      if (!parse_definition(r, d.children[size_t(ci)], error)) return false;
    }
  }
  return true;
}

void accumulate_counts(const PropPhysicsDef& d, bool is_top, PhysicsDefinitionsCounts& c) {
  ++c.by_object_type[int(d.object_type_index)];
  for (const auto& s : d.shapes) {
    if (s.kind == PropShapeKind::kSphere) ++c.spheres;
    else if (s.kind == PropShapeKind::kObb) ++c.obbs;
    else if (s.kind == PropShapeKind::kConvexHull) ++c.hulls;
  }
  if (is_top && !d.children.empty()) {
    ++c.parents_with_children;
    c.max_children = std::max(c.max_children, d.children.size());
  }
  c.total_children += d.children.size();
  for (const auto& ch : d.children) accumulate_counts(ch, false, c);
}

drive::Quat quat_normalize(const drive::Quat& q) {
  const double len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (len < 1e-9) return {0, 0, 0, 1};
  const double inv = 1.0 / len;
  return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

drive::Quat quat_integrate(const drive::Quat& q, const std::array<float, 3>& omega, float dt) {
  const double hx = 0.5 * double(omega[0]) * double(dt);
  const double hy = 0.5 * double(omega[1]) * double(dt);
  const double hz = 0.5 * double(omega[2]) * double(dt);
  // dq = 0.5 * (wx, wy, wz, 0) * q
  drive::Quat out{
      q.x + (hx * q.w + hy * q.z - hz * q.y),
      q.y + (hy * q.w + hz * q.x - hx * q.z),
      q.z + (hz * q.w + hx * q.y - hy * q.x),
      q.w + (-hx * q.x - hy * q.y - hz * q.z),
  };
  return quat_normalize(out);
}

}  // namespace

const PropPhysicsDef* PhysicsDefinitions::resolve_packed(uint32_t packed_id) const {
  const uint16_t global_idx = uint16_t((packed_id >> 16) & 0xFFFFu);
  const uint16_t child_idx = uint16_t(packed_id & 0xFFFFu);
  if (global_idx >= global_to_list.size()) return nullptr;
  const uint16_t list_idx = global_to_list[global_idx];
  if (list_idx >= definitions.size()) return nullptr;
  const PropPhysicsDef& parent = definitions[list_idx];
  if (child_idx == 0xFFFFu) return &parent;
  if (child_idx < parent.children.size()) return &parent.children[child_idx];
  return nullptr;
}

const PropPhysicsDef* PhysicsDefinitions::find_by_object_type(int8_t object_type_index) const {
  for (const auto& d : definitions) {
    if (d.object_type_index == object_type_index) return &d;
  }
  return nullptr;
}

PhysicsDefinitionsCounts PhysicsDefinitions::counts() const {
  PhysicsDefinitionsCounts c;
  c.top_level = definitions.size();
  c.max_global_index = global_to_list.size();
  for (const auto& d : definitions) accumulate_counts(d, true, c);
  return c;
}

bool parse_physics_definitions(std::span<const uint8_t> bytes, PhysicsDefinitions& out, std::string* error) {
  out = PhysicsDefinitions{};
  try {
    fh1::BeReader r(bytes);
    const uint32_t num_defs = r.u32();
    if (num_defs > 4096) {
      if (error) *error = "NumDefinitions too large: " + std::to_string(num_defs);
      return false;
    }
    out.definitions.resize(num_defs);
    for (uint32_t i = 0; i < num_defs; ++i) {
      if (!parse_definition(r, out.definitions[i], error)) return false;
    }
    if (num_defs > 0 && out.definitions[0].version >= 11) {
      const uint32_t max_global = r.u32();
      if (max_global > 16384) {
        if (error) *error = "MaxGlobalIndex too large: " + std::to_string(max_global);
        return false;
      }
      out.global_to_list.resize(max_global);
      for (uint32_t g = 0; g < max_global; ++g) {
        out.global_to_list[g] = r.u16();
        if (out.global_to_list[g] >= num_defs) {
          if (error) *error = "GlobalToListIndex out of range";
          return false;
        }
      }
      if (out.definitions[0].version >= 13) {
        out.child_render_models.resize(max_global);
        for (uint32_t g = 0; g < max_global; ++g) {
          const auto& d = out.definitions[out.global_to_list[g]];
          const size_t count = size_t(std::max<int32_t>(1, d.num_children));
          out.child_render_models[g].resize(count);
          for (size_t k = 0; k < count; ++k) {
            out.child_render_models[g][k] = int32_t(r.u32());
          }
        }
      }
    }
    if (r.remaining() != 0) {
      if (error) *error = "PhysicsDefinitions.bin trailing bytes: " + std::to_string(r.remaining());
      return false;
    }
  } catch (const fh1::FormatError& e) {
    if (error) *error = std::string("PhysicsDefinitions.bin: ") + e.what();
    return false;
  }
  return true;
}

bool load_physics_definitions_file(const std::filesystem::path& path, PhysicsDefinitions& out, std::string* error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    if (error) *error = "cannot open " + path.string();
    return false;
  }
  const auto sz = f.tellg();
  if (sz < 4) {
    if (error) *error = "file too short: " + path.string();
    return false;
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(sz));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(bytes.data()), sz);
  return parse_physics_definitions(bytes, out, error);
}

bool parse_coll_objects_xml(std::string_view xml, std::vector<CollObjectType>& out) {
  out.clear();
  const size_t root = find_tag(xml, "CollObjTypes", 0, xml.size());
  if (root == std::string_view::npos) return false;
  const size_t root_gt = xml.find('>', root);
  const size_t root_end = xml.find("</CollObjTypes>", root_gt);
  if (root_gt == std::string_view::npos || root_end == std::string_view::npos) return false;

  size_t p = root_gt + 1;
  while (p < root_end) {
    const size_t lt = xml.find('<', p);
    if (lt == std::string_view::npos || lt >= root_end) break;
    if (lt + 1 < root_end && (xml[lt + 1] == '/' || xml[lt + 1] == '!' || xml[lt + 1] == '?')) {
      p = xml.find('>', lt);
      if (p == std::string_view::npos) break;
      ++p;
      continue;
    }
    const size_t gt = xml.find('>', lt);
    if (gt == std::string_view::npos || gt >= root_end) break;
    size_t name_end = lt + 1;
    while (name_end < gt && xml[name_end] != ' ' && xml[name_end] != '\t' && xml[name_end] != '/' &&
           xml[name_end] != '\r' && xml[name_end] != '\n')
      ++name_end;
    const std::string type_name(xml.substr(lt + 1, name_end - (lt + 1)));
    const std::string close = "</" + type_name + ">";
    const size_t close_pos = xml.find(close, gt);
    if (close_pos == std::string_view::npos || close_pos > root_end) {
      p = gt + 1;
      continue;
    }

    CollObjectType obj;
    obj.name = type_name;
    if (const size_t cp = find_tag(xml, "Collision", gt, close_pos); cp != std::string_view::npos) {
      const auto ctag = tag_slice(xml, cp);
      float ox = 0, oy = 0, oz = 0, pscale = 1.0f, rscale = 1.0f;
      int n_spheres = 0;
      read_attr_i(ctag, "NumCollSpheres", n_spheres);
      read_attr_f(ctag, "SpherePosOffsetX", ox);
      read_attr_f(ctag, "SpherePosOffsetY", oy);
      read_attr_f(ctag, "SpherePosOffsetZ", oz);
      read_attr_f(ctag, "SpherePosScale", pscale);
      read_attr_f(ctag, "SphereRadiusScale", rscale);
      const size_t cend = std::min(close_pos, xml.find("</Collision>", cp));
      for (int si = 0; si < n_spheres; ++si) {
        const std::string sname = "Sphere" + std::to_string(si);
        if (const size_t sp = find_tag(xml, sname, cp, cend); sp != std::string_view::npos) {
          const auto st = tag_slice(xml, sp);
          float r = 0.1f, sx = 0, sy = 0, sz = 0;
          read_attr_f(st, "Radius", r);
          read_attr_f(st, "XPos", sx);
          read_attr_f(st, "YPos", sy);
          read_attr_f(st, "ZPos", sz);
          obj.spheres.push_back({{sx * pscale + ox, sy * pscale + oy, sz * pscale + oz}, r * rscale});
        }
      }
    }
    if (const size_t mp = find_tag(xml, "MassDistribution", gt, close_pos); mp != std::string_view::npos) {
      const size_t mend = std::min(close_pos, xml.find("</MassDistribution>", mp));
      float mass_sum = 0.0f;
      for (size_t q = mp; q < mend;) {
        const size_t tag_p = xml.find("<Sphere", q);
        const size_t blk_p = xml.find("<Block", q);
        const size_t next_p = std::min(tag_p, blk_p);
        if (next_p == std::string_view::npos || next_p >= mend) break;
        const auto t = tag_slice(xml, next_p);
        float m = 0.0f;
        if (read_attr_f(t, "Mass", m)) mass_sum += m;
        q = next_p + t.size();
      }
      if (mass_sum > 0.0f) obj.total_mass = mass_sum;
    }
    if (const size_t misc = find_tag(xml, "Misc", gt, close_pos); misc != std::string_view::npos) {
      const auto mt = tag_slice(xml, misc);
      read_attr_i(mt, "SurfaceId", obj.surface_id);
      int trig = 0;
      if (read_attr_i(mt, "IsTrigger", trig)) obj.is_trigger = (trig != 0);
    }
    out.push_back(std::move(obj));
    p = close_pos + close.size();
  }
  return !out.empty();
}

bool parse_smashable_object_types_xml(std::string_view xml, std::vector<std::string>& out) {
  out.clear();
  for (size_t p = find_tag(xml, "Object", 0, xml.size()); p != std::string_view::npos;
       p = find_tag(xml, "Object", p + 1, xml.size())) {
    std::string t;
    if (read_attr(tag_slice(xml, p), "type", t)) out.push_back(t);
  }
  return !out.empty();
}

bool parse_coll_objs_xml(std::string_view xml, std::vector<CollObjPlacement>& out) {
  out.clear();
  size_t p = 0;
  while (true) {
    const size_t obj_p = xml.find("<Obj", p);
    if (obj_p == std::string_view::npos) break;
    if (obj_p + 4 >= xml.size() || !std::isdigit(static_cast<unsigned char>(xml[obj_p + 4]))) {
      p = obj_p + 4;
      continue;
    }
    const size_t gt = xml.find('>', obj_p);
    if (gt == std::string_view::npos) break;
    const size_t end = xml.find("</Obj", gt);
    if (end == std::string_view::npos) break;

    CollObjPlacement item;
    const auto header = xml.substr(obj_p, gt - obj_p + 1);
    if (read_attr(header, "PhysicsType", item.physics_type)) {
      const size_t dot = item.physics_type.find('.');
      item.stem = to_lower(dot == std::string::npos ? item.physics_type : item.physics_type.substr(0, dot));
      if (const size_t pp = find_tag(xml, "Pos", gt, end); pp != std::string_view::npos) {
        const auto pt = tag_slice(xml, pp);
        read_attr_f(pt, "x", item.pos[0]);
        read_attr_f(pt, "y", item.pos[1]);
        read_attr_f(pt, "z", item.pos[2]);
      }
      auto read_ax = [&](std::string_view ax, std::array<float, 3>& dst) {
        if (const size_t ap = find_tag(xml, ax, gt, end); ap != std::string_view::npos) {
          const auto at = tag_slice(xml, ap);
          read_attr_f(at, "x", dst[0]);
          read_attr_f(at, "y", dst[1]);
          read_attr_f(at, "z", dst[2]);
        }
      };
      read_ax("XAxis", item.x_axis);
      read_ax("YAxis", item.y_axis);
      read_ax("ZAxis", item.z_axis);
      out.push_back(std::move(item));
    }
    p = end + 5;
  }
  return !out.empty();
}

// ---------------------------------------------------------------------------
// SmashableWorld
// ---------------------------------------------------------------------------

int64_t SmashableWorld::cell_key(float x, float z) {
  const int32_t gx = int32_t(std::floor(x / 25.0f));
  const int32_t gz = int32_t(std::floor(z / 25.0f));
  return (int64_t(gx) << 32) ^ uint32_t(gz);
}

float SmashableWorld::rand01() {
  uint32_t x = rng_;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_ = x ? x : 0xA3C59AC3u;
  return float(rng_ & 0x00FFFFFFu) / float(0x01000000u);
}

float SmashableWorld::rand_sym() { return rand01() * 2.0f - 1.0f; }

bool SmashableWorld::should_break(const PropPhysicsDef& def, float impact_speed_mps) {
  if (def.unbreakable()) return false;
  const float thresh = def.break_speed_mps();
  return (impact_speed_mps * impact_speed_mps) > (thresh * thresh) && impact_speed_mps > 0.1f;
}

const PropPhysicsDef* SmashableWorld::match_definition(const CollObjPlacement& p) const {
  if (defs_.definitions.empty()) return nullptr;
  const std::string& st = p.stem;
  // Match known gameplay smashable categories first via ObjectTypeIndex.
  if (st.find("flyer") != std::string::npos || st.find("discount") != std::string::npos) {
    if (const auto* d = defs_.find_by_object_type(1)) return d;
  }
  if (st.find("mailbox") != std::string::npos) {
    if (const auto* d = defs_.find_by_object_type(2)) return d;
  }
  if (st.find("fence") != std::string::npos || st.find("woodenbarrier") != std::string::npos ||
      st.find("barrier") != std::string::npos) {
    if (const auto* d = defs_.find_by_object_type(0)) return d;
  }
  if (st.find("racearch") != std::string::npos || st.find("startarch") != std::string::npos) {
    if (const auto* d = defs_.find_by_object_type(3)) return d;
  }
  // Match by material surface type hints for benches, bins, signs, cones, speed cameras, poles.
  auto find_by_surface = [&](int32_t surf) -> const PropPhysicsDef* {
    for (const auto& d : defs_.definitions) {
      if (d.surface_type_index == surf && !d.unbreakable()) return &d;
    }
    return nullptr;
  };
  if (st.find("cone") != std::string::npos) {
    if (const auto* d = find_by_surface(64)) return d;
  }
  if (st.find("speedcamera") != std::string::npos) {
    if (const auto* d = find_by_surface(67)) return d;
  }
  if (st.find("bench") != std::string::npos || st.find("picnic") != std::string::npos ||
      st.find("outhouse") != std::string::npos || st.find("table") != std::string::npos) {
    if (const auto* d = find_by_surface(60)) return d;
  }
  if (st.find("sign") != std::string::npos || st.find("pole") != std::string::npos ||
      st.find("arrow") != std::string::npos || st.find("bin") != std::string::npos) {
    if (const auto* d = find_by_surface(61)) return d;
  }
  if (st.find("parasol") != std::string::npos || st.find("umbrella") != std::string::npos) {
    if (const auto* d = find_by_surface(65)) return d;
  }
  // Fallback to a light generic breakable wood/plastic prop definition.
  for (const auto& d : defs_.definitions) {
    if (!d.unbreakable() && d.object_type_index == -1 && d.break_speed_mph <= 15.0f) return &d;
  }
  return &defs_.definitions[0];
}

std::array<uint8_t, 4> SmashableWorld::colour_for_prop(int32_t surface_type, int8_t object_type,
                                                       std::string_view stem) const {
  if (object_type == 1 || stem.find("flyer") != std::string_view::npos) {
    // Horizon discount sign: bright yellow/gold board shards!
    return {245, 200, 35, 255};
  }
  if (surface_type == 64 || stem.find("cone") != std::string_view::npos) {
    return {240, 105, 25, 255};  // road cone orange
  }
  if (surface_type == 60 || surface_type == 15 || object_type == 0) {
    return {168, 126, 78, 255};  // weathered Colorado fence/picnic wood
  }
  if (surface_type == 61 || surface_type == 66 || surface_type == 67 || object_type == 2) {
    return {175, 182, 190, 255};  // painted metal sign/pole/mailbox
  }
  if (surface_type == 63 || surface_type == 65) {
    return {215, 215, 220, 255};  // plastic/parasol
  }
  return {165, 138, 105, 255};
}

bool SmashableWorld::load(const std::filesystem::path& media_dir, const std::filesystem::path& world_dir,
                          std::string* error) {
  defs_ = PhysicsDefinitions{};
  coll_types_.clear();
  smash_types_.clear();
  props_.clear();
  grid_.clear();
  dynamic_bodies_.clear();
  evictions_ = 0;

  if (!world_dir.empty()) {
    const auto pd_path = world_dir / "PhysicsDefinitions.bin";
    if (std::filesystem::exists(pd_path)) {
      if (!load_physics_definitions_file(pd_path, defs_, error)) return false;
    }
    const auto sot_path = world_dir / "SmashableObjectTypes.xml";
    if (std::filesystem::exists(sot_path)) {
      parse_smashable_object_types_xml(read_text_file(sot_path), smash_types_);
    }
    const auto co_path = world_dir / "Ribbon_00" / "CollObjs.xml";
    if (std::filesystem::exists(co_path)) {
      std::vector<CollObjPlacement> placements;
      if (parse_coll_objs_xml(read_text_file(co_path), placements)) {
        props_.reserve(placements.size());
        for (const auto& pl : placements) add_prop(pl);
      }
    }
  }

  const auto pzip_path = media_dir / "physics.zip";
  if (std::filesystem::exists(pzip_path)) {
    std::string zerr;
    if (auto z = fh1::Archive::open(pzip_path, &zerr)) {
      if (auto res = z->read("CollObjects.xml"); res.ok()) {
        parse_coll_objects_xml(std::string_view(reinterpret_cast<const char*>(res.data.data()), res.data.size()),
                               coll_types_);
      }
      if (smash_types_.empty()) {
        if (auto res = z->read("SmashableObjectTypes.xml"); res.ok()) {
          parse_smashable_object_types_xml(
              std::string_view(reinterpret_cast<const char*>(res.data.data()), res.data.size()), smash_types_);
        }
      }
    }
  }
  return true;
}

bool SmashableWorld::load_barn_doors(const std::filesystem::path& game_objs_path,
                                     const std::vector<BarnDoorCollider>& barn_specs) {
  barn_doors_.clear();
  const std::string xml = read_text_file(game_objs_path);
  if (xml.empty()) return false;

  struct ObjPose {
    std::array<float, 3> pos{};
    std::array<float, 3> x_axis{1, 0, 0};
    std::array<float, 3> y_axis{0, 1, 0};
    std::array<float, 3> z_axis{0, 0, 1};
  };
  std::unordered_map<std::string, ObjPose> poses;
  size_t p = 0;
  while (true) {
    const size_t obj_p = xml.find("<Obj", p);
    if (obj_p == std::string_view::npos) break;
    const size_t gt = xml.find('>', obj_p);
    if (gt == std::string_view::npos) break;
    const size_t end = xml.find("</Obj", gt);
    if (end == std::string_view::npos) break;
    std::string gid;
    if (read_attr(xml.substr(obj_p, gt - obj_p + 1), "GameplayID", gid)) {
      const std::string low = to_lower(gid);
      if (low.rfind("bf_", 0) == 0) {
        ObjPose pose;
        if (const size_t pp = find_tag(xml, "Pos", gt, end); pp != std::string_view::npos) {
          const auto pt = tag_slice(xml, pp);
          read_attr_f(pt, "x", pose.pos[0]);
          read_attr_f(pt, "y", pose.pos[1]);
          read_attr_f(pt, "z", pose.pos[2]);
        }
        auto read_ax = [&](std::string_view ax, std::array<float, 3>& dst) {
          if (const size_t ap = find_tag(xml, ax, gt, end); ap != std::string_view::npos) {
            const auto at = tag_slice(xml, ap);
            read_attr_f(at, "x", dst[0]);
            read_attr_f(at, "y", dst[1]);
            read_attr_f(at, "z", dst[2]);
          }
        };
        read_ax("XAxis", pose.x_axis);
        read_ax("YAxis", pose.y_axis);
        read_ax("ZAxis", pose.z_axis);
        poses[low] = pose;
      }
    }
    p = end + 5;
  }

  for (auto spec : barn_specs) {
    const std::string cid = to_lower(spec.closed_coll_id);
    const std::string oid = to_lower(spec.open_coll_id);
    spec.closed_coll_id = cid;
    spec.open_coll_id = oid;
    auto it = poses.find(cid);
    if (it == poses.end()) it = poses.find(oid);
    if (it != poses.end()) {
      spec.pos = it->second.pos;
      spec.x_axis = it->second.x_axis;
      spec.y_axis = it->second.y_axis;
      spec.z_axis = it->second.z_axis;
      barn_doors_.push_back(std::move(spec));
    }
  }
  return !barn_doors_.empty();
}

void SmashableWorld::set_barn_open(std::string_view barn_name, bool open) {
  const std::string low = to_lower(barn_name);
  for (auto& b : barn_doors_) {
    if (to_lower(b.barn_name) == low) b.is_open = open;
  }
}

void SmashableWorld::sync_barns_from_hidden(const std::set<std::string>& hidden_ids) {
  for (auto& b : barn_doors_) {
    // When closed_coll_id is hidden (or open_coll_id is NOT hidden), the barn doorway is open!
    b.is_open = (!b.closed_coll_id.empty() && hidden_ids.count(b.closed_coll_id) > 0);
  }
}

void SmashableWorld::add_prop(const CollObjPlacement& p, const PropPhysicsDef* explicit_def) {
  PlacedProp pp;
  pp.place = p;
  pp.def = explicit_def ? explicit_def : match_definition(p);
  if (pp.def) {
    pp.radius = std::clamp(pp.def->bounding_radius, 0.35f, 2.5f);
  } else {
    pp.radius = 0.75f;
  }
  const uint32_t idx = uint32_t(props_.size());
  props_.push_back(std::move(pp));
  grid_[cell_key(p.pos[0], p.pos[2])].push_back(idx);
}

void SmashableWorld::spawn_dynamic_body(const DynamicBody& body) {
  if (dynamic_bodies_.size() >= kMaxDynamicBodies) {
    // Confirmed oldest-first FIFO eviction when the 384-slot pool is full.
    dynamic_bodies_.erase(dynamic_bodies_.begin());
    ++evictions_;
  }
  dynamic_bodies_.push_back(body);
}

int SmashableWorld::spawn_flyer_shatter(const std::array<float, 3>& pos, const std::array<float, 3>& car_vel) {
  const auto* flyer_def = defs_.find_by_object_type(1);
  const int pieces = (flyer_def && !flyer_def->children.empty()) ? int(flyer_def->children.size()) : 12;
  const float spd = std::sqrt(car_vel[0] * car_vel[0] + car_vel[2] * car_vel[2]);
  const float dir_x = (spd > 0.5f) ? car_vel[0] / spd : 0.0f;
  const float dir_z = (spd > 0.5f) ? car_vel[2] / spd : 1.0f;
  const float boost = std::clamp(spd * 1.15f, 6.0f, 32.0f);

  for (int i = 0; i < pieces; ++i) {
    DynamicBody b;
    const float row = float(i % 4) - 1.5f;
    const float col = float(i / 4) - 1.0f;
    b.pos = {pos[0] + row * 0.22f + rand_sym() * 0.1f, pos[1] + 0.45f + col * 0.28f + 0.35f,
             pos[2] + rand_sym() * 0.2f};
    b.vel = {
        dir_x * boost * (0.75f + 0.55f * rand01()) + rand_sym() * 4.5f,
        3.2f + rand01() * 5.2f,
        dir_z * boost * (0.75f + 0.55f * rand01()) + rand_sym() * 4.5f,
    };
    b.omega = {rand_sym() * 14.0f, rand_sym() * 14.0f, rand_sym() * 14.0f};
    if (flyer_def && size_t(i) < flyer_def->children.size()) {
      const auto& ch = flyer_def->children[size_t(i)];
      b.half_extents = {
          std::clamp(ch.half_extents[0] * 0.55f, 0.10f, 0.28f),
          std::clamp(ch.half_extents[1] * 0.55f, 0.07f, 0.22f),
          std::clamp(ch.half_extents[2] * 0.25f, 0.018f, 0.032f),
      };
      b.mass_kg = ch.mass_kg();
    } else {
      b.half_extents = {0.20f, 0.14f, 0.022f};
      b.mass_kg = 1.8f;
    }
    // Authentic Horizon discount sign board panels: crimson red face, charcoal trim, weathered wood frame.
    if (i % 3 == 0) b.colour = {205, 38, 62, 255};
    else if (i % 3 == 1) b.colour = {42, 40, 44, 255};
    else b.colour = {178, 136, 86, 255};
    spawn_dynamic_body(b);
  }
  return pieces;
}

void SmashableWorld::step_barn_doors(drive::Vehicle& vehicle) {
  if (barn_doors_.empty()) return;
  const drive::Vec3 cpos = vehicle.position();
  const double car_r = std::max(vehicle.params().size.x, vehicle.params().size.z) * 0.45;
  for (const auto& door : barn_doors_) {
    const double dx = cpos.x - double(door.pos[0]);
    const double dy = cpos.y - double(door.pos[1]);
    const double dz = cpos.z - double(door.pos[2]);
    if (dx * dx + dz * dz > 144.0 || std::fabs(dy) > 5.0) continue;

    // Express car centre in the barn door's local frame.
    const double lx = dx * door.x_axis[0] + dy * door.x_axis[1] + dz * door.x_axis[2];
    const double lz = dx * door.z_axis[0] + dy * door.z_axis[1] + dz * door.z_axis[2];

    // Closed barn door (`closed_coll_id`): solid doorway slab spanning [-2.4, +2.4] m in X, [-0.55, +0.55] m in Z.
    // Open barn door (`open_coll_id`): doorway centre [-2.0, +2.0] m is clear; only the two jamb posts collide.
    auto test_obb = [&](double bx_min, double bx_max, double bz_min, double bz_max) {
      const double cx = std::clamp(lx, bx_min, bx_max);
      const double cz = std::clamp(lz, bz_min, bz_max);
      const double rx = lx - cx, rz = lz - cz;
      const double dist2 = rx * rx + rz * rz;
      if (dist2 >= car_r * car_r) return;
      double nx_l = 0.0, nz_l = 0.0, pen = 0.0;
      if (dist2 > 1e-8) {
        const double dist = std::sqrt(dist2);
        nx_l = rx / dist;
        nz_l = rz / dist;
        pen = car_r - dist;
      } else {
        nx_l = 0.0;
        nz_l = (lz >= 0.0) ? 1.0 : -1.0;
        pen = car_r;
      }
      const drive::Vec3 n_world{
          nx_l * door.x_axis[0] + nz_l * door.z_axis[0],
          0.0,
          nx_l * door.x_axis[2] + nz_l * door.z_axis[2],
      };
      vehicle.translate(n_world * (pen + 0.02));
      const drive::Vec3 v = vehicle.velocity();
      const double vn = v.x * n_world.x + v.z * n_world.z;
      if (vn < 0.0) {
        const double j = -(1.25 * vn) * vehicle.params().mass;
        vehicle.apply_impulse(cpos, n_world * j, 0.15);
      }
    };

    if (!door.is_open) {
      test_obb(-2.5, 2.5, -0.55, 0.55);
    } else {
      // Open doorway jambs admit the car cleanly through [-2.0, +2.0] m!
      test_obb(-3.0, -2.05, -0.55, 0.55);
      test_obb(2.05, 3.0, -0.55, 0.55);
    }
  }
}

void SmashableWorld::step(drive::Vehicle& vehicle, const drive::Ground& ground, float dt,
                          std::vector<SmashEvent>& events) {
  events.clear();
  if (dt <= 0.0f) return;
  const float step_dt = std::min(dt, 0.05f);

  // 1. Barn-door open/closed physical collision swap.
  step_barn_doors(vehicle);

  // 2. Broadphase check of nearby intact smashables against the car OBB.
  const drive::Vec3 cpos = vehicle.position();
  const drive::Vec3 cvel = vehicle.velocity();
  const drive::Quat crot = vehicle.orientation();
  const drive::Vec3 cfwd = drive::rotate(crot, {0, 0, 1});
  const drive::Vec3 cright = drive::rotate(crot, {1, 0, 0});
  const float half_w = float(std::max(0.8, vehicle.params().size.x * 0.5));
  const float half_l = float(std::max(1.8, vehicle.params().size.z * 0.5));
  const float car_speed = float(std::sqrt(cvel.x * cvel.x + cvel.y * cvel.y + cvel.z * cvel.z));

  const int32_t cx_cell = int32_t(std::floor(float(cpos.x) / 25.0f));
  const int32_t cz_cell = int32_t(std::floor(float(cpos.z) / 25.0f));

  for (int32_t dz = -1; dz <= 1; ++dz) {
    for (int32_t dx = -1; dx <= 1; ++dx) {
      const int64_t key = (int64_t(cx_cell + dx) << 32) ^ uint32_t(cz_cell + dz);
      auto it = grid_.find(key);
      if (it == grid_.end()) continue;
      for (uint32_t idx : it->second) {
        auto& pp = props_[idx];
        if (pp.broken || !pp.def) continue;
        const float px = pp.place.pos[0] - float(cpos.x);
        const float py = pp.place.pos[1] - float(cpos.y);
        const float pz = pp.place.pos[2] - float(cpos.z);
        if (std::fabs(py) > 3.2f) continue;
        const float max_r = half_l + pp.radius + 0.5f;
        if (px * px + pz * pz > max_r * max_r) continue;

        const float along = px * float(cfwd.x) + pz * float(cfwd.z);
        const float across = px * float(cright.x) + pz * float(cright.z);
        if (std::fabs(along) > half_l + pp.radius * 0.65f || std::fabs(across) > half_w + pp.radius * 0.65f)
          continue;

        const auto& def = *pp.def;
        if (!should_break(def, car_speed)) {
          // Solid bounce against unbreakable or below-BreakSpeedMph prop.
          const float len = std::sqrt(px * px + pz * pz);
          if (len > 1e-4f) {
            const drive::Vec3 n{-double(px / len), 0.0, -double(pz / len)};
            const double vn = cvel.x * n.x + cvel.z * n.z;
            if (vn < 0.0) {
              vehicle.translate(n * 0.06);
              vehicle.apply_impulse(cpos, n * (-(1.2 * vn) * vehicle.params().mass), 0.15);
            }
          }
          continue;
        }

        // Break prop -> record broken position for static prop hiding and spawn fractured child debris.
        pp.broken = true;
        const float hide_r = std::clamp(std::max(pp.radius, def.bounding_radius) + 0.65f, 1.5f, 4.5f);
        auto next_hidden = broken_positions_
                               ? std::make_shared<std::vector<GatherOptions::HiddenSphere>>(*broken_positions_)
                               : std::make_shared<std::vector<GatherOptions::HiddenSphere>>();
        next_hidden->push_back({pp.place.pos, hide_r});
        broken_positions_ = std::move(next_hidden);

        const float prop_mass_kg = def.mass_kg();
        const float car_mass = float(std::max(500.0, vehicle.params().mass));
        // Apply clamped momentum exchange (minimal speed loss on lightweight <=30 kg props per spec).
        const float decel_frac = (def.mass <= 0.30f) ? std::min(0.025f, prop_mass_kg / (car_mass * 2.5f))
                                                     : std::clamp(prop_mass_kg / (car_mass + prop_mass_kg), 0.02f, 0.25f);
        if (car_speed > 0.5f) {
          const drive::Vec3 dv{-cvel.x * decel_frac, 0.0, -cvel.z * decel_frac};
          vehicle.apply_impulse(cpos, dv * vehicle.params().mass, 0.05);
        }

        const auto base_col = colour_for_prop(def.surface_type_index, def.object_type_index, pp.place.stem);
        auto vary_col = [&](std::array<uint8_t, 4> c) {
          const float shade = 0.82f + 0.30f * rand01();
          return std::array<uint8_t, 4>{
              uint8_t(std::clamp(int(float(c[0]) * shade), 0, 255)),
              uint8_t(std::clamp(int(float(c[1]) * shade), 0, 255)),
              uint8_t(std::clamp(int(float(c[2]) * shade), 0, 255)),
              c[3],
          };
        };
        int spawned = 0;
        const float impulse_scale = std::clamp(car_speed * 1.15f, 5.5f, 30.0f);
        const float dir_x = (car_speed > 0.5f) ? float(cvel.x) / car_speed : float(cfwd.x);
        const float dir_z = (car_speed > 0.5f) ? float(cvel.z) / car_speed : float(cfwd.z);

        if (!def.children.empty()) {
          for (size_t ci = 0; ci < def.children.size(); ++ci) {
            const auto& ch = def.children[ci];
            DynamicBody b;
            b.pos = {
                pp.place.pos[0] + ch.aabb_centre_offset[0] + rand_sym() * 0.35f,
                pp.place.pos[1] + std::max(0.25f, ch.aabb_centre_offset[1] + 0.5f) + rand01() * 0.45f,
                pp.place.pos[2] + ch.aabb_centre_offset[2] + rand_sym() * 0.35f,
            };
            b.vel = {
                dir_x * impulse_scale * (0.65f + 0.65f * rand01()) + rand_sym() * 3.8f,
                2.8f + rand01() * 4.8f,
                dir_z * impulse_scale * (0.65f + 0.65f * rand01()) + rand_sym() * 3.8f,
            };
            b.omega = {rand_sym() * 12.0f, rand_sym() * 12.0f, rand_sym() * 12.0f};
            // Preserve authentic fractured plank/panel proportions from child_render_models / child AABBs.
            b.half_extents = {
                std::clamp(ch.half_extents[0], 0.05f, 0.72f),
                std::clamp(ch.half_extents[1], 0.04f, 0.72f),
                std::clamp(ch.half_extents[2], 0.02f, 0.22f),
            };
            b.mass_kg = ch.mass_kg();
            b.colour = vary_col(base_col);
            spawn_dynamic_body(b);
            ++spawned;
          }
        } else {
          // Single-piece breakable prop (e.g. fence plank, sign post, mailbox): spawn proportioned shards.
          const int chunks = (def.object_type_index == 0) ? 4 : 3;
          for (int ci = 0; ci < chunks; ++ci) {
            DynamicBody b;
            b.pos = {pp.place.pos[0] + rand_sym() * 0.25f, pp.place.pos[1] + 0.45f + float(ci) * 0.2f,
                     pp.place.pos[2] + rand_sym() * 0.25f};
            b.vel = {
                dir_x * impulse_scale * (0.7f + 0.5f * rand01()) + rand_sym() * 3.5f,
                2.5f + rand01() * 4.5f,
                dir_z * impulse_scale * (0.7f + 0.5f * rand01()) + rand_sym() * 3.5f,
            };
            b.omega = {rand_sym() * 11.0f, rand_sym() * 11.0f, rand_sym() * 11.0f};
            if (def.object_type_index == 0) {
              // Wooden fence slats/rails: elongated thin planks rather than cubes.
              b.half_extents = {0.42f + 0.18f * rand01(), 0.07f + 0.03f * rand01(), 0.025f};
            } else if (def.object_type_index == 1) {
              // Road sign / flyer shards: flat thin panel fragments.
              b.half_extents = {0.26f + 0.10f * rand01(), 0.22f + 0.08f * rand01(), 0.018f};
            } else {
              b.half_extents = {
                  std::clamp(def.half_extents[0] * 0.45f, 0.09f, 0.48f),
                  std::clamp(def.half_extents[1] * 0.40f, 0.06f, 0.48f),
                  std::clamp(def.half_extents[2] * 0.30f, 0.03f, 0.18f),
              };
            }
            b.mass_kg = prop_mass_kg / float(chunks);
            b.colour = vary_col(base_col);
            spawn_dynamic_body(b);
            ++spawned;
          }
        }

        SmashEvent ev;
        ev.pos = pp.place.pos;
        ev.impulse_vel = {dir_x * impulse_scale, 3.5f, dir_z * impulse_scale};
        ev.prop_stem = pp.place.stem;
        ev.object_type_index = def.object_type_index;
        ev.surface_type_index = def.surface_type_index;
        ev.debris_spawned = spawned;
        ev.hide_radius = hide_r;
        events.push_back(std::move(ev));
      }
    }
  }

  // 3. Integrate all active dynamic debris bodies (gravity, spin, ground bounce, despawn).
  size_t write = 0;
  for (size_t i = 0; i < dynamic_bodies_.size(); ++i) {
    DynamicBody& b = dynamic_bodies_[i];
    b.age += step_dt;
    if (b.age >= kDebrisDespawnSeconds) continue;

    if (!b.settled) {
      b.vel[1] -= 9.81f * step_dt;
      b.pos[0] += b.vel[0] * step_dt;
      b.pos[1] += b.vel[1] * step_dt;
      b.pos[2] += b.vel[2] * step_dt;
      b.rot = quat_integrate(b.rot, b.omega, step_dt);

      const float min_h = std::min({b.half_extents[0], b.half_extents[1], b.half_extents[2]});
      drive::Hit hit;
      if (ground.raycast({double(b.pos[0]), double(b.pos[1]) + 1.5, double(b.pos[2])}, {0, -1, 0}, 4.0, hit)) {
        const float floor_y = float(double(b.pos[1]) + 1.5 - hit.distance) + min_h;
        if (b.pos[1] < floor_y) {
          b.pos[1] = floor_y;
          if (b.vel[1] < -0.6f) {
            b.vel[1] = -b.vel[1] * 0.34f;
          } else {
            b.vel[1] = 0.0f;
          }
          b.vel[0] *= 0.72f;
          b.vel[2] *= 0.72f;
          b.omega[0] *= 0.68f;
          b.omega[1] *= 0.68f;
          b.omega[2] *= 0.68f;
          const float spd2 = b.vel[0] * b.vel[0] + b.vel[1] * b.vel[1] + b.vel[2] * b.vel[2];
          if (spd2 < 0.08f && b.age > 0.6f) b.settled = true;
        }
      }
    }

    if (write != i) dynamic_bodies_[write] = b;
    ++write;
  }
  dynamic_bodies_.resize(write);
}

void SmashableWorld::clear_debris() { dynamic_bodies_.clear(); }

void SmashableWorld::reset_broken() {
  dynamic_bodies_.clear();
  broken_positions_.reset();
  for (auto& pp : props_) pp.broken = false;
}

void SmashableWorld::build_debris_mesh(std::vector<DebrisVertex>& out_tris) const {
  out_tris.clear();
  if (dynamic_bodies_.empty()) return;
  out_tris.reserve(dynamic_bodies_.size() * 36);

  // Unit cube 6 faces (2 triangles each = 36 vertices).
  static constexpr int kFaces[6][4] = {
      {0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0},
  };
  static constexpr float kNormals[6][3] = {
      {0, 0, 1}, {0, 0, -1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0},
  };
  static constexpr float kCorners[8][3] = {
      {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
  };

  for (const auto& b : dynamic_bodies_) {
    const float fade = (b.age > kDebrisDespawnSeconds - 1.5f)
                           ? std::clamp((kDebrisDespawnSeconds - b.age) / 1.5f, 0.0f, 1.0f)
                           : 1.0f;
    if (fade <= 0.02f) continue;
    const float scale = (fade < 1.0f) ? (0.4f + 0.6f * fade) : 1.0f;
    const float hx = b.half_extents[0] * scale;
    const float hy = b.half_extents[1] * scale;
    const float hz = b.half_extents[2] * scale;

    std::array<std::array<float, 3>, 8> wv{};
    for (int i = 0; i < 8; ++i) {
      const drive::Vec3 r = drive::rotate(b.rot, {kCorners[i][0] * hx, kCorners[i][1] * hy, kCorners[i][2] * hz});
      wv[size_t(i)] = {b.pos[0] + float(r.x), b.pos[1] + float(r.y), b.pos[2] + float(r.z)};
    }
    std::array<uint8_t, 4> col = b.colour;
    col[3] = uint8_t(float(col[3]) * fade);

    for (int f = 0; f < 6; ++f) {
      const drive::Vec3 wn = drive::rotate(b.rot, {kNormals[f][0], kNormals[f][1], kNormals[f][2]});
      const std::array<float, 3> n = {float(wn.x), float(wn.y), float(wn.z)};
      const auto& p0 = wv[size_t(kFaces[f][0])];
      const auto& p1 = wv[size_t(kFaces[f][1])];
      const auto& p2 = wv[size_t(kFaces[f][2])];
      const auto& p3 = wv[size_t(kFaces[f][3])];
      out_tris.push_back({p0, n, col});
      out_tris.push_back({p1, n, col});
      out_tris.push_back({p2, n, col});
      out_tris.push_back({p0, n, col});
      out_tris.push_back({p2, n, col});
      out_tris.push_back({p3, n, col});
    }
  }
}

}  // namespace world
