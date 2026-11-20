#include "traffic/roads.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace traffic {

void Roads::build(const NavMap& map) {
  roads_.clear();
  at_node_.assign(map.nodes.size(), {});
  degree_.assign(map.nodes.size(), 0);
  for (uint32_t w = 0; w < map.ways.size(); ++w) {
    const std::string_view type = map.way_tag(w, "road_type");
    if (type.empty() || map.ways[w].node_count < 2) continue;
    Road r;
    r.way = w;
    r.type = std::string(type);
    const std::string_view density = map.way_tag(w, "traffic_density");
    r.density = density.empty() ? 0 : std::atoi(std::string(density).c_str());
    r.disabled = map.way_tag(w, "traffic_disabled") == "true" || map.way_tag(w, "ai_disabled") == "true";
    r.oneway = map.way_tag(w, "oneway") == "true";
    r.traffic = r.density > 0 && !r.disabled;
    for (uint32_t k = 0; k < map.ways[w].node_count; ++k) {
      const uint32_t n = map.way_node(w, k);
      const NavNode& nd = map.nodes[n];
      r.nodes.push_back(n);
      r.points.push_back({nd.x, nd.y, nd.z});
    }
    r.s.assign(r.points.size(), 0);
    for (size_t k = 1; k < r.points.size(); ++k) {
      const P3 &a = r.points[k - 1], &b = r.points[k];
      const double h = std::hypot(b.x - a.x, b.z - a.z);
      r.s[k] = r.s[k - 1] + h;
      r.length3 += std::sqrt(h * h + (b.y - a.y) * (b.y - a.y));
    }
    r.length = r.s.back();
    // The bend radius at each inner point: the mean of its two segments
    // over the turn between them.
    r.radius.assign(r.points.size(), 1e9);
    for (size_t k = 1; k + 1 < r.points.size(); ++k) {
      const P3 &a = r.points[k - 1], &b = r.points[k], &c = r.points[k + 1];
      const double a1 = std::atan2(b.x - a.x, b.z - a.z), a2 = std::atan2(c.x - b.x, c.z - b.z);
      double turn = std::fabs(std::remainder(a2 - a1, 2 * 3.14159265358979323846));
      const double l = 0.5 * ((r.s[k] - r.s[k - 1]) + (r.s[k + 1] - r.s[k]));
      if (turn > 1e-4) r.radius[k] = l / turn;
    }
    const int index = int(roads_.size());
    for (size_t k = 0; k < r.nodes.size(); ++k) {
      at_node_[r.nodes[k]].push_back({index, int(k)});
      degree_[r.nodes[k]] += (k > 0) + (k + 1 < r.nodes.size());
    }
    roads_.push_back(std::move(r));
  }
  // The grid over every segment's bounding box.
  double max_x = -1e30, max_z = -1e30;
  min_x_ = min_z_ = 1e30;
  for (const auto& r : roads_)
    for (const auto& p : r.points) {
      min_x_ = std::min(min_x_, p.x), min_z_ = std::min(min_z_, p.z);
      max_x = std::max(max_x, p.x), max_z = std::max(max_z, p.z);
    }
  if (roads_.empty()) return;
  nx_ = int((max_x - min_x_) / cell_) + 1;
  nz_ = int((max_z - min_z_) / cell_) + 1;
  grid_.assign(size_t(nx_) * size_t(nz_), {});
  for (int ri = 0; ri < int(roads_.size()); ++ri) {
    const auto& p = roads_[size_t(ri)].points;
    for (int k = 0; k + 1 < int(p.size()); ++k) {
      const auto &a = p[size_t(k)], &b = p[size_t(k) + 1];
      const int x0 = int((std::min(a.x, b.x) - min_x_) / cell_), x1 = int((std::max(a.x, b.x) - min_x_) / cell_);
      const int z0 = int((std::min(a.z, b.z) - min_z_) / cell_), z1 = int((std::max(a.z, b.z) - min_z_) / cell_);
      for (int gz = z0; gz <= z1; ++gz)
        for (int gx = x0; gx <= x1; ++gx) grid_[size_t(gz) * size_t(nx_) + size_t(gx)].push_back({ri, k});
    }
  }
}

