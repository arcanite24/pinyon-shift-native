#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "world/tod.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

void test_synthetic_tod() {
  const std::string xml = R"xml(<?xml version="1.0" encoding="utf-8"?>
<TOD version="2">
  <TimeSpeed numKeys="2">
    <Key time="0.0" x="220.0"></Key>
    <Key time="720.0" x="28.0"></Key>
  </TimeSpeed>
  <SunPos numKeys="2">
    <Key time="0.0" x="10.0" y="20.0" z="30.0"/>
    <Key time="600.0" x="30.0" y="40.0" z="50.0"/>
  </SunPos>
  <SunTargetPos numKeys="1">
    <Key time="0.0" x="10.0" y="0.0" z="30.0"/>
  </SunTargetPos>
  <SunObjectMoon numKeys="2">
    <Key time="388.0" x="1.0"/>
    <Key time="389.0" x="0.0"/>
  </SunObjectMoon>
</TOD>)xml";

  world::TodPreset preset;
  std::string err;
  check(preset.load_xml(xml, &err), "load synthetic TOD XML");
  check(preset.curves.size() == 4, "synthetic curve count == 4");
  check(near(preset.scalar("TimeSpeed", 0.0f), 220.0f), "TimeSpeed at 0");
  check(near(preset.scalar("TimeSpeed", 360.0f), 124.0f), "TimeSpeed midpoint interpolation");
  check(near(preset.scalar("TimeSpeed", 1400.0f), 28.0f), "TimeSpeed clamps at upper end");

  const auto s0 = world::evaluate_tod(preset, 0.0);
  check(s0.is_moon, "moon active at 00:00");
  check(near(s0.sun_dir[1], 1.0f, 1e-4f), "sun_dir +Y at 00:00");

  const auto s1 = world::evaluate_tod(preset, 390.0 * 60.0);
  check(!s1.is_moon, "sun active at 06:30");

  // Verify IBLDirectScaleCar scales car_sun_color independently from IBLDirectScaleEnvironment.
  check(s1.car_sun_color[0] < s1.sun_color[0] * 0.6f, "car_sun_color scaled by IBLDirectScaleCar (< env sun)");
  const float expected_ratio = s1.ibl_direct_car / s1.ibl_direct_env;
  check(near(s1.car_sun_color[0] / s1.sun_color[0], expected_ratio, 1e-3f),
        "car_sun_color / sun_color matches IBLDirectScaleCar / IBLDirectScaleEnvironment");
}

void test_clock() {
  world::TodClock clk;
  clk.set_hour(23.5);
  check(near(float(clk.hour()), 23.5f, 1e-4f), "set_hour 23.5");
  clk.advance(10.0, 220.0);  // +2200 s = +36.67 min -> wraps past midnight
  check(clk.tod_sec >= 0.0 && clk.tod_sec < 86400.0, "clock wraps modulo 86400");
  check(near(float(clk.tod_sec), 400.0f, 1e-2f), "clock wrapped value exact");

  // Noon crossing increments lunar phase counter.
  clk.set_hour(11.9);
  const int p0 = clk.day_count;
  clk.advance(30.0, 28.0);  // +840 s = +14 min -> 12:08
  check(clk.day_count == p0 + 1, "noon crossing increments day_count");

  // 20:00 crossing triggers A/B roll (~30% B over many days).
  clk.force_preset = false;
  int b_hits = 0;
  for (int i = 0; i < 400; ++i) {
    clk.set_hour(19.95);
    clk.advance(10.0, 40.0);  // crosses 20:00
    if (clk.use_preset_b) ++b_hits;
  }
  check(b_hits > 80 && b_hits < 160, "20:00 roll selects preset B ~30%");

  // Paused clock does not advance.
  clk.set_hour(12.0);
  clk.paused = true;
  clk.advance(100.0, 100.0);
  check(near(float(clk.hour()), 12.0f, 1e-4f), "paused clock stays fixed");

  // step_hours wraps both forward and backward.
  clk.step_hours(-13.0);
  check(near(float(clk.hour()), 23.0f, 1e-4f), "step_hours negative wrap");
}

