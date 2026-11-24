// Colorado's collision over the player's own files (no game data in the
// repository): the census COLLISION.md's Tests section gives, the surface
// table, and the ground under the measured spawn. Skipped (exit 77) unless
// SHIFT_GAME_ROOT names the disc.
// Spec: COLLISION.md (horizon1-recomp docs/formats, 43346e1d), Tests.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_set>

#include "drive/mesh_ground.h"
#include "world/collision.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

struct KeyHash {
  size_t operator()(const std::string& s) const { return std::hash<std::string>()(s); }
};

void test_surface_physics_extras_parser() {
  const char* kXml = R"xml(
<SurfaceTypesData>
  <SurfaceTypesList>
    <SurfaceType value="Asphalt"/>
    <SurfaceType value="DeepMud"/>
    <SurfaceType value="Guardrail"/>
    <SurfaceType value="UndefinedKerb"/>
  </SurfaceTypesList>
  <CarWorldCollFricMods>
    <Guardrail value="0.25"/>
    <SoftWorld FrictionScale="1.6"/>
  </CarWorldCollFricMods>
  <CategoryToCategoryParams>
    <Pair CategoryA="Car" CategoryB="HardWorld" Friction="0.18" Restitution="0.05"/>
    <Car>
      <SoftWorld Friction="0.65" Restitution="0.02"/>
    </Car>
  </CategoryToCategoryParams>
  <SurfaceTypes>
    <Asphalt Category="HardWorld">
      <MaxSteeringLockCap value="90.0"/>
      <LinearDrag value="0.0"/>
      <AngularDrag value="0.0"/>
      <ChaseCamSpringMultiplier value="1.0"/>
      <SteerTorque value="1.0"/>
    </Asphalt>
    <DeepMud Category="SoftWorld">
      <MaxSteeringLockCap value="25.0"/>
      <LinearDrag value="1.8"/>
      <AngularDrag value="1.2"/>
      <ChaseCamSpringMultiplier value="0.65"/>
      <SteerTorque value="0.55"/>
      <Springiness>
        <IsSpringy value="true"/>
        <SpringK value="45.0"/>
        <NormDepSpringNormYCutoff value="0.80"/>
        <NormDepSpringSpeedCutoff value="6.0"/>
      </Springiness>
    </DeepMud>
    <Guardrail Category="Metal">
      <MaxSteeringLockCap value="0.0"/>
    </Guardrail>
  </SurfaceTypes>
</SurfaceTypesData>
)xml";
  world::SurfaceTableExtras ex;
  std::string err;
  CHECK(world::parse_surface_physics_extras(kXml, ex, &err));
  CHECK(ex.surfaces.size() == 4);
  CHECK(ex.surfaces[0].defined && std::fabs(ex.surfaces[0].max_steering_lock_cap_deg - 90.0f) < 1e-4f);
  // Retail stores MaxSteeringLockCap in degrees (25.0 deg), not normalized [0.35, 1.0].
  CHECK(ex.surfaces[1].defined && std::fabs(ex.surfaces[1].max_steering_lock_cap_deg - 25.0f) < 1e-4f);
  CHECK(std::fabs(ex.surfaces[1].linear_drag - 1.8f) < 1e-4f);
  CHECK(std::fabs(ex.surfaces[1].angular_drag - 1.2f) < 1e-4f);
  CHECK(std::fabs(ex.surfaces[1].chase_cam_spring_mult - 0.65f) < 1e-4f);
  CHECK(std::fabs(ex.surfaces[1].steer_torque - 0.55f) < 1e-4f);
  CHECK(std::fabs(ex.surfaces[1].car_world_coll_fric_mod - 1.6f) < 1e-4f);
  CHECK(std::fabs(ex.surfaces[1].spring_norm_y_cutoff - 0.80f) < 1e-4f);
  CHECK(std::fabs(ex.surfaces[1].spring_speed_cutoff - 6.0f) < 1e-4f);
  CHECK(std::fabs(ex.surfaces[2].car_world_coll_fric_mod - 0.25f) < 1e-4f);
  CHECK(!ex.surfaces[3].defined && std::fabs(ex.surfaces[3].max_steering_lock_cap_deg - 90.0f) < 1e-4f);

  const auto* p_hard = ex.find_category_params("Car", "HardWorld");
  const auto* p_hard_rev = ex.find_category_params("HardWorld", "Car");
  CHECK(p_hard != nullptr && p_hard == p_hard_rev);
  CHECK(p_hard && p_hard->has_friction && std::fabs(p_hard->friction - 0.18f) < 1e-4f);
  CHECK(p_hard && p_hard->has_restitution && std::fabs(p_hard->restitution - 0.05f) < 1e-4f);
  const auto* p_soft = ex.find_category_params("Car", "SoftWorld");
  CHECK(p_soft != nullptr && std::fabs(p_soft->friction - 0.65f) < 1e-4f);
}

}  // namespace

