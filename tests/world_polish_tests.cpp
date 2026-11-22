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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "drive/vehicle.h"
#include "settings_page.h"
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
    if (dir.y >= -1e-6) return false;
    const double t = (144.8 - origin.y) / dir.y;
    if (t < 0.0 || t > max_dist) return false;
    hit.distance = t;
    hit.normal = {0.0, 1.0, 0.0};
    hit.surface = surface_;
    return true;
  }
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

  // 4. AnimatedWorldSystem mesh generation & animation motion + grass road/dirt filtering.
  {
    world::AnimatedWorldSystem anim;
    FlatTestGround grass_ground(2);   // Grass in surfaceTypes.xml
    FlatTestGround road_ground(0);    // Asphalt in surfaceTypes.xml
    std::vector<world::DebrisVertex> v0, v1, v_road;
    anim.build_mesh(2005.0, 146.5, 955.0, 0.0f, &grass_ground, v0, true);
    anim.build_mesh(2005.0, 146.5, 955.0, 1.5f, &grass_ground, v1, true);
    anim.build_mesh(2005.0, 146.5, 955.0, 0.0f, &road_ground, v_road, true);
    CHECK(v0.size() > 1000 && v0.size() % 3 == 0);
    CHECK(v0.size() > v_road.size());  // grass grows on dirt, skipped on asphalt road
    // Verify vertices actually animate over time (balloons, windmills, flags, crowds, grass).
    bool moved = false;
    const size_t n = std::min(v0.size(), v1.size());
    for (size_t i = 0; i < n; ++i) {
      if (std::fabs(v0[i].pos[0] - v1[i].pos[0]) > 1e-3f || std::fabs(v0[i].pos[1] - v1[i].pos[1]) > 1e-3f) {
        moved = true;
        break;
      }
    }
    CHECK(moved);
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
    world::GatherOptions go;
    CHECK(go.far_inner_uberlod >= 800.0);
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
      std::printf("world_polish_tests corpus: %zu zones, %zu blooms, %zu colors, %zu fogs, %zu vignettes, "
                  "%zu spectator models, %zu crowdclasses, %zu anim_pgeo\n",
                  zs.zones.size(), zs.bloom_templates.size(), zs.color_templates.size(), zs.fog_templates.size(),
                  zs.vignette_templates.size(), cat.models.size(), cat.crowdclasses.size(), cat.animated_pgeos);
    }
  }

  std::printf("world_polish_tests: %d failures\n", g_failures);
  return g_failures ? 1 : 0;
}