RoadHit Roads::nearest(double x, double z, bool traffic_only, double max_distance) const {
  RoadHit best;
  if (grid_.empty()) return best;
  const int x0 = std::max(0, int((x - max_distance - min_x_) / cell_)),
            x1 = std::min(nx_ - 1, int((x + max_distance - min_x_) / cell_));
  const int z0 = std::max(0, int((z - max_distance - min_z_) / cell_)),
            z1 = std::min(nz_ - 1, int((z + max_distance - min_z_) / cell_));
  for (int gz = z0; gz <= z1; ++gz)
    for (int gx = x0; gx <= x1; ++gx)
      for (const auto& [ri, k] : grid_[size_t(gz) * size_t(nx_) + size_t(gx)]) {
        const Road& r = roads_[size_t(ri)];
        if (traffic_only && !r.traffic) continue;
        const P3 &a = r.points[size_t(k)], &b = r.points[size_t(k) + 1];
        const double dx = b.x - a.x, dz = b.z - a.z, len2 = dx * dx + dz * dz;
        double t = len2 > 0 ? ((x - a.x) * dx + (z - a.z) * dz) / len2 : 0;
        t = std::clamp(t, 0.0, 1.0);
        const double px = a.x + dx * t, pz = a.z + dz * t;
        const double d = std::hypot(x - px, z - pz);
        if (d < best.distance && d <= max_distance) {
          best.road = ri;
          best.segment = k;
          best.s = r.s[size_t(k)] + t * (r.s[size_t(k) + 1] - r.s[size_t(k)]);
          best.distance = d;
          const double len = std::sqrt(len2);
          const auto right = right_of(len > 0 ? dx / len : 0, len > 0 ? dz / len : 1);
          best.lateral = (x - px) * right[0] + (z - pz) * right[1];
        }
      }
  return best;
}

P3 Roads::point_at(int road, double s, int* segment, std::array<double, 2>* dir) const {
  const Road& r = roads_[size_t(road)];
  s = std::clamp(s, 0.0, r.length);
  size_t k = size_t(std::upper_bound(r.s.begin(), r.s.end(), s) - r.s.begin());
  k = std::clamp<size_t>(k, 1, r.points.size() - 1) - 1;
  const P3 &a = r.points[k], &b = r.points[k + 1];
  const double l = r.s[k + 1] - r.s[k];
  const double t = l > 0 ? (s - r.s[k]) / l : 0;
  if (segment) *segment = int(k);
  if (dir) *dir = l > 0 ? std::array<double, 2>{(b.x - a.x) / l, (b.z - a.z) / l} : std::array<double, 2>{0, 1};
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

std::map<int, double> Roads::traffic_length_within(double x, double z, double radius) const {
  std::map<int, double> out;
  const double r2 = radius * radius;
  for (const auto& r : roads_) {
    if (!r.traffic) continue;
    for (size_t k = 0; k + 1 < r.points.size(); ++k) {
      // The part of the segment inside the circle (a quadratic in t).
      const P3 &a = r.points[k], &b = r.points[k + 1];
      const double dx = b.x - a.x, dz = b.z - a.z, ax = a.x - x, az = a.z - z;
      const double qa = dx * dx + dz * dz, qb = 2 * (ax * dx + az * dz), qc = ax * ax + az * az - r2;
      if (qa <= 0) continue;
      const double disc = qb * qb - 4 * qa * qc;
      if (disc <= 0) continue;
      const double sq = std::sqrt(disc);
      const double t0 = std::max(0.0, (-qb - sq) / (2 * qa)), t1 = std::min(1.0, (-qb + sq) / (2 * qa));
      if (t1 > t0) out[r.density] += (t1 - t0) * std::sqrt(qa);
    }
  }
  return out;
}

}  // namespace traffic
