#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "drive/mesh_ground.h"
#include "drive/vehicle.h"
#include "world/collision.h"
#include "world/smashables.h"
#include "world/stream.h"
#include "world/vfx.h"
#include "world/world.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* msg, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL (line %d): %s\n", line, msg);
    ++g_failures;
  }
}

#define CHECK(c) check((c), #c, __LINE__)

void test_break_rules_and_pool() {
  world::PropPhysicsDef flyer{};
  flyer.object_type_index = 1;
  flyer.break_speed_mph = 0.0f;
  flyer.mass = 0.20f;
  flyer.children.resize(12);

  world::PropPhysicsDef mailbox{};
  mailbox.object_type_index = 2;
  mailbox.break_speed_mph = 10.0f;  // 4.4704 m/s
  mailbox.mass = 0.15f;

  world::PropPhysicsDef arch{};
  arch.object_type_index = 3;
  arch.break_speed_mph = 9999.0f;
  arch.mass = 50.0f;

  CHECK(world::SmashableWorld::should_break(flyer, 1.0f));
  CHECK(!world::SmashableWorld::should_break(mailbox, 4.0f));  // 8.95 mph < 10 mph
  CHECK(world::SmashableWorld::should_break(mailbox, 5.0f));   // 11.18 mph > 10 mph
  CHECK(!world::SmashableWorld::should_break(arch, 100.0f));   // unbreakable sentinel

  // Verify 384-body pool eviction (40 flyers * 12 children = 480 > 384).
  world::PhysicsDefinitions defs;
  defs.definitions.push_back(flyer);
  world::SmashableWorld sw;
  sw.set_definitions(defs);
  for (int i = 0; i < 40; ++i) {
    const int n = sw.spawn_flyer_shatter({float(i) * 5.0f, 0.5f, 0.0f}, {15.0f, 0.0f, 0.0f});
    CHECK(n == 12);
  }
  CHECK(sw.active_dynamic_count() == world::SmashableWorld::kMaxDynamicBodies);
  CHECK(sw.total_evictions() == 40 * 12 - world::SmashableWorld::kMaxDynamicBodies);

  std::vector<world::DebrisVertex> mesh;
  sw.build_debris_mesh(mesh);
  CHECK(mesh.size() == world::SmashableWorld::kMaxDynamicBodies * 36);

  // Pillar 4: smashing a placed prop records broken_hidden_positions for static world hiding
  // and clears on reset_broken().
  {
    world::SmashableWorld sw2;
    world::PropPhysicsDef fence{};
    fence.object_type_index = 0;
    fence.break_speed_mph = 5.0f;
    fence.mass = 0.25f;
    fence.bounding_radius = 1.2f;
    world::CollObjPlacement pl{};
    pl.physics_type = "CO_Fence.0.rmb";
    pl.stem = "co_fence";
    pl.pos = {0.0f, 0.0f, 5.0f};
    sw2.add_prop(pl, &fence);
    drive::FlatGround g(0.0);
    drive::CarParams cp{};
    drive::Vehicle v(cp);
    v.launch({0.0, 0.0, 2.5}, 0.0, 15.0);
    std::vector<world::SmashEvent> evs;
    double speed_before = 0.0, speed_after = 0.0, ang_after = 0.0;
    for (int i = 0; i < 40 && evs.empty(); ++i) {
      speed_before = v.speed();
      v.step({0.0, 0.5, 0.0, false}, g);
      sw2.step(v, g, float(drive::Vehicle::kStep), evs);
      if (!evs.empty()) {
        speed_after = v.speed();
        const auto a = v.angular_velocity();
        ang_after = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
      }
    }
    CHECK(!evs.empty());
    // Soft FH1-style fence smash-through: brief light velocity damping (< 5% loss), no dramatic spin/launch.
    CHECK(speed_after > speed_before * 0.95);
    CHECK(ang_after < 0.25);
    CHECK(sw2.broken_prop_count() == 1);
    const auto hp = sw2.broken_hidden_positions();
    CHECK(hp && hp->size() == 1 && (*hp)[0].radius >= 1.5f);
    sw2.reset_broken();
    CHECK(sw2.broken_prop_count() == 0);
  }
}

