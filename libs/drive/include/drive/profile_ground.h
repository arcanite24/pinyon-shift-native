// SPEC-CAR-PHYSICS, Drive on analytic ground: a road that follows a height
// profile along one direction and is level across it, all asphalt (grip 1).
// Heights are piecewise linear between the profile's points (the spec's
// rule), each inner point rounded over +-`blend` m so the slope does not
// step (ours: a massless wheel reads a slope step as a jump in suspension
// speed), and continue at the end slopes beyond them.
#pragma once

#include <vector>

#include "drive/vehicle.h"

namespace drive {

class ProfileGround : public Ground {
 public:
  // `origin` (x, z) is distance 0 at height `origin_y`; `dir_x`, `dir_z`
  // the road's direction (normalised here); `distances` rising, `heights`
  // relative to origin_y.
  ProfileGround(double origin_x, double origin_y, double origin_z, double dir_x, double dir_z,
                std::vector<double> distances, std::vector<double> heights, double blend = 4.0);
  bool raycast(const Vec3& origin, const Vec3& dir, double max_distance, Hit& hit) const override;

  double distance_along(double x, double z) const;  // the road distance of a point
  double height(double s) const;                    // world y at road distance s
  double slope(double s) const;                     // dy/ds there

 private:
  void eval(double s, double& height, double& slope) const;
  double ox_, oy_, oz_, dx_, dz_, blend_;
  std::vector<double> s_, h_;
};

// The spawn road's profile (SPEC-CAR-PHYSICS: the centre of mass's height
// every 10 m from the launch point, 0 to 100 m).
ProfileGround spawn_road(double origin_x, double origin_y, double origin_z, double dir_x, double dir_z);

}  // namespace drive
