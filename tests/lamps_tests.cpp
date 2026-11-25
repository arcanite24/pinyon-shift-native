#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "drive/lamps.h"
#include "drive/vehicle.h"
#include "fh1data/archive.h"
#include "fh1data/carbin.h"
#include "fh1data/xds.h"
#define CARVIEW_ARENA_LRU_ONLY
#include "../apps/carview/showroom.h"

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

  // Phase R2-3A rare lamp techniques.
  check(drive::classify_lamp_technique("drlinlorange") == drive::LampChannel::kIndicatorLeft, "drlinlorange");
  check(drive::classify_lamp_technique("drlinrorange") == drive::LampChannel::kIndicatorRight, "drlinrorange");
  check(drive::classify_lamp_technique("drlinlwhite") == drive::LampChannel::kIndicatorLeft, "drlinlwhite");
  check(drive::classify_lamp_technique("drlinrwhite") == drive::LampChannel::kIndicatorRight, "drlinrwhite");
  check(drive::classify_lamp_technique("tail_light_on") == drive::LampChannel::kBrake, "tail_light_on");
  check(drive::classify_lamp_technique("headlightl_lod0") == drive::LampChannel::kHeadOld, "headlightl_lod0");
  check(drive::classify_lamp_technique("headlightr_lod0") == drive::LampChannel::kHeadOld, "headlightr_lod0");
  check(drive::classify_lamp_technique("taillightl_lod0") == drive::LampChannel::kTailBrake2S, "taillightl_lod0");
  check(drive::classify_lamp_technique("taillightr_lod0") == drive::LampChannel::kTailBrake2S, "taillightr_lod0");

  // GaugeEmissiveScale parsing in LampColorTable::load_shader_settings_xml.
  {
    drive::LampColorTable ct;
    ct.load_shader_settings_xml(R"(<ShaderSettings><gauge_emissive><GaugeEmissiveScale value="4.25"/></gauge_emissive></ShaderSettings>)");
    check(std::fabs(ct.gauge_emissive - 4.25f) < 1e-4f, "LampColorTable parses GaugeEmissiveScale");
  }

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

  // PT-23: Night cruising (brake == 0) must keep brake-only channels strictly off while
  // illuminating running tail/2S lens channels softly; braking vividly boosts both.
  const auto tail_cruise = drive::evaluate_lamp_emission(drive::LampChannel::kTailBrake2S, v_night, cols);
  const auto brake_cruise = drive::evaluate_lamp_emission(drive::LampChannel::kBrake, v_night, cols);
  check(brake_cruise[0] == 0.0f && brake_cruise[1] == 0.0f && brake_cruise[2] == 0.0f,
        "brake-only lamp channel off while cruising at night");
  check(tail_cruise[0] > 0.2f && tail_cruise[0] < 1.0f,
        "running tail light lens illuminates cleanly without overbright flare");

  in.brake_cmd = 1.0f;
  in.brake_pedal = 1.0f;
  st.update(in);
  const auto v_night_brake = st.vectors();
  const auto tail_brake = drive::evaluate_lamp_emission(drive::LampChannel::kTailBrake2S, v_night_brake, cols);
  const auto brake_on = drive::evaluate_lamp_emission(drive::LampChannel::kBrake, v_night_brake, cols);
  check(brake_on[0] >= 1.8f, "brake-only / third brake light glows vividly under braking");
  check(tail_brake[0] > tail_cruise[0] * 2.5f, "2S tail/brake intensifies vividly over running tail when braking");
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

  // Pillar 2: per-car dial max speed and live turbo boost gauge math.
  drive::CarParams hatchback{}, supercar{};
  hatchback.redline_rpm = 6500.0;
  hatchback.final_drive = 4.20;
  hatchback.gears = {3.5, 2.1, 1.4, 1.05, 0.85};
  hatchback.tyre_radius = {0.30, 0.30, 0.30, 0.30};
  supercar.redline_rpm = 8500.0;
  supercar.final_drive = 3.20;
  supercar.gears = {3.1, 2.2, 1.6, 1.2, 0.95, 0.72};
  supercar.tyre_radius = {0.34, 0.34, 0.34, 0.34};
  supercar.has_turbo = true;
  supercar.boost_max_psi = 32.7;  // +18.0 psi gauge boost
  const float dial_hatch = drive::CockpitAnimState::estimate_dial_max_speed_mps(hatchback);
  const float dial_super = drive::CockpitAnimState::estimate_dial_max_speed_mps(supercar);
  check(dial_super > dial_hatch + 15.0f, "per-car speedometer dial max scales with gearing/redline");
  const float bmax = drive::CockpitAnimState::gauge_max_boost_psi(supercar);
  check(std::fabs(bmax - 18.0f) < 0.05f, "turbo gauge max boost converts abs psi to gauge psi");
  anim.update_gauges(30.0f, dial_super, 2000.0f, 8500.0f,
                     drive::CockpitAnimState::gauge_boost_psi(3.675, true), bmax);
  const float boost_vac = anim.boost_norm;
  anim.update_gauges(60.0f, dial_super, 7000.0f, 8500.0f,
                     drive::CockpitAnimState::gauge_boost_psi(32.7, true), bmax);
  const float boost_full = anim.boost_norm;
  check(boost_vac < 0.25f && boost_full > 0.95f, "live turbo boost needle sweeps from vacuum to full boost");

  // Phase R2-3A: speed-dependent steering lock scaling (CAR_LAMPS.md §7.1 & §8).
  // Low speed (0 m/s, mult=12.0) yields ~2x cockpit wheel angle vs high speed (50 m/s, mult=6.0).
  {
    drive::CockpitAnimState low_v, high_v;
    for (int i = 0; i < 120; ++i) {
      low_v.update_steering(0.30f, 0.0f, 1.0f / 60.0f);
      high_v.update_steering(0.30f, 50.0f, 1.0f / 60.0f);
    }
    // 0.30 * 12 * 0.75 = 2.70 rad; 0.30 * 6 * 0.75 = 1.35 rad.
    check(std::fabs(low_v.wheel_rad - 2.70f) < 0.05f, "low-speed steering uses mult=12.0 * 0.75");
    check(std::fabs(high_v.wheel_rad - 1.35f) < 0.05f, "high-speed steering tapers to mult=6.0 * 0.75");
    check(low_v.wheel_rad > high_v.wheel_rad * 1.8f, "low-speed steering angle exceeds high-speed steering");
  }

  // Phase R2-3A: piecewise-linear <TickMark> calibration & power_norm gauge.
  {
    drive::CockpitAnimState cal_anim;
    const char* tick_xml = R"(
      <SpeedGauge>
        <TickMark pos="0.0" val="0.0"/>
        <TickMark pos="0.5" val="30.0"/>
        <TickMark pos="1.0" val="100.0"/>
      </SpeedGauge>)";
    cal_anim.calibration.speed.min_val = 0.0f;
    cal_anim.calibration.speed.max_val = 100.0f;
    check(drive::parse_gauge_tick_marks_xml(tick_xml, cal_anim.calibration.speed), "parse <TickMark> XML");
    cal_anim.calibration.has_speed = true;
    cal_anim.update_gauges(15.0f, 100.0f, 4000.0f, 8000.0f, 0.0f, 20.0f, 0.5f, 150000.0f, 300000.0f, 0.0f);
    // 15 m/s is halfway between 0 (pos 0.0) and 30 (pos 0.5) -> speed_norm == 0.25 (non-linear!).
    check(std::fabs(cal_anim.speed_norm - 0.25f) < 1e-4f, "piecewise <TickMark> speed gauge lower segment");
    check(std::fabs(cal_anim.power_norm - 0.50f) < 1e-4f, "power_norm evaluates linear power range");

    cal_anim.update_gauges(65.0f, 100.0f, 6000.0f, 8000.0f, 0.0f, 20.0f, 0.5f, 300000.0f, 300000.0f, 0.0f);
    // 65 m/s is halfway between 30 (pos 0.5) and 100 (pos 1.0) -> speed_norm == 0.75.
    check(std::fabs(cal_anim.speed_norm - 0.75f) < 1e-4f, "piecewise <TickMark> speed gauge upper segment");
    check(std::fabs(cal_anim.power_norm - 1.0f) < 1e-4f, "power_norm reaches 1.0 at max power");
  }
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

  // PT-23 disc check: Viper rear lamp points carry rear-facing 0x01 markers without 0x04,
  // while Corrado carries rear 0x01, 0x02, and dedicated 0x04 brake points.
  if (std::filesystem::exists(game_root / "media/cars/VIP_Viper_13.zip")) {
    auto varch = fh1::Archive::open(game_root / "media/cars/VIP_Viper_13.zip", &err);
    if (varch) {
      auto vrd = varch->read("VIP_Viper_13.carbin");
      if (vrd.ok()) {
        const auto vpts = fh1::parse_carbin_lamp_points(vrd.data);
        int rear_01 = 0, brake_04 = 0;
        for (const auto& p : vpts) {
          if ((p.channel_mask & 0x01u) && (p.position[2] < 0.0f || p.direction[2] < -0.2f)) ++rear_01;
          if (p.channel_mask & 0x04u) ++brake_04;
        }
        check(rear_01 >= 2 && brake_04 == 0, "Viper rear lamp points have rear-facing 0x01 without 0x04");
      }
    }
  }
  if (std::filesystem::exists(game_root / "media/cars/VW_Corrado_95.zip")) {
    auto carch = fh1::Archive::open(game_root / "media/cars/VW_Corrado_95.zip", &err);
    if (carch) {
      auto crd = carch->read("VW_Corrado_95.carbin");
      if (crd.ok()) {
        const auto cpts = fh1::parse_carbin_lamp_points(crd.data);
        int tail_02 = 0, brake_04 = 0;
        for (const auto& p : cpts) {
          if (p.channel_mask & 0x02u) ++tail_02;
          if (p.channel_mask & 0x04u) ++brake_04;
        }
        check(tail_02 >= 2 && brake_04 >= 2, "Corrado has both 0x02 running tail and 0x04 brake points");
      }
    }
  }

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

