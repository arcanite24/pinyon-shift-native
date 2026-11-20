// The world over the player's own files (no game data in the repository):
// Colorado streamed around the free-roam spot of WORLD_GEOMETRY's frame
// dump, the render-road spawn search (the test driver's recovery), then
// the collision ground (COLLISION.md) at the measured spawn and a minute of
// the test driver on it. Skipped (exit 77) unless SHIFT_GAME_ROOT names the
// disc. Synthetic car: no bake needed.
// Spec: WORLD-GEOMETRY (selection by header box, props through the object
// table, the file frame), COLLISION.md (tiles, surfaces, spawn),
// REIMPLEMENTATION_PLAN M3. The render surfaces, LOD choice and the test
// driver are ours.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "drive/vehicle.h"
#include "world/road_follow.h"
#include "world/stream.h"
#include "world/world.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

drive::CarParams test_car() {
  drive::CarParams p;
  const double r = 0.31;
  p.wheel_centres = {drive::Vec3{-0.72, r, 1.15}, drive::Vec3{0.72, r, 1.15}, drive::Vec3{-0.72, r, -1.15},
                     drive::Vec3{0.72, r, -1.15}};
  p.tyre_radius = {r, r, r, r};
  p.centre_of_mass = {0, 0.5, 0.05};
  p.size = {1.64, 1.3, 3.9};
  return p;
}

}  // namespace

