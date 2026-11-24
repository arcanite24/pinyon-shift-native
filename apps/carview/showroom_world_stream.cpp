#include "showroom_internal.h"

#include "world_gpu_logic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace carview {

namespace {

int8_t snorm8(float v) { return int8_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f)); }

// Returns true if the top mip of `tex` contains any transparent texels (alpha < 0.5).
// Used to reject non-cutout textures (e.g. opaque tree bark / rock textures) that
// would otherwise render as solid opaque rectangular billboard cards.
bool texture_has_cutout_alpha(const bake::Texture& tex) {
  if (tex.levels.empty()) return false;
  const auto& lvl = tex.levels[0];
  const uint8_t* p = lvl.data.data();
  const size_t n = lvl.data.size();
  switch (tex.format) {
    case bake::TextureFormat::kRgba8:
      for (size_t i = 3; i < n; i += 4)
        if (p[i] < 128u) return true;
      return false;
    case bake::TextureFormat::kBc1: {
      // 8 bytes per 4x4 block: u16 c0, u16 c1, u32 indices.
      // 1-bit alpha exists iff c0 <= c1 and at least one 2-bit index == 3.
      for (size_t i = 0; i + 8 <= n; i += 8) {
        const uint16_t c0 = uint16_t(p[i]) | (uint16_t(p[i + 1]) << 8);
        const uint16_t c1 = uint16_t(p[i + 2]) | (uint16_t(p[i + 3]) << 8);
        if (c0 > c1) continue;
        uint32_t idx = uint32_t(p[i + 4]) | (uint32_t(p[i + 5]) << 8) |
                       (uint32_t(p[i + 6]) << 16) | (uint32_t(p[i + 7]) << 24);
        for (int k = 0; k < 16; ++k, idx >>= 2)
          if ((idx & 3u) == 3u) return true;
      }
      return false;
    }
    case bake::TextureFormat::kBc2: {
      // 16 bytes per 4x4 block: first 8 bytes are 16x 4-bit alpha values.
      for (size_t i = 0; i + 16 <= n; i += 16) {
        for (size_t b = 0; b < 8; ++b) {
          if ((p[i + b] & 0x0Fu) < 8u || (p[i + b] >> 4) < 8u) return true;
        }
      }
      return false;
    }
    case bake::TextureFormat::kBc3: {
      // 16 bytes per 4x4 block: first 8 bytes are alpha (a0, a1, 48-bit 3-bit indices).
      for (size_t i = 0; i + 16 <= n; i += 16) {
        const uint8_t a0 = p[i];
        const uint8_t a1 = p[i + 1];
        if (a0 < 128u || a1 < 128u) return true;
        if (a0 <= a1) {
          // 6-interpolated mode: index 6 = 0 (transparent), index 7 = 255.
          uint64_t bits = 0;
          for (int b = 0; b < 6; ++b) bits |= uint64_t(p[i + 2 + b]) << (8 * b);
          for (int k = 0; k < 16; ++k, bits >>= 3)
            if ((bits & 7u) == 6u) return true;
        }
      }
      return false;
    }
    case bake::TextureFormat::kBc4:
    case bake::TextureFormat::kBc5:
    case bake::TextureFormat::kR8:
      return false;
  }
  return false;
}

