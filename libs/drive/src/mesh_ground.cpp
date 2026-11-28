#include "drive/mesh_ground.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define SHIFT_BVH_SSE 1
#include <xmmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define SHIFT_BVH_NEON 1
#include <arm_neon.h>
#endif

namespace drive {

namespace {

constexpr uint32_t kLeafSize = 8;
constexpr int32_t kEmptySlot = std::numeric_limits<int32_t>::min();
constexpr float kBoxPad = 1e-4f;  // guards double->float boundary rounding at large world coordinates

struct BuildNode {
  std::array<float, 3> lo{};
  std::array<float, 3> hi{};
  uint32_t first = 0;
  uint32_t count = 0;  // >0 => leaf
  uint32_t left = 0;
  uint32_t right = 0;
};

uint32_t build_binary(const std::vector<std::array<float, 3>>& positions,
                      const std::vector<std::array<float, 3>>& centroids, std::vector<uint32_t>& order,
                      std::vector<BuildNode>& nodes, uint32_t first, uint32_t count) {
  const uint32_t index = uint32_t(nodes.size());
  nodes.push_back({});
  BuildNode node;
  node.lo = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
  node.hi = {-node.lo[0], -node.lo[1], -node.lo[2]};
  std::array<float, 3> clo = node.lo, chi = node.hi;
  for (uint32_t i = first; i < first + count; ++i) {
    const uint32_t t = order[i];
    for (int c = 0; c < 3; ++c) {
      const auto& v = positions[size_t(t) * 3 + size_t(c)];
      for (int k = 0; k < 3; ++k) {
        node.lo[size_t(k)] = std::min(node.lo[size_t(k)], v[size_t(k)]);
        node.hi[size_t(k)] = std::max(node.hi[size_t(k)], v[size_t(k)]);
      }
    }
    const auto& ct = centroids[t];
    for (int k = 0; k < 3; ++k) {
      clo[size_t(k)] = std::min(clo[size_t(k)], ct[size_t(k)]);
      chi[size_t(k)] = std::max(chi[size_t(k)], ct[size_t(k)]);
    }
  }
  int axis = 0;
  for (int k = 1; k < 3; ++k) {
    if (chi[size_t(k)] - clo[size_t(k)] > chi[size_t(axis)] - clo[size_t(axis)]) axis = k;
  }
  if (count <= kLeafSize || !(chi[size_t(axis)] > clo[size_t(axis)])) {
    node.first = first;
    node.count = count;
    nodes[index] = node;
    return index;
  }
  const uint32_t half = count / 2;
  std::nth_element(order.begin() + ptrdiff_t(first), order.begin() + ptrdiff_t(first + half),
                   order.begin() + ptrdiff_t(first + count),
                   [&](uint32_t a, uint32_t b) { return centroids[a][size_t(axis)] < centroids[b][size_t(axis)]; });
  nodes[index] = node;
  const uint32_t left = build_binary(positions, centroids, order, nodes, first, half);
  const uint32_t right = build_binary(positions, centroids, order, nodes, first + half, count - half);
  nodes[index].left = left;
  nodes[index].right = right;
  return index;
}

inline bool exact_ray_tri(const Vec3& origin, const Vec3& dir, const std::array<float, 3>& a,
                          const std::array<float, 3>& b, const std::array<float, 3>& c, double best, double& out_d,
                          Vec3& out_n) {
  const Vec3 pa{a[0], a[1], a[2]};
  const Vec3 e1 = Vec3{b[0], b[1], b[2]} - pa;
  const Vec3 e2 = Vec3{c[0], c[1], c[2]} - pa;
  const Vec3 p = cross(dir, e2);
  const double det = dot(e1, p);
  if (!(std::fabs(det) >= 1e-12)) return false;
  const double id = 1.0 / det;
  const Vec3 s = origin - pa;
  const double u = dot(s, p) * id;
  if (!(u >= 0.0 && u <= 1.0)) return false;
  const Vec3 q = cross(s, e1);
  const double v = dot(dir, q) * id;
  if (!(v >= 0.0 && u + v <= 1.0)) return false;
  const double d = dot(e2, q) * id;
  if (!(d >= 0.0 && d < best)) return false;
  out_d = d;
  out_n = cross(e1, e2);
  return true;
}

// The point of triangle abc nearest p (Ericson, Real-Time Collision Detection, 5.1.5).
inline Vec3 closest_on_triangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
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
  const double denom = 1.0 / (va + vb + vc);
  return a + ab * (vb * denom) + ac * (vc * denom);
}

#if defined(SHIFT_BVH_NEON)
inline int neon_movemask_u32(uint32x4_t m) {
  static constexpr uint32_t kBits[4] = {1u, 2u, 4u, 8u};
  return int(vaddvq_u32(vandq_u32(m, vld1q_u32(kBits))));
}
#endif

}  // namespace

