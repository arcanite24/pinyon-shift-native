#include "drive/mesh_ground.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace drive {

namespace {

constexpr uint32_t kLeafSize = 8;

}  // namespace

MeshGround::MeshGround(std::vector<std::array<float, 3>> positions, std::vector<float> grip,
                       std::vector<uint8_t> surface)
    : positions_(std::move(positions)), grip_(std::move(grip)), surface_(std::move(surface)) {
  const uint32_t n = uint32_t(positions_.size() / 3);
  positions_.resize(size_t(n) * 3);
  if (grip_.size() != n) grip_.assign(n, 1.0f);
  order_.resize(n);
  centroid_.resize(n);
  for (uint32_t t = 0; t < n; ++t) {
    order_[t] = t;
    for (int k = 0; k < 3; ++k)
      centroid_[t][size_t(k)] =
          (positions_[t * 3][size_t(k)] + positions_[t * 3 + 1][size_t(k)] + positions_[t * 3 + 2][size_t(k)]) / 3;
  }
  nodes_.reserve(n ? 2 * (n / kLeafSize + 1) : 1);
  if (n) build(0, n, 1);
  centroid_.clear();
  centroid_.shrink_to_fit();
}

// Median split on the longest axis of the centroids' box.
uint32_t MeshGround::build(uint32_t first, uint32_t count, int depth) {
  depth_ = std::max(depth_, depth);
  const uint32_t index = uint32_t(nodes_.size());
  nodes_.push_back({});
  Node node;
  node.lo = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
  node.hi = {-node.lo[0], -node.lo[1], -node.lo[2]};
  std::array<float, 3> clo = node.lo, chi = node.hi;
  for (uint32_t i = first; i < first + count; ++i) {
    const uint32_t t = order_[i];
    for (int c = 0; c < 3; ++c)
      for (int k = 0; k < 3; ++k) {
        node.lo[size_t(k)] = std::min(node.lo[size_t(k)], positions_[t * 3 + uint32_t(c)][size_t(k)]);
        node.hi[size_t(k)] = std::max(node.hi[size_t(k)], positions_[t * 3 + uint32_t(c)][size_t(k)]);
      }
    for (int k = 0; k < 3; ++k) {
      clo[size_t(k)] = std::min(clo[size_t(k)], centroid_[t][size_t(k)]);
      chi[size_t(k)] = std::max(chi[size_t(k)], centroid_[t][size_t(k)]);
    }
  }
  int axis = 0;
  for (int k = 1; k < 3; ++k)
    if (chi[size_t(k)] - clo[size_t(k)] > chi[size_t(axis)] - clo[size_t(axis)]) axis = k;
  if (count <= kLeafSize || !(chi[size_t(axis)] > clo[size_t(axis)])) {
    node.first = first;
    node.count = count;
    nodes_[index] = node;
    return index;
  }
  const uint32_t half = count / 2;
  std::nth_element(order_.begin() + first, order_.begin() + first + half, order_.begin() + first + count,
                   [&](uint32_t a, uint32_t b) { return centroid_[a][size_t(axis)] < centroid_[b][size_t(axis)]; });
  nodes_[index] = node;
  const uint32_t left = build(first, half, depth + 1);
  const uint32_t right = build(first + half, count - half, depth + 1);
  nodes_[index].left = left;
  nodes_[index].right = right;
  return index;
}

bool MeshGround::raycast(const Vec3& origin, const Vec3& dir, double max_distance, Hit& hit) const {
  if (nodes_.empty()) return false;
  const double inv[3] = {1 / dir.x, 1 / dir.y, 1 / dir.z};
  const double o[3] = {origin.x, origin.y, origin.z};
  double best = max_distance;
  int64_t best_t = -1;
  Vec3 best_n{};
  uint32_t stack[64];
  int top = 0;
  stack[top++] = 0;
  while (top > 0) {
    const Node& node = nodes_[stack[--top]];
    // Slab test against the node's box.
    double t0 = 0, t1 = best;
    for (int k = 0; k < 3; ++k) {
      double a = (node.lo[size_t(k)] - o[k]) * inv[k], b = (node.hi[size_t(k)] - o[k]) * inv[k];
      if (a > b) std::swap(a, b);
      if (std::isnan(a)) a = -std::numeric_limits<double>::infinity();
      if (std::isnan(b)) b = std::numeric_limits<double>::infinity();
      t0 = std::max(t0, a);
      t1 = std::min(t1, b);
    }
    if (t0 > t1) continue;
    if (node.count == 0) {
      if (top + 2 > 64) continue;  // deeper than any median split of 2^32 triangles
      stack[top++] = node.left;
      stack[top++] = node.right;
      continue;
    }
    for (uint32_t i = node.first; i < node.first + node.count; ++i) {
      const uint32_t t = order_[i];
      const auto& a = positions_[t * 3];
      const auto& b = positions_[t * 3 + 1];
      const auto& c = positions_[t * 3 + 2];
      // Moller-Trumbore in double, relative to the first corner.
      const Vec3 pa{a[0], a[1], a[2]};
      const Vec3 e1 = Vec3{b[0], b[1], b[2]} - pa, e2 = Vec3{c[0], c[1], c[2]} - pa;
      const Vec3 p = cross(dir, e2);
      const double det = dot(e1, p);
      if (std::fabs(det) < 1e-12) continue;
      const double id = 1 / det;
      const Vec3 s = origin - pa;
      const double u = dot(s, p) * id;
      if (u < 0 || u > 1) continue;
      const Vec3 q = cross(s, e1);
      const double v = dot(dir, q) * id;
      if (v < 0 || u + v > 1) continue;
      const double d = dot(e2, q) * id;
      if (d < 0 || d >= best) continue;
      best = d;
      best_t = t;
      best_n = cross(e1, e2);
    }
  }
  if (best_t < 0) return false;
  const double len = std::sqrt(dot(best_n, best_n));
  Vec3 n = best_n * (1 / len);
  if (dot(n, dir) > 0) n = n * -1.0;
  hit.distance = best;
  hit.normal = n;
  hit.grip = grip_[size_t(best_t)];
  hit.surface = surface_.size() == grip_.size() ? int(surface_[size_t(best_t)]) : -1;
  hit.triangle = best_t;
  hit.tyre = hit.surface >= 0 && size_t(hit.surface) < tyres_.size() ? tyres_[size_t(hit.surface)] : SurfaceTyre{};
  return true;
}

