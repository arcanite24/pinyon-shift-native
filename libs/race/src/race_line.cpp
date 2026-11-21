#include "race/race_line.h"

#include <algorithm>
#include <cmath>

namespace race {

RaceLine build_race_line(const AiLineFile& f, const Route* route, int copies) {
  RaceLine r;
  r.closed = f.closed;
  const size_t n = f.points.size();
  if (n < 3) return r;
  // Per point of the file: the racing point, the radius from the points 4
  // either side (wrapping on a closed line) and the room to each edge.
  std::vector<float> rx(n), ry(n), rz(n), rad(n, 1e6f), left(n), right(n);
  for (size_t i = 0; i < n; ++i) {
    const auto& p = f.points[i];
    rx[i] = p.race_x(), ry[i] = p.race_y(), rz[i] = p.race_z();
    const float hw = p.half_width();
    left[i] = hw * (1 - p.offset);
    right[i] = hw * (1 + p.offset);
  }
  constexpr size_t kSide = 4;
  for (size_t i = 0; i < n; ++i) {
    size_t a, b;
    if (f.closed) a = (i + n - kSide) % n, b = (i + kSide) % n;
    else {
      if (i < kSide || i + kSide >= n) continue;
      a = i - kSide, b = i + kSide;
    }
    const double ax = rx[a] - rx[i], az = rz[a] - rz[i], bx = rx[b] - rx[i], bz = rz[b] - rz[i];
    const double cross = ax * bz - az * bx;
    const double la = std::hypot(ax, az), lb = std::hypot(bx, bz), lc = std::hypot(bx - ax, bz - az);
    if (std::fabs(cross) > 1e-9) rad[i] = float(std::min(1e6, la * lb * lc / (2 * std::fabs(cross))));
  }
  if (!f.closed) {
    for (size_t i = 0; i < kSide; ++i) rad[i] = rad[kSide];
    for (size_t i = n - kSide; i < n; ++i) rad[i] = rad[n - kSide - 1];
  }
  const int reps = f.closed ? std::max(1, copies) : 1;
  drive::RacingLine& L = r.line;
  double s = 0;
  for (int c = 0; c < reps; ++c)
    for (size_t i = 0; i < n; ++i) {
      if (!L.x.empty()) {
        const double d = std::hypot(rx[i] - L.x.back(), rz[i] - L.z.back());
        if (d < 1e-3) continue;
        s += d;
      }
      L.x.push_back(rx[i]), L.y.push_back(ry[i]), L.z.push_back(rz[i]);
      L.dist.push_back(float(s));
      L.radius.push_back(rad[i]);
      L.room_left.push_back(left[i]), L.room_right.push_back(right[i]);
      if (c == 0 && i == n - 1 && f.closed) r.lap_length = s + std::hypot(rx[0] - rx[i], rz[0] - rz[i]);
    }
  if (!f.closed) r.lap_length = s;
  // A default speed profile (the old controller's): corner 11 m/s^2, braking 14.
  const size_t m = L.size();
  L.speed.resize(m);
  for (size_t i = 0; i < m; ++i) L.speed[i] = float(std::min(90.0, std::sqrt(11.0 * L.radius[i])));
  for (size_t i = m - 1; i-- > 0;)
    L.speed[i] = std::min(L.speed[i], float(std::sqrt(double(L.speed[i + 1]) * L.speed[i + 1] + 28.0 * (L.dist[i + 1] - L.dist[i]))));
  if (route)
    if (auto g = route->start_line())
      r.gate_progress = f.closed ? L.nearest(g->x, g->z, r.lap_length, 2 * r.lap_length) : L.nearest(g->x, g->z, 0, L.length());
  return r;
}

RibbonGround::RibbonGround(const AiLineFile& f, double margin, double cell) : cell_(cell) {
  const size_t n = f.points.size();
  if (n < 2) return;
  const double k = 1 + margin;
  auto corner = [&](size_t i, double side, float* out) {
    const auto& p = f.points[i];
    out[0] = float(p.x + side * k * p.lx), out[1] = float(p.y + side * k * p.ly), out[2] = float(p.z + side * k * p.lz);
  };
  double minx = 1e300, minz = 1e300, maxx = -1e300, maxz = -1e300;
  for (size_t i = 0; i + 1 < n + (f.closed ? 1 : 0); ++i) {
    const size_t j = (i + 1) % n;
    float a[3], b[3], c[3], d[3];
    corner(i, 1, a), corner(i, -1, b), corner(j, 1, c), corner(j, -1, d);
    for (const float* t : {a, b, c, b, d, c})
      for (int q = 0; q < 3; ++q) tri_.push_back(t[q]);
  }
  for (size_t i = 0; i < tri_.size(); i += 3)
    minx = std::min(minx, double(tri_[i])), maxx = std::max(maxx, double(tri_[i])),
    minz = std::min(minz, double(tri_[i + 2])), maxz = std::max(maxz, double(tri_[i + 2]));
  x0_ = minx, z0_ = minz;
  nx_ = int((maxx - minx) / cell_) + 1, nz_ = int((maxz - minz) / cell_) + 1;
  cells_.assign(size_t(nx_) * size_t(nz_), {});
  for (size_t t = 0; t < tri_.size() / 9; ++t) {
    const float* v = &tri_[t * 9];
    const double lox = std::min({v[0], v[3], v[6]}), hix = std::max({v[0], v[3], v[6]});
    const double loz = std::min({v[2], v[5], v[8]}), hiz = std::max({v[2], v[5], v[8]});
    for (int cx = int((lox - x0_) / cell_); cx <= int((hix - x0_) / cell_); ++cx)
      for (int cz = int((loz - z0_) / cell_); cz <= int((hiz - z0_) / cell_); ++cz)
        cells_[size_t(cz) * size_t(nx_) + size_t(cx)].push_back(uint32_t(t));
  }
}

bool RibbonGround::raycast(const drive::Vec3& o, const drive::Vec3& dir, double max_distance, drive::Hit& hit) const {
  if (cells_.empty()) return false;
  const double ex = o.x + dir.x * max_distance, ez = o.z + dir.z * max_distance;
  const int c0x = std::clamp(int((std::min(o.x, ex) - x0_) / cell_), 0, nx_ - 1);
  const int c1x = std::clamp(int((std::max(o.x, ex) - x0_) / cell_), 0, nx_ - 1);
  const int c0z = std::clamp(int((std::min(o.z, ez) - z0_) / cell_), 0, nz_ - 1);
  const int c1z = std::clamp(int((std::max(o.z, ez) - z0_) / cell_), 0, nz_ - 1);
  if (std::max(o.x, ex) < x0_ || std::min(o.x, ex) > x0_ + nx_ * cell_ || std::max(o.z, ez) < z0_ ||
      std::min(o.z, ez) > z0_ + nz_ * cell_)
    return false;
  double best = max_distance;
  bool found = false;
  drive::Vec3 normal{0, 1, 0};
  for (int cz = c0z; cz <= c1z; ++cz)
    for (int cx = c0x; cx <= c1x; ++cx)
      for (uint32_t t : cells_[size_t(cz) * size_t(nx_) + size_t(cx)]) {
        const float* v = &tri_[size_t(t) * 9];
        const drive::Vec3 a{v[0], v[1], v[2]}, b{v[3], v[4], v[5]}, c{v[6], v[7], v[8]};
        const drive::Vec3 e1 = b - a, e2 = c - a;
        const drive::Vec3 pv = cross(dir, e2);
        const double det = dot(e1, pv);
        if (std::fabs(det) < 1e-12) continue;
        const double inv = 1 / det;
        const drive::Vec3 tv = o - a;
        const double u = dot(tv, pv) * inv;
        if (u < 0 || u > 1) continue;
        const drive::Vec3 qv = cross(tv, e1);
        const double w = dot(dir, qv) * inv;
        if (w < 0 || u + w > 1) continue;
        const double d = dot(e2, qv) * inv;
        if (d < 0 || d >= best) continue;
        best = d, found = true;
        drive::Vec3 nn = cross(e1, e2);
        if (nn.y < 0) nn = nn * -1.0;
        const double l = std::sqrt(dot(nn, nn));
        normal = l > 0 ? nn * (1 / l) : drive::Vec3{0, 1, 0};
      }
  if (!found) return false;
  hit = drive::Hit{};
  hit.distance = best;
  hit.normal = normal;
  hit.grip = 1;
  return true;
}

}  // namespace race