void test_barn_door_swap() {
  world::SmashableWorld sw;
  world::BarnDoorCollider door;
  door.barn_name = "barnfind_02";
  door.closed_coll_id = "bf_bug_eb110bf_92_closedc";
  door.open_coll_id = "bf_bug_eb110bf_92_openc";
  door.pos = {0.0f, 0.0f, 5.0f};
  door.is_open = false;
  sw.add_barn_door(door);

  drive::FlatGround ground(0.0);
  drive::CarParams params{};
  drive::Vehicle car(params);
  std::vector<world::SmashEvent> events;

  // Closed barn door blocks car trying to drive through z = 5.
  car.launch({0.0, 0.4, 3.0}, 0.0, 12.0);
  for (int i = 0; i < 60; ++i) {
    car.step({0.0, 0.4, 0.0, false}, ground);
    sw.step(car, ground, float(drive::Vehicle::kStep), events);
  }
  CHECK(car.position().z < 5.0);

  // Sync from hidden set (closed_coll_id hidden => discovered => open_coll_id active).
  std::set<std::string> hidden = {"bf_bug_eb110bf_92_closedc"};
  sw.sync_barns_from_hidden(hidden);
  CHECK(sw.barn_doors()[0].is_open);

  // Now car drives right through the open doorway past z = 5!
  car.launch({0.0, 0.4, 2.0}, 0.0, 14.0);
  for (int i = 0; i < 60; ++i) {
    car.step({0.0, 0.4, 0.0, false}, ground);
    sw.step(car, ground, float(drive::Vehicle::kStep), events);
  }
  CHECK(car.position().z > 6.0);
}

