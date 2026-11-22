// Unit tests for Slice 12 Sub-Milestone 1:
// Racing wheel shaping, Separate Devices multi-USB bindings, H-shifter 1-6+R,
// DirectInput FFB physics solver & trace CSV, DualSense trigger packet
// encoding, release-on-pause safety, lightbar RPM, gyro tilt steering, and
// PlayStation UI4 glyphs.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "drive/dualsense.h"
#include "drive/wheel_ffb.h"
#include "ui.h"
#include "ui4_chrome.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

void test_wheel_shaping() {
  drive::WheelShaping s;
  // Identity shaping.
  check(drive::shape_wheel_steering_i16(0, s) == 0, "wheel shaping: 0 stays 0");
  check(drive::shape_wheel_steering_i16(16384, s) == 16384, "wheel shaping: half lock unchanged at default");
  check(drive::shape_wheel_steering_i16(-32768, s) == -32768, "wheel shaping: full left lock preserved");
  check(drive::shape_wheel_pedal_u8(128, s) == 128, "wheel shaping: pedal unchanged at 0% deadzone");

  // 50% saturation: 50% physical travel reaches full lock (225 deg on 900 deg wheel).
  s.steering_saturation = 50;
  check(drive::shape_wheel_steering_i16(16384, s) == 32767, "wheel shaping: 50% saturation locks at half travel");
  check(drive::shape_wheel_steering_i16(-16384, s) == -32768, "wheel shaping: 50% saturation left locks at half travel");

  // Linearity curve 200 (quadratic): 50% travel -> 25% output.
  s.steering_saturation = 100;
  s.steering_curve = 200;
  const double q = drive::shape_wheel_steering(0.5, s);
  check(std::fabs(q - 0.25) < 1e-6, "wheel shaping: linearity 200 squares travel");

  // Steering deadzone skip (20%): tiny off-centre input jumps past 20%.
  s.steering_curve = 100;
  s.steering_deadzone = 20;
  check(drive::shape_wheel_steering(0.01, s) > 0.20, "wheel shaping: deadzone skip lifts small input");

  // Pedal deadzone (10%): <=10% returns 0, 100% returns 1.0.
  s.pedal_deadzone = 10;
  check(drive::shape_wheel_pedal(0.09, s) == 0.0, "wheel shaping: pedal below deadzone is 0");
  check(std::fabs(drive::shape_wheel_pedal(1.0, s) - 1.0) < 1e-6, "wheel shaping: pedal full press is 1.0");
  check(drive::shape_wheel_pedal_u8(20, s) == 0, "wheel shaping: u8 pedal below 10% is 0");
  check(drive::shape_wheel_pedal_u8(255, s) == 255, "wheel shaping: u8 pedal full press is 255");
}

