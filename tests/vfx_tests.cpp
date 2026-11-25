#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "fh1data/archive.h"
#include "fh1data/collision.h"
#include "world/vfx.h"
#include "world/world_anim.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* msg, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL (line %d): %s\n", line, msg);
    ++g_failures;
  }
}

#define CHECK(c) check((c), #c, __LINE__)

void test_xml_parsers() {
  // 1. Effect XML with Box emitter dual <Size> elements + midnight-wrapping TimeOfDayData.
  const std::string xml = R"(
<Effect Version="14" budget="200" lowerbudget="100">
  <TimeOfDayData startH="20" startM="0" endH="4" endM="0"/>
  <EmitterProperties type="Box">
    <Size length="1.0" height="5.0" width="2.0"/>
    <Quantity value="40.0" var="0.0"/>
    <Velocity value="3.5" variation="0.2"/>
    <EmitTime start="0.0" end="0.0" infinite="1"/>
    <Life value="1.5" variation="0.1"/>
    <ParticleShaderType Version="3" value="_animated_soft"/>
    <Animation rate="1.0" numFramesX="4" numFramesY="2"/>
    <Size valueX="0.20" valueY="0.40" variationX="0.10" variationY="0.15"
          growforX="0.50" growforY="0.50" shrinkforX="0.25" shrinkforY="0.25"
          maxValueX="0.80" maxValueY="1.20" endValueX="0.0" endValueY="0.0"/>
    <SpinTime value="4.0" variation="0.2"/>
    <SpinPhase value="0.0" variation="1.0"/>
    <SpinAxis type="Random" alternate="1"/>
  </EmitterProperties>
  <Behavior>
    <MotionInheritance influence="100" multiplier="0.45"/>
    <Gravity strength="-1.5" decay="0.0"/>
    <Friction strength="0.65" velocityKept="2.0"/>
  </Behavior>
  <Render>
    <ParticleType value="Quad" dynamic_lighting="1" zsort="1" max_screen_size="0.25">
      <light_influence v.x="0.2" v.y="1.8" v.z="1.4" v.w="1.0"/>
      <HeadLightPower value="1.25"/>
      <Texture value="GR_Backfire_DIFF.xds"/>
      <Material>
        <BlendType value="additive"/>
        <RGBStart r="1.0" g="0.9" b="0.5" a="1.0"/>
        <RGBMid r="1.0" g="0.6" b="0.2" a="0.8"/>
        <RGBEnd r="0.3" g="0.1" b="0.0" a="0.0"/>
        <FadeInTo fadeInTo="0.2"/>
        <FadeOutFrom fadeOutFrom="0.6"/>
      </Material>
    </ParticleType>
  </Render>
</Effect>)";

  world::ParticleEffectDef def;
  CHECK(world::parse_particle_effect_xml(xml, "TestFire", def));
  CHECK(def.name == "testfire");
  CHECK(def.budget == 200);
  CHECK(def.shader_kind == world::ParticleShaderKind::kAnimatedSoft);
  CHECK(def.num_frames_x == 4 && def.num_frames_y == 2);
  CHECK(std::fabs(def.size_x - 0.20f) < 1e-4f);
  CHECK(std::fabs(def.max_size_y - 1.20f) < 1e-4f);
  CHECK(def.additive);
  CHECK(def.velocity_quad);
  CHECK(def.texture_name == "gr_backfire_diff.xds");
  CHECK(std::fabs(def.headlight_power - 1.25f) < 1e-4f);
  CHECK(def.active_at_hour(22.0f));
  CHECK(def.active_at_hour(2.0f));
  CHECK(!def.active_at_hour(12.0f));

  // 2. SmashableEffects.xml
  std::map<std::string, std::string> se;
  CHECK(world::parse_smashable_effects_xml(
      R"(<Track><FileEffectPair smashableName="co_bench_001" system_name="BI_Wood_Picnic"/></Track>)", se));
  CHECK(se["co_bench_001"] == "bi_wood_picnic");

  // 3. BackfireTimerConfig.xml
  world::BackfireTimerConfig bf;
  CHECK(world::parse_backfire_timer_config_xml(R"(
<BackfireTimerConfig>
  <Backfire><Timer MinTime="0" MaxTime="0" MinNormRPM="0" MaxNormRPM="0" SilenceWeighting="100"/></Backfire>
  <Backfire><Timer MinTime="900" MaxTime="1800" MinNormRPM="0.6" MaxNormRPM="0.75" SilenceWeighting="95"/></Backfire>
  <Backfire><Timer MinTime="700" MaxTime="1200" MinNormRPM="0.6" MaxNormRPM="0.82" SilenceWeighting="90"/></Backfire>
  <Backfire><Timer MinTime="500" MaxTime="1000" MinNormRPM="0.4" MaxNormRPM="1.0" SilenceWeighting="80"/></Backfire>
</BackfireTimerConfig>)", bf));
  CHECK(bf.loaded);
  CHECK(std::fabs(bf.tiers[3].min_time_ms - 500.0f) < 1e-3f);
  CHECK(std::fabs(bf.tiers[3].silence_weighting - 80.0f) < 1e-3f);

  // 4. ParticleEmitters.xml (verifies file-frame Z negation -> game frame).
  std::vector<world::WorldEmitter> emitters;
  CHECK(world::parse_particle_emitters_xml(R"(
<ParticleEmitters>
  <SimpleEmitter>
    <Name value="fest_flare_1"/>
    <Effect value="_Composite_MainStage"/>
    <Type value="Superspray"/>
    <Position v.x="1992.4" v.y="143.6" v.z="-956.3"/>
    <Orientation>
      <XAxis v.x="1" v.y="0" v.z="0"/>
      <YAxis v.x="0" v.y="1" v.z="0"/>
      <ZAxis v.x="0" v.y="0" v.z="1"/>
    </Orientation>
  </SimpleEmitter>
</ParticleEmitters>)", emitters));
  CHECK(emitters.size() == 1);
  CHECK(std::fabs(emitters[0].pos[2] - 956.3f) < 0.05f);
  const auto expanded = world::expand_composite_effect(emitters[0].effect);
  CHECK(expanded.size() >= 3);

  // 5. surfaceTypes.xml SkidData / SmokeType / ShouldSpark
  const std::string surf_xml = R"(
<Root>
  <SurfaceTypesList>
    <SurfaceType value="Asphalt"/>
    <SurfaceType value="Dirt"/>
    <SurfaceType value="GuardRail"/>
  </SurfaceTypesList>
  <SurfaceTypes>
    <Asphalt Category="HardWorld">
      <SkidData>
        <ColorAlpha value="200"/><ColorRed value="0"/><ColorGreen value="0"/><ColorBlue value="0"/>
        <TextureIndex value="0"/><MinIntensity value="0.0"/>
        <SmokeType Smoke="1" Dust="0" Grass="0" Dirt="0"/>
      </SkidData>
      <ShouldSpark value="true"/>
    </Asphalt>
    <Dirt Category="SoftWorld">
      <SkidData>
        <ColorAlpha value="125"/><ColorRed value="255"/><ColorGreen value="255"/><ColorBlue value="255"/>
        <TextureIndex value="1"/><MinIntensity value="0.4"/>
        <SmokeType Smoke="0" Dust="0.5" Grass="0" Dirt="1"/>
      </SkidData>
      <ShouldSpark value="false"/>
    </Dirt>
    <GuardRail Category="GuardRail">
      <SkidData>
        <ColorAlpha value="0"/><ColorRed value="0"/><ColorGreen value="0"/><ColorBlue value="0"/>
        <TextureIndex value="0"/><MinIntensity value="0.0"/>
        <SmokeType Smoke="0" Dust="0" Grass="0" Dirt="0"/>
      </SkidData>
      <ShouldSpark value="true"/>
    </GuardRail>
  </SurfaceTypes>
</Root>)";
  std::vector<fh1::SurfaceType> surfs;
  std::string err;
  CHECK(fh1::parse_surface_types(surf_xml, surfs, &err));
  CHECK(surfs.size() == 3);
  CHECK(surfs[0].skid_a == 200 && surfs[0].skid_texture_index == 0 && surfs[0].smoke_weight > 0.9f &&
        surfs[0].should_spark);
  CHECK(surfs[1].skid_a == 125 && surfs[1].skid_texture_index == 1 &&
        std::fabs(surfs[1].skid_min_intensity - 0.4f) < 1e-4f && surfs[1].dirt_weight > 0.9f && !surfs[1].should_spark);
  CHECK(surfs[2].skid_a == 0 && surfs[2].should_spark);
}

