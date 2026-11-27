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

// Uses exact .fiz collision first, falling back to visual terrain ground only
// where the .fiz strip terminates (e.g. off-strip dirt yard east of Bugatti barn).
class FallbackGround final : public drive::Ground {
 public:
  FallbackGround(const drive::MeshGround* primary, std::unique_ptr<drive::MeshGround> fallback)
      : primary_(primary), fallback_(std::move(fallback)) {}

  bool raycast(const drive::Vec3& origin, const drive::Vec3& dir, double max_distance, drive::Hit& hit) const override {
    if (primary_ && primary_->raycast(origin, dir, max_distance, hit)) return true;
    // Only fall back for downward wheel/ground probes, never horizontal wall rays.
    if (fallback_ && dir.y < -0.5 && fallback_->raycast(origin, dir, max_distance, hit)) {
      if (hit.surface < 0) hit.surface = 1;  // Dirt
      return true;
    }
    return false;
  }
  // The body spheres meet the .fiz collision only (the fallback terrain is
  // for the wheels).
  void sphere_contacts(const drive::Vec3& inside, const drive::SphereQuery* spheres, int n,
                       drive::SphereHit* hits) const override {
    if (primary_) primary_->sphere_contacts(inside, spheres, n, hits);
    else for (int i = 0; i < n; ++i) hits[i] = {};
  }

 private:
  const drive::MeshGround* primary_ = nullptr;
  std::unique_ptr<drive::MeshGround> fallback_;
};

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

