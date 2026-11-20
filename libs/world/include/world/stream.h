// Streaming Colorado around the car: gathers the world near a point and
// builds the stand-in ground from it, and gathers again once the car has
// moved far enough from where the last gather was centred.
//
// M3 stand-in until COLLISION.md: the ground is the gathered world-space
// render triangles (props are drawn, not collided with), and the grip per
// surface class is a placeholder of ours.
#pragma once
#include <memory>
#include <string>

#include "drive/mesh_ground.h"
#include "world/world.h"

namespace world {

// Placeholder grip factors per surface class (not the game's).
float placeholder_grip(Surface s);

class Streamer {
 public:
  Streamer(World& world, GatherOptions options, double regather_distance = 200)
      : world_(world), options_(options), regather_(regather_distance) {}
  // Gathers on the first call and whenever (x, z) is more than the
  // regather distance from the last centre. `changed`: whether it did.
  bool update(double x, double z, bool* changed, std::string* error);
  const Mesh& mesh() const { return mesh_; }
  const drive::Ground& ground() const { return *ground_; }
  const drive::MeshGround& mesh_ground() const { return *ground_; }
  const GatherStats& stats() const { return stats_; }
  double centre_x() const { return cx_; }
  double centre_z() const { return cz_; }
  int gathers() const { return gathers_; }

 private:
  World& world_;
  GatherOptions options_;
  double regather_;
  double cx_ = 0, cz_ = 0;
  int gathers_ = 0;
  Mesh mesh_;
  GatherStats stats_;
  std::unique_ptr<drive::MeshGround> ground_;
};

}  // namespace world