MeshGround::MeshGround(std::vector<std::array<float, 3>> positions, std::vector<float> grip,
                       std::vector<uint8_t> surface)
    : positions_(std::move(positions)), grip_(std::move(grip)), surface_(std::move(surface)) {
  const uint32_t n = uint32_t(positions_.size() / 3);
  positions_.resize(size_t(n) * 3);
  if (grip_.size() != n) grip_.assign(n, 1.0f);
  if (n == 0) return;

  std::vector<uint32_t> order(n);
  std::vector<std::array<float, 3>> centroids(n);
  for (uint32_t t = 0; t < n; ++t) {
    order[t] = t;
    const auto& v0 = positions_[size_t(t) * 3];
    const auto& v1 = positions_[size_t(t) * 3 + 1];
    const auto& v2 = positions_[size_t(t) * 3 + 2];
    centroids[t] = {(v0[0] + v1[0] + v2[0]) / 3.0f, (v0[1] + v1[1] + v2[1]) / 3.0f, (v0[2] + v1[2] + v2[2]) / 3.0f};
  }

  std::vector<BuildNode> bin_nodes;
  bin_nodes.reserve(2 * (n / kLeafSize + 1));
  const uint32_t root = build_binary(positions_, centroids, order, bin_nodes, 0, n);
  centroids.clear();
  centroids.shrink_to_fit();

  bvh4_.reserve(std::max<size_t>(1, bin_nodes.size() / 2));
  packets_.reserve((n + 3) / 4 + bin_nodes.size() / 4);

  auto emit_leaf = [&](uint32_t first, uint32_t count, int32_t& out_child, uint16_t& out_count) {
    const uint32_t pkt_begin = uint32_t(packets_.size());
    uint32_t pkt_count = 0;
    for (uint32_t i = 0; i < count; i += 4) {
      TriPacket4 pkt{};
      uint32_t mask = 0;
      for (uint32_t lane = 0; lane < 4 && (i + lane) < count; ++lane) {
        const uint32_t t = order[first + i + lane];
        const auto& a = positions_[size_t(t) * 3];
        const auto& b = positions_[size_t(t) * 3 + 1];
        const auto& c = positions_[size_t(t) * 3 + 2];
        pkt.ax[lane] = a[0];
        pkt.ay[lane] = a[1];
        pkt.az[lane] = a[2];
        pkt.e1x[lane] = b[0] - a[0];
        pkt.e1y[lane] = b[1] - a[1];
        pkt.e1z[lane] = b[2] - a[2];
        pkt.e2x[lane] = c[0] - a[0];
        pkt.e2y[lane] = c[1] - a[1];
        pkt.e2z[lane] = c[2] - a[2];
        pkt.tri_idx[lane] = t;
        mask |= (1u << lane);
      }
      pkt.valid_mask = mask;
      packets_.push_back(pkt);
      ++pkt_count;
    }
    out_child = -int32_t(pkt_begin) - 1;
    out_count = uint16_t(pkt_count);
  };

  auto init_empty_node = []() {
    Bvh4Node q{};
    for (int c = 0; c < 4; ++c) {
      q.lo_x[c] = q.lo_y[c] = q.lo_z[c] = std::numeric_limits<float>::max();
      q.hi_x[c] = q.hi_y[c] = q.hi_z[c] = -std::numeric_limits<float>::max();
      q.child[c] = kEmptySlot;
      q.count[c] = 0;
    }
    return q;
  };

  auto collapse = [&](auto&& self, uint32_t bin_idx, int d) -> int32_t {
    depth_ = std::max(depth_, d);
    const int32_t q_idx = int32_t(bvh4_.size());
    bvh4_.push_back(init_empty_node());

    const BuildNode& bn = bin_nodes[bin_idx];
    uint32_t kids[4];
    int n_kids = 0;
    if (bn.count > 0) {
      // Single-leaf mesh: put directly in slot 0 of root Bvh4Node.
      kids[n_kids++] = bin_idx;
    } else {
      kids[n_kids++] = bn.left;
      kids[n_kids++] = bn.right;
      // Expand up to 4 children by splitting the internal child with the largest surface area.
      while (n_kids < 4) {
        int best_k = -1;
        float best_sa = -1.0f;
        for (int k = 0; k < n_kids; ++k) {
          const BuildNode& cn = bin_nodes[kids[k]];
          if (cn.count > 0) continue;  // already a leaf
          const float sx = std::max(0.0f, cn.hi[0] - cn.lo[0]);
          const float sy = std::max(0.0f, cn.hi[1] - cn.lo[1]);
          const float sz = std::max(0.0f, cn.hi[2] - cn.lo[2]);
          const float sa = sx * sy + sy * sz + sz * sx;
          if (sa > best_sa) {
            best_sa = sa;
            best_k = k;
          }
        }
        if (best_k < 0) break;
        const BuildNode& split = bin_nodes[kids[best_k]];
        kids[best_k] = split.left;
        kids[n_kids++] = split.right;
      }
    }

    for (int k = 0; k < n_kids; ++k) {
      const BuildNode& cn = bin_nodes[kids[k]];
      bvh4_[size_t(q_idx)].lo_x[k] = cn.lo[0] - kBoxPad;
      bvh4_[size_t(q_idx)].lo_y[k] = cn.lo[1] - kBoxPad;
      bvh4_[size_t(q_idx)].lo_z[k] = cn.lo[2] - kBoxPad;
      bvh4_[size_t(q_idx)].hi_x[k] = cn.hi[0] + kBoxPad;
      bvh4_[size_t(q_idx)].hi_y[k] = cn.hi[1] + kBoxPad;
      bvh4_[size_t(q_idx)].hi_z[k] = cn.hi[2] + kBoxPad;
      if (cn.count > 0) {
        int32_t leaf_child = kEmptySlot;
        uint16_t leaf_cnt = 0;
        emit_leaf(cn.first, cn.count, leaf_child, leaf_cnt);
        bvh4_[size_t(q_idx)].child[k] = leaf_child;
        bvh4_[size_t(q_idx)].count[k] = leaf_cnt;
        depth_ = std::max(depth_, d + 1);
      } else {
        const int32_t sub = self(self, kids[k], d + 1);
        bvh4_[size_t(q_idx)].child[k] = sub;
        bvh4_[size_t(q_idx)].count[k] = 0;
      }
    }
    return q_idx;
  };

  collapse(collapse, root, 1);
}

