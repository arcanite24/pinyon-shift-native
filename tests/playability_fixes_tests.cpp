// Unit & regression tests for playability fixes:
// 1. 8BitDo / SDL_GameControllerDB loading + SDL3 HIDAPI hints + raw joystick fallback
// 2. Keyboard W/Up acceleration from 0 -> >50 km/h with & without phantom controller/wheel axes
// 3. Alt-tab focus-gain zero-recreate fast path & FramePacer reset
// 4. Water material detection (LAKE_ANIM_NORM_OPAC_REFL_3, RIVER/WATER, non-decal alpha)

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "drive/controls.h"
#include "drive/vehicle.h"
#include "drive/wheel_ffb.h"
#include "gamepad_db.h"
#include "render_modern.h"
#include "renderer.h"
#include "world/world.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
    ++g_failures;
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

void test_gamepad_db_and_8bitdo() {
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  carview::apply_sdl_gamepad_hints();
  CHECK(std::string(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI)) == "1");
  CHECK(std::string(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX)) == "1");
  CHECK(std::string(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360)) == "1");
  CHECK(std::string(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX_ONE)) == "1");
  CHECK(std::string(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH)) == "1");
  CHECK(std::string(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4)) == "1");
  CHECK(std::string(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5)) == "1");
  CHECK(std::string(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS)) == "1");
  CHECK(std::string(SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS)) == "1");
  CHECK(std::string(SDL_GetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS)) == "0");

  CHECK(SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK));

  // Verify embedded mappings include all major 8BitDo families.
  const auto& embedded = carview::embedded_gamepad_mappings();
  CHECK(embedded.size() >= 18);
  bool has_u2c = false, has_pro2 = false, has_sn30 = false, has_m30 = false, has_lite2 = false, has_arcade = false;
  for (std::string_view m : embedded) {
    if (m.find("Ultimate 2C") != std::string_view::npos) has_u2c = true;
    if (m.find("Pro 2") != std::string_view::npos) has_pro2 = true;
    if (m.find("SN30 Pro") != std::string_view::npos) has_sn30 = true;
    if (m.find("M30") != std::string_view::npos) has_m30 = true;
    if (m.find("Lite 2") != std::string_view::npos) has_lite2 = true;
    if (m.find("Arcade Stick") != std::string_view::npos) has_arcade = true;
  }
  CHECK(has_u2c && has_pro2 && has_sn30 && has_m30 && has_lite2 && has_arcade);

  // Load both embedded mappings and bundled config/gamecontrollerdb.txt.
  const std::filesystem::path src_root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto stats = carview::load_gamepad_mappings(src_root);
  CHECK(stats.embedded_added > 0);
  CHECK(!stats.loaded_file.empty());
  CHECK(stats.file_added >= 0);

  // Wheel vs gamepad classification: 8BitDo / Xbox / PS / Switch / Logitech F310 are NOT wheels.
  CHECK(!carview::is_racing_wheel_device(0x2DC8, 0x301D, "8BitDo Ultimate 2C Wired Controller"));
  CHECK(!carview::is_racing_wheel_device(0x2DC8, 0x6003, "8BitDo Pro 2"));
  CHECK(!carview::is_racing_wheel_device(0x045E, 0x02EA, "Xbox One Controller"));
  CHECK(!carview::is_racing_wheel_device(0x054C, 0x0CE6, "DualSense Wireless Controller"));
  CHECK(!carview::is_racing_wheel_device(0x057E, 0x2009, "Nintendo Switch Pro Controller"));
  CHECK(!carview::is_racing_wheel_device(0x046D, 0xC21D, "Logitech Gamepad F310"));
  CHECK(!carview::is_racing_wheel_device(0x0079, 0x0006, "Generic USB Joystick"));
  // Genuine wheels ARE classified as wheels.
  CHECK(carview::is_racing_wheel_device(0x046D, 0xC24F, "Logitech G29 Driving Force Racing Wheel"));
  CHECK(carview::is_racing_wheel_device(0x044F, 0xB66E, "Thrustmaster T300RS Racing Wheel"));
  CHECK(carview::is_racing_wheel_device(0x0EB7, 0x0006, "Fanatec CSL DD"));
  CHECK(carview::is_racing_wheel_device(0x346E, 0x0004, "MOZA R9 Base"));

  SDL_Quit();
}