// The textured world: one vertex per corner, one draw per material, the
// the sky dome first, then opaque materials, then the cut-outs, then the
// blended ones.
WorldGeometry build_textured_world(const WorldTriangles& world) {
  WorldGeometry out;
  out.textured = true;
  out.textures = world.textures;
  {
    // The sky (ours): a unit sphere of directions in the game frame, which
    // world.vert centres on the eye at the far plane and world.frag shades
    // by height.
    WorldGeometry::Draw d;
    d.first_index = 0;
    constexpr int kRings = 12, kSegments = 24;
    constexpr float kPi = 3.14159265358979f;
    auto dir = [&](int ring, int seg) {
      const float a = kPi * float(ring) / kRings - kPi / 2, b = 2 * kPi * float(seg) / kSegments;
      return std::array<float, 3>{std::cos(a) * std::cos(b), std::sin(a), std::cos(a) * std::sin(b)};
    };
    auto vertex = [&](const std::array<float, 3>& p) {
      WorldVertex v{};
      for (int i = 0; i < 3; ++i) v.position[i] = p[size_t(i)];
      out.indices.push_back(uint32_t(out.textured_vertices.size()));
      out.textured_vertices.push_back(v);
    };
    for (int r = 0; r < kRings; ++r)
      for (int s = 0; s < kSegments; ++s) {
        const auto a = dir(r, s), b = dir(r + 1, s), c = dir(r + 1, s + 1), e = dir(r, s + 1);
        vertex(a), vertex(b), vertex(c), vertex(a), vertex(c), vertex(e);
      }
    d.index_count = uint32_t(out.indices.size());
    d.texture = kNoWorldTexture;
    d.blend = 0;
    d.push.params[0] = 0;
    d.push.params[1] = 2;  // world.frag's kFlagSky
    out.draws.push_back(d);
  }
  const size_t n = world.material.size();
  std::vector<std::vector<uint32_t>> by_material(world.materials.size());
  for (uint32_t t = 0; t < n; ++t)
    if (world.material[t] < by_material.size()) by_material[world.material[t]].push_back(t);
  std::vector<uint32_t> order(world.materials.size());
  for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(),
                   [&](uint32_t a, uint32_t b) { return world.materials[a].blend < world.materials[b].blend; });
  out.textured_vertices.reserve(n * 3);
  out.indices.reserve(n * 3);
  // Each material's triangles split by ground cell (by the triangle's
  // first corner), so a draw has a box the view can cull: near scenery in
  // near_cell squares, the far ring in far_cell ones (ours, a balance of
  // draws against culled area: 128 / 768 m cut 4,100 draws but recorded
  // 1,400, which cost the UHD 620 more than the triangles saved;
  // SHIFT_WORLD_CELL="near far" overrides).
  float near_cell = 256, far_cell = 1536;
  if (const char* e = std::getenv("SHIFT_WORLD_CELL")) std::sscanf(e, "%f %f", &near_cell, &far_cell);
  near_cell = std::max(near_cell, 1.0f), far_cell = std::max(far_cell, 1.0f);
  // Within a draw, corners with the same attributes share one vertex, so
  // the post-transform cache can reuse it (SHIFT_WORLD_SHARE=0: one per
  // corner, for comparison).
  struct VertexKey {
    WorldVertex v;
    bool operator==(const VertexKey& o) const { return std::memcmp(&v, &o.v, sizeof v) == 0; }
  };
  struct VertexHash {
    size_t operator()(const VertexKey& k) const {
      const auto* p = reinterpret_cast<const unsigned char*>(&k.v);
      uint64_t h = 1469598103934665603ull;
      for (size_t i = 0; i < sizeof k.v; ++i) h = (h ^ p[i]) * 1099511628211ull;
      return size_t(h);
    }
  };
  std::unordered_map<VertexKey, uint32_t, VertexHash> shared;
  const char* share_env = std::getenv("SHIFT_WORLD_SHARE");
  const bool share = !(share_env && *share_env == '0');
  // Cache cutout-alpha status per texture record across streaming regathers so
  // distant-tree billboards whose model_textures[0] is an opaque bark/rock texture
  // (0% transparent texels) are never emitted as solid rectangular billboard cards.
  static std::mutex cutout_cache_mutex;
  static std::unordered_map<uint32_t, bool> cutout_cache;
  {
    std::lock_guard<std::mutex> lock(cutout_cache_mutex);
    for (const auto& wt : world.textures) {
      if (wt) cutout_cache[wt->record] = texture_has_cutout_alpha(wt->texture);
    }
  }
  auto billboard_has_cutout = [&](uint32_t record) -> bool {
    if (record == kNoWorldTexture) return false;
    std::lock_guard<std::mutex> lock(cutout_cache_mutex);
    const auto it = cutout_cache.find(record);
    return it == cutout_cache.end() ? true : it->second;
  };
  std::vector<std::pair<int64_t, uint32_t>> cells;  // (cell, triangle), sorted by cell
  for (uint32_t m : order) {
    if (by_material[m].empty()) continue;
    const auto& mat = world.materials[m];
    const bool billboard = mat.layers == 4;
    if (billboard && !billboard_has_cutout(mat.texture)) continue;
    const bool fade_class = mat.fade != 0;
    // A prop's LOD00 copies (fade class 1) in small cells, so the ones past
    // the LOD band are left out whole on the CPU (near_lod_beyond).
    const float cell = mat.far ? far_cell : mat.fade == 1 ? 64.0f : near_cell;
    cells.clear();
    for (uint32_t t : by_material[m]) {
      const auto& p = world.positions[size_t(t) * 3];
      const int64_t cx = int64_t(std::floor(p[0] / cell)), cz = int64_t(std::floor(p[2] / cell));
      cells.emplace_back(((cx + (int64_t(1) << 20)) << 21) | (cz + (int64_t(1) << 20)), t);
    }
    std::stable_sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // The flat colour of an untextured material: its triangles' surface class.
    std::array<float, 3> tint{0.3f, 0.3f, 0.3f};
    const uint8_t g = world.group[by_material[m][0]];
    if (g < world.colours.size()) tint = world.colours[g];
    for (size_t first = 0; first < cells.size();) {
    size_t last = first;
    while (last < cells.size() && cells[last].first == cells[first].first) ++last;
    WorldGeometry::Draw d;
    d.first_index = uint32_t(out.indices.size());
    d.cull = true;
    for (int i = 0; i < 3; ++i) d.box_min[i] = 1e30f, d.box_max[i] = -1e30f;
    float reach = 0;
    shared.clear();
    const bool normalize_ccw = (mat.blend == 0 && !mat.is_water && !mat.anim_flag && !mat.is_grass && !billboard);
    for (size_t j = first; j < last; ++j) {
      const uint32_t t = cells[j].second;
      int corners[3] = {0, 1, 2};
      if (normalize_ccw) {
        const size_t c0 = size_t(t) * 3;
        const auto& p0 = world.positions[c0];
        const auto& p1 = world.positions[c0 + 1];
        const auto& p2 = world.positions[c0 + 2];
        const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const float g_x = e1[1] * e2[2] - e1[2] * e2[1];
        const float g_y = e1[2] * e2[0] - e1[0] * e2[2];
        const float g_z = e1[0] * e2[1] - e1[1] * e2[0];
        const float nx = world.normals[c0][0] + world.normals[c0 + 1][0] + world.normals[c0 + 2][0];
        const float ny = world.normals[c0][1] + world.normals[c0 + 1][1] + world.normals[c0 + 2][1];
        const float nz = world.normals[c0][2] + world.normals[c0 + 1][2] + world.normals[c0 + 2][2];
        const bool have_n = (nx * nx + ny * ny + nz * nz) > 1e-4f;
        const float ref_dot = have_n ? (g_x * nx + g_y * ny + g_z * nz) : g_y;
        if (ref_dot < 0.0f) {
          corners[1] = 2;
          corners[2] = 1;
        }
      }
      for (int k : corners) {
        const size_t c = size_t(t) * 3 + size_t(k);
        VertexKey key{};
        WorldVertex& v = key.v;
        for (int i = 0; i < 3; ++i) v.position[i] = world.positions[c][size_t(i)];
        for (int i = 0; i < 3; ++i) v.normal[i] = snorm8(world.normals[c][size_t(i)]);
        for (int i = 0; i < 2; ++i) v.uv0[i] = world.uv0[c][size_t(i)], v.uv1[i] = world.uv1[c][size_t(i)];
        if (c < world.uv2.size()) {
          v.uv2[0] = float_to_half(world.uv2[c][0]);
          v.uv2[1] = float_to_half(world.uv2[c][1]);
        } else {
          v.uv2[0] = float_to_half(world.uv1[c][0]);
          v.uv2[1] = float_to_half(world.uv1[c][1]);
        }
        for (int i = 0; i < 4; ++i) v.colour[i] = world.vertex_colours[c][size_t(i)];
        if (fade_class && t < world.instance.size())
          for (int i = 0; i < 3; ++i)
            v.centre[i] = int16_t(std::clamp(std::lround((world.instance[t][size_t(i)] - v.position[i]) * 10.0f), -32767L, 32767L));
        for (int i = 0; i < 3; ++i) {
          d.box_min[i] = std::min(d.box_min[i], v.position[i]);
          d.box_max[i] = std::max(d.box_max[i], v.position[i]);
        }
        // A tree sprite's corner sits up to |uv1.x| + |uv1.y| from its base.
        if (billboard) reach = std::max(reach, std::fabs(v.uv1[0]) + std::fabs(v.uv1[1]));
        if (!share) {
          out.indices.push_back(uint32_t(out.textured_vertices.size()));
          out.textured_vertices.push_back(v);
          continue;
        }
        const auto [it, added] = shared.try_emplace(key, uint32_t(out.textured_vertices.size()));
        if (added) out.textured_vertices.push_back(v);
        out.indices.push_back(it->second);
      }
      ++out.triangles;
    }
    for (int i = 0; i < 3; ++i) d.box_min[i] -= reach, d.box_max[i] += reach;
    first = last;
    d.index_count = uint32_t(out.indices.size()) - d.first_index;
    d.texture = mat.texture;
    d.blend = mat.blend;
    d.decal = mat.decal;
    for (int k = 0; k < 3; ++k) d.push.tint[k] = tint[size_t(k)];
    d.push.tint[3] = 1;
    d.push.params[0] = float(mat.blend);
    // world.frag's flags: 1 textured, 4 vertex-blended layers, 8 splat
    // layers, 16 decal alpha from the vertex colour's R, 32 tree sprite; a textured draw's
    // tint holds the layers' tilings.
    uint32_t flags = mat.texture != kNoWorldTexture ? 1 : 0;
    if (flags && mat.layer_b != kNoWorldTexture) {
      if (mat.layers == 1) {
        flags |= 4;
        d.texture_b = mat.layer_b;
      } else if (mat.layers == 2 && mat.splat != kNoWorldTexture) {
        flags |= 8;
        d.texture_b = mat.layer_b;
        d.splat = mat.splat;
      }
    }
    if (flags && mat.layers == 3) {
      flags |= 16;
      if (mat.splat != kNoWorldTexture) {
        d.splat = mat.splat;
        flags |= 2097152u;  // world.frag kFlagDecalNoise (H_VBLND_DECAL Noise_Sampler)
      }
    }
    if (flags && mat.layers == 4) flags |= 32;  // tree sprite: turned to the camera in world.vert
    if (mat.light_map != kNoWorldTexture) {
      d.light_map = mat.light_map;
      flags |= mat.is_emissive_map ? 8388608u : 2048u;  // kFlagEmissiveMap (slot 3) vs kFlagLightMap (slot 7)
    }
    if (mat.normal_map != kNoWorldTexture) {
      d.normal_map = mat.normal_map;
      flags |= 4096u;  // world.frag kFlagNormalMap
    }
    if (mat.ao_map != kNoWorldTexture) {
      d.ao_map = mat.ao_map;
      flags |= 8192u;  // world.frag kFlagAoMap
    }
    if (flags && mat.layer_c != kNoWorldTexture) {
      d.layer_c = mat.layer_c;
      flags |= 16384u;  // world.frag kFlagLayerC
    }
    if (mat.is_water) {
      flags |= 32768u;  // world.frag kFlagWater
      if (mat.layer_b != kNoWorldTexture) {
        d.texture_b = mat.layer_b;
        flags |= 4194304u;  // world.frag kFlagWaterNormalB (LAKE_* slot 2 normal/foam map)
      }
    }
    if (mat.anim_flag) {
      flags |= 131072u;  // world.vert kFlagAnimFlag
    }
    if (mat.is_grass) {
      flags |= 262144u;  // world.vert kFlagGrass
    }
    if (mat.spec_map != kNoWorldTexture) {
      d.spec_map = mat.spec_map;
      flags |= 524288u;  // world.frag kFlagSpecMap
    }
    if (mat.is_road) {
      flags |= 1048576u;  // world.frag kFlagRoad
    }
    if (flags & 1u) std::memcpy(d.push.tint, mat.tiling.data(), sizeof d.push.tint);
    // The distance cross-fade (world.frag): 128 a prop's LOD00, 256 its
    // LOD02, 512 a prop of one level.
    if (mat.fade >= 1 && mat.fade <= 3) flags |= 64u << mat.fade;
    d.push.params[1] = float(flags);
    // Encode integer far-ring level (0, 1, 2) plus c3.w emissive_force in [0, 0.45] so
    // world_cull.comp (params.z < 0.5) and world_vert.glsl (> 0.5 / > 1.5) stay exact.
    d.push.params[2] = float(mat.far) + std::clamp(mat.emissive_force, 0.0f, 1.0f) * 0.45f;
    // params[3] carries packed half2x16: tiling_c for H_BLND3 (kFlagLayerC),
    // or decal_noise_c4 for H_VBLND_DECAL (kFlagDecalNoise); 1.0 otherwise
    // (the obsolete 2.5x H_VBLND light gain hack is removed).
    if (flags & 16384u) {
      d.push.params[3] = pack_half2x16_float(mat.tiling_c[0], mat.tiling_c[1]);
    } else if (flags & 2097152u) {
      d.push.params[3] = pack_half2x16_float(mat.decal_noise_c4[0], mat.decal_noise_c4[1]);
    } else {
      d.push.params[3] = 1.0f;
    }
    // Skip bogus paper-thin giant vertical opaque planes (e.g. FH1's 500m-tall X=0
    // _0x00002c81 light/horizon wall) that would otherwise slice across the sky.
    const float box_dx = d.box_max[0] - d.box_min[0];
    const float box_dy = d.box_max[1] - d.box_min[1];
    const float box_dz = d.box_max[2] - d.box_min[2];
    if (mat.blend == 0 && !mat.far && mat.layers == 0 && box_dy >= 200.0f &&
        (box_dx < 0.5f || box_dz < 0.5f)) {
      out.triangles -= (d.index_count / 3);
      out.indices.resize(d.first_index);
      continue;
    }
    out.draws.push_back(d);
    }
  }
  return out;
}

}  // namespace