bool MeshGround::raycast(const Vec3& origin, const Vec3& dir, double max_distance, Hit& hit) const {
  if (bvh4_.empty() || max_distance <= 0.0) return false;

  const float ox = float(origin.x), oy = float(origin.y), oz = float(origin.z);
  const float dx = float(dir.x), dy = float(dir.y), dz = float(dir.z);
  auto safe_inv = [](double v) -> float {
    if (std::fabs(v) > 1e-18) return float(1.0 / v);
    return v >= 0.0 ? 1e30f : -1e30f;
  };
  const float ix = safe_inv(dir.x), iy = safe_inv(dir.y), iz = safe_inv(dir.z);

  double best = max_distance;
  int64_t best_t = -1;
  Vec3 best_n{};

#if defined(SHIFT_BVH_SSE)
  const __m128 v_ox = _mm_set1_ps(ox), v_oy = _mm_set1_ps(oy), v_oz = _mm_set1_ps(oz);
  const __m128 v_dx = _mm_set1_ps(dx), v_dy = _mm_set1_ps(dy), v_dz = _mm_set1_ps(dz);
  const __m128 v_ix = _mm_set1_ps(ix), v_iy = _mm_set1_ps(iy), v_iz = _mm_set1_ps(iz);
  const __m128 v_zero = _mm_setzero_ps();
  const __m128 v_eps = _mm_set1_ps(1e-12f);
  const __m128 v_neg_slack = _mm_set1_ps(-1e-4f);
  const __m128 v_pos_slack = _mm_set1_ps(1.0001f);
  const __m128 v_t_neg = _mm_set1_ps(-1e-3f);
  const __m128 v_abs_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
#elif defined(SHIFT_BVH_NEON)
  const float32x4_t v_ox = vdupq_n_f32(ox), v_oy = vdupq_n_f32(oy), v_oz = vdupq_n_f32(oz);
  const float32x4_t v_dx = vdupq_n_f32(dx), v_dy = vdupq_n_f32(dy), v_dz = vdupq_n_f32(dz);
  const float32x4_t v_ix = vdupq_n_f32(ix), v_iy = vdupq_n_f32(iy), v_iz = vdupq_n_f32(iz);
  const float32x4_t v_zero = vdupq_n_f32(0.0f);
  const float32x4_t v_eps = vdupq_n_f32(1e-12f);
  const float32x4_t v_neg_slack = vdupq_n_f32(-1e-4f);
  const float32x4_t v_pos_slack = vdupq_n_f32(1.0001f);
  const float32x4_t v_t_neg = vdupq_n_f32(-1e-3f);
#endif

  auto intersect_leaf = [&](uint32_t pkt_begin, uint16_t pkt_count) {
    const float best_f = float(best) + 1e-3f;
#if defined(SHIFT_BVH_SSE)
    __m128 v_best = _mm_set1_ps(best_f);
#elif defined(SHIFT_BVH_NEON)
    float32x4_t v_best = vdupq_n_f32(best_f);
#endif
    for (uint32_t pi = 0; pi < pkt_count; ++pi) {
      const TriPacket4& pkt = packets_[pkt_begin + pi];
      int cand_mask = 0;
#if defined(SHIFT_BVH_SSE)
      const __m128 e1x = _mm_load_ps(pkt.e1x), e1y = _mm_load_ps(pkt.e1y), e1z = _mm_load_ps(pkt.e1z);
      const __m128 e2x = _mm_load_ps(pkt.e2x), e2y = _mm_load_ps(pkt.e2y), e2z = _mm_load_ps(pkt.e2z);
      // p = cross(dir, e2)
      const __m128 px = _mm_sub_ps(_mm_mul_ps(v_dy, e2z), _mm_mul_ps(v_dz, e2y));
      const __m128 py = _mm_sub_ps(_mm_mul_ps(v_dz, e2x), _mm_mul_ps(v_dx, e2z));
      const __m128 pz = _mm_sub_ps(_mm_mul_ps(v_dx, e2y), _mm_mul_ps(v_dy, e2x));
      // det = dot(e1, p)
      const __m128 det = _mm_add_ps(_mm_add_ps(_mm_mul_ps(e1x, px), _mm_mul_ps(e1y, py)), _mm_mul_ps(e1z, pz));
      const __m128 abs_det = _mm_and_ps(det, v_abs_mask);
      __m128 m = _mm_cmpgt_ps(abs_det, v_eps);
      if (_mm_movemask_ps(m) == 0) continue;
      const __m128 id = _mm_div_ps(_mm_set1_ps(1.0f), _mm_or_ps(_mm_and_ps(m, det), _mm_andnot_ps(m, _mm_set1_ps(1.0f))));
      // s = origin - a
      const __m128 sx = _mm_sub_ps(v_ox, _mm_load_ps(pkt.ax));
      const __m128 sy = _mm_sub_ps(v_oy, _mm_load_ps(pkt.ay));
      const __m128 sz = _mm_sub_ps(v_oz, _mm_load_ps(pkt.az));
      const __m128 u = _mm_mul_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(sx, px), _mm_mul_ps(sy, py)), _mm_mul_ps(sz, pz)), id);
      m = _mm_and_ps(m, _mm_and_ps(_mm_cmpge_ps(u, v_neg_slack), _mm_cmple_ps(u, v_pos_slack)));
      if (_mm_movemask_ps(m) == 0) continue;
      // q = cross(s, e1)
      const __m128 qx = _mm_sub_ps(_mm_mul_ps(sy, e1z), _mm_mul_ps(sz, e1y));
      const __m128 qy = _mm_sub_ps(_mm_mul_ps(sz, e1x), _mm_mul_ps(sx, e1z));
      const __m128 qz = _mm_sub_ps(_mm_mul_ps(sx, e1y), _mm_mul_ps(sy, e1x));
      const __m128 v = _mm_mul_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(v_dx, qx), _mm_mul_ps(v_dy, qy)), _mm_mul_ps(v_dz, qz)), id);
      m = _mm_and_ps(m, _mm_and_ps(_mm_cmpge_ps(v, v_neg_slack), _mm_cmple_ps(_mm_add_ps(u, v), v_pos_slack)));
      if (_mm_movemask_ps(m) == 0) continue;
      const __m128 d = _mm_mul_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(e2x, qx), _mm_mul_ps(e2y, qy)), _mm_mul_ps(e2z, qz)), id);
      m = _mm_and_ps(m, _mm_and_ps(_mm_cmpge_ps(d, v_t_neg), _mm_cmple_ps(d, v_best)));
      cand_mask = _mm_movemask_ps(m) & int(pkt.valid_mask);
