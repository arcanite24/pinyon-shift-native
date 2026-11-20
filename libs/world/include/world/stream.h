// Streaming Colorado around the car: gathers the world near a point (the
// render models to draw) and the ground to drive on, and gathers again once
// the car has moved far enough from where the last gather was centred.
//
// The ground is the game's collision (COLLISION.md `.fiz` tiles) when a
// world::Collision is given, with grip from each triangle's surface.
// Without one it falls back to the earlier stand-in: the gathered
// world-space render triangles, with a placeholder grip per surface class.
#pragma once
#include <memory>
#include <string>

#include "drive/mesh_ground.h"
#include "world/collision.h"
#include "world/world.h"

namespace world {

// Placeholder grip factors per surface class (not the game's).
float placeholder_grip(Surface s);

class Streamer {
 public:
  // `collision`: the ground source (null: the render-triangle stand-in),
  // loaded within `collision_radius` of the centre.
  Streamer(World& world, GatherOptions options, double regather_distance = 200, Collision* collision = nullptr,
           double collision_radius = 350)
      : world_(world),
        options_(options),
        regather_(regather_distance),
        collision_(collision),
        collision_radius_(collision_radius) {}
  // Gathers on the first call and whenever (x, z) is more than the
  // regather distance from the last centre. `changed`: whether it did.
  bool update(double x, double z, bool* changed, std::string* error);
  const Mesh& mesh() const { return mesh_; }
  const drive::Ground& ground() const { return *ground_; }
  const drive::MeshGround& mesh_ground() const { return *ground_; }
  const GatherStats& stats() const { return stats_; }
  // The collision triangles of the last gather (empty without a Collision).
  const CollisionMesh& collision_mesh() const { return collision_mesh_; }
  bool uses_collision() const { return collision_ != nullptr; }
  double centre_x() const { return cx_; }
  double centre_z() const { return cz_; }
  int gathers() const { return gathers_; }

 private:
  World& world_;
  GatherOptions options_;
  double regather_;
  Collision* collision_;
  double collision_radius_;
  CollisionMesh collision_mesh_;
  double cx_ = 0, cz_ = 0;
  int gathers_ = 0;
  Mesh mesh_;
  GatherStats stats_;
  std::unique_ptr<drive::MeshGround> ground_;
};

// Where a car starts: at `given` (x, y, z, heading) when set; else on the
// road triangle nearest `near` (x, z) when set, or when the streamer has no
// collision; else at COLLISION.md's measured spawn. The point is then
// dropped onto the streamer's ground (the body frame's origin rests on the
// ground). The streamer must have gathered around the point. `how` names
// the choice. False when no road is near.
bool choose_spawn(const Streamer& streamer, const double* given, const double* near, const double* prefer_heading,
                  Spawn& out, const char** how = nullptr);

// Puts a stuck or rolled car back on the road nearest it, on the ground,
// facing along the road nearest its heading. False when no road is near.
bool recover(const Streamer& streamer, drive::Vehicle& vehicle, Spawn* where = nullptr);

}  // namespace world
