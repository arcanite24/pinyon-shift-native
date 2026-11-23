#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "drive/lamps.h"
#include "fh1data/archive.h"
#include "fh1data/carbin.h"
#include "fh1data/xds.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

void test_lamp_state_rules() {
  // 1. Brake vs handbrake (CAR_LAMPS.md §2.1 & §9.2.2).
  drive::CarLampState s;
  drive::LampInputs in{};
  in.tod_sec = 13.0 * 3600.0;  // midday
  in.brake_cmd = 0.8f;
  in.brake_pedal = 0.8f;
  s.update(in);
  check(s.brake_on && s.vectors().dynamic_lights_amount[0] == 1.0f, "service brake activates brake lamps");

  // Brake command without pedal (e.g. handbrake only) must NOT activate brake lamps.
  in.brake_cmd = 0.8f;
  in.brake_pedal = 0.0f;
  s.update(in);
  check(!s.brake_on && s.vectors().dynamic_lights_amount[0] == 0.0f, "handbrake alone does not light brake lamps");

  // AI 15-tick anti-flicker hold latch.
  drive::CarLampState ai;
  in.is_ai = true;
  in.brake_cmd = 0.5f;
  in.brake_pedal = 0.5f;
  ai.update(in);
  check(ai.brake_on, "AI brake activates immediately");
  in.brake_cmd = 0.0f;
  in.brake_pedal = 0.0f;
  for (int i = 0; i < 15; ++i) {
    ai.update(in);
    check(ai.brake_on, "AI brake holds for 15 ticks");
  }
  ai.update(in);
  check(!ai.brake_on, "AI brake releases after 15-tick hold");

  // 2. Reverse gear == 0 only.
  in.is_ai = false;
  in.gear = 0;
  s.update(in);
  check(s.reverse_on && s.vectors().dynamic_lights_amount[1] == 1.0f, "reverse active in gear 0");
  in.gear = 1;
  s.update(in);
  check(!s.reverse_on && s.vectors().dynamic_lights_amount[1] == 0.0f, "reverse off in gear 1");

  // 3. Indicator 0.36 s on / 0.33 s off cadence & persistent active flags.
  in.indicator_mode = 1;
  in.dt = 0.20f;
  s.update(in);
  check(s.ind_left == 1.0f && s.ind_right == 0.0f && s.left_ind_active && !s.right_ind_active,
        "left indicator on phase");
  in.dt = 0.20f;  // 0.40s > 0.36s -> off phase, but left_ind_active stays true!
  s.update(in);
  check(s.ind_left == 0.0f && s.left_ind_active, "left indicator off half keeps left_active=1");
  in.indicator_mode = 0;
  s.update(in);
  check(!s.left_ind_active && s.ind_timer == 0.0f, "indicator mode 0 resets timer and active flags");

  // 4. TOD headlamp window [08:10, 18:50] exact boundaries (29400 s and 67800 s).
  check(drive::CarLampState::tod_requests_headlamps(8.0 * 3600.0 + 9.0 * 60.0), "headlamps on at 08:09");
  check(!drive::CarLampState::tod_requests_headlamps(8.0 * 3600.0 + 11.0 * 60.0), "headlamps off at 08:11");
  check(!drive::CarLampState::tod_requests_headlamps(18.0 * 3600.0 + 49.0 * 60.0), "headlamps off at 18:49");
  check(drive::CarLampState::tod_requests_headlamps(18.0 * 3600.0 + 51.0 * 60.0), "headlamps on at 18:51");

  // Pop-up suppression.
  in.tod_sec = 0.0;
  in.popup_open = 0.5f;
  s.update(in);
  check(!s.headlamps_on, "pop-up lamps suppressed while opening");
  in.popup_open = 1.0f;
  s.update(in);
  check(s.headlamps_on && s.vectors().dynamic_lights_3[2] == 1.0f, "headlamps + side lights on when open");
}