#elif defined(SHIFT_BVH_NEON)
      const float32x4_t e1x = vld1q_f32(pkt.e1x), e1y = vld1q_f32(pkt.e1y), e1z = vld1q_f32(pkt.e1z);
      const float32x4_t e2x = vld1q_f32(pkt.e2x), e2y = vld1q_f32(pkt.e2y), e2z = vld1q_f32(pkt.e2z);
      const float32x4_t px = vsubq_f32(vmulq_f32(v_dy, e2z), vmulq_f32(v_dz, e2y));
      const float32x4_t py = vsubq_f32(vmulq_f32(v_dz, e2x), vmulq_f32(v_dx, e2z));
      const float32x4_t pz = vsubq_f32(vmulq_f32(v_dx, e2y), vmulq_f32(v_dy, e2x));
      const float32x4_t det = vaddq_f32(vaddq_f32(vmulq_f32(e1x, px), vmulq_f32(e1y, py)), vmulq_f32(e1z, pz));
      uint32x4_t m = vcgtq_f32(vabsq_f32(det), v_eps);
      if (neon_movemask_u32(m) == 0) continue;
      const float32x4_t safe_det = vbslq_f32(m, det, vdupq_n_f32(1.0f));
      const float32x4_t id = vdivq_f32(vdupq_n_f32(1.0f), safe_det);
      const float32x4_t sx = vsubq_f32(v_ox, vld1q_f32(pkt.ax));
      const float32x4_t sy = vsubq_f32(v_oy, vld1q_f32(pkt.ay));
      const float32x4_t sz = vsubq_f32(v_oz, vld1q_f32(pkt.az));
      const float32x4_t u = vmulq_f32(vaddq_f32(vaddq_f32(vmulq_f32(sx, px), vmulq_f32(sy, py)), vmulq_f32(sz, pz)), id);
      m = vandq_u32(m, vandq_u32(vcgeq_f32(u, v_neg_slack), vcleq_f32(u, v_pos_slack)));
      if (neon_movemask_u32(m) == 0) continue;
      const float32x4_t qx = vsubq_f32(vmulq_f32(sy, e1z), vmulq_f32(sz, e1y));
      const float32x4_t qy = vsubq_f32(vmulq_f32(sz, e1x), vmulq_f32(sx, e1z));
      const float32x4_t qz = vsubq_f32(vmulq_f32(sx, e1y), vmulq_f32(sy, e1x));
      const float32x4_t v = vmulq_f32(vaddq_f32(vaddq_f32(vmulq_f32(v_dx, qx), vmulq_f32(v_dy, qy)), vmulq_f32(v_dz, qz)), id);
      m = vandq_u32(m, vandq_u32(vcgeq_f32(v, v_neg_slack), vcleq_f32(vaddq_f32(u, v), v_pos_slack)));
      if (neon_movemask_u32(m) == 0) continue;
      const float32x4_t d = vmulq_f32(vaddq_f32(vaddq_f32(vmulq_f32(e2x, qx), vmulq_f32(e2y, qy)), vmulq_f32(e2z, qz)), id);
      m = vandq_u32(m, vandq_u32(vcgeq_f32(d, v_t_neg), vcleq_f32(d, v_best)));
      cand_mask = neon_movemask_u32(m) & int(pkt.valid_mask);
#else
      cand_mask = int(pkt.valid_mask);