void test_particle_simulation() {
  world::ParticleLibrary lib;
  auto make_eff = [](const char* name, bool additive, float grav, float life) {
    world::ParticleEffectDef d;
    d.name = name;
    d.quantity = 60.0f;
    d.life = life;
    d.velocity = 2.0f;
    d.gravity_strength = grav;
    d.additive = additive;
    d.shader_kind = world::ParticleShaderKind::kSoft;
    d.rgb_start = {1, 1, 1, 0.8f};
    d.rgb_mid = {1, 1, 1, 0.6f};
    d.rgb_end = {0.5f, 0.5f, 0.5f, 0.0f};
    return d;
  };
  lib.add_effect(make_eff("smoke", false, -1.0f, 1.0f));
  lib.add_effect(make_eff("dust1", false, -2.0f, 1.2f));
  lib.add_effect(make_eff("dirt1", false, 200.0f, 0.5f));
  lib.add_effect(make_eff("sparks1", true, 300.0f, 0.4f));
  lib.add_effect(make_eff("backfire", true, 0.0f, 0.12f));
  lib.add_effect(make_eff("bi_wood", false, 250.0f, 0.8f));
  lib.add_effect(make_eff("bi_dust_small", false, -0.5f, 0.8f));

  world::ParticleSystem ps;
  ps.set_library(&lib);

  // Trigger drift smoke + backfire pulse.
  world::VehicleVfxInput vin{};
  vin.car_pos = {0, 1, 0};
  vin.car_vel = {12, 0, 0};
  vin.exhaust_tips[0] = {0, 0.4f, -2.0f};
  vin.exhaust_tip_count = 1;
  vin.audio_backfire_pulses = 1;
  for (int i = 0; i < 4; ++i) {
    vin.wheels[size_t(i)].grounded = true;
    vin.wheels[size_t(i)].contact_pos = {float(i), 0, 0};
    vin.wheels[size_t(i)].slip_combined = 1.4f;
    vin.wheels[size_t(i)].wheel_speed_mps = 15.0f;
    vin.wheels[size_t(i)].surface.smoke_weight = 1.0f;
  }
  for (int k = 0; k < 10; ++k) {
    if (k == 8) vin.audio_backfire_pulses = 1;
    ps.update_vehicle(vin, 0.02f);
    vin.audio_backfire_pulses = 0;
    ps.step(0.02f);
  }
  std::vector<world::ParticleSprite> alpha, add;
  ps.build_sprites({0, 2, -6}, alpha, add);
  CHECK(!alpha.empty());
  CHECK(!add.empty());  // backfire flames spawned!

  // Verify back-to-front sorting of alpha sprites.
  for (size_t i = 1; i < alpha.size(); ++i) {
    auto d2 = [](const std::array<float, 3>& p) {
      return p[0] * p[0] + (p[1] - 2.0f) * (p[1] - 2.0f) + (p[2] + 6.0f) * (p[2] + 6.0f);
    };
    CHECK(d2(alpha[i - 1].pos) + 1e-3f >= d2(alpha[i].pos));
  }

  // Smashable burst spawns wood + dust.
  const size_t before = ps.live_count();
  ps.spawn_smashable_burst("co_woodenbarrier_001", {5, 1, 5}, {8, 2, 0});
  CHECK(ps.live_count() > before + 10);

  // Advance past max lifetime -> all particles expire.
  for (int k = 0; k < 40; ++k) ps.step(0.05f);
  CHECK(ps.live_count() == 0);
}