void test_separate_devices_and_h_shifter() {
  // Axis binding parsing & round-trip formatting.
  auto ax = drive::parse_wheel_axis_binding("0EB7:0020|-axis2");
  check(ax.has_value(), "parse axis binding with vid:pid and invert");
  check(ax->device == "0EB7:0020" && ax->axis == 2 && ax->direction == drive::AxisDirection::kInverted,
        "axis binding fields parsed accurately");
  check(ax->format() == "0EB7:0020|-axis2", "axis binding format round-trips");

  // Device matching: hex vid:pid, path:, name substring, empty.
  check(drive::wheel_device_matches("0eb7:0020", "Fanatec CSL DD", 0x0EB7, 0x0020), "vid:pid match case-insensitive");
  check(!drive::wheel_device_matches("0eb7:0021", "Fanatec CSL DD", 0x0EB7, 0x0020), "vid:pid mismatch rejected");
  check(drive::wheel_device_matches("path:\\\\?\\hid#vid_0eb7", "Any", 0, 0, "\\\\?\\HID#VID_0EB7"),
        "path: exact match case-insensitive");
  check(drive::wheel_device_matches("simjack", "SimJack Pro Pedals", 0x1234, 0x5678), "name substring match");
  check(drive::is_known_wheel_vendor(0x0EB7) && drive::is_known_wheel_vendor(0x346E), "Fanatec and Moza known vendors");

  // Multi-USB rig: Fanatec wheel base + SimJack pedals + USB H-shifter + USB handbrake + button box.
  drive::SeparateWheelConfig cfg;
  cfg.enabled = true;
  cfg.linear_900_deg = true;
  cfg.wheel_rotation_deg = 900;
  cfg.steering = *drive::parse_wheel_axis_binding("Fanatec|axis0");
  cfg.throttle = *drive::parse_wheel_axis_binding("SimJack|+axis0");
  cfg.brake = *drive::parse_wheel_axis_binding("SimJack|-axis1");
  cfg.clutch = *drive::parse_wheel_axis_binding("SimJack|+axis2");
  cfg.handbrake = *drive::parse_wheel_axis_binding("Handbrake|+axis0");
  cfg.throttle_dz_bottom = 5;
  cfg.throttle_dz_top = 5;
  cfg.buttons = drive::parse_wheel_button_bindings(
      "SHIFT_UP=Fanatec|b0,SHIFT_DOWN=Fanatec|b1,"
      "GEAR1=Shifter|0,GEAR2=Shifter|1,GEAR3=Shifter|2,GEAR4=Shifter|3,GEAR5=Shifter|4,GEAR6=Shifter|5,"
      "REVERSE=Shifter|6,CAMERA=ButtonBox|h0.up,REWIND=ButtonBox|b3");
  check(cfg.buttons.size() == 11, "all 11 separate-device buttons/shifter gates parsed");

  drive::UsbDeviceSnapshot base;
  base.name = "Fanatec CSL DD";
  base.vendor = 0x0EB7;
  base.product = 0x0020;
  base.axes = {16384};  // half right
  base.buttons = {0, 0};

  drive::UsbDeviceSnapshot pedals;
  pedals.name = "SimJack Pro Pedals";
  pedals.axes = {32767, -32768, 0};  // full throttle (+), full brake (-), half clutch (+)

  drive::UsbDeviceSnapshot shifter;
  shifter.name = "USB H-Shifter";
  shifter.buttons = {0, 0, 1, 0, 0, 0, 0};  // 3rd gear engaged

  drive::UsbDeviceSnapshot hb;
  hb.name = "USB Handbrake";
  hb.axes = {32767};  // pulled

  drive::UsbDeviceSnapshot bbox;
  bbox.name = "Sim ButtonBox";
  bbox.hats = {0x01};  // h0.up
  bbox.buttons = {0, 0, 0, 1};  // b3

  const auto out = drive::evaluate_separate_wheel(cfg, {base, pedals, shifter, hb, bbox});
  check(out.active && out.linear_900, "separate wheel active with 900 deg linear mode");
  check(std::fabs(out.steer - 0.5) < 0.01, "separate wheel steering reads half right");
  check(out.throttle > 0.99 && out.brake > 0.99, "separate pedals read full throttle and inverted brake");
  check(std::fabs(out.clutch - 0.5) < 0.02, "separate clutch reads half travel");
  check(out.handbrake > 0.99, "separate handbrake axis reads full pull");
  check(out.has_h_shifter && out.h_shifter_gear == 3, "H-shifter reads 3rd gear");
  check(out.camera && out.rewind, "button box hat and button read correctly");
  check(drive::resolve_wheel_shift(out, 2, false, false) == +1, "H-shifter gate 3 from gear 2 requests upshift");
  check(drive::resolve_wheel_shift(out, 4, false, false) == -1, "H-shifter gate 3 from gear 4 requests downshift");
  check(drive::resolve_wheel_shift(out, 3, false, false) == 0, "H-shifter gate 3 in gear 3 holds gear");

  // Reverse gate.
  shifter.buttons = {0, 0, 0, 0, 0, 0, 1};
  const auto out_rev = drive::evaluate_separate_wheel(cfg, {base, pedals, shifter, hb, bbox});
  check(out_rev.h_shifter_gear == -1, "H-shifter reverse gate reads -1");
}