int main() {
  const char* root = std::getenv("SHIFT_GAME_ROOT");
  if (!root) {
    std::printf("world_tests: SHIFT_GAME_ROOT not set, skipped\n");
    return 77;
  }
  namespace fs = std::filesystem;
  const fs::path colorado = fs::path(root) / "media" / "tracks" / "colorado";
  const fs::path cache = fs::temp_directory_path() / "shift-engine-tests" / "colorado.index";
  world::World w;
  std::string error;
  if (!w.open(colorado, cache, &error)) {
    std::printf("FAIL open: %s\n", error.c_str());
    return 1;
  }
  // WORLD_GEOMETRY: 14,291 render models, every one parses; 11,898
  // world-space by our 200 m rule.
  size_t ok = 0, world_space = 0;
  for (const auto& m : w.models()) {
    ok += m.ok;
    world_space += m.ok && m.world_space;
  }
  std::printf("models %zu, parsed %zu, world-space %zu, prop placements %zu\n", w.models().size(), ok, world_space,
              w.placement_count());
  CHECK(w.models().size() == 14291 && ok == 14291 && world_space == 11898);
  CHECK(w.placement_count() > 0);

  // The dumped camera, game frame (file frame 1999.0, 145.4, -962.9).
  const double x = world::kDumpCameraX, z = world::kDumpCameraZ;
  world::Streamer render(w, world::GatherOptions{});
  CHECK(render.update(x, z, nullptr, &error));
  const auto& st = render.stats();
  size_t road = 0;
  for (auto s : render.mesh().surface) road += s == world::Surface::kRoad;
  std::printf("gathered %zu world-space models, %zu props, %zu triangles (%zu road, %zu in the stand-in ground)\n",
              st.world_models, st.props, render.mesh().triangles(), road, render.mesh_ground().triangle_count());
  CHECK(st.world_models > 100 && st.props > 100 && road > 1000);

  world::Spawn spawn;
  CHECK(world::find_spawn(render.mesh(), x, z, spawn, &world::kDumpCameraHeading));
  std::printf("spawn %.3f %.3f %.3f heading %.4f, %.2f m from the search point\n", spawn.position[0],
              spawn.position[1], spawn.position[2], spawn.heading, spawn.distance);
  CHECK(spawn.distance < 5);
  // The dumped camera stood 1.8 m above the road there (145.4 m).
  CHECK(spawn.position[1] > 140 && spawn.position[1] < 145.4);
  drive::Hit hit;
  CHECK(render.ground().raycast({spawn.position[0], spawn.position[1] + 2, spawn.position[2]}, {0, -1, 0}, 5, hit));
  CHECK(std::fabs(hit.distance - 2) < 0.05 && hit.normal.y > 0.9);

  // The collision ground and the measured spawn on it: Asphalt, 0.48 m
  // under the settled body reference point.
  const fs::path media = fs::path(root) / "media";
  world::Collision collision;
  std::string warning;
  if (!collision.open(colorado, media / "physics.zip", &error, &warning)) {
    std::printf("FAIL collision: %s\n", error.c_str());
    return 1;
  }
  world::Streamer stream(w, world::GatherOptions{}, 200, &collision);
  CHECK(stream.update(world::kSpawnX, world::kSpawnZ, nullptr, &error));
  const char* how = "";
  CHECK(world::choose_spawn(stream, nullptr, nullptr, nullptr, spawn, &how));
  CHECK(stream.ground().raycast({spawn.position[0], spawn.position[1] + 2, spawn.position[2]}, {0, -1, 0}, 5, hit));
  std::printf("collision: %zu tiles, %zu triangles in the ground; spawn %s %.3f %.3f %.3f heading %.4f on %s\n",
              stream.collision_mesh().tiles, stream.mesh_ground().triangle_count(), how, spawn.position[0],
              spawn.position[1], spawn.position[2], spawn.heading, collision.surface_name(hit.surface).c_str());
  CHECK(hit.surface == world::kSurfaceAsphalt);
  CHECK(std::fabs(world::kSpawnY - (spawn.position[1] + 2 - hit.distance) - 0.48) < 0.03);
  CHECK(std::fabs(spawn.heading * 180 / 3.14159265358979 - 126.1) < 0.1);

  // A minute of the test driver: on the ground throughout, moving on.
  drive::Vehicle v(test_car());
  v.reset({spawn.position[0], spawn.position[1], spawn.position[2]}, spawn.heading);
  world::RoadFollower follower;
  follower.rebuild(stream.mesh());
  follower.set_ground(&stream.ground());
  int airborne = 0, longest_airborne = 0, gathers = 0;
  double path = 0;
  drive::Vec3 last = v.position();
  for (int i = 0; i < 120 * 60; ++i) {
    v.step(follower.input(v), stream.ground());
    bool changed = false;
    const drive::Vec3 at = v.position();
    CHECK(stream.update(at.x, at.z, &changed, &error));
    if (changed) {
      follower.rebuild(stream.mesh());
      follower.set_ground(&stream.ground());
      ++gathers;
    }
    bool contact = false;
    for (const auto& wh : v.wheels()) contact = contact || wh.contact;
    airborne = contact ? 0 : airborne + 1;
    longest_airborne = std::max(longest_airborne, airborne);
    path += std::hypot(at.x - last.x, at.z - last.z);
    last = at;
  }
  const drive::Vec3 up = drive::rotate(v.orientation(), {0, 1, 0});
  std::printf("a minute of the test driver: %.0f m driven, %d gathers, longest airborne %.2f s, upright %.2f\n",
              path, gathers, longest_airborne * drive::Vehicle::kStep, up.y);
  CHECK(path > 400);
  CHECK(longest_airborne < 120);
  CHECK(up.y > 0.8);

  // A background gather: the car keeps the old ground until the new one is
  // swapped in by an update, then the centre and ground are the new ones.
  {
    world::Streamer bg(w, world::GatherOptions{}, 200, &collision);
    bg.set_async(true);
    CHECK(bg.update(world::kSpawnX, world::kSpawnZ, nullptr, &error));  // the first gather runs in the call
    const double fx = world::kSpawnX + 300, fz = world::kSpawnZ;
    bool changed = false;
    CHECK(bg.update(fx, fz, &changed, &error));
    CHECK(!changed && bg.gathering() && bg.centre_x() == world::kSpawnX);
    const auto t0 = std::chrono::steady_clock::now();
    int polls = 0;
    while (!changed && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      CHECK(bg.update(fx, fz, &changed, &error));
      ++polls;
    }
    std::printf("background gather: swapped after %d polls (%.2f s on the worker), centre %.1f %.1f, %zu "
                "triangles in the ground\n",
                polls, bg.last_gather_seconds(), bg.centre_x(), bg.centre_z(), bg.mesh_ground().triangle_count());
    CHECK(changed && bg.centre_x() == fx && bg.gathers() == 2 && !bg.gathering());
    CHECK(bg.mesh_ground().triangle_count() > 1000);
  }

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("world_tests: all passed\n");
  return 0;
}
