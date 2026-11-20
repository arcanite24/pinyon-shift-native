// The stand-in world over the player's own files (no game data in the
// repository): Colorado streamed around the free-roam spot of
// WORLD_GEOMETRY's frame dump, the stand-in spawn, the mesh ground under it
// and a minute of the test driver on the road. Skipped (exit 77) unless
// SHIFT_GAME_ROOT names the disc. Synthetic car: no bake needed.
// Spec: WORLD-GEOMETRY (selection by header box, props through the object
// table, the file frame), REIMPLEMENTATION_PLAN M3. The surfaces, LOD
// choice, spawn and ground are our stand-ins until COLLISION.md.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

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
  world::Streamer stream(w, world::GatherOptions{});
  CHECK(stream.update(x, z, nullptr, &error));
  const auto& st = stream.stats();
  size_t road = 0;
  for (auto s : stream.mesh().surface) road += s == world::Surface::kRoad;
  std::printf("gathered %zu world-space models, %zu props, %zu triangles (%zu road, %zu in the ground)\n",
              st.world_models, st.props, stream.mesh().triangles(), road, stream.mesh_ground().triangle_count());
  CHECK(st.world_models > 100 && st.props > 100 && road > 1000);

  world::Spawn spawn;
  CHECK(world::find_spawn(stream.mesh(), x, z, spawn, &world::kDumpCameraHeading));
  std::printf("spawn %.3f %.3f %.3f heading %.4f, %.2f m from the search point\n", spawn.position[0],
              spawn.position[1], spawn.position[2], spawn.heading, spawn.distance);
  CHECK(spawn.distance < 5);
  // The dumped camera stood 1.8 m above the road there (145.4 m).
  CHECK(spawn.position[1] > 140 && spawn.position[1] < 145.4);
  drive::Hit hit;
  CHECK(stream.ground().raycast({spawn.position[0], spawn.position[1] + 2, spawn.position[2]}, {0, -1, 0}, 5, hit));
  CHECK(std::fabs(hit.distance - 2) < 0.05 && hit.normal.y > 0.9);

  // A minute of the test driver: on the ground throughout, moving on.
  drive::Vehicle v(test_car());
  v.reset({spawn.position[0], spawn.position[1], spawn.position[2]}, spawn.heading);
  world::RoadFollower follower;
  follower.rebuild(stream.mesh());
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

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("world_tests: all passed\n");
  return 0;
}