void test_dual_headlights_and_rotor_bias() {
  // R3-REN-10: Dual L+R headlight origin splitting & fallback symmetry.
  const std::array<float, 3> fallback{0.0f, 0.70f, 1.95f};
  {
    const float pts[] = {
        -0.72f, 0.68f, 2.04f,  // FL headlamp
        -0.64f, 0.70f, 2.00f,  // FL inner beam
        0.70f,  0.69f, 2.02f,  // FR headlamp
        0.0f,   0.80f, -2.0f,  // rear point (ignored)
    };
    const uint8_t mask[] = {0x01u, 0x01u, 0x01u, 0x01u};
    const auto d = carview::resolve_dual_headlight_origins(pts, mask, 4, fallback);
    check(d.has_left_pts && d.has_right_pts, "dual headlight origins detect both L and R front lamps");
    check(std::fabs(d.left[0] - (-0.68f)) < 1e-3f && std::fabs(d.right[0] - 0.70f) < 1e-3f,
          "dual headlight origins preserve distinct L/R lateral separation");
    check(d.left[2] > 1.9f && d.right[2] > 1.9f, "dual headlight origins ignore rear lamp points");
  }
  {
    // Single-side carbin lamp mirrors cleanly to opposite corner.
    const float pts[] = {-0.75f, 0.66f, 1.90f};
    const uint8_t mask[] = {0x01u};
    const auto d = carview::resolve_dual_headlight_origins(pts, mask, 1, fallback);
    check(d.has_left_pts && !d.has_right_pts, "single-side headlight sets left flag only");
    check(std::fabs(d.right[0] - 0.75f) < 1e-4f && std::fabs(d.right[1] - 0.66f) < 1e-4f,
          "missing right headlight mirrors left X coordinate");
  }
  {
    // Empty lamp table falls back to +/-0.62 m around fallback origin.
    const auto d = carview::resolve_dual_headlight_origins(nullptr, nullptr, 0, fallback);
    check(!d.has_left_pts && !d.has_right_pts, "fallback reports no carbin lamp points");
    check(std::fabs(d.left[0] - (-0.62f)) < 1e-4f && std::fabs(d.right[0] - 0.62f) < 1e-4f,
          "fallback separates L and R headlights by +/-0.62 m");
  }

  // R3-REN-10: Per-corner headlight damage independently kills smashed side while preserving intact side.
  {
    const float fl_dead = carview::eval_headlight_corner_damage(0.60f, 0.60f, true, 1.0f, 0.0f);
    const float fr_ok = carview::eval_headlight_corner_damage(0.05f, 0.60f, true, 1.0f, 1.7f);
    check(fl_dead == 0.0f && fr_ok == 1.0f,
          "smashing front-left corner (>=0.45) kills only left headlight while right stays full strength");
    const float fl_flicker = carview::eval_headlight_corner_damage(0.35f, 0.35f, true, 0.5f, 0.0f);
    check(fl_flicker > 0.0f && fl_flicker < 1.0f, "moderate front corner damage (0.24..0.45) flickers beam");
    const float legacy_dead = carview::eval_headlight_corner_damage(0.0f, 0.92f, false, 1.0f, 0.0f);
    const float legacy_ok = carview::eval_headlight_corner_damage(0.0f, 0.30f, false, 1.0f, 0.0f);
    check(legacy_dead == 0.0f && legacy_ok == 1.0f, "legacy front-zone damage fallback works when corners absent");
  }

  // R3-REN-19: Brake rotor heat splits front/rear with ~70/30 bias so front rotors glow hotter.
  {
    const auto zero = carview::split_rotor_heat_bias(0.0f);
    check(zero[0] == 0.0f && zero[1] == 0.0f, "zero brake heat keeps front and rear rotors cool");
    const auto hard = carview::split_rotor_heat_bias(0.90f, 0.70f);
    check(std::fabs(hard[0] - 0.90f) < 1e-4f, "front rotors carry full peak heat at 70/30 brake bias");
    check(std::fabs(hard[1] - (0.90f * (30.0f / 70.0f))) < 1e-3f && hard[0] > hard[1] * 2.0f,
          "rear rotors receive ~30/70 proportion of front brake rotor heat");
    const auto clamped = carview::split_rotor_heat_bias(2.5f, 0.70f);
    check(clamped[0] <= 1.0f && clamped[1] <= 1.0f, "rotor heat split clamps within [0, 1]");
  }
}

}  // namespace

int main() {
  test_lamp_state_rules();
  test_technique_classification_and_emission();
  test_cockpit_animation_math();
  test_dual_headlights_and_rotor_bias();
  test_disc_lamp_points_if_available();
  if (g_failures == 0) std::printf("lamps_tests: all checks passed\n");
  return g_failures ? 1 : 0;
}