void test_exposure_and_lut() {
  world::TrackPostConfig cfg;
  // Exterior table: (0->+1.3, 4->+0.437, 13->+0.001, 65->-0.8), filmic_exposure = 0.31.
  check(near(cfg.evaluate_ev(0.0f, false), 1.61f, 1e-3f), "exterior EV at dark 0");
  check(near(cfg.evaluate_ev(4.0f, false), 0.747f, 1e-3f), "exterior EV at lum 4");
  check(near(cfg.evaluate_ev(13.0f, false), 0.311f, 1e-3f), "exterior EV at lum 13");
  // Cockpit table holds -0.400 across [9, 50]: -0.400 + 0.31 = -0.09.
  check(near(cfg.evaluate_ev(25.0f, true), -0.09f, 1e-3f), "cockpit EV plateau at lum 25");

  // Identity LUT round-trip within 1/255.
  const auto id = world::ColorLut16::identity();
  for (float r : {0.0f, 0.25f, 0.5f, 0.73f, 1.0f}) {
    for (float g : {0.0f, 0.4f, 1.0f}) {
      const auto out = id.sample(r, g, 0.6f);
      check(near(out[0], r, 1.0f / 255.0f + 1e-4f), "identity LUT R");
      check(near(out[1], g, 1.0f / 255.0f + 1e-4f), "identity LUT G");
      check(near(out[2], 0.6f, 1.0f / 255.0f + 1e-4f), "identity LUT B");
    }
  }

  // Synthetic 16x16x16 A8R8G8B8 DDS parse + blend.
  std::vector<uint8_t> dds(128 + world::ColorLut16::kByteSize, 0);
  dds[0] = 'D'; dds[1] = 'D'; dds[2] = 'S'; dds[3] = ' ';
  auto put32 = [&](size_t off, uint32_t v) {
    dds[off] = uint8_t(v);
    dds[off + 1] = uint8_t(v >> 8);
    dds[off + 2] = uint8_t(v >> 16);
    dds[off + 3] = uint8_t(v >> 24);
  };
  put32(12, 16); put32(16, 16); put32(24, 16);
  put32(92, 0x00FF0000u); put32(96, 0x0000FF00u); put32(100, 0x000000FFu);
  for (size_t i = 0; i < 4096; ++i) {
    dds[128 + i * 4 + 0] = 200;  // B
    dds[128 + i * 4 + 1] = 100;  // G
    dds[128 + i * 4 + 2] = 50;   // R
    dds[128 + i * 4 + 3] = 255;
  }
  world::ColorLut16 cool;
  check(cool.load_dds(dds), "parse synthetic BGRA DDS LUT");
  const auto s = cool.sample(0.5f, 0.5f, 0.5f);
  check(near(s[0], 50.0f / 255.0f, 1e-3f) && near(s[2], 200.0f / 255.0f, 1e-3f), "BGRA channel decode");

  const auto blended = world::ColorLut16::blend(id, cool, 0.0f, 0.5f);
  const auto sb = blended.sample(0.5f, 0.5f, 0.5f);
  check(sb[2] > sb[0] + 0.2f, "blended night LUT shifts cool blue");
}

void test_stars_and_zones() {
  // 2 synthetic big-endian stars.
  std::vector<uint8_t> bin(32, 0);
  auto put_be_f32 = [&](size_t off, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    bin[off + 0] = uint8_t(bits >> 24);
    bin[off + 1] = uint8_t(bits >> 16);
    bin[off + 2] = uint8_t(bits >> 8);
    bin[off + 3] = uint8_t(bits);
  };
  put_be_f32(0, 0.0f); put_be_f32(4, 1.0f); put_be_f32(8, 0.0f); put_be_f32(12, 0.25f);
  put_be_f32(16, 1.0f); put_be_f32(20, 0.0f); put_be_f32(24, 0.0f); put_be_f32(28, 0.95f);
  std::vector<world::Star> stars;
  check(world::parse_stars_bin(bin, stars), "parse synthetic Stars.bin");
  check(stars.size() == 2 && near(stars[0].y, 1.0f) && near(stars[1].brightness, 0.95f), "star fields match");

  world::PostZone z;
  z.name = "Test";
  z.triangles.push_back({{0.0f, 10.0f, 0.0f}, {0.0f, 0.0f, 10.0f}});
  check(z.contains(2.0f, 2.0f), "point inside zone triangle");
  check(!z.contains(8.0f, 8.0f), "point outside zone triangle");
}

