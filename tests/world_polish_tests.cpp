// Slice 4 Sub-milestone 3 unit & corpus tests:
// - TrackSettings.xml wind sanitization, grass shells, motion blur & DoF params
// - PostZoneSet (Bloom, ColorSettings, Fog, paired VignetteTop/Bottom) & PostZoneBlender
// - Spectators.zip (spectators.xml) & animatedobjects.zip catalog
// - AnimatedWorldSystem mesh generation (balloons, windmills, flags, crowds, grass)
// - Wheel angular velocity blur_rim blend curve
// - SettingsPage motion blur & depth of field toggles
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cars/bake.h"
#include "drive/vehicle.h"
#include "settings_page.h"
#include "world/stream.h"
#include "world/tod.h"
#include "world/world.h"
#include "world/world_anim.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

class FlatTestGround final : public drive::Ground {
 public:
  explicit FlatTestGround(int surface) : surface_(surface) {}
  bool raycast(const drive::Vec3& origin, const drive::Vec3& dir, double max_dist, drive::Hit& hit) const override {
    ++raycast_calls;
    if (dir.y >= -1e-6) return false;
    const double t = (144.8 - origin.y) / dir.y;
    if (t < 0.0 || t > max_dist) return false;
    hit.distance = t;
    hit.normal = {0.0, 1.0, 0.0};
    hit.surface = surface_;
    return true;
  }
  mutable size_t raycast_calls = 0;
 private:
  int surface_;
};