WorldGeometry build_world(const WorldTriangles& world) {
  if (!world.material.empty()) return build_textured_world(world);
  WorldGeometry out;
  const size_t n = world.group.size();
  if (n == 0) return out;
  // One vertex per corner with the face normal as the tangent frame's
  // first axis (the shader's normal is rotate(frame, +x)); grouped by colour.
  std::vector<std::vector<uint32_t>> by_group(world.colours.size());
  for (uint32_t t = 0; t < n; ++t)
    if (world.group[t] < by_group.size()) by_group[world.group[t]].push_back(t);
  auto& vertices = out.vertices;
  auto& indices = out.indices;
  vertices.reserve(n * 3);
  indices.reserve(n * 3);
  const bool has_smooth_normals = (world.normals.size() == world.positions.size());
  for (size_t g = 0; g < by_group.size(); ++g) {
    if (by_group[g].empty()) continue;
    WorldGeometry::Draw d;
    d.first_index = uint32_t(indices.size());
    for (uint32_t t : by_group[g]) {
      const auto& a = world.positions[size_t(t) * 3];
      const auto& b = world.positions[size_t(t) * 3 + 1];
      const auto& c = world.positions[size_t(t) * 3 + 2];
      const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
      const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
      float face_n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
      const float flen = std::sqrt(face_n[0] * face_n[0] + face_n[1] * face_n[1] + face_n[2] * face_n[2]);
      if (!(flen > 0)) continue;
      for (float& k : face_n) k /= flen;
      if (face_n[1] < 0)
        for (float& k : face_n) k = -k;  // up-facing; the shader faces it to the eye anyway
      const std::array<float, 3>* pts[3] = {&a, &b, &c};
      for (int vi = 0; vi < 3; ++vi) {
        float nv[3] = {face_n[0], face_n[1], face_n[2]};
        if (has_smooth_normals) {
          const auto& sn = world.normals[size_t(t) * 3 + size_t(vi)];
          const float sl = std::sqrt(sn[0] * sn[0] + sn[1] * sn[1] + sn[2] * sn[2]);
          if (sl > 1e-5f) nv[0] = sn[0] / sl, nv[1] = sn[1] / sl, nv[2] = sn[2] / sl;
        }
        // The rotation taking +x to the normal: (x cross n, 1 + x dot n), normalised.
        float q[4] = {0, -nv[2], nv[1], 1 + nv[0]};
        if (q[3] < 1e-4f) {
          q[0] = 0;
          q[1] = 1;
          q[2] = 0;
          q[3] = 0;
        }
        const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        bake::Vertex v{};
        for (int k = 0; k < 4; ++k) v.frame[size_t(k)] = int16_t(std::lround(q[k] / ql * 32767));
        v.lighting = {100, 0, 0, 0};  // open to the sky
        v.position = *pts[vi];
        indices.push_back(uint32_t(vertices.size()));
        vertices.push_back(v);
      }
      ++out.triangles;
    }
    d.index_count = uint32_t(indices.size()) - d.first_index;
    if (d.index_count == 0) continue;
    d.push.uv0[1] = d.push.uv0[3] = d.push.uv1[1] = d.push.uv1[3] = 1;
    for (int k = 0; k < 3; ++k) d.push.tint[k] = world.colours[g][size_t(k)];
    d.push.tint[3] = 1;
    d.push.params[0] = 11;  // the shader's kWorld
    d.push.params[1] = 16;  // kFlagNoSlot0
    out.draws.push_back(d);
  }
  return out;
}

