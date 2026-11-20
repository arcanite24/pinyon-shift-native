// A test driver for headless runs on the stand-in world: steers towards
// the road ahead and holds a speed. Ours (not the game's AI): it looks for
// road triangles at a look-ahead distance in front of the car and aims at
// the one needing the least turn, so it keeps to the straightest road at a
// junction. With a ground set, road points behind a wall (a horizontal
// ray at bonnet height meets a surface facing sideways) are left out.
#pragma once
#include <array>
#include <vector>

#include "drive/vehicle.h"
#include "world/world.h"

namespace world {

// Whether a horizontal ray 0.7 m above `from` towards (x, z) meets a
// surface facing sideways (a wall, barrier or rail) before it gets there.
bool wall_between(const drive::Ground& ground, const drive::Vec3& from, double x, double z);

class RoadFollower {
 public:
  explicit RoadFollower(double target_speed = 40 / 3.6) : target_(target_speed) {}
  // The input for this step. Call `rebuild` after the mesh changes.
  void rebuild(const Mesh& mesh);
  // The collision to look for walls in (null: none). Set it again after
  // the ground changes.
  void set_ground(const drive::Ground* ground) { ground_ = ground; }
  drive::Input input(const drive::Vehicle& v) const;
  size_t road_points() const { return points_.size(); }

  // For headless runs: true once the car has been on its side or roof, or
  // still or going backwards while the driver asks for throttle (a spin),
  // for `seconds` (call
  // once per step). The caller puts it back on the road (find_spawn).
  bool stuck(const drive::Vehicle& v, const drive::Input& in, double seconds = 3);

 private:
  double stuck_time_ = 0;
  const drive::Ground* ground_ = nullptr;
  double target_;
  std::vector<std::array<double, 2>> points_;  // road triangle centres (x, z)
};

}  // namespace world