void test_skidmarks() {
  world::SkidMarkSystem skids;

  fh1::SurfaceType asphalt{};
  asphalt.skid_a = 200;
  asphalt.skid_texture_index = 0;
  asphalt.skid_min_intensity = 0.0f;

  fh1::SurfaceType dirt{};
  dirt.skid_r = dirt.skid_g = dirt.skid_b = 255;
  dirt.skid_a = 125;
  dirt.skid_texture_index = 1;
  dirt.skid_min_intensity = 0.4f;

  fh1::SurfaceType guardrail{};
  guardrail.skid_a = 0;

  // Rolling straight on asphalt (slip 0.1) leaves ZERO skidmarks.
  world::WheelVfxInput w{};
  w.grounded = true;
  w.wheel_speed_mps = 20.0f;
  w.slip_combined = 0.1f;
  w.surface = asphalt;
  for (int i = 0; i < 10; ++i) {
    w.contact_pos = {0, 0, float(i) * 0.5f};
    skids.update_wheel(0, w);
  }
  CHECK(skids.segment_count() == 0);

  // Sliding on asphalt (slip 1.2) leaves dark rubber marks (TextureIndex 0).
  w.slip_combined = 1.2f;
  for (int i = 0; i < 10; ++i) {
    w.contact_pos = {0, 0, float(i) * 0.5f};
    skids.update_wheel(0, w);
  }
  CHECK(skids.count_by_texture(0) == 9);
  CHECK(skids.count_by_texture(1) == 0);

  // Rolling straight on dirt (slip 0.05, MinIntensity 0.4) leaves continuous ruts (TextureIndex 1).
  w.surface = dirt;
  w.slip_combined = 0.05f;
  for (int i = 0; i < 8; ++i) {
    w.contact_pos = {2.0f, 0, float(i) * 0.5f};
    skids.update_wheel(1, w);
  }
  CHECK(skids.count_by_texture(1) == 7);

  // GuardRail (ColorAlpha 0) leaves zero marks.
  const size_t before_gr = skids.segment_count();
  w.surface = guardrail;
  w.slip_combined = 2.0f;
  for (int i = 0; i < 5; ++i) {
    w.contact_pos = {4.0f, 0, float(i) * 0.5f};
    skids.update_wheel(2, w);
  }
  CHECK(skids.segment_count() == before_gr);

  std::vector<world::SkidVertex> asph_tris, rut_tris;
  skids.build_geometry(asph_tris, rut_tris);
  CHECK(asph_tris.size() == 9 * 6);
  CHECK(rut_tris.size() == 7 * 6);

  // Ring buffer FIFO wrap stays bounded at kMaxSegments and fades oldest tail.
  w.surface = asphalt;
  w.slip_combined = 1.5f;
  for (size_t i = 0; i < world::SkidMarkSystem::kMaxSegments + 200; ++i) {
    w.contact_pos = {10.0f, 0, float(i) * 0.5f};
    skids.update_wheel(3, w);
  }
  CHECK(skids.segment_count() == world::SkidMarkSystem::kMaxSegments);
  skids.build_geometry(asph_tris, rut_tris);
  CHECK(!asph_tris.empty());
  CHECK(asph_tris.front().rgba[3] < asph_tris.back().rgba[3]);

  // Verify <ParticleSpawn> child burst fires automatically when parent particle expires.
  {
    world::ParticleLibrary clib;
    world::ParticleEffectDef rocket{};
    rocket.name = "fw_rocket";
    rocket.life = 0.25f;
    rocket.life_var = 0.0f;
    rocket.child_spawn = "fw_spark";
    rocket.quantity = 10.0f;
    clib.add_effect(rocket);
    world::ParticleEffectDef spark{};
    spark.name = "fw_spark";
    spark.life = 1.0f;
    spark.life_var = 0.0f;
    spark.quantity = 20.0f;
    clib.add_effect(spark);
    world::ParticleSystem csys;
    csys.set_library(&clib);
    csys.spawn_burst("fw_rocket", {0, 10, 0}, {0, 1, 0}, {0, 0, 0}, 1, 1.0f);
    CHECK(csys.live_count() == 1);
    for (int k = 0; k < 4; ++k) csys.step(0.08f);
    CHECK(csys.live_count() >= 3);  // rocket expired and spawned child fw_spark burst
  }

  skids.clear();
  CHECK(skids.segment_count() == 0);
}

void write_be16(std::vector<uint8_t>& buf, size_t off, uint16_t v) {
  buf[off + 0] = uint8_t((v >> 8) & 0xFFu);
  buf[off + 1] = uint8_t(v & 0xFFu);
}

void write_be32(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
  buf[off + 0] = uint8_t((v >> 24) & 0xFFu);
  buf[off + 1] = uint8_t((v >> 16) & 0xFFu);
  buf[off + 2] = uint8_t((v >> 8) & 0xFFu);
  buf[off + 3] = uint8_t(v & 0xFFu);
}

void write_bef32(std::vector<uint8_t>& buf, size_t off, float f) {
  uint32_t u = 0;
  std::memcpy(&u, &f, 4);
  write_be32(buf, off, u);
}

uint16_t encode_f16(float f) {
  uint32_t u = 0;
  std::memcpy(&u, &f, 4);
  const uint32_t sign = (u >> 16) & 0x8000u;
  const int32_t exp = int32_t((u >> 23) & 0xFFu) - 127 + 15;
  const uint32_t mant = u & 0x7FFFFFu;
  if (exp <= 0) return uint16_t(sign);
  if (exp >= 31) return uint16_t(sign | 0x7BFFu);
  return uint16_t(sign | (uint32_t(exp) << 10) | (mant >> 13));
}