void test_technique_classification_and_emission() {
  // Passive covers and reflectors MUST NEVER be classified emissive (CAR_LAMPS.md §3.2 & §9.1).
  for (const char* passive : {"detail_glass_red", "detail_glass_clear", "detail_glass_amber",
                              "lights_glass", "lights_gls_noemit", "red_glass",
                              "head_light", "head_light2", "reflector", "textured_reflector"}) {
    check(drive::classify_lamp_technique(passive) == drive::LampChannel::kNone,
          "passive glass/reflector technique must be kNone");
  }

  // Expanded 176-car lamp & emissive gauge techniques must classify to active channels (never kNone).
  check(drive::classify_lamp_technique("fogred") == drive::LampChannel::kFogRed, "fogred -> kFogRed");
  check(drive::classify_lamp_technique("fogwhite") == drive::LampChannel::kFogWhite, "fogwhite -> kFogWhite");
  check(drive::classify_lamp_technique("slinlorange") == drive::LampChannel::kSideIndLeft, "slinlorange");
  check(drive::classify_lamp_technique("slinrorange") == drive::LampChannel::kSideIndRight, "slinrorange");
  check(drive::classify_lamp_technique("lindtailside") == drive::LampChannel::kTailBrakeIndLeft, "lindtailside");
  check(drive::classify_lamp_technique("rindtailside") == drive::LampChannel::kTailBrakeIndRight, "rindtailside");
  check(drive::classify_lamp_technique("drlred") == drive::LampChannel::kDrlRed, "drlred");
  check(drive::classify_lamp_technique("linred") == drive::LampChannel::kIndicatorLeft, "linred");
  check(drive::classify_lamp_technique("rinred") == drive::LampChannel::kIndicatorRight, "rinred");
  check(drive::classify_lamp_technique("brkreverse") == drive::LampChannel::kBrakeReverse, "brkreverse");
  check(drive::classify_lamp_technique("z_gauge_emissive_alpha") == drive::LampChannel::kGaugeBacklight,
        "z_gauge_emissive_alpha -> kGaugeBacklight");
  check(drive::classify_lamp_technique("gauge_emissive_alpha") == drive::LampChannel::kGaugeBacklight,
        "gauge_emissive_alpha -> kGaugeBacklight");

  drive::LampColorTable cols;
  drive::CarLampState st;
  drive::LampInputs in{};
  in.tod_sec = 13.0 * 3600.0;  // day
  st.update(in);
  auto v_day = st.vectors();
  // DRLs on during day; drlwhiteswitch on during day, off at night.
  check(drive::evaluate_lamp_emission(drive::LampChannel::kDrlWhite, v_day, cols)[0] > 0.5f, "drlwhite on by day");
  check(drive::evaluate_lamp_emission(drive::LampChannel::kDrlWhiteSwitch, v_day, cols)[0] > 0.5f,
        "drlwhiteswitch on by day");
  check(drive::evaluate_lamp_emission(drive::LampChannel::kHeadXenon, v_day, cols)[0] == 0.0f, "xenon off by day");
  check(drive::evaluate_lamp_emission(drive::LampChannel::kGaugeBacklight, v_day, cols)[0] == 0.0f,
        "gauge backlight off by day");

  in.tod_sec = 22.0 * 3600.0;  // night
  st.update(in);
  auto v_night = st.vectors();
  check(drive::evaluate_lamp_emission(drive::LampChannel::kDrlWhite, v_night, cols)[0] > 0.5f, "drlwhite on at night");
  check(drive::evaluate_lamp_emission(drive::LampChannel::kDrlWhiteSwitch, v_night, cols)[0] == 0.0f,
        "drlwhiteswitch turns off when headlamps are on");
  check(drive::evaluate_lamp_emission(drive::LampChannel::kHeadXenon, v_night, cols)[2] > 1.0f, "xenon on at night");
  check(drive::evaluate_lamp_emission(drive::LampChannel::kNumPlate, v_night, cols)[0] > 0.5f, "numplate on at night");
  check(drive::evaluate_lamp_emission(drive::LampChannel::kGaugeBacklight, v_night, cols)[0] > 1.0f,
        "gauge backlight on at night");
}

void test_cockpit_animation_math() {
  drive::CockpitAnimState anim;
  for (int i = 0; i < 60; ++i) anim.update_steering(0.35f, 10.0f, 1.0f / 60.0f);
  check(anim.wheel_rad > 1.0f && anim.wheel_rad < 4.72f, "steering wheel rotates smoothly to road wheel angle");

  // Gauge monotonic sweep 0..1.
  float prev = -1.0f;
  for (int i = 0; i <= 10; ++i) {
    anim.update_gauges(float(i) * 8.0f, 80.0f, float(i) * 800.0f, 8000.0f);
    check(anim.speed_norm >= prev && anim.tach_norm >= prev, "gauge sweep monotonic");
    prev = anim.speed_norm;
  }
  check(std::fabs(anim.speed_norm - 1.0f) < 1e-4f && std::fabs(anim.tach_norm - 1.0f) < 1e-4f,
        "gauge hits 1.0 at max");
}