bool resolve_fast_travel_spawn(const drive::Ground& ground, const Mesh* mesh, double& x, double& y, double& z,
                               bool has_hint_y, double hint_y) {
  auto find_ground_y = [&](double px, double pz, bool& open_sky, double& out_y) -> bool {
    open_sky = false;
    drive::Hit top_hit;
    const bool got_top = ground.raycast({px, 2000.0, pz}, {0, -1, 0}, 4000.0, top_hit);
    const double y_top = got_top ? (2000.0 - top_hit.distance) : 0.0;

    if (has_hint_y) {
      drive::Hit h;
      if (ground.raycast({px, hint_y + 3.0, pz}, {0, -1, 0}, 20.0, h) && h.normal.y >= 0.90) {
        out_y = (hint_y + 3.0) - h.distance;
        open_sky = !got_top || std::fabs(y_top - out_y) <= 1.0;
        return true;
      }
    }
    if (!got_top) return false;
    double best_y = y_top;
    bool best_flat = (top_hit.normal.y >= 0.90);
    // Step beneath overhead canopy/sign/arch roofs if a real flat ground surface sits below.
    double probe_y = best_y - 1.5;
    for (int step = 0; step < 4; ++step) {
      drive::Hit below;
      if (!ground.raycast({px, probe_y, pz}, {0, -1, 0}, 25.0, below)) break;
      const double by = probe_y - below.distance;
      if (below.normal.y >= 0.90) {
        best_y = by;
        best_flat = true;
      }
      probe_y = by - 1.5;
    }
    if (!best_flat) return false;
    out_y = best_y;
    open_sky = std::fabs(y_top - best_y) <= 1.0;
    return true;
  };

  auto has_clearance = [&](double px, double py, double pz) -> bool {
    // Reject low ceilings / arch bases (< 2.8 m vertical headroom).
    drive::Hit up;
    if (ground.raycast({px, py + 0.35, pz}, {0, 1, 0}, 2.8, up)) return false;
    // Reject enclosing walls/pillars/barriers within 3.0 m horizontally at bumper & cabin heights.
    constexpr double kPi = 3.14159265358979323846;
    for (double hy : {0.65, 1.35}) {
      for (int i = 0; i < 8; ++i) {
        const double a = i * (kPi * 0.25);
        drive::Hit side;
        if (ground.raycast({px, py + hy, pz}, {std::sin(a), 0, std::cos(a)}, 3.0, side) &&
            std::fabs(side.normal.y) < 0.70) {
          return false;
        }
      }
    }
    return true;
  };

  // First check the exact requested point: if unobstructed with open sky (or valid headroom under
  // a high festival banner arch), keep the authored coordinates untouched.
  double y0 = 0.0;
  bool sky0 = false;
  if (find_ground_y(x, z, sky0, y0) && has_clearance(x, y0, z)) {
    y = y0;
    return true;
  }

  // Otherwise search expanding rings for the closest unobstructed open-sky spot (preferring spots
  // near hint_y when known so we don't jump onto a raised embankment).
  constexpr double kPi = 3.14159265358979323846;
  bool found = false;
  double best_score = 1e30;
  double best_x = x, best_y = y, best_z = z;
  for (double r : {4.0, 7.0, 10.0, 13.0, 16.0, 20.0, 25.0}) {
    for (int i = 0; i < 16; ++i) {
      const double a = i * (2.0 * kPi / 16.0);
      const double px = x + r * std::sin(a);
      const double pz = z + r * std::cos(a);
      double py = 0.0;
      bool sky = false;
      if (!find_ground_y(px, pz, sky, py)) continue;
      if (has_hint_y && std::fabs(py - hint_y) > 4.5) continue;
      if (!has_clearance(px, py, pz)) continue;
      const double score = r + (sky ? 0.0 : 5.0) + (has_hint_y ? 0.5 * std::fabs(py - hint_y) : 0.0);
      if (score < best_score) {
        best_score = score;
        best_x = px;
        best_y = py;
        best_z = pz;
        found = true;
      }
    }
    if (found) break;
  }
  if (found) {
    x = best_x;
    y = best_y;
    z = best_z;
    return true;
  }

  // Fallback: keep the ground elevation below any overhang, or nearest road triangle from mesh.
  if (find_ground_y(x, z, sky0, y0)) {
    y = y0;
    return true;
  }
  if (mesh) {
    Spawn fsp;
    if (find_spawn(*mesh, x, z, fsp)) {
      x = fsp.position[0];
      y = fsp.position[1];
      z = fsp.position[2];
      return true;
    }
  }
  return false;
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
  if (collision_) {
    g->ground = std::make_unique<drive::MeshGround>(std::move(positions), std::move(grip), std::move(surface));
    std::vector<drive::SurfaceTyre> tyres;
    for (const auto& s : collision_->surfaces()) {
      drive::SurfaceTyre t;
      t.rear_grip = s.rear_grip;
      t.off_roadness = s.off_roadness;
      t.off_road_peak_sa = s.off_road_peak_sa;
      t.handbrake_grip = s.handbrake_grip;
      t.tcs_speed_scale = s.tcs_speed_scale;
      t.max_steering_lock_cap = s.max_steering_lock_cap;
      t.steering_rate_mult = s.steering_rate_mult;
      t.aero_drag_scale = s.aero_drag_scale;
      t.vel_peak0 = s.vel_peak0;
      t.vel_peak = s.vel_peak;
      t.vel_long0 = s.vel_long0;
      t.vel_long = s.vel_long;
      t.vel_lat0 = s.vel_lat0;
      t.vel_lat = s.vel_lat;
      t.bump_noise_type = s.bump_noise_type;
      t.bump_frequency = s.bump_frequency;
      t.bump_clamp_min = s.bump_clamp_min;
      t.bump_clamp_max = s.bump_clamp_max;
      t.bump_scale = s.bump_scale;
      t.bump_offset = s.bump_offset;
      t.bump_rumble = s.bump_rumble;
      t.is_springy = s.is_springy;
      t.spring_k = s.spring_k;
      t.spring_comp_d = s.spring_comp_d;
      t.spring_reb_d = s.spring_reb_d;
      t.spring_norm_y_cutoff = s.spring_norm_y_cutoff;
      t.spring_speed_cutoff = s.spring_speed_cutoff;
      t.is_penalty_surface = s.is_penalty_surface;
      t.is_hand_of_god = s.is_hand_of_god;
      tyres.push_back(t);
    }
    g->ground->set_surface_tyres(tyres);
    // Build a visual terrain fallback for off-strip .fiz edge aprons (e.g. Bugatti barn east).
    std::vector<std::array<float, 3>> fb_pos;
    std::vector<float> fb_grip;
    std::vector<uint8_t> fb_surf;
    const auto& mesh = g->mesh;
    for (size_t t = 0; t < mesh.triangles(); ++t) {
      if (!mesh.ground[t]) continue;
      fb_pos.push_back(mesh.positions[t * 3]);
      fb_pos.push_back(mesh.positions[t * 3 + 1]);
      fb_pos.push_back(mesh.positions[t * 3 + 2]);
      fb_grip.push_back(placeholder_grip(mesh.surface[t]));
      fb_surf.push_back(mesh.surface[t] == Surface::kRoad ? uint8_t(0) : uint8_t(1));
    }
    std::unique_ptr<drive::MeshGround> fallback;
    if (!fb_pos.empty()) {
      fallback = std::make_unique<drive::MeshGround>(std::move(fb_pos), std::move(fb_grip), std::move(fb_surf));
      fallback->set_surface_tyres(std::move(tyres));
    }
    g->composite_ground = std::make_unique<FallbackGround>(g->ground.get(), std::move(fallback));
  } else {
    g->ground = std::make_unique<drive::MeshGround>(std::move(positions), std::move(grip), std::move(surface));
  }
  if (prepare_) g->prepared = prepare_(g->mesh, x, z);
  g->raw_triangles = g->mesh.triangles();
  g->raw_materials = g->mesh.materials.size();
  if (compact_after_prepare_ && prepare_) {
    const size_t tris = g->mesh.triangles();
    size_t road_tris = 0;
    for (size_t t = 0; t < tris; ++t) {
      if (g->mesh.surface[t] == Surface::kRoad) ++road_tris;
    }
    std::vector<std::array<float, 3>> road_pos;
    std::vector<Surface> road_surf;
    road_pos.reserve(road_tris * 3);
    road_surf.reserve(road_tris);
    for (size_t t = 0; t < tris; ++t) {
      if (g->mesh.surface[t] != Surface::kRoad) continue;
      road_pos.push_back(g->mesh.positions[t * 3]);
      road_pos.push_back(g->mesh.positions[t * 3 + 1]);
      road_pos.push_back(g->mesh.positions[t * 3 + 2]);
      road_surf.push_back(Surface::kRoad);
    }
    g->mesh.positions.swap(road_pos);
    g->mesh.surface.swap(road_surf);
    std::vector<uint32_t>().swap(g->mesh.model);
    std::vector<uint8_t>().swap(g->mesh.ground);
    std::vector<std::array<float, 3>>().swap(g->mesh.normals);
    std::vector<std::array<float, 2>>().swap(g->mesh.uv0);
    std::vector<std::array<float, 2>>().swap(g->mesh.uv1);
    std::vector<std::array<float, 2>>().swap(g->mesh.uv2);
    std::vector<std::array<uint8_t, 4>>().swap(g->mesh.colours);
    std::vector<uint32_t>().swap(g->mesh.material);
    std::vector<std::array<float, 3>>().swap(g->mesh.instance);
    std::vector<MeshMaterial>().swap(g->mesh.materials);
    std::vector<WorldGlow>().swap(g->mesh.world_glows);
  }
  g->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  g->ok = true;
  return g;
}