void test_pgeo_parsing_and_loading() {
  // Build a synthetic clean-room kind-4 .pgeo container (1 submesh, 3 vertices, 3 indices = 1 triangle).
  const std::string obj_name = "ANIM_TestBalloon_001";
  const std::string tex_name = "TEST_Balloon_DIFF";
  const size_t name_len = obj_name.size() + 1;
  const size_t hdr_base = (0x60 + name_len + 15u) & ~size_t(15u);
  const size_t tex_table_off = hdr_base + 0x60;
  const size_t tex_str_off = tex_table_off + 4u;
  const size_t sub_off = (tex_str_off + tex_name.size() + 1u + 15u) & ~size_t(15u);
  const size_t draw_off = sub_off + 0x44;
  const size_t gr2_off = draw_off + 0x2c;
  const uint32_t gr2_size = 0x20;
  const size_t geom_off = gr2_off + gr2_size;
  const uint32_t idx_bytes = 3u * 2u;     // 3 uint16 indices
  const uint32_t vb_bytes = 3u * 12u;     // 3 vertices * 12-byte stride
  const uint32_t geom_size = idx_bytes + vb_bytes;
  const uint32_t total_size = uint32_t(geom_off + geom_size);

  std::vector<uint8_t> pgeo(total_size, 0);
  std::memcpy(pgeo.data(), "OEGP", 4);
  write_bef32(pgeo, 0x10, -8.0f);
  write_bef32(pgeo, 0x14, 0.0f);
  write_bef32(pgeo, 0x18, -8.0f);
  write_bef32(pgeo, 0x20, 8.0f);
  write_bef32(pgeo, 0x24, 24.0f);
  write_bef32(pgeo, 0x28, 8.0f);
  write_be32(pgeo, 0x30, 4u);  // kind 4 = animated object
  write_be32(pgeo, 0x3c, uint32_t(name_len));
  write_be32(pgeo, 0x48, total_size);
  write_be32(pgeo, 0x4c, gr2_size);
  write_be32(pgeo, 0x50, geom_size);
  std::memcpy(pgeo.data() + 0x60, obj_name.c_str(), name_len);

  // Texture count (1) and submesh count (1).
  write_be32(pgeo, hdr_base + 0x44, 1u);
  write_be32(pgeo, hdr_base + 0x50, 1u);
  write_be32(pgeo, tex_table_off, uint32_t(tex_name.size() + 1u));
  std::memcpy(pgeo.data() + tex_str_off, tex_name.c_str(), tex_name.size() + 1u);

  // Submesh descriptor: (nv=3, ni=3, flag=1).
  write_be32(pgeo, sub_off + 4, 3u);
  write_be32(pgeo, sub_off + 8, 3u);
  write_be32(pgeo, sub_off + 12, 1u);

  // Draw descriptor: ntris=1 at +20.
  write_be32(pgeo, draw_off + 20, 1u);

  // Geometry block: 3 indices (0, 1, 2) + 3 half-float vertices.
  write_be16(pgeo, geom_off + 0, 0u);
  write_be16(pgeo, geom_off + 2, 1u);
  write_be16(pgeo, geom_off + 4, 2u);
  const float verts[3][3] = {{-4.0f, 1.0f, 0.0f}, {4.0f, 1.0f, 0.0f}, {0.0f, 18.0f, 2.0f}};
  for (int i = 0; i < 3; ++i) {
    const size_t vp = geom_off + idx_bytes + size_t(i) * 12u;
    write_be16(pgeo, vp + 0, encode_f16(verts[i][0]));
    write_be16(pgeo, vp + 2, encode_f16(verts[i][1]));
    write_be16(pgeo, vp + 4, encode_f16(verts[i][2]));
  }

  world::AnimatedPgeoObject parsed;
  CHECK(world::parse_animated_pgeo(pgeo, parsed, true));
  CHECK(parsed.name == obj_name);
  CHECK(parsed.kind == 4u);
  CHECK(parsed.category == world::AnimatedPgeoCategory::kBalloon);
  CHECK(parsed.textures.size() == 1 && parsed.textures[0] == tex_name);
  CHECK(parsed.submeshes.size() == 1);
  CHECK(parsed.submeshes[0].vertices.size() == 3);
  CHECK(parsed.submeshes[0].indices.size() == 3);
  CHECK(std::fabs(parsed.submeshes[0].vertices[2].pos[1] - 18.0f) < 0.25f);

  // Verify 10-angle crowd impostor azimuth quantization covers all 10 bins cleanly.
  std::set<int> bins;
  for (int step = 0; step < 100; ++step) {
    const float az = float(step) * (3.14159265f / 100.0f);
    const int idx = world::select_impostor_angle(az, 10, false);
    CHECK(idx >= 0 && idx < 10);
    bins.insert(idx);
  }
  CHECK(bins.size() == 10);
}