void test_disc_lamp_points_if_available() {
  const char* env_root = std::getenv("SHIFT_GAME_ROOT");
  std::filesystem::path game_root;
  if (env_root && *env_root) game_root = env_root;
  else {
    const std::filesystem::path local_default = R"(../horizon1-recomp/.local\game\base)";
    if (std::filesystem::exists(local_default / "media/cars/NIS_SkylineGTRVSpecII_02.zip")) game_root = local_default;
  }
  if (game_root.empty()) return;

  // Verify R34 exterior .carbin lamp points and headlight beam resolution.
  std::string err;
  auto arch = fh1::Archive::open(game_root / "media/cars/NIS_SkylineGTRVSpecII_02.zip", &err);
  check(arch != nullptr, "open R34 zip");
  if (!arch) return;
  auto rd = arch->read("NIS_SkylineGTRVSpecII_02.carbin");
  check(rd.ok(), "read R34 exterior .carbin");
  const auto pts = fh1::parse_carbin_lamp_points(rd.data);
  check(pts.size() >= 4, "R34 has >= 4 lamp points");
  bool masks_valid = true, tex_valid = true;
  int head_count = 0;
  for (const auto& p : pts) {
    if ((p.channel_mask & ~0x1Fu) != 0) masks_valid = false;
    if (p.texture_index > 7) tex_valid = false;
    if (p.channel_mask & 0x01u) ++head_count;
  }
  check(masks_valid, "all R34 lamp point channel masks within 0x01..0x10");
  check(tex_valid, "all R34 lamp point texture indices <= 7");
  check(head_count >= 2, "R34 has front headlamp points");
  const auto beam = drive::resolve_headlight_beam(pts, false);
  check(beam.valid && beam.origin_body[2] > 1.0f && beam.origin_body[1] > 0.3f,
        "R34 projected headlight beam origin sits at front nose (+Z, +Y)");

  // Verify showroom staticcarcubemap.xpr XPR2 untiling + LogLuv HDR decode (SHOWROOM_LIGHTING.md).
  const auto xpr_path = game_root / "media/tracks/uiautoshow/staticcarcubemap.xpr";
  if (std::filesystem::exists(xpr_path)) {
    std::vector<uint8_t> raw;
    if (FILE* f = std::fopen(xpr_path.string().c_str(), "rb")) {
      std::fseek(f, 0, SEEK_END);
      const long sz = std::ftell(f);
      if (sz > 0) {
        raw.resize(size_t(sz));
        std::fseek(f, 0, SEEK_SET);
        std::fread(raw.data(), 1, raw.size(), f);
      }
      std::fclose(f);
    }
    fh1::XprCubemap cube;
    std::string xerr;
    const bool ok = fh1::decode_xpr_cubemap(raw, cube, &xerr);
    check(ok && cube.size == 512, "decode staticcarcubemap.xpr 512x512x6");
    if (ok && cube.size == 512) {
      double sum[3] = {0, 0, 0}, wsum = 0;
      for (int face = 0; face < 6; ++face) {
        for (uint32_t y = 0; y < cube.size; y += 4) {
          const double v = (double(y) + 0.5) / cube.size * 2.0 - 1.0;
          for (uint32_t x = 0; x < cube.size; x += 4) {
            const double u = (double(x) + 0.5) / cube.size * 2.0 - 1.0;
            const double d2 = 1.0 + u * u + v * v;
            const double w = 1.0 / (d2 * std::sqrt(d2));
            const auto& px = cube.faces[size_t(face)][size_t(y) * cube.size + x];
            sum[0] += px[0] * w;
            sum[1] += px[1] * w;
            sum[2] += px[2] * w;
            wsum += w;
          }
        }
      }
      const double mr = sum[0] / wsum, mg = sum[1] / wsum, mb = sum[2] / wsum;
      // SHOWROOM_LIGHTING.md: solid-angle mean radiance ~(0.180, 0.166, 0.239).
      check(std::fabs(mr - 0.180) < 0.025 && std::fabs(mg - 0.166) < 0.025 && std::fabs(mb - 0.239) < 0.025,
            "staticcarcubemap.xpr solid-angle mean radiance matches SHOWROOM_LIGHTING.md");
    }
  }
}

}  // namespace

int main() {
  test_lamp_state_rules();
  test_technique_classification_and_emission();
  test_cockpit_animation_math();
  test_disc_lamp_points_if_available();
  if (g_failures == 0) std::printf("lamps_tests: all checks passed\n");
  return g_failures ? 1 : 0;
}