bool Showroom::set_world(const WorldTriangles& world, std::string& error) {
  return set_world(build_world(world), error);
}

bool Showroom::set_world(WorldGeometry geometry, std::string& error) {
  if (arena_on() && geometry.textured && !geometry.indices.empty()) {
    r_.wait_idle();
    ++world_frame_;
    drop_pending_world();
    drop_fading_world(true);
    StagedWorld st;
    if (!stage_world(std::move(geometry), st, error, 0, 0)) return false;
    const bool ok = begin_world(std::move(st), error) && poll_world(true, nullptr);
    drop_fading_world(true);
    return ok;
  }
  r_.wait_idle();
  ++world_frame_;
  drop_pending_world();
  drop_fading_world(true);
  r_.destroy_buffer(world_vertices_);
  r_.destroy_buffer(world_indices_);
  world_draws_.clear();
  world_triangles_ = 0;
  if (geometry.indices.empty()) {
    evict_world_textures();
    release_retired();
    return true;
  }
  if (!upload_world_textures(geometry.textures, error)) return false;
  world_textured_ = geometry.textured;
  const void* vertex_data =
      geometry.textured ? static_cast<const void*>(geometry.textured_vertices.data()) : geometry.vertices.data();
  const VkDeviceSize vertex_bytes = geometry.textured ? geometry.textured_vertices.size() * sizeof(WorldVertex)
                                                      : geometry.vertices.size() * sizeof(bake::Vertex);
  const VkBufferUsageFlags rt_geom_flags =
      r_.rt_supported()
          ? (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
          : 0;
  if (!r_.upload_buffer(vertex_data, vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | rt_geom_flags,
                        world_vertices_) ||
      !r_.upload_buffer(geometry.indices.data(), geometry.indices.size() * sizeof(uint32_t),
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | rt_geom_flags, world_indices_)) {
    error = "world buffers";
    return false;
  }
  world_draws_ = std::move(geometry.draws);
  world_triangles_ = geometry.triangles;
  evict_world_textures();
  release_retired();
  return true;
}

bool Showroom::stage_world(WorldGeometry geometry, StagedWorld& out, std::string& error, double origin_x,
                           double origin_z) {
  out = StagedWorld{};
  out.draws = std::move(geometry.draws);
  out.triangles = geometry.triangles;
  out.textured = geometry.textured;
  out.textures = std::move(geometry.textures);
  if (geometry.indices.empty()) return true;
  if (arena_on() && geometry.textured) {
    // Extract hardware-instanced tree sprite quads before index relocalization.
    for (const auto& d : out.draws) {
      const uint32_t fl = uint32_t(d.push.params[1] + 0.5f);
      if ((fl & 32u) == 0u || d.index_count < 6 || (d.index_count % 6) != 0) continue;
      for (uint32_t k = 0; k + 5 < d.index_count; k += 6) {
        const WorldVertex& v0 = geometry.textured_vertices[geometry.indices[size_t(d.first_index) + k + 0]];
        const WorldVertex& v1 = geometry.textured_vertices[geometry.indices[size_t(d.first_index) + k + 1]];
        const WorldVertex& v2 = geometry.textured_vertices[geometry.indices[size_t(d.first_index) + k + 2]];
        const WorldVertex& v3 = geometry.textured_vertices[geometry.indices[size_t(d.first_index) + k + 5]];
        if (v0.position[0] != v1.position[0] || v0.position[2] != v1.position[2]) continue;
        StagedWorld::TreeRaw tr{};
        for (int i = 0; i < 3; ++i) {
          tr.base[i] = v0.position[i];
          tr.up[i] = float(v0.normal[i]) / 127.0f;
        }
        tr.half_width = std::max(std::fabs(v0.uv1[0]), std::fabs(v1.uv1[0]));
        tr.height = std::max(v2.uv1[1], v3.uv1[1]);
        tr.uv[0] = v0.uv0[0];
        tr.uv[1] = v2.uv0[1];
        tr.uv[2] = v2.uv0[0];
        tr.uv[3] = v0.uv0[1];
        tr.rgba8 = uint32_t(v0.colour[0]) | (uint32_t(v0.colour[1]) << 8) |
                   (uint32_t(v0.colour[2]) << 16) | (uint32_t(v0.colour[3]) << 24);
        tr.texture = d.texture;
        out.trees.push_back(tr);
      }
    }
    // The resident arenas: each draw's indices made relative to its own vertices, and its content key (positions
    // made absolute, so a later gather, relative to another origin, finds the same draw).
    out.arena = true;
    for (auto& d : out.draws) {
      if (d.index_count == 0) continue;
      uint32_t lo = ~0u, hi = 0;
      for (uint32_t k = 0; k < d.index_count; ++k) {
        const uint32_t v = geometry.indices[size_t(d.first_index) + k];
        lo = std::min(lo, v), hi = std::max(hi, v);
      }
      d.vertex_begin = lo;
      d.vertex_count = hi - lo + 1;
      for (uint32_t k = 0; k < d.index_count; ++k) geometry.indices[size_t(d.first_index) + k] -= lo;
      uint64_t h = 0x9e3779b97f4a7c15ull;
      for (uint32_t t : {d.texture, d.texture_b, d.splat, d.light_map, d.normal_map, d.ao_map, d.layer_c, d.spec_map})
        h = gpuworld::hash_mix(h, t);
      h = gpuworld::hash_mix(h, uint64_t(d.blend) | uint64_t(d.decal) << 8 | uint64_t(d.cull) << 16);
      for (float f : d.push.params) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof bits);
        h = gpuworld::hash_mix(h, bits);
      }
      for (float f : d.push.tint) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof bits);
        h = gpuworld::hash_mix(h, bits);
      }
      h = gpuworld::hash_mix(h, uint64_t(d.index_count) << 32 | d.vertex_count);
      h = gpuworld::hash_bytes(h, &geometry.indices[d.first_index], size_t(d.index_count) * sizeof(uint32_t));
      for (uint32_t v = lo; v <= hi; ++v) {
        const WorldVertex& wv = geometry.textured_vertices[v];
        h = gpuworld::hash_mix(h, uint64_t(gpuworld::quantize_mm(double(wv.position[0]) + origin_x)));
        h = gpuworld::hash_mix(h, uint64_t(gpuworld::quantize_mm(double(wv.position[1]))));
        h = gpuworld::hash_mix(h, uint64_t(gpuworld::quantize_mm(double(wv.position[2]) + origin_z)));
        h = gpuworld::hash_bytes(h, reinterpret_cast<const unsigned char*>(&wv) + sizeof wv.position,
                                 sizeof wv - sizeof wv.position);
      }
      d.key = h;
    }
  }
  Renderer& r = r_;
  auto stage = [&](const void* data, VkDeviceSize size, std::shared_ptr<Renderer::Buffer>& buffer) {
    buffer = std::shared_ptr<Renderer::Buffer>(new Renderer::Buffer{}, [&r](Renderer::Buffer* b) {
      r.destroy_buffer(*b);
      delete b;
    });
    if (!r.create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, *buffer))
      return false;
    void* mapped = nullptr;
    if (vmaMapMemory(r.allocator_, buffer->allocation, &mapped) != VK_SUCCESS) return false;
    std::memcpy(mapped, data, size_t(size));
    vmaUnmapMemory(r.allocator_, buffer->allocation);
    world_staging_bytes_.fetch_add(uint64_t(size), std::memory_order_relaxed);
    return true;
  };
  const bool staged_vertices =
      geometry.textured
          ? stage(geometry.textured_vertices.data(), geometry.textured_vertices.size() * sizeof(WorldVertex),
                  out.vertices)
          : stage(geometry.vertices.data(), geometry.vertices.size() * sizeof(bake::Vertex), out.vertices);
  if (!staged_vertices ||
      !stage(geometry.indices.data(), geometry.indices.size() * sizeof(uint32_t), out.indices)) {
    error = "world staging buffers";
    return false;
  }
  // The new textures' levels, packed (each from a 16-byte boundary), honouring TextureCap.
  VkDeviceSize texel_bytes = 0;
  out.texel_ranges.assign(out.textures.size(), {0, 0});
  out.texel_skips.assign(out.textures.size(), 0);
  for (size_t i = 0; i < out.textures.size(); ++i) {
    const auto& t = out.textures[i];
    if (!t || t->texture.levels.empty()) continue;
    VkDeviceSize bytes = 0;
    const uint32_t levels = texture_chain(t->texture, bytes);
    if (!levels) continue;  // uploaded apart, which reports it
    const uint32_t skip = modern::texture_levels_to_skip(t->texture.levels[0].width, t->texture.levels[0].height,
                                                         levels, texture_cap_);
    VkDeviceSize kept_bytes = 0;
    for (uint32_t l = skip; l < levels; ++l) kept_bytes += t->texture.levels[l].data.size();
    out.texel_ranges[i] = {texel_bytes, levels};
    out.texel_skips[i] = skip;
    texel_bytes += (kept_bytes + 15) / 16 * 16;
  }
  if (texel_bytes) {
    std::vector<uint8_t> packed(size_t(texel_bytes), 0);
    for (size_t i = 0; i < out.textures.size(); ++i) {
      if (!out.texel_ranges[i].second) continue;
      size_t at = size_t(out.texel_ranges[i].first);
      for (uint32_t l = out.texel_skips[i]; l < out.texel_ranges[i].second; ++l) {
        const auto& data = out.textures[i]->texture.levels[l].data;
        std::memcpy(packed.data() + at, data.data(), data.size());
        at += data.size();
      }
    }
    if (!stage(packed.data(), texel_bytes, out.texels)) {
      out.texels.reset();  // every texture uploads apart instead
      out.texel_ranges.assign(out.textures.size(), {0, 0});
      out.texel_skips.assign(out.textures.size(), 0);
    }
  }
  // Their images and views too (thread-safe device calls; on the render
  // thread they cost a fraction of a millisecond each, tens per gather).
  if (out.texels && out.vertices && out.indices) {
    out.images = std::shared_ptr<std::vector<Renderer::Image>>(
        new std::vector<Renderer::Image>(out.textures.size()), [&r](std::vector<Renderer::Image>* v) {
          for (auto& i : *v) r.destroy_image(i);
          delete v;
        });
    for (size_t i = 0; i < out.textures.size(); ++i) {
      const auto& t = out.textures[i];
      if (!t || !out.texel_ranges[i].second) continue;
      {
        std::lock_guard<std::mutex> lock(world_textures_mutex_);
        if (world_textures_.count(t->record)) continue;
      }
      std::string why;
      if (!create_texture_image(t->texture, t->srgb, out.texel_ranges[i].second, (*out.images)[i], why,
                                out.texel_skips[i]))
        r.destroy_image((*out.images)[i]);  // begin_world tries again and reports it
    }
  }
  return true;
}