void test_ffb_solver_and_trace() {
  drive::WheelFfbConfig cfg;
  cfg.mode = drive::WheelFfbMode::kFull;
  cfg.strength_percent = 80;
  cfg.invert = false;

  drive::WheelFfbSolver solver;
  drive::WheelFfbTelemetry slow_tel;
  slow_tel.driving = true;
  slow_tel.speed_mps = 5.0;
  slow_tel.front_load_n = 7000.0;
  slow_tel.static_front_load_n = 7000.0;
  slow_tel.front_slip_angle = 0.5;
  slow_tel.rpm = 2000.0;
  slow_tel.redline_rpm = 7000.0;

  const auto f_slow = solver.step(slow_tel, cfg, 1.0 / 60.0);
  check(f_slow.active, "FFB active while driving");
  check(f_slow.inertia_coeff > 0 && f_slow.friction_coeff > 0 && f_slow.damper_coeff > 0 && f_slow.spring_coeff > 0,
        "FFB outputs all four condition effects (inertia, friction, damper, spring)");

  // Higher speed stiffens spring and lowers static friction; aligning force opposes positive slip.
  drive::WheelFfbTelemetry fast_tel = slow_tel;
  fast_tel.speed_mps = 50.0;
  fast_tel.rpm = 6800.0;
  fast_tel.throttle_input = 1.0;
  fast_tel.surface_bump_rumble = 0.0;
  drive::WheelFfbForces f_fast{};
  for (int i = 0; i < 6; ++i) f_fast = solver.step(fast_tel, cfg, 1.0 / 60.0);
  check(f_fast.spring_coeff > f_slow.spring_coeff, "speed-stiffening spring increases with speed");
  check(f_fast.friction_coeff < f_slow.friction_coeff, "static friction eases off at speed");
  check(f_fast.engine_rumble > f_slow.engine_rumble, "engine rumble grows toward redline");
  check(f_fast.aligning_norm < -0.2 && f_fast.constant_force < 0,
        "positive front slip produces opposing aligning force");

  fast_tel.surface_bump_rumble = 0.8;
  const auto f_bump = solver.step(fast_tel, cfg, 1.0 / 60.0);
  check(f_bump.road_rumble > 0.5, "road rumble responds to surface bumpiness");

  // Invert flips constant force sign.
  cfg.invert = true;
  fast_tel.surface_bump_rumble = 0.0;
  solver.reset();
  drive::WheelFfbForces f_inv{};
  for (int i = 0; i < 6; ++i) f_inv = solver.step(fast_tel, cfg, 1.0 / 60.0);
  check(f_inv.constant_force > 0, "invert flips FFB constant force direction");

  // Paused / menus immediately zero FFB.
  fast_tel.driving = false;
  const auto f_paused = solver.step(fast_tel, cfg, 1.0 / 60.0);
  check(!f_paused.active && f_paused.constant_force == 0 && f_paused.spring_coeff == 0,
        "FFB zeroes completely when not driving");

  // Test pull pulls left first (0..0.4s), pauses (0.4..0.6s), then pulls right (0.6..1.0s).
  cfg.invert = false;
  solver.start_test_pull();
  const auto pull_left = solver.step(fast_tel, cfg, 0.05);
  check(pull_left.active && pull_left.constant_force < 0, "test pull pulls left first");
  drive::WheelFfbForces pull_mid{};
  for (int i = 0; i < 9; ++i) pull_mid = solver.step(fast_tel, cfg, 0.05);  // t=0.50s (pause)
  check(pull_mid.constant_force == 0, "test pull pauses in middle");
  drive::WheelFfbForces pull_right{};
  for (int i = 0; i < 5; ++i) pull_right = solver.step(fast_tel, cfg, 0.05);  // t=0.75s (right)
  check(pull_right.active && pull_right.constant_force > 0, "test pull pulls right second");

  // CSV trace recorder.
  const auto tmp_dir = std::filesystem::temp_directory_path() / "shift_s12_ffb_trace_test";
  drive::FfbTraceRecorder rec;
  std::string csv_path;
  check(rec.start(tmp_dir, &csv_path), "FFB trace recorder creates CSV file");
  rec.record(0.016, cfg.mode, slow_tel, f_slow);
  rec.record(0.033, cfg.mode, fast_tel, f_fast);
  rec.stop();
  check(rec.rows_written() == 2, "FFB trace recorder wrote 2 rows");
  std::ifstream in(csv_path);
  std::string header;
  std::getline(in, header);
  check(header.find("constant_force,spring,damper,inertia,friction,engine_rumble,road_rumble") != std::string::npos,
        "FFB trace CSV header contains all channels");
  in.close();
  std::error_code ec;
  std::filesystem::remove_all(tmp_dir, ec);
}