void test_rally_world_separation_and_sharing() {
  // 1. Verify AnimatedWorldSystem ColoradoDirt separation: no Colorado Festival balloons/stage crowds.
  world::AnimatedWorldSystem primary;
  world::TrackPostConfig cfg;
  cfg.crowd_sprite_distance = 52.0f;
  cfg.crowd_dark[0] = 0.45f;
  primary.set_world({}, std::filesystem::path("Media/DLCZips/1600_pri_65/Media/Tracks/ColoradoDirt"), cfg);
  CHECK(primary.is_rally_world());
  CHECK(primary.balloons().empty());
  CHECK(primary.pgeo_placements().empty());
  CHECK(!primary.crowds().empty());
  CHECK(!primary.flags().empty());
  for (const auto& cl : primary.crowds()) {
    CHECK(!cl.stage_lit);  // Rally roadside spectators & marshals have no Festival stage floodlights
  }

  // 2. Verify shared AnimatedWorldSystem state between DriveApp and Showroom (zero duplicate init,
  //    real TrackPostConfig & live Rally/Colorado world switch preserved).
  world::AnimatedWorldSystem::set_shared(&primary);
  CHECK(world::AnimatedWorldSystem::shared() == &primary);
  world::AnimatedWorldSystem mirror;
  mirror.share_from(*world::AnimatedWorldSystem::shared());
  CHECK(mirror.is_rally_world());
  CHECK(std::fabs(mirror.post_config().crowd_sprite_distance - 52.0f) < 1e-4f);
  CHECK(mirror.balloons().empty());

  // Switch primary back to Colorado -> shared mirror sees Colorado balloons & festival crowds immediately.
  primary.set_world({}, std::filesystem::path("media/tracks/colorado"), cfg);
  CHECK(!mirror.is_rally_world());
  CHECK(!mirror.balloons().empty());
  world::AnimatedWorldSystem::set_shared(nullptr);

  // 3. Verify ParticleLibrary records Rally world context cleanly when world_dir is ColoradoDirt.
  world::ParticleLibrary rally_lib;
  rally_lib.load({}, std::filesystem::path("Media/DLCZips/1600_pri_65/Media/Tracks/ColoradoDirt"));
  CHECK(rally_lib.is_rally_world());
}

void test_particle_atlas_capacity_and_gradient() {
  // 1. Verify >64 particle textures receive unique monotonic atlas slots (supporting all 72+ .xds with zero truncation).
  world::ParticleSystem ps;
  std::set<uint16_t> assigned;
  for (int i = 0; i < 80; ++i) {
    const std::string tname = "particle_tex_" + std::to_string(i) + ".xds";
    const uint16_t slot = ps.resolve_texture_slot(tname);
    CHECK(slot == uint16_t(i + 1));
    assigned.insert(slot);
  }
  CHECK(assigned.size() == 80);
  CHECK(ps.used_textures().size() == 80);
  CHECK(ps.resolve_texture_slot("particle_tex_71.xds") == 72u);  // 72nd .xds never truncated at 63!

  // 2. Verify <Texture2>, <Gradient numGradientRows>, and .tga -> .xds normalization in particle XMLs.
  const std::string grad_xml = R"(
<Effect Version="14" budget="80" lowerbudget="40">
  <EmitterProperties type="SuperSpray">
    <Quantity value="20.0" var="0.0"/>
    <Velocity value="2.0" variation="0.0"/>
    <Life value="1.5" variation="0.0"/>
    <ParticleShaderType Version="3" value="_animated_rowonly_gradient"/>
    <Animation rate="1.0" numFramesX="4" numFramesY="2"/>
    <Gradient numGradientRows="10"/>
    <Size valueX="0.2" valueY="0.2" maxValueX="0.4" maxValueY="0.4"/>
  </EmitterProperties>
  <Render>
    <ParticleType value="BillboardedQuad" dynamic_lighting="1">
      <Texture value="GR_Confetti_DIFF.tga"/>
      <Texture2 value="GR_Confetti_GRAD.tga"/>
      <Material>
        <BlendType value="alpha"/>
        <RGBStart r="1.0" g="1.0" b="1.0" a="1.0"/>
        <RGBMid r="1.0" g="1.0" b="1.0" a="1.0"/>
        <RGBEnd r="1.0" g="1.0" b="1.0" a="0.0"/>
      </Material>
    </ParticleType>
  </Render>
</Effect>)";
  world::ParticleEffectDef gdef;
  CHECK(world::parse_particle_effect_xml(grad_xml, "ConfettiTest", gdef));
  CHECK(gdef.shader_kind == world::ParticleShaderKind::kAnimatedRowOnlyGradient);
  CHECK(gdef.num_gradient_rows == 10);
  CHECK(gdef.texture_name == "gr_confetti_diff.xds");
  CHECK(gdef.texture2_name == "gr_confetti_grad.xds");

  world::ParticleLibrary glib;
  glib.add_effect(gdef);
  world::ParticleSystem gps;
  gps.set_library(&glib);
  gps.spawn_burst("confettitest", {0, 2, 0}, {0, 1, 0}, {0, 0, 0}, 6, 1.0f);
  std::vector<world::ParticleSprite> alpha, add;
  gps.build_sprites({0, 2, -5}, alpha, add);
  CHECK(alpha.size() == 6);
  for (const auto& sp : alpha) {
    CHECK(sp.use_gradient);
    CHECK(sp.texture_slot > 0);
    CHECK(sp.texture2_slot > 0 && sp.texture2_slot != sp.texture_slot);
    CHECK(sp.gradient_row_v > 0.0f && sp.gradient_row_v < 1.0f);
  }
}