int main() {
  test_surface_physics_extras_parser();
  if (failures) {
    std::printf("%d failure(s) in synthetic surface tests\n", failures);
    return 1;
  }
  const char* root = std::getenv("SHIFT_GAME_ROOT");
  if (!root) {
    std::printf("collision_tests: SHIFT_GAME_ROOT not set, skipped\n");
    return 77;
  }
  namespace fs = std::filesystem;
  const fs::path media = fs::path(root) / "media";
  world::Collision c;
  std::string error, warning;
  if (!c.open(media / "tracks" / "colorado", media / "physics.zip", &error, &warning)) {
    std::printf("FAIL open: %s\n", error.c_str());
    return 1;
  }
  std::printf("%s", warning.c_str());

  // The surface table: 68 entries, 0 Asphalt, 2 Grass; Kerb (58) has no
  // definition.
  const auto& surfaces = c.surfaces();
  std::printf("surfaces %zu: 0 %s (%s, friction %.3g), 2 %s (%.3g), 9 %s, 49 %s\n", surfaces.size(),
              c.surface_name(0).c_str(), surfaces.empty() ? "" : surfaces[0].category.c_str(), c.grip(0),
              c.surface_name(2).c_str(), c.grip(2), c.surface_name(9).c_str(), c.surface_name(49).c_str());
  CHECK(surfaces.size() == 68);
  CHECK(c.surface_name(0) == "Asphalt" && c.surface_name(2) == "Grass" && c.surface_name(9) == "Invisible");
  CHECK(c.surface_name(49) == "CameraCollision" && c.surface_name(58) == "Kerb");
  CHECK(surfaces.size() > 58 && !surfaces[58].defined);
  CHECK(!surfaces.empty() && surfaces[0].category == "HardWorld");

  // The .col index: a 133 x 100 lattice, 3,201 records, contiguous.
  const auto& index = c.index();
  std::printf("lattice %u x %u at (%.3f, %.3f), %zu records\n", index.width, index.height, index.x0, index.z0,
              index.records.size());
  CHECK(index.width == 133 && index.height == 100 && index.records.size() == 3201);

  // Every tile: the census.
  size_t vertices = 0, triangles = 0, core = 0, bad_normals = 0, degenerate = 0, unlisted = 0, bad_box = 0;
  size_t rank_mismatch = 0, size_mismatch = 0, non_contiguous = 0;
  std::map<int, size_t> by_surface, up_by_surface;
  std::unordered_set<std::string, KeyHash> distinct;
  std::set<uint32_t> cells;
  for (const auto& r : index.records) cells.insert(r.cell);
  uint32_t rank = 0;
  struct Word {
    size_t count = 0, steep = 0, up = 0;
    std::map<int, size_t> surfaces;
    std::set<uint32_t> tiles;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  };
  std::map<uint32_t, Word> words;
  std::map<int, size_t> steep_by_surface_8000;
  size_t masked_up_covered = 0, masked_up_holes = 0;
  size_t collide_all = 0, skip_all = 0, collide_core = 0, skip_core = 0, route0_only = 0, tiles_listing_route0 = 0;
  std::vector<std::array<float, 4>> hole_examples;
  for (size_t k = 0; k < index.records.size(); ++k) {
    const auto& r = index.records[k];
    // Tile numbers are the (j, i) rank of their cells.
    if (uint32_t(std::distance(cells.begin(), cells.find(r.cell))) != r.tile) ++rank_mismatch;
    if (k > 0 && r.stream_offset != index.records[k - 1].stream_offset + index.records[k - 1].block_size)
      ++non_contiguous;
    fh1::FizTile tile;
    if (!c.read_tile(r.tile, tile, &error)) {
      std::printf("FAIL tile %u: %s\n", r.tile, error.c_str());
      ++failures;
      continue;
    }
    ++rank;
    if (tile.block_a_size != r.block_size) ++size_mismatch;
    if (tile.grid_x != 12 || tile.grid_z != 12 || std::fabs(tile.box[2] - tile.box[0] - 116) > 0.01f ||
        std::fabs(tile.box[3] - tile.box[1] - 116) > 0.01f)
      ++bad_box;
    vertices += tile.vertices.size();
    triangles += tile.triangles.size();
    {
      // Masked up-facing triangles (word without 0x8000): is there an
      // always-on (0x8000) surface within 1 m under or over the centroid?
      std::vector<std::array<float, 3>> always;
      for (const auto& tri : tile.triangles)
        if (tri.word & 0x8000)
          for (auto v : tri.v) always.push_back(tile.vertices[v]);
      drive::MeshGround g(always);
      for (const auto& tri : tile.triangles) {
        if ((tri.word & 0x8000) || tri.normal[1] <= 0.7f) continue;
        const auto& a = tile.vertices[tri.v[0]];
        const auto& b = tile.vertices[tri.v[1]];
        const auto& cc = tile.vertices[tri.v[2]];
        const drive::Vec3 m{(double(a[0]) + b[0] + cc[0]) / 3, (double(a[1]) + b[1] + cc[1]) / 3 + 1,
                            (double(a[2]) + b[2] + cc[2]) / 3};
        drive::Hit h;
        if (g.raycast(m, {0, -1, 0}, 2, h)) ++masked_up_covered;
        else {
          ++masked_up_holes;
          if (hole_examples.size() < 12) hole_examples.push_back({float(m.x), float(m.y - 1), float(m.z), float(tri.word)});
        }
      }
    }
    // Route groups with route 0 active (free roam): the tile's mask.
    uint16_t free_roam_mask = 0x8000;
    for (size_t e = 0; e < tile.routes.size(); ++e)
      if (tile.routes[e] == 0) free_roam_mask |= uint16_t(1u << e);
    if (free_roam_mask != 0x8000) ++tiles_listing_route0;
    std::vector<char> listed(tile.triangles.size(), 0);
    for (const auto& cell : tile.grid)
      for (auto t : cell) listed[t] = 1;
    for (size_t t = 0; t < tile.triangles.size(); ++t) {
      const auto& tri = tile.triangles[t];
      if (!listed[t]) ++unlisted;
      const auto& a = tile.vertices[tri.v[0]];
      const auto& b = tile.vertices[tri.v[1]];
      const auto& cc = tile.vertices[tri.v[2]];
      const double e1[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]};
      const double e2[3] = {double(cc[0]) - a[0], double(cc[1]) - a[1], double(cc[2]) - a[2]};
      const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                           e1[0] * e2[1] - e1[1] * e2[0]};
      const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      // Degenerate: |(b - a) x (c - a)| under 1e-9 m^2 (two triangles, at
      // 4.7e-10 and 7.0e-10; the next is 1.9e-9). COLLISION.md counts two.
      if (len < 1e-9) {
        ++degenerate;
      } else {
        double err = 0;
        for (int i = 0; i < 3; ++i) err = std::max(err, std::fabs(n[i] / len - tri.normal[i]));
        if (err > 1e-3) ++bad_normals;
      }
      ++by_surface[tri.surface];
      const bool route_on = (tri.word & free_roam_mask) != 0;
      ++(route_on ? collide_all : skip_all);
      if ((tri.word & 0x8000) == 0 && route_on) ++route0_only;
      if (tri.normal[1] > 0.7f) ++up_by_surface[tri.surface];
      const double cx = (double(a[0]) + b[0] + cc[0]) / 3, cz = (double(a[2]) + b[2] + cc[2]) / 3;
      if (!c.in_cell(r, cx, cz)) continue;
      ++core;
      ++(route_on ? collide_core : skip_core);
      if (tri.word != 0x8000) {
        auto& w = words[tri.word];
        ++w.count;
        ++w.surfaces[tri.surface];
        if (std::fabs(tri.normal[1]) < 0.5f) ++w.steep;
        if (tri.normal[1] > 0.7f) ++w.up;
        w.tiles.insert(r.tile);
        for (const auto* p : {&a, &b, &cc})
          for (int i = 0; i < 3; ++i) w.lo[i] = std::min(w.lo[i], (*p)[i]), w.hi[i] = std::max(w.hi[i], (*p)[i]);
      } else if (std::fabs(tri.normal[1]) < 0.5f) {
        ++steep_by_surface_8000[tri.surface];
      }
      std::array<std::array<float, 3>, 3> key{a, b, cc};
      std::sort(key.begin(), key.end());
      distinct.insert(std::string(reinterpret_cast<const char*>(key.data()), sizeof key));
    }
  }
  // The u16 at triangle offset 2 (COLLISION.md: 0x8000 on most, not
  // decoded): the core triangles with another value.
  {
    size_t n = 0, steep = 0, up = 0;
    for (const auto& [v, w] : words) n += w.count, steep += w.steep, up += w.up;
    std::printf("word != 0x8000: %zu core triangles (%zu steep, %zu up), %zu values\n", n, steep, up, words.size());
    for (const auto& [v, w] : words) {
      std::printf("  word %u: %zu triangles, %zu steep, %zu up, %zu tiles, x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f; surfaces",
                  v, w.count, w.steep, w.up, w.tiles.size(), w.lo[0], w.hi[0], w.lo[1], w.hi[1], w.lo[2], w.hi[2]);
      for (const auto& [s, k] : w.surfaces) std::printf(" %d:%zu", s, k);
      std::printf("\n");
    }
    std::printf("masked up-facing triangles: %zu with a 0x8000 surface within 1 m, %zu without\n", masked_up_covered,
                masked_up_holes);
    for (const auto& e : hole_examples) std::printf("  hole at %.1f %.1f %.1f word %.0f\n", e[0], e[1], e[2], e[3]);
    std::printf("steep core triangles with 0x8000 by surface:");
    for (const auto& [s, k] : steep_by_surface_8000) std::printf(" %d:%zu", s, k);
    std::printf("\n");
  }
  {
    size_t n = 0, steep = 0;
    for (const auto& [v, w] : words) n += w.count, steep += w.steep;
    CHECK(n == 110702 && steep == 107802 && words.size() == 54);
    CHECK(masked_up_covered == 0 && masked_up_holes == 3340);
    // COLLISION.md, Route groups, free roam (route 0).
    std::printf("route 0: %zu tiles list it; collide %zu, skip %zu (core %zu, %zu); %zu only through route 0\n",
                tiles_listing_route0, collide_all, skip_all, collide_core, skip_core, route0_only);
    CHECK(tiles_listing_route0 == 7);
    CHECK(collide_all == 1851955 && skip_all == 155640);
    CHECK(collide_core == 1291222 && skip_core == 110640);
    CHECK(route0_only == 96);
  }
  std::printf("tiles %u, vertices %zu, triangles %zu, core %zu (%zu distinct), degenerate %zu\n", rank, vertices,
              triangles, core, distinct.size(), degenerate);
  std::printf("rank mismatches %zu, block size mismatches %zu, non-contiguous %zu, bad boxes %zu, unlisted %zu, "
              "bad normals %zu\n",
              rank_mismatch, size_mismatch, non_contiguous, bad_box, unlisted, bad_normals);
  CHECK(rank == 3201);
  CHECK(vertices == 1729941 && triangles == 2007595);
  CHECK(core == 1401862 && distinct.size() == 1399504);
  CHECK(degenerate == 2 && bad_normals == 0);
  CHECK(rank_mismatch == 0 && size_mismatch == 0 && non_contiguous == 0 && bad_box == 0 && unlisted == 0);
  // The surface table of COLLISION.md (a few rows) and 50 indices in use,
  // the largest 62.
  for (const auto& [s, n] : std::map<int, std::array<size_t, 2>>{{0, {507494, 333715}},
                                                                  {1, {675593, 647432}},
                                                                  {2, {351735, 343695}},
                                                                  {3, {145547, 293}},
                                                                  {9, {39572, 165}},
                                                                  {57, {3153, 3153}}}) {
    if (by_surface[s] != n[0] || up_by_surface[s] != n[1]) {
      std::printf("FAIL surface %d: %zu (%zu up), expected %zu (%zu)\n", s, by_surface[s], up_by_surface[s], n[0],
                  n[1]);
      ++failures;
    }
  }
  size_t in_use = 0;
  int largest = 0;
  for (const auto& [s, n] : by_surface)
    if (n) {
      ++in_use;
      largest = std::max(largest, s);
    }
  std::printf("surfaces in use %zu, largest %d\n", in_use, largest);
  CHECK(in_use == 50 && largest == 62);

  // The measured spawn: one up-facing Asphalt triangle 0.48 m under the
  // settled body reference point.
  world::CollisionMesh mesh;
  CHECK(c.gather(world::kSpawnX, world::kSpawnZ, 150, mesh, &error));
  drive::MeshGround ground(mesh.positions, {}, mesh.surface);
  double y = 0;
  drive::Hit hit;
  CHECK(world::ground_height(ground, world::kSpawnX, world::kSpawnZ, world::kSpawnY + 2, y, &hit));
  std::printf("spawn: %zu tiles, %zu triangles; ground %.3f m (%.3f under the body), surface %d, normal y %.3f\n",
              mesh.tiles, mesh.triangles(), y, world::kSpawnY - y, hit.surface, hit.normal.y);
  CHECK(std::fabs(world::kSpawnY - y - 0.48) < 0.03);
  CHECK(hit.surface == world::kSurfaceAsphalt && hit.normal.y > 0.7);

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("collision_tests: all passed\n");
  return 0;
}