void test_corpus() {
  const char* env_root = std::getenv("SHIFT_GAME_ROOT");
  if (!env_root || !*env_root) return;
  const std::filesystem::path root(env_root);
  const auto media = root / "media";
  const auto colorado = media / "tracks" / "colorado";
  if (!std::filesystem::exists(colorado / "PhysicsDefinitions.bin")) return;

  world::SmashableWorld sw;
  std::string err;
  CHECK(sw.load(media, colorado, &err));
  const auto c = sw.definitions().counts();
  std::printf("corpus smashables: defs=%zu max_global=%zu children=%zu (parents=%zu max=%zu) shapes=(%zu S, %zu OBB, %zu H) coll_types=%zu placed=%zu\n",
              c.top_level, c.max_global_index, c.total_children, c.parents_with_children, c.max_children,
              c.spheres, c.obbs, c.hulls, sw.coll_object_types().size(), sw.placed_prop_count());

  CHECK(c.top_level == 84);
  CHECK(c.max_global_index == 189);
  CHECK(c.total_children == 371);  // 84 top-level + 371 recursive children = 455 total defs
  CHECK(c.parents_with_children == 64);
  CHECK(c.max_children == 16);
  CHECK(c.spheres == 827);
  CHECK(c.obbs == 394);
  CHECK(c.hulls == 7);
  CHECK(c.by_object_type.at(0) == 15);
  CHECK(c.by_object_type.at(1) == 1);
  CHECK(c.by_object_type.at(2) == 2);
  CHECK(c.by_object_type.at(3) == 3);
  CHECK(c.by_object_type.at(-1) == 434);

  const auto* flyer = sw.definitions().find_by_object_type(1);
  CHECK(flyer != nullptr && flyer->unique_index == 346 && flyer->children.size() == 12 &&
        std::fabs(flyer->break_speed_mph) < 1e-4f);

  // Verify UI track 4-byte PhysicsDefinitions.bin parses cleanly with 0 definitions.
  const auto ui_pd = media / "tracks" / "uiautoshow" / "PhysicsDefinitions.bin";
  if (std::filesystem::exists(ui_pd)) {
    world::PhysicsDefinitions ui_defs;
    CHECK(world::load_physics_definitions_file(ui_pd, ui_defs, &err));
    CHECK(ui_defs.definitions.empty());
  }

  // Verify CollObjects.xml (31 types: 3 triggers, 28 physical) and CollObjs.xml (21,877 XML rows, deduplicated parents).
  CHECK(sw.coll_object_types().size() == 31);
  int triggers = 0;
  for (const auto& t : sw.coll_object_types())
    if (t.is_trigger) ++triggers;
  CHECK(triggers == 3);
  CHECK(sw.placed_prop_count() == 21877);
  CHECK(sw.unique_prop_count() >= 5000 && sw.unique_prop_count() < 12000);
  CHECK(sw.loaded_debris_model_count() >= 640);
  CHECK(sw.matched_global_index_count() >= 4500);
  std::printf("corpus 1:1 debris: unique_props=%zu matched_global=%zu rmb_models=%zu\n",
              sw.unique_prop_count(), sw.matched_global_index_count(), sw.loaded_debris_model_count());

  // Verify Flyer shatter uses real FH1 textured .rmb.bin shards (not 12 * 36 untextured boxes).
  {
    const int n = sw.spawn_flyer_shatter({1999.0f, 145.0f, 963.0f}, {20.0f, 0.0f, 0.0f});
    CHECK(n == 12);
    std::vector<world::DebrisVertex> mesh;
    sw.build_debris_mesh(mesh);
    CHECK(!mesh.empty() && (mesh.size() % 3) == 0);
    size_t textured_verts = 0;
    uint32_t sample_tex = world::kNoTexture;
    for (const auto& dv : mesh) {
      if (dv.texture_record != world::kNoTexture) {
        ++textured_verts;
        sample_tex = dv.texture_record;
      }
    }
    CHECK(textured_verts == mesh.size());
    CHECK(sample_tex != world::kNoTexture);
    fh1::WorldTexture wt;
    CHECK(sw.read_texture(sample_tex, wt));
    CHECK(!wt.blocks.levels.empty());
    std::printf("corpus flyer real .rmb.bin shatter: %zu verts (tex_rec=%u, %ux%u)\n",
                mesh.size(), sample_tex, wt.blocks.levels[0].width, wt.blocks.levels[0].height);
    sw.clear_debris();
  }

  // Verify flyer_053 .fiz coverage fix: Collision::gather() returns ground at (6497.95, -227.24), y ≈ 91.70, surf 5.
  world::Collision col;
  std::string warn;
  CHECK(col.open(colorado, media / "physics.zip", &err, &warn));
  world::CollisionMesh cm;
  CHECK(col.gather(6497.95, -227.24, 150.0, cm, &err));
  drive::MeshGround g53(cm.positions, {}, cm.surface);
  double y = 0.0;
  drive::Hit hit;
  CHECK(world::ground_height(g53, 6497.95, -227.24, 95.0, y, &hit));
  CHECK(std::fabs(y - 91.70) < 0.25);
  CHECK(hit.surface == 5);
  std::printf("flyer_053 ground fix: y=%.2f m surface=%d (%s)\n", y, hit.surface, col.surface_name(hit.surface).c_str());

  // Verify Bugatti barn (1418.53, -5366.79) .fiz hit + east edge (1448.0, -5363.0) Streamer fallback hit!
  CHECK(col.gather(1418.53, -5366.79, 150.0, cm, &err));
  drive::MeshGround gbug(cm.positions, {}, cm.surface);
  CHECK(world::ground_height(gbug, 1418.53, -5366.79, 202.0, y, &hit));
  CHECK(std::fabs(y - 198.77) < 0.25);
  // Raw .fiz stops west of 1448.0, while Streamer's visual-terrain fallback covers (1448.0, -5363.0) cleanly!
  world::World w;
  const auto cache = std::filesystem::temp_directory_path() / "shift_smashables_world_cache.bin";
  if (w.open(colorado, cache, &err)) {
    world::GatherOptions opts;
    opts.radius = 250;
    opts.prop_radius = 100;
    world::Streamer streamer(w, opts, 100.0, &col, 150.0);
    bool changed = false;
    CHECK(streamer.update(1435.0, -5365.0, &changed, &err));
    double fb_y = 0.0;
    drive::Hit fb_hit;
    CHECK(world::ground_height(streamer.ground(), 1448.0, -5363.0, 210.0, fb_y, &fb_hit));
    std::printf("Bugatti barn east edge fallback: y=%.2f m surface=%d\n", fb_y, fb_hit.surface);
  }
}

