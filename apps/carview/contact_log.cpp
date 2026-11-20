#include "contact_log.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <functional>
#include <tuple>

#include "drive/vehicle.h"

namespace carview {
namespace {

constexpr double kCell = 4;

using V = std::array<double, 3>;
V sub(const V& a, const V& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const V& a, const V& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V cross(const V& a, const V& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
V d(const std::array<float, 3>& a) { return {a[0], a[1], a[2]}; }

// The closest point on triangle abc to p (Ericson, Real-Time Collision
// Detection 5.1.5), as a squared distance.
double point_triangle2(const V& p, const V& a, const V& b, const V& c) {
  const V ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
  const double d1 = dot(ab, ap), d2 = dot(ac, ap);
  auto len2 = [](const V& v) { return dot(v, v); };
  if (d1 <= 0 && d2 <= 0) return len2(ap);
  const V bp = sub(p, b);
  const double d3 = dot(ab, bp), d4 = dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return len2(bp);
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) {
    const double v = d1 / (d1 - d3);
    return len2(sub(ap, {ab[0] * v, ab[1] * v, ab[2] * v}));
  }
  const V cp = sub(p, c);
  const double d5 = dot(ab, cp), d6 = dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) return len2(cp);
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) {
    const double w = d2 / (d2 - d6);
    return len2(sub(ap, {ac[0] * w, ac[1] * w, ac[2] * w}));
  }
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
    const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    const V bc = sub(c, b);
    return len2(sub(bp, {bc[0] * w, bc[1] * w, bc[2] * w}));
  }
  const double den = 1 / (va + vb + vc);
  const double v = vb * den, w = vc * den;
  const V q{a[0] + ab[0] * v + ac[0] * w, a[1] + ab[1] * v + ac[1] * w, a[2] + ab[2] * v + ac[2] * w};
  return len2(sub(p, q));
}

}  // namespace

void DrawnIndex::build(const world::Mesh& mesh, bool upright) {
  mesh_ = &mesh;
  cells_.clear();
  count_ = mesh.triangles();
  sprite_.assign(count_, 0);
  for (size_t t = 0; t < count_; ++t) {
    const auto& a = mesh.positions[t * 3];
    const auto& b = mesh.positions[t * 3 + 1];
    const auto& c = mesh.positions[t * 3 + 2];
    double lo_x = std::min({a[0], b[0], c[0]}), hi_x = std::max({a[0], b[0], c[0]});
    double lo_z = std::min({a[2], b[2], c[2]}), hi_z = std::max({a[2], b[2], c[2]});
    if (mesh.has_render_data() && mesh.material[t] < mesh.materials.size() &&
        mesh.materials[mesh.material[t]].layers == world::Layers::kBillboard) {
      // Every corner is the base point; uv1 is the corner's offset (across, up).
      sprite_[t] = 1;
      double w = 0;
      for (int k = 0; k < 3; ++k) w = std::max(w, double(std::fabs(mesh.uv1[t * 3 + size_t(k)][0])));
      lo_x -= w, hi_x += w, lo_z -= w, hi_z += w;
    } else {
      // Upright surfaces only: the road and the terrain under a wall are not
      // what shows it.
      const V n = cross(sub(d(b), d(a)), sub(d(c), d(a)));
      const double len = std::sqrt(dot(n, n));
      if (upright && (len < 1e-9 || std::fabs(n[1] / len) >= 0.7)) continue;
    }
    const int i0 = int(std::floor(lo_x / kCell)), i1 = int(std::floor(hi_x / kCell));
    const int j0 = int(std::floor(lo_z / kCell)), j1 = int(std::floor(hi_z / kCell));
    if ((i1 - i0 + 1) * (j1 - j0 + 1) > 4096) continue;  // a far-ring sheet: never within a metre of a wall's point
    for (int i = i0; i <= i1; ++i)
      for (int j = j0; j <= j1; ++j) cells_[key(i, j)].push_back(uint32_t(t));
  }
}