#endif
      while (cand_mask) {
        int lane = 0;
        while ((cand_mask & (1 << lane)) == 0) ++lane;
        cand_mask &= ~(1 << lane);
        const uint32_t t = pkt.tri_idx[lane];
        double hit_d = 0.0;
        Vec3 hit_n{};
        if (exact_ray_tri(origin, dir, positions_[size_t(t) * 3], positions_[size_t(t) * 3 + 1],
                          positions_[size_t(t) * 3 + 2], best, hit_d, hit_n)) {
          best = hit_d;
          best_t = t;
          best_n = hit_n;
#if defined(SHIFT_BVH_SSE)
          v_best = _mm_set1_ps(float(best) + 1e-3f);
#elif defined(SHIFT_BVH_NEON)
          v_best = vdupq_n_f32(float(best) + 1e-3f);
#endif
        }
      }
    }
  };

  struct StackEntry {
    int32_t node;
    float tmin;
  };
  StackEntry stack[128];
  int top = 0;
  stack[top++] = {0, 0.0f};

  while (top > 0) {
    const StackEntry cur = stack[--top];
    if (double(cur.tmin) > best + 1e-3) continue;

    const Bvh4Node& node = bvh4_[size_t(cur.node)];
    const float best_f = float(best) + 1e-3f;
    alignas(16) float tnear[4];
    int hit_mask = 0;

#if defined(SHIFT_BVH_SSE)
    const __m128 v_best = _mm_set1_ps(best_f);
    const __m128 tx0 = _mm_mul_ps(_mm_sub_ps(_mm_load_ps(node.lo_x), v_ox), v_ix);
    const __m128 tx1 = _mm_mul_ps(_mm_sub_ps(_mm_load_ps(node.hi_x), v_ox), v_ix);
    const __m128 ty0 = _mm_mul_ps(_mm_sub_ps(_mm_load_ps(node.lo_y), v_oy), v_iy);
    const __m128 ty1 = _mm_mul_ps(_mm_sub_ps(_mm_load_ps(node.hi_y), v_oy), v_iy);
    const __m128 tz0 = _mm_mul_ps(_mm_sub_ps(_mm_load_ps(node.lo_z), v_oz), v_iz);
    const __m128 tz1 = _mm_mul_ps(_mm_sub_ps(_mm_load_ps(node.hi_z), v_oz), v_iz);

    const __m128 tmin = _mm_max_ps(_mm_max_ps(_mm_min_ps(tx0, tx1), _mm_min_ps(ty0, ty1)),
                                   _mm_max_ps(_mm_min_ps(tz0, tz1), v_zero));
    const __m128 tmax = _mm_min_ps(_mm_min_ps(_mm_max_ps(tx0, tx1), _mm_max_ps(ty0, ty1)),
                                   _mm_min_ps(_mm_max_ps(tz0, tz1), v_best));
    _mm_store_ps(tnear, tmin);
    hit_mask = _mm_movemask_ps(_mm_cmple_ps(tmin, tmax));
#elif defined(SHIFT_BVH_NEON)
    const float32x4_t v_best = vdupq_n_f32(best_f);
    const float32x4_t tx0 = vmulq_f32(vsubq_f32(vld1q_f32(node.lo_x), v_ox), v_ix);
    const float32x4_t tx1 = vmulq_f32(vsubq_f32(vld1q_f32(node.hi_x), v_ox), v_ix);
    const float32x4_t ty0 = vmulq_f32(vsubq_f32(vld1q_f32(node.lo_y), v_oy), v_iy);
    const float32x4_t ty1 = vmulq_f32(vsubq_f32(vld1q_f32(node.hi_y), v_oy), v_iy);
    const float32x4_t tz0 = vmulq_f32(vsubq_f32(vld1q_f32(node.lo_z), v_oz), v_iz);
    const float32x4_t tz1 = vmulq_f32(vsubq_f32(vld1q_f32(node.hi_z), v_oz), v_iz);

    const float32x4_t tmin = vmaxq_f32(vmaxq_f32(vminq_f32(tx0, tx1), vminq_f32(ty0, ty1)),
                                       vmaxq_f32(vminq_f32(tz0, tz1), v_zero));
    const float32x4_t tmax = vminq_f32(vminq_f32(vmaxq_f32(tx0, tx1), vmaxq_f32(ty0, ty1)),
                                       vminq_f32(vmaxq_f32(tz0, tz1), v_best));
    vst1q_f32(tnear, tmin);
    hit_mask = neon_movemask_u32(vcleq_f32(tmin, tmax));
#else
    for (int c = 0; c < 4; ++c) {
      if (node.child[c] == kEmptySlot) continue;
      float tx0 = (node.lo_x[c] - ox) * ix, tx1 = (node.hi_x[c] - ox) * ix;
      float ty0 = (node.lo_y[c] - oy) * iy, ty1 = (node.hi_y[c] - oy) * iy;
      float tz0 = (node.lo_z[c] - oz) * iz, tz1 = (node.hi_z[c] - oz) * iz;
      float mn = std::max({0.0f, std::min(tx0, tx1), std::min(ty0, ty1), std::min(tz0, tz1)});
      float mx = std::min({best_f, std::max(tx0, tx1), std::max(ty0, ty1), std::max(tz0, tz1)});
      tnear[c] = mn;
      if (mn <= mx) hit_mask |= (1 << c);
    }
#endif

    if (hit_mask == 0) continue;

    // Collect hit children, intersect leaves immediately or sort internal nodes far-to-near.
    StackEntry inner[4];
    int n_inner = 0;
    for (int c = 0; c < 4; ++c) {
      if ((hit_mask & (1 << c)) == 0) continue;
      const int32_t ch = node.child[c];
      if (ch == kEmptySlot) continue;
      if (ch < 0) {
        if (double(tnear[c]) <= best + 1e-3) {
          intersect_leaf(uint32_t(-(ch + 1)), node.count[c]);
        }
      } else {
        inner[n_inner++] = {ch, tnear[c]};
      }
    }
    // Insertion sort descending by tmin so nearest child is popped first.
    for (int a = 1; a < n_inner; ++a) {
      const StackEntry key = inner[a];
      int b = a - 1;
      while (b >= 0 && inner[b].tmin < key.tmin) {
        inner[b + 1] = inner[b];
        --b;
      }
      inner[b + 1] = key;
    }
    for (int k = 0; k < n_inner; ++k) {
      if (top < 128 && double(inner[k].tmin) <= best + 1e-3) stack[top++] = inner[k];
    }
  }

  if (best_t < 0) return false;
  const double len = std::sqrt(dot(best_n, best_n));
  Vec3 n = best_n * (1.0 / len);
  if (dot(n, dir) > 0.0) n = n * -1.0;
  hit.distance = best;
  hit.normal = n;
  hit.grip = grip_[size_t(best_t)];
  hit.surface = surface_.size() == grip_.size() ? int(surface_[size_t(best_t)]) : -1;
  hit.triangle = best_t;
  hit.tyre = hit.surface >= 0 && size_t(hit.surface) < tyres_.size() ? tyres_[size_t(hit.surface)] : SurfaceTyre{};
  return true;
}