void test_world_flare_composite_and_bounds() {
  // 1. Ground activation flares (_Composite_Flare / _Composite_Flare_1..7) must expand to ground flare +
  //    handflare smoke (never aerial flare-gun trail smoke); _Composite_FlareGun expands to flaregun effects.
  const auto f5 = world::expand_composite_effect("_Composite_Flare_5");
  CHECK(f5.size() == 2);
  CHECK(f5[0] == "amb_ground_flare_5");
  CHECK(f5[1] == "amb_handflare_smoke");

  const auto fbase = world::expand_composite_effect("_Composite_Flare");
  CHECK(fbase.size() == 2);
  CHECK(fbase[0] == "amb_ground_flare");
  CHECK(fbase[1] == "amb_handflare_smoke");

  const auto fgun = world::expand_composite_effect("_Composite_FlareGun");
  CHECK(fgun.size() == 2);
  CHECK(fgun[0] == "amb_flaregun_redtrailsmoke");

  // 2. Texture path prefix normalization in particle XMLs.
  const std::string xml = R"(
<Effect Version="14" budget="120" lowerbudget="60">
  <EmitterProperties type="SuperSpray">
    <Quantity value="60.0" var="0.0"/>
    <Velocity value="22.0" variation="0.0"/>
    <Life value="4.0" variation="0.0"/>
    <ParticleShaderType Version="3" value="_default"/>
    <Size valueX="6.0" valueY="6.0" maxValueX="14.0" maxValueY="14.0" growforX="0.5" growforY="0.5"/>
  </EmitterProperties>
  <Behavior>
    <Gravity strength="-45.0" decay="0.0"/>
  </Behavior>
  <Render>
    <ParticleType value="BillboardedQuad" dynamic_lighting="1" max_screen_size="0.80">
      <Texture value="Effects\GR_SmokeFlare_DIFF.tga"/>
      <Material>
        <BlendType value="alpha"/>
        <RGBStart r="1.0" g="1.0" b="1.0" a="0.9"/>
        <RGBMid r="1.0" g="1.0" b="1.0" a="0.9"/>
        <RGBEnd r="1.0" g="1.0" b="1.0" a="0.0"/>
      </Material>
    </ParticleType>
  </Render>
</Effect>)";
  world::ParticleEffectDef def;
  CHECK(world::parse_particle_effect_xml(xml, "amb_handflare_smoke", def));
  CHECK(def.texture_name == "gr_smokeflare_diff.xds");

  // 3. Emit via update_world_emitters: verify bounded size, bounded upward buoyancy, and soft depth fade.
  std::vector<world::WorldEmitter> wems;
  CHECK(world::parse_particle_emitters_xml(R"(
<ParticleEmitters>
  <SimpleEmitter>
    <Name value="fx_activationzone_test_1"/>
    <Effect value="_Composite_Flare_5"/>
    <Type value="Superspray"/>
    <Position v.x="10.0" v.y="5.0" v.z="-20.0"/>
    <Orientation>
      <XAxis v.x="1" v.y="0" v.z="0"/>
      <YAxis v.x="0" v.y="1" v.z="0"/>
      <ZAxis v.x="0" v.y="0" v.z="1"/>
    </Orientation>
  </SimpleEmitter>
</ParticleEmitters>)", wems));
  CHECK(wems.size() == 1);

  world::ParticleLibrary lib;
  lib.add_effect(def);
  world::ParticleSystem ps;
  ps.set_library(&lib);
  // Spawn directly via emit_continuous / step check on negative gravity buoyancy scaling:
  ps.spawn_burst("amb_handflare_smoke", {10.0f, 5.0f, 20.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 4, 1.0f);
  for (int s = 0; s < 20; ++s) ps.step(0.05f);
  std::vector<world::ParticleSprite> alpha, add;
  ps.build_sprites({10.0f, 6.0f, 10.0f}, alpha, add);
  CHECK(!alpha.empty());
  // With clamped negative gravity (<= 2.2 m/s^2 instead of 24.75 m/s^2), vertical velocity stays reasonable.
  for (const auto& sp : alpha) {
    CHECK(sp.vel[1] < 26.0f);
  }
}

void test_corpus_counts() {
  const char* env_root = std::getenv("SHIFT_GAME_ROOT");
  if (!env_root || !*env_root) return;
  const std::filesystem::path root(env_root);
  const auto media = root / "media";
  const auto colorado = media / "tracks" / "colorado";
  if (!std::filesystem::exists(media / "effects.zip")) return;

  world::ParticleLibrary lib;
  std::string err;
  CHECK(lib.load(media, colorado, &err));
  CHECK(lib.effects().size() == 260);  // 261 XMLs in effects.zip minus DefaultTrack.xml
  CHECK(lib.xds_count() == 72);
  CHECK(lib.world_emitters().size() == 2367);
  CHECK(lib.smashable_effects().size() == 15);
  CHECK(lib.backfire_config().loaded);

  // Spot-check core effects and decode game .xds textures.
  CHECK(lib.find_effect("smoke") != nullptr);
  CHECK(lib.find_effect("dust1") != nullptr);
  CHECK(lib.find_effect("dirt1") != nullptr);
  CHECK(lib.find_effect("gravelbits") != nullptr);
  CHECK(lib.find_effect("sparks1") != nullptr);
  CHECK(lib.find_effect("backfire") != nullptr);
  fh1::RgbaImage smoke_tex, backfire_tex;
  CHECK(lib.decode_texture("GR_Puffy_Smoke_DIFF.xds", smoke_tex));
  CHECK(smoke_tex.width > 0 && smoke_tex.height > 0);
  CHECK(lib.decode_texture("GR_Backfire_DIFF.xds", backfire_tex));
  CHECK(backfire_tex.width > 0 && backfire_tex.height > 0);

  // Verify surfaceTypes.xml counts from SPEC-VFX-AND-PARTICLES.
  auto pzip = fh1::Archive::open(media / "physics.zip", &err);
  CHECK(pzip != nullptr);
  if (pzip) {
    auto res = pzip->read("surfaceTypes.xml");
    CHECK(res.ok());
    std::vector<fh1::SurfaceType> surfs;
    CHECK(fh1::parse_surface_types(
        std::string_view(reinterpret_cast<const char*>(res.data.data()), res.data.size()), surfs, &err));
    CHECK(surfs.size() == 68);
    int tex0 = 0, tex1 = 0, spark = 0;
    for (const auto& s : surfs) {
      if (s.defined) {
        if (s.skid_texture_index == 0) ++tex0;
        else if (s.skid_texture_index == 1) ++tex1;
        if (s.should_spark) ++spark;
      }
    }
    CHECK(tex0 >= 45 && tex1 >= 8 && spark >= 28);
    std::printf("corpus vfx: %zu effects, %zu xds, %zu emitters, %zu surfaces (tex0=%d tex1=%d spark=%d)\n",
                lib.effects().size(), lib.xds_count(), lib.world_emitters().size(), surfs.size(), tex0, tex1, spark);
  }

  // Verify all 72 retail .xds particle textures receive unique slots > 64 without truncation.
  {
    world::ParticleSystem ps;
    for (const auto& xname : lib.xds_names()) {
      CHECK(ps.resolve_texture_slot(xname) > 0);
    }
    CHECK(ps.used_textures().size() == 72);
  }

  // Verify animatedobjects.zip .pgeo decoding, sitting spectators, .anim.bin clips, and impostors.
  if (std::filesystem::exists(media / "Spectators.zip") && std::filesystem::exists(media / "animatedobjects.zip")) {
    world::AnimatedWorldSystem anim;
    CHECK(anim.init(media, world::TrackPostConfig{}, &err, colorado));
    const auto& cat = anim.catalog();
    CHECK(cat.animated_pgeos == 290);
    CHECK(!cat.pgeo_objects.empty());
    CHECK(!anim.pgeo_placements().empty());
    CHECK(!cat.sit_assets.empty());
    CHECK(!cat.anim_clips.empty());
    CHECK(cat.has_impostor_sprites);
  }
  // Verify world flare emitters stay bounded in billboard size & upward velocity (PT-15).
  {
    world::ParticleSystem wps;
    wps.set_library(&lib);
    // Focus directly on the Horizon Festival activation zone flare (`bug_005`).
    const std::array<float, 3> focus{-355.6f, 4.1f, -193.9f};
    for (int s = 0; s < 40; ++s) {
      wps.update_world_emitters(focus, (s < 20) ? 7.25f : 22.5f, 0.05f);
      wps.step(0.05f);
    }
    std::vector<world::ParticleSprite> wa, wadd;
    wps.build_sprites({focus[0], focus[1] + 2.0f, focus[2] - 12.0f}, wa, wadd);
    CHECK(!wa.empty() || !wadd.empty());
    for (const auto& sp : wa) {
      CHECK(sp.size_x <= 4.05f && sp.size_y <= 4.05f);
      CHECK(sp.vel[1] <= 3.25f);
      CHECK(sp.soft_depth_m > 0.0f);
    }
    for (const auto& sp : wadd) {
      CHECK(sp.size_x <= 4.05f && sp.size_y <= 4.05f);
    }
  }
}

void test_r3_multi_vehicle_skids_quad_exhaust_and_glass_shatter() {
  // 1. Multi-vehicle AI skidmark slots (kMaxSkidVehicles = 8), smooth strip start alpha (0 -> target),
  //    and tight 0.008 m road lift.
  {
    CHECK(world::SkidMarkSystem::kMaxSkidVehicles == 8u);
    CHECK(std::fabs(world::SkidMarkSystem::kSkidLiftM - 0.008f) < 1e-6f);

    world::SkidMarkSystem skids;
    fh1::SurfaceType asphalt{};
    asphalt.skid_a = 200;
    asphalt.skid_texture_index = 0;
    asphalt.skid_min_intensity = 0.0f;

    fh1::SurfaceType dirt{};
    dirt.skid_r = dirt.skid_g = dirt.skid_b = 255;
    dirt.skid_a = 140;
    dirt.skid_texture_index = 1;
    dirt.skid_min_intensity = 0.45f;

    world::WheelVfxInput w{};
    w.grounded = true;
    w.surface_normal = {0.0f, 1.0f, 0.0f};
    w.wheel_right = {1.0f, 0.0f, 0.0f};
    w.wheel_forward = {0.0f, 0.0f, 1.0f};
    w.wheel_speed_mps = 18.0f;
    w.slip_combined = 1.35f;

    // Player (slot 0) on asphalt + 7 AI cars (slots 1..7) on alternating asphalt & dirt trails.
    for (int step = 0; step < 4; ++step) {
      for (size_t veh = 0; veh < world::SkidMarkSystem::kMaxSkidVehicles; ++veh) {
        w.surface = (veh % 2 == 0) ? asphalt : dirt;
        w.contact_pos = {float(veh) * 4.0f, 0.0f, float(step) * 0.5f};
        skids.update_vehicle_wheel(veh, 0, w, float(step) * 0.1f);
      }
    }
    // 8 vehicles * 3 emitted segments each = 24 segments (12 asphalt + 12 dirt ruts).
    CHECK(skids.segment_count() == 24u);
    CHECK(skids.count_by_texture(0) == 12u);
    CHECK(skids.count_by_texture(1) == 12u);

    std::vector<world::SkidVertex> asph_tris, rut_tris;
    skids.build_geometry(asph_tris, rut_tris);
    CHECK(asph_tris.size() == 12u * 6u);
    CHECK(rut_tris.size() == 12u * 6u);

    // First segment of the strip must ramp smoothly from alpha == 0 on leading edge (v0, v1)
    // to target alpha > 0 on trailing edge (v2, v3), with exact 0.008 m Y lift.
    CHECK(asph_tris[0].rgba[3] == 0u);  // v0 = prevL
    CHECK(asph_tris[1].rgba[3] == 0u);  // v1 = prevR
    CHECK(asph_tris[2].rgba[3] > 100u); // v2 = currR
    CHECK(asph_tris[5].rgba[3] > 100u); // v3 = currL
    for (const auto& v : asph_tris) {
      CHECK(std::fabs(v.pos[1] - 0.008f) < 1e-5f);
    }
    for (const auto& v : rut_tris) {
      CHECK(std::fabs(v.pos[1] - 0.008f) < 1e-5f);
    }

    // Breaking AI vehicle slot 3 must not disrupt player slot 0's continuous strip.
    skids.break_vehicle(3);
    w.surface = asphalt;
    w.contact_pos = {0.0f, 0.0f, 2.0f};
    skids.update_vehicle_wheel(0, 0, w, 0.4f);
    CHECK(skids.segment_count() == 25u);  // player slot 0 immediately appended next segment
    w.surface = dirt;
    w.contact_pos = {12.0f, 0.0f, 2.0f};
    skids.update_vehicle_wheel(3, 0, w, 0.4f);
    CHECK(skids.segment_count() == 25u);  // broken slot 3 only anchored new strip start (0 new segs)
  }

  // 2. Quad-tip (4-tip) exhaust backfires + dark exhaust puff + isolated AI vehicle dust/smoke.
  {
    world::ParticleLibrary lib;
    auto make_eff = [](const char* name, bool additive, float life) {
      world::ParticleEffectDef d;
      d.name = name;
      d.budget = 120;
      d.quantity = 80.0f;
      d.life = life;
      d.velocity = 2.0f;
      d.additive = additive;
      d.rgb_start = {1, 0.9f, 0.6f, 0.9f};
      d.rgb_mid = {1, 0.6f, 0.2f, 0.7f};
      d.rgb_end = {0.2f, 0.1f, 0.0f, 0.0f};
      return d;
    };
    lib.add_effect(make_eff("backfire", true, 0.25f));
    lib.add_effect(make_eff("smokecollision", false, 0.50f));
    lib.add_effect(make_eff("dust1", false, 0.80f));
    lib.add_effect(make_eff("smoke", false, 0.80f));

    world::ParticleSystem ps;
    ps.set_library(&lib);

    world::VehicleVfxInput vin{};
    vin.car_pos = {0, 0.6f, 0};
    vin.car_vel = {0, 0, 20.0f};
    vin.car_forward = {0, 0, 1};
    vin.exhaust_tip_count = 4;
    vin.exhaust_tips[0] = {-0.65f, 0.30f, -2.1f};
    vin.exhaust_tips[1] = {-0.45f, 0.30f, -2.1f};
    vin.exhaust_tips[2] = {0.45f, 0.30f, -2.1f};
    vin.exhaust_tips[3] = {0.65f, 0.30f, -2.1f};
    vin.audio_backfire_pulses = 1;
    ps.update_vehicle(vin, 0.016f);

    std::vector<world::ParticleSprite> alpha, add;
    ps.build_sprites({0, 1.5f, -6.0f}, alpha, add);
    // 4 tips * 2 backfire particles = 8 additive flames, plus 4 dark exhaust puff alpha particles!
    CHECK(add.size() == 8u);
    CHECK(alpha.size() == 4u);
    bool saw_tip0 = false, saw_tip1 = false, saw_tip2 = false, saw_tip3 = false;
    for (const auto& sp : add) {
      if (std::fabs(sp.pos[0] - (-0.65f)) < 0.02f) saw_tip0 = true;
      if (std::fabs(sp.pos[0] - (-0.45f)) < 0.02f) saw_tip1 = true;
      if (std::fabs(sp.pos[0] - 0.45f) < 0.02f) saw_tip2 = true;
      if (std::fabs(sp.pos[0] - 0.65f) < 0.02f) saw_tip3 = true;
    }
    CHECK(saw_tip0 && saw_tip1 && saw_tip2 && saw_tip3);

    // AI vehicle on slot 2 kicks up off-road dirt dust without touching slot 0's accumulators.
    world::VehicleVfxInput ai{};
    ai.vehicle_slot = 2;
    ai.car_pos = {20.0f, 0.5f, 10.0f};
    ai.car_vel = {0, 0, 18.0f};
    for (int wi = 0; wi < 4; ++wi) {
      ai.wheels[size_t(wi)].grounded = true;
      ai.wheels[size_t(wi)].contact_pos = {20.0f + float(wi) * 0.5f, 0.0f, 10.0f};
      ai.wheels[size_t(wi)].wheel_speed_mps = 18.0f;
      ai.wheels[size_t(wi)].slip_combined = 0.6f;
      ai.wheels[size_t(wi)].surface.dust_weight = 1.0f;
      ai.wheels[size_t(wi)].surface.skid_min_intensity = 0.5f;
    }
    const size_t before_ai = ps.live_count();
    for (int k = 0; k < 5; ++k) ps.update_vehicle(ai, 0.02f);
    CHECK(ps.live_count() >= before_ai + 8u);
  }

  // 3. Glass shatter particle burst (translucent shards + sparkling glints + expiry).
  {
    world::ParticleLibrary lib;
    world::ParticleEffectDef dust{};
    dust.name = "bi_dust_small";
    dust.life = 0.6f;
    dust.rgb_start = {0.9f, 0.9f, 0.9f, 0.5f};
    dust.rgb_mid = {0.8f, 0.8f, 0.8f, 0.3f};
    dust.rgb_end = {0.5f, 0.5f, 0.5f, 0.0f};
    lib.add_effect(dust);

    world::ParticleSystem ps;
    ps.set_library(&lib);
    ps.spawn_glass_shatter({0.0f, 1.1f, 0.5f}, {0.0f, 0.0f, 15.0f});
    CHECK(ps.live_count() >= 20u);

    std::vector<world::ParticleSprite> alpha, add;
    ps.build_sprites({0.0f, 1.6f, -5.0f}, alpha, add);
    CHECK(alpha.size() >= 16u);  // glass shards + fine dust
    CHECK(add.size() >= 6u);     // sparkling glass glints

    for (int k = 0; k < 35; ++k) ps.step(0.05f);
    CHECK(ps.live_count() == 0u);
  }
}

}  // namespace

int main() {
  test_xml_parsers();
  test_particle_simulation();
  test_skidmarks();
  test_pgeo_parsing_and_loading();
  test_rally_world_separation_and_sharing();
  test_particle_atlas_capacity_and_gradient();
  test_world_flare_composite_and_bounds();
  test_r3_multi_vehicle_skids_quad_exhaust_and_glass_shatter();
  test_corpus_counts();
  if (g_failures == 0) {
    std::printf("vfx_tests: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "vfx_tests: %d failure(s)\n", g_failures);
  return 1;
}