void test_raw_joystick_and_wheel_calibration() {
  const drive::ControlDeadzones dz{};
  const drive::WheelShaping ws{};

  // 1. Unmapped 8BitDo in D-Input/macOS HID mode resting at 0 on all 6 axes:
  //    MUST output zero throttle, zero brake, zero steer, any_active == false!
  {
    carview::RawJoystickSnapshot idle_zero;
    idle_zero.axes = {0, 0, 0, 0, 0, 0};
    idle_zero.rest_axes = {0, 0, 0, 0, 0, 0};
    idle_zero.buttons.assign(16, 0u);
    const auto out = carview::sample_raw_joystick_pad(idle_zero, dz);
    CHECK(!out.any_active);
    CHECK(out.input.throttle == 0.0);
    CHECK(out.input.brake == 0.0);
    CHECK(out.input.steer == 0.0);
  }

  // 2. Unmapped controller with triggers resting at -32768 (standard HID) and sticks at 0:
  {
    carview::RawJoystickSnapshot idle_trig;
    idle_trig.axes = {0, 0, 0, 0, -32768, -32768};
    idle_trig.rest_axes = {0, 0, 0, 0, -32768, -32768};
    idle_trig.buttons.assign(16, 0u);
    const auto out_idle = carview::sample_raw_joystick_pad(idle_trig, dz);
    CHECK(!out_idle.any_active);
    CHECK(out_idle.input.throttle == 0.0);
    CHECK(out_idle.input.brake == 0.0);

    // Pull RT (axis 5 -> +32767) and steer right (axis 0 -> +24000):
    idle_trig.axes[0] = 24000;
    idle_trig.axes[5] = 32767;
    const auto out_drive = carview::sample_raw_joystick_pad(idle_trig, dz);
    CHECK(out_drive.any_active);
    CHECK(out_drive.input.throttle > 0.95);
    CHECK(out_drive.input.brake == 0.0);
    CHECK(out_drive.input.steer > 0.5);
  }

  // 3. Genuine racing wheel with untouched pedals resting at 0, +32767, or -32768:
  //    MUST NEVER emit 0.50 phantom brake/throttle!
  for (int16_t rest_val : {int16_t(0), int16_t(32767), int16_t(-32768)}) {
    carview::RawJoystickSnapshot wheel_idle;
    wheel_idle.axes = {0, rest_val, rest_val};
    wheel_idle.rest_axes = {0, rest_val, rest_val};
    const auto out = carview::sample_wheel_joystick(wheel_idle, ws);
    CHECK(!out.any_active);
    CHECK(out.input.throttle == 0.0);
    CHECK(out.input.brake == 0.0);
  }

  // 4. Genuine racing wheel with inverted pedals resting at +32767 and throttle pressed to -32768:
  {
    carview::RawJoystickSnapshot wheel_gas;
    wheel_gas.rest_axes = {0, 32767, 32767};
    wheel_gas.axes = {0, -32768, 32767};
    const auto out = carview::sample_wheel_joystick(wheel_gas, ws);
    CHECK(out.any_active);
    CHECK(out.input.throttle > 0.95);
    CHECK(out.input.brake == 0.0);
  }
}

void test_keyboard_acceleration_regression() {
  // Verify keyboard W/Up accelerates a car cleanly from 0 -> >50 km/h with
  // zero brake pressure in ALL 3 attachment states:
  //   A) No controller connected
  //   B) Unmapped 8BitDo USB controller resting at 0 / -32768
  //   C) Attached gamepad with slight left-stick drift + racing wheel resting at 0
  const drive::ControlDeadzones dz{};
  const drive::WheelShaping ws{};

  auto run_launch_scenario = [&](const char* label, const drive::Input& pad, bool pad_active,
                                 const carview::RawJoystickSnapshot* raw_snap,
                                 const carview::RawJoystickSnapshot* wheel_snap) {
    drive::CarParams cp;
    const double r = 0.32;
    cp.wheel_centres = {drive::Vec3{-0.78, r, 1.30}, drive::Vec3{0.78, r, 1.30}, drive::Vec3{-0.79, r, -1.25},
                        drive::Vec3{0.79, r, -1.25}};
    cp.tyre_radius = {r, r, r, r};
    cp.centre_of_mass = {0, 0.5, 0.05};
    drive::Vehicle v(cp);
    drive::FlatGround ground;
    v.reset({0, 0, 0}, 0.0);

    drive::Input kb;
    kb.throttle = 1.0;  // Holding W / Up
    kb.brake = 0.0;

    carview::RawJoystickDriveOutput raw_out;
    if (raw_snap) raw_out = carview::sample_raw_joystick_pad(*raw_snap, dz);
    carview::RawJoystickDriveOutput wheel_out;
    if (wheel_snap) wheel_out = carview::sample_wheel_joystick(*wheel_snap, ws);

    const drive::Input merged = carview::merge_driving_inputs(kb, pad, pad_active, raw_out.input, raw_out.any_active,
                                                              wheel_out.input, wheel_out.any_active);
    CHECK(merged.throttle == 1.0);
    CHECK(merged.brake == 0.0);
    CHECK(!merged.handbrake);

    // Step 3.5 seconds of physics simulation at Vehicle::kStep (360 Hz).
    const int n_steps = int(std::lround(3.5 / drive::Vehicle::kStep));
    for (int step = 0; step < n_steps; ++step) {
      v.step(merged, ground);
    }
    const double kmh = v.speed() * 3.6;
    const auto& w = v.wheels();
    if (kmh <= 50.0) {
      std::fprintf(stderr, "scenario %s failed: speed %.2f km/h, rpm %.0f, fl_brake %.3f\n", label, kmh, v.rpm(),
                   w[0].brake);
    }
    CHECK(kmh > 50.0);
    CHECK(w[0].brake < 0.01 && w[1].brake < 0.01);
  };

  // Scenario A: No controller attached.
  run_launch_scenario("keyboard_only", drive::Input{}, false, nullptr, nullptr);

  // Scenario B: Unmapped 8BitDo USB controller with axes at 0 & triggers at -32768.
  {
    carview::RawJoystickSnapshot pad_8bitdo;
    pad_8bitdo.axes = {0, 0, 0, 0, -32768, -32768};
    pad_8bitdo.rest_axes = {0, 0, 0, 0, -32768, -32768};
    pad_8bitdo.buttons.assign(16, 0u);
    run_launch_scenario("keyboard_with_8bitdo_raw", drive::Input{}, false, &pad_8bitdo, nullptr);
  }

  // Scenario C: Drifting gamepad stick + uncalibrated wheel resting at 0.
  {
    drive::Input drifting_pad;
    drifting_pad.steer = 0.02;
    drifting_pad.throttle = 0.0;
    drifting_pad.brake = 0.0;
    carview::RawJoystickSnapshot wheel_zero;
    wheel_zero.axes = {0, 0, 0};
    wheel_zero.rest_axes = {0, 0, 0};
    run_launch_scenario("keyboard_with_drifting_pad_and_wheel", drifting_pad, true, nullptr, &wheel_zero);
  }
}

