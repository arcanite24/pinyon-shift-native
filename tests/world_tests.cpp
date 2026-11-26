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
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "drive/mesh_ground.h"
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

  // WORLD-MATERIALS: the texture records by type (4 .bix, 0 CAFF, 1 and 3
  // no file), and a gather with its render data around the dumped camera:
  // every slot-0 texture decodes and every vertex layout is known.
  {
    size_t types[5] = {};
    for (const auto& t : w.textures())
      if (t.type < 5) ++types[t.type];
    std::printf("texture records %zu: %zu .bix, %zu CAFF, %zu type 1, %zu type 3\n", w.textures().size(), types[4],
                types[0], types[1], types[3]);
    CHECK(w.textures().size() == 19613 && types[4] == 5234 && types[0] == 2965 && types[1] == 11409 && types[3] == 5);
    world::GatherOptions o;
    o.render_data = true;
    world::Mesh mesh;
    world::GatherStats gs;
    CHECK(w.gather(world::kDumpCameraX, world::kDumpCameraZ, o, mesh, &gs, &error));
    CHECK(mesh.material.size() == mesh.triangles() && mesh.uv0.size() == mesh.positions.size() &&
          mesh.normals.size() == mesh.positions.size());
    std::set<uint32_t> textures;
    size_t untextured = 0;
    size_t vertex_blend = 0, splat = 0;
    for (const auto& m : mesh.materials) {
      if (m.albedo != world::kNoTexture) textures.insert(m.albedo);
      else ++untextured;
      // Terrain layers (WORLD-MATERIALS, Terrain shading): Blend_B and
      // Splat decode too.
      if (m.layer_b != world::kNoTexture) textures.insert(m.layer_b);
      if (m.splat != world::kNoTexture) textures.insert(m.splat);
      vertex_blend += m.layers == world::Layers::kVertexBlend && m.layer_b != world::kNoTexture;
      splat += m.layers == world::Layers::kSplat && m.splat != world::kNoTexture;
    }
    std::printf("terrain layers: %zu vertex-blended materials with Blend_B, %zu with a Splat\n", vertex_blend, splat);
    CHECK(vertex_blend > 0 && splat > 0);
    size_t decoded = 0;
    for (uint32_t r : textures) {
      fh1::WorldTexture t;
      std::string e;
      if (w.read_texture(r, t, &e) && t.blocks.levels.size() == t.levels) ++decoded;
      else std::printf("  texture record %u: %s\n", r, e.c_str());
    }
    std::printf("render data: %zu materials (%zu without a slot-0 file), %zu textures, %zu decode; %zu models with "
                "the default UV scale, %zu submeshes without a material\n",
                mesh.materials.size(), untextured, textures.size(), decoded, gs.default_uv_models.size(),
                gs.no_material);
    CHECK(!textures.empty() && decoded == textures.size() && gs.no_material == 0);

    // A bundle-only record (WORLD-MATERIALS, Small textures): the power-line
    // cables' 4203, a 4 x 4 DXT1 of 3 levels, level 0 all (33, 34, 33).
    {
      fh1::WorldTexture cable;
      std::string e;
      const bool ok = w.read_texture(4203, cable, &e);
      if (!ok) std::printf("  record 4203: %s\n", e.c_str());
      CHECK(ok && cable.width == 4 && cable.height == 4 && cable.levels == 3 && cable.blocks.levels.size() == 3);
      if (ok && cable.blocks.levels[0].blocks.size() >= 8) {
        const uint8_t* b = cable.blocks.levels[0].blocks.data();
        // Texel 0 of the first DXT1 block (four-colour mode when c0 > c1).
        const uint16_t c[2] = {uint16_t(b[0] | b[1] << 8), uint16_t(b[2] | b[3] << 8)};
        int rgb[2][3];
        for (int k = 0; k < 2; ++k)
          rgb[k][0] = (c[k] >> 11) * 255 / 31, rgb[k][1] = ((c[k] >> 5) & 63) * 255 / 63, rgb[k][2] = (c[k] & 31) * 255 / 31;
        const int index = b[4] & 3;
        int texel[3];
        for (int i = 0; i < 3; ++i)
          texel[i] = index < 2 ? rgb[index][i]
                     : c[0] > c[1] ? (index == 2 ? (2 * rgb[0][i] + rgb[1][i]) / 3 : (rgb[0][i] + 2 * rgb[1][i]) / 3)
                                   : (index == 2 ? (rgb[0][i] + rgb[1][i]) / 2 : 0);
        const int r = texel[0], g = texel[1], bl = texel[2];
        std::printf("  record 4203 texel 0: %d %d %d\n", r, g, bl);
        CHECK(std::abs(r - 33) <= 4 && std::abs(g - 34) <= 4 && std::abs(bl - 33) <= 4);
      }
    }

    // The far ring (ours): MIDDIST and UberLOD terrain beyond far_inner,
    // never ground or road, its materials marked far.
    world::GatherOptions fo = o;
    fo.far_radius = 3000;
    world::Mesh far_mesh;
    world::GatherStats fs;
    CHECK(w.gather(world::kDumpCameraX, world::kDumpCameraZ, fo, far_mesh, &fs, &error));
    size_t far_tris = 0, far_bad = 0, far_inside = 0;
    for (size_t t = 0; t < far_mesh.triangles(); ++t) {
      if (!far_mesh.materials[far_mesh.material[t]].far) continue;
      ++far_tris;
      far_bad += far_mesh.ground[t] || far_mesh.surface[t] == world::Surface::kRoad;
      bool inside = true;
      for (int k = 0; k < 3; ++k) {
        const auto& p = far_mesh.positions[t * 3 + size_t(k)];
        inside = inside && std::hypot(p[0] - world::kDumpCameraX, p[2] - world::kDumpCameraZ) < fo.far_inner;
      }
      far_inside += inside;
    }
    std::printf("far ring: %zu models, %zu triangles (%zu marked far, %zu within far_inner over near-ground holes), "
                "near triangles %zu as before\n",
                fs.far_models, fs.far_triangles, far_tris, fs.far_fill_triangles, far_mesh.triangles() - far_tris);
    CHECK(fs.far_models > 50 && far_tris == fs.far_triangles && far_tris > 10000);
    CHECK(far_bad == 0 && far_inside <= fs.far_fill_triangles);
    CHECK(far_mesh.triangles() - far_tris == mesh.triangles());

    // Tree sprites (WORLD-MATERIALS, Tree billboards): the file records
    // near the camera stand on the drawn terrain (the spec: all within 1 m,
    // median 0.08 m below) and use the frame's atlases.
    world::GatherOptions to = o;
    to.tree_radius = 600;
    world::Mesh tree_mesh;
    world::GatherStats ts;
    CHECK(w.gather(world::kDumpCameraX, world::kDumpCameraZ, to, tree_mesh, &ts, &error));
    std::vector<std::array<float, 3>> terrain;
    for (size_t t = 0; t < mesh.triangles(); ++t)
      if (mesh.surface[t] != world::Surface::kProp)  // the drawn world-space models
        for (int k = 0; k < 3; ++k) terrain.push_back(mesh.positions[t * 3 + size_t(k)]);
    const drive::MeshGround ground(std::move(terrain));
    std::vector<double> below;
    std::set<uint32_t> atlases;
    size_t sprites = 0;
    for (size_t t = 0; t < tree_mesh.triangles(); ++t) {
      const auto& m = tree_mesh.materials[tree_mesh.material[t]];
      if (m.layers != world::Layers::kBillboard) continue;
      atlases.insert(w.textures()[m.albedo].id);
      if (++sprites % 2 == 0) continue;  // two triangles per sprite
      const auto& b = tree_mesh.positions[t * 3];
      drive::Hit h;
      if (ground.raycast({b[0], b[1] + 40.0, b[2]}, {0, -1, 0}, 80, h)) below.push_back(40.0 - h.distance);  // ground minus base
    }
    std::sort(below.begin(), below.end());
    size_t within = 0;
    for (double d : below) within += std::fabs(d) < 1.0;
    const double median = below.empty() ? 0 : below[below.size() / 2];
    std::string names;
    for (uint32_t id : atlases) names += " " + std::to_string(id);
    std::printf("tree sprites within 600 m: %zu (%zu without an atlas), %zu over the terrain, %zu within 1 m, median "
                "%.2f m below; atlas IDs%s\n",
                ts.trees, ts.trees_untextured, below.size(), within, median, names.c_str());
    CHECK(ts.trees > 3000 && ts.trees_untextured == 0);
    CHECK(!below.empty() && within >= below.size() * 99 / 100 && std::fabs(median) < 0.3);
    CHECK(tree_mesh.triangles() == mesh.triangles() + 2 * ts.trees);
  }

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

  // Named props hidden by GameplayID (SPEC-FREEROAM-COLLECTIBLES): the
  // first discount sign stands at 1410.35, 128.44, 3126.53 (GameObjs.xml).
  {
    world::GatherOptions o;
    o.hidden = std::make_shared<const std::set<std::string>>(std::set<std::string>{"flyer_001", "flyer_002"});
    world::Streamer near_sign(w, o);
    CHECK(near_sign.update(1410.35, 3126.53, nullptr, &error));
    std::printf("near flyer_001: %zu props, %zu hidden by name\n", near_sign.stats().props,
                near_sign.stats().props_hidden);
    CHECK(near_sign.stats().props_hidden == 1);
    // A barn's door variants and its car are named props too (DoorInfo,
    // TriggerZone object), one placement each.
    o.hidden = std::make_shared<const std::set<std::string>>(
        std::set<std::string>{"bf_bug_eb110bf_92_closed", "bf_bug_eb110bf_92_open", "bf_bug_eb110bf_92_closedc",
                              "bf_bug_eb110bf_92_openc", "barnfind_bug_eb110bf_92"});
    world::Streamer barn(w, o);
    CHECK(barn.update(1418.5, -5366.8, nullptr, &error));
    std::printf("barn find 01: %zu of %zu props hidden by name\n", barn.stats().props_hidden, barn.stats().props);
    CHECK(barn.stats().props_hidden == 5);

    // Regression: Streamer::set_options() across a route change (0 -> 42 -> 0) MUST preserve
    // hidden discount signs (options.hidden + options.hidden_positions) and clear any stale
    // pending_hidden_positions_ so the smashed sign never reappears after a race!
    {
      world::GatherOptions ro;
      ro.route = 0;
      world::Streamer sign_stream(w, ro);
      CHECK(sign_stream.update(1410.35, 3126.53, nullptr, &error));
      CHECK(sign_stream.stats().props_hidden == 0);

      // Simulate smashing flyer_001 during free roam: update hidden + hidden_positions,
      // and leave a stale pending_hidden_positions_ (nullptr) queued before set_options.
      ro.hidden = std::make_shared<const std::set<std::string>>(std::set<std::string>{"flyer_001"});
      auto hpos = std::make_shared<std::vector<world::GatherOptions::HiddenSphere>>();
      hpos->push_back({{1410.35f, 128.44f, 3126.53f}, 3.5f, true});
      ro.hidden_positions = hpos;
      sign_stream.set_hidden_positions(nullptr);  // stale pending clear that set_options must discard

      // Enter race route 42 (free-roam-only discount signs are filtered out by route),
      // then return to free roam route 0 via set_options(): flyer_001 MUST stay hidden!
      ro.route = 42;
      sign_stream.set_options(ro);
      CHECK(sign_stream.update(1410.35, 3126.53, nullptr, &error));
      CHECK(sign_stream.stats().props_off_route > 0);

      ro.route = 0;
      sign_stream.set_options(ro);
      CHECK(sign_stream.update(1410.35, 3126.53, nullptr, &error));
      CHECK(sign_stream.stats().props_hidden >= 1);

      // Verify persistent hidden_positions alone (even without options.hidden) still hides flyer_001.
      ro.hidden = nullptr;
      sign_stream.set_options(ro);
      CHECK(sign_stream.update(1410.35, 3126.53, nullptr, &error));
      CHECK(sign_stream.stats().props_hidden >= 1);

      // Verify a non-persistent hidden sphere with large radius (4.5 m) placed 2.5 m away from
      // flyer_001 does NOT accidentally hide flyer_001, while placing it at flyer_001 (< 0.05 m) DOES.
      auto non_persist_far = std::make_shared<std::vector<world::GatherOptions::HiddenSphere>>();
      non_persist_far->push_back({{1412.85f, 128.44f, 3126.53f}, 4.5f, false});
      ro.hidden_positions = non_persist_far;
      sign_stream.set_options(ro);
      CHECK(sign_stream.update(1410.35, 3126.53, nullptr, &error));
      CHECK(sign_stream.stats().props_hidden == 0);

      auto non_persist_exact = std::make_shared<std::vector<world::GatherOptions::HiddenSphere>>();
      non_persist_exact->push_back({{1410.35f, 128.44f, 3126.53f}, 4.5f, false});
      ro.hidden_positions = non_persist_exact;
      sign_stream.set_options(ro);
      CHECK(sign_stream.update(1410.35, 3126.53, nullptr, &error));
      CHECK(sign_stream.stats().props_hidden >= 1);
    }
  }

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

  // Walls: from the spawn, full throttle and steering right after 2 s puts
  // the car into the concrete barrier at about 50 km/h. It must hit it (a
  // sharp loss of speed) and never cross a wall: no upright collision
  // triangle between its centre at one step and the next (the body's
  // contact points against the collision).
  {
    drive::Vehicle wv(test_car());
    wv.reset({spawn.position[0], spawn.position[1], spawn.position[2]}, spawn.heading);
    double sharpest = 0;  // the largest speed loss over 0.25 s, km/h
    int crossings = 0;
    std::vector<double> kmh;
    for (int i = 0; i < 120 * 8; ++i) {
      drive::Input in;
      in.throttle = i < 240 ? 1.0 : 0.8;
      in.steer = i < 240 ? 0.0 : 0.6;
      const drive::Vec3 before = wv.position();
      wv.step(in, stream.ground());
      const drive::Vec3 now = wv.position();
      const drive::Vec3 from{before.x, before.y + 0.4, before.z};
      const drive::Vec3 d{now.x - before.x, now.y - before.y, now.z - before.z};
      const double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
      drive::Hit h;
      if (len > 1e-6 && stream.ground().raycast(from, d * (1 / len), len, h) && std::fabs(h.normal.y) < 0.5)
        ++crossings;
      kmh.push_back(std::fabs(wv.speed()) * 3.6);
      if (kmh.size() > 30) sharpest = std::max(sharpest, kmh[kmh.size() - 31] - kmh.back());
    }
    std::printf("wall: sharpest speed loss %.1f km/h in 0.25 s, %d wall crossings\n", sharpest, crossings);
    CHECK(sharpest > 20);
    CHECK(crossings == 0);
  }

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

    // With the lead (carview's live drive), the same move centres the gather
    // min(150 m, 0.2 x the 750 m collision radius) further along it.
    world::Streamer led(w, world::GatherOptions{}, 200, &collision);
    led.set_async(true);
    led.set_lead(true);
    CHECK(led.update(world::kSpawnX, world::kSpawnZ, nullptr, &error));
    changed = false;
    CHECK(led.update(fx, fz, &changed, &error));
    const auto t1 = std::chrono::steady_clock::now();
    while (!changed && std::chrono::steady_clock::now() - t1 < std::chrono::seconds(60)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      CHECK(led.update(fx, fz, &changed, &error));
    }
    std::printf("led background gather: centre %.1f %.1f\n", led.centre_x(), led.centre_z());
    CHECK(changed && std::fabs(led.centre_x() - (fx + 150)) < 1e-6 && std::fabs(led.centre_z() - fz) < 1e-6);
  }

  // Horizon Festival gather: physics-only .fiz PVS entries are classified separately
  // rather than reported as unresolved render props, and reflective festival arches
  // carry their authored environment reflection strength.
  {
    world::GatherOptions fest_opt;
    fest_opt.render_data = true;
    fest_opt.radius = 600.0;
    fest_opt.prop_radius = 600.0;
    fest_opt.prop_near_radius = 120.0;
    fest_opt.route = 0;
    world::Mesh fest_mesh;
    world::GatherStats fest_st;
    CHECK(w.gather(-1027.3, -145.6, fest_opt, fest_mesh, &fest_st, &error));
    size_t refl_arch_mats = 0;
    for (const auto& mm : fest_mesh.materials) {
      if (mm.effect == "H_DIFF_SPEC_REFL_AO_4_LM" && mm.decal_noise_c4[1] < -0.1f) ++refl_arch_mats;
    }
    // Large festival buildings (>= 25 m) are promoted to fade == 0, so no fade == 1/2
    // triangle exceeds the 40 m prop reach threshold.
    float max_fade_lod_reach = 0.0f;
    for (size_t t = 0; t < fest_mesh.triangles(); ++t) {
      const uint8_t f = fest_mesh.materials[fest_mesh.material[t]].fade;
      if ((f != 1 && f != 2) || t >= fest_mesh.instance.size()) continue;
      for (int k = 0; k < 3; ++k) {
        const auto& p = fest_mesh.positions[t * 3 + size_t(k)];
        const auto& inst = fest_mesh.instance[t];
        const float dx = p[0] - inst[0], dy = p[1] - inst[1], dz = p[2] - inst[2];
        max_fade_lod_reach = std::max(max_fade_lod_reach, std::sqrt(dx * dx + dy * dy + dz * dz));
      }
    }
    std::printf("festival gather: %zu props placed, %zu physics-only (.fiz), %zu unresolved, %zu reflective arch materials, "
                "max fade1/2 reach %.1f m\n",
                fest_st.props, fest_st.props_physics_only, fest_st.prop_unresolved, refl_arch_mats, max_fade_lod_reach);
    CHECK(fest_st.props > 3500);
    CHECK(fest_st.props_physics_only == 327);
    CHECK(fest_st.prop_unresolved == 0);
    CHECK(refl_arch_mats > 0);
    CHECK(max_fade_lod_reach <= 40.0f);
  }

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("world_tests: all passed\n");
  return 0;
}