std::string read_text(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

int main() {
  // 1. TrackPostConfig wind sanitization & optical/grass XML parsing.
  {
    const std::string xml = R"(
      <TrackSettings>
        <WindDirection x="0.01" y="0.01" z="-431602080.0"/>
        <WindAmplitude value="0.22"/>
        <WindSpeed value="1.35"/>
        <GrassHeightScale value="1.25"/>
        <GrassLODDist value="28.0"/>
        <GrassLODFadeRange value="12.0"/>
        <GrassFadeStartDistance value="10.0"/>
        <GrassFadeEndDistance value="36.0"/>
        <motionBlurAmount value="0.8"/>
        <DetailBlurMPHForMaxBlur value="65.0"/>
        <dofFocalDistance value="14.0"/>
        <dofFocalLength value="38.0"/>
        <dofFocalMaxBlur value="0.65"/>
      </TrackSettings>
    )";
    world::TrackPostConfig cfg;
    CHECK(cfg.load_xml(xml));
    const float wlen = std::sqrt(cfg.wind_dir[0] * cfg.wind_dir[0] + cfg.wind_dir[1] * cfg.wind_dir[1] +
                                 cfg.wind_dir[2] * cfg.wind_dir[2]);
    CHECK(std::isfinite(cfg.wind_dir[0]) && std::isfinite(cfg.wind_dir[2]));
    CHECK(std::fabs(wlen - 1.0f) < 1e-3f);
    CHECK(std::fabs(cfg.wind_dir[2]) <= 1.01f);
    CHECK(std::fabs(cfg.wind_amplitude - 0.22f) < 1e-4f);
    CHECK(std::fabs(cfg.wind_speed - 1.35f) < 1e-4f);
    CHECK(std::fabs(cfg.grass_height_scale - 1.25f) < 1e-4f);
    CHECK(std::fabs(cfg.motion_blur_amount - 0.8f) < 1e-4f);
    CHECK(std::fabs(cfg.detail_blur_mph_max - 65.0f) < 1e-4f);
    CHECK(std::fabs(cfg.dof_focal_distance - 14.0f) < 1e-4f);
    CHECK(std::fabs(cfg.dof_focal_max_blur - 0.65f) < 1e-4f);

    // FEEL-31: blur strength is motionBlurAmount scaled by speed up to
    // DetailBlurMPHForMaxBlur, with no floor and no speed offset.
    const double mps_per_mph = 1.0 / 2.23693629;
    CHECK(std::fabs(cfg.motion_blur_strength(0.0)) < 1e-6f);
    CHECK(std::fabs(cfg.motion_blur_strength(6.5 * mps_per_mph) - 0.08f) < 1e-4f);
    CHECK(std::fabs(cfg.motion_blur_strength(32.5 * mps_per_mph) - 0.4f) < 1e-4f);
    CHECK(std::fabs(cfg.motion_blur_strength(65.0 * mps_per_mph) - 0.8f) < 1e-4f);
    CHECK(std::fabs(cfg.motion_blur_strength(120.0 * mps_per_mph) - 0.8f) < 1e-4f);
    world::TrackPostConfig roam;  // free-roam baseline: motionBlurAmount 0
    CHECK(roam.motion_blur_amount == 0.0f && roam.motion_blur_strength(60.0) == 0.0f);
    roam.motion_blur_amount = 3.0f;
    CHECK(std::fabs(roam.motion_blur_strength(60.0) - 0.95f) < 1e-6f);
  }

  // 2. PostZoneBlender smooth LerpTime transitions.
  {
    world::PostZoneSet zs;
    world::PostZone z0;
    z0.name = "Default";
    z0.lerp_time = 2.0f;
    z0.bloom_template = "Default";
    z0.color_settings = "Default";
    z0.vignette_top = "Default";
    z0.vignette_bottom = "DefaultBottom";
    z0.triangles = {{{-50.0f, 50.0f, 0.0f}, {-50.0f, -50.0f, 50.0f}}};
    world::PostZone z1;
    z1.name = "Canyon";
    z1.lerp_time = 2.0f;
    z1.bloom_template = "Warm";
    z1.color_settings = "Punchy";
    z1.vignette_top = "CanyonTop";
    z1.vignette_bottom = "CanyonBottom";
    z1.triangles = {{{80.0f, 120.0f, 100.0f}, {80.0f, 80.0f, 120.0f}}};
    zs.zones = {z0, z1};

    world::BloomTemplate b0;
    b0.scale = 1.0f;
    world::BloomTemplate b1;
    b1.scale = 2.0f;
    zs.bloom_templates = {{"default", b0}, {"warm", b1}};

    world::ColorSettingsTemplate c0;
    c0.contrast = 0.0f;
    world::ColorSettingsTemplate c1;
    c1.contrast = 0.25f;
    zs.color_templates = {{"default", c0}, {"punchy", c1}};

    world::VignetteTemplate vt0;
    vt0.color[3] = 0.2f;
    world::VignetteTemplate vt1;
    vt1.color[3] = 0.8f;
    world::VignetteTemplate vb0;
    vb0.color[3] = 0.3f;
    world::VignetteTemplate vb1;
    vb1.color[3] = 0.7f;
    zs.vignette_templates = {{"default", vt0}, {"canyontop", vt1}, {"defaultbottom", vb0}, {"canyonbottom", vb1}};

    world::TrackPostConfig cfg;
    world::PostZoneBlender blender;
    blender.update(zs, cfg, 0.0f, 0.0f, 0.016f);
    CHECK(std::fabs(blender.bloom.scale - 1.0f) < 1e-4f);
    CHECK(std::fabs(blender.color.contrast - 0.0f) < 1e-4f);
    CHECK(std::fabs(blender.vig_top.color[3] - 0.2f) < 1e-4f);

    // Move into Canyon zone: after 0.5 s of a 2.0 s LerpTime, should be smoothly partway!
    blender.update(zs, cfg, 100.0f, 100.0f, 0.5f);
    CHECK(blender.bloom.scale > 1.2f && blender.bloom.scale < 1.85f);
    CHECK(blender.color.contrast > 0.05f && blender.color.contrast < 0.22f);
    CHECK(blender.vig_top.color[3] > 0.3f && blender.vig_top.color[3] < 0.7f);

    // After another 6.0 s, converged to Canyon.
    blender.update(zs, cfg, 100.0f, 100.0f, 6.0f);
    CHECK(std::fabs(blender.bloom.scale - 2.0f) < 1e-2f);
    CHECK(std::fabs(blender.color.contrast - 0.25f) < 1e-2f);
    CHECK(std::fabs(blender.vig_top.color[3] - 0.8f) < 1e-2f);
  }

  // 3. Wheel blur factor curve.
  {
    CHECK(world::wheel_blur_factor(0.0f) == 0.0f);
    CHECK(world::wheel_blur_factor(15.0f) == 0.0f);
    CHECK(std::fabs(world::wheel_blur_factor(30.0f) - 0.5f) < 1e-5f);
    CHECK(world::wheel_blur_factor(45.0f) == 1.0f);
    CHECK(world::wheel_blur_factor(120.0f) == 1.0f);
    CHECK(world::wheel_blur_factor(-45.0f) == 1.0f);
  }

  // 4. AnimatedWorldSystem mesh generation, R3-REN-08 Colorado landmarks, animation motion,
  //    grass road/dirt filtering, and R3-REN-14 per-cell grass ground height caching.
  {
    world::AnimatedWorldSystem anim;
    // WORLD-14/15: Colorado has no engine-placed props at fixed positions.
    CHECK(anim.windmills().empty());
    CHECK(anim.flags().empty());
    CHECK(anim.aircraft_banners().empty());
    CHECK(anim.balloons().empty());
    CHECK(anim.crowds().empty());
    CHECK(anim.ferris_wheels().empty());
    CHECK(anim.stage_lights().empty());
    CHECK(anim.traffic_lights().empty());

    // Traffic light 30 s Green/Yellow/Red smooth emissive cycle check.
    {
      const auto g = world::evaluate_traffic_light_cycle(5.0f, 0.0f);
      CHECK(g.green > 0.99f && g.yellow < 0.01f && g.red < 0.01f);
      const auto gy = world::evaluate_traffic_light_cycle(12.875f, 0.0f);
      CHECK(gy.green > 0.2f && gy.yellow > 0.2f && std::fabs((gy.green + gy.yellow + gy.red) - 1.0f) < 1e-4f);
      const auto y = world::evaluate_traffic_light_cycle(14.5f, 0.0f);
      CHECK(y.yellow > 0.99f && y.green < 0.01f && y.red < 0.01f);
      const auto r = world::evaluate_traffic_light_cycle(22.0f, 0.0f);
      CHECK(r.red > 0.99f && r.yellow < 0.01f && r.green < 0.01f);
      // Negative time wraps cleanly into [0, 30).
      const auto neg = world::evaluate_traffic_light_cycle(-8.0f, 0.0f);
      CHECK(neg.red > 0.99f);
    }

    FlatTestGround grass_ground(2);   // Grass in surfaceTypes.xml
    FlatTestGround road_ground(0);    // Asphalt in surfaceTypes.xml
    std::vector<world::DebrisVertex> v0, v1, v_road;
    anim.build_mesh(2005.0, 146.5, 955.0, 0.0f, &grass_ground, v0, true);
    const size_t rays_after_first = grass_ground.raycast_calls;
    CHECK(rays_after_first > 0);
    CHECK(anim.cached_grass_ground_count() > 0);

    // Second build at the same position & Ground* must perform ZERO additional BVH raycasts!
    anim.build_mesh(2005.0, 146.5, 955.0, 1.5f, &grass_ground, v1, true);
    CHECK(grass_ground.raycast_calls == rays_after_first);

    // Switching Ground* auto-invalidates the cache and re-samples cleanly (asphalt skips grass).
    anim.build_mesh(2005.0, 146.5, 955.0, 0.0f, &road_ground, v_road, true);
    CHECK(road_ground.raycast_calls > 0);
    CHECK(v0.size() > 1000 && v0.size() % 3 == 0);
    CHECK(v0.size() > v_road.size());  // grass grows on grass surface, skipped on asphalt road

    // Verify vertices actually animate over time (balloons, windmills, flags, Ferris wheel, stage lights, aircraft, grass).
    bool moved = false;
    const size_t n = std::min(v0.size(), v1.size());
    for (size_t i = 0; i < n; ++i) {
      if (std::fabs(v0[i].pos[0] - v1[i].pos[0]) > 1e-3f || std::fabs(v0[i].pos[1] - v1[i].pos[1]) > 1e-3f) {
        moved = true;
        break;
      }
    }
    CHECK(moved);

    // With roadside grass off there is nothing left to draw: no landmark
    // at a fixed position near the Gauntlet sign-up (WORLD-15).
    std::vector<world::DebrisVertex> v_no_grass;
    anim.build_mesh(2005.0, 146.5, 955.0, 0.0f, &grass_ground, v_no_grass, false);
    CHECK(v_no_grass.empty());

    anim.invalidate_grass_ground_cache();
    CHECK(anim.cached_grass_ground_count() == 0);
  }

  // 5. SettingsPage motion blur & depth of field toggles.
  {
    carview::SettingsPage page;
    page.load({});
    CHECK(page.value("motion blur") == "on");
    CHECK(page.value("depth of field") == "on");
    CHECK(page.live_settings().motion_blur);
    CHECK(page.live_settings().dof);

    page.load({"--no-motion-blur", "--no-dof"});
    CHECK(page.value("motion blur") == "off");
    CHECK(page.value("depth of field") == "off");
    CHECK(!page.live_settings().motion_blur);
    CHECK(!page.live_settings().dof);
    const auto toks = page.tokens();
    bool has_no_mb = false, has_no_dof = false;
    for (const auto& t : toks) {
      if (t == "--no-motion-blur") has_no_mb = true;
      if (t == "--no-dof") has_no_dof = true;
    }
    CHECK(has_no_mb && has_no_dof);
  }

  // 5b. World terrain/road material key & far-ring UberLOD invariants.
  {
    world::MeshMaterial a, b;
    a.effect = b.effect = "H_ROAD_DIFF3_NORM1_AO_LM";
    a.is_road = 0;
    b.is_road = 1;
    CHECK(a.key() != b.key());
    world::MeshMaterial c = a, d = a;
    c.decal_noise_c4 = {3.5f, 8.0f};
    d.decal_noise_c4 = {2.0f, 6.0f};
    CHECK(c.key() != d.key());
    world::MeshMaterial e1 = a, e2 = a;
    e1.emissive_force = 0.0f;
    e2.emissive_force = 0.8f;
    CHECK(e1.key() != e2.key());
    world::MeshMaterial em1 = a, em2 = a;
    em1.is_emissive_map = 0;
    em2.is_emissive_map = 1;
    CHECK(em1.key() != em2.key());
    world::GatherOptions go;
    CHECK(go.far_inner_uberlod >= 800.0);
    // Verify subterranean catch planes & shadow-only mountain proxies are excluded from color meshes.
    CHECK(world::skipped_object("ShadowMountainsA3_NOLOD"));

    // Spatial world-space LOD deduplication: distant LOD00 sharing the same
    // lod_family must NOT suppress a higher-LOD object > 50 m away, while a
    // co-located LOD00 within 25 m still suppresses duplicate LOD01/LOD02.
    {
      world::WorldLodFilter f;
      f.add("gondola_lod**_009", 0, {0.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 10.0f});
      // Co-located LOD01/LOD02 at same spot -> suppressed; LOD00 itself -> kept.
      CHECK(!f.suppressed("gondola_lod**_009", 0, {0.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 10.0f}));
      CHECK(f.suppressed("gondola_lod**_009", 1, {2.0f, 0.0f, 2.0f}, {12.0f, 10.0f, 12.0f}));
      CHECK(f.suppressed("gondola_lod**_009", 2, {18.0f, 0.0f, 0.0f}, {28.0f, 10.0f, 10.0f}));
      // Distant LOD01 200 m away sharing the same family -> NOT suppressed.
      CHECK(!f.suppressed("gondola_lod**_009", 1, {200.0f, 0.0f, 0.0f}, {210.0f, 10.0f, 10.0f}));
      // Unrelated family at the exact same spot -> NOT suppressed.
      CHECK(!f.suppressed("other_lod**_009", 1, {0.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 10.0f}));
    }

    // Degraded LOD02 detection for placed buildings/props.
    {
      // Fewer sub-objects on LOD02 -> degraded.
      CHECK(world::prop_lod_degraded({-50, 0, -50}, {50, 30, 50}, 2, {-50, 0, -50}, {50, 30, 50}, 1));
      // Bounding box shrunk by > 15% and > 2 m -> degraded (e.g. missing tent roof).
      CHECK(world::prop_lod_degraded({-10, 0, -10}, {10, 8, 10}, 1, {-6, 0, -6}, {6, 8, 6}, 1));
      // Intact LOD02 with same object count and minor (< 15%) shrink -> NOT degraded.
      CHECK(!world::prop_lod_degraded({-10, 0, -10}, {10, 8, 10}, 1, {-9.5f, 0, -9.5f}, {9.5f, 8, 9.5f}, 1));
      CHECK(world::kLargeBuildingExtent == 25.0f);
    }
    CHECK(world::skipped_object("Underground_NOLOD"));
    CHECK(world::skipped_object("World_Cage_01"));
    CHECK(!world::skipped_object("Terrain_Road_LOD00"));
    // The far ring's level: by the name, else an object drawn only with the
    // UberLOD effect (a far terrain sheet named like near terrain).
    CHECK(world::far_level("TERR_Zone_MIDDIST_01_LOD00") == 1);
    CHECK(world::far_level("UberLOD_01_LOD00") == 2);
    CHECK(world::far_level("UberLOD_01_CAGE_LOD00") == 0);
    CHECK(world::far_level("TERR_CUBE_Area_01") == 0);
    CHECK(world::far_level("Hills_Terrain_LOD00") == 0);
    CHECK(world::far_level("Hills_Terrain_07_LOD00", {"H_BLND1_SPEC"}) == 2);
    CHECK(world::far_level("Hills_Terrain_07_LOD00", {"H_BLND1_SPEC", "H_BLND1_SPEC"}) == 2);
    CHECK(world::far_level("Hills_Terrain_07_LOD00", {"H_BLND1_SPEC", "H_VBLND_NORM_AO_LM"}) == 0);
    CHECK(world::far_level("Hills_Terrain_07_LOD00", {"H_BLND1"}) == 0);
    CHECK(world::far_level("Hills_MIDDIST_07_LOD00", {"H_BLND1_SPEC"}) == 1);
    CHECK(world::far_level("Hills_CAGE_LOD00", {"H_BLND1_SPEC"}) == 0);

    // The near ground's footprint for the far ring's hole filling: a 100 m
    // square round (1000, -500) in 10 m cells; a near quad covers its west
    // half, a sliver triangle (smaller than a cell) one cell, an edge-on
    // triangle (an armco) none.
    world::GroundCover cover(1000, -500, 50, 10);
    cover.add({950, 0, -550}, {1000, 0, -550}, {1000, 0, -450});
    cover.add({950, 0, -550}, {1000, 0, -450}, {950, 0, -450});
    cover.add({1031, 0, -531}, {1033, 0, -531}, {1031, 0, -533});
    cover.add({1010, 0, -490}, {1040, 3, -490}, {1040, 0, -490});
    CHECK(cover.covered(960, -540) && cover.covered(995, -455));
    CHECK(!cover.covered(1005, -545) && !cover.covered(1045, -455));
    CHECK(cover.covered(1035, -535));   // the sliver's cell
    CHECK(!cover.covered(1025, -495));  // the armco's footprint is a line
    CHECK(cover.covered(2000, 0));      // outside the grid
    CHECK(cover.cell_of(2000, 0) == -1 && cover.cell_of(955, -545) == 0);
    const auto samples = world::footprint_samples({0, 0, 0}, {30, 0, 0}, {0, 0, 30});
    CHECK(samples[3][0] == 15 && samples[3][1] == 0);
    CHECK(samples[6][0] == 10 && samples[6][1] == 10);
    // Height-aware hole filling: a hillside rising well above a lower roadside patch is NOT covered.
    CHECK(cover.covered_at(960, 0.5, -540, 1.5));
    CHECK(!cover.covered_at(960, 8.0, -540, 1.5));
    CHECK(!cover.covered_at(1005, 0.0, -545, 1.5));
    // CrowdTERR authoring helper mesh is skipped so it never z-fights festival grounds.
    CHECK(world::skipped_material("CrowdTERR"));
    CHECK(world::skipped_material("crowdterr"));

    // Fast-travel spawn solver: avoids landing on overhead arches/roofs AND nudges out of
    // enclosing booth/arch pillars (like Race Central's festival_02) onto open ground nearby.
    {
      class ArchPillarGround final : public drive::Ground {
       public:
        bool raycast(const drive::Vec3& o, const drive::Vec3& d, double max_d, drive::Hit& h) const override {
          double best_t = max_d + 1.0;
          drive::Vec3 best_n{0, 1, 0};
          auto hit_plane_y = [&](double py, double ny, double xmin, double xmax, double zmin, double zmax) {
            if (std::fabs(d.y) < 1e-6) return;
            const double t = (py - o.y) / d.y;
            if (t < 0.0 || t >= best_t) return;
            const double hx = o.x + d.x * t, hz = o.z + d.z * t;
            if (hx < xmin || hx > xmax || hz < zmin || hz > zmax) return;
            best_t = t;
            best_n = {0, ny, 0};
          };
          // Road everywhere at y = -6.5.
          hit_plane_y(-6.5, 1.0, -50.0, 50.0, -50.0, 50.0);
          // Overhead sign roof at y = +4.2 over [-3, 3] x [-3, 3].
          hit_plane_y(4.2, 1.0, -3.0, 3.0, -3.0, 3.0);
          // Pillar vertical wall box around [-2.5, 2.5] x [-2.5, 2.5], y in [-6.5, 4.2].
          auto hit_wall_x = [&](double wx, double nx) {
            if (std::fabs(d.x) < 1e-6) return;
            const double t = (wx - o.x) / d.x;
            if (t < 0.0 || t >= best_t) return;
            const double hy = o.y + d.y * t, hz = o.z + d.z * t;
            if (hy < -6.5 || hy > 4.2 || hz < -2.5 || hz > 2.5) return;
            best_t = t;
            best_n = {nx, 0, 0};
          };
          auto hit_wall_z = [&](double wz, double nz) {
            if (std::fabs(d.z) < 1e-6) return;
            const double t = (wz - o.z) / d.z;
            if (t < 0.0 || t >= best_t) return;
            const double hy = o.y + d.y * t, hx = o.x + d.x * t;
            if (hy < -6.5 || hy > 4.2 || hx < -2.5 || hx > 2.5) return;
            best_t = t;
            best_n = {0, 0, nz};
          };
          hit_wall_x(-2.5, -1.0);
          hit_wall_x(2.5, 1.0);
          hit_wall_z(-2.5, -1.0);
          hit_wall_z(2.5, 1.0);
          if (best_t > max_d) return false;
          h.distance = best_t;
          h.normal = best_n;
          h.surface = 0;
          return true;
        }
        void sphere_contacts(const drive::Vec3&, const drive::SphereQuery*, int n, drive::SphereHit* hits) const override {
          for (int i = 0; i < n; ++i) hits[i] = {};
        }
      };
      ArchPillarGround g;
      // 1) Target inside pillar & under roof: must land on road y == -6.5 AND outside [-2.5, 2.5] pillar.
      double x = 0.0, y = -6.45, z = 0.0;
      CHECK(world::resolve_fast_travel_spawn(g, nullptr, x, y, z, true, -6.45));
      CHECK(std::fabs(y - (-6.5)) < 0.1);
      CHECK(std::hypot(x, z) >= 4.0);
      // 2) Same without hint_y: must STILL avoid roof (+4.2) and pillar interior.
      double x2 = 0.0, y2 = 0.0, z2 = 0.0;
      CHECK(world::resolve_fast_travel_spawn(g, nullptr, x2, y2, z2, false, 0.0));
      CHECK(std::fabs(y2 - (-6.5)) < 0.1);
      CHECK(std::hypot(x2, z2) >= 4.0);
      // 3) Already open road at (15, 0): stays at exact (15, -6.5, 0).
      double x3 = 15.0, y3 = 0.0, z3 = 0.0;
      CHECK(world::resolve_fast_travel_spawn(g, nullptr, x3, y3, z3, false, 0.0));
      CHECK(std::fabs(x3 - 15.0) < 1e-6 && std::fabs(z3) < 1e-6 && std::fabs(y3 - (-6.5)) < 0.1);
    }
  }

  // 5b2. Clean-room Round 2 world material resolution & TrackSettings road/lightmap parsing:
  // - H_VBLND_*_AO* authored AO preservation (file-backed AO kept, type-1 stubs filtered out)
  // - Dual _EMIS + _LM building prop binding (both Light_Map slot 7 and EmissiveMap slot 3 preserved)
  // - H_ROAD Noise (slot 2) & Modulate (slot 6) slot assignment + RoadSpecular / TrackLightMapParams
  // - Non-prop SpecMap binding & H_VBLND_DECAL*_MASK* decal masking
  {
    const std::string track_xml = R"(
      <TrackSettings>
        <RoadSpecular RoadSpec="0.95" RoadGlossiness="56.0" RoadFresnel="0.34" RoadSkid="0.72" RoadSkid2="0.48"/>
        <TrackLightMapParams Scale="1.65" Bias="0.03" AOFloor="0.08"/>
      </TrackSettings>
    )";
    world::TrackMaterialSettings tms;
    CHECK(tms.load_xml(track_xml));
    CHECK(tms.loaded);
    CHECK(std::fabs(tms.road_specular_scale - 0.95f) < 1e-4f);
    CHECK(std::fabs(tms.road_glossiness - 56.0f) < 1e-4f);
    CHECK(std::fabs(tms.road_fresnel - 0.34f) < 1e-4f);
    CHECK(std::fabs(tms.road_skid_strength - 0.72f) < 1e-4f);
    CHECK(std::fabs(tms.road_skid2_strength - 0.48f) < 1e-4f);
    CHECK(std::fabs(tms.lightmap_scale - 1.65f) < 1e-4f);
    CHECK(std::fabs(tms.lightmap_bias - 0.03f) < 1e-4f);
    CHECK(std::fabs(tms.lightmap_ao_floor - 0.08f) < 1e-4f);

    const uint32_t N = world::kNoTexture;
    // (a) H_VBLND_*_AO* authored AO preservation: file-backed slot 5 AO must NOT be stripped.
    {
      const std::array<uint32_t, 8> raw{100u, 101u, N, 103u, N, 105u, N, 107u};
      const std::array<uint32_t, 8> file{100u, 101u, N, 103u, N, 105u, N, 107u};
      const std::vector<std::array<float, 4>> pc{{2.0f, 2.0f, 3.0f, 3.0f}};
      const auto r = world::resolve_effect_material("H_VBLND_NORM_AO_LM", raw, file, pc, tms);
      CHECK(r.material.layers == world::Layers::kVertexBlend);
      CHECK(r.material.albedo == 100u);
      CHECK(r.material.layer_b == 101u);
      CHECK(r.material.normal_map == 103u);
      CHECK(r.material.ao_map == 105u);
      CHECK(r.material.light_map == 107u);
      CHECK(std::fabs(r.material.decal_noise_c4[0] - 1.65f) < 1e-4f);
      CHECK(std::fabs(r.material.decal_noise_c4[1] - 0.08f) < 1e-4f);

      // Type-1 stub in slot 5 (present in raw, kNoTexture in file) must stay kNoTexture (MIDDIST hills).
      const std::array<uint32_t, 8> stub_file{100u, 101u, N, 103u, N, N, N, N};
      const auto r_stub = world::resolve_effect_material("H_VBLND_NORM_AO_LM", raw, stub_file, pc, tms);
      CHECK(r_stub.material.ao_map == N);
    }

    // (b) Dual _EMIS + _LM building prop: preserve BOTH slot 7 Light_Map AND slot 3 EmissiveMapSampler.
    {
      const std::array<uint32_t, 8> slots{200u, 201u, N, 203u, 204u, 205u, N, 207u};
      const std::vector<std::array<float, 4>> pc{{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0.25f}};
      const auto r = world::resolve_effect_material("H_DIFF_NORM_SPEC_EMIS_AO_LM", slots, slots, pc, tms);
      CHECK(r.material.albedo == 200u);
      CHECK(r.material.normal_map == 201u);
      CHECK(r.material.spec_map == 204u);
      CHECK(r.material.ao_map == 205u);
      CHECK(r.material.light_map == 207u);
      CHECK(r.material.layer_c == 203u);
      CHECK(r.material.is_emissive_map == world::kEmissiveDualLm);
      CHECK(std::fabs(r.material.emissive_force - 0.25f) < 1e-4f);

      // When slot 7 Light_Map is stubbed/empty, _EMIS falls back to kEmissiveOnly in light_map.
      const std::array<uint32_t, 8> no_lm{200u, 201u, N, 203u, 204u, 205u, N, N};
      const auto r_em = world::resolve_effect_material("H_DIFF_NORM_SPEC_EMIS_AO_LM", slots, no_lm, pc, tms);
      CHECK(r_em.material.light_map == 203u);
      CHECK(r_em.material.layer_c == N);
      CHECK(r_em.material.is_emissive_map == world::kEmissiveOnly);
    }

    // (c) H_ROAD Noise (slot 2) & Modulate (slot 6) + AO slot 4/6 + RoadSpecular propagation.
    {
      const std::array<uint32_t, 8> slots{300u, 301u, 302u, 303u, 304u, 305u, 306u, 307u};
      const std::vector<std::array<float, 4>> pc{{4.0f, 4.0f, 2.0f, 2.0f}, {3.0f, 3.0f, 0.5f, 0.25f}};
      const auto r = world::resolve_effect_material("H_ROAD_DIFF3_NORM1_NOISE_MODULATE_AO_LM", slots, slots, pc, tms);
      CHECK((r.material.is_road & world::kRoadSurface) != 0u);
      CHECK((r.material.is_road & world::kRoadNoise) != 0u);
      CHECK((r.material.is_road & world::kRoadModulate) != 0u);
      CHECK(r.material.albedo == 300u);
      CHECK(r.material.layer_b == 301u);
      CHECK(r.material.splat == 302u);      // slot 2 Noise
      CHECK(r.material.normal_map == 303u); // slot 3 NormalMap
      CHECK(r.material.ao_map == 304u);     // slot 4 AO (when DIFF3 + MODULATE)
      CHECK(r.material.layer_c == 305u);    // slot 5 Blend_C
      CHECK(r.material.spec_map == 306u);   // slot 6 Modulate
      CHECK(r.material.light_map == 307u);  // slot 7 Light_Map
      CHECK(r.splat_uv == 1);
      CHECK(std::fabs(r.splat_tiling[0] - 0.5f) < 1e-4f && std::fabs(r.splat_tiling[1] - 0.25f) < 1e-4f);
      CHECK(std::fabs(r.material.decal_noise_c4[0] - 0.95f) < 1e-4f);
      CHECK(std::fabs(r.material.decal_noise_c4[1] - 56.0f) < 1e-4f);

      // H_ROAD_DIFF3 without MODULATE places AO in slot 6 and does not set kRoadModulate.
      const auto r_nomod = world::resolve_effect_material("H_ROAD_DIFF3_NORM1_NOISE_AO_LM", slots, slots, pc, tms);
      CHECK((r_nomod.material.is_road & world::kRoadNoise) != 0u);
      CHECK((r_nomod.material.is_road & world::kRoadModulate) == 0u);
      CHECK(r_nomod.material.splat == 302u);
      CHECK(r_nomod.material.ao_map == 306u);
      CHECK(r_nomod.material.spec_map == N);
    }

    // (d) Non-prop SpecMap binding & H_VBLND_DECAL*_MASK* decal masking.
    {
      const std::array<uint32_t, 8> slots{400u, 401u, N, 403u, 404u, 405u, N, 407u};
      const auto r_spec = world::resolve_effect_material("H_VBLND_NORM_SPEC_AO_LM", slots, slots, {}, tms);
      CHECK(r_spec.material.spec_map == 404u);
      CHECK(r_spec.material.ao_map == 405u);

      const std::array<uint32_t, 8> mask_slots{500u, N, 502u, 503u, N, N, N, N};
      const auto r_mask = world::resolve_effect_material("H_VBLND_DECAL_NORM_MASK", mask_slots, mask_slots, {}, tms);
      CHECK(r_mask.material.layers == world::Layers::kVertexFade);
      CHECK(r_mask.material.splat == 502u);
      CHECK(r_mask.material.decal_noise_c4[1] < 0.0f);  // negative sentinel selects _MASK mode in world_frag.glsl

      const std::array<uint32_t, 8> noise_slots{510u, N, 512u, N, N, N, N, N};
      const std::vector<std::array<float, 4>> npc{{1, 1, 1, 1}, {1, 1, 2, 2}, {1, 1, 1, 1}, {1, 1, 1, 1}, {3.5f, 8.0f, 0, 0}};
      const auto r_noise = world::resolve_effect_material("H_VBLND_DECAL_NOISE", noise_slots, noise_slots, npc, tms);
      CHECK(r_noise.material.splat == 512u);
      CHECK(r_noise.material.decal_noise_c4[1] > 0.0f);

      // (e) R3-REN-15: _NORM2 multi-layer road/terrain binds Layer B normal map from RMB slot 4
      // without clobbering AO or H_ROAD Modulate.
      const std::array<uint32_t, 8> norm2_slots{600u, 601u, 602u, 603u, 604u, 605u, 606u, 607u};
      const auto r_road_n2 = world::resolve_effect_material("H_ROAD_DIFF2_NORM2_AO_LM", norm2_slots, norm2_slots, {}, tms);
      CHECK(r_road_n2.material.normal_map == 603u);
      CHECK(r_road_n2.material.spec_map == 604u);  // Layer B normal map
      CHECK(r_road_n2.material.ao_map == 605u);
      const auto r_blnd_n2 = world::resolve_effect_material("H_BLND2_NORM2_AO_LM", norm2_slots, norm2_slots, {}, tms);
      CHECK(r_blnd_n2.material.normal_map == 603u);
      CHECK(r_blnd_n2.material.spec_map == 604u);  // Layer B normal map
      CHECK(r_blnd_n2.material.ao_map == 605u);
      // H_ROAD_DIFF3 + MODULATE keeps slot 4 as AO and slot 6 as Modulate (never aliases Layer B normal).
      const auto r_mod_n2 =
          world::resolve_effect_material("H_ROAD_DIFF3_NORM2_MODULATE_AO_LM", norm2_slots, norm2_slots, {}, tms);
      CHECK(r_mod_n2.material.ao_map == 604u);
      CHECK(r_mod_n2.material.spec_map == 606u);

      // (f) H_DIFF_SPEC_REFL_AO_4_LM (Horizon Festival hub arches): encodes authored c2.x
      // reflection strength as negative decal_noise_c4[1] while preserving lightmap_scale.
      const std::array<uint32_t, 8> refl_slots{700u, N, 702u, N, 704u, 705u, N, 707u};
      const std::vector<std::array<float, 4>> refl_pc{{1, 1, 1, 0.4f}, {6.5f, 0, 0, 0}, {1.9f, 0, 0, 0}};
      const auto r_refl =
          world::resolve_effect_material("H_DIFF_SPEC_REFL_AO_4_LM", refl_slots, refl_slots, refl_pc, tms);
      CHECK(r_refl.material.spec_map == 704u);
      CHECK(r_refl.material.ao_map == 705u);
      CHECK(r_refl.material.light_map == 707u);
      CHECK(std::fabs(r_refl.material.decal_noise_c4[0] - tms.lightmap_scale) < 1e-4f);
      CHECK(std::fabs(r_refl.material.decal_noise_c4[1] - (-1.9f)) < 1e-4f);
    }
  }

  // 5c. Spectators.zip `.skinbin` big-endian parser (valid mesh + outward winding + truncated rejection).
  {
    auto push_be16 = [](std::vector<uint8_t>& buf, uint16_t v) {
      buf.push_back(uint8_t((v >> 8) & 0xFF));
      buf.push_back(uint8_t(v & 0xFF));
    };
    auto push_be32 = [](std::vector<uint8_t>& buf, uint32_t v) {
      buf.push_back(uint8_t((v >> 24) & 0xFF));
      buf.push_back(uint8_t((v >> 16) & 0xFF));
      buf.push_back(uint8_t((v >> 8) & 0xFF));
      buf.push_back(uint8_t(v & 0xFF));
    };
    auto push_bef32 = [&](std::vector<uint8_t>& buf, float f) {
      uint32_t bits = 0;
      std::memcpy(&bits, &f, 4);
      push_be32(buf, bits);
    };

    std::vector<uint8_t> bin(0x40, 0);
    auto set_be32 = [&](size_t off, uint32_t v) {
      bin[off + 0] = uint8_t((v >> 24) & 0xFF);
      bin[off + 1] = uint8_t((v >> 16) & 0xFF);
      bin[off + 2] = uint8_t((v >> 8) & 0xFF);
      bin[off + 3] = uint8_t(v & 0xFF);
    };
    set_be32(0x04, 4);  // num_lods (4-LOD spectator layout: vert_count at 0x38, verts at 0x40)
    set_be32(0x14, 3);  // lod0_count
    set_be32(0x38, 3);  // vertex_count

    const float verts[3][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    for (int i = 0; i < 3; ++i) {
      push_bef32(bin, verts[i][0]);
      push_bef32(bin, verts[i][1]);
      push_bef32(bin, verts[i][2]);
      push_bef32(bin, 0.0f);
      push_bef32(bin, 0.0f);
      push_bef32(bin, 1.0f);  // +Z normal
      push_be16(bin, uint16_t(i * 16384));
      push_be16(bin, uint16_t(i * 8192));
      bin.push_back(0);
      bin.push_back(0);
      bin.push_back(255);
      bin.push_back(0);
      push_be32(bin, 1);
    }
    push_be32(bin, 3);  // total_index_count
    // Intentionally CW relative to +Z normal; parser must flip to outward CCW.
    push_be16(bin, 0);
    push_be16(bin, 2);
    push_be16(bin, 1);

    world::SpectatorMeshLod mesh;
    CHECK(world::parse_skinbin(bin, mesh, 0));
    CHECK(mesh.vertices.size() == 3);
    CHECK(mesh.indices.size() == 3);
    if (mesh.indices.size() == 3) {
      CHECK(mesh.indices[0] == 0 && mesh.indices[1] == 1 && mesh.indices[2] == 2);
    }

    // 1-LOD driver layout: vert_count at 0x20, verts at 0x28.
    std::vector<uint8_t> bin1(0x28, 0);
    auto set_be32_1 = [&](size_t off, uint32_t v) {
      bin1[off + 0] = uint8_t((v >> 24) & 0xFF);
      bin1[off + 1] = uint8_t((v >> 16) & 0xFF);
      bin1[off + 2] = uint8_t((v >> 8) & 0xFF);
      bin1[off + 3] = uint8_t(v & 0xFF);
    };
    set_be32_1(0x04, 1);
    set_be32_1(0x14, 3);
    set_be32_1(0x20, 3);
    bin1.insert(bin1.end(), bin.begin() + 0x40, bin.end());
    world::SpectatorMeshLod mesh1;
    CHECK(world::parse_skinbin(bin1, mesh1, 0));
    CHECK(mesh1.vertices.size() == 3);
    CHECK(mesh1.indices.size() == 3);

    // Truncated buffer must fail cleanly.
    std::vector<uint8_t> bad(bin.begin(), bin.begin() + 32);
    CHECK(!world::parse_skinbin(bad, mesh, 0));
  }

  // 6. Retail Colorado corpus checks (when SHIFT_GAME_ROOT is set).
  if (const char* gr = std::getenv("SHIFT_GAME_ROOT"); gr && *gr) {
    const std::filesystem::path media = std::filesystem::path(gr) / "media";
    const std::filesystem::path colorado = media / "tracks" / "colorado";
    if (std::filesystem::exists(colorado / "TrackSettings.xml")) {
      world::TrackPostConfig cfg;
      CHECK(cfg.load_xml(read_text(colorado / "TrackSettings.xml")));
      const float wlen = std::sqrt(cfg.wind_dir[0] * cfg.wind_dir[0] + cfg.wind_dir[1] * cfg.wind_dir[1] +
                                   cfg.wind_dir[2] * cfg.wind_dir[2]);
      CHECK(std::isfinite(wlen) && std::fabs(wlen - 1.0f) < 1e-3f);
      CHECK(cfg.detail_blur_mph_max > 10.0f);

      world::PostZoneSet zs;
      CHECK(zs.load(colorado, media));
      CHECK(zs.zones.size() == 16);
      CHECK(zs.bloom_templates.size() >= 7);
      CHECK(zs.color_templates.size() >= 4);
      CHECK(zs.fog_templates.size() >= 6);
      CHECK(zs.vignette_templates.size() >= 14);

      world::SpectatorCatalog cat;
      CHECK(cat.load_from_media(media));
      CHECK(cat.num_sprite_angles == 10);
      CHECK(cat.models.size() >= 16);
      CHECK(cat.modelsets.size() >= 2);
      CHECK(cat.crowdclasses.size() >= 20);
      CHECK(cat.spectators_anim_bins > 50);
      CHECK(cat.animated_pgeos > 200);
      CHECK(cat.has_3d_spectators());
      CHECK(cat.assets.size() >= 8);

      // PT-08 Hotspot B & PT-16: verify per-spectator ground height caching (zero repeated raycasts),
      // frustum culling, and lively visible crowd animation displacement over time.
      {
        world::AnimatedWorldSystem anim;
        CHECK(anim.init(media, cfg, nullptr, colorado));
        FlatTestGround gnd(2);
        std::vector<world::SpectatorVertex> sv0, sv1, sv_culled;
        std::vector<std::array<float, 4>> discs;
        float nearest_d = 1e6f;
        // Festival barrier cluster sits near (1994, 144, 957).
        anim.build_spectator_mesh(1994.0, 145.5, 965.0, 0.0f, &gnd, sv0, &discs, nullptr, &nearest_d);
        CHECK(!sv0.empty());
        CHECK(nearest_d < 25.0f);
        CHECK(anim.cached_spectator_ground_count() > 0);
        const size_t rays_after_first = gnd.raycast_calls;
        CHECK(rays_after_first > 0);

        // Second build at t=0.55s: ZERO additional ground raycasts, and visible limb motion (> 12 cm)!
        anim.build_spectator_mesh(1994.0, 145.5, 965.0, 0.55f, &gnd, sv1, &discs, nullptr, &nearest_d);
        CHECK(gnd.raycast_calls == rays_after_first);
        CHECK(sv1.size() == sv0.size());
        float max_disp = 0.0f;
        for (size_t i = 0; i < sv0.size(); ++i) {
          const float dx = sv1[i].pos[0] - sv0[i].pos[0];
          const float dy = sv1[i].pos[1] - sv0[i].pos[1];
          const float dz = sv1[i].pos[2] - sv0[i].pos[2];
          max_disp = std::max(max_disp, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        CHECK(max_disp > 0.12f);

        // Frustum whose in-front half-space starts 500 m ahead (beyond max_crowd_dist) culls all nearby crowds.
        world::SpectatorFrustum away{};
        away.active = true;
        away.origin_x = 1994.0;
        away.origin_z = 965.0;
        away.planes[0] = {0.70710678f, 0.0f, 0.70710678f, 0.0f};
        away.planes[1] = {-0.70710678f, 0.0f, 0.70710678f, 0.0f};
        away.planes[2] = {0.0f, 0.70710678f, 0.70710678f, 0.0f};
        away.planes[3] = {0.0f, -0.70710678f, 0.70710678f, 0.0f};
        away.planes[4] = {0.0f, 0.0f, 1.0f, -500.0f};
        anim.build_spectator_mesh(1994.0, 145.5, 965.0, 0.55f, &gnd, sv_culled, nullptr, &away, nullptr);
        CHECK(sv_culled.empty());

        anim.invalidate_spectator_ground_cache();
        CHECK(anim.cached_spectator_ground_count() == 0);
      }

      // Verify Shared.zip decodes all 4 shared car interior normal maps used as fallbacks.
      {
        const auto shared_zip = media / "cars" / "Shared.zip";
        if (std::filesystem::exists(shared_zip)) {
          cars::FolderRef ref;
          ref.kind = cars::FolderRef::Kind::kArchive;
          ref.name = "shared";
          ref.path = shared_zip;
          ref.origin = "disc";
          cars::bake::Bake shared_bake;
          std::string serr;
          CHECK(cars::bake_shared(ref, shared_bake, &serr));
          auto has_tex = [&](const char* want) {
            for (const auto& t : shared_bake.textures)
              if (t.name == want && !t.levels.empty()) return true;
            return false;
          };
          CHECK(has_tex("plastic_nrm.xds"));
          CHECK(has_tex("plastic2_nrm.xds"));
          CHECK(has_tex("leather_nrm.xds"));
          CHECK(has_tex("stitches_nrm.xds"));
        }
      }

      std::printf("world_polish_tests corpus: %zu zones, %zu blooms, %zu colors, %zu fogs, %zu vignettes, "
                  "%zu spectator models (%zu 3D), %zu crowdclasses, %zu anim_pgeo\n",
                  zs.zones.size(), zs.bloom_templates.size(), zs.color_templates.size(), zs.fog_templates.size(),
                  zs.vignette_templates.size(), cat.models.size(), cat.assets.size(), cat.crowdclasses.size(),
                  cat.animated_pgeos);
    }
  }

  std::printf("world_polish_tests: %d failures\n", g_failures);
  return g_failures ? 1 : 0;
}
