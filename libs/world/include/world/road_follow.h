// A test driver for headless runs on the stand-in world: steers towards
// the road ahead and holds a speed. Ours (not the game's AI): it looks for
// road triangles at a look-ahead distance in front of the car and aims at
// the one needing the least turn, so it keeps to the straightest road at a
// junction.
#pragma once
#include <array>
#include <vector>

#include "drive/vehicle.h"
#include "world/world.h"

namespace world {

class RoadFollower {
 public:
  explicit RoadFollower(double target_speed = 40 / 3.6) : target_(target_speed) {}
  // The input for this step. Call `rebuild` after the mesh changes.
  void rebuild(const Mesh& mesh);
  drive::Input input(const drive::Vehicle& v) const;
  size_t road_points() const { return points_.size(); }

  // For headless runs: true once the car has been on its side or roof, or
  // still or going backwards while the driver asks for throttle (a spin),
  // for `seconds` (call
  // once per step). The caller puts it back on the road (find_spawn).
  bool stuck(const drive::Vehicle& v, const drive::Input& in, double seconds = 3);

 private:
  double stuck_time_ = 0;
  double target_;
  std::vector<std::array<double, 2>> points_;  // road triangle centres (x, z)
};

}  // namespace world