void test_smashables_rewind_restore() {
  world::SmashableWorld sw;
  world::PropPhysicsDef def;
  def.mass = 0.08f;
  def.break_speed_mph = 5.0f;
  def.bounding_radius = 1.0f;
  def.object_type_index = 0;  // Fence
  world::CollObjPlacement p;
  p.physics_type = "CO_Fence.0.rmb";
  p.stem = "co_fence";
  p.pos = {0.0f, 0.5f, 10.0f};
  sw.add_prop(p, &def);

  const auto snap_intact = sw.capture_rewind();
  CHECK(snap_intact.broken_indices.empty());

  // Smash the prop with a moving vehicle.
  drive::CarParams cp{};
  drive::Vehicle veh(cp);
  veh.launch({0.0, 0.0, 7.5}, 0.0, 15.0);
  drive::FlatGround fg(0.0);
  std::vector<world::SmashEvent> evs;
  for (int i = 0; i < 40 && evs.empty(); ++i) {
    veh.step({0.0, 0.5, 0.0, false}, fg);
    sw.step(veh, fg, float(drive::Vehicle::kStep), evs);
  }
  CHECK(sw.broken_prop_count() == 1);

  // Rewind restores the smashed prop and returns its HiddenSphere so Showroom un-hides it.
  const auto unhidden = sw.restore_rewind(snap_intact);
  CHECK(sw.broken_prop_count() == 0);
  CHECK(sw.active_dynamic_count() == 0);
  CHECK(unhidden.size() == 1);
}

void test_solid_festival_barriers_and_ground_height() {
  // PT-16: model classification separates solid festival crowd barriers from wooden fences & decorative props.
  CHECK(world::SmashableWorld::is_solid_barrier_model("OBJ_FEST_CrowdBarrier_LOD00", {-1.3f, 0.0f, -0.15f},
                                                      {1.3f, 1.15f, 0.15f}));
  CHECK(world::SmashableWorld::is_solid_barrier_model("ChainLink_Fence_2_66m_LOD00", {-1.33f, 0.0f, -0.05f},
                                                      {1.33f, 1.85f, 0.05f}));
  CHECK(!world::SmashableWorld::is_solid_barrier_model("CO_WoodenBarrier_001_LOD00", {-1.3f, 0.0f, -0.15f},
                                                       {1.3f, 1.15f, 0.15f}));
  CHECK(!world::SmashableWorld::is_solid_barrier_model("OBJ_FEST_FlagTall_LOD00", {-0.6f, 0.0f, -0.1f},
                                                       {0.6f, 5.5f, 0.1f}));
  CHECK(!world::SmashableWorld::is_solid_barrier_model("Temp_Arrow_Pointer_LOD00", {-1.0f, 0.0f, -1.5f},
                                                       {1.0f, 0.85f, 1.5f}));

  // PT-16: solid festival crowd barrier stops the car smoothly along the barrier without launching it vertically.
  world::SmashableWorld sw;
  world::SolidBarrier bar;
  bar.pos = {0.0f, 0.0f, 8.0f};
  bar.local_center = {0.0f, 0.6f, 0.0f};
  bar.half_extents = {2.6f, 0.6f, 0.22f};
  bar.routes = {0};  // free-roam barrier
  sw.add_solid_barrier(bar);
  CHECK(sw.solid_barrier_count() == 1);

  drive::FlatGround ground(0.0);
  drive::CarParams cp{};
  drive::Vehicle car(cp);
  car.launch({0.0, 0.4, 3.0}, 0.0, 14.0);
  std::vector<world::SmashEvent> evs;
  double max_y = 0.0;
  double max_vy = 0.0;
  for (int i = 0; i < 80; ++i) {
    car.step({0.0, 0.4, 0.0, false}, ground);
    sw.step(car, ground, float(drive::Vehicle::kStep), evs);
    CHECK(evs.empty());  // solid barrier must NOT shatter
    max_y = std::max(max_y, car.position().y);
    max_vy = std::max(max_vy, car.velocity().y);
  }
  CHECK(car.position().z < 7.8);  // stopped in front of z = 8 barrier
  CHECK(max_y < 1.5);             // never launched into the air
  CHECK(max_vy < 1.0);            // zero upward launch velocity

  // Route-gated barrier must NOT block an unrelated event route.
  sw.set_active_route(16);
  car.launch({0.0, 0.4, 3.0}, 0.0, 14.0);
  for (int i = 0; i < 80; ++i) {
    car.step({0.0, 0.4, 0.0, false}, ground);
    sw.step(car, ground, float(drive::Vehicle::kStep), evs);
  }
  CHECK(car.position().z > 9.5);

  // PT-21: oriented flyer shatter + record_hidden_prop persists hidden sphere.
  const int shards = sw.spawn_flyer_shatter({10.0f, 1.0f, 20.0f}, {0.5f, 0.0f, 0.866f}, {0.0f, 1.0f, 0.0f},
                                            {-0.866f, 0.0f, 0.5f}, {12.0f, 0.0f, 0.0f});
  CHECK(shards == 12);
  sw.record_hidden_prop({10.0f, 1.0f, 20.0f}, 3.5f);
  CHECK(sw.broken_prop_count() == 1);

  // PT-22: world::ground_height skips steep vertical wall planes sitting over flat road
  // and recovers cleanly across a hairline triangle seam.
  {
    // Two road triangles at y = 10.0 with a tiny 2 cm seam along x = 0, plus a steep wall
    // triangle leaning over x = 0 at y = 12.1 (normal.y ~ 0.05).
    std::vector<std::array<float, 3>> tris = {
        // Steep wall triangle strictly spanning x in [-0.05, 0.05] over (0, 0) (normal.y ~ 0.04)
        {-0.05f, 10.0f, -5.0f}, {-0.05f, 10.0f, 5.0f}, {0.05f, 12.5f, 0.0f},
        // Road left of hairline seam (x in [-5, -0.01])
        {-5.0f, 10.0f, -5.0f}, {-0.01f, 10.0f, 5.0f}, {-0.01f, 10.0f, -5.0f},
        // Road right of hairline seam (x in [0.01, 5])
        {0.01f, 10.0f, -5.0f}, {0.01f, 10.0f, 5.0f}, {5.0f, 10.0f, -5.0f},
    };
    drive::MeshGround mg(tris);
    double gy = 0.0;
    drive::Hit h{};
    CHECK(world::ground_height(mg, 0.0, 0.0, 12.2, gy, &h));
    CHECK(std::fabs(gy - 10.0) < 0.05);
    CHECK(h.normal.y > 0.9);
  }
}

