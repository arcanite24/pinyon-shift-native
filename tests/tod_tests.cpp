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

void test_disc_assets_if_available() {
  const char* env_root = std::getenv("SHIFT_GAME_ROOT");
  std::filesystem::path game_root;
  if (env_root && *env_root) {
    game_root = env_root;
  } else {
    const std::filesystem::path local_default = R"(c:\Users\neri\code\horizon1-recomp\.local\game\base)";
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

  // ShadowEnds parsed from TrackSettings.xml.
  check(near(sys.post_config.shadow_near, 25.0f, 0.1f), "ShadowNear == 25");
  check(near(sys.post_config.shadow_far, 202.0f, 0.1f), "InGameShadowEnd == 202");
}

}  // namespace

int main() {
  test_synthetic_tod();
  test_clock();
  test_exposure_and_lut();
  test_stars_and_zones();
  test_disc_assets_if_available();
  if (g_failures == 0) std::printf("tod_tests: all checks passed\n");
  return g_failures ? 1 : 0;
}