std::vector<double> DrawnIndex::heights(double x, double z) const {
  std::vector<double> out;
  if (!mesh_) return out;
  const auto it = cells_.find(key(int(std::floor(x / kCell)), int(std::floor(z / kCell))));
  if (it == cells_.end()) return out;
  for (uint32_t t : it->second) {
    if (sprite_[t]) continue;
    const V a = d(mesh_->positions[size_t(t) * 3]), b = d(mesh_->positions[size_t(t) * 3 + 1]),
            c = d(mesh_->positions[size_t(t) * 3 + 2]);
    // Barycentric in x-z.
    const double det = (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]);
    if (std::fabs(det) < 1e-12) continue;
    const double u = ((x - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (z - a[2])) / det;
    const double v = ((b[0] - a[0]) * (z - a[2]) - (x - a[0]) * (b[2] - a[2])) / det;
    if (u < 0 || v < 0 || u + v > 1) continue;
    out.push_back(a[1] + u * (b[1] - a[1]) + v * (c[1] - a[1]));
  }
  std::sort(out.begin(), out.end(), std::greater<double>());
  return out;
}

double DrawnIndex::nearest(const std::array<float, 3>& pf, double reach, uint32_t* triangle) const {
  if (!mesh_ || !std::isfinite(pf[0]) || !std::isfinite(pf[1]) || !std::isfinite(pf[2]) || reach > 50) return -1;
  const V p = d(pf);
  double best2 = reach * reach;
  int64_t best = -1;
  const int i0 = int(std::floor((p[0] - reach) / kCell)), i1 = int(std::floor((p[0] + reach) / kCell));
  const int j0 = int(std::floor((p[2] - reach) / kCell)), j1 = int(std::floor((p[2] + reach) / kCell));
  for (int i = i0; i <= i1; ++i)
    for (int j = j0; j <= j1; ++j) {
      const auto it = cells_.find(key(i, j));
      if (it == cells_.end()) continue;
      for (uint32_t t : it->second) {
        double e2;
        if (sprite_[t]) {
          // An upright box: the base, the widest offset across, the tallest up.
          const V base = d(mesh_->positions[size_t(t) * 3]);
          double w = 0, h = 0;
          for (int k = 0; k < 3; ++k) {
            w = std::max(w, double(std::fabs(mesh_->uv1[size_t(t) * 3 + size_t(k)][0])));
            h = std::max(h, double(mesh_->uv1[size_t(t) * 3 + size_t(k)][1]));
          }
          const double r = std::max(0.0, std::hypot(p[0] - base[0], p[2] - base[2]) - w);
          const double y = p[1] < base[1] ? base[1] - p[1] : std::max(0.0, p[1] - (base[1] + h));
          e2 = r * r + y * y;
        } else {
          e2 = point_triangle2(p, d(mesh_->positions[size_t(t) * 3]), d(mesh_->positions[size_t(t) * 3 + 1]),
                               d(mesh_->positions[size_t(t) * 3 + 2]));
        }
        if (e2 < best2) best2 = e2, best = t;
      }
    }
  if (best < 0) return -1;
  if (triangle) *triangle = uint32_t(best);
  return std::sqrt(best2);
}