void Streamer::set_options(const GatherOptions& options) {
  if (job_.valid()) (void)job_.get();
  options_ = options;
  pending_hidden_ = options.hidden;
  pending_hidden_positions_ = options.hidden_positions;
  has_pending_hidden_positions_ = false;
  preload_active_ = false;
  force_ = true;
}

bool Streamer::preload_async(double x, double z) {
  if (!async_ || gathers_ == 0) return false;
  preload_active_ = true;
  preload_x_ = x;
  preload_z_ = z;
  if (std::hypot(x - target_x(), z - target_z()) <= regather_) return true;
  if (job_.valid()) return true;
  if (force_) options_.hidden = pending_hidden_, force_ = false;
  if (has_pending_hidden_positions_) {
    options_.hidden_positions = pending_hidden_positions_;
    has_pending_hidden_positions_ = false;
  }
  job_cx_ = x;
  job_cz_ = z;
  job_ = std::async(std::launch::async, [this, x, z] { return gather_now(x, z); });
  return true;
}

void Streamer::take(Gathered&& g) {
  // Swapped, not moved: `g` leaves with the old gather (the caller frees it).
  std::swap(ground_, g.ground);
  std::swap(composite_ground_, g.composite_ground);
  std::swap(mesh_, g.mesh);
  last_mesh_triangles_ = g.raw_triangles ? g.raw_triangles : mesh_.triangles();
  last_mesh_materials_ = g.raw_materials ? g.raw_materials : mesh_.materials.size();
  stats_ = std::move(g.stats);
  std::swap(collision_mesh_, g.collision);
  std::swap(ground_source_, g.ground_source);
  std::swap(prepared_, g.prepared);
  last_seconds_ = g.seconds;
  cx_ = g.x;
  cz_ = g.z;
  job_cx_ = cx_;
  job_cz_ = cz_;
  ++gathers_;
}

