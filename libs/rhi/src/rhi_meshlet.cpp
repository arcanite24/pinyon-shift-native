#include "rhi/meshlet.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace shift::rhi {
namespace {

struct Vec3f {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec3f operator+(Vec3f a, Vec3f b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3f operator-(Vec3f a, Vec3f b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3f operator*(Vec3f a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }

inline float dot3(Vec3f a, Vec3f b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3f cross3(Vec3f a, Vec3f b) noexcept {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length3(Vec3f a) noexcept { return std::sqrt(dot3(a, a)); }
inline Vec3f normalize3(Vec3f a) noexcept {
  const float len = length3(a);
  return len > 1e-8f ? a * (1.0f / len) : Vec3f{0.0f, 1.0f, 0.0f};
}

inline Vec3f load_pos(const VisVertexGpu& v) noexcept {
  return {v.pos[0], v.pos[1], v.pos[2]};
}

void compute_meshlet_bounds(std::span<const VisVertexGpu> vertices,
                            std::span<const uint32_t> tri_indices,
                            MeshletGpu& out) noexcept {
  if (tri_indices.size() < 3) return;

  // Tight AABB -> Ritter-refined bounding sphere.
  Vec3f lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
           std::numeric_limits<float>::max()};
  Vec3f hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
           -std::numeric_limits<float>::max()};
  for (uint32_t vi : tri_indices) {
    if (vi >= vertices.size()) continue;
    const Vec3f p = load_pos(vertices[vi]);
    lo.x = std::min(lo.x, p.x);
    lo.y = std::min(lo.y, p.y);
    lo.z = std::min(lo.z, p.z);
    hi.x = std::max(hi.x, p.x);
    hi.y = std::max(hi.y, p.y);
    hi.z = std::max(hi.z, p.z);
  }
  Vec3f center = (lo + hi) * 0.5f;
  float radius_sq = 0.0f;
  for (uint32_t vi : tri_indices) {
    if (vi >= vertices.size()) continue;
    const Vec3f d = load_pos(vertices[vi]) - center;
    radius_sq = std::max(radius_sq, dot3(d, d));
  }
  const float radius = std::sqrt(radius_sq);
  out.bound_center[0] = center.x;
  out.bound_center[1] = center.y;
  out.bound_center[2] = center.z;
  out.bound_radius = radius;

  // Backface normal cone (cluster culling): average triangle normals and find min dot.
  Vec3f avg_normal{0.0f, 0.0f, 0.0f};
  const size_t tri_count = tri_indices.size() / 3;
  std::vector<Vec3f> tri_normals;
  tri_normals.reserve(tri_count);
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = tri_indices[t * 3 + 0];
    const uint32_t i1 = tri_indices[t * 3 + 1];
    const uint32_t i2 = tri_indices[t * 3 + 2];
    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;
    const Vec3f p0 = load_pos(vertices[i0]);
    const Vec3f p1 = load_pos(vertices[i1]);
    const Vec3f p2 = load_pos(vertices[i2]);
    const Vec3f n = cross3(p1 - p0, p2 - p0);
    const float len = length3(n);
    if (len > 1e-8f) {
      const Vec3f nn = n * (1.0f / len);
      tri_normals.push_back(nn);
      avg_normal = avg_normal + nn;
    }
  }

  out.cone_apex[0] = center.x;
  out.cone_apex[1] = center.y;
  out.cone_apex[2] = center.z;
  if (tri_normals.empty() || length3(avg_normal) < 1e-5f) {
    out.cone_axis[0] = 0.0f;
    out.cone_axis[1] = 1.0f;
    out.cone_axis[2] = 0.0f;
    out.cone_cutoff = 1.0f;  // cannot backface-cull
    return;
  }

  const Vec3f axis = normalize3(avg_normal);
  float min_dot = 1.0f;
  for (const Vec3f& n : tri_normals) {
    min_dot = std::min(min_dot, dot3(n, axis));
  }
  out.cone_axis[0] = axis.x;
  out.cone_axis[1] = axis.y;
  out.cone_axis[2] = axis.z;

  if (min_dot <= 0.1f) {
    out.cone_cutoff = 1.0f;  // hemisphere or wider -> never backface-cull
    return;
  }
  // Safe backface cutoff sin(half_angle) = sqrt(1 - min_dot^2) for sphere-centered cone test:
  // dot(normalize(center - cam_pos), cone_axis) >= cone_cutoff * length + radius.
  out.cone_cutoff = std::sqrt(std::max(0.0f, 1.0f - min_dot * min_dot));
}

}  // namespace

MeshletBuildOutput build_meshlets(std::span<const VisVertexGpu> vertices,
                                  std::span<const uint32_t> indices,
                                  uint32_t material_id,
                                  uint32_t global_vertex_offset,
                                  uint32_t global_index_offset,
                                  uint32_t max_vertices,
                                  uint32_t max_triangles) {
  MeshletBuildOutput out;
  const size_t tri_count = indices.size() / 3;
  if (tri_count == 0 || vertices.empty()) return out;

  max_vertices = std::clamp(max_vertices, 3u, kMaxMeshletVertices);
  max_triangles = std::clamp(max_triangles, 1u, kMaxMeshletTriangles);

  // Compute triangle centroids and Morton-like spatial ordering so triangles within
  // each meshlet stay tightly clustered in 3D space (minimizing bounding sphere radius).
  struct TriSort {
    uint32_t tri_idx = 0;
    uint32_t code = 0;
  };
  Vec3f lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
           std::numeric_limits<float>::max()};
  Vec3f hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
           -std::numeric_limits<float>::max()};
  std::vector<Vec3f> centroids(tri_count);
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = indices[t * 3 + 0];
    const uint32_t i1 = indices[t * 3 + 1];
    const uint32_t i2 = indices[t * 3 + 2];
    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;
    const Vec3f c = (load_pos(vertices[i0]) + load_pos(vertices[i1]) + load_pos(vertices[i2])) * (1.0f / 3.0f);
    centroids[t] = c;
    lo.x = std::min(lo.x, c.x);
    lo.y = std::min(lo.y, c.y);
    lo.z = std::min(lo.z, c.z);
    hi.x = std::max(hi.x, c.x);
    hi.y = std::max(hi.y, c.y);
    hi.z = std::max(hi.z, c.z);
  }

  auto part1by2 = [](uint32_t v) noexcept -> uint32_t {
    v &= 0x3FFu;
    v = (v ^ (v << 16)) & 0x030000FFu;
    v = (v ^ (v << 8)) & 0x0300F00Fu;
    v = (v ^ (v << 4)) & 0x030C30C3u;
    v = (v ^ (v << 2)) & 0x09249249u;
    return v;
  };

  const Vec3f inv_ext{
      (hi.x > lo.x + 1e-6f) ? 1023.0f / (hi.x - lo.x) : 0.0f,
      (hi.y > lo.y + 1e-6f) ? 1023.0f / (hi.y - lo.y) : 0.0f,
      (hi.z > lo.z + 1e-6f) ? 1023.0f / (hi.z - lo.z) : 0.0f,
  };

  std::vector<TriSort> order(tri_count);
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t qx = static_cast<uint32_t>(std::clamp((centroids[t].x - lo.x) * inv_ext.x, 0.0f, 1023.0f));
    const uint32_t qy = static_cast<uint32_t>(std::clamp((centroids[t].y - lo.y) * inv_ext.y, 0.0f, 1023.0f));
    const uint32_t qz = static_cast<uint32_t>(std::clamp((centroids[t].z - lo.z) * inv_ext.z, 0.0f, 1023.0f));
    order[t] = {static_cast<uint32_t>(t), (part1by2(qx) << 2) | (part1by2(qy) << 1) | part1by2(qz)};
  }
  std::stable_sort(order.begin(), order.end(),
                   [](const TriSort& a, const TriSort& b) { return a.code < b.code; });

  out.reordered_indices.reserve(indices.size());
  std::vector<uint32_t> cur_tris;
  cur_tris.reserve(max_triangles * 3);
  std::unordered_set<uint32_t> cur_verts;
  cur_verts.reserve(max_vertices * 2);

  auto flush_meshlet = [&]() {
    if (cur_tris.empty()) return;
    MeshletGpu m{};
    m.index_offset = global_index_offset + static_cast<uint32_t>(out.reordered_indices.size());
    m.vertex_offset = global_vertex_offset;
    m.triangle_count = static_cast<uint32_t>(cur_tris.size() / 3);
    m.vertex_count = static_cast<uint32_t>(cur_verts.size());
    m.material_id = material_id;
    compute_meshlet_bounds(vertices, cur_tris, m);
    out.reordered_indices.insert(out.reordered_indices.end(), cur_tris.begin(), cur_tris.end());
    out.meshlets.push_back(m);
    cur_tris.clear();
    cur_verts.clear();
  };

  for (const TriSort& ts : order) {
    const uint32_t i0 = indices[ts.tri_idx * 3 + 0];
    const uint32_t i1 = indices[ts.tri_idx * 3 + 1];
    const uint32_t i2 = indices[ts.tri_idx * 3 + 2];
    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

    uint32_t added_verts = 0;
    if (cur_verts.find(i0) == cur_verts.end()) ++added_verts;
    if (i1 != i0 && cur_verts.find(i1) == cur_verts.end()) ++added_verts;
    if (i2 != i0 && i2 != i1 && cur_verts.find(i2) == cur_verts.end()) ++added_verts;

    if (!cur_tris.empty() &&
        (cur_tris.size() / 3 + 1 > max_triangles || cur_verts.size() + added_verts > max_vertices)) {
      flush_meshlet();
    }

    cur_verts.insert(i0);
    cur_verts.insert(i1);
    cur_verts.insert(i2);
    cur_tris.push_back(i0);
    cur_tris.push_back(i1);
    cur_tris.push_back(i2);
  }
  flush_meshlet();

  return out;
}