std::vector<UndrawnWalls> undrawn_walls(const world::CollisionMesh& cm, const drive::Ground& ground,
                                        const DrawnIndex& drawn, double x, double z, double radius, double reach,
                                        bool skip_masked, size_t* samples, size_t* bare) {
  constexpr double kStep = 0.5;
  std::map<std::tuple<uint32_t, int, int, int, uint32_t>, UndrawnWalls> groups;
  size_t all = 0, all_bare = 0;
  for (size_t t = 0; t < cm.triangles(); ++t) {
    if (!world::Collision::car_collides(cm.surface[t]) || (skip_masked && !(cm.word[t] & 0x8000))) continue;
    const V a = d(cm.positions[t * 3]), b = d(cm.positions[t * 3 + 1]), c = d(cm.positions[t * 3 + 2]);
    V n = cross(sub(b, a), sub(c, a));
    const double len = std::sqrt(dot(n, n));
    if (len < 1e-6) continue;
    n = {n[0] / len, n[1] / len, n[2] / len};
    if (std::fabs(n[1]) >= 0.5) continue;
    const V m{(a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3};
    if (std::hypot(m[0] - x, m[2] - z) > radius) continue;
    // In-plane axes: u horizontal, w = n x u (mostly up). Corners in (s, t).
    const double hl = std::hypot(n[0], n[2]);
    const V u{-n[2] / hl, 0, n[0] / hl};
    V w = cross(n, u);
    if (w[1] < 0) w = {-w[0], -w[1], -w[2]};
    const V nh{n[0] / hl, 0, n[2] / hl};
    const V corner[3] = {a, b, c};
    double s2[3], t2[3];
    for (int k = 0; k < 3; ++k) s2[k] = dot(sub(corner[k], a), u), t2[k] = dot(sub(corner[k], a), w);
    const double s_lo = std::min({s2[0], s2[1], s2[2]}), s_hi = std::max({s2[0], s2[1], s2[2]});
    if (!(s_hi - s_lo < 2000)) continue;  // not a wall the car meets at one spot
    UndrawnWalls* g = nullptr;
    for (double s = s_lo + kStep / 2; s < s_hi; s += kStep) {
      // The triangle's t-range on this column.
      double t0 = 1e30, t1 = -1e30;
      for (int k = 0; k < 3; ++k) {
        const int l = (k + 1) % 3;
        const double ds = s2[l] - s2[k];
        if (std::fabs(ds) < 1e-9) continue;
        const double f = (s - s2[k]) / ds;
        if (f < 0 || f > 1) continue;
        const double tt = t2[k] + f * (t2[l] - t2[k]);
        t0 = std::min(t0, tt), t1 = std::max(t1, tt);
      }
      if (t0 > t1) continue;
      const V top{a[0] + u[0] * s + w[0] * t1, a[1] + w[1] * t1, a[2] + u[2] * s + w[2] * t1};
      const double y_lo = a[1] + w[1] * t0, y_hi = top[1];
      for (double side : {-0.4, 0.4}) {
        drive::Hit h;
        const drive::Vec3 o{top[0] + nh[0] * side, y_hi + 0.5, top[2] + nh[2] * side};
        if (!ground.raycast(o, {0, -1, 0}, y_hi - y_lo + 5, h)) continue;
        const double yg = o.y - h.distance;
        for (double y = yg + 0.1; y <= yg + 1.5; y += kStep) {
          if (y < y_lo || y > y_hi) continue;
          const double tt = (y - a[1]) / w[1];
          const V p{a[0] + u[0] * s + w[0] * tt, y, a[2] + u[2] * s + w[2] * tt};
          ++all;
          if (!g) {
            const uint32_t tile = t < cm.tile.size() ? cm.tile[t] : 0;
            const int flags = t < cm.flags.size() ? cm.flags[t] : 0;
            const int word = t < cm.word.size() ? cm.word[t] : 0;
            const uint32_t edges = t < cm.edges.size() ? cm.edges[t] : 0;
            g = &groups[{tile, int(cm.surface[t]), flags, word, edges}];
            g->tile = tile, g->surface = cm.surface[t], g->flags = flags, g->word = word, g->edges = edges;
            ++g->triangles;
          }
          ++g->samples;
          if (drawn.nearest({float(p[0]), float(p[1]), float(p[2])}, reach) >= 0) continue;
          ++g->bare;
          ++all_bare;
          for (int k = 0; k < 3; ++k)
            g->lo[size_t(k)] = std::min(g->lo[size_t(k)], p[size_t(k)]),
            g->hi[size_t(k)] = std::max(g->hi[size_t(k)], p[size_t(k)]);
        }
      }
    }
  }
  if (samples) *samples = all;
  if (bare) *bare = all_bare;
  std::vector<UndrawnWalls> out;
  for (auto& [k, g] : groups)
    if (g.bare) out.push_back(g);
  std::sort(out.begin(), out.end(), [](const auto& l, const auto& r) { return l.bare > r.bare; });
  return out;
}

}  // namespace carview
