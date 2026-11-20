#include "world/road_follow.h"

#include <algorithm>
#include <cmath>

namespace world {

bool wall_between(const drive::Ground& ground, const drive::Vec3& from, double x, double z) {
  const drive::Vec3 origin{from.x, from.y + 0.7, from.z};
  const double dx = x - from.x, dz = z - from.z, d = std::hypot(dx, dz);
  if (d < 1e-6) return false;
  drive::Hit hit;
  return ground.raycast(origin, {dx / d, 0, dz / d}, d, hit) && std::fabs(hit.normal.y) < 0.5;
}

void RoadFollower::rebuild(const Mesh& mesh) {
  points_.clear();
  for (size_t t = 0; t < mesh.triangles(); ++t) {
    if (mesh.surface[t] != Surface::kRoad || !mesh.ground[t]) continue;
    const auto& a = mesh.positions[t * 3];
    const auto& b = mesh.positions[t * 3 + 1];
    const auto& c = mesh.positions[t * 3 + 2];
    points_.push_back({(double(a[0]) + b[0] + c[0]) / 3, (double(a[2]) + b[2] + c[2]) / 3});
  }
}

drive::Input RoadFollower::input(const drive::Vehicle& v) const {
  const drive::Vec3 p = v.position();
  const drive::Vec3 fwd = drive::rotate(v.orientation(), {0, 0, 1});
  const double yaw = std::atan2(fwd.x, fwd.z);
  const double speed = v.speed();
  const double ahead = std::clamp(1.0 * std::fabs(speed), 8.0, 30.0);
  // The road points in a ring at the look-ahead distance, by the turn
  // they need; aim at the middle of the run of road around the least turn
  // (the road's width, not its nearer edge).
  std::vector<double> turns;
  for (const auto& q : points_) {
    const double dx = q[0] - p.x, dz = q[1] - p.z;
    const double d = std::hypot(dx, dz);
    if (d < ahead * 0.8 || d > ahead * 1.2) continue;
    const double turn = std::remainder(std::atan2(dx, dz) - yaw, 2 * 3.14159265358979323846);
    if (std::fabs(turn) > 1.2) continue;
    if (ground_ && wall_between(*ground_, p, q[0], q[1])) continue;
    turns.push_back(turn);
  }
  drive::Input in;
  if (!turns.empty()) {
    std::sort(turns.begin(), turns.end());
    size_t best = 0;
    for (size_t i = 1; i < turns.size(); ++i)
      if (std::fabs(turns[i]) < std::fabs(turns[best])) best = i;
    // Grow the run while neighbouring points are within 0.15 rad of each other.
    size_t lo = best, hi = best;
    while (lo > 0 && turns[lo] - turns[lo - 1] < 0.15) --lo;
    while (hi + 1 < turns.size() && turns[hi + 1] - turns[hi] < 0.15) ++hi;
    const double aim = (turns[lo] + turns[hi]) / 2;
    in.steer = std::clamp(aim / 0.45, -1.0, 1.0);
  }
  const double error = target_ - speed;
  in.throttle = std::clamp(error * 0.2, 0.0, 0.6);
  in.brake = std::clamp(-error * 0.15, 0.0, 1.0);
  if (speed < 1 && error > 0) in.brake = 0;  // never into reverse
  return in;
}

bool RoadFollower::stuck(const drive::Vehicle& v, const drive::Input& in, double seconds) {
  const drive::Vec3 up = drive::rotate(v.orientation(), {0, 1, 0});
  const bool bad = up.y < 0.5 || (v.speed() < 0.5 && in.throttle > 0.1);  // still, or going backwards
  stuck_time_ = bad ? stuck_time_ + drive::Vehicle::kStep : 0;
  if (stuck_time_ < seconds) return false;
  stuck_time_ = 0;
  return true;
}

}  // namespace world
