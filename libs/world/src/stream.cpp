#include "world/stream.h"

#include "world/road_follow.h"

#include <chrono>
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


Streamer::Streamer(World& world, GatherOptions options, double regather_distance, Collision* collision,
                   double collision_radius)
    : world_(world),
      options_(options),
      regather_(regather_distance),
      collision_(collision),
      collision_radius_(collision_radius) {}

Streamer::~Streamer() {
  if (job_.valid()) job_.wait();
}

std::unique_ptr<Streamer::Gathered> Streamer::gather_now(double x, double z) const {
  const auto t0 = std::chrono::steady_clock::now();
  auto g = std::make_unique<Gathered>();
  g->x = x;
  g->z = z;
  if (!world_.gather(x, z, options_, g->mesh, &g->stats, &g->error)) return g;
  std::vector<std::array<float, 3>> positions;
  std::vector<float> grip;
  std::vector<uint8_t> surface;
  if (collision_) {
    // The ground: the collision's core triangles the car collides with.
    if (!collision_->gather(x, z, collision_radius_, g->collision, &g->error)) return g;
    const auto& cm = g->collision;
    positions.reserve(cm.positions.size());
    for (size_t t = 0; t < cm.triangles(); ++t) {
      if (!Collision::collides(cm, t)) continue;
      positions.push_back(cm.positions[t * 3]);
      positions.push_back(cm.positions[t * 3 + 1]);
      positions.push_back(cm.positions[t * 3 + 2]);
      grip.push_back(collision_->grip(cm.surface[t]));
      surface.push_back(cm.surface[t]);
      g->ground_source.push_back(uint32_t(t));
    }
  } else {
    // Without collision: the world-space triangles world::ground_object keeps.
    const auto& mesh = g->mesh;
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
  g->ground = std::make_unique<drive::MeshGround>(std::move(positions), std::move(grip), std::move(surface));
  if (collision_) {
    std::vector<drive::SurfaceTyre> tyres;
    for (const auto& s : collision_->surfaces()) tyres.push_back({s.rear_grip, s.off_roadness, s.off_road_peak_sa});
    g->ground->set_surface_tyres(std::move(tyres));
  }
  if (prepare_) g->prepared = prepare_(g->mesh, x, z);
  g->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  g->ok = true;
  return g;
}

void Streamer::take(Gathered&& g) {
  // Swapped, not moved: `g` leaves with the old gather (the caller frees it).
  std::swap(ground_, g.ground);
  std::swap(mesh_, g.mesh);
  stats_ = std::move(g.stats);
  std::swap(collision_mesh_, g.collision);
  std::swap(ground_source_, g.ground_source);
  std::swap(prepared_, g.prepared);
  last_seconds_ = g.seconds;
  cx_ = g.x;
  cz_ = g.z;
  ++gathers_;
}

bool Streamer::update(double x, double z, bool* changed, std::string* error) {
  if (changed) *changed = false;
  // A finished background gather: swap it in.
  if (job_.valid() && job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto g = job_.get();
    if (!g->ok) {
      if (error) *error = g->error;
      return false;
    }
    take(std::move(*g));
    // The old gather goes on a thread of its own, off the step.
    if (drop_.valid()) drop_.wait();
    drop_ = std::async(std::launch::async, [old = std::move(g)]() mutable { old.reset(); });
    if (changed) *changed = true;
    return true;
  }
  if (job_.valid()) return true;
  const bool forced = force_ && gathers_ > 0;
  if (force_) options_.hidden = pending_hidden_, force_ = false;
  if (!forced && gathers_ > 0 && std::hypot(x - cx_, z - cz_) <= regather_) return true;
  if (async_ && gathers_ > 0) {
    job_ = std::async(std::launch::async, [this, x, z] { return gather_now(x, z); });
    return true;
  }
  auto g = gather_now(x, z);
  if (!g->ok) {
    if (error) *error = g->error;
    return false;
  }
  take(std::move(*g));
  if (changed) *changed = true;
  return true;
}

}  // namespace world
