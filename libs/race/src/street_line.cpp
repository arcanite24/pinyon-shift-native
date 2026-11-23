#include "race/street_line.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <queue>

namespace race {
namespace {

struct Edge {
  uint32_t to;
  float length;
};

struct Graph {
  std::vector<std::vector<Edge>> out;
  std::vector<uint8_t> on_way;
};

Graph build_graph(const traffic::NavMap& nav) {
  Graph g;
  g.out.resize(nav.nodes.size());
  g.on_way.assign(nav.nodes.size(), 0);
  for (uint32_t w = 0; w < nav.ways.size(); ++w) {
    const uint32_t n = nav.ways[w].node_count;
    for (uint32_t k = 0; k + 1 < n; ++k) {
      const uint32_t a = nav.way_node(w, k), b = nav.way_node(w, k + 1);
      if (a >= nav.nodes.size() || b >= nav.nodes.size() || a == b) continue;
      const auto& p = nav.nodes[a];
      const auto& q = nav.nodes[b];
      const float len = std::hypot(q.x - p.x, q.z - p.z);
      // Both directions: a race closes the roads.
      g.out[a].push_back({b, len});
      g.out[b].push_back({a, len});
      g.on_way[a] = g.on_way[b] = 1;
    }
  }
  return g;
}

uint32_t nearest_node(const traffic::NavMap& nav, const Graph& g, double x, double z, double& dist) {
  uint32_t best = std::numeric_limits<uint32_t>::max();
  double bd = 1e30;
  for (uint32_t i = 0; i < nav.nodes.size(); ++i) {
    if (!g.on_way[i]) continue;
    const double d = std::hypot(nav.nodes[i].x - x, nav.nodes[i].z - z);
    if (d < bd) bd = d, best = i;
  }
  dist = bd;
  return best;
}

// Dijkstra from a to b; the node indices from a to b, empty when none.
// When b is out of reach, `reached` (if given) holds every node's distance
// from a (infinite when unreachable): the whole component was searched.
std::vector<uint32_t> shortest(const Graph& g, uint32_t a, uint32_t b, std::vector<float>* reached = nullptr) {
  const size_t n = g.out.size();
  std::vector<float> dist(n, std::numeric_limits<float>::infinity());
  std::vector<uint32_t> prev(n, std::numeric_limits<uint32_t>::max());
  using Q = std::pair<float, uint32_t>;
  std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
  dist[a] = 0.0f;
  q.push({0.0f, a});
  while (!q.empty()) {
    const auto [d, u] = q.top();
    q.pop();
    if (d > dist[u]) continue;
    if (u == b) break;
    for (const Edge& e : g.out[u]) {
      const float nd = d + e.length;
      if (nd < dist[e.to]) dist[e.to] = nd, prev[e.to] = u, q.push({nd, e.to});
    }
  }
  if (!std::isfinite(dist[b])) {
    if (reached) *reached = std::move(dist);
    return {};
  }
  std::vector<uint32_t> path;
  for (uint32_t v = b; v != std::numeric_limits<uint32_t>::max(); v = prev[v]) {
    path.push_back(v);
    if (v == a) break;
  }
  std::reverse(path.begin(), path.end());
  return path;
}

struct P {
  double x, y, z;
};

}  // namespace

std::vector<const RouteTransform*> checkpoints(const Route& route) {
  std::vector<const RouteTransform*> out;
  for (int k = 0; k < 100; ++k) {
    char n[40];
    std::snprintf(n, sizeof n, "route_checkpoint_%02d", k);
    const RouteTransform* t = route.find(n);
    if (!t) break;
    out.push_back(t);
  }
  return out;
}

std::optional<Gate> checkpoint_gate(const Route& route, size_t k) {
  const auto cps = checkpoints(route);
  const auto slots = route.slots();
  if (k >= cps.size()) return std::nullopt;
  const RouteTransform* from = k > 0 ? cps[k - 1] : (slots.empty() ? nullptr : slots[0]);
  const RouteTransform& c = *cps[k];
  double nx = c.fx, nz = c.fz;
  if (std::hypot(nx, nz) < 1e-6 && from) nx = c.x - from->x, nz = c.z - from->z;
  const double l = std::hypot(nx, nz);
  if (l < 1e-6) return std::nullopt;
  Gate g;
  g.x = c.x, g.z = c.z, g.nx = nx / l, g.nz = nz / l;
  g.reach = c.width ? 0.5 * *c.width : 25.0;
  g.both_ways = true;
  return g;
}

bool build_street_line(const traffic::NavMap& nav, const Route& route, AiLineFile& out, std::string* error,
                       const StreetLineOptions& o) {
  const auto slots = route.slots();
  const auto cps = checkpoints(route);
  if (slots.empty() || cps.empty()) {
    if (error) *error = "route " + std::to_string(route.id) + ": no slots or checkpoints";
    return false;
  }
  const Graph g = build_graph(nav);
  std::vector<const RouteTransform*> stops{slots[0]};
  stops.insert(stops.end(), cps.begin(), cps.end());
  std::vector<uint32_t> nodes;
  for (const auto* t : stops) {
    double d = 0;
    const uint32_t n = nearest_node(nav, g, t->x, t->z, d);
    if (n == std::numeric_limits<uint32_t>::max() || d > 80) {
      if (error) *error = "route " + std::to_string(route.id) + ": no road within 80 m of " + t->name;
      return false;
    }
    nodes.push_back(n);
  }
  std::vector<P> pts;
  pts.push_back({slots[0]->x, slots[0]->y, slots[0]->z});
  for (size_t k = 0; k + 1 < nodes.size(); ++k) {
    std::vector<float> reached;
    auto path = shortest(g, nodes[k], nodes[k + 1], &reached);
    if (path.empty() && !reached.empty()) {
      // The nearest road is not joined to the last one (a separate piece
      // of the road map): the nearest node within 80 m that is.
      const RouteTransform* t = stops[k + 1];
      uint32_t best = std::numeric_limits<uint32_t>::max();
      double bd = 80;
      for (uint32_t i = 0; i < nav.nodes.size(); ++i) {
        if (!std::isfinite(reached[i])) continue;
        const double d = std::hypot(nav.nodes[i].x - t->x, nav.nodes[i].z - t->z);
        if (d < bd) bd = d, best = i;
      }
      if (best != std::numeric_limits<uint32_t>::max()) {
        nodes[k + 1] = best;
        path = shortest(g, nodes[k], best);
      }
    }
    if (path.empty()) {
      if (error) *error = "route " + std::to_string(route.id) + ": no road path to " + stops[k + 1]->name;
      return false;
    }
    for (uint32_t v : path) {
      const auto& n = nav.nodes[v];
      if (!pts.empty() && std::hypot(pts.back().x - n.x, pts.back().z - n.z) < 0.5) continue;
      pts.push_back({n.x, n.y, n.z});
    }
  }
  // Past the last checkpoint and behind slot 00: straight on, so the field
  // finishes on line and the grid stands on it.
  auto extend = [&](const P& a, const P& b, double len) {
    const double dx = b.x - a.x, dz = b.z - a.z, l = std::max(1e-6, std::hypot(dx, dz));
    return P{b.x + dx / l * len, b.y, b.z + dz / l * len};
  };
  if (pts.size() < 2) {
    if (error) *error = "route " + std::to_string(route.id) + ": a street line of one point";
    return false;
  }
  pts.push_back(extend(pts[pts.size() - 2], pts.back(), 200));
  pts.insert(pts.begin(), extend(pts[1], pts[0], 80));
  // Resampled at `spacing`.
  std::vector<P> even;
  even.push_back(pts[0]);
  double carry = 0;
  for (size_t i = 1; i < pts.size(); ++i) {
    const P a = pts[i - 1], b = pts[i];
    const double l = std::hypot(b.x - a.x, b.z - a.z);
    double s = o.spacing - carry;
    while (s <= l) {
      const double u = s / l;
      even.push_back({a.x + u * (b.x - a.x), a.y + u * (b.y - a.y), a.z + u * (b.z - a.z)});
      s += o.spacing;
    }
    carry = l - (s - o.spacing);
  }
  // Rounded corners: the road map's nodes turn a junction in one point;
  // passes of a 9-point (+-16 m) weighted mean give the corner a radius of
  // roughly 10 to 20 m, cutting the inside by a few metres as a racing
  // line does. The ends stay put.
  for (int pass = 0; pass < o.smooth_passes; ++pass) {
    std::vector<P> s = even;
    for (size_t i = 4; i + 4 < even.size(); ++i) {
      double x = 0, z = 0, w = 0;
      for (int k = -4; k <= 4; ++k) {
        const double wk = 5 - std::abs(k);
        x += wk * even[i + size_t(k)].x, z += wk * even[i + size_t(k)].z, w += wk;
      }
      s[i].x = x / w, s[i].z = z / w;
    }
    even.swap(s);
  }
  out = AiLineFile{};
  out.version = 1;
  out.closed = false;
  out.points.resize(even.size());
  for (size_t i = 0; i < even.size(); ++i) {
    const P a = even[i > 0 ? i - 1 : 0], b = even[std::min(i + 1, even.size() - 1)];
    double dx = b.x - a.x, dz = b.z - a.z;
    const double l = std::max(1e-6, std::hypot(dx, dz));
    dx /= l, dz /= l;
    AiLinePoint& p = out.points[i];
    p.x = float(even[i].x), p.y = float(even[i].y), p.z = float(even[i].z);
    // Left of travel: (-dz, dx), at the half width.
    p.lx = float(-dz * o.half_width), p.ly = 0, p.lz = float(dx * o.half_width);
    p.offset = 0;
  }
  // The racing offset: a minimum-curvature line inside the road. Each
  // pass moves every point along its lateral towards the point that zeroes
  // its second difference against the neighbours k and 2k points either
  // side, (4 (r[i-k] + r[i+k]) - (r[i-2k] + r[i+2k])) / 6 (coarse chords
  // first, then finer), clamped to max_offset half widths; the ends stay on
  // the centre. Minimising curvature, not length, spreads each turn over
  // its apex instead of a kink where the line meets the road's edge. It
  // cuts each bend to its inside across the road's width, as the measured
  // AI spread (about +-4.5 m about the centre) does.
  const size_t n = out.points.size();
  std::vector<double> off(n, 0.0);
  auto rx = [&](size_t i) { return out.points[i].x + off[i] * out.points[i].lx; };
  auto rz = [&](size_t i) { return out.points[i].z + off[i] * out.points[i].lz; };
  const double hw2 = o.half_width * o.half_width;
  for (const int k : {12, 8, 5, 3, 2}) {
    const size_t K = size_t(k);
    if (n <= 4 * K) continue;
    for (int pass = 0; pass < o.relax_passes; ++pass)
      for (size_t i = 2 * K; i + 2 * K < n; ++i) {
        const double mx = (4 * (rx(i - K) + rx(i + K)) - (rx(i - 2 * K) + rx(i + 2 * K))) / 6;
        const double mz = (4 * (rz(i - K) + rz(i + K)) - (rz(i - 2 * K) + rz(i + 2 * K))) / 6;
        const auto& p = out.points[i];
        const double d = ((mx - rx(i)) * p.lx + (mz - rz(i)) * p.lz) / hw2;  // in half widths
        off[i] = std::clamp(off[i] + 0.5 * d, -o.max_offset, o.max_offset);
      }
  }
  for (size_t i = 0; i < n; ++i) out.points[i].offset = float(off[i]);
  return true;
}

}  // namespace race