void test_dualsense_protocol_and_model() {
  using namespace drive::dualsense;

  check(is_dualsense(0x054C, 0x0CE6), "DualSense VID/PID recognized");
  check(is_dualsense(0x054C, 0x0DF2), "DualSense Edge VID/PID recognized");
  check(!is_dualsense(0x054C, 0x05C4), "DS4 is not DualSense");
  check(is_playstation_pad(0x054C, 0x05C4), "DS4 is PlayStation pad for glyphs");
  check(supports_gyro_tilt(0x057E, 0x2009), "Switch Pro supports gyro tilt");

  // Trigger block byte encoding checks.
  const auto e_off = off();
  check(e_off[0] == 0x05 && reported_family(e_off) == 0, "Off block is 0x05");

  const auto e_fb = feedback(2, 6);
  check(e_fb[0] == 0x21 && reported_family(e_fb) == 1, "Feedback block is 0x21");
  // Zones 2..9 = bits 2..9 = 0x03FC.
  check(e_fb[1] == 0xFC && e_fb[2] == 0x03, "Feedback zone mask covers zones 2..9");

  const auto e_vib = vibration(3, 4, 40);
  check(e_vib[0] == 0x26 && e_vib[9] == 40 && reported_family(e_vib) == 3, "Vibration mode & 40 Hz byte");

  const auto e_mac = machine(2, 8, 1, 3, 24, 1);
  check(e_mac[0] == 0x27 && e_mac[3] == ((1 & 7) | ((3 & 7) << 3)) && e_mac[4] == 24 && e_mac[5] == 1 &&
            reported_family(e_mac) == 6,
        "Machine ABS pulse block encoded accurately");

  const auto e_bow = bow(4, 6, 8, 8);
  check(e_bow[0] == 0x22 && reported_family(e_bow) == 4, "Bow gear-click block encoded accurately");

  // Out-of-range inputs safely return Off.
  check(feedback(10, 5) == off(), "Out-of-range feedback returns Off");
  check(machine(8, 2, 1, 3, 24, 1) == off(), "Invalid machine range returns Off");

  // 47-byte payload sets ONLY bits 0x04 | 0x08 in byte 0 (never clobbers SDL rumble/LED bits).
  TriggerEffects fx{e_mac, e_bow};
  const auto payload = build_effect_payload(fx);
  check(payload.size() == 47, "SDL_SendGamepadEffect payload is 47 bytes");
  check(payload[0] == (kEnableRightTrigger | kEnableLeftTrigger), "Only trigger enable bits set in byte 0");
  check(payload[1] == 0 && payload[38] == 0, "LED and rumble enable bits remain 0");
  check(payload[kRightTriggerOffset] == 0x22 && payload[kLeftTriggerOffset] == 0x27,
        "Right trigger at offset 10, left trigger at offset 21");

  // Emergency USB/BT release reports.
  const auto usb_rel = build_usb_release_report();
  check(usb_rel[0] == 0x02 && usb_rel[1 + kLeftTriggerOffset] == 0x05 && usb_rel[1 + kRightTriggerOffset] == 0x05,
        "USB release report sets ID 0x02 and both triggers Off");
  const auto bt_rel = build_bt_release_report();
  const uint32_t expected_crc = dualsense_bt_crc32(bt_rel.data(), 74);
  const uint32_t stored_crc = uint32_t(bt_rel[74]) | (uint32_t(bt_rel[75]) << 8) |
                              (uint32_t(bt_rel[76]) << 16) | (uint32_t(bt_rel[77]) << 24);
  check(bt_rel[0] == 0x31 && stored_crc == expected_crc, "BT release report sets ID 0x31 with valid 0xA2 CRC32");

  // EffectChannel rate limit: 1000 changes over 1 second -> <= 61 reports.
  EffectChannel ch;
  EffectPayload out_p{};
  uint64_t sent = 0;
  for (uint64_t ms = 0; ms < 1000; ++ms) {
    TriggerEffects w;
    w.left = feedback(int(ms % 9), 4);
    if (ch.update(w, ms * 1000000ULL, &out_p)) ++sent;
  }
  check(sent >= 55 && sent <= 61, "EffectChannel caps output at ~60 Hz");

  // TriggerModel: Brake slope -> ABS pulse -> Wheel lock -> Throttle wheelspin -> Gear click -> Release on pause!
  TriggerModel model;
  Settings st;
  st.trigger_mode = TriggerMode::kFull;
  st.trigger_strength_pct = 100;
  st.gear_click = true;

  CarTelemetry car;
  car.valid = true;
  car.gear = 2;
  car.rpm = 5000.0f;
  car.redline_rpm = 7500.0f;
  car.driven_rear = true;

  TriggerInputs in;
  in.driving = true;
  in.brake = 0.6f;
  in.telemetry = &car;

  // Normal braking -> slope feedback.
  auto out = model.step(in, st, 1.0 / 60.0);
  check(reported_family(out.left) == 1, "Normal braking produces rising slope feedback");

  // ABS active -> Machine pulse.
  car.abs_active = true;
  out = model.step(in, st, 1.0 / 60.0);
  check(reported_family(out.left) == 6, "ABS active produces Machine pulse");

  // Wheel lock (no ABS) -> Vibration buzz.
  car.abs_active = false;
  car.front_slip = -2.5f;
  for (int i = 0; i < 20; ++i) out = model.step(in, st, 1.0 / 60.0);  // wait out 200ms ABS hold
  check(reported_family(out.left) == 3, "Locked wheel under braking produces Vibration buzz");

  // Throttle wheelspin -> Vibration buzz.
  in.brake = 0.0f;
  in.throttle = 0.9f;
  car.front_slip = 0.0f;
  car.rear_slip = 3.5f;
  for (int i = 0; i < 5; ++i) out = model.step(in, st, 1.0 / 60.0);
  check(reported_family(out.right) == 3, "Driven wheelspin produces throttle Vibration buzz");

  // Gear shift -> Bow click.
  car.gear = 3;
  out = model.step(in, st, 1.0 / 60.0);
  check(reported_family(out.right) == 4, "Gear shift produces Bow click on throttle");

  // Pause / menu / focus loss -> immediate release (both Off).
  in.driving = false;
  out = model.step(in, st, 1.0 / 60.0);
  check(out.left == off() && out.right == off(), "Triggers release immediately when paused/unfocused");

  // Lightbar RPM gradient & limiter flash.
  const Rgb low = rpm_colour(2000.0f, 7000.0f, true);
  const Rgb mid = rpm_colour(5100.0f, 7000.0f, true);
  const Rgb hi = rpm_colour(6900.0f, 7000.0f, true);
  const Rgb hi_off = rpm_colour(6900.0f, 7000.0f, false);
  check(low.r == 0 && low.g > 200, "Low RPM lightbar is green");
  check(mid.r > 150 && mid.g > 150, "Mid-high RPM lightbar transitions through yellow");
  check(hi.r == 255 && hi.g == 0 && hi_off.r == 0, "Limiter >=97% flashes red/off");

  // Gyro tilt steering + touchpad recenter.
  GyroTiltSteering gyro;
  gyro.on_accel(0.0f, 9.81f, 0.0f);  // level -> 0 rad
  check(gyro.valid() && std::fabs(gyro.steering(100)) < 1e-4f, "Level pad steers 0");
  // Rotate clockwise by 45 degrees via gyro (negative Z rotation -> positive steer right).
  gyro.on_gyro(0.0f, 0.0f, -0.785398f / 0.04f, 100000000ULL);
  gyro.on_gyro(0.0f, 0.0f, -0.785398f / 0.04f, 140000000ULL);  // 40ms step -> -45 deg
  check(gyro.steering(100) > 0.9f, "45 deg tilt at 100% sensitivity reaches near full lock");
  gyro.recenter();
  check(std::fabs(gyro.steering(100)) < 1e-4f, "Touchpad recenter zeroes tilt steering");
}