void extract_frustum_planes(const float m[16], float out_planes[6][4]) noexcept {
  // Column-major 4x4 matrix M: row r is (m[0+r], m[4+r], m[8+r], m[12+r]).
  auto set_plane = [&](int idx, float a, float b, float c, float d) {
    const float len = std::sqrt(a * a + b * b + c * c);
    const float inv = len > 1e-8f ? 1.0f / len : 1.0f;
    out_planes[idx][0] = a * inv;
    out_planes[idx][1] = b * inv;
    out_planes[idx][2] = c * inv;
    out_planes[idx][3] = d * inv;
  };
  const float r0[4] = {m[0], m[4], m[8], m[12]};
  const float r1[4] = {m[1], m[5], m[9], m[13]};
  const float r2[4] = {m[2], m[6], m[10], m[14]};
  const float r3[4] = {m[3], m[7], m[11], m[15]};

  set_plane(0, r3[0] + r0[0], r3[1] + r0[1], r3[2] + r0[2], r3[3] + r0[3]);  // Left
  set_plane(1, r3[0] - r0[0], r3[1] - r0[1], r3[2] - r0[2], r3[3] - r0[3]);  // Right
  set_plane(2, r3[0] + r1[0], r3[1] + r1[1], r3[2] + r1[2], r3[3] + r1[3]);  // Bottom
  set_plane(3, r3[0] - r1[0], r3[1] - r1[1], r3[2] - r1[2], r3[3] - r1[3]);  // Top
  set_plane(4, r2[0],         r2[1],         r2[2],         r2[3]);          // Near (z >= 0)
  set_plane(5, r3[0] - r2[0], r3[1] - r2[1], r3[2] - r2[2], r3[3] - r2[3]);  // Far (z <= w)
}

}  // namespace shift::rhi