void Streamer::release_render_mesh() {
  if (!mesh_.has_render_data()) return;
  // Compact positions + surface down to Surface::kRoad triangles only so
  // find_spawn() / recover() keep working without holding ~180 MB of CPU
  // render vertex attributes after GPU staging.
  const size_t tris = mesh_.triangles();
  size_t road_tris = 0;
  for (size_t t = 0; t < tris; ++t) {
    if (mesh_.surface[t] == Surface::kRoad) ++road_tris;
  }
  std::vector<std::array<float, 3>> road_pos;
  std::vector<Surface> road_surf;
  road_pos.reserve(road_tris * 3);
  road_surf.reserve(road_tris);
  for (size_t t = 0; t < tris; ++t) {
    if (mesh_.surface[t] != Surface::kRoad) continue;
    road_pos.push_back(mesh_.positions[t * 3]);
    road_pos.push_back(mesh_.positions[t * 3 + 1]);
    road_pos.push_back(mesh_.positions[t * 3 + 2]);
    road_surf.push_back(Surface::kRoad);
  }
  mesh_.positions.swap(road_pos);
  mesh_.surface.swap(road_surf);
  std::vector<uint32_t>().swap(mesh_.model);
  std::vector<uint8_t>().swap(mesh_.ground);
  std::vector<std::array<float, 3>>().swap(mesh_.normals);
  std::vector<std::array<float, 2>>().swap(mesh_.uv0);
  std::vector<std::array<float, 2>>().swap(mesh_.uv1);
  std::vector<std::array<float, 2>>().swap(mesh_.uv2);
  std::vector<std::array<uint8_t, 4>>().swap(mesh_.colours);
  std::vector<uint32_t>().swap(mesh_.material);
  std::vector<std::array<float, 3>>().swap(mesh_.instance);
  std::vector<MeshMaterial>().swap(mesh_.materials);
  std::vector<WorldGlow>().swap(mesh_.world_glows);
}

bool Streamer::update(double x, double z, bool* changed, std::string* error) {
  if (changed) *changed = false;
  const double dist = (gathers_ > 0) ? std::hypot(x - cx_, z - cz_) : 0.0;
  // Never let a fast car outrun the loaded collision mesh while an async gather is in flight.
  const double hard_edge = std::max(regather_ + 80.0, collision_radius_ - 140.0);
  const bool must_wait_job = job_.valid() && gathers_ > 0 && dist >= hard_edge;
  // A finished background gather (or one we must wait for before leaving collision): swap it in.
  if (job_.valid() && (must_wait_job || job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
    auto g = job_.get();
    if (!g->ok) {
      if (error) *error = g->error;
      return false;
    }
    take(std::move(*g));
    // Once the car is within regather_ of the newly swapped centre, any advisory preload hold is satisfied.
    if (preload_active_ && std::hypot(x - cx_, z - cz_) <= regather_) preload_active_ = false;
    // The old gather goes on a thread of its own, off the step.
    if (drop_.valid()) drop_.wait();
    drop_ = std::async(std::launch::async, [old = std::move(g)]() mutable { old.reset(); });
    if (changed) *changed = true;
    return true;
  }
  if (job_.valid()) return true;
  const bool forced = force_ && gathers_ > 0;
  if (force_) options_.hidden = pending_hidden_, force_ = false;
  if (has_pending_hidden_positions_) {
    options_.hidden_positions = pending_hidden_positions_;
    has_pending_hidden_positions_ = false;
  }
  if (preload_active_ && gathers_ > 0) {
    // Safety: always drop an advisory preload hold before the car can leave collision.
    if (dist >= hard_edge) {
      preload_active_ = false;
    } else if (!forced && async_ && std::hypot(preload_x_ - cx_, preload_z_ - cz_) > regather_) {
      job_cx_ = preload_x_;
      job_cz_ = preload_z_;
      const double px = preload_x_, pz = preload_z_;
      job_ = std::async(std::launch::async, [this, px, pz] { return gather_now(px, pz); });
      return true;
    } else if (!forced) {
      // Preloaded target is already resident and the car is safely inside collision.
      return true;
    }
  }
  if (!forced && gathers_ > 0 && dist <= regather_) return true;
  if (async_ && gathers_ > 0) {
    // With set_lead: lead the gather centre ahead along the travel vector so a
    // fast car stays centred inside the new collision bubble when the async
    // gather finishes. Off by default: the centre is the point asked for.
    double gx = x, gz = z;
    if (lead_ && !forced && dist > 50.0) {
      const double lead = std::min(150.0, collision_radius_ * 0.20);
      gx += (x - cx_) / dist * lead;
      gz += (z - cz_) / dist * lead;
    }
    job_cx_ = gx;
    job_cz_ = gz;
    job_ = std::async(std::launch::async, [this, gx, gz] { return gather_now(gx, gz); });
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
