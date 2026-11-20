#include "drive/profile_ground.h"

#include <algorithm>
#include <cmath>

namespace drive {

ProfileGround::ProfileGround(double origin_x, double origin_y, double origin_z, double dir_x, double dir_z,
                             std::vector<double> distances, std::vector<double> heights)
    : ox_(origin_x), oy_(origin_y), oz_(origin_z), s_(std::move(distances)), h_(std::move(heights)) {
  const double l = std::hypot(dir_x, dir_z);
  dx_ = dir_x / l;
  dz_ = dir_z / l;
}

double ProfileGround::distance_along(double x, double z) const { return (x - ox_) * dx_ + (z - oz_) * dz_; }

double ProfileGround::slope(double s) const {
  if (s_.size() < 2) return 0;
  size_t i = 0;
  while (i + 2 < s_.size() && s >= s_[i + 1]) ++i;
  return (h_[i + 1] - h_[i]) / (s_[i + 1] - s_[i]);
}

double ProfileGround::height(double s) const {
  if (s_.empty()) return oy_;
  if (s_.size() == 1) return oy_ + h_[0];
  size_t i = 0;
  while (i + 2 < s_.size() && s >= s_[i + 1]) ++i;
  return oy_ + h_[i] + (h_[i + 1] - h_[i]) * (s - s_[i]) / (s_[i + 1] - s_[i]);
}

bool ProfileGround::raycast(const Vec3& origin, const Vec3& dir, double max_distance, Hit& hit) const {
  // March along the ray for the first crossing below the surface, then
  // bisect. The surface is piecewise planar, so a fine march finds it.
  auto above = [&](double t) {
    const Vec3 p = origin + dir * t;
    return p.y - height(distance_along(p.x, p.z));
  };
  double t0 = 0, f0 = above(0);
  if (f0 < 0) return false;  // starts under the road
  const double step = 0.05;
  for (double t1 = std::min(step, max_distance);; t1 = std::min(t1 + step, max_distance)) {
    const double f1 = above(t1);
    if (f1 <= 0) {
      double a = t0, b = t1;
      for (int k = 0; k < 50; ++k) {
        const double m = 0.5 * (a + b);
        (above(m) > 0 ? a : b) = m;
      }
      hit.distance = 0.5 * (a + b);
      const Vec3 p = origin + dir * hit.distance;
      const double g = slope(distance_along(p.x, p.z));
      const Vec3 n{-g * dx_, 1, -g * dz_};
      const double l = std::sqrt(dot(n, n));
      hit.normal = n * (1 / l);
      hit.grip = 1;
      hit.surface = -1;
      return true;
    }
    if (t1 >= max_distance) return false;
    t0 = t1;
    f0 = f1;
  }
}

ProfileGround spawn_road(double origin_x, double origin_y, double origin_z, double dir_x, double dir_z) {
  return ProfileGround(origin_x, origin_y, origin_z, dir_x, dir_z,
                       {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100},
                       {0, -0.44, -0.85, -1.24, -1.57, -1.80, -2.06, -2.58, -3.24, -4.06, -5.06});
}

}  // namespace drive
