#include "world/stream.h"

#include "world/road_follow.h"

#include <cmath>

namespace world {

float placeholder_grip(Surface s) {
  switch (s) {
    case Surface::kRoad: return 1.0f;
    case Surface::kTerrain: return 0.75f;
    case Surface::kWater: return 0.5f;
    case Surface::kOther: return 0.9f;
    case Surface::kProp: return 0.9f;
  }
  return 1.0f;
}

namespace {

void drop(const Streamer& streamer, Spawn& s) {
  double y = 0;
  if (ground_height(streamer.ground(), s.position[0], s.position[2], s.position[1] + 2, y)) s.position[1] = y + 0.02;
}

}  // namespace

bool choose_spawn(const Streamer& streamer, const double* given, const double* near, const double* prefer_heading,
                  Spawn& out, const char** how) {
  out = Spawn{};
  if (given) {
    out.position = {given[0], given[1], given[2]};
    out.heading = given[3];
    if (how) *how = "given";
  } else if (near || !streamer.uses_collision()) {
    const double x = near ? near[0] : kDumpCameraX, z = near ? near[1] : kDumpCameraZ;
    if (!find_spawn(streamer.mesh(), x, z, out, prefer_heading)) return false;
    if (how) *how = "nearest road triangle (stand-in)";
  } else {
    out.position = {kSpawnX, kSpawnY, kSpawnZ};
    out.heading = spawn_heading();
    if (how) *how = "measured (COLLISION.md)";
  }
  drop(streamer, out);
  return true;
}

bool recover(const Streamer& streamer, drive::Vehicle& vehicle, Spawn* where) {
  const drive::Vec3 at = vehicle.position();
  const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
  const double heading = std::atan2(fwd.x, fwd.z);
  Spawn s;
  if (!find_spawn(streamer.mesh(), at.x, at.z, s, &heading)) return false;
  drop(streamer, s);
  // Facing a wall within 10 m (a dead end): turn around.
  const drive::Vec3 p{s.position[0], s.position[1], s.position[2]};
  if (wall_between(streamer.ground(), p, p.x + 10 * std::sin(s.heading), p.z + 10 * std::cos(s.heading)))
    s.heading = std::remainder(s.heading + 3.14159265358979323846, 2 * 3.14159265358979323846);
  vehicle.place({s.position[0], s.position[1] + 0.03, s.position[2]}, s.heading);
  if (where) *where = s;
  return true;
}

bool Streamer::update(double x, double z, bool* changed, std::string* error) {
  if (changed) *changed = false;
  if (gathers_ > 0 && std::hypot(x - cx_, z - cz_) <= regather_) return true;
  Mesh mesh;
  GatherStats stats;
  if (!world_.gather(x, z, options_, mesh, &stats, error)) return false;
  std::vector<std::array<float, 3>> positions;
  std::vector<float> grip;
  std::vector<uint8_t> surface;
  if (collision_) {
    // The ground: the collision's core triangles the car collides with.
    CollisionMesh cm;
    if (!collision_->gather(x, z, collision_radius_, cm, error)) return false;
    positions.reserve(cm.positions.size());
    for (size_t t = 0; t < cm.triangles(); ++t) {
      if (!Collision::car_collides(cm.surface[t])) continue;
      positions.push_back(cm.positions[t * 3]);
      positions.push_back(cm.positions[t * 3 + 1]);
      positions.push_back(cm.positions[t * 3 + 2]);
      grip.push_back(collision_->grip(cm.surface[t]));
      surface.push_back(cm.surface[t]);
    }
    collision_mesh_ = std::move(cm);
  } else {
    // Without collision: the world-space triangles world::ground_object keeps.
    positions.reserve(mesh.positions.size());
    grip.reserve(mesh.triangles());
    for (size_t t = 0; t < mesh.triangles(); ++t) {
      if (!mesh.ground[t]) continue;
      positions.push_back(mesh.positions[t * 3]);
      positions.push_back(mesh.positions[t * 3 + 1]);
      positions.push_back(mesh.positions[t * 3 + 2]);
      grip.push_back(placeholder_grip(mesh.surface[t]));
    }
  }
  ground_ = std::make_unique<drive::MeshGround>(std::move(positions), std::move(grip), std::move(surface));
  mesh_ = std::move(mesh);
  stats_ = std::move(stats);
  cx_ = x;
  cz_ = z;
  ++gathers_;
  if (changed) *changed = true;
  return true;
}

}  // namespace world