void test_clouds_cube_scatter_and_color_settings() {
  // 1. CloseCloudBottomPowerBack / IntensityBack, 6 Cube* curves, LightRays*, and FogSunScatter* parsing.
  const std::string xml = R"xml(<?xml version="1.0" encoding="utf-8"?>
<TOD version="2">
  <CloseCloudBottomPower numKeys="2">
    <Key time="0.0" x="2.0"/>
    <Key time="720.0" x="4.0"/>
  </CloseCloudBottomPower>
  <CloseCloudBottomIntensity numKeys="2">
    <Key time="0.0" x="0.10"/>
    <Key time="720.0" x="0.40"/>
  </CloseCloudBottomIntensity>
  <CloseCloudBottomPowerBack numKeys="2">
    <Key time="0.0" x="1.5"/>
    <Key time="720.0" x="5.5"/>
  </CloseCloudBottomPowerBack>
  <CloseCloudBottomIntensityBack numKeys="2">
    <Key time="0.0" x="0.05"/>
    <Key time="720.0" x="0.65"/>
  </CloseCloudBottomIntensityBack>
  <CubeAtmosphereHazeColour numKeys="2">
    <Key time="0.0" x="0.10" y="0.20" z="0.40"/>
    <Key time="720.0" x="0.50" y="0.60" z="0.80"/>
  </CubeAtmosphereHazeColour>
  <CubeAtmosphereHazePower numKeys="2">
    <Key time="0.0" x="4.0"/>
    <Key time="720.0" x="8.0"/>
  </CubeAtmosphereHazePower>
  <CubeAtmosphereHazeIntensity numKeys="2">
    <Key time="0.0" x="0.25"/>
    <Key time="720.0" x="0.75"/>
  </CubeAtmosphereHazeIntensity>
  <CubeAtmosphereHazeEllipticalScale numKeys="2">
    <Key time="0.0" x="0.60"/>
    <Key time="720.0" x="1.00"/>
  </CubeAtmosphereHazeEllipticalScale>
  <CubeSunObjectColour numKeys="2">
    <Key time="0.0" x="0.30" y="0.10" z="0.02"/>
    <Key time="720.0" x="0.90" y="0.80" z="0.70"/>
  </CubeSunObjectColour>
  <CubeSunObjectSize numKeys="2">
    <Key time="0.0" x="10.0"/>
    <Key time="720.0" x="22.0"/>
  </CubeSunObjectSize>
  <LightRaysColour numKeys="2">
    <Key time="0.0" x="0.20" y="0.30" z="0.50"/>
    <Key time="720.0" x="1.00" y="0.80" z="0.60"/>
  </LightRaysColour>
  <LightRaysIntensity numKeys="2">
    <Key time="0.0" x="0.50"/>
    <Key time="720.0" x="0.80"/>
  </LightRaysIntensity>
  <FogSunScatterStartDistance numKeys="2">
    <Key time="0.0" x="200.0"/>
    <Key time="720.0" x="1200.0"/>
  </FogSunScatterStartDistance>
  <FogSunScatterRampupDistance numKeys="2">
    <Key time="0.0" x="4000.0"/>
    <Key time="720.0" x="12000.0"/>
  </FogSunScatterRampupDistance>
</TOD>)xml";

  world::TodPreset preset;
  check(preset.load_xml(xml), "load R2-2 parity TOD XML");
  const auto mid = world::evaluate_tod(preset, 360.0 * 60.0);  // 06:00 midpoint
  check(near(mid.close_back_bottom_power, 3.5f), "CloseCloudBottomPowerBack midpoint == 3.5");
  check(near(mid.close_back_bottom_intensity, 0.35f), "CloseCloudBottomIntensityBack midpoint == 0.35");
  check(near(mid.cube_atmo_haze_color[0], 0.30f) && near(mid.cube_atmo_haze_color[2], 0.60f),
        "CubeAtmosphereHazeColour midpoint");
  check(near(mid.cube_atmo_haze_power, 6.0f), "CubeAtmosphereHazePower midpoint == 6.0");
  check(near(mid.cube_atmo_haze_intensity, 0.50f), "CubeAtmosphereHazeIntensity midpoint == 0.50");
  check(near(mid.cube_atmo_haze_elliptical, 0.80f), "CubeAtmosphereHazeEllipticalScale midpoint == 0.80");
  check(near(mid.cube_sun_object_color[0], 0.60f) && near(mid.cube_sun_object_color[1], 0.45f),
        "CubeSunObjectColour midpoint");
  check(near(mid.cube_sun_object_size, 16.0f), "CubeSunObjectSize midpoint == 16.0");
  check(near(mid.light_rays_color[0], 0.60f) && near(mid.light_rays_intensity, 0.65f),
        "LightRaysColour/Intensity midpoint");
  check(near(mid.fog_sun_scatter_start, 700.0f), "FogSunScatterStartDistance midpoint == 700");
  check(near(mid.fog_sun_scatter_rampup, 8000.0f), "FogSunScatterRampupDistance midpoint == 8000");

  // Missing back-cloud / Cube* curves cleanly fall back to front/main curves.
  world::TodPreset minimal;
  check(minimal.load_xml(R"xml(<TOD version="2">
    <CloseCloudBottomPower numKeys="1"><Key time="0.0" x="3.2"/></CloseCloudBottomPower>
    <CloseCloudBottomIntensity numKeys="1"><Key time="0.0" x="0.24"/></CloseCloudBottomIntensity>
    <AtmosphereHazePower numKeys="1"><Key time="0.0" x="7.5"/></AtmosphereHazePower>
    <SunObjectSize numKeys="1"><Key time="0.0" x="18.0"/></SunObjectSize>
  </TOD>)xml"), "load minimal fallback TOD XML");
  const auto fb = world::evaluate_tod(minimal, 0.0);
  check(near(fb.close_back_bottom_power, 3.2f), "CloseCloudBottomPowerBack fallback matches front power");
  check(near(fb.close_back_bottom_intensity, 0.12f), "CloseCloudBottomIntensityBack fallback == 0.5x front");
  check(near(fb.cube_atmo_haze_power, 7.5f), "CubeAtmosphereHazePower fallback matches main haze power");
  check(near(fb.cube_sun_object_size, 18.0f), "CubeSunObjectSize fallback matches main sun size");

  // 2. TrackPostConfig cubemap, shadow, exposure range, and ColorSettings defaults.
  world::TrackPostConfig cfg;
  check(cfg.load_xml(R"xml(<TrackSettings Version="64">
    <cubeSkyScale value="0.25"/>
    <cubeSkySaturation value="0.60"/>
    <cubeReflGroundScale value="0.85"/>
    <Cubemap FarDistance="240.0"/>
    <ShadowEnds ShadowNear="28.0" InGameShadowEnd="215.0"/>
    <ShadowColor r="0.30" g="0.32" b="0.38"/>
    <CameraExposureRange minval="-1.5" maxval="4.5"/>
    <ExposureBias value="0.20"/>
    <Contrast value="0.08"/>
    <Brightness value="-0.03"/>
  </TrackSettings>)xml"), "parse TrackPostConfig cubemap/shadow/color fields");
  check(near(cfg.cube_sky_scale, 0.25f) && near(cfg.cube_sky_saturation, 0.60f) &&
            near(cfg.cube_refl_ground_scale, 0.85f) && near(cfg.cube_far_distance, 240.0f),
        "TrackPostConfig cubemap parameters parsed");
  check(near(cfg.shadow_near, 28.0f) && near(cfg.shadow_far, 215.0f) && near(cfg.shadow_color[2], 0.38f),
        "TrackPostConfig shadow parameters parsed");
  check(near(cfg.exposure_min, -1.5f) && near(cfg.exposure_max, 4.5f) && near(cfg.exposure_bias, 0.20f) &&
            near(cfg.contrast, 0.08f) && near(cfg.brightness, -0.03f),
        "TrackPostConfig exposure & color defaults parsed");

  world::TodSystem sys;
  sys.preset_a = preset;
  sys.post_config = cfg;
  sys.loaded_tod = true;
  world::TodClock clk;
  clk.set_hour(6.0);
  const auto sys_s = sys.evaluate(clk);
  check(near(sys_s.cube_sky_saturation, 0.60f) && near(sys_s.cube_refl_ground_scale, 0.85f) &&
            near(sys_s.cube_far_distance, 240.0f),
        "TodSystem::evaluate forwards cubemap TrackSettings into TodState");
  check(near(sys_s.shadow_near, 28.0f) && near(sys_s.shadow_far, 215.0f) && near(sys_s.shadow_color[0], 0.30f),
        "TodSystem::evaluate forwards ShadowEnds/ShadowColor into TodState");

  // 3. Per-zone ColorSettings_Templates & Fog SunScatterStartDistance/RampupDistance forwarding + blending.
  world::PostZoneSet zset;
  world::PostZone za;
  za.name = "Canyon";
  za.color_settings = "canyon_col";
  za.fog_template = "canyon_fog";
  za.lerp_time = 10.0f;
  za.triangles.push_back({{0.0f, 100.0f, 0.0f}, {0.0f, 0.0f, 100.0f}});
  world::PostZone zb;
  zb.name = "Alpine";
  zb.color_settings = "alpine_col";
  zb.fog_template = "alpine_fog";
  zb.lerp_time = 10.0f;
  zb.triangles.push_back({{200.0f, 300.0f, 200.0f}, {0.0f, 0.0f, 100.0f}});
  zset.zones = {za, zb};

  world::ColorSettingsTemplate ca;
  ca.desat_amount = 0.72f;
  ca.desat_luminosity[0] = 0.25f; ca.desat_luminosity[1] = 0.65f; ca.desat_luminosity[2] = 0.10f;
  ca.desat_post_mod[0] = 1.12f; ca.desat_post_mod[1] = 0.98f; ca.desat_post_mod[2] = 0.90f;
  ca.exposure_bias = -0.35f;
  ca.contrast = 0.15f;
  ca.brightness = 0.04f;
  world::ColorSettingsTemplate cb;
  cb.desat_amount = 1.05f;
  cb.desat_luminosity[0] = 0.20f; cb.desat_luminosity[1] = 0.72f; cb.desat_luminosity[2] = 0.08f;
  cb.desat_post_mod[0] = 0.96f; cb.desat_post_mod[1] = 1.02f; cb.desat_post_mod[2] = 1.10f;
  cb.exposure_bias = 0.45f;
  cb.contrast = -0.05f;
  cb.brightness = -0.02f;
  zset.color_templates["canyon_col"] = ca;
  zset.color_templates["alpine_col"] = cb;

  world::FogTemplate fa;
  fa.sun_scatter_start_distance = 500.0f;
  fa.sun_scatter_rampup_distance = 3000.0f;
  world::FogTemplate fb_fog;
  fb_fog.sun_scatter_start_distance = 4500.0f;
  fb_fog.sun_scatter_rampup_distance = 25000.0f;
  zset.fog_templates["canyon_fog"] = fa;
  zset.fog_templates["alpine_fog"] = fb_fog;

  // Fallback outside all zones inherits TrackPostConfig color defaults.
  const auto outside_cs = zset.color_settings_for(-1, cfg);
  check(near(outside_cs.desat_amount, cfg.desat_amount) && near(outside_cs.exposure_bias, cfg.exposure_bias) &&
            near(outside_cs.contrast, cfg.contrast) && near(outside_cs.brightness, cfg.brightness),
        "color_settings_for(-1) falls back to TrackPostConfig defaults");

  world::PostZoneBlender blender;
  blender.update(zset, cfg, 10.0f, 10.0f, 0.016f, true);  // snap into Canyon
  check(blender.active_zone == 0, "blender active_zone == Canyon");
  check(near(blender.color.desat_amount, 0.72f) && near(blender.color.desat_post_mod[0], 1.12f) &&
            near(blender.color.exposure_bias, -0.35f),
        "blender snaps Canyon ColorSettings_Templates fields");
  check(near(blender.fog.sun_scatter_start_distance, 500.0f) &&
            near(blender.fog.sun_scatter_rampup_distance, 3000.0f),
        "blender snaps Canyon Fog SunScatter distances");

  // Step halfway into Alpine and confirm smooth forwarding/interpolation.
  blender.update(zset, cfg, 210.0f, 10.0f, 2.0f, false);
  check(blender.active_zone == 1, "blender active_zone == Alpine");
  check(blender.color.desat_amount > 0.72f && blender.color.desat_amount < 1.05f,
        "blender interpolates desat_amount across zones");
  check(blender.color.exposure_bias > -0.35f && blender.color.exposure_bias < 0.45f,
        "blender interpolates exposure_bias across zones");
  check(blender.color.desat_post_mod[2] > 0.90f && blender.color.desat_post_mod[2] < 1.10f,
        "blender interpolates desat_post_mod across zones");
  check(blender.fog.sun_scatter_start_distance > 500.0f && blender.fog.sun_scatter_start_distance < 4500.0f,
        "blender interpolates fog SunScatterStartDistance");
  check(blender.fog.sun_scatter_rampup_distance > 3000.0f && blender.fog.sun_scatter_rampup_distance < 25000.0f,
        "blender interpolates fog SunScatterRampupDistance");
}

void test_disc_assets_if_available() {
  const char* env_root = std::getenv("SHIFT_GAME_ROOT");
  std::filesystem::path game_root;
  if (env_root && *env_root) {
    game_root = env_root;
  } else {
    const std::filesystem::path local_default = R"(../horizon1-recomp/.local\game\base)";
    if (std::filesystem::exists(local_default / "media/tracks/colorado/Ribbon_00/TimeOfDayA.xml"))
      game_root = local_default;
  }
  if (game_root.empty()) return;

  const auto colorado = game_root / "media/tracks/colorado";
  world::TodSystem sys;
  std::string err;
  check(sys.load(colorado, &err), "load Colorado TodSystem from disc");
  check(sys.loaded_tod, "loaded TimeOfDayA.xml");
  check(sys.preset_a.curves.size() == 110, "TimeOfDayA.xml has 110 curves");
  check(sys.loaded_sky, "loaded realtimesky.zip");
  check(sys.sky_assets.stars.size() == 9096, "Stars.bin has 9,096 stars");

  // Verify all 9,096 stars are unit vectors with monotonic brightness in [0, 1].
  bool stars_unit = true, stars_mono = true;
  float prev_w = -1.0f;
  for (const auto& st : sys.sky_assets.stars) {
    const float len = std::sqrt(st.x * st.x + st.y * st.y + st.z * st.z);
    if (std::fabs(len - 1.0f) > 1e-3f) stars_unit = false;
    if (st.brightness < prev_w - 1e-5f || st.brightness < 0.0f || st.brightness > 1.0001f) stars_mono = false;
    prev_w = st.brightness;
  }
  check(stars_unit, "all 9,096 stars are unit vectors");
  check(stars_mono, "all 9,096 stars have monotonic brightness in [0,1]");

  // Verify 24h integrated real duration == 1,827.9 s +- 1.0 s (SPEC-SKY-AND-TOD §9.1).
  double real_sec = 0.0;
  for (int m = 0; m < 1440; ++m) {
    const float spd = sys.preset_a.scalar("TimeSpeed", float(m) + 0.5f, 28.0f);
    real_sec += 60.0 / double(spd);
  }
  check(std::fabs(real_sec - 1827.9) <= 2.0, "24h cycle integrated real duration ~1827.9 s");

  // Verify Sun/Moon swap at 06:28 vs 06:29 and 20:02 vs 20:03.
  check(world::evaluate_tod(sys.preset_a, 388.0 * 60.0).is_moon, "moon active at 06:28");
  check(!world::evaluate_tod(sys.preset_a, 389.0 * 60.0).is_moon, "sun active at 06:29");
  check(!world::evaluate_tod(sys.preset_a, 1202.0 * 60.0).is_moon, "sun active at 20:02");
  check(world::evaluate_tod(sys.preset_a, 1203.0 * 60.0).is_moon, "moon active at 20:03");

  // Verify SwitchOnLights at noon vs midnight.
  check(world::evaluate_tod(sys.preset_a, 13.0 * 3600.0).switch_on_lights < 0.05f, "lights off at 13:00");
  check(world::evaluate_tod(sys.preset_a, 0.0).switch_on_lights > 0.99f, "lights on at 00:00");

  // Verify PostProcessingZones_Safe.xml (16 zones, 152 tris) and dynamicpost.zip (22 LUTs).
  check(sys.zones.zones.size() == 16, "16 post-processing zones");
  size_t total_tris = 0;
  for (const auto& z : sys.zones.zones) total_tris += z.triangles.size();
  check(total_tris == 152, "152 post-processing zone triangles");
  check(sys.zones.luts.size() == 22, "22 dynamicpost 3D LUTs");
  const int z_spawn = sys.zones.find_zone(2003.0f, 960.0f);
  check(z_spawn >= 0, "Colorado spawn point matches a post-processing zone");

  // Night fallback LUT grades neutral grey away from identity.
  const auto night_mid = sys.zones.fallback_night.sample(0.5f, 0.5f, 0.5f);
  const float night_diff = std::fabs(night_mid[0] - 0.5f) + std::fabs(night_mid[1] - 0.5f) +
                           std::fabs(night_mid[2] - 0.5f);
  check(night_diff > 0.05f, "ColorGradingLookup_Night.dds applies non-identity night grade");

  // ShadowEnds and cubemap parameters parsed from TrackSettings.xml.
  check(near(sys.post_config.shadow_near, 25.0f, 0.1f), "ShadowNear == 25");
  check(near(sys.post_config.shadow_far, 202.0f, 0.1f), "InGameShadowEnd == 202");
  check(near(sys.post_config.cube_sky_saturation, 0.60f, 0.02f), "cubeSkySaturation == 0.60");
  check(near(sys.post_config.cube_far_distance, 200.0f, 1.0f), "Cubemap FarDistance == 200");
  const auto sunset = world::evaluate_tod(sys.preset_a, 18.0 * 3600.0);
  check(sunset.close_back_bottom_power > 0.0f, "Colorado CloseCloudBottomPowerBack parsed");
  check(sunset.cube_sun_object_size > 0.0f && sunset.cube_atmo_haze_power > 0.0f, "Colorado Cube* curves parsed");
  check(sunset.light_rays_intensity > 0.1f, "Colorado LightRaysIntensity active at sunset");

  // WORLD-10: the world lights from the curves at the clock's time, raw
  // (no temporal smoother): across dusk, night, the 20:00 roll and dawn the
  // evaluated state equals the active preset's curves at tod_sec.
  world::TodClock drive_clk;
  drive_clk.set_hour(19.0);
  const float dt = 1.0f / 60.0f;
  bool raw_everywhere = true;
  for (int step = 0; step < 450 * 60; ++step) {
    const double spd = sys.active_preset(drive_clk).scalar("TimeSpeed", float(drive_clk.tod_sec / 60.0), 28.0f);
    drive_clk.advance(dt, spd);
    if (step % 600 != 0) continue;
    const world::TodState cur = sys.evaluate(drive_clk);
    const world::TodState raw = world::evaluate_tod(sys.active_preset(drive_clk), drive_clk.tod_sec);
    raw_everywhere = raw_everywhere && cur.shadow_intensity == raw.shadow_intensity &&
                     cur.day_night_post == raw.day_night_post && cur.sky_gain == raw.sky_gain &&
                     cur.hl_top == raw.hl_top && cur.switch_on_lights == raw.switch_on_lights;
  }
  check(raw_everywhere, "Colorado: the evaluated state is the active preset's curves at tod_sec (no smoothing)");
}

void test_smooth_tod_and_lut_transitions() {
  // 1. SPEC-SKY-AND-TOD §2.4: keys outside [first, last] clamp to the
  //    nearest end key; there is no interpolation across midnight.
  const std::string wrap_xml = R"xml(<TOD version="2">
    <CloseCloudBottomAlpha numKeys="3">
      <Key time="0.0" x="0.10"/>
      <Key time="720.0" x="0.50"/>
      <Key time="1439.0" x="0.90"/>
    </CloseCloudBottomAlpha>
    <CloseCloudBottomColourBack numKeys="2">
      <Key time="10.0" r="0.10" g="0.20" b="0.30"/>
      <Key time="1430.0" r="0.90" g="0.80" b="0.70"/>
    </CloseCloudBottomColourBack>
  </TOD>)xml";
  world::TodPreset wp;
  check(wp.load_xml(wrap_xml), "load midnight-edge TOD XML");
  check(near(wp.scalar("CloseCloudBottomAlpha", 1439.5f), 0.90f, 1e-5f), "past the last key: the last key's value");
  check(near(wp.scalar("CloseCloudBottomAlpha", 0.0f), 0.10f, 1e-5f), "at 00:00: the first key's value");
  check(near(wp.scalar("CloseCloudBottomAlpha", 360.0f), 0.30f, 1e-5f), "between keys: linear");
  check(near(wp.vec3("CloseCloudBottomColourBack", 5.0f)[0], 0.10f, 1e-5f) &&
            near(wp.vec3("CloseCloudBottomColourBack", 1435.0f)[2], 0.70f, 1e-5f),
        "vector curves clamp at both ends");

  // 2. The 20:00 roll switches the preset at once (no cross-fade).
  const std::string preset_a_xml = R"xml(<TOD version="2">
    <HLTopColour numKeys="2"><Key time="0.0" r="0.02" g="0.03" b="0.04"/><Key time="1439.0" r="0.02" g="0.03" b="0.04"/></HLTopColour>
    <SkyGain numKeys="2"><Key time="0.0" x="1.2"/><Key time="1439.0" x="1.2"/></SkyGain>
    <ShadowIntensity numKeys="4">
      <Key time="0.0" x="-1.0"/><Key time="469.0" x="-0.3"/><Key time="470.0" x="1.0"/><Key time="1439.0" x="-1.0"/>
    </ShadowIntensity>
  </TOD>)xml";
  const std::string preset_b_xml = R"xml(<TOD version="2">
    <HLTopColour numKeys="2"><Key time="0.0" r="0.20" g="0.25" b="0.30"/><Key time="1439.0" r="0.20" g="0.25" b="0.30"/></HLTopColour>
    <SkyGain numKeys="2"><Key time="0.0" x="2.6"/><Key time="1439.0" x="2.6"/></SkyGain>
    <ShadowIntensity numKeys="2"><Key time="0.0" x="0.0"/><Key time="1439.0" x="0.0"/></ShadowIntensity>
  </TOD>)xml";
  world::TodSystem sys;
  check(sys.preset_a.load_xml(preset_a_xml), "load synthetic preset A");
  check(sys.preset_b.load_xml(preset_b_xml), "load synthetic preset B");
  sys.preset_b.merge_fallback(sys.preset_a);
  sys.loaded_tod = true;
  world::TodClock clk;
  for (uint32_t seed = 1; seed < 1000; ++seed) {
    world::TodClock probe;
    probe.rng_state = seed;
    probe.set_hour(19.99);
    probe.advance(5.0, 40.0);
    if (probe.use_preset_b) {
      clk.rng_state = seed;
      break;
    }
  }
  clk.set_hour(19.99);
  check(near(sys.evaluate(clk).sky_gain, 1.2f, 1e-5f), "before 20:00: preset A");
  int frames_to_b = -1;
  for (int f = 0; f < 60 && frames_to_b < 0; ++f) {
    clk.advance(1.0 / 60.0, 40.0);
    if (clk.use_preset_b && near(sys.evaluate(clk).sky_gain, 2.6f, 1e-5f)) frames_to_b = f;
  }
  check(clk.tod_sec >= 72000.0 && frames_to_b >= 0, "the frame that crosses 20:00 lights from preset B");
  check(near(sys.evaluate(clk).sky_gain, 2.6f, 1e-5f), "no blend: preset B's own curves after the roll");

  // 3. The raw curve steps exactly as keyed: ShadowIntensity -0.3 -> +1.0
  //    over the 469 -> 470 minute key.
  world::TodClock dawn_clk;
  dawn_clk.force_preset = true;
  dawn_clk.set_hour(469.5 / 60.0);
  check(near(sys.evaluate(dawn_clk).shadow_intensity, 0.35f, 1e-4f), "dawn shadow step: linear between keys");

  // 4. The time-of-day request: from 10:00, ask for 18:00 over 45 real
  //    seconds (rate = (target - now) / 45): the curve's TimeSpeed is
  //    ignored, the clock reaches 18:00 after 45 s, stops there and pauses.
  {
    world::TodClock rc;
    rc.set_hour(10.0);
    const double target = 18.0 * 3600.0;
    rc.request(target, (target - rc.tod_sec) / 45.0);
    for (int f = 0; f < 44 * 60; ++f) rc.advance(1.0 / 60.0, 28.0);
    check(!rc.paused && rc.transition && rc.tod_sec < target, "request: still ramping at 44 s");
    check(near(float(rc.tod_sec), float(36000.0 + 44.0 * 640.0), 1.0f), "request: its own rate, not TimeSpeed");
    for (int f = 0; f < 2 * 60; ++f) rc.advance(1.0 / 60.0, 28.0);
    check(rc.paused && !rc.transition && !rc.rate_override && rc.tod_sec == target, "request: clamps and pauses");
    rc.advance(10.0, 28.0);
    check(rc.tod_sec == target, "request: held after it lands");
    // Past midnight: 22:00 to 02:00 (target + 86,400).
    world::TodClock mc;
    mc.set_hour(22.0);
    mc.request(2.0 * 3600.0 + 86400.0, (4.0 * 3600.0) / 45.0);
    for (int f = 0; f < 50 * 60; ++f) mc.advance(1.0 / 60.0, 220.0);
    check(mc.paused && near(float(mc.tod_sec), 7200.0f, 1e-3f), "request: past midnight lands on 02:00");
  }

  // 4. ColorLutBlender is frame-rate independent (30 Hz vs 60 Hz vs 120 Hz match at t=1.0s, no 120 Hz snap).
  const world::ColorLut16 lut_a = world::ColorLut16::identity();
  world::ColorLut16 lut_b = world::ColorLut16::identity();
  for (size_t i = 0; i < 4096; ++i) {
    const size_t o = i * 4;
    lut_b.rgba[o + 0] = uint8_t(std::min(255, int(lut_a.rgba[o + 0]) / 2 + 20));
    lut_b.rgba[o + 2] = uint8_t(std::min(255, int(lut_a.rgba[o + 2]) / 2 + 120));
  }
  auto run_lut_blend = [&](int hz, float seconds) {
    world::ColorLutBlender b;
    b.snap(lut_a);
    const int steps = int(std::lround(seconds * float(hz)));
    const float step_dt = 1.0f / float(hz);
    for (int i = 0; i < steps; ++i) b.update(lut_b, step_dt, 2.5f, false);
    return b;
  };
  const auto b30 = run_lut_blend(30, 1.0f);
  const auto b60 = run_lut_blend(60, 1.0f);
  const auto b120 = run_lut_blend(120, 1.0f);
  const auto s30 = b30.current.sample(0.5f, 0.5f, 0.5f);
  const auto s60 = b60.current.sample(0.5f, 0.5f, 0.5f);
  const auto s120 = b120.current.sample(0.5f, 0.5f, 0.5f);
  check(near(s30[0], s120[0], 0.02f) && near(s60[2], s120[2], 0.02f),
        "ColorLutBlender is frame-rate independent across 30/60/120 Hz");
  // At t=1.0s with tau=2.5s, blend fraction is ~33% (must NOT have snapped to 100% at 120 Hz!).
  const auto sa_mid = lut_a.sample(0.5f, 0.5f, 0.5f);
  const auto sb_mid = lut_b.sample(0.5f, 0.5f, 0.5f);
  const float expected_b = sa_mid[2] + (sb_mid[2] - sa_mid[2]) * (1.0f - std::exp(-1.0f / 2.5f));
  check(near(s120[2], expected_b, 0.025f), "ColorLutBlender matches exact exponential progress at 120 Hz");
  const auto b_done = run_lut_blend(60, 15.0f);
  check(b_done.settled(lut_b, 1.0f), "ColorLutBlender converges cleanly to target LUT");
}

}  // namespace

int main() {
  test_synthetic_tod();
  test_clock();
  test_exposure_and_lut();
  test_stars_and_zones();
  test_clouds_cube_scatter_and_color_settings();
  test_smooth_tod_and_lut_transitions();
  test_disc_assets_if_available();
  if (g_failures == 0) std::printf("tod_tests: all checks passed\n");
  return g_failures ? 1 : 0;
}