void Showroom::drop_pending_world() {
  if (!pending_world_) return;
  auto& p = *pending_world_;
  if (p.fence) {
    vkWaitForFences(r_.device_, 1, &p.fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(r_.device_, p.fence, nullptr);
  }
  if (p.fence)  // submitted and waited: the copies ran
    for (const auto& d : p.staged.draws)
      if (d.resident && arena_on()) gpu_world_->resident().mark_valid(d.key);
  world_release_draws(p.staged.draws);
  if (p.cmd) vkFreeCommandBuffers(r_.device_, r_.pool_, 1, &p.cmd);
  // Its copy is done (the fence): the staging buffers, the worker's
  // images not taken and buffers never swapped in go on the reaper.
  auto staged = std::make_shared<StagedWorld>(std::move(p.staged));
  reap([this, staged, v = p.vertices, i = p.indices]() mutable {
    r_.destroy_buffer(v);
    r_.destroy_buffer(i);
    staged.reset();
  });
  pending_world_.reset();
}

void Showroom::reap(std::function<void()> f) {
  if (!reaper_.joinable()) {
    reaper_ = std::thread([this] {
      std::unique_lock<std::mutex> lock(reap_mutex_);
      for (;;) {
        reap_cv_.wait(lock, [this] { return reap_stop_ || !reap_queue_.empty(); });
        if (reap_queue_.empty()) return;  // stopping, and drained
        auto job = std::move(reap_queue_.front());
        reap_queue_.pop_front();
        lock.unlock();
        job();
        lock.lock();
      }
    });
  }
  {
    std::lock_guard<std::mutex> lock(reap_mutex_);
    reap_queue_.push_back(std::move(f));
  }
  reap_cv_.notify_one();
}

void Showroom::retire(std::function<void()> f) { retired_.push_back({r_.frames_submitted(), std::move(f)}); }

void Showroom::release_retired() {
  const uint64_t done = r_.frames_done();
  for (size_t k = 0; k < retired_.size();) {
    if (retired_[k].first <= done) {
      reap(std::move(retired_[k].second));
      retired_.erase(retired_.begin() + ptrdiff_t(k));
    } else {
      ++k;
    }
  }
  for (size_t k = 0; k < retired_sets_.size();) {
    if (retired_sets_[k].first <= done) {
      free_world_sets_.insert(free_world_sets_.end(), retired_sets_[k].second.begin(), retired_sets_[k].second.end());
      retired_sets_.erase(retired_sets_.begin() + ptrdiff_t(k));
    } else {
      ++k;
    }
  }
}

void Showroom::stop_reaper() {
  // The GPU is idle (the caller waited): what is retired can go now.
  for (auto& [tag, f] : retired_) f();
  retired_.clear();
  retired_sets_.clear();
  free_world_sets_.clear();
  if (!reaper_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(reap_mutex_);
    reap_stop_ = true;
  }
  reap_cv_.notify_one();
  reaper_.join();
}

bool Showroom::begin_world(StagedWorld staged, std::string& error) {
  // One copy in flight at a time: a newer world replaces a pending one.
  drop_pending_world();
  // Textures staged by the worker are copied in the scenery's fenced
  // upload below; the rest (and every texture of a world with no buffers)
  // are uploaded here, one wait each.
  const bool fenced = staged.vertices && staged.indices && staged.texels;
  std::vector<std::shared_ptr<const WorldTextureData>> apart;
  for (size_t i = 0; i < staged.textures.size(); ++i)
    if (!fenced || i >= staged.texel_ranges.size() || !staged.texel_ranges[i].second)
      apart.push_back(staged.textures[i]);
  if (!upload_world_textures(apart, error)) return false;
  auto p = std::make_unique<PendingWorld>();
  p->staged = std::move(staged);
  if (p->staged.vertices && p->staged.indices) {
    const VkDeviceSize vb = p->staged.vertices->size, ib = p->staged.indices->size;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = r_.pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    const bool arena = p->staged.arena && arena_on();
    std::vector<WorldGpu::Copy> arena_vertex_copies, arena_index_copies;
    if (arena && !place_world_arena(p->staged, arena_vertex_copies, arena_index_copies, error)) {
      // Long-distance teleports (e.g. New Career / Fast Travel) can temporarily exceed the
      // resident arena while old cells are still live. Flush old/fading draws and retry once.
      r_.wait_idle();
      drop_fading_world(true);
      for (auto& d : world_draws_) {
        if (d.resident) gpu_world_->resident().release(d.key, 0);
      }
      world_draws_.clear();
      arena_vertex_copies.clear();
      arena_index_copies.clear();
      error.clear();
      if (!place_world_arena(p->staged, arena_vertex_copies, arena_index_copies, error)) {
        pending_world_ = std::move(p);
        drop_pending_world();
        return false;
      }
    }
    if (arena) {
      // build_textured_world() computes d.box_min/max in p->staged's own origin frame
      // (game_pos - st.origin). place_world_arena() then adds d.shift (rd->origin - st.origin)
      // to d.box_min/max, which double-shifts reused resident draws by rd->origin - st.origin
      // (~200+ m after a streaming regather) and causes world_cull.comp & CPU frustum culling
      // to drop road junctions and terrain chunks directly under the player's car.
      // Undo that extra shift so d.box_min/max stay in p->staged's scenery frame.
      for (auto& d : p->staged.draws) {
        if (!d.resident) continue;
        for (int k = 0; k < 3; k += 2) {
          d.box_min[k] -= d.shift[k / 2];
          d.box_max[k] -= d.shift[k / 2];
        }
      }
    }
    const VkBufferUsageFlags world_rt_flags =
        r_.rt_supported()
            ? (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
            : 0u;
    if ((!arena && (!r_.create_buffer(vb, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | world_rt_flags,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, p->vertices) ||
                    !r_.create_buffer(ib, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | world_rt_flags,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, p->indices))) ||
        vkAllocateCommandBuffers(r_.device_, &alloc, &p->cmd) != VK_SUCCESS ||
        vkCreateFence(r_.device_, &fence, nullptr, &p->fence) != VK_SUCCESS) {
      pending_world_ = std::move(p);
      drop_pending_world();
      error = "world buffers";
      return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(p->cmd, &begin);
    if (arena) {
      // Only the draws the arenas do not hold yet.
      gpu_world_->record_copies(p->cmd, p->staged.vertices->buffer, arena_vertex_copies, p->staged.indices->buffer,
                                arena_index_copies);
    } else {
      const VkBufferCopy v{0, 0, vb}, i{0, 0, ib};
      vkCmdCopyBuffer(p->cmd, p->staged.vertices->buffer, p->vertices.buffer, 1, &v);
      vkCmdCopyBuffer(p->cmd, p->staged.indices->buffer, p->indices.buffer, 1, &i);
    }
    if (fenced) {
      const auto& st = p->staged;
      for (size_t k = 0; k < st.textures.size() && k < st.texel_ranges.size(); ++k) {
        const auto& t = st.textures[k];
        if (!t || !st.texel_ranges[k].second) continue;
        {
          std::lock_guard<std::mutex> lock(world_textures_mutex_);
          if (world_textures_.count(t->record)) continue;
        }
        std::string why;
        Renderer::Image* made = st.images && k < st.images->size() ? &(*st.images)[k] : nullptr;
        const uint32_t skip = k < st.texel_skips.size() ? st.texel_skips[k] : 0u;
        if (!begin_world_texture(*t, st.texels->buffer, st.texel_ranges[k].first, st.texel_ranges[k].second, skip,
                                 p->cmd, made, why))
          notes_.push_back("world texture " + t->texture.name + ": " + why);  // drawn flat
      }
    }
    vkEndCommandBuffer(p->cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &p->cmd;
    VkResult submitted;
    {
      std::lock_guard<std::mutex> qlock(r_.queue_mutex_);
      submitted = vkQueueSubmit(r_.queue_, 1, &submit, p->fence);
    }
    if (submitted != VK_SUCCESS) {
      vkDestroyFence(r_.device_, p->fence, nullptr);
      p->fence = VK_NULL_HANDLE;
      pending_world_ = std::move(p);
      drop_pending_world();
      error = "world upload submit";
      return false;
    }
  }
  pending_world_ = std::move(p);
  evict_world_textures();
  return true;
}

bool Showroom::poll_world(bool wait, bool* swapped) {
  if (swapped) *swapped = false;
  release_retired();
  if (!pending_world_) return true;
  auto& p = *pending_world_;
  if (p.fence) {
    if (!wait && vkGetFenceStatus(r_.device_, p.fence) != VK_SUCCESS) return true;
    vkWaitForFences(r_.device_, 1, &p.fence, VK_TRUE, UINT64_MAX);
  }
  // The scenery in use may still be read by the frame in flight: it goes
  // once that frame is done, off this thread (no device wait here). With a
  // swap fade it is kept and drawn dithered under the new one first (ours:
  // a regather's changes cross-fade rather than pop).
  drop_fading_world(false);
  if (p.staged.arena && arena_on()) {
    // Resident draws: the new scenery's references were taken in begin_world. What the old scenery drew that the
    // new one does not is kept, dithered out; the rest of its references go (the arenas keep the content cached).
    const uint64_t tag = r_.frames_submitted();
    std::unordered_set<uint64_t> new_keys;
    for (const auto& d : p.staged.draws)
      if (d.resident) {
        new_keys.insert(d.key);
        gpu_world_->resident().mark_valid(d.key);  // the copy is done (the fence above)
      }
    std::unordered_set<uint64_t> old_keys;
    for (const auto& d : world_draws_)
      if (d.resident) old_keys.insert(d.key);
    const bool fade = swap_fade_seconds_ > 0 && world_textured_ && !world_draws_.empty();
    fading_ = FadingWorld{};
    fading_.arena = true;
    fading_.origin[0] = world_origin_[0];
    fading_.origin[1] = world_origin_[1];
    for (auto& d : world_draws_) {
      if (!d.resident) continue;
      if (fade && d.cull && !new_keys.count(d.key)) {
        d.fade_in = false;
        fading_.draws.push_back(d);  // keeps its reference until the fade ends
      } else {
        gpu_world_->resident().release(d.key, tag);
      }
    }
    world_draws_ = std::move(p.staged.draws);
    for (auto& d : world_draws_) d.fade_in = fade && d.resident && d.cull && !old_keys.count(d.key);
    if (!fading_.draws.empty()) {
      fading_on_ = true;
      fade_start_ = std::chrono::steady_clock::now();
    }
    world_triangles_ = p.staged.triangles;
    world_textured_ = p.staged.textured;
    {
      std::lock_guard<std::mutex> lock(world_textures_mutex_);
      for (const auto& d : world_draws_) {
        if (d.texture != kNoWorldTexture) world_lru_.touch(d.texture, world_frame_);
        if (d.texture_b != kNoWorldTexture) world_lru_.touch(d.texture_b, world_frame_);
        if (d.splat != kNoWorldTexture) world_lru_.touch(d.splat, world_frame_);
        if (d.light_map != kNoWorldTexture) world_lru_.touch(d.light_map, world_frame_);
        if (d.normal_map != kNoWorldTexture) world_lru_.touch(d.normal_map, world_frame_);
        if (d.ao_map != kNoWorldTexture) world_lru_.touch(d.ao_map, world_frame_);
        if (d.layer_c != kNoWorldTexture) world_lru_.touch(d.layer_c, world_frame_);
        if (d.spec_map != kNoWorldTexture) world_lru_.touch(d.spec_map, world_frame_);
      }
    }
    const bool origin_changed = (world_origin_[0] != p.staged.origin_x || world_origin_[1] != p.staged.origin_z);
    world_origin_[0] = p.staged.origin_x;
    world_origin_[1] = p.staged.origin_z;
    world_trees_ = std::move(p.staged.trees);
    if (origin_changed) {
      gpu_world_->invalidate_hiz();
      have_prev_vp_ = false;
    }
    std::string table_error;
    if (!build_gpu_table(table_error)) notes_.push_back("world draw table: " + table_error);
    drop_pending_world();
    evict_world_textures();
    release_retired();
    if (swapped) *swapped = true;
    return true;
  }
  if (swap_fade_seconds_ > 0 && world_textured_ && p.staged.textured && world_vertices_.buffer && p.vertices.buffer) {
    fading_.vertices = world_vertices_;
    fading_.indices = world_indices_;
    fading_.draws = std::move(world_draws_);
    fading_.origin[0] = world_origin_[0];
    fading_.origin[1] = world_origin_[1];
    fading_on_ = true;
    fade_start_ = std::chrono::steady_clock::now();
  } else {
    retire([this, v = world_vertices_, i = world_indices_]() mutable {
      r_.destroy_buffer(v);
      r_.destroy_buffer(i);
    });
  }
  world_vertices_ = p.vertices;
  world_indices_ = p.indices;
  p.vertices = {};
  p.indices = {};
  if (world_vertices_.buffer) {
    world_draws_ = std::move(p.staged.draws);
    world_triangles_ = p.staged.triangles;
    world_textured_ = p.staged.textured;
    std::lock_guard<std::mutex> lock(world_textures_mutex_);
    for (const auto& d : world_draws_) {
      if (d.texture != kNoWorldTexture) world_lru_.touch(d.texture, world_frame_);
      if (d.texture_b != kNoWorldTexture) world_lru_.touch(d.texture_b, world_frame_);
      if (d.splat != kNoWorldTexture) world_lru_.touch(d.splat, world_frame_);
      if (d.light_map != kNoWorldTexture) world_lru_.touch(d.light_map, world_frame_);
      if (d.normal_map != kNoWorldTexture) world_lru_.touch(d.normal_map, world_frame_);
      if (d.ao_map != kNoWorldTexture) world_lru_.touch(d.ao_map, world_frame_);
      if (d.layer_c != kNoWorldTexture) world_lru_.touch(d.layer_c, world_frame_);
      if (d.spec_map != kNoWorldTexture) world_lru_.touch(d.spec_map, world_frame_);
    }
  } else {
    world_draws_.clear();
    world_triangles_ = 0;
  }
  world_origin_[0] = p.staged.origin_x;
  world_origin_[1] = p.staged.origin_z;
  drop_pending_world();
  evict_world_textures();
  release_retired();
  if (swapped) *swapped = true;
  return true;
}

bool Showroom::set_world(StagedWorld staged, std::string& error) {
  return begin_world(std::move(staged), error) && poll_world(true, nullptr);
}

void Showroom::drop_fading_world(bool now) {
  if (!fading_on_) return;
  fading_on_ = false;
  if (fading_.arena) {
    world_release_draws(fading_.draws);
    fading_.draws.clear();
    fading_.arena = false;
    if (!now) evict_world_textures();
    return;
  }
  fading_.draws.clear();
  if (now) {
    r_.destroy_buffer(fading_.vertices);
    r_.destroy_buffer(fading_.indices);
  } else {
    retire([this, v = fading_.vertices, i = fading_.indices]() mutable {
      r_.destroy_buffer(v);
      r_.destroy_buffer(i);
    });
    evict_world_textures();
  }
  fading_.vertices = {};
  fading_.indices = {};
}


}  // namespace carview