void test_road_sign_debris_and_barn_door_no_launch() {
  // PT-29: Smashing a roadside sign launches tumbling debris laterally outward away from
  // the car centerline, applies air drag & gravity so it never floats in the chase camera
  // corridor, and bounces/settles onto the ground naturally.
  {
    world::SmashableWorld sw;
    world::PropPhysicsDef sign{};
    sign.object_type_index = 2;
    sign.break_speed_mph = 5.0f;
    sign.mass = 0.25f;
    sign.bounding_radius = 1.2f;
    world::CollObjPlacement pl{};
    pl.physics_type = "CO_RoadSign.0.rmb";
    pl.stem = "co_roadsign";
    pl.pos = {-0.6f, 0.0f, 8.0f};  // left side of car path
    sw.add_prop(pl, &sign);

    drive::FlatGround ground(0.0);
    drive::CarParams cp{};
    drive::Vehicle car(cp);
    car.launch({0.0, 0.0, 2.0}, 0.0, 24.0);

    std::vector<world::SmashEvent> evs;
    bool smashed = false;
    bool entered_chase_lens_corridor = false;
    float min_x = 0.0f;
    float final_y = 0.0f;
    float final_z = 0.0f;
    std::vector<world::DebrisVertex> mesh;

    for (int i = 0; i < 220; ++i) {
      car.step({0.0, 0.4, 0.0, false}, ground);
      evs.clear();
      sw.step(car, ground, float(drive::Vehicle::kStep), evs);
      if (!evs.empty()) smashed = true;
      if (smashed && sw.active_dynamic_count() > 0) {
        mesh.clear();
        sw.build_debris_mesh(mesh);
        if (!mesh.empty()) {
          double sx = 0.0, sy = 0.0, sz = 0.0;
          for (const auto& v : mesh) {
            sx += v.pos[0];
            sy += v.pos[1];
            sz += v.pos[2];
          }
          const float cx = float(sx / double(mesh.size()));
          const float cy = float(sy / double(mesh.size()));
          const float cz = float(sz / double(mesh.size()));
          min_x = std::min(min_x, cx);
          final_y = cy;
          final_z = cz;
          const float dz = cz - float(car.position().z);
          const float dx = std::fabs(cx - float(car.position().x));
          if (dz < -1.5f && dz > -7.5f && dx < 1.1f && cy > 0.6f && cy < 3.2f) {
            entered_chase_lens_corridor = true;
          }
        }
      }
    }
    CHECK(smashed);
    CHECK(sw.active_dynamic_count() >= 1);
    CHECK(min_x < -2.0f);                           // kicked cleanly outward to the left roadside
    CHECK(!entered_chase_lens_corridor);            // never floats in front of chase camera lens
    CHECK(final_y < 1.6f);                          // pulled down by gravity toward ground
    CHECK(final_z < float(car.position().z) - 8.0f);  // air drag drops debris well behind car
  }

  // PT-27: Hitting a closed barn door deflects strictly horizontally with zero upward launch.
  {
    world::SmashableWorld sw;
    world::BarnDoorCollider door;
    door.barn_name = "barnfind_pt27";
    door.pos = {0.0f, 0.0f, 9.0f};
    door.is_open = false;
    sw.add_barn_door(door);

    drive::FlatGround ground(0.0);
    drive::CarParams cp{};
    drive::Vehicle car(cp);
    car.launch({0.0, 0.0, 3.0}, 0.0, 22.0);
    std::vector<world::SmashEvent> evs;
    double max_y = 0.0, max_vy = 0.0;
    for (int i = 0; i < 80; ++i) {
      car.step({0.0, 0.2, 0.0, false}, ground);
      sw.step(car, ground, float(drive::Vehicle::kStep), evs);
      max_y = std::max(max_y, car.position().y);
      max_vy = std::max(max_vy, car.velocity().y);
    }
    CHECK(max_y < 1.2);
    CHECK(max_vy < 0.8);
  }
}

