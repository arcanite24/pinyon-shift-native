#include "world/smashables.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "fh1data/archive.h"
#include "fh1data/bytes.h"
#include "fh1data/members.h"
#include "fh1data/placements.h"
#include "fh1data/props.h"
#include "fh1data/rmb.h"
#include "fh1data/world_texture.h"

namespace world {
namespace {

constexpr uint32_t kPlaceholderTextureId = 0x00002CA9u;

struct DebrisModelVertex {
  std::array<float, 3> pos{};     // local game frame (v_file.x, v_file.y, -v_file.z)
  std::array<float, 3> normal{};  // local game frame (n_file.x, n_file.y, -n_file.z)
  std::array<float, 2> uv{};
  std::array<uint8_t, 4> rgba{255, 255, 255, 255};
};

struct DebrisSubmesh {
  uint32_t texture_record = kNoTexture;
  uint8_t cutout = 0;
  uint8_t decal = 0;
  std::vector<DebrisModelVertex> tris;  // 3 verts per triangle
};

struct DebrisModel {
  std::vector<DebrisSubmesh> submeshes;
  std::array<float, 3> box_min{0, 0, 0};
  std::array<float, 3> box_max{0, 0, 0};
  bool valid() const {
    for (const auto& s : submeshes)
      if (!s.tris.empty()) return true;
    return false;
  }
};

drive::Quat quat_normalize(const drive::Quat& q);

drive::Quat quat_from_basis(const std::array<float, 3>& x_in, const std::array<float, 3>& y_in,
                            const std::array<float, 3>& z_in) {
  auto norm3 = [](drive::Vec3 v, drive::Vec3 fallback) -> drive::Vec3 {
    const double l = std::sqrt(drive::dot(v, v));
    return l > 1e-6 ? v * (1.0 / l) : fallback;
  };
  const drive::Vec3 x = norm3({x_in[0], x_in[1], x_in[2]}, {1, 0, 0});
  drive::Vec3 y = {y_in[0], y_in[1], y_in[2]};
  y = norm3(y - x * drive::dot(y, x), {0, 1, 0});
  drive::Vec3 z = drive::cross(x, y);
  // Respect input Z orientation if consistent, keeping right-handedness (det = +1).
  if (drive::dot(z, {z_in[0], z_in[1], z_in[2]}) < 0.0) {
    y = y * -1.0;
    z = drive::cross(x, y);
  }
  z = norm3(z, {0, 0, 1});

  // Rotation matrix R has columns (x, y, z): R[row][col].
  const double m00 = x.x, m01 = y.x, m02 = z.x;
  const double m10 = x.y, m11 = y.y, m12 = z.y;
  const double m20 = x.z, m21 = y.z, m22 = z.z;
  const double trace = m00 + m11 + m22;
  drive::Quat q{};
  if (trace > 0.0) {
    const double s = std::sqrt(trace + 1.0) * 2.0;
    q.w = 0.25 * s;
    q.x = (m21 - m12) / s;
    q.y = (m02 - m20) / s;
    q.z = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const double s = std::sqrt(std::max(1e-12, 1.0 + m00 - m11 - m22)) * 2.0;
    q.w = (m21 - m12) / s;
    q.x = 0.25 * s;
    q.y = (m01 + m10) / s;
    q.z = (m02 + m20) / s;
  } else if (m11 > m22) {
    const double s = std::sqrt(std::max(1e-12, 1.0 + m11 - m00 - m22)) * 2.0;
    q.w = (m02 - m20) / s;
    q.x = (m01 + m10) / s;
    q.y = 0.25 * s;
    q.z = (m12 + m21) / s;
  } else {
    const double s = std::sqrt(std::max(1e-12, 1.0 + m22 - m00 - m11)) * 2.0;
    q.w = (m10 - m01) / s;
    q.x = (m02 + m20) / s;
    q.y = (m12 + m21) / s;
    q.z = 0.25 * s;
  }
  return quat_normalize(q);
}

bool decode_rmb_to_debris_model(const fh1::RenderModel& model, uint32_t member, const fh1::PvsObjectTable& table,
                                DebrisModel& out) {
  out = DebrisModel{};
  out.box_min = {1e30f, 1e30f, 1e30f};
  out.box_max = {-1e30f, -1e30f, -1e30f};
  const std::vector<uint32_t>* texture_list = nullptr;
  if (member >= 1 && member - 1 < table.model_textures.size()) {
    texture_list = &table.model_textures[member - 1];
  }

  std::unordered_map<uint64_t, size_t> submesh_by_key;
  size_t submesh_number = 0;
  for (const auto& o : model.objects) {
    const size_t first_submesh = submesh_number;
    submesh_number += o.submeshes.size();
    if (skipped_object(o.name)) continue;
    for (size_t si = 0; si < o.submeshes.size(); ++si) {
      const auto& s = o.submeshes[si];
      if (skipped_material(s.material)) continue;
      uint32_t albedo_rec = kNoTexture;
      uint8_t cutout = 0;
      uint8_t decal = 0;
      fh1::RmbVertexLayout layout;
      std::array<float, 4> uv_scale{0.0f, 0.0f, 1.0f, 1.0f};
      if (s.material_index < model.materials.size()) {
        const fh1::RmbMaterial& mat = model.materials[s.material_index];
        const std::string& eff = mat.effect < model.effects.size() ? model.effects[mat.effect] : std::string{};
        if (eff == "UNDERGROUND") continue;
        bool is_placeholder = false;
        const int32_t slot0 = !mat.slots.empty() ? mat.slots[0] : -2;
        if (slot0 >= 0 && texture_list && size_t(slot0) < texture_list->size()) {
          const uint32_t rec = (*texture_list)[size_t(slot0)];
          if (rec < table.textures.size()) {
            if (table.textures[rec].id == kPlaceholderTextureId) {
              is_placeholder = true;
            } else {
              albedo_rec = rec;
            }
          }
        }
        if (is_placeholder) continue;
        // Fallback: some multi-slot prop materials omit slot 0 (-1) and bind diffuse in a secondary slot.
        if (albedo_rec == kNoTexture && texture_list) {
          for (size_t k = 1; k < mat.slots.size(); ++k) {
            const int32_t sk = mat.slots[k];
            if (sk < 0 || size_t(sk) >= texture_list->size()) continue;
            const uint32_t rec = (*texture_list)[size_t(sk)];
            if (rec < table.textures.size() && table.textures[rec].id != kPlaceholderTextureId) {
              albedo_rec = rec;
              break;
            }
          }
        }
        cutout = (effect_blend(eff) == Blend::kCoverage) ? 1u : 0u;
        decal = (eff.find("DEPTHBIAS") != std::string::npos || eff.find("DECAL") != std::string::npos) ? 1u : 0u;
        layout = fh1::rmb_vertex_layout(eff, o.stride);
        if (first_submesh + si < model.uv_offset_scale.size()) {
          uv_scale = model.uv_offset_scale[first_submesh + si];
        }
      }
      const auto tri = fh1::rmb_triangle_list(s);
      if (tri.size() < 3) continue;

      const uint64_t sm_key = (uint64_t(decal) << 40) | (uint64_t(cutout) << 32) | uint64_t(albedo_rec);
      auto [it, added] = submesh_by_key.emplace(sm_key, out.submeshes.size());
      if (added) {
        DebrisSubmesh ds;
        ds.texture_record = albedo_rec;
        ds.cutout = cutout;
        ds.decal = decal;
        out.submeshes.push_back(std::move(ds));
      }
      auto& dst = out.submeshes[it->second].tris;
      dst.reserve(dst.size() + tri.size());

      auto fetch = [&](uint32_t vi) -> DebrisModelVertex {
        DebrisModelVertex dv;
        const auto p = o.position(vi);
        // File frame -> local game frame (Z negated).
        dv.pos = {p[0], p[1], -p[2]};
        for (int k = 0; k < 3; ++k) {
          out.box_min[size_t(k)] = std::min(out.box_min[size_t(k)], dv.pos[size_t(k)]);
          out.box_max[size_t(k)] = std::max(out.box_max[size_t(k)], dv.pos[size_t(k)]);
        }
        const uint8_t* vptr = &o.vertices[size_t(vi) * o.stride];
        if (layout.normal >= 0) {
          const auto n = fh1::rmb_normal(vptr + layout.normal);
          const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
          if (len > 1e-4f) dv.normal = {n[0] / len, n[1] / len, -n[2] / len};
          else dv.normal = {0.0f, 1.0f, 0.0f};
        } else {
          dv.normal = {0.0f, 1.0f, 0.0f};
        }
        if (layout.texcoord[0] >= 0) {
          const auto t = fh1::rmb_texcoord(vptr + layout.texcoord[0]);
          dv.uv = {uv_scale[0] + uv_scale[2] * t[0], uv_scale[1] + uv_scale[3] * t[1]};
        }
        dv.rgba = layout.colour >= 0 ? fh1::rmb_colour(vptr + layout.colour)
                                     : std::array<uint8_t, 4>{255, 255, 255, 255};
        return dv;
      };

      for (size_t t = 0; t + 2 < tri.size(); t += 3) {
        DebrisModelVertex v0 = fetch(tri[t]);
        DebrisModelVertex v1 = fetch(tri[t + 1]);
        DebrisModelVertex v2 = fetch(tri[t + 2]);
        if (layout.normal < 0) {
          const float e1[3] = {v1.pos[0] - v0.pos[0], v1.pos[1] - v0.pos[1], v1.pos[2] - v0.pos[2]};
          const float e2[3] = {v2.pos[0] - v0.pos[0], v2.pos[1] - v0.pos[1], v2.pos[2] - v0.pos[2]};
          const float nx = e1[1] * e2[2] - e1[2] * e2[1];
          const float ny = e1[2] * e2[0] - e1[0] * e2[2];
          const float nz = e1[0] * e2[1] - e1[1] * e2[0];
          const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
          if (l > 1e-6f) v0.normal = v1.normal = v2.normal = {nx / l, ny / l, nz / l};
        }
        if (decal != 0) {
          // Offset coplanar DEPTHBIAS/DECAL sign-face meshes slightly outward along their
          // normal so painted sign faces never z-fight with the backing plate in debris draws.
          constexpr float kDecalLiftM = 0.003f;
          for (DebrisModelVertex* dv : {&v0, &v1, &v2}) {
            dv->pos[0] += dv->normal[0] * kDecalLiftM;
            dv->pos[1] += dv->normal[1] * kDecalLiftM;
            dv->pos[2] += dv->normal[2] * kDecalLiftM;
          }
        }
        dst.push_back(v0);
        dst.push_back(v1);
        dst.push_back(v2);
      }
    }
  }
  if (!out.valid()) {
    out.box_min = out.box_max = {0, 0, 0};
    return false;
  }
  return true;
}

int64_t spatial_key_1m(float x, float y, float z) {
  const int64_t ix = int64_t(std::floor(x));
  const int64_t iy = int64_t(std::floor(y * 0.5f));
  const int64_t iz = int64_t(std::floor(z));
  return (ix * 73856093ll) ^ (iy * 19349663ll) ^ (iz * 83492791ll);
}

int part_index_of(std::string_view physics_type) {
  const size_t d1 = physics_type.find('.');
  if (d1 == std::string_view::npos) return 0;
  const size_t d2 = physics_type.find('.', d1 + 1);
  const auto mid = physics_type.substr(d1 + 1, d2 == std::string_view::npos ? std::string_view::npos : d2 - d1 - 1);
  if (mid.empty() || !std::isdigit(static_cast<unsigned char>(mid[0]))) return 0;
  return std::atoi(std::string(mid).c_str());
}

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

// Fast header-only peek of a local-space .rmb.bin model: reads its header box
// and first mesh object's name without decoding vertex or index buffers.
bool peek_rmb_barrier_info(std::span<const uint8_t> bytes, std::array<float, 3>& box_min,
                           std::array<float, 3>& box_max, std::string& first_name) {
  if (bytes.size() < 180) return false;
  try {
    fh1::BeReader r(bytes);
    if (r.u32() != 6u) return false;  // RenderModel kind == 6
    box_min = {r.f32(), r.f32(), r.f32()};
    r.skip(4);
    box_max = {r.f32(), r.f32(), r.f32()};
    r.skip(4);
    r.skip(16 * 4 + 4 * 4);  // identity matrix + 4 header words
    const uint32_t obj_count = r.u32();
    if (obj_count == 0 || obj_count > 4096) return false;
    if (r.u32() != 1u) return false;  // object tag
    r.skip(48);                       // centre, -half_extent, half_extent
    if (r.u32() != 1u) return false;  // object count word
    const uint32_t nlen = r.u32();
    if (nlen == 0 || nlen > 512 || nlen > r.remaining()) return false;
    const auto raw = r.raw(nlen);
    first_name.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
    while (!first_name.empty() && first_name.back() == '\0') first_name.pop_back();
    return !first_name.empty();
  } catch (...) {
    return false;
  }
}

// Resolves a 2D XZ Separating Axis Theorem (SAT) overlap between the vehicle OBB
// and a solid unbreakable barrier OBB with a strictly horizontal push-out and
// damped guardrail-style impulse (never injects upward velocity / vertical launch).
bool resolve_car_obb_vs_solid_obb(drive::Vehicle& vehicle, const drive::Vec3& cpos, const drive::Vec3& cfwd,
                                  const drive::Vec3& cright, float half_l, float half_w,
                                  const std::array<float, 3>& bw, const std::array<float, 3>& ox,
                                  const std::array<float, 3>& oy, const std::array<float, 3>& oz, float hx, float hy,
                                  float hz) {
  const float b_hy = std::fabs(ox[1]) * hx + std::fabs(oy[1]) * hy + std::fabs(oz[1]) * hz;
  if (bw[1] + b_hy < float(cpos.y) - 0.35f || bw[1] - b_hy > float(cpos.y) + 1.75f) return false;

  const float dx = float(cpos.x) - bw[0];
  const float dz = float(cpos.z) - bw[2];
  const float fx = float(cfwd.x), fz = float(cfwd.z);
  const float rx = float(cright.x), rz = float(cright.z);

  float min_pen = 1e9f;
  float mtv_x = 0.0f, mtv_z = 0.0f;
  bool found_axis = false;

  auto test_axis = [&](float ax, float az) -> bool {
    const float l = std::hypot(ax, az);
    if (l < 1e-4f) return true;
    const float ux = ax / l, uz = az / l;
    const float signed_d = dx * ux + dz * uz;
    const float r_car = half_w * std::fabs(rx * ux + rz * uz) + half_l * std::fabs(fx * ux + fz * uz);
    const float r_obb = hx * std::fabs(ox[0] * ux + ox[2] * uz) + hy * std::fabs(oy[0] * ux + oy[2] * uz) +
                        hz * std::fabs(oz[0] * ux + oz[2] * uz);
    const float overlap = (r_car + r_obb) - std::fabs(signed_d);
    if (overlap <= 0.0f) return false;
    if (overlap < min_pen) {
      min_pen = overlap;
      found_axis = true;
      if (signed_d >= 0.0f) {
        mtv_x = ux;
        mtv_z = uz;
      } else {
        mtv_x = -ux;
        mtv_z = -uz;
      }
    }
    return true;
  };

  if (!test_axis(rx, rz) || !test_axis(fx, fz) || !test_axis(ox[0], ox[2]) || !test_axis(oz[0], oz[2])) {
    return false;
  }
  if (!found_axis) return false;

  const drive::Vec3 n{double(mtv_x), 0.0, double(mtv_z)};
  const double push = std::clamp(double(min_pen) + 0.015, 0.01, 0.65);
  vehicle.translate(n * push);

  const drive::Vec3 v = vehicle.velocity();
  const double vn = v.x * n.x + v.z * n.z;
  if (vn < 0.0) {
    const double mass = std::max(500.0, vehicle.params().mass);
    const double jn = -(1.08 * vn) * mass;
    const drive::Vec3 t{-n.z, 0.0, n.x};
    const double vt = v.x * t.x + v.z * t.z;
    const double jt = std::clamp(-vt * mass * 0.18, -0.22 * jn, 0.22 * jn);
    vehicle.apply_impulse(vehicle.centre_of_mass(), n * jn + t * jt, 0.04);
  }
  return true;
}


// A body sphere against a fixed box (centre, unit axes, half extents) or
// sphere: the push out along the contact normal (FEEL-21).
struct ContactBox {
  drive::Vec3 c, u[3];
  double h[3];
};

bool sphere_vs_box(const drive::Vec3& s, double r, const ContactBox& b, drive::SphereHit& out) {
  const drive::Vec3 d = s - b.c;
  double l[3];
  bool inside = true;
  drive::Vec3 q = b.c;
  for (int k = 0; k < 3; ++k) {
    l[k] = drive::dot(d, b.u[k]);
    const double lc = std::clamp(l[k], -b.h[k], b.h[k]);
    if (lc != l[k]) inside = false;
    q = q + b.u[k] * lc;
  }
  if (!inside) {
    const drive::Vec3 n = s - q;
    const double dist = std::sqrt(drive::dot(n, n));
    if (dist >= r || dist < 1e-9) return false;
    out.normal = n * (1.0 / dist);
    out.depth = r - dist;
    out.point = q;
    return true;
  }
  // The centre is inside: out through the nearest face.
  int k_best = 0;
  double gap_best = 1e30;
  for (int k = 0; k < 3; ++k) {
    const double gap = b.h[k] - std::fabs(l[k]);
    if (gap < gap_best) gap_best = gap, k_best = k;
  }
  out.normal = b.u[k_best] * (l[k_best] < 0 ? -1.0 : 1.0);
  out.depth = r + gap_best;
  out.point = s + out.normal * gap_best;
  return true;
}

bool sphere_vs_sphere(const drive::Vec3& s, double r, const drive::Vec3& c, double rc, drive::SphereHit& out) {
  const drive::Vec3 d = s - c;
  const double dist = std::sqrt(drive::dot(d, d));
  if (dist >= r + rc) return false;
  out.normal = dist > 1e-9 ? d * (1.0 / dist) : drive::Vec3{0, 1, 0};
  out.depth = r + rc - dist;
  out.point = c + out.normal * rc;
  return true;
}

drive::SurfaceTyre contact_tyre(const CategoryCollisionParams* cp, double e, double mu, double low) {
  drive::SurfaceTyre t;
  t.coll_elasticity = cp && cp->has_restitution ? double(cp->restitution) : e;
  t.coll_friction = cp && cp->has_friction ? double(cp->friction) : mu;
  t.coll_low_speed_friction = cp ? (cp->has_low_speed_friction ? double(cp->low_speed_friction) : -1.0) : low;
  if (cp) {
    t.coll_noise_freq = cp->noise_freq;
    t.coll_noise_in0 = cp->noise_in0;
    t.coll_noise_in1 = cp->noise_in1;
    t.coll_noise_add0 = cp->noise_add0;
    t.coll_noise_add1 = cp->noise_add1;
  }
  return t;
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

bool SmashableWorld::is_solid_barrier_model(std::string_view object_name, const std::array<float, 3>& box_min,
                                            const std::array<float, 3>& box_max) {
  if (object_name.empty()) return false;
  // Reject world-space or invalid/degenerate bounding boxes.
  for (int k = 0; k < 3; ++k) {
    if (!std::isfinite(box_min[size_t(k)]) || !std::isfinite(box_max[size_t(k)])) return false;
    if (std::fabs(box_min[size_t(k)]) > 200.0f || std::fabs(box_max[size_t(k)]) > 200.0f) return false;
    if (box_max[size_t(k)] < box_min[size_t(k)]) return false;
  }
  const float ex = box_max[0] - box_min[0];
  const float ey = box_max[1] - box_min[1];
  const float ez = box_max[2] - box_min[2];
  const float max_xz = std::max(ex, ez);
  const float min_xz = std::min(ex, ez);
  // Barrier wall proportions: upright panel (0.5..3.0 m tall), 0.9..14 m long,
  // <= 2.2 m thick, elongated rather than a square crate/bin/pole.
  if (ey < 0.50f || ey > 3.0f) return false;
  if (max_xz < 0.90f || max_xz > 14.0f) return false;
  if (min_xz > 2.2f) return false;
  if (max_xz / std::max(min_xz, 0.05f) < 1.35f) return false;

  const std::string low = to_lower(object_name);
  auto has = [&](std::string_view needle) { return low.find(needle) != std::string::npos; };

  // Never classify smashable wooden/plastic/ranch fences or decorative props as solid barriers.
  if (has("wood") || has("wooden") || has("plastic") || has("cone") || has("flag") || has("arch") ||
      has("gantry") || has("tent") || has("stage") || has("grandstand") || has("bleacher") ||
      has("trailer") || has("truck") || has("tree") || has("bush") || has("lamp") || has("light") ||
      has("pole") || has("temp_") || has("arrow") || has("trigger") || has("marker") || has("ranch") ||
      has("picket") || has("farm") || has("wire") || has("tape") || has("ribbon") || has("spectator") ||
      has("sign") || has("flyer") || has("discount")) {
    return false;
  }

  return has("barrier") || has("barricade") || has("crowdfence") || has("crowd_fence") ||
         has("crowdrail") || has("crowd_rail") || has("chainlink") || has("chain_link") ||
         has("metalfence") || has("metal_fence") || has("fest_fence") || has("festival_fence") ||
         has("guardrail") || has("guard_rail") || has("safety_fence") || has("security_fence");
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
  if (object_type == 1 || stem.find("flyer") != std::string_view::npos ||
      stem.find("discount") != std::string_view::npos) {
    // Horizon discount sign: warm gold/crimson board shards.
    return {212, 162, 28, 255};
  }
  if (stem.find("stop") != std::string_view::npos || stem.find("hydrant") != std::string_view::npos ||
      stem.find("noentry") != std::string_view::npos) {
    return {168, 30, 34, 255};  // traffic-sign crimson red
  }
  if (surface_type == 64 || stem.find("cone") != std::string_view::npos ||
      stem.find("bollard") != std::string_view::npos) {
    return {208, 84, 20, 255};  // road cone / construction orange
  }
  if (stem.find("sign") != std::string_view::npos || stem.find("milepost") != std::string_view::npos) {
    return {178, 138, 36, 255};  // Colorado highway sign ochre/amber
  }
  if (stem.find("wood") != std::string_view::npos || stem.find("fence") != std::string_view::npos ||
      stem.find("rail") != std::string_view::npos || stem.find("bench") != std::string_view::npos ||
      stem.find("crate") != std::string_view::npos || stem.find("table") != std::string_view::npos ||
      surface_type == 60 || surface_type == 15 || object_type == 0) {
    return {118, 84, 52, 255};  // weathered Colorado cedar/pine wood
  }
  if (stem.find("pole") != std::string_view::npos || stem.find("lamp") != std::string_view::npos ||
      stem.find("light") != std::string_view::npos || stem.find("post") != std::string_view::npos ||
      stem.find("mail") != std::string_view::npos || stem.find("metal") != std::string_view::npos ||
      surface_type == 61 || surface_type == 66 || surface_type == 67 || object_type == 2) {
    return {76, 82, 88, 255};  // weathered dark galvanized steel / iron pole (never bright white)
  }
  if (surface_type == 63 || surface_type == 65) {
    return {96, 102, 108, 255};  // matte charcoal/slate plastic (never blown-out white)
  }
  return {116, 94, 68, 255};  // earthy weathered timber/metal fallback
}

struct SmashableWorld::Impl {
  fh1::MemberSource source;
  fh1::PvsObjectTable table;
  std::string prefix = "coloradoout";
  std::unordered_map<uint32_t, DebrisModel> debris_models;
  mutable std::mutex tex_mutex;
  bool small_read = false;
  struct SmallTex {
    uint32_t width = 0, height = 0, levels = 0, format_word = 0;
    std::vector<uint8_t> payload;
  };
  mutable std::unordered_map<uint32_t, SmallTex> small_textures;
  bool source_ready = false;

  void ensure_small_textures_locked() const {
    if (small_read || !source_ready) return;
    const_cast<Impl*>(this)->small_read = true;
    const auto names = const_cast<fh1::MemberSource&>(source).with_suffix(".bundle");
    std::vector<uint8_t> buf;
    std::vector<fh1::BundleEntry> entries;
    for (const auto& name : names) {
      if (const_cast<fh1::MemberSource&>(source).read(name, buf, nullptr) &&
          fh1::parse_bundle(buf, entries, nullptr)) {
        for (const auto& e : entries) {
          if (!small_textures.count(e.record)) {
            small_textures[e.record] = {e.width, e.height, e.levels, e.format_word,
                                        {e.payload.begin(), e.payload.end()}};
          }
        }
      }
    }
  }
};

SmashableWorld::SmashableWorld() : impl_(std::make_unique<Impl>()) { set_contact_params(nullptr, nullptr); }

void SmashableWorld::set_contact_params(const CategoryCollisionParams* prop, const CategoryCollisionParams* barrier) {
  // Shipped rows: Smashable/Car mu 0.5, Elasticity 0.3; Car/GuardRail mu
  // 0.5, low-speed 0.1, Elasticity -0.9.
  prop_contact_ = contact_tyre(prop, 0.3, 0.5, -1.0);
  barrier_contact_ = contact_tyre(barrier, -0.9, 0.5, 0.1);
}

void SmashableWorld::add_sphere_contacts(const drive::Vec3& inside, double car_speed_mps,
                                         const drive::SphereQuery* spheres, int n, drive::SphereHit* hits) const {
  if (n <= 0) return;
  thread_local std::vector<ContactBox> boxes;
  thread_local std::vector<int> box_kind;  // 0 prop, 1 barrier
  thread_local std::vector<std::array<double, 5>> balls;  // centre, radius, kind
  boxes.clear();
  box_kind.clear();
  balls.clear();
  const float car_speed = float(car_speed_mps);
  const int32_t cx_cell = int32_t(std::floor(float(inside.x) / 25.0f));
  const int32_t cz_cell = int32_t(std::floor(float(inside.z) / 25.0f));
  auto vec = [](const std::array<float, 3>& a) { return drive::Vec3{a[0], a[1], a[2]}; };
  for (int32_t dz = -1; dz <= 1; ++dz) {
    for (int32_t dx = -1; dx <= 1; ++dx) {
      const int64_t key = (int64_t(cx_cell + dx) << 32) ^ uint32_t(cz_cell + dz);
      if (auto it = grid_.find(key); it != grid_.end()) {
        for (uint32_t idx : it->second) {
          const auto& pp = props_[idx];
          if (pp.broken || !pp.def) continue;
          if (active_route_ >= 0 && !fh1::shown_on_route(pp.routes, active_route_)) continue;
          if (should_break(*pp.def, car_speed)) continue;  // step() breaks it instead
          const drive::Vec3 pos = vec(pp.place.pos);
          const drive::Vec3 rel = pos - inside;
          const double reach = 6.0 + std::max(pp.radius, pp.def->bounding_radius);
          if (drive::dot(rel, rel) > reach * reach) continue;
          const drive::Vec3 ax = vec(pp.place.x_axis), ay = vec(pp.place.y_axis), az = vec(pp.place.z_axis);
          auto to_world = [&](double lx, double ly, double lz) { return pos + ax * lx + ay * ly + az * lz; };
          auto dir_world = [&](double lx, double ly, double lz) {
            const drive::Vec3 v = ax * lx + ay * ly + az * lz;
            const double l = std::sqrt(drive::dot(v, v));
            return l > 1e-9 ? v * (1.0 / l) : drive::Vec3{1, 0, 0};
          };
          const auto& def = *pp.def;
          if (def.shapes.empty()) {
            ContactBox b;
            b.c = to_world(def.aabb_centre_offset[0], def.aabb_centre_offset[1], def.aabb_centre_offset[2]);
            b.u[0] = ax, b.u[1] = ay, b.u[2] = az;
            for (int k = 0; k < 3; ++k) b.h[k] = std::max(0.05f, def.half_extents[size_t(k)]);
            boxes.push_back(b);
            box_kind.push_back(0);
            continue;
          }
          for (const auto& sh : def.shapes) {
            const auto& m = sh.transform;
            if (sh.kind == PropShapeKind::kSphere) {
              const drive::Vec3 sc = to_world(sh.sphere[0], sh.sphere[1], sh.sphere[2]);
              balls.push_back({sc.x, sc.y, sc.z, std::max(0.02, double(sh.sphere[3])), 0});
            } else if (sh.kind == PropShapeKind::kObb) {
              ContactBox b;
              b.c = to_world(m[12], m[13], m[14]);
              b.u[0] = dir_world(m[0], m[1], m[2]);
              b.u[1] = dir_world(m[4], m[5], m[6]);
              b.u[2] = dir_world(m[8], m[9], m[10]);
              for (int k = 0; k < 3; ++k) b.h[k] = std::max(0.02f, sh.half_extents[size_t(k)]);
              boxes.push_back(b);
              box_kind.push_back(0);
            } else if (sh.kind == PropShapeKind::kConvexHull && !sh.hull_verts.empty()) {
              // Interim (Q106): a hull meets the car as its box in the prop's frame.
              double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
              for (const auto& hv : sh.hull_verts) {
                const double l[3] = {m[0] * hv[0] + m[4] * hv[1] + m[8] * hv[2] + m[12],
                                     m[1] * hv[0] + m[5] * hv[1] + m[9] * hv[2] + m[13],
                                     m[2] * hv[0] + m[6] * hv[1] + m[10] * hv[2] + m[14]};
                for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], l[k]), hi[k] = std::max(hi[k], l[k]);
              }
              ContactBox b;
              b.c = to_world(0.5 * (lo[0] + hi[0]), 0.5 * (lo[1] + hi[1]), 0.5 * (lo[2] + hi[2]));
              b.u[0] = ax, b.u[1] = ay, b.u[2] = az;
              for (int k = 0; k < 3; ++k) b.h[k] = std::max(0.02, 0.5 * (hi[k] - lo[k]));
              boxes.push_back(b);
              box_kind.push_back(0);
            }
          }
        }
      }
      if (auto it = barrier_grid_.find(key); it != barrier_grid_.end()) {
        for (uint32_t idx : it->second) {
          const auto& sb = solid_barriers_[idx];
          if (active_route_ >= 0 && !fh1::shown_on_route(sb.routes, active_route_)) continue;
          const drive::Vec3 pos = vec(sb.pos);
          const drive::Vec3 rel = pos - inside;
          const double reach = 6.0 + sb.radius;
          if (drive::dot(rel, rel) > reach * reach) continue;
          ContactBox b;
          b.u[0] = vec(sb.x_axis), b.u[1] = vec(sb.y_axis), b.u[2] = vec(sb.z_axis);
          b.c = pos + b.u[0] * sb.local_center[0] + b.u[1] * sb.local_center[1] + b.u[2] * sb.local_center[2];
          for (int k = 0; k < 3; ++k) b.h[k] = sb.half_extents[size_t(k)];
          // A barrier cell can list it more than once; one box is enough.
          bool dup = false;
          for (size_t j = 0; j < boxes.size() && !dup; ++j)
            dup = box_kind[j] == 1 && drive::dot(boxes[j].c - b.c, boxes[j].c - b.c) < 1e-8;
          if (dup) continue;
          boxes.push_back(b);
          box_kind.push_back(1);
        }
      }
    }
  }
  if (boxes.empty() && balls.empty()) return;
  for (int i = 0; i < n; ++i) {
    const drive::Vec3& sc = spheres[i].centre;
    const double r = spheres[i].radius;
    for (size_t j = 0; j < boxes.size(); ++j) {
      drive::SphereHit h;
      if (!sphere_vs_box(sc, r, boxes[j], h) || h.depth <= hits[i].depth) continue;
      h.tyre = box_kind[j] == 1 ? barrier_contact_ : prop_contact_;
      hits[i] = h;
    }
    for (const auto& bl : balls) {
      drive::SphereHit h;
      if (!sphere_vs_sphere(sc, r, {bl[0], bl[1], bl[2]}, bl[3], h) || h.depth <= hits[i].depth) continue;
      h.tyre = prop_contact_;
      hits[i] = h;
    }
  }
}
SmashableWorld::~SmashableWorld() = default;
SmashableWorld::SmashableWorld(SmashableWorld&&) noexcept = default;
SmashableWorld& SmashableWorld::operator=(SmashableWorld&&) noexcept = default;

size_t SmashableWorld::loaded_debris_model_count() const {
  return impl_ ? impl_->debris_models.size() : 0;
}

size_t SmashableWorld::matched_global_index_count() const {
  size_t n = 0;
  for (const auto& pp : props_)
    if (pp.global_index >= 0) ++n;
  return n;
}

bool SmashableWorld::read_texture(uint32_t record, fh1::WorldTexture& out, std::string* error) const {
  if (!impl_ || !impl_->source_ready || record >= impl_->table.textures.size()) {
    if (error) *error = "smashable texture record unavailable";
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->tex_mutex);
  const fh1::PvsTexture& t = impl_->table.textures[record];
  char name[32];
  std::vector<uint8_t> a, b;
  bool exact = false, ok = false;
  auto& src = const_cast<fh1::MemberSource&>(impl_->source);
  if (t.type == fh1::kPvsTextureBix) {
    std::snprintf(name, sizeof name, "_0x%08x.bix", t.id);
    std::string base = name;
    base.insert(base.size() - 4, "_b");
    ok = src.read(name, a, error) && src.read(base, b, error) && fh1::decode_bix(a, b, out, error, &exact);
  } else if (t.type == fh1::kPvsTextureCaff) {
    std::snprintf(name, sizeof name, "_0x%08x.bin", t.id);
    ok = src.read(name, a, error) && fh1::decode_caff(a, out, error, &exact);
  } else {
    impl_->ensure_small_textures_locked();
    const auto it = impl_->small_textures.find(record);
    if (it == impl_->small_textures.end()) {
      if (error) *error = "smashable bundle texture missing";
      return false;
    }
    const auto& s = it->second;
    return fh1::decode_bundle_entry({record, s.width, s.height, s.levels, s.format_word, s.payload}, out, error);
  }
  return ok && exact;
}

bool SmashableWorld::load(const std::filesystem::path& media_dir, const std::filesystem::path& world_dir,
                          std::string* error) {
  defs_ = PhysicsDefinitions{};
  coll_types_.clear();
  smash_types_.clear();
  props_.clear();
  raw_xml_prop_count_ = 0;
  grid_.clear();
  solid_barriers_.clear();
  barrier_grid_.clear();
  dynamic_bodies_.clear();
  broken_positions_.reset();
  evictions_ = 0;
  impl_ = std::make_unique<Impl>();

  std::vector<CollObjPlacement> raw_placements;
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
      parse_coll_objs_xml(read_text_file(co_path), raw_placements);
      raw_xml_prop_count_ = raw_placements.size();
    }
  }

  // Deduplicate CollObjs.xml stacked child rows (e.g. CO_Bench_001.0.rmb .. .12.rmb all share
  // the exact same world position): keep one parent prop per physical placement, preferring .0.rmb.
  struct DedupKey {
    std::string stem;
    int32_t qx, qy, qz;
    bool operator==(const DedupKey& o) const {
      return qx == o.qx && qy == o.qy && qz == o.qz && stem == o.stem;
    }
  };
  struct DedupHash {
    size_t operator()(const DedupKey& k) const {
      size_t h = std::hash<std::string>{}(k.stem);
      h ^= size_t(uint32_t(k.qx)) * 2654435761u + 0x9e3779b9u + (h << 6) + (h >> 2);
      h ^= size_t(uint32_t(k.qy)) * 2246822519u + 0x9e3779b9u + (h << 6) + (h >> 2);
      h ^= size_t(uint32_t(k.qz)) * 3266489917u + 0x9e3779b9u + (h << 6) + (h >> 2);
      return h;
    }
  };
  std::unordered_map<DedupKey, std::pair<size_t, int>, DedupHash> unique_xml;
  unique_xml.reserve(raw_placements.size() / 3 + 64);
  for (size_t i = 0; i < raw_placements.size(); ++i) {
    const auto& pl = raw_placements[i];
    const DedupKey dk{pl.stem, int32_t(std::lround(pl.pos[0] * 4.0f)), int32_t(std::lround(pl.pos[1] * 4.0f)),
                      int32_t(std::lround(pl.pos[2] * 4.0f))};
    const int part = part_index_of(pl.physics_type);
    auto it = unique_xml.find(dk);
    if (it == unique_xml.end()) {
      unique_xml.emplace(dk, std::make_pair(i, part));
    } else if (part < it->second.second) {
      it->second = {i, part};
    }
  }
  std::vector<size_t> parent_indices;
  parent_indices.reserve(unique_xml.size());
  for (const auto& [k, v] : unique_xml) parent_indices.push_back(v.first);
  std::sort(parent_indices.begin(), parent_indices.end());
  props_.reserve(parent_indices.size());
  for (size_t idx : parent_indices) add_prop(raw_placements[idx]);

  // Open bin.zip + Ribbon_00/<world>_00.pvs to resolve exact FH1 GlobalIndex from .pvsz records
  // (PvszRecord::word == GlobalIndex 0..MaxGlobalIndex-1) and pre-decode real .rmb.bin debris meshes.
  if (!world_dir.empty() && !defs_.definitions.empty()) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path zip = world_dir / "bin.zip";
    fs::path dlc_zips;
    if (!fs::exists(zip, ec)) {
      const fs::path alt1 = world_dir / "ColoradoDirt_pri_65.zip";
      const fs::path parent_zips = world_dir.parent_path().parent_path().parent_path().parent_path();
      const fs::path alt2 = parent_zips / "ColoradoDirt_pri_65.zip";
      if (fs::exists(alt1, ec)) {
        zip = alt1;
        dlc_zips = world_dir;
      } else if (fs::exists(alt2, ec)) {
        zip = alt2;
        dlc_zips = parent_zips;
      }
    }
    std::vector<uint8_t> pvs_bytes;
    const fs::path pvs_path = ribbon_file(world_dir, "_00.pvs");
    if (std::ifstream pf(pvs_path, std::ios::binary); pf) {
      pvs_bytes.assign((std::istreambuf_iterator<char>(pf)), {});
    } else if (!dlc_zips.empty()) {
      std::string zwhy;
      if (auto pkg = fh1::Archive::open(dlc_zips / "1600_pri_65.zip", &zwhy)) {
        auto r = pkg->read("media/Tracks/ColoradoDirt/Ribbon_00/ColoradoDirt_00.pvs");
        if (r.ok()) pvs_bytes = std::move(r.data);
      }
    }

    std::string m_err;
    if (!pvs_bytes.empty() && fs::exists(zip, ec) && impl_->source.open(zip, &m_err) &&
        fh1::parse_object_table(pvs_bytes, impl_->table, &m_err)) {
      impl_->source_ready = true;
      impl_->prefix = model_prefix(world_dir);

      // 1. Scan all .pvsz zones in parallel for smashable placements (extended && word < MaxGlobalIndex)
      //    and static placed props (potential festival crowd barriers / barricades).
      const auto pvsz_names = impl_->source.with_suffix(".pvsz");
      struct PvszSmash {
        uint32_t object_id = 0;
        uint32_t global_index = 0;
        int32_t intact_member = -1;
        std::array<float, 3> pos{};
        std::array<float, 3> x_axis{1, 0, 0};
        std::array<float, 3> y_axis{0, 1, 0};
        std::array<float, 3> z_axis{0, 0, 1};
        std::vector<uint8_t> routes;
        std::string name;
      };
      struct PvszStatic {
        uint32_t object_id = 0;
        uint32_t member = 0;
        std::array<float, 3> pos{};
        std::array<float, 3> x_axis{1, 0, 0};
        std::array<float, 3> y_axis{0, 1, 0};
        std::array<float, 3> z_axis{0, 0, 1};
        std::vector<uint8_t> routes;
        std::string name;
      };
      const uint32_t max_g = uint32_t(defs_.global_to_list.size());
      std::vector<std::vector<PvszSmash>> per_zone(pvsz_names.size());
      std::vector<std::vector<PvszStatic>> per_zone_static(pvsz_names.size());
      {
        const size_t n = pvsz_names.size();
        const size_t threads = std::min<size_t>(n, std::max<size_t>(1, std::thread::hardware_concurrency()));
        std::atomic<size_t> next{0};
        auto worker = [&]() {
          std::vector<uint8_t> bytes;
          for (;;) {
            const size_t zi = next.fetch_add(1, std::memory_order_relaxed);
            if (zi >= n) break;
            fh1::PvszZone zone;
            if (!impl_->source.read(pvsz_names[zi], bytes, nullptr) ||
                !fh1::parse_pvsz(bytes, zone, nullptr, fh1::PvszStartRule::kMostRigid))
              continue;
            auto& dst = per_zone[zi];
            auto& sdst = per_zone_static[zi];
            for (size_t k = 0; k < zone.ids.size() && k < zone.records.size(); ++k) {
              const auto& rec = zone.records[k];
              if (std::fabs(rec.position[0]) < 1e-3f && std::fabs(rec.position[1]) < 1e-3f &&
                  std::fabs(rec.position[2]) < 1e-3f)
                continue;
              const uint32_t oid = fh1::pvsz_object_id(zone.ids[k]);
              if (rec.extended && rec.word < max_g) {
                PvszSmash s;
                s.object_id = oid;
                s.global_index = rec.word;
                if (oid < impl_->table.objects.size()) s.intact_member = int32_t(impl_->table.objects[oid].member);
                s.pos = rec.position;
                // R_game = M * F (col0 = h0..2, col1 = h3..5, col2 = -h6..8 -> right-handed SO(3), det = +1).
                s.x_axis = {rec.rotation[0], rec.rotation[1], rec.rotation[2]};
                s.y_axis = {rec.rotation[3], rec.rotation[4], rec.rotation[5]};
                s.z_axis = {-rec.rotation[6], -rec.rotation[7], -rec.rotation[8]};
                s.routes = rec.extra;
                s.name = to_lower(rec.name);
                dst.push_back(std::move(s));
              } else if (oid < impl_->table.objects.size()) {
                const std::string low_name = rec.extended ? to_lower(rec.name) : std::string{};
                if (low_name.rfind("bf_", 0) == 0 || low_name.rfind("flyer_", 0) == 0 ||
                    low_name.rfind("speed_", 0) == 0 || low_name.rfind("average_", 0) == 0 ||
                    low_name.rfind("barnfind_", 0) == 0 || low_name.rfind("e3_", 0) == 0 ||
                    low_name.find("_node") != std::string::npos || low_name.find("_endnode") != std::string::npos) {
                  continue;
                }
                PvszStatic st;
                st.object_id = oid;
                st.member = impl_->table.objects[oid].member;
                st.pos = rec.position;
                st.x_axis = {rec.rotation[0], rec.rotation[1], rec.rotation[2]};
                st.y_axis = {rec.rotation[3], rec.rotation[4], rec.rotation[5]};
                st.z_axis = {-rec.rotation[6], -rec.rotation[7], -rec.rotation[8]};
                if (rec.extended) st.routes = rec.extra;
                st.name = low_name;
                sdst.push_back(std::move(st));
              }
            }
          }
        };
        if (threads > 1 && n >= 4) {
          std::vector<std::thread> pool;
          pool.reserve(threads);
          for (size_t t = 0; t < threads; ++t) pool.emplace_back(worker);
          for (auto& w : pool) w.join();
        } else {
          worker();
        }
      }

      // Deduplicate by object_id and build a 1 m spatial index for exact CollObjs.xml linking.
      std::unordered_map<uint32_t, PvszSmash> unique_pvsz;
      for (auto& zv : per_zone) {
        for (auto& s : zv) unique_pvsz.try_emplace(s.object_id, std::move(s));
      }
      per_zone.clear();

      std::vector<const PvszSmash*> pvsz_list;
      pvsz_list.reserve(unique_pvsz.size());
      std::unordered_map<int64_t, std::vector<size_t>> pvsz_grid;
      for (const auto& [oid, s] : unique_pvsz) {
        const size_t si = pvsz_list.size();
        pvsz_list.push_back(&s);
        pvsz_grid[spatial_key_1m(s.pos[0], s.pos[1], s.pos[2])].push_back(si);
      }

      std::vector<uint8_t> pvsz_claimed(pvsz_list.size(), 0);
      std::unordered_map<std::string, int32_t> stem_to_global;
      std::unordered_map<std::string, int32_t> stem_to_intact;

      // Pass 1: exact positional link from CollObjs.xml -> .pvsz GlobalIndex.
      for (auto& pp : props_) {
        const int32_t ix0 = int32_t(std::floor(pp.place.pos[0]));
        const int32_t iy0 = int32_t(std::floor(pp.place.pos[1] * 0.5f));
        const int32_t iz0 = int32_t(std::floor(pp.place.pos[2]));
        size_t best_si = SIZE_MAX;
        float best_d2 = 0.65f * 0.65f;
        for (int32_t dz = -1; dz <= 1; ++dz) {
          for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
              const int64_t key =
                  (int64_t(ix0 + dx) * 73856093ll) ^ (int64_t(iy0 + dy) * 19349663ll) ^ (int64_t(iz0 + dz) * 83492791ll);
              auto it = pvsz_grid.find(key);
              if (it == pvsz_grid.end()) continue;
              for (size_t si : it->second) {
                const auto& ps = *pvsz_list[si];
                const float ddx = ps.pos[0] - pp.place.pos[0];
                const float ddy = ps.pos[1] - pp.place.pos[1];
                const float ddz = ps.pos[2] - pp.place.pos[2];
                const float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
                if (d2 < best_d2) {
                  best_d2 = d2;
                  best_si = si;
                }
              }
            }
          }
        }
        if (best_si != SIZE_MAX) {
          const auto& ps = *pvsz_list[best_si];
          pvsz_claimed[best_si] = 1;
          pp.global_index = int32_t(ps.global_index);
          pp.intact_member = ps.intact_member;
          pp.routes = ps.routes;
          const uint16_t list_idx = defs_.global_to_list[ps.global_index];
          if (list_idx < defs_.definitions.size()) {
            pp.def = &defs_.definitions[list_idx];
            pp.radius = std::clamp(pp.def->bounding_radius, 0.35f, 12.0f);
          }
          stem_to_global.try_emplace(pp.place.stem, pp.global_index);
          if (pp.intact_member >= 0) stem_to_intact.try_emplace(pp.place.stem, pp.intact_member);
        }
      }

      // Pass 2: propagate stem -> GlobalIndex to any CollObjs.xml entry not directly matched.
      for (auto& pp : props_) {
        if (pp.global_index >= 0) continue;
        if (const auto it = stem_to_global.find(pp.place.stem); it != stem_to_global.end()) {
          pp.global_index = it->second;
          const uint16_t list_idx = defs_.global_to_list[size_t(it->second)];
          if (list_idx < defs_.definitions.size()) {
            pp.def = &defs_.definitions[list_idx];
            pp.radius = std::clamp(pp.def->bounding_radius, 0.35f, 12.0f);
          }
          if (const auto mit = stem_to_intact.find(pp.place.stem); mit != stem_to_intact.end()) {
            pp.intact_member = mit->second;
          }
        }
      }

      // Pass 3: include unclaimed breakable .pvsz smashables not in CollObjs.xml, and register
      // unclaimed unbreakable solid barriers (excluding Flyer signs, BF_* barn doors, and RaceActivation arches).
      std::unordered_set<int64_t> extra_seen;
      std::unordered_set<int64_t> barrier_seen;
      for (size_t si = 0; si < pvsz_list.size(); ++si) {
        if (pvsz_claimed[si]) continue;
        const auto& ps = *pvsz_list[si];
        if (ps.name.rfind("bf_", 0) == 0 || ps.name.rfind("flyer_", 0) == 0) continue;
        const uint16_t list_idx = defs_.global_to_list[ps.global_index];
        if (list_idx >= defs_.definitions.size()) continue;
        const PropPhysicsDef& d = defs_.definitions[list_idx];
        if (d.object_type_index == 1 || d.object_type_index == 3) continue;
        const int64_t pos_key =
            (int64_t(std::lround(ps.pos[0] * 4.0f)) * 73856093ll) ^
            (int64_t(std::lround(ps.pos[1] * 4.0f)) * 19349663ll) ^
            (int64_t(std::lround(ps.pos[2] * 4.0f)) * 83492791ll);
        if (d.unbreakable()) {
          if (!barrier_seen.insert(pos_key).second) continue;
          bool added_shape = false;
          for (const auto& sh : d.shapes) {
            if (sh.kind != PropShapeKind::kObb) continue;
            const auto& m = sh.transform;
            SolidBarrier sb;
            sb.pos = {
                ps.pos[0] + ps.x_axis[0] * m[12] + ps.y_axis[0] * m[13] + ps.z_axis[0] * m[14],
                ps.pos[1] + ps.x_axis[1] * m[12] + ps.y_axis[1] * m[13] + ps.z_axis[1] * m[14],
                ps.pos[2] + ps.x_axis[2] * m[12] + ps.y_axis[2] * m[13] + ps.z_axis[2] * m[14],
            };
            sb.x_axis = {
                ps.x_axis[0] * m[0] + ps.y_axis[0] * m[1] + ps.z_axis[0] * m[2],
                ps.x_axis[1] * m[0] + ps.y_axis[1] * m[1] + ps.z_axis[1] * m[2],
                ps.x_axis[2] * m[0] + ps.y_axis[2] * m[1] + ps.z_axis[2] * m[2],
            };
            sb.y_axis = {
                ps.x_axis[0] * m[4] + ps.y_axis[0] * m[5] + ps.z_axis[0] * m[6],
                ps.x_axis[1] * m[4] + ps.y_axis[1] * m[5] + ps.z_axis[1] * m[6],
                ps.x_axis[2] * m[4] + ps.y_axis[2] * m[5] + ps.z_axis[2] * m[6],
            };
            sb.z_axis = {
                ps.x_axis[0] * m[8] + ps.y_axis[0] * m[9] + ps.z_axis[0] * m[10],
                ps.x_axis[1] * m[8] + ps.y_axis[1] * m[9] + ps.z_axis[1] * m[10],
                ps.x_axis[2] * m[8] + ps.y_axis[2] * m[9] + ps.z_axis[2] * m[10],
            };
            sb.local_center = {0.0f, 0.0f, 0.0f};
            sb.half_extents = {
                std::max(0.18f, sh.half_extents[0]),
                std::max(0.35f, sh.half_extents[1]),
                std::max(0.18f, sh.half_extents[2]),
            };
            sb.routes = ps.routes;
            add_solid_barrier(sb);
            added_shape = true;
          }
          if (!added_shape) {
            SolidBarrier sb;
            sb.pos = ps.pos;
            sb.x_axis = ps.x_axis;
            sb.y_axis = ps.y_axis;
            sb.z_axis = ps.z_axis;
            sb.local_center = d.aabb_centre_offset;
            sb.half_extents = {
                std::max(0.18f, d.half_extents[0]),
                std::max(0.35f, d.half_extents[1]),
                std::max(0.18f, d.half_extents[2]),
            };
            sb.routes = ps.routes;
            add_solid_barrier(sb);
          }
          continue;
        }
        if (!extra_seen.insert(pos_key).second) continue;

        PlacedProp pp;
        pp.place.pos = ps.pos;
        pp.place.x_axis = ps.x_axis;
        pp.place.y_axis = ps.y_axis;
        pp.place.z_axis = ps.z_axis;
        pp.place.stem = !ps.name.empty() ? ps.name : ("pvsz_g" + std::to_string(ps.global_index));
        pp.place.physics_type = pp.place.stem;
        pp.def = &d;
        pp.global_index = int32_t(ps.global_index);
        pp.intact_member = ps.intact_member;
        pp.routes = ps.routes;
        pp.radius = std::clamp(d.bounding_radius, 0.35f, 12.0f);
        const uint32_t idx = uint32_t(props_.size());
        props_.push_back(std::move(pp));
        grid_[cell_key(ps.pos[0], ps.pos[2])].push_back(idx);
      }

      // Pass 4: register static .pvsz solid festival crowd barriers / barricades / chain-link fences
      // whose render models are solid barriers not already covered by a breakable smashable prop.
      {
        std::unordered_map<uint32_t, PvszStatic> unique_static;
        for (auto& zv : per_zone_static) {
          for (auto& s : zv) unique_static.try_emplace(s.object_id, std::move(s));
        }
        per_zone_static.clear();

        std::unordered_set<uint32_t> static_members_set;
        for (const auto& [oid, s] : unique_static) static_members_set.insert(s.member);
        std::vector<uint32_t> static_members(static_members_set.begin(), static_members_set.end());
        std::sort(static_members.begin(), static_members.end());

        struct BarrierModelBox {
          std::array<float, 3> local_center{};
          std::array<float, 3> half_extents{};
          bool is_barrier = false;
        };
        std::vector<BarrierModelBox> scanned(static_members.size());
        {
          const size_t n = static_members.size();
          const size_t threads = std::min<size_t>(n, std::max<size_t>(1, std::thread::hardware_concurrency()));
          std::atomic<size_t> next{0};
          auto worker = [&]() {
            std::vector<uint8_t> buf;
            for (;;) {
              const size_t i = next.fetch_add(1, std::memory_order_relaxed);
              if (i >= n) break;
              const std::string mname = fh1::PropModels::member_name(static_members[i], impl_->prefix);
              if (!impl_->source.contains(mname) || !impl_->source.read(mname, buf, nullptr)) continue;
              std::array<float, 3> bmin{}, bmax{};
              std::string obj_name;
              if (!peek_rmb_barrier_info(buf, bmin, bmax, obj_name)) continue;
              if (!is_solid_barrier_model(obj_name, bmin, bmax)) continue;
              // Convert file-frame header box to local game frame (Z negated).
              const float min_gz = -bmax[2];
              const float max_gz = -bmin[2];
              scanned[i].local_center = {0.5f * (bmin[0] + bmax[0]), 0.5f * (bmin[1] + bmax[1]),
                                         0.5f * (min_gz + max_gz)};
              scanned[i].half_extents = {
                  std::max(0.18f, 0.5f * (bmax[0] - bmin[0])),
                  std::max(0.35f, 0.5f * (bmax[1] - bmin[1])),
                  std::max(0.18f, 0.5f * (max_gz - min_gz)),
              };
              scanned[i].is_barrier = true;
            }
          };
          if (threads > 1 && n >= 8) {
            std::vector<std::thread> pool;
            pool.reserve(threads);
            for (size_t t = 0; t < threads; ++t) pool.emplace_back(worker);
            for (auto& w : pool) w.join();
          } else {
            worker();
          }
        }
        std::unordered_map<uint32_t, BarrierModelBox> barrier_models;
        for (size_t i = 0; i < static_members.size(); ++i) {
          if (scanned[i].is_barrier) barrier_models.emplace(static_members[i], scanned[i]);
        }

        auto near_breakable_prop = [&](const std::array<float, 3>& p) -> bool {
          const int32_t cx_c = int32_t(std::floor(p[0] / 25.0f));
          const int32_t cz_c = int32_t(std::floor(p[2] / 25.0f));
          for (int32_t dz = -1; dz <= 1; ++dz) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
              const int64_t key = (int64_t(cx_c + dx) << 32) ^ uint32_t(cz_c + dz);
              auto it = grid_.find(key);
              if (it == grid_.end()) continue;
              for (uint32_t idx : it->second) {
                const auto& pp = props_[idx];
                const float ddx = pp.place.pos[0] - p[0];
                const float ddy = pp.place.pos[1] - p[1];
                const float ddz = pp.place.pos[2] - p[2];
                if (ddx * ddx + ddz * ddz <= 0.75f * 0.75f && std::fabs(ddy) <= 1.5f) return true;
              }
            }
          }
          return false;
        };

        for (const auto& [oid, st] : unique_static) {
          const auto bit = barrier_models.find(st.member);
          if (bit == barrier_models.end()) continue;
          const int64_t pos_key =
              (int64_t(std::lround(st.pos[0] * 4.0f)) * 73856093ll) ^
              (int64_t(std::lround(st.pos[1] * 4.0f)) * 19349663ll) ^
              (int64_t(std::lround(st.pos[2] * 4.0f)) * 83492791ll);
          if (!barrier_seen.insert(pos_key).second) continue;
          if (near_breakable_prop(st.pos)) continue;

          SolidBarrier sb;
          sb.pos = st.pos;
          sb.x_axis = st.x_axis;
          sb.y_axis = st.y_axis;
          sb.z_axis = st.z_axis;
          sb.local_center = bit->second.local_center;
          sb.half_extents = bit->second.half_extents;
          sb.routes = st.routes;
          add_solid_barrier(sb);
        }
      }

      // 2. Pre-decode all unique FH1 smashable child/intact .rmb.bin render models in parallel
      //    so runtime prop shattering has zero disc I/O stutter.
      std::unordered_set<uint32_t> wanted_members;
      for (const auto& row : defs_.child_render_models) {
        for (int32_t rm : row)
          if (rm >= 0) wanted_members.insert(uint32_t(rm));
      }
      for (const auto& pp : props_) {
        if (pp.intact_member >= 0) wanted_members.insert(uint32_t(pp.intact_member));
      }
      std::vector<uint32_t> member_list(wanted_members.begin(), wanted_members.end());
      std::sort(member_list.begin(), member_list.end());
      std::vector<DebrisModel> decoded(member_list.size());
      std::vector<uint8_t> decoded_ok(member_list.size(), 0);
      {
        const size_t n = member_list.size();
        const size_t threads = std::min<size_t>(n, std::max<size_t>(1, std::thread::hardware_concurrency()));
        std::atomic<size_t> next{0};
        auto worker = [&]() {
          std::vector<uint8_t> buf;
          for (;;) {
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) break;
            const uint32_t m = member_list[i];
            const std::string mname = fh1::PropModels::member_name(m, impl_->prefix);
            if (!impl_->source.contains(mname) || !impl_->source.read(mname, buf, nullptr)) continue;
            fh1::RenderModel rm;
            if (!fh1::parse_render_model(buf, rm, nullptr)) continue;
            std::string ignored;
            fh1::parse_render_materials(buf, rm, &ignored);
            if (decode_rmb_to_debris_model(rm, m, impl_->table, decoded[i])) {
              decoded_ok[i] = 1;
            }
          }
        };
        if (threads > 1 && n >= 4) {
          std::vector<std::thread> pool;
          pool.reserve(threads);
          for (size_t t = 0; t < threads; ++t) pool.emplace_back(worker);
          for (auto& w : pool) w.join();
        } else {
          worker();
        }
      }
      impl_->debris_models.reserve(member_list.size());
      for (size_t i = 0; i < member_list.size(); ++i) {
        if (decoded_ok[i]) impl_->debris_models.emplace(member_list[i], std::move(decoded[i]));
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
      // The contact rows for intact props and barriers (FEEL-21).
      if (auto res = z->read("surfacetypes.xml"); res.ok()) {
        SurfaceTableExtras extras;
        if (parse_surface_physics_extras(
                std::string_view(reinterpret_cast<const char*>(res.data.data()), res.data.size()), extras,
                nullptr)) {
          const auto* prop = extras.find_category_params("Smashable", "Car");
          const auto* barrier = extras.find_category_params("Car", "GuardRail");
          set_contact_params(prop, barrier);
          contact_rows_loaded_ = prop && barrier;
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
    pp.radius = std::clamp(pp.def->bounding_radius, 0.35f, 12.0f);
  } else {
    pp.radius = 0.75f;
  }
  const uint32_t idx = uint32_t(props_.size());
  props_.push_back(std::move(pp));
  grid_[cell_key(p.pos[0], p.pos[2])].push_back(idx);
}

void SmashableWorld::add_solid_barrier(const SolidBarrier& barrier) {
  SolidBarrier b = barrier;
  b.half_extents[0] = std::max(0.15f, b.half_extents[0]);
  b.half_extents[1] = std::max(0.25f, b.half_extents[1]);
  b.half_extents[2] = std::max(0.15f, b.half_extents[2]);
  const float center_off = std::sqrt(b.local_center[0] * b.local_center[0] + b.local_center[1] * b.local_center[1] +
                                     b.local_center[2] * b.local_center[2]);
  const float he_r = std::sqrt(b.half_extents[0] * b.half_extents[0] + b.half_extents[1] * b.half_extents[1] +
                               b.half_extents[2] * b.half_extents[2]);
  b.radius = std::max(b.radius, center_off + he_r);

  const uint32_t idx = uint32_t(solid_barriers_.size());
  solid_barriers_.push_back(b);
  const float r = b.radius;
  const int32_t gx0 = int32_t(std::floor((b.pos[0] - r) / 25.0f));
  const int32_t gx1 = int32_t(std::floor((b.pos[0] + r) / 25.0f));
  const int32_t gz0 = int32_t(std::floor((b.pos[2] - r) / 25.0f));
  const int32_t gz1 = int32_t(std::floor((b.pos[2] + r) / 25.0f));
  for (int32_t gz = gz0; gz <= gz1; ++gz) {
    for (int32_t gx = gx0; gx <= gx1; ++gx) {
      const int64_t key = (int64_t(gx) << 32) ^ uint32_t(gz);
      barrier_grid_[key].push_back(idx);
    }
  }
}

bool SmashableWorld::is_discount_sign(const PlacedProp& pp) const {
  if (pp.def) return pp.def->object_type_index == 1;
  return pp.place.stem.find("flyer") != std::string::npos;
}

float SmashableWorld::hide_radius_for(const PlacedProp& pp) const {
  const float def_r = pp.def ? pp.def->bounding_radius : pp.radius;
  return std::clamp(std::max(pp.radius, def_r) + 0.65f, 1.5f, 4.5f);
}

void SmashableWorld::rebuild_broken_positions() {
  auto next_hidden = std::make_shared<std::vector<GatherOptions::HiddenSphere>>();
  next_hidden->reserve(persistent_hidden_.size() + 32);
  for (const auto& hs : persistent_hidden_) next_hidden->push_back(hs);
  for (const auto& pp : props_) {
    if (pp.broken) next_hidden->push_back({pp.place.pos, hide_radius_for(pp)});
  }
  broken_positions_ = next_hidden->empty() ? nullptr : std::move(next_hidden);
}

void SmashableWorld::record_hidden_prop(const std::array<float, 3>& pos, float hide_radius) {
  const float r = std::clamp(hide_radius, 0.75f, 6.0f);
  for (auto& existing : persistent_hidden_) {
    const float dx = existing.pos[0] - pos[0];
    const float dy = existing.pos[1] - pos[1];
    const float dz = existing.pos[2] - pos[2];
    if (dx * dx + dy * dy + dz * dz < 0.25f) {
      existing.radius = std::max(existing.radius, r);
      rebuild_broken_positions();
      return;
    }
  }
  persistent_hidden_.push_back({pos, r});
  rebuild_broken_positions();
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
  const float spd = std::sqrt(car_vel[0] * car_vel[0] + car_vel[2] * car_vel[2]);
  const float dir_x = (spd > 0.5f) ? car_vel[0] / spd : 0.0f;
  const float dir_z = (spd > 0.5f) ? car_vel[2] / spd : 1.0f;
  // Align sign plane roughly perpendicular to striking direction when orientation isn't supplied.
  const std::array<float, 3> x_axis{dir_z, 0.0f, -dir_x};
  const std::array<float, 3> y_axis{0.0f, 1.0f, 0.0f};
  const std::array<float, 3> z_axis{dir_x, 0.0f, dir_z};
  return spawn_flyer_shatter(pos, x_axis, y_axis, z_axis, car_vel);
}

int SmashableWorld::spawn_flyer_shatter(const std::array<float, 3>& pos, const std::array<float, 3>& x_axis,
                                        const std::array<float, 3>& y_axis, const std::array<float, 3>& z_axis,
                                        const std::array<float, 3>& car_vel) {
  const auto* flyer_def = defs_.find_by_object_type(1);
  const int pieces = (flyer_def && !flyer_def->children.empty()) ? int(flyer_def->children.size()) : 12;
  const float spd = std::sqrt(car_vel[0] * car_vel[0] + car_vel[2] * car_vel[2]);
  const float dir_x = (spd > 0.5f) ? car_vel[0] / spd : 0.0f;
  const float dir_z = (spd > 0.5f) ? car_vel[2] / spd : 1.0f;
  const float boost = std::clamp(spd * 1.05f, 5.5f, 28.0f);

  // Locate the Flyer's exact GlobalIndex (g = 162 in Colorado) so we spawn its 12 real FH1 .rmb.bin shards.
  const std::vector<int32_t>* flyer_rms = nullptr;
  for (size_t g = 0; g < defs_.global_to_list.size() && g < defs_.child_render_models.size(); ++g) {
    const uint16_t li = defs_.global_to_list[g];
    if (li < defs_.definitions.size() && defs_.definitions[li].object_type_index == 1) {
      flyer_rms = &defs_.child_render_models[g];
      break;
    }
  }
  const drive::Quat flyer_rot = quat_from_basis(x_axis, y_axis, z_axis);

  for (int i = 0; i < pieces; ++i) {
    DynamicBody b;
    const int32_t rm = (flyer_rms && size_t(i) < flyer_rms->size()) ? (*flyer_rms)[size_t(i)] : -1;
    const bool has_rm = (rm >= 0 && impl_ && impl_->debris_models.count(uint32_t(rm)) > 0);
    if (flyer_def && size_t(i) < flyer_def->children.size()) {
      const auto& ch = flyer_def->children[size_t(i)];
      if (has_rm) {
        const auto& dm = impl_->debris_models.at(uint32_t(rm));
        const float go_len2 = ch.graphics_offset[0] * ch.graphics_offset[0] +
                              ch.graphics_offset[1] * ch.graphics_offset[1] +
                              ch.graphics_offset[2] * ch.graphics_offset[2];
        std::array<float, 3> com_local{};
        if (go_len2 > 1e-6f) {
          com_local = {-ch.graphics_offset[0], -ch.graphics_offset[1], -ch.graphics_offset[2]};
          b.mesh_offset = ch.graphics_offset;
        } else {
          com_local = {0.5f * (dm.box_min[0] + dm.box_max[0]), 0.5f * (dm.box_min[1] + dm.box_max[1]),
                       0.5f * (dm.box_min[2] + dm.box_max[2])};
          b.mesh_offset = {-com_local[0], -com_local[1], -com_local[2]};
        }
        const drive::Vec3 cw = drive::rotate(flyer_rot, {com_local[0], com_local[1], com_local[2]});
        b.pos = {pos[0] + float(cw.x), pos[1] + float(cw.y), pos[2] + float(cw.z)};
        b.rot = flyer_rot;
        b.render_model_member = rm;
        b.half_extents = {
            std::max(0.03f, 0.5f * (dm.box_max[0] - dm.box_min[0])),
            std::max(0.03f, 0.5f * (dm.box_max[1] - dm.box_min[1])),
            std::max(0.03f, 0.5f * (dm.box_max[2] - dm.box_min[2])),
        };
      } else {
        const float row = float(i % 4) - 1.5f;
        const float col = float(i / 4) - 1.0f;
        const drive::Vec3 lo = drive::rotate(flyer_rot, {double(row * 0.45f), double(0.75f + col * 0.35f), 0.0});
        b.pos = {pos[0] + float(lo.x) + rand_sym() * 0.1f, pos[1] + float(lo.y),
                 pos[2] + float(lo.z) + rand_sym() * 0.1f};
        b.rot = flyer_rot;
        b.half_extents = {
            std::clamp(ch.half_extents[0] * 0.55f, 0.10f, 0.28f),
            std::clamp(ch.half_extents[1] * 0.55f, 0.07f, 0.22f),
            std::clamp(ch.half_extents[2] * 0.25f, 0.018f, 0.032f),
        };
      }
      b.mass_kg = ch.mass_kg();
    } else {
      const float row = float(i % 4) - 1.5f;
      const float col = float(i / 4) - 1.0f;
      const drive::Vec3 lo = drive::rotate(flyer_rot, {double(row * 0.45f), double(0.75f + col * 0.35f), 0.0});
      b.pos = {pos[0] + float(lo.x) + rand_sym() * 0.1f, pos[1] + float(lo.y),
               pos[2] + float(lo.z) + rand_sym() * 0.1f};
      b.rot = flyer_rot;
      b.half_extents = {0.20f, 0.14f, 0.022f};
      b.mass_kg = 1.8f;
    }
    b.vel = {
        dir_x * boost * (0.70f + 0.55f * rand01()) + rand_sym() * 3.8f,
        2.6f + rand01() * 4.4f,
        dir_z * boost * (0.70f + 0.55f * rand01()) + rand_sym() * 3.8f,
    };
    b.omega = {rand_sym() * 12.0f, rand_sym() * 12.0f, rand_sym() * 12.0f};
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
      double nx_w = nx_l * door.x_axis[0] + nz_l * door.z_axis[0];
      double nz_w = nx_l * door.x_axis[2] + nz_l * door.z_axis[2];
      const double nw_len = std::hypot(nx_w, nz_w);
      if (nw_len <= 1e-6) return;
      nx_w /= nw_len;
      nz_w /= nw_len;
      const drive::Vec3 n_world{nx_w, 0.0, nz_w};
      vehicle.translate(n_world * (pen + 0.02));
      const drive::Vec3 v = vehicle.velocity();
      const double vn = v.x * n_world.x + v.z * n_world.z;
      if (vn < 0.0) {
        const double j = -(1.15 * vn) * vehicle.params().mass;
        vehicle.apply_impulse(vehicle.centre_of_mass(), n_world * j, 0.04);
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

  // 1. Barn-door open/closed physical collision swap (the solid barriers push
  //    through add_sphere_contacts).
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
        if (active_route_ >= 0 && !fh1::shown_on_route(pp.routes, active_route_)) continue;
        const float px = pp.place.pos[0] - float(cpos.x);
        const float py = pp.place.pos[1] - float(cpos.y);
        const float pz = pp.place.pos[2] - float(cpos.z);
        if (std::fabs(py) > 3.2f) continue;
        const float max_r = half_l + pp.radius + 0.5f;
        if (px * px + pz * pz > max_r * max_r) continue;

        const float along = px * float(cfwd.x) + pz * float(cfwd.z);
        const float across = px * float(cright.x) + pz * float(cright.z);
        bool hit_prop = false;
        if (pp.def->shapes.empty()) {
          hit_prop = std::fabs(along) <= half_l + pp.radius * 0.65f &&
                     std::fabs(across) <= half_w + pp.radius * 0.65f;
        } else {
          // Test real PhysicsDefinitions.bin shapes (Sphere, OBB, ConvexHull) transformed
          // into world space via pp.place orientation against the car OBB.
          const auto& ax = pp.place.x_axis;
          const auto& ay = pp.place.y_axis;
          const auto& az = pp.place.z_axis;
          auto to_world = [&](float lx, float ly, float lz) -> std::array<float, 3> {
            return {pp.place.pos[0] + ax[0] * lx + ay[0] * ly + az[0] * lz,
                    pp.place.pos[1] + ax[1] * lx + ay[1] * ly + az[1] * lz,
                    pp.place.pos[2] + ax[2] * lx + ay[2] * ly + az[2] * lz};
          };
          for (const auto& sh : pp.def->shapes) {
            if (sh.kind == PropShapeKind::kSphere) {
              const auto sw = to_world(sh.sphere[0], sh.sphere[1], sh.sphere[2]);
              const float r = std::max(0.15f, sh.sphere[3]);
              if (std::fabs(sw[1] - float(cpos.y)) > 1.8f + r) continue;
              const float rx = sw[0] - float(cpos.x), rz = sw[2] - float(cpos.z);
              const float sa = rx * float(cfwd.x) + rz * float(cfwd.z);
              const float sc = rx * float(cright.x) + rz * float(cright.z);
              const float da = std::max(0.0f, std::fabs(sa) - half_l);
              const float dc = std::max(0.0f, std::fabs(sc) - half_w);
              if (da * da + dc * dc <= r * r) {
                hit_prop = true;
                break;
              }
            } else if (sh.kind == PropShapeKind::kObb) {
              // 2D SAT in XZ between car OBB and prop OBB + vertical overlap check.
              const auto& m = sh.transform;
              const auto cw = to_world(m[12], m[13], m[14]);
              const float hx = std::max(0.08f, sh.half_extents[0]);
              const float hy = std::max(0.08f, sh.half_extents[1]);
              const float hz = std::max(0.08f, sh.half_extents[2]);
              if (std::fabs(cw[1] - float(cpos.y)) > 1.8f + hy) continue;
              // Prop local X and Z axes transformed to world XZ.
              const float ox_x = ax[0] * m[0] + ay[0] * m[1] + az[0] * m[2];
              const float ox_z = ax[2] * m[0] + ay[2] * m[1] + az[2] * m[2];
              const float oz_x = ax[0] * m[8] + ay[0] * m[9] + az[0] * m[10];
              const float oz_z = ax[2] * m[8] + ay[2] * m[9] + az[2] * m[10];
              const float dx_c = cw[0] - float(cpos.x), dz_c = cw[2] - float(cpos.z);
              auto sat_sep = [&](float axis_x, float axis_z) -> bool {
                const float l = std::hypot(axis_x, axis_z);
                if (l < 1e-4f) return false;
                const float ux = axis_x / l, uz = axis_z / l;
                const float dist = std::fabs(dx_c * ux + dz_c * uz);
                const float r_car = half_w * std::fabs(float(cright.x) * ux + float(cright.z) * uz) +
                                    half_l * std::fabs(float(cfwd.x) * ux + float(cfwd.z) * uz);
                const float r_obb = hx * std::fabs(ox_x * ux + ox_z * uz) +
                                    hz * std::fabs(oz_x * ux + oz_z * uz);
                return dist > r_car + r_obb;
              };
              if (!sat_sep(float(cright.x), float(cright.z)) && !sat_sep(float(cfwd.x), float(cfwd.z)) &&
                  !sat_sep(ox_x, ox_z) && !sat_sep(oz_x, oz_z)) {
                hit_prop = true;
                break;
              }
            } else if (sh.kind == PropShapeKind::kConvexHull && !sh.hull_verts.empty()) {
              const auto& m = sh.transform;
              float min_a = 1e9f, max_a = -1e9f, min_c = 1e9f, max_c = -1e9f, min_y = 1e9f, max_y = -1e9f;
              for (const auto& hv : sh.hull_verts) {
                const float lx = m[0] * hv[0] + m[4] * hv[1] + m[8] * hv[2] + m[12];
                const float ly = m[1] * hv[0] + m[5] * hv[1] + m[9] * hv[2] + m[13];
                const float lz = m[2] * hv[0] + m[6] * hv[1] + m[10] * hv[2] + m[14];
                const auto wv = to_world(lx, ly, lz);
                const float rx = wv[0] - float(cpos.x), rz = wv[2] - float(cpos.z);
                const float sa = rx * float(cfwd.x) + rz * float(cfwd.z);
                const float sc = rx * float(cright.x) + rz * float(cright.z);
                min_a = std::min(min_a, sa); max_a = std::max(max_a, sa);
                min_c = std::min(min_c, sc); max_c = std::max(max_c, sc);
                min_y = std::min(min_y, wv[1]); max_y = std::max(max_y, wv[1]);
              }
              if (max_a >= -half_l && min_a <= half_l && max_c >= -half_w && min_c <= half_w &&
                  max_y >= float(cpos.y) - 0.6f && min_y <= float(cpos.y) + 1.8f) {
                hit_prop = true;
                break;
              }
            }
          }
        }
        if (!hit_prop) continue;

        const auto& def = *pp.def;
        const drive::Vec3 com = vehicle.centre_of_mass();
        // Below BreakSpeedMph the prop is a fixed body in the car's own
        // contact solve (add_sphere_contacts, FEEL-21).
        if (!should_break(def, car_speed)) {
          continue;
        }

        // Break prop -> record broken position for static prop hiding and spawn real FH1 fractured debris.
        pp.broken = true;
        const float hide_r = std::clamp(std::max(pp.radius, def.bounding_radius) + 0.65f, 1.5f, 4.5f);
        auto next_hidden = broken_positions_
                               ? std::make_shared<std::vector<GatherOptions::HiddenSphere>>(*broken_positions_)
                               : std::make_shared<std::vector<GatherOptions::HiddenSphere>>();
        next_hidden->push_back({pp.place.pos, hide_r});
        broken_positions_ = std::move(next_hidden);

        const float prop_mass_kg = def.mass_kg();
        const float car_mass = float(std::max(500.0, vehicle.params().mass));
        // Apply brief, soft linear velocity damping on smash-through (FH1 feel:
        // fences and light props absorb a tiny fraction of speed with zero pitch/spin kick).
        float decel_frac = (def.mass <= 0.30f) ? std::min(0.025f, prop_mass_kg / (car_mass * 2.5f))
                                               : std::clamp(prop_mass_kg / (car_mass + prop_mass_kg), 0.02f, 0.14f);
        if (def.object_type_index == 0) decel_frac = std::min(decel_frac, 0.035f);
        if (car_speed > 0.5f) {
          const drive::Vec3 dv{-cvel.x * decel_frac, 0.0, -cvel.z * decel_frac};
          vehicle.apply_impulse(com, dv * vehicle.params().mass, 0.0);
        }

        const auto base_col = colour_for_prop(def.surface_type_index, def.object_type_index, pp.place.stem);
        const bool is_pole_prop = pp.place.stem.find("pole") != std::string::npos ||
                                  pp.place.stem.find("lamp") != std::string::npos ||
                                  pp.place.stem.find("post") != std::string::npos ||
                                  def.object_type_index == 2;
        const bool is_sign_prop = pp.place.stem.find("sign") != std::string::npos ||
                                  pp.place.stem.find("stop") != std::string::npos ||
                                  def.object_type_index == 1;
        const std::array<uint8_t, 4> post_steel_col{70, 76, 82, 255};
        auto vary_col = [&](std::array<uint8_t, 4> c) {
          const float shade = 0.78f + 0.24f * rand01();
          return std::array<uint8_t, 4>{
              uint8_t(std::clamp(int(float(c[0]) * shade), 0, 225)),
              uint8_t(std::clamp(int(float(c[1]) * shade), 0, 225)),
              uint8_t(std::clamp(int(float(c[2]) * shade), 0, 225)),
              c[3],
          };
        };

        const drive::Quat prop_rot = quat_from_basis(pp.place.x_axis, pp.place.y_axis, pp.place.z_axis);
        const std::vector<int32_t>* child_rms =
            (pp.global_index >= 0 && size_t(pp.global_index) < defs_.child_render_models.size())
                ? &defs_.child_render_models[size_t(pp.global_index)]
                : nullptr;

        int spawned = 0;
        // Keep forward debris carry below vehicle speed and deflect laterally outward
        // away from the car's centerline so knocked signs/poles tumble cleanly to the
        // roadside instead of pacing the windshield or drifting into the chase camera lens.
        const float impulse_scale = std::clamp(car_speed * 0.62f, 3.0f, 19.0f);
        const float lift_scale = std::clamp(1.1f + car_speed * 0.09f, 1.3f, 3.6f);
        const float spin_scale = std::clamp(4.5f + car_speed * 0.32f, 5.0f, 13.0f);
        const float dir_x = (car_speed > 0.5f) ? float(cvel.x) / car_speed : float(cfwd.x);
        const float dir_z = (car_speed > 0.5f) ? float(cvel.z) / car_speed : float(cfwd.z);
        const float side_sign = (across >= 0.0f) ? 1.0f : -1.0f;
        const float cright_x = float(cright.x);
        const float cright_z = float(cright.z);
        // Horizontal tumble axis perpendicular to impact velocity (pitches sign top away).
        const float tumble_ax = -dir_z;
        const float tumble_az = dir_x;
        const float base_lat_kick = (is_sign_prop || is_pole_prop) ? 4.8f : 2.6f;

        auto setup_real_piece = [&](DynamicBody& b, int32_t rm, const PropPhysicsDef& piece_def) -> bool {
          if (rm < 0 || !impl_) return false;
          const auto mit = impl_->debris_models.find(uint32_t(rm));
          if (mit == impl_->debris_models.end()) return false;
          const auto& dm = mit->second;
          const float go_len2 = piece_def.graphics_offset[0] * piece_def.graphics_offset[0] +
                                piece_def.graphics_offset[1] * piece_def.graphics_offset[1] +
                                piece_def.graphics_offset[2] * piece_def.graphics_offset[2];
          std::array<float, 3> com_local{};
          if (go_len2 > 1e-6f) {
            // Confirmed FH1 convention: graphics_offset == -(local game-frame CoM of child .rmb.bin).
            com_local = {-piece_def.graphics_offset[0], -piece_def.graphics_offset[1], -piece_def.graphics_offset[2]};
            b.mesh_offset = piece_def.graphics_offset;
          } else {
            com_local = {0.5f * (dm.box_min[0] + dm.box_max[0]), 0.5f * (dm.box_min[1] + dm.box_max[1]),
                         0.5f * (dm.box_min[2] + dm.box_max[2])};
            b.mesh_offset = {-com_local[0], -com_local[1], -com_local[2]};
          }
          const drive::Vec3 com_world = drive::rotate(prop_rot, {com_local[0], com_local[1], com_local[2]});
          b.pos = {pp.place.pos[0] + float(com_world.x), pp.place.pos[1] + float(com_world.y),
                   pp.place.pos[2] + float(com_world.z)};
          b.rot = prop_rot;
          b.render_model_member = rm;
          b.half_extents = {
              std::max(0.025f, 0.5f * (dm.box_max[0] - dm.box_min[0])),
              std::max(0.025f, 0.5f * (dm.box_max[1] - dm.box_min[1])),
              std::max(0.025f, 0.5f * (dm.box_max[2] - dm.box_min[2])),
          };
          b.mass_kg = piece_def.mass_kg();
          // Radial scatter + deterministic outward lateral deflection away from the car corridor.
          const float rx = float(com_world.x);
          const float rz = float(com_world.z);
          const float rlen = std::sqrt(rx * rx + rz * rz);
          const float out_x = (rlen > 0.05f) ? (rx / rlen) * (1.0f + 1.2f * rand01()) : 0.0f;
          const float out_z = (rlen > 0.05f) ? (rz / rlen) * (1.0f + 1.2f * rand01()) : 0.0f;
          const float lat_v = side_sign * (base_lat_kick + 3.0f * rand01());
          const float fwd_frac = 0.52f + 0.36f * rand01();
          b.vel = {
              dir_x * impulse_scale * fwd_frac + cright_x * lat_v + out_x + rand_sym() * 1.1f,
              lift_scale * (0.55f + 0.55f * rand01()),
              dir_z * impulse_scale * fwd_frac + cright_z * lat_v + out_z + rand_sym() * 1.1f,
          };
          const float tumble_spd = spin_scale * (0.65f + 0.55f * rand01());
          b.omega = {
              tumble_ax * tumble_spd + rand_sym() * 3.5f,
              side_sign * (2.5f + 3.5f * rand01()),
              tumble_az * tumble_spd + rand_sym() * 3.5f,
          };
          b.colour = base_col;
          return true;
        };

        if (!def.children.empty()) {
          for (size_t ci = 0; ci < def.children.size(); ++ci) {
            const auto& ch = def.children[ci];
            DynamicBody b;
            const int32_t rm = (child_rms && ci < child_rms->size() && (*child_rms)[ci] >= 0) ? (*child_rms)[ci] : -1;
            if (!setup_real_piece(b, rm, ch)) {
              b.pos = {
                  pp.place.pos[0] + ch.aabb_centre_offset[0] + rand_sym() * 0.35f,
                  pp.place.pos[1] + std::max(0.25f, ch.aabb_centre_offset[1] + 0.5f) + rand01() * 0.45f,
                  pp.place.pos[2] + ch.aabb_centre_offset[2] + rand_sym() * 0.35f,
              };
              const float lat_v = side_sign * (base_lat_kick + 2.8f * rand01());
              b.vel = {
                  dir_x * impulse_scale * (0.52f + 0.40f * rand01()) + cright_x * lat_v + rand_sym() * 1.6f,
                  lift_scale * (0.55f + 0.60f * rand01()),
                  dir_z * impulse_scale * (0.52f + 0.40f * rand01()) + cright_z * lat_v + rand_sym() * 1.6f,
              };
              const float tumble_spd = spin_scale * (0.65f + 0.50f * rand01());
              b.omega = {
                  tumble_ax * tumble_spd + rand_sym() * 3.5f,
                  side_sign * (2.5f + 3.5f * rand01()),
                  tumble_az * tumble_spd + rand_sym() * 3.5f,
              };
              if (is_pole_prop && !is_sign_prop) {
                b.half_extents = {
                    std::clamp(ch.half_extents[0], 0.045f, 0.09f),
                    std::clamp(ch.half_extents[1], 0.28f, 0.65f),
                    std::clamp(ch.half_extents[2], 0.045f, 0.09f),
                };
              } else {
                b.half_extents = {
                    std::clamp(ch.half_extents[0], 0.05f, 0.58f),
                    std::clamp(ch.half_extents[1], 0.04f, 0.58f),
                    std::clamp(ch.half_extents[2], 0.02f, 0.16f),
                };
              }
              b.mass_kg = ch.mass_kg();
              b.colour = vary_col((is_sign_prop && (ci % 2 == 1)) ? post_steel_col : base_col);
            }
            spawn_dynamic_body(b);
            ++spawned;
          }
        } else {
          // Single-piece breakable prop (e.g. cone, deer sign, fence end post): send the prop's own
          // real .rmb.bin mesh tumbling whole as a single rigid body!
          const int32_t single_rm = (child_rms && !child_rms->empty() && (*child_rms)[0] >= 0) ? (*child_rms)[0]
                                                                                               : pp.intact_member;
          DynamicBody whole;
          if (setup_real_piece(whole, single_rm, def)) {
            spawn_dynamic_body(whole);
            spawned = 1;
          } else {
            // Synthetic unit-test fallback when no .rmb.bin archive is loaded.
            const int chunks = (def.object_type_index == 0) ? 4 : 3;
            for (int ci = 0; ci < chunks; ++ci) {
              DynamicBody b;
              b.pos = {pp.place.pos[0] + rand_sym() * 0.25f, pp.place.pos[1] + 0.45f + float(ci) * 0.2f,
                       pp.place.pos[2] + rand_sym() * 0.25f};
              const float lat_v = side_sign * (base_lat_kick + 2.8f * rand01());
              b.vel = {
                  dir_x * impulse_scale * (0.52f + 0.38f * rand01()) + cright_x * lat_v + rand_sym() * 1.5f,
                  lift_scale * (0.55f + 0.55f * rand01()),
                  dir_z * impulse_scale * (0.52f + 0.38f * rand01()) + cright_z * lat_v + rand_sym() * 1.5f,
              };
              const float tumble_spd = spin_scale * (0.65f + 0.50f * rand01());
              b.omega = {
                  tumble_ax * tumble_spd + rand_sym() * 3.5f,
                  side_sign * (2.5f + 3.5f * rand01()),
                  tumble_az * tumble_spd + rand_sym() * 3.5f,
              };
              if (def.object_type_index == 0) {
                b.half_extents = {0.42f + 0.18f * rand01(), 0.07f + 0.03f * rand01(), 0.025f};
              } else if (is_sign_prop) {
                if (ci == 0) {
                  b.half_extents = {0.26f + 0.08f * rand01(), 0.22f + 0.07f * rand01(), 0.016f};
                } else {
                  b.half_extents = {0.042f, 0.36f + 0.16f * rand01(), 0.042f};
                }
              } else if (is_pole_prop) {
                b.half_extents = {0.052f, 0.38f + 0.18f * rand01(), 0.052f};
              } else {
                b.half_extents = {
                    std::clamp(def.half_extents[0] * 0.40f, 0.08f, 0.38f),
                    std::clamp(def.half_extents[1] * 0.36f, 0.06f, 0.38f),
                    std::clamp(def.half_extents[2] * 0.26f, 0.03f, 0.14f),
                };
              }
              b.mass_kg = prop_mass_kg / float(chunks);
              b.colour = vary_col((is_sign_prop && ci > 0) ? post_steel_col : base_col);
              spawn_dynamic_body(b);
              ++spawned;
            }
          }
        }

        SmashEvent ev;
        ev.pos = pp.place.pos;
        ev.impulse_vel = {dir_x * impulse_scale, 3.5f, dir_z * impulse_scale};
        ev.prop_stem = pp.place.stem;
        ev.object_type_index = def.object_type_index;
        ev.surface_type_index = def.surface_type_index;
        ev.global_index = pp.global_index;
        ev.debris_spawned = spawned;
        ev.hide_radius = hide_r;
        events.push_back(std::move(ev));
      }
    }
  }

  // 3. Integrate all active dynamic debris bodies (gravity, air drag, spin damping,
  //    chase-camera corridor deflection, ground bounce, despawn).
  const float lin_drag = std::exp(-1.35f * step_dt);
  const float vert_drag = std::exp(-0.35f * step_dt);
  const float ang_drag = std::exp(-0.65f * step_dt);
  const float crx = float(cright.x), crz = float(cright.z);
  const float cfx = float(cfwd.x), cfz = float(cfwd.z);
  size_t write = 0;
  for (size_t i = 0; i < dynamic_bodies_.size(); ++i) {
    DynamicBody& b = dynamic_bodies_[i];
    b.age += step_dt;
    if (b.age >= kDebrisDespawnSeconds) continue;

    if (!b.settled) {
      b.vel[0] *= lin_drag;
      b.vel[2] *= lin_drag;
      b.vel[1] = (b.vel[1] - 9.81f * step_dt) * vert_drag;
      b.omega[0] *= ang_drag;
      b.omega[1] *= ang_drag;
      b.omega[2] *= ang_drag;

      // Gently deflect airborne debris laterally out of the rear chase-camera sightline
      // so broken signs never float directly in front of the camera lens.
      const float dx_c = b.pos[0] - float(cpos.x);
      const float dy_c = b.pos[1] - float(cpos.y);
      const float dz_c = b.pos[2] - float(cpos.z);
      const float d_along = dx_c * cfx + dz_c * cfz;
      const float d_across = dx_c * crx + dz_c * crz;
      if (d_along > -8.0f && d_along < 1.5f && std::fabs(d_across) < 2.2f && dy_c > -0.5f && dy_c < 3.8f) {
        const float push_s = (d_across >= 0.0f) ? 1.0f : -1.0f;
        const float push_dv = 14.0f * step_dt;
        b.vel[0] += crx * push_s * push_dv;
        b.vel[2] += crz * push_s * push_dv;
        b.pos[0] += crx * push_s * (2.5f * step_dt);
        b.pos[2] += crz * push_s * (2.5f * step_dt);
      }

      b.pos[0] += b.vel[0] * step_dt;
      b.pos[1] += b.vel[1] * step_dt;
      b.pos[2] += b.vel[2] * step_dt;
      b.rot = quat_integrate(b.rot, b.omega, step_dt);

      const float min_h = std::min({b.half_extents[0], b.half_extents[1], b.half_extents[2]});
      drive::Hit hit;
      if (ground.raycast({double(b.pos[0]), double(b.pos[1]) + 2.5, double(b.pos[2])}, {0, -1, 0}, 8.0, hit)) {
        const float floor_y = float(double(b.pos[1]) + 2.5 - hit.distance) + min_h;
        if (b.pos[1] < floor_y) {
          b.pos[1] = floor_y;
          if (b.vel[1] < -0.6f) {
            b.vel[1] = -b.vel[1] * 0.28f;
          } else {
            b.vel[1] = 0.0f;
          }
          b.vel[0] *= 0.68f;
          b.vel[2] *= 0.68f;
          b.omega[0] *= 0.60f;
          b.omega[1] *= 0.60f;
          b.omega[2] *= 0.60f;
          const float spd2 = b.vel[0] * b.vel[0] + b.vel[1] * b.vel[1] + b.vel[2] * b.vel[2];
          if (spd2 < 0.12f && b.age > 0.5f) b.settled = true;
        }
      }
    }

    if (write != i) dynamic_bodies_[write] = b;
    ++write;
  }
  dynamic_bodies_.resize(write);

  // 4. Periodic distance-based respawn in Free Roam: every ~1 s restore non-Discount-Sign
  //    broken props more than 300 m away from the player and refresh broken_positions_.
  respawn_timer_ += step_dt;
  if (respawn_timer_ >= kRespawnCheckIntervalS) {
    respawn_timer_ = std::fmod(respawn_timer_, kRespawnCheckIntervalS);
    const size_t dynamic_broken = broken_positions_
                                      ? (broken_positions_->size() > persistent_hidden_.size()
                                             ? broken_positions_->size() - persistent_hidden_.size()
                                             : 0)
                                      : 0;
    if (dynamic_broken > 0) {
      constexpr float kMaxDist2 = kRespawnDistanceM * kRespawnDistanceM;
      bool any_respawned = false;
      for (auto& pp : props_) {
        if (!pp.broken || is_discount_sign(pp)) continue;
        const float dx = pp.place.pos[0] - float(cpos.x);
        const float dz = pp.place.pos[2] - float(cpos.z);
        if (dx * dx + dz * dz > kMaxDist2) {
          pp.broken = false;
          respawned_hidden_.push_back({pp.place.pos, hide_radius_for(pp)});
          any_respawned = true;
        }
      }
      if (any_respawned) rebuild_broken_positions();
    }
  }
}