void test_alt_tab_zero_recreate_fast_path() {
  // Focus gain with unchanged pixel dimensions -> ZERO swapchain or target recreation.
  CHECK(!carview::Renderer::needs_swapchain_recreate(false, true, 1600, 900, 1600, 900));
  CHECK(!carview::Renderer::needs_target_recreate(false, true, 1600, 900, 1600, 900, false));

  // VK_SUBOPTIMAL_KHR during alt-tab when dimensions didn't change -> DO NOT rebuild swapchain!
  CHECK(!carview::Renderer::should_mark_dirty_on_present(VK_SUBOPTIMAL_KHR, 1600, 900, 1600, 900));
  // VK_SUBOPTIMAL_KHR when dimensions actually changed -> DO rebuild.
  CHECK(carview::Renderer::should_mark_dirty_on_present(VK_SUBOPTIMAL_KHR, 1920, 1080, 1600, 900));
  // VK_ERROR_OUT_OF_DATE_KHR -> always rebuild swapchain (but still skips target rebuild if size/format unchanged!).
  CHECK(carview::Renderer::should_mark_dirty_on_present(VK_ERROR_OUT_OF_DATE_KHR, 1600, 900, 1600, 900));
  CHECK(!carview::Renderer::needs_target_recreate(false, true, 1600, 900, 1600, 900, false));
  CHECK(carview::Renderer::needs_target_recreate(false, true, 1600, 900, 1600, 900, true));

  // FramePacer::reset() clears stale schedule so returning from alt-tab doesn't sleep/stall.
  carview::modern::FramePacer pacer;
  pacer.set_interval(1.0 / 60.0);
  CHECK(pacer.wait_for(10.0) == 0.0);
  pacer.reset();
  CHECK(pacer.wait_for(15.0) == 0.0);
}

void test_water_material_detection() {
  CHECK(world::is_water_effect("LAKE_ANIM_NORM_OPAC_REFL_3"));
  CHECK(world::is_water_effect("LAKE_STATIC_REFL"));
  CHECK(world::is_water_effect("H_WATER_RIVER_1"));
  CHECK(world::is_water_effect("RIVER_FLOW_NORM"));
  CHECK(world::is_water_effect("pond_surface"));
  CHECK(!world::is_water_effect("H_ROAD_DIFF3_NORM1_NOISE_AO_LM"));
  CHECK(!world::is_water_effect("H_DIFF_WATERMARK"));
  CHECK(!world::is_water_effect("TREE_DIFF_OPAC_DIRLIGHT_2_2SD"));

  // Water effects draw with alpha blending and are NOT road decals.
  CHECK(world::effect_blend("LAKE_ANIM_NORM_OPAC_REFL_3") == world::Blend::kAlpha);
  CHECK(!world::decal_effect("LAKE_ANIM_NORM_OPAC_REFL_3"));
  CHECK(world::effect_blend("H_WATER_RIVER_1") == world::Blend::kAlpha);
  CHECK(!world::decal_effect("H_WATER_RIVER_1"));
}

}  // namespace

int main() {
  test_gamepad_db_and_8bitdo();
  test_raw_joystick_and_wheel_calibration();
  test_keyboard_acceleration_regression();
  test_alt_tab_zero_recreate_fast_path();
  test_water_material_detection();
  if (g_failures == 0) {
    std::printf("playability_fixes_tests: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "playability_fixes_tests: %d failure(s)\n", g_failures);
  return 1;
}
