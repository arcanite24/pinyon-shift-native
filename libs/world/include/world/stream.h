// Streaming Colorado around the car: gathers the world near a point (the
// render models to draw) and the ground to drive on, and gathers again once
// the car has moved far enough from where the last gather was centred.
//
// The ground is the game's collision (COLLISION.md `.fiz` tiles) when a
// world::Collision is given, with grip from each triangle's surface.
// Without one it falls back to the earlier stand-in: the gathered
// world-space render triangles, with a placeholder grip per surface class.
#pragma once
#include <any>
#include <functional>
#include <future>
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
  // Work done with each gather's mesh before it is handed over (for
  // instance building draw data), on the worker when async: its result is
  // `prepared()`.
  using Prepare = std::function<std::any(const Mesh& mesh, double centre_x, double centre_z)>;

  // `collision`: the ground source (null: the render-triangle stand-in),
  // loaded within `collision_radius` of the centre.
  Streamer(World& world, GatherOptions options, double regather_distance = 200, Collision* collision = nullptr,
           double collision_radius = 750);
  ~Streamer();
  // Gathers on the first call and whenever (x, z) is more than the
  // regather distance from the last centre. `changed`: whether the mesh
  // and ground were replaced in this call.
  //
  // Async (set_async): the first gather still runs in the call; later ones
  // run on a worker thread while the car keeps the current mesh and
  // ground, and are swapped in by the first update after they finish, so
  // the step does not wait for them. The swap time then depends on the
  // wall clock, so headless runs that must repeat stay synchronous. While a
  // gather is running, the World and Collision belong to the worker: the
  // caller must not use them.
  bool update(double x, double z, bool* changed, std::string* error);
  void set_async(bool async) { async_ = async; }
  // Ours (no spec covers the streaming centre): an async gather started more
  // than 50 m from the last centre is centred up to min(150 m, 0.2 x the
  // collision radius) further along the car's displacement from it, so a
  // fast car is still central when the gather lands. Off by default, where
  // the centre is the point update() was given.
  void set_lead(bool lead) { lead_ = lead; }
  // A new set of hidden props (GatherOptions::hidden): the next update
  // gathers again wherever the car is, once no gather is running.
  void set_hidden(std::shared_ptr<const std::set<std::string>> hidden) {
    pending_hidden_ = std::move(hidden);
    force_ = true;
  }
  // Broken smashable prop positions (game frame) masked out on subsequent gathers.
  void set_hidden_positions(std::shared_ptr<const std::vector<GatherOptions::HiddenSphere>> hidden_pos) {
    pending_hidden_positions_ = std::move(hidden_pos);
    has_pending_hidden_positions_ = true;
  }
  void set_prepare(Prepare prepare) { prepare_ = std::move(prepare); }
  // The Prepare result of the mesh in use (moved out by the caller).
  std::any& prepared() { return prepared_; }
  bool gathering() const { return job_.valid(); }
  // Seconds the last gather took (on whichever thread ran it).
  double last_gather_seconds() const { return last_seconds_; }
  const Mesh& mesh() const { return mesh_; }
  const drive::Ground& ground() const { return composite_ground_ ? *composite_ground_ : static_cast<const drive::Ground&>(*ground_); }
  const drive::MeshGround& mesh_ground() const { return *ground_; }
  const GatherStats& stats() const { return stats_; }
  // The collision triangles of the last gather (empty without a Collision).
  const CollisionMesh& collision_mesh() const { return collision_mesh_; }
  // With collision: per ground triangle (Hit::triangle) its index in
  // collision_mesh().
  const std::vector<uint32_t>& ground_source() const { return ground_source_; }
  bool uses_collision() const { return collision_ != nullptr; }
  double centre_x() const { return cx_; }
  double centre_z() const { return cz_; }
  double collision_radius() const { return collision_radius_; }
  int gathers() const { return gathers_; }

 private:
  // One gather's results, built on whichever thread ran it.
  struct Gathered {
    Mesh mesh;
    GatherStats stats;
    CollisionMesh collision;
    std::unique_ptr<drive::MeshGround> ground;
    std::unique_ptr<drive::Ground> composite_ground;
    std::vector<uint32_t> ground_source;
    std::any prepared;
    double x = 0, z = 0, seconds = 0;
    bool ok = false;
    std::string error;
  };
  std::unique_ptr<Gathered> gather_now(double x, double z) const;
  void take(Gathered&& g);

  World& world_;
  GatherOptions options_;
  std::shared_ptr<const std::set<std::string>> pending_hidden_;
  std::shared_ptr<const std::vector<GatherOptions::HiddenSphere>> pending_hidden_positions_;
  bool has_pending_hidden_positions_ = false;
  bool force_ = false;
  double regather_;
  Collision* collision_;
  double collision_radius_;
  CollisionMesh collision_mesh_;
  std::vector<uint32_t> ground_source_;
  double cx_ = 0, cz_ = 0;
  int gathers_ = 0;
  Mesh mesh_;
  GatherStats stats_;
  std::unique_ptr<drive::MeshGround> ground_;
  std::unique_ptr<drive::Ground> composite_ground_;
  bool async_ = false;
  bool lead_ = false;
  Prepare prepare_;
  std::any prepared_;
  double last_seconds_ = 0;
  std::future<std::unique_ptr<Gathered>> job_;
  // The gather a swap replaced, freed on another thread (its meshes take
  // milliseconds to free).
  std::future<void> drop_;
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