namespace {

// The point of triangle abc nearest p (Ericson, Real-Time Collision
// Detection, 5.1.5).
Vec3 closest_on_triangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
  const Vec3 ab = b - a, ac = c - a, ap = p - a;
  const double d1 = dot(ab, ap), d2 = dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) return a;
  const Vec3 bp = p - b;
  const double d3 = dot(ab, bp), d4 = dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return b;
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
  const Vec3 cp = p - c;
  const double d5 = dot(ab, cp), d6 = dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) return c;
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  const double denom = 1 / (va + vb + vc);
  return a + ab * (vb * denom) + ac * (vc * denom);
}

}  // namespace

void MeshGround::sphere_contacts(const Vec3& inside, const SphereQuery* spheres, int n, SphereHit* hits) const {
  for (int i = 0; i < n; ++i) hits[i] = {};
  if (nodes_.empty() || n <= 0) return;
  // One gather for all the spheres: the box around them and `inside`.
  double lo[3] = {inside.x, inside.y, inside.z}, hi[3] = {inside.x, inside.y, inside.z};
  for (int i = 0; i < n; ++i) {
    const double c[3] = {spheres[i].centre.x, spheres[i].centre.y, spheres[i].centre.z};
    for (int k = 0; k < 3; ++k) {
      lo[k] = std::min(lo[k], c[k] - spheres[i].radius);
      hi[k] = std::max(hi[k], c[k] + spheres[i].radius);
    }
  }
  thread_local std::vector<uint32_t> near;
  near.clear();
  uint32_t stack[64];
  int top = 0;
  stack[top++] = 0;
  while (top > 0) {
    const Node& node = nodes_[stack[--top]];
    bool overlap = true;
    for (int k = 0; k < 3 && overlap; ++k) overlap = node.lo[size_t(k)] <= hi[k] && node.hi[size_t(k)] >= lo[k];
    if (!overlap) continue;
    if (node.count == 0) {
      if (top + 2 > 64) continue;
      stack[top++] = node.left;
      stack[top++] = node.right;
      continue;
    }
    for (uint32_t j = node.first; j < node.first + node.count; ++j) near.push_back(order_[j]);
  }
  for (const uint32_t t : near) {
    const auto& fa = positions_[t * 3];
    const auto& fb = positions_[t * 3 + 1];
    const auto& fc = positions_[t * 3 + 2];
    const Vec3 a{fa[0], fa[1], fa[2]}, b{fb[0], fb[1], fb[2]}, c{fc[0], fc[1], fc[2]};
    Vec3 nt = cross(b - a, c - a);
    const double len = std::sqrt(dot(nt, nt));
    if (len < 1e-12) continue;
    nt = nt * (1 / len);
    // The face the body is on.
    if (dot(inside - a, nt) < 0) nt = nt * -1.0;
    for (int i = 0; i < n; ++i) {
      const Vec3 centre = spheres[i].centre;
      const double r = spheres[i].radius;
      const Vec3 q = closest_on_triangle(centre, a, b, c);
      const Vec3 dq = centre - q;
      const double d2 = dot(dq, dq);
      const double side = dot(centre - a, nt);
      double depth = 0;
      Vec3 normal = nt;
      if (side >= 0) {
        if (d2 >= r * r) continue;
        const double d = std::sqrt(d2);
        depth = r - d;
        if (d > 1e-6) normal = dq * (1 / d);
      } else {
        // The centre is behind the face: pushed back only when the way from
        // `inside` crosses this triangle.
        const double s0 = dot(inside - a, nt);
        const double f = s0 / (s0 - side);
        const Vec3 x = inside + (centre - inside) * f;
        const Vec3 qx = closest_on_triangle(x, a, b, c);
        if (dot(x - qx, x - qx) > 1e-8) continue;
        depth = r - side;
      }
      SphereHit& h = hits[i];
      if (depth <= h.depth) continue;
      h.point = q;
      h.normal = normal;
      h.depth = depth;
      h.grip = grip_[size_t(t)];
      h.surface = surface_.size() == grip_.size() ? int(surface_[size_t(t)]) : -1;
      h.triangle = t;
      h.tyre = h.surface >= 0 && size_t(h.surface) < tyres_.size() ? tyres_[size_t(h.surface)] : SurfaceTyre{};
    }
  }
}

}  // namespace drive