// R3-PHY-13: Periodic distance-based (> 300 m) respawn of non-Discount-Sign smashables
// in Free Roam, Discount Sign preservation across respawn and reset_broken(), and
// SkidMarkSystem::truncate_after(time_s) erasing post-rewind skidmarks cleanly.
void test_smashable_respawn_and_skidmark_truncate() {
  {
    world::SmashableWorld sw;
    world::PropPhysicsDef fence{};
    fence.object_type_index = 0;  // Fence (respawns)
    fence.break_speed_mph = 2.0f;
    fence.mass = 0.2f;
    fence.bounding_radius = 1.2f;

    world::PropPhysicsDef sign{};
    sign.object_type_index = 1;  // Flyer / Discount Sign (must NOT respawn)
    sign.break_speed_mph = 0.0f;
    sign.mass = 0.2f;
    sign.bounding_radius = 1.2f;

    world::CollObjPlacement pf{};
    pf.physics_type = "CO_FenceRespawn.0.rmb";
    pf.stem = "co_fencerespawn";
    pf.pos = {0.0f, 0.0f, 6.0f};
    sw.add_prop(pf, &fence);

    world::CollObjPlacement ps{};
    ps.physics_type = "CO_FlyerSign.0.rmb";
    ps.stem = "co_flyersign";
    ps.pos = {0.0f, 0.0f, 12.0f};
    sw.add_prop(ps, &sign);

    // Also record an external Discount Sign hide sphere (FreeRoam collectible path).
    sw.record_hidden_prop({50.0f, 0.0f, 50.0f}, 3.5f);
    CHECK(sw.broken_prop_count() == 1);

    drive::FlatGround ground(0.0);
    drive::CarParams cp{};
    drive::Vehicle car(cp);
    car.launch({0.0, 0.0, 2.0}, 0.0, 20.0);
    std::vector<world::SmashEvent> evs;
    for (int i = 0; i < 80; ++i) {
      car.step({0.0, 0.4, 0.0, false}, ground);
      sw.step(car, ground, float(drive::Vehicle::kStep), evs);
    }
    // Both the fence, the Flyer prop, and the external Discount Sign are now broken/hidden (3 total).
    CHECK(sw.broken_prop_count() == 3);

    // Stay within 150 m (< 300 m) for > 1.2 s: fence must NOT respawn yet.
    car.reset({0.0, 0.0, 150.0}, 0.0);
    for (int i = 0; i < 30; ++i) sw.step(car, ground, 0.05f, evs);
    CHECK(sw.broken_prop_count() == 3);
    CHECK(sw.consume_respawned_hidden().empty());

    // Move > 350 m away and step > 1.1 s: fence respawns, BOTH Discount Signs stay broken!
    car.reset({0.0, 0.0, 360.0}, 0.0);
    for (int i = 0; i < 25; ++i) sw.step(car, ground, 0.05f, evs);
    CHECK(sw.broken_prop_count() == 2);
    const auto resp = sw.consume_respawned_hidden();
    CHECK(resp.size() == 1 && std::fabs(resp[0].pos[2] - 6.0f) < 0.1f);

    // Smash the respawned fence again, then call reset_broken() (event start/restart):
    // fence resets immediately, while both Discount Signs remain preserved!
    car.launch({0.0, 0.0, 2.0}, 0.0, 20.0);
    for (int i = 0; i < 50; ++i) {
      car.step({0.0, 0.4, 0.0, false}, ground);
      sw.step(car, ground, float(drive::Vehicle::kStep), evs);
    }
    CHECK(sw.broken_prop_count() == 3);
    sw.reset_broken();
    CHECK(sw.broken_prop_count() == 2);
    CHECK(sw.consume_respawned_hidden().size() == 1);
  }

  // SkidMarkSystem timestamping & truncate_after(time_s) on rewind resume.
  {
    world::SkidMarkSystem skids;
    world::WheelVfxInput wv{};
    wv.grounded = true;
    wv.surface_normal = {0.0f, 1.0f, 0.0f};
    wv.wheel_right = {1.0f, 0.0f, 0.0f};
    wv.wheel_forward = {0.0f, 0.0f, 1.0f};
    wv.tyre_width_m = 0.25f;
    wv.slip_combined = 1.2f;
    wv.wheel_speed_mps = 15.0f;
    wv.surface.skid_a = 200;
    wv.surface.skid_texture_index = 0;

    // Seed wheel strip at t = 0.5 s, then emit 10 segments from t = 1.0 s to t = 10.0 s.
    wv.contact_pos = {0.0f, 0.0f, 0.0f};
    skids.set_time(0.5f);
    skids.update_wheel(0, wv);
    for (int i = 1; i <= 10; ++i) {
      wv.contact_pos = {0.0f, 0.0f, float(i) * 1.0f};
      skids.set_time(float(i));
      skids.update_wheel(0, wv);
    }
    CHECK(skids.segment_count() == 10);

    // Rewind resume to t = 4.0 s: post-rewind segments (t = 5..10) are cleanly erased!
    skids.truncate_after(4.0f);
    CHECK(skids.segment_count() == 4);
    std::vector<world::SkidVertex> asph, ruts;
    skids.build_geometry(asph, ruts);
    CHECK(asph.size() == 4 * 6);
    CHECK(ruts.empty());

    // Subsequent skidmark at t = 4.5 s starts a fresh strip cleanly after break_all().
    wv.contact_pos = {0.0f, 0.0f, 4.0f};
    skids.update_wheel(0, wv, 4.2f);
    wv.contact_pos = {0.0f, 0.0f, 5.0f};
    skids.update_wheel(0, wv, 4.5f);
    CHECK(skids.segment_count() == 5);
  }
}

}  // namespace

int main() {
  test_break_rules_and_pool();
  test_barn_door_swap();
  test_smashables_rewind_restore();
  test_solid_festival_barriers_and_ground_height();
  test_road_sign_debris_and_barn_door_no_launch();
  test_smashable_respawn_and_skidmark_truncate();
  test_corpus();
  if (g_failures == 0) {
    std::printf("smashables_tests: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "smashables_tests: %d failure(s)\n", g_failures);
  return 1;
}