void MeshGround::sphere_contacts(const Vec3& inside, const SphereQuery* spheres, int n, SphereHit* hits) const {
  for (int i = 0; i < n; ++i) hits[i] = {};
  if (bvh4_.empty() || n <= 0) return;

  // One gather for all the spheres: the box around them and `inside`.
  double lo[3] = {inside.x, inside.y, inside.z}, hi[3] = {inside.x, inside.y, inside.z};
  for (int i = 0; i < n; ++i) {
    const double c[3] = {spheres[i].centre.x, spheres[i].centre.y, spheres[i].centre.z};
    for (int k = 0; k < 3; ++k) {
      lo[k] = std::min(lo[k], c[k] - spheres[i].radius);
      hi[k] = std::max(hi[k], c[k] + spheres[i].radius);
    }
  }

  const float flox = float(lo[0]) - kBoxPad, floy = float(lo[1]) - kBoxPad, floz = float(lo[2]) - kBoxPad;
  const float fhix = float(hi[0]) + kBoxPad, fhiy = float(hi[1]) + kBoxPad, fhiz = float(hi[2]) + kBoxPad;

#if defined(SHIFT_BVH_SSE)
  const __m128 v_lox = _mm_set1_ps(flox), v_loy = _mm_set1_ps(floy), v_loz = _mm_set1_ps(floz);
  const __m128 v_hix = _mm_set1_ps(fhix), v_hiy = _mm_set1_ps(fhiy), v_hiz = _mm_set1_ps(fhiz);
#elif defined(SHIFT_BVH_NEON)
  const float32x4_t v_lox = vdupq_n_f32(flox), v_loy = vdupq_n_f32(floy), v_loz = vdupq_n_f32(floz);
  const float32x4_t v_hix = vdupq_n_f32(fhix), v_hiy = vdupq_n_f32(fhiy), v_hiz = vdupq_n_f32(fhiz);
#endif

  thread_local std::vector<uint32_t> near;
  near.clear();
  int32_t stack[128];
  int top = 0;
  stack[top++] = 0;
  while (top > 0) {
    const Bvh4Node& node = bvh4_[size_t(stack[--top])];
    int ov_mask = 0;
#if defined(SHIFT_BVH_SSE)
    const __m128 ox = _mm_and_ps(_mm_cmple_ps(_mm_load_ps(node.lo_x), v_hix), _mm_cmpge_ps(_mm_load_ps(node.hi_x), v_lox));
    const __m128 oy = _mm_and_ps(_mm_cmple_ps(_mm_load_ps(node.lo_y), v_hiy), _mm_cmpge_ps(_mm_load_ps(node.hi_y), v_loy));
    const __m128 oz = _mm_and_ps(_mm_cmple_ps(_mm_load_ps(node.lo_z), v_hiz), _mm_cmpge_ps(_mm_load_ps(node.hi_z), v_loz));
    ov_mask = _mm_movemask_ps(_mm_and_ps(_mm_and_ps(ox, oy), oz));
#elif defined(SHIFT_BVH_NEON)
    const uint32x4_t ox = vandq_u32(vcleq_f32(vld1q_f32(node.lo_x), v_hix), vcgeq_f32(vld1q_f32(node.hi_x), v_lox));
    const uint32x4_t oy = vandq_u32(vcleq_f32(vld1q_f32(node.lo_y), v_hiy), vcgeq_f32(vld1q_f32(node.hi_y), v_loy));
    const uint32x4_t oz = vandq_u32(vcleq_f32(vld1q_f32(node.lo_z), v_hiz), vcgeq_f32(vld1q_f32(node.hi_z), v_loz));
    ov_mask = neon_movemask_u32(vandq_u32(vandq_u32(ox, oy), oz));
#else
    for (int c = 0; c < 4; ++c) {
      if (node.child[c] == kEmptySlot) continue;
      if (node.lo_x[c] <= fhix && node.hi_x[c] >= flox && node.lo_y[c] <= fhiy && node.hi_y[c] >= floy &&
          node.lo_z[c] <= fhiz && node.hi_z[c] >= floz) {
        ov_mask |= (1 << c);
      }
    }
#endif
    for (int c = 0; c < 4; ++c) {
      if ((ov_mask & (1 << c)) == 0) continue;
      const int32_t ch = node.child[c];
      if (ch == kEmptySlot) continue;
      if (ch < 0) {
        const uint32_t pkt_begin = uint32_t(-(ch + 1));
        const uint16_t pkt_count = node.count[c];
        for (uint32_t pi = 0; pi < pkt_count; ++pi) {
          const TriPacket4& pkt = packets_[pkt_begin + pi];
          for (uint32_t lane = 0; lane < 4; ++lane) {
            if (pkt.valid_mask & (1u << lane)) near.push_back(pkt.tri_idx[lane]);
          }
        }
      } else if (top < 128) {
        stack[top++] = ch;
      }
    }
  }

  for (const uint32_t t : near) {
    const auto& fa = positions_[size_t(t) * 3];
    const auto& fb = positions_[size_t(t) * 3 + 1];
    const auto& fc = positions_[size_t(t) * 3 + 2];
    const Vec3 a{fa[0], fa[1], fa[2]}, b{fb[0], fb[1], fb[2]}, c{fc[0], fc[1], fc[2]};
    Vec3 nt = cross(b - a, c - a);
    const double len = std::sqrt(dot(nt, nt));
    if (len < 1e-12) continue;
    nt = nt * (1.0 / len);
    // The face the body is on.
    if (dot(inside - a, nt) < 0.0) nt = nt * -1.0;

    const double tmin_x = std::min({a.x, b.x, c.x}), tmax_x = std::max({a.x, b.x, c.x});
    const double tmin_y = std::min({a.y, b.y, c.y}), tmax_y = std::max({a.y, b.y, c.y});
    const double tmin_z = std::min({a.z, b.z, c.z}), tmax_z = std::max({a.z, b.z, c.z});

    for (int i = 0; i < n; ++i) {
      const Vec3 centre = spheres[i].centre;
      const double r = spheres[i].radius;
      const double side = dot(centre - a, nt);
      if (side >= 0.0) {
        // Quick AABB rejection before exact closest_on_triangle.
        if (side >= r || centre.x + r < tmin_x || centre.x - r > tmax_x || centre.y + r < tmin_y ||
            centre.y - r > tmax_y || centre.z + r < tmin_z || centre.z - r > tmax_z) {
          continue;
        }
      }
      const Vec3 q = closest_on_triangle(centre, a, b, c);
      const Vec3 dq = centre - q;
      const double d2 = dot(dq, dq);
      double depth = 0.0;
      Vec3 normal = nt;
      if (side >= 0.0) {
        if (d2 >= r * r) continue;
        const double d = std::sqrt(d2);
        depth = r - d;
        if (d > 1e-6) normal = dq * (1.0 / d);
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
    }
  }
  // Resolve ~300-byte SurfaceTyre once per winning sphere hit instead of inside the triangle loop.
  if (!tyres_.empty()) {
    for (int i = 0; i < n; ++i) {
      SphereHit& h = hits[i];
      if (h.depth > 0.0 && h.surface >= 0 && size_t(h.surface) < tyres_.size()) {
        h.tyre = tyres_[size_t(h.surface)];
      }
    }
  }
}

namespace {

constexpr char kBvh4Magic[8] = {'S', 'H', 'B', 'V', 'H', '4', '0', '1'};

struct Bvh4BlobHeader {
  char magic[8];
  uint32_t version;
  uint32_t triangle_count;
  uint32_t node_count;
  uint32_t packet_count;
  uint32_t surface_count;
  int32_t depth;
};
static_assert(sizeof(Bvh4BlobHeader) == 32);

}  // namespace

std::vector<uint8_t> MeshGround::serialize_bvh4() const {
  const uint32_t tri_count = static_cast<uint32_t>(positions_.size() / 3);
  Bvh4BlobHeader hdr{};
  std::memcpy(hdr.magic, kBvh4Magic, sizeof(kBvh4Magic));
  hdr.version = 1u;
  hdr.triangle_count = tri_count;
  hdr.node_count = static_cast<uint32_t>(bvh4_.size());
  hdr.packet_count = static_cast<uint32_t>(packets_.size());
  hdr.surface_count = static_cast<uint32_t>(surface_.size());
  hdr.depth = depth_;

  const size_t pos_bytes = positions_.size() * sizeof(std::array<float, 3>);
  const size_t grip_bytes = grip_.size() * sizeof(float);
  const size_t surf_bytes = surface_.size() * sizeof(uint8_t);
  const size_t node_bytes = bvh4_.size() * sizeof(Bvh4Node);
  const size_t pkt_bytes = packets_.size() * sizeof(TriPacket4);

  std::vector<uint8_t> out(sizeof(Bvh4BlobHeader) + pos_bytes + grip_bytes + surf_bytes + node_bytes + pkt_bytes);
  size_t off = 0;
  auto write_chunk = [&](const void* src, size_t n) {
    if (n == 0) return;
    std::memcpy(out.data() + off, src, n);
    off += n;
  };
  write_chunk(&hdr, sizeof(hdr));
  write_chunk(positions_.data(), pos_bytes);
  write_chunk(grip_.data(), grip_bytes);
  write_chunk(surface_.data(), surf_bytes);
  write_chunk(bvh4_.data(), node_bytes);
  write_chunk(packets_.data(), pkt_bytes);
  return out;
}

std::unique_ptr<MeshGround> MeshGround::deserialize_bvh4(std::span<const uint8_t> bytes) {
  if (bytes.size() < sizeof(Bvh4BlobHeader)) return nullptr;
  Bvh4BlobHeader hdr{};
  std::memcpy(&hdr, bytes.data(), sizeof(hdr));
  if (std::memcmp(hdr.magic, kBvh4Magic, sizeof(kBvh4Magic)) != 0 || hdr.version != 1u ||
      hdr.depth < 0 || hdr.depth > 128) {
    return nullptr;
  }
  if (hdr.surface_count != 0u && hdr.surface_count != hdr.triangle_count) return nullptr;
  if (hdr.triangle_count == 0u && (hdr.node_count != 0u || hdr.packet_count != 0u || hdr.depth != 0)) {
    return nullptr;
  }
  if (hdr.triangle_count > 0u && (hdr.node_count == 0u || hdr.packet_count == 0u)) return nullptr;

  const uint64_t pos_count = static_cast<uint64_t>(hdr.triangle_count) * 3ull;
  const uint64_t pos_bytes = pos_count * sizeof(std::array<float, 3>);
  const uint64_t grip_bytes = static_cast<uint64_t>(hdr.triangle_count) * sizeof(float);
  const uint64_t surf_bytes = static_cast<uint64_t>(hdr.surface_count) * sizeof(uint8_t);
  const uint64_t node_bytes = static_cast<uint64_t>(hdr.node_count) * sizeof(Bvh4Node);
  const uint64_t pkt_bytes = static_cast<uint64_t>(hdr.packet_count) * sizeof(TriPacket4);
  const uint64_t expected = sizeof(Bvh4BlobHeader) + pos_bytes + grip_bytes + surf_bytes + node_bytes + pkt_bytes;
  if (expected != bytes.size()) return nullptr;

  std::unique_ptr<MeshGround> g(new MeshGround());
  g->positions_.resize(static_cast<size_t>(pos_count));
  g->grip_.resize(hdr.triangle_count);
  g->surface_.resize(hdr.surface_count);
  g->bvh4_.resize(hdr.node_count);
  g->packets_.resize(hdr.packet_count);
  g->depth_ = hdr.depth;

  size_t off = sizeof(Bvh4BlobHeader);
  auto read_chunk = [&](void* dst, uint64_t n) {
    if (n == 0) return;
    std::memcpy(dst, bytes.data() + off, static_cast<size_t>(n));
    off += static_cast<size_t>(n);
  };
  read_chunk(g->positions_.data(), pos_bytes);
  read_chunk(g->grip_.data(), grip_bytes);
  read_chunk(g->surface_.data(), surf_bytes);
  read_chunk(g->bvh4_.data(), node_bytes);
  read_chunk(g->packets_.data(), pkt_bytes);

  // Reject non-finite floats in positions, grip, BVH bounds, and triangle packets.
  for (const auto& p : g->positions_) {
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return nullptr;
  }
  for (float gr : g->grip_) {
    if (!std::isfinite(gr)) return nullptr;
  }

  // Validate all BVH4 child/leaf indices, finite bounds, and triangle packet references.
  for (const Bvh4Node& node : g->bvh4_) {
    for (int c = 0; c < 4; ++c) {
      const int32_t ch = node.child[c];
      if (ch == kEmptySlot) continue;
      if (!std::isfinite(node.lo_x[c]) || !std::isfinite(node.lo_y[c]) || !std::isfinite(node.lo_z[c]) ||
          !std::isfinite(node.hi_x[c]) || !std::isfinite(node.hi_y[c]) || !std::isfinite(node.hi_z[c])) {
        return nullptr;
      }
      if (ch >= 0) {
        if (static_cast<uint32_t>(ch) >= hdr.node_count) return nullptr;
      } else {
        const uint32_t pkt_begin = static_cast<uint32_t>(-(ch + 1));
        if (static_cast<uint64_t>(pkt_begin) + static_cast<uint64_t>(node.count[c]) > hdr.packet_count) {
          return nullptr;
        }
      }
    }
  }
  for (const TriPacket4& pkt : g->packets_) {
    if ((pkt.valid_mask & ~0xFu) != 0u) return nullptr;
    for (uint32_t lane = 0; lane < 4; ++lane) {
      if ((pkt.valid_mask & (1u << lane)) != 0u) {
        if (pkt.tri_idx[lane] >= hdr.triangle_count) return nullptr;
        if (!std::isfinite(pkt.ax[lane]) || !std::isfinite(pkt.ay[lane]) || !std::isfinite(pkt.az[lane]) ||
            !std::isfinite(pkt.e1x[lane]) || !std::isfinite(pkt.e1y[lane]) || !std::isfinite(pkt.e1z[lane]) ||
            !std::isfinite(pkt.e2x[lane]) || !std::isfinite(pkt.e2y[lane]) || !std::isfinite(pkt.e2z[lane])) {
          return nullptr;
        }
      }
    }
  }

  // Validate strict tree topology (acyclic, no shared DAG parents, max depth <= 128)
  // so raycast()/sphere_contacts() can never infinite-loop or overflow stack[128].
  if (!g->bvh4_.empty()) {
    std::vector<uint8_t> visited(hdr.node_count, 0u);
    struct VisitItem {
      uint32_t idx;
      uint32_t depth;
    };
    std::vector<VisitItem> stack;
    stack.reserve(std::min<uint32_t>(hdr.node_count, 256u));
    visited[0] = 1u;
    stack.push_back({0u, 1u});
    while (!stack.empty()) {
      const VisitItem cur = stack.back();
      stack.pop_back();
      if (cur.depth > 128u) return nullptr;
      const Bvh4Node& node = g->bvh4_[cur.idx];
      for (int c = 0; c < 4; ++c) {
        const int32_t ch = node.child[c];
        if (ch == kEmptySlot || ch < 0) continue;
        const uint32_t u = static_cast<uint32_t>(ch);
        if (visited[u] != 0u) return nullptr;  // cycle or DAG back-edge
        visited[u] = 1u;
        stack.push_back({u, cur.depth + 1u});
      }
    }
  }
  return g;
}

}  // namespace drive