void SmashableWorld::clear_debris() { dynamic_bodies_.clear(); }

void SmashableWorld::reset_broken() {
  dynamic_bodies_.clear();
  respawn_timer_ = 0.0f;
  for (auto& pp : props_) {
    if (pp.broken && !is_discount_sign(pp)) {
      pp.broken = false;
      respawned_hidden_.push_back({pp.place.pos, hide_radius_for(pp)});
    }
  }
  rebuild_broken_positions();
}

std::vector<GatherOptions::HiddenSphere> SmashableWorld::consume_respawned_hidden() {
  std::vector<GatherOptions::HiddenSphere> out;
  out.swap(respawned_hidden_);
  return out;
}

void SmashableWorld::build_debris_mesh(std::vector<DebrisVertex>& out_tris) const {
  out_tris.clear();
  if (dynamic_bodies_.empty()) return;

  // Unit cube 6 faces (2 triangles each = 36 vertices) used only as synthetic fallback when no disc .rmb.bin is loaded.
  static constexpr int kFaces[6][4] = {
      {0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0},
  };
  static constexpr float kNormals[6][3] = {
      {0, 0, 1}, {0, 0, -1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0},
  };
  static constexpr float kCorners[8][3] = {
      {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
  };

  // Group real textured triangles by (cutout, texture_record) so Showroom issues minimal batched draw calls.
  std::map<uint64_t, std::vector<DebrisVertex>> batches;
  std::vector<DebrisVertex> untextured;

  for (const auto& b : dynamic_bodies_) {
    const float fade = (b.age > kDebrisDespawnSeconds - 1.5f)
                           ? std::clamp((kDebrisDespawnSeconds - b.age) / 1.5f, 0.0f, 1.0f)
                           : 1.0f;
    if (fade <= 0.02f) continue;
    const float scale = (fade < 1.0f) ? (0.25f + 0.75f * fade) : 1.0f;

    const DebrisModel* dm = nullptr;
    if (b.render_model_member >= 0 && impl_) {
      const auto mit = impl_->debris_models.find(uint32_t(b.render_model_member));
      if (mit != impl_->debris_models.end()) dm = &mit->second;
    }

    if (dm && dm->valid()) {
      for (const auto& sm : dm->submeshes) {
        if (sm.tris.empty()) continue;
        std::vector<DebrisVertex>* dst = &untextured;
        if (sm.texture_record != kNoTexture) {
          const uint64_t bkey = (uint64_t(sm.decal) << 40) | (uint64_t(sm.cutout) << 32) | uint64_t(sm.texture_record);
          dst = &batches[bkey];
        }
        dst->reserve(dst->size() + sm.tris.size());
        for (const auto& mv : sm.tris) {
          const drive::Vec3 local{
              double((mv.pos[0] + b.mesh_offset[0]) * scale),
              double((mv.pos[1] + b.mesh_offset[1]) * scale),
              double((mv.pos[2] + b.mesh_offset[2]) * scale),
          };
          const drive::Vec3 wp = drive::rotate(b.rot, local);
          const drive::Vec3 wn = drive::rotate(b.rot, {double(mv.normal[0]), double(mv.normal[1]), double(mv.normal[2])});
          DebrisVertex out{};
          out.pos = {b.pos[0] + float(wp.x), b.pos[1] + float(wp.y), b.pos[2] + float(wp.z)};
          out.normal = {float(wn.x), float(wn.y), float(wn.z)};
          out.rgba = (sm.texture_record != kNoTexture) ? mv.rgba : b.colour;
          out.rgba[3] = uint8_t(float(out.rgba[3]) * fade);
          out.uv = mv.uv;
          out.texture_record = sm.texture_record;
          out.cutout = sm.cutout;
          dst->push_back(out);
        }
      }
      continue;
    }

    // Synthetic box fallback (only when no game disc .rmb.bin library was loaded, e.g. unit tests).
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

    static constexpr float kFaceShade[6] = {0.96f, 0.84f, 0.88f, 0.92f, 1.00f, 0.68f};
    for (int f = 0; f < 6; ++f) {
      const drive::Vec3 wn = drive::rotate(b.rot, {kNormals[f][0], kNormals[f][1], kNormals[f][2]});
      const std::array<float, 3> n = {float(wn.x), float(wn.y), float(wn.z)};
      const float fs = kFaceShade[f];
      const std::array<uint8_t, 4> fcol = {
          uint8_t(std::clamp(int(float(col[0]) * fs), 0, 255)),
          uint8_t(std::clamp(int(float(col[1]) * fs), 0, 255)),
          uint8_t(std::clamp(int(float(col[2]) * fs), 0, 255)),
          col[3],
      };
      const auto& p0 = wv[size_t(kFaces[f][0])];
      const auto& p1 = wv[size_t(kFaces[f][1])];
      const auto& p2 = wv[size_t(kFaces[f][2])];
      const auto& p3 = wv[size_t(kFaces[f][3])];
      untextured.push_back({p0, n, fcol});
      untextured.push_back({p1, n, fcol});
      untextured.push_back({p2, n, fcol});
      untextured.push_back({p0, n, fcol});
      untextured.push_back({p2, n, fcol});
      untextured.push_back({p3, n, fcol});
    }
  }

  size_t total = untextured.size();
  for (const auto& [k, v] : batches) total += v.size();
  out_tris.reserve(total);
  for (const auto& [k, v] : batches) out_tris.insert(out_tris.end(), v.begin(), v.end());
  out_tris.insert(out_tris.end(), untextured.begin(), untextured.end());
}

SmashableWorld::RewindSnapshot SmashableWorld::capture_rewind() const {
  RewindSnapshot snap;
  for (uint32_t i = 0; i < uint32_t(props_.size()); ++i) {
    if (props_[i].broken) snap.broken_indices.push_back(i);
  }
  return snap;
}

std::vector<GatherOptions::HiddenSphere> SmashableWorld::restore_rewind(const RewindSnapshot& s) {
  std::vector<GatherOptions::HiddenSphere> restored;
  std::vector<uint8_t> want_broken(props_.size(), 0);
  for (uint32_t idx : s.broken_indices) {
    if (idx < props_.size()) want_broken[idx] = 1;
  }
  auto next_hidden = std::make_shared<std::vector<GatherOptions::HiddenSphere>>(persistent_hidden_);
  for (size_t i = 0; i < props_.size(); ++i) {
    auto& pp = props_[i];
    const float hide_r = hide_radius_for(pp);
    if (pp.broken && !want_broken[i]) {
      pp.broken = false;
      restored.push_back({pp.place.pos, hide_r});
    } else if (!pp.broken && want_broken[i]) {
      pp.broken = true;
    }
    if (pp.broken) next_hidden->push_back({pp.place.pos, hide_r});
  }
  broken_positions_ = next_hidden->empty() ? nullptr : std::move(next_hidden);
  dynamic_bodies_.clear();
  return restored;
}

}  // namespace world