void test_playstation_ui4_glyphs() {
  carview::ui::List ps_list;
  carview::ui4::set_glyph_set(carview::ui4::GlyphSet::kPlayStation);
  check(carview::ui4::glyph_set() == carview::ui4::GlyphSet::kPlayStation, "PlayStation glyph set active");
  carview::ui4::button_bar(ps_list, {{"A", "SELECT"}, {"B", "BACK"}, {"X", "FILTER"}, {"Y", "SORT"}, {"LB", "PREV"}, {"RB", "NEXT"}},
                           1280.0f, 720.0f);
  check(ps_list.items().size() > 20, "PlayStation button bar emits vector glyph instances");
  std::vector<uint8_t> fb(1280 * 720 * 4, 0);
  carview::ui::rasterize(ps_list.items(), fb, 1280, 720);
  size_t lit = 0;
  for (size_t i = 3; i < fb.size(); i += 4) lit += fb[i] > 0;
  check(lit > 5000, "PlayStation glyphs rasterize cleanly");
  carview::ui4::set_glyph_set(carview::ui4::GlyphSet::kXbox);
}

}  // namespace

int main() {
  test_wheel_shaping();
  test_separate_devices_and_h_shifter();
  test_ffb_solver_and_trace();
  test_dualsense_protocol_and_model();
  test_playstation_ui4_glyphs();
  if (g_failures > 0) {
    std::fprintf(stderr, "wheel_dualsense_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("wheel_dualsense_tests: PASS\n");
  return 0;
}
