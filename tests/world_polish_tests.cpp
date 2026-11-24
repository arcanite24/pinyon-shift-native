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
    // Verify synthetic procedural windmills/flags/box-crowds are NOT emitted near ground level
    // when grass is disabled (only high-altitude hot-air balloons remain).
    std::vector<world::DebrisVertex> v_no_grass;
    anim.build_mesh(2005.0, 146.5, 955.0, 0.0f, &grass_ground, v_no_grass, false);
    for (const auto& v : v_no_grass) {
      CHECK(v.pos[1] > 165.0f);
    }
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
    CHECK(world::skipped_object("Underground_NOLOD"));
    CHECK(world::skipped_object("World_Cage_01"));
    CHECK(!world::skipped_object("Terrain_Road_LOD00"));
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
