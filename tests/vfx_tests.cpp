#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "fh1data/archive.h"
#include "fh1data/collision.h"
#include "world/vfx.h"

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

  skids.clear();
  CHECK(skids.segment_count() == 0);
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
}

}  // namespace

int main() {
  test_xml_parsers();
  test_particle_simulation();
  test_skidmarks();
  test_corpus_counts();
  if (g_failures == 0) {
    std::printf("vfx_tests: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "vfx_tests: %d failure(s)\n", g_failures);
  return 1;
}
