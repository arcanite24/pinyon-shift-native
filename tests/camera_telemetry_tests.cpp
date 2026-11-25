// Unit tests for Slice 12 Sub-Milestone 2:
// OpenTrack 48-byte UDP packet parser & timeout recenter, Live Camera Editor
// & Immersive Camera pose math, FM7 311B & FH4/5 324B UDP telemetry binary
// layouts & probe CSV, and Camera Editor UI4 overlay.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "camera_editor.h"
#include "drive/cameras.h"
#include "drive/head_tracking.h"
#include "drive/telemetry_udp.h"
#include "drive/vehicle.h"
#include "ui.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

uint32_t read_u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

int32_t read_s32(const uint8_t* p) { return static_cast<int32_t>(read_u32(p)); }

uint16_t read_u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }

float read_f32(const uint8_t* p) {
  const uint32_t bits = read_u32(p);
  float f = 0.0f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

void test_opentrack_parser_and_timeout() {
  // Short packet rejected.
  std::array<uint8_t, 32> short_pkt{};
  check(!drive::parse_opentrack_packet(short_pkt).has_value(), "OpenTrack packet < 48 bytes rejected");

  // Valid 48-byte packet: x=10 cm, y=-5 cm, z=12 cm, yaw=45 deg, pitch=10 deg, roll=-8 deg.
  double vals[6] = {10.0, -5.0, 12.0, 45.0, 10.0, -8.0};
  std::array<uint8_t, 48> pkt{};
  std::memcpy(pkt.data(), vals, sizeof(vals));
  const auto p = drive::parse_opentrack_packet(pkt);
  check(p.has_value(), "OpenTrack 48-byte packet parsed");
  check(std::fabs(p->x_m - 0.10f) < 1e-5f && std::fabs(p->y_m + 0.05f) < 1e-5f && std::fabs(p->z_m - 0.12f) < 1e-5f,
        "OpenTrack XYZ converted from cm to metres");
  check(std::fabs(p->yaw_deg - 45.0f) < 1e-5f && std::fabs(p->pitch_deg - 10.0f) < 1e-5f &&
            std::fabs(p->roll_deg + 8.0f) < 1e-5f,
        "OpenTrack yaw/pitch/roll parsed in degrees");

  // NaN / Inf sanitized to 0.
  vals[0] = std::numeric_limits<double>::quiet_NaN();
  vals[3] = std::numeric_limits<double>::infinity();
  std::memcpy(pkt.data(), vals, sizeof(vals));
  const auto p_nan = drive::parse_opentrack_packet(pkt);
  check(p_nan.has_value() && p_nan->x_m == 0.0f && p_nan->yaw_deg == 0.0f, "OpenTrack sanitizes NaN/Inf to 0");

  // Invert parser & timeout hold (1.0 s) + smooth ease-back (0.5 s).
  drive::HeadTrackingConfig cfg;
  cfg.enabled = true;
  cfg.invert = drive::parse_head_invert("yaw, roll");
  check(cfg.invert[0] && !cfg.invert[1] && cfg.invert[2], "parse_head_invert sets yaw and roll");
  check(drive::format_head_invert(cfg.invert) == "yaw,roll", "format_head_invert round-trips");

  vals[0] = 5.0;
  vals[3] = 60.0;
  vals[5] = 10.0;
  std::memcpy(pkt.data(), vals, sizeof(vals));
  drive::HeadTrackingReceiver rx;
  rx.inject_packet_at(pkt, 10.0);

  const auto at_10_5 = rx.pose_at(10.5, cfg);  // age 0.5 s -> full hold
  check(std::fabs(at_10_5.yaw_deg + 60.0f) < 1e-4f && std::fabs(at_10_5.roll_deg + 10.0f) < 1e-4f,
        "OpenTrack holds full pose within 1.0 s timeout and applies invert");

  const auto at_11_25 = rx.pose_at(11.25, cfg);  // age 1.25 s -> 50% eased back
  check(std::fabs(at_11_25.yaw_deg + 30.0f) < 1e-3f, "OpenTrack eases back halfway at 1.25 s");

  const auto at_11_6 = rx.pose_at(11.6, cfg);  // age 1.6 s -> fully recentered
  check(at_11_6.is_zero(), "OpenTrack fully recenters after 1.5 s");

  // Apply to cockpit/hood vs chase.
  drive::CameraView view_cockpit;
  drive::HeadPose ninety;
  ninety.yaw_deg = 90.0f;
  drive::apply_head_tracking(view_cockpit, ninety, drive::CameraMode::kCockpit);
  check(view_cockpit.forward[0] > 0.99 && std::fabs(view_cockpit.forward[2]) < 0.02,
        "OpenTrack turns cockpit view full 90 deg right");

  drive::CameraView view_chase;
  drive::apply_head_tracking(view_chase, ninety, drive::CameraMode::kFollowLow);
  check(std::fabs(view_chase.forward[2] - 1.0) < 1e-6, "OpenTrack leaves chase camera untouched");
}

void test_camera_editor_and_immersive() {
  drive::CameraTuning tuning;
  tuning.chase[0].height = 0.5;
  tuning.chase[0].radius = 1.0;
  tuning.chase[0].fov = 50.0;
  tuning.chase[0].yaw_k = 40.0;
  tuning.chase[0].yaw_d = 12.0;
  tuning.chase[0].height_k = 30.0;
  tuning.chase[0].height_d = 10.0;

  drive::CameraCar car;
  car.box_min = {-0.9, 0.0, -2.1};
  car.box_max = {0.9, 1.3, 2.2};
  car.com = {0.0, 0.5, 0.0};
  car.steering_wheel = {-0.35, 0.95, 0.35};

  drive::CameraInput in;
  in.speed = 35.0;  // ~78 mph
  in.g_longitudinal = 0.85;
  in.g_lateral = 1.1;

  // Baseline chase view.
  drive::DriveCamera cam_base;
  cam_base.set_tuning(tuning);
  cam_base.set_car(car);
  cam_base.set_mode(drive::CameraMode::kFollowLow);
  const auto v_base = cam_base.update(1.0, in);

  // Camera Editor offsets: +0.50m height, +1.00m distance, 120% FOV, 150% stiffness.
  drive::DriveCamera cam_ed;
  cam_ed.set_tuning(tuning);
  cam_ed.set_car(car);
  cam_ed.set_mode(drive::CameraMode::kFollowLow);
  drive::CameraEditorConfig ed;
  ed.chase_height_m = 0.50;
  ed.chase_distance_m = 1.00;
  ed.chase_fov_scale = 1.20;
  ed.chase_stiffness = 1.50;
  cam_ed.set_camera_editor(ed);
  const auto v_ed = cam_ed.update(1.0, in);

  check(std::fabs((v_ed.eye[1] - v_base.eye[1]) - 0.50) < 0.02, "Camera Editor raises chase eye by +0.50 m");
  check((v_base.eye[2] - v_ed.eye[2]) > 0.95, "Camera Editor pulls chase eye back by +1.00 m");
  check(std::fabs(v_ed.fov_degrees - 60.0) < 0.05, "Camera Editor scales chase FOV to 120% (60 deg)");

  // Wheel & Replay driving views.
  ed.driving_view = drive::DrivingView::kWheel;
  cam_ed.set_camera_editor(ed);
  const auto v_wheel = cam_ed.update(1.0 / 60.0, in);
  check(v_wheel.eye[0] < car.box_min[0] && v_wheel.eye[1] < 0.6, "Wheel driving view sits beside front wheel arch");

  ed.driving_view = drive::DrivingView::kReplay;
  cam_ed.set_camera_editor(ed);
  const auto v_replay = cam_ed.update(1.0 / 60.0, in);
  check(v_replay.eye[0] > car.box_max[0] && v_replay.eye[2] < car.box_min[2],
        "Replay driving view places car-relative 3/4 tracking camera");

  ed.reset();
  check(ed.is_default(), "CameraEditorConfig::reset restores defaults");

  // Immersive Camera presets & cockpit vs chase layering.
  drive::ImmersiveCameraConfig imm;
  imm.apply_preset(drive::ImmersivePreset::kStrong);
  check(imm.detect_preset() == drive::ImmersivePreset::kStrong && imm.gforce == 1.5, "Immersive Strong preset = 1.5x");
  imm.lean = 0.75;
  check(imm.detect_preset() == drive::ImmersivePreset::kCustom, "Changing one group switches preset to Custom");

  drive::DriveCamera cam_cockpit_off;
  cam_cockpit_off.set_tuning(tuning);
  cam_cockpit_off.set_car(car);
  cam_cockpit_off.set_mode(drive::CameraMode::kCockpit);
  const auto vc_off = cam_cockpit_off.update(1.0 / 60.0, in);

  drive::DriveCamera cam_cockpit_on;
  cam_cockpit_on.set_tuning(tuning);
  cam_cockpit_on.set_car(car);
  cam_cockpit_on.set_mode(drive::CameraMode::kCockpit);
  imm.apply_preset(drive::ImmersivePreset::kDefault);
  cam_cockpit_on.set_immersive_camera(imm);
  drive::CameraView vc_on{};
  for (int i = 0; i < 12; ++i) vc_on = cam_cockpit_on.update(1.0 / 60.0, in);

  check(vc_on.eye[2] < vc_off.eye[2] - 0.01, "Immersive cockpit pushes camera back under acceleration");
  check(std::fabs(vc_on.up[0] - vc_off.up[0]) > 0.01, "Immersive cockpit leans roll into corners");

  // Hard braking throws camera forward.
  in.g_longitudinal = -1.2;
  in.g_lateral = 0.0;
  for (int i = 0; i < 20; ++i) vc_on = cam_cockpit_on.update(1.0 / 60.0, in);
  check(vc_on.eye[2] > vc_off.eye[2] + 0.01, "Immersive cockpit throws camera forward under braking");

  // UI4 overlay rasterization check.
  carview::CameraEditorOverlay ov;
  ov.open(ed, imm, drive::HeadTrackingConfig{});
  ov.step(+1);
  ov.switch_tab(+1);
  carview::ui::List list;
  carview::draw_camera_editor_ui4(list, 1280.0f, 720.0f, ov);
  std::vector<uint8_t> fb(1280 * 720 * 4, 0);
  carview::ui::rasterize(list.items(), fb, 1280, 720);
  size_t lit = 0;
  for (size_t i = 3; i < fb.size(); i += 4) lit += fb[i] > 0;
  check(lit > 30000, "Camera Editor UI4 overlay rasterizes cleanly at 1280x720");
}

void test_udp_telemetry_layouts() {
  using namespace drive::telemetry;

  Sample s;
  s.driving = true;
  s.redline_rpm = 7500.0f;
  s.idle_rpm = 850.0f;
  s.rpm = 5400.0f;
  s.vel_z = 42.5f;
  s.speed_mps = 42.5f;
  s.pos_x = 123.5f;
  s.pos_y = 45.25f;
  s.pos_z = -980.0f;
  s.wheel_slip_ratio = {0.3f, 0.4f, 0.6f, 0.8f};
  s.wheel_slip_angle = {0.4f, 0.3f, 0.8f, 0.6f};
  s.wheel_speed_rad_s = {120.0f, 121.0f, 125.0f, 126.0f};
  s.suspension_m = {0.03f, 0.04f, 0.05f, 0.06f};
  s.suspension_normalized = {0.25f, 0.35f, 0.50f, 0.65f};
  s.surface_rumble = {0.2f, 0.2f, 0.2f, 0.2f};
  s.throttle = 1.0f;
  s.brake = 0.0f;
  s.clutch = 0.0f;
  s.handbrake = 0.0f;
  s.steer = -0.5f;
  s.gear = 4;
  s.lap_number = 2;
  s.race_position = 1;

  // FM7 Dash (311 bytes).
  const auto fm7 = encode_fm7_dash(s, 98765);
  static_assert(sizeof(fm7) == 311);
  check(read_s32(fm7.data() + 0) == 1, "FM7 IsRaceOn = 1 at offset 0");
  check(read_u32(fm7.data() + 4) == 98765, "FM7 TimestampMS at offset 4");
  check(std::fabs(read_f32(fm7.data() + 8) - 7500.0f) < 1e-3f, "FM7 EngineMaxRpm at offset 8");
  check(std::fabs(read_f32(fm7.data() + 12) - 850.0f) < 1e-3f, "FM7 EngineIdleRpm at offset 12");
  check(std::fabs(read_f32(fm7.data() + 16) - 5400.0f) < 1e-3f, "FM7 CurrentEngineRpm at offset 16");
  check(std::fabs(read_f32(fm7.data() + 40) - 42.5f) < 1e-3f, "FM7 VelocityZ at offset 40");
  check(std::fabs(read_f32(fm7.data() + 84) - 0.3f) < 1e-4f, "FM7 TireSlipRatio[FL] at offset 84");
  check(std::fabs(read_f32(fm7.data() + 180) - 0.5f) < 1e-4f, "FM7 TireCombinedSlip[FL] = hypot(0.3,0.4) = 0.5 at offset 180");
  check(std::fabs(read_f32(fm7.data() + 232) - 123.5f) < 1e-3f, "FM7 PositionX at offset 232");
  check(std::fabs(read_f32(fm7.data() + 244) - 42.5f) < 1e-3f, "FM7 Speed at offset 244");
  check(read_u16(fm7.data() + 300) == 2 && fm7[302] == 1, "FM7 LapNumber@300 and RacePosition@302");
  check(fm7[303] == 255 && fm7[304] == 0 && fm7[307] == 4, "FM7 Accel@303, Brake@304, Gear=4@307");
  check(static_cast<int8_t>(fm7[308]) == -64 || static_cast<int8_t>(fm7[308]) == -63, "FM7 Steer=-0.5@308");

  // Neutral = 11, Reverse = 0.
  s.gear = 0;
  check(encode_fm7_dash(s, 1)[307] == 11, "FM7 neutral gear encodes as 11");
  s.gear = -1;
  check(encode_fm7_dash(s, 1)[307] == 0, "FM7 reverse gear encodes as 0");
  s.gear = 4;

  // FH4/5 Horizon (324 bytes): 12-byte gap after byte 232 shifts dash fields by +12.
  const auto fh4 = encode_fh4_horizon(s, 98765);
  static_assert(sizeof(fh4) == 324);
  check(read_s32(fh4.data() + 0) == 1 && read_u32(fh4.data() + 4) == 98765, "FH4 Sled header matches");
  check(std::fabs(read_f32(fh4.data() + 244) - 123.5f) < 1e-3f, "FH4 PositionX shifted +12 to offset 244");
  check(std::fabs(read_f32(fh4.data() + 256) - 42.5f) < 1e-3f, "FH4 Speed shifted +12 to offset 256");
  check(fh4[315] == 255 && fh4[319] == 4, "FH4 Accel@315 and Gear@319 shifted +12");

  // Menus / paused -> IsRaceOn == 0 and all telemetry fields zeroed.
  s.driving = false;
  const auto paused = encode_fm7_dash(s, 100000);
  check(read_s32(paused.data() + 0) == 0 && read_u32(paused.data() + 4) == 100000 &&
            read_f32(paused.data() + 16) == 0.0f && paused[303] == 0,
        "Menus/paused sends IsRaceOn=0 with zeroed payload");

  // Sampler with real Vehicle + moving_only + 0.5s frozen-sim auto race-off.
  drive::CarParams params;
  const double wr = 0.32;
  params.wheel_centres = {drive::Vec3{-0.78, wr, 1.30}, drive::Vec3{0.78, wr, 1.30},
                          drive::Vec3{-0.79, wr, -1.25}, drive::Vec3{0.79, wr, -1.25}};
  params.tyre_radius = {wr, wr, wr, wr};
  params.centre_of_mass = {0.0, 0.5, 0.05};
  drive::Vehicle veh(params);
  drive::FlatGround ground(0.0);
  veh.reset({10.0, 0.0, 20.0}, 0.0);
  Sampler sampler;
  const auto stopped_mo = sampler.build(veh, true, 1.0 / 60.0, true);
  check(!stopped_mo.driving, "moving_only sends race-off while car stands still");

  drive::Input gas;
  gas.throttle = 1.0;
  for (int i = 0; i < 60; ++i) veh.step(gas, ground);
  const auto moving = sampler.build(veh, true, 1.0 / 60.0, true);
  check(moving.driving && moving.speed_mps > 1.0f && moving.rpm > 1000.0f,
        "Sampler reads live RPM and speed from Vehicle");

  // Simulate paused clock (same veh.time() for 0.6 s) -> auto race-off.
  Sample frozen{};
  for (int i = 0; i < 40; ++i) frozen = sampler.build(veh, true, 1.0 / 60.0, false);
  check(!frozen.driving, "Sampler auto-silences shakers after 0.5 s frozen sim");

  // Probe CSV logger.
  const auto csv = std::filesystem::temp_directory_path() / "shift_s12_telemetry_probe.csv";
  ProbeRecorder probe;
  check(probe.start(csv), "Telemetry probe CSV opens");
  probe.record(100, moving);
  probe.stop();
  check(probe.rows_written() == 1, "Telemetry probe CSV wrote 1 row");
  std::ifstream in(csv);
  std::string hdr;
  std::getline(in, hdr);
  check(hdr.find("timestamp_ms,race_on,rpm,speed_mps,gear") != std::string::npos, "Telemetry probe CSV header valid");
  in.close();
  std::error_code ec;
  std::filesystem::remove(csv, ec);
}

void test_camera_parity_layers_and_pullin() {
  // 1) CameraPhysics.xml SteeringWheel layer parsing & output kick.
  const std::string xml = R"(
<CameraPhysics>
  <FollowCam n="0"></FollowCam>
  <FollowCam2 n="0"></FollowCam2>
  <DriverCam n="1">
    <Layer Enabled="1" InputType="SurfaceBumpOffset_FL" OutputType="SteeringWheel"
           NoiseType="None" InputToParamMapping="Linear"
           Param.x="15.0" Param.y="0" Param.z="0" Param.w="0" />
  </DriverCam>
  <Hood n="0"></Hood>
  <BumperHigh n="0"></BumperHigh>
</CameraPhysics>)";
  drive::CameraTuning t;
  std::string err;
  check(drive::parse_camera_physics(xml, t, &err), "parse_camera_physics accepts SteeringWheel layer");
  check(t.layers[size_t(drive::CameraMode::kCockpit)].size() == 1, "SteeringWheel layer retained on kCockpit");
  drive::CameraCar car;
  car.box_min = {-0.9, 0.0, -2.1};
  car.box_max = {0.9, 1.3, 2.2};
  car.com = {0.0, 0.5, 0.0};
  car.steering_wheel = {-0.35, 0.85, 0.30};
  car.overrides["CamHoodPitchOffset"] = 3.5;

  drive::DriveCamera cam;
  cam.set_tuning(t);
  cam.set_car(car);
  cam.set_mode(drive::CameraMode::kCockpit);
  drive::CameraInput in;
  in.surface_bump[0] = 0.2;
  const auto vc = cam.update(1.0 / 60.0, in);
  check(std::fabs(vc.steering_wheel_kick) > 0.01, "SteeringWheel layer produces non-zero kick in radians");

  // 2) Hood near_clip halved when CamHoodPitchOffset != 0.
  cam.set_mode(drive::CameraMode::kHood);
  const auto vh = cam.update(1.0 / 60.0, in);
  check(std::fabs(vh.near_clip - t.hood_near * 0.5) < 1e-6, "hood near_clip halves when CamHoodPitchOffset != 0");

  // 3) Chase camera horizontal wall pull-in brings eye closer when blocked.
  t.chase[0].height = 0.5;
  t.chase[0].radius = 1.0;
  t.chase[0].fov = 50.0;
  cam.set_tuning(t);
  cam.set_mode(drive::CameraMode::kFollowLow);
  const auto v_open = cam.update(1.0, in);
  const double d_open = std::hypot(v_open.eye[0], v_open.eye[2]);
  in.obstacle_ray = [](const std::array<double, 3>&, const std::array<double, 3>&, double, double& hit_d) {
    hit_d = 1.8;
    return true;
  };
  const auto v_wall = cam.update(1.0 / 60.0, in);
  const double d_wall = std::hypot(v_wall.eye[0], v_wall.eye[2]);
  check(d_wall < d_open - 1.0, "chase camera pulls in horizontally when obstacle_ray hits wall");

  // 4) Car without cockpit mesh skips kCockpit in cycle() and set_mode().
  car.has_cockpit_mesh = false;
  cam.set_car(car);
  cam.set_mode(drive::CameraMode::kFollowHigh);
  cam.cycle();
  check(cam.mode() == drive::CameraMode::kHood, "cycle() skips kCockpit when !has_cockpit_mesh");
}

void test_camera_collision_restraint_and_restart() {
  // PT-28: Camera collision impulse layers distinguish light glancing fence scrapes
  // from heavy solid-wall crashes, prevent multi-frame impulse stacking, bound total
  // kick, recover crisply with damped spring, and clamp sudden chase yaw_rate spikes.
  const std::string xml = R"(
<CameraPhysics>
  <FollowCam n="2">
    <Layer Enabled="1" InputType="WorldCollision_Lateral" OutputType="CameraSpaceYPRImpulse"
           NoiseType="None" InputToParamMapping="Linear"
           Param.x="4.0" Param.y="5.0" Param.z="2.0" Param.w="0"
           Sprung="1" SpringK="45.0" SpringDPercent="0.40" ImpulseInputSizeCutoff="0.8" />
    <Layer Enabled="1" InputType="WorldCollision_Longitudinal" OutputType="CameraSpaceXYZImpulse"
           NoiseType="None" InputToParamMapping="Linear"
           Param.x="0.0" Param.y="0.3" Param.z="1.2" Param.w="0"
           Sprung="1" SpringK="45.0" SpringDPercent="0.40" ImpulseInputSizeCutoff="0.8" />
  </FollowCam>
  <FollowCam2 n="0"></FollowCam2>
  <DriverCam n="0"></DriverCam>
  <Hood n="0"></Hood>
  <BumperHigh n="0"></BumperHigh>
</CameraPhysics>)";
  drive::CameraTuning t;
  t.chase[0].height = 0.6;
  t.chase[0].radius = 1.2;
  t.chase[0].fov = 52.0;
  t.chase[0].yaw_k = 40.0;
  t.chase[0].yaw_d = 12.0;
  t.chase[0].height_k = 30.0;
  t.chase[0].height_d = 10.0;
  std::string err;
  check(drive::parse_camera_physics(xml, t, &err), "parse_camera_physics accepts collision impulse layers");

  drive::CameraCar car;
  car.box_min = {-0.9, 0.0, -2.1};
  car.box_max = {0.9, 1.3, 2.2};
  car.com = {0.0, 0.5, 0.0};

  drive::DriveCamera cam;
  cam.set_tuning(t);
  cam.set_car(car);
  cam.set_mode(drive::CameraMode::kFollowLow);

  drive::CameraInput in;
  in.speed = 25.0;
  const auto base = cam.update(1.0, in);

  auto angle_deg = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
    const double d = std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0, 1.0);
    return std::acos(d) * (180.0 / 3.14159265358979323846);
  };
  auto dist3 = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                     (a[2] - b[2]) * (a[2] - b[2]));
  };

  // Sustained light fence scrape for 20 frames at 120 FPS must NOT stack impulses.
  double light_max_angle = 0.0;
  double light_max_disp = 0.0;
  for (int i = 0; i < 20; ++i) {
    drive::CameraInput scrape = in;
    scrape.world_collision_lateral = 6.0;
    const auto v = cam.update(1.0 / 120.0, scrape);
    light_max_angle = std::max(light_max_angle, angle_deg(v.forward, base.forward));
    light_max_disp = std::max(light_max_disp, dist3(v.eye, base.eye));
  }
  check(light_max_angle > 0.05 && light_max_angle < 1.5,
        "light fence scrape produces gentle bounded angular kick without stacking");
  check(light_max_disp < 0.25, "light fence scrape keeps positional kick bounded");

  // Smooth damped recovery back to neutral within 1.0 s.
  drive::CameraView settled{};
  for (int i = 0; i < 60; ++i) settled = cam.update(1.0 / 60.0, in);
  check(angle_deg(settled.forward, base.forward) < 0.15, "camera angular kick recovers cleanly to neutral");
  check(dist3(settled.eye, base.eye) < 0.03, "camera positional kick recovers cleanly to neutral");

  // Heavy solid-wall impact feels stronger than a fence brush yet stays strictly clamped.
  double heavy_max_angle = 0.0;
  double heavy_max_disp = 0.0;
  for (int i = 0; i < 12; ++i) {
    drive::CameraInput crash = in;
    if (i < 3) {
      crash.world_collision_lateral = 35.0;
      crash.world_collision_longitudinal = 45.0;
    }
    const auto v = cam.update(1.0 / 60.0, crash);
    heavy_max_angle = std::max(heavy_max_angle, angle_deg(v.forward, base.forward));
    heavy_max_disp = std::max(heavy_max_disp, dist3(v.eye, base.eye));
  }
  check(heavy_max_angle > light_max_angle * 1.4, "heavy wall impact has stronger tactile feedback than light scrape");
  check(heavy_max_angle <= 3.6, "heavy wall angular kick stays strictly bounded");
  check(heavy_max_disp <= 0.30, "heavy wall positional kick stays strictly bounded");

  // Sudden fence-clip yaw_rate shock does not whip the chase camera wildly off-axis.
  cam.restart();
  cam.update(1.0, in);
  double max_lat_whip = 0.0;
  for (int i = 0; i < 6; ++i) {
    drive::CameraInput whip = in;
    whip.yaw_rate = 10.0;
    const auto v = cam.update(1.0 / 60.0, whip);
    max_lat_whip = std::max(max_lat_whip, std::fabs(v.eye[0]));
  }
  check(max_lat_whip < 1.2, "sudden yaw_rate shock does not whip chase camera wildly");

  // PT-31 helper: restart_count() increments on restart() and clear_pending_restart()
  // cancels the pending snap while preserving the monotonic counter.
  const uint64_t before_r = cam.restart_count();
  cam.restart();
  check(cam.restart_count() == before_r + 1, "DriveCamera::restart_count increments on restart()");
  cam.clear_pending_restart();
  check(cam.restart_count() == before_r + 1, "clear_pending_restart preserves restart_count");
}

void test_tunnel_ceiling_clearance_and_3ray_fan() {
  drive::CameraTuning t;
  t.chase[0].height = 0.5;
  t.chase[0].radius = 1.0;
  t.chase[0].fov = 50.0;
  t.chase[0].yaw_k = 40.0;
  t.chase[0].yaw_d = 12.0;
  t.chase[0].height_k = 30.0;
  t.chase[0].height_d = 10.0;
  t.chase[1] = t.chase[0];
  t.chase[1].height = 0.85;
  t.chase[1].fov = 58.0;

  drive::CameraCar car;
  car.box_min = {-0.9, 0.0, -2.1};
  car.box_max = {0.9, 1.3, 2.2};
  car.com = {0.0, 0.5, 0.0};

  // 1) R3-PHY-04: Tunnel/overpass roof at y = 2.60 m (road at y = 0.0 m).
  // Previously probing ground_below from eye[1] + 2.0 hit the overpass deck
  // and snapped eye[1] up onto the roof (2.90 m). Must stay inside the tunnel!
  {
    drive::DriveCamera cam;
    cam.set_tuning(t);
    cam.set_car(car);
    cam.set_mode(drive::CameraMode::kFollowLow);
    drive::CameraInput in;
    in.ground_below = [](double, double y, double, double& gy) {
      gy = (y >= 2.60) ? 2.60 : 0.0;
      return true;
    };
    const auto v = cam.update(1.0, in);
    check(v.eye[1] >= 0.30 && v.eye[1] < 2.50,
          "chase camera stays inside tunnel and never roof-snaps onto overpass deck");
  }

  // 2) R3-PHY-04: Explicit low tunnel ceiling at y = 2.05 m clamps high chase eye
  // strictly within [ground_y + 0.30, ceiling_y - 0.25] = [0.30, 1.80].
  {
    drive::DriveCamera cam;
    cam.set_tuning(t);
    cam.set_car(car);
    cam.set_mode(drive::CameraMode::kFollowHigh);
    drive::CameraInput in;
    in.ground_below = [](double, double y, double, double& gy) {
      gy = (y >= 2.05) ? 2.05 : 0.0;
      return true;
    };
    in.ceiling_above = [](double, double y, double, double& cy) {
      if (y < 2.05) {
        cy = 2.05;
        return true;
      }
      return false;
    };
    const auto v = cam.update(1.0, in);
    check(v.eye[1] >= 0.30 && v.eye[1] <= 1.801,
          "low tunnel ceiling clamps FollowHigh eye to [ground + 0.30, ceiling - 0.25]");
  }

  // 3) R3-PHY-15: 3-ray horizontal fan (center + +-0.35 m right offsets) catches
  // a tunnel wall/girder at x = +0.25 m that the center ray (x = 0) misses.
  {
    drive::DriveCamera cam;
    cam.set_tuning(t);
    cam.set_car(car);
    cam.set_mode(drive::CameraMode::kFollowLow);
    drive::CameraInput in;
    const auto v_open = cam.update(1.0, in);
    const double d_open = std::hypot(v_open.eye[0], v_open.eye[2]);

    int ray_calls = 0;
    in.obstacle_ray = [&](const std::array<double, 3>& origin, const std::array<double, 3>& dir, double max_dist,
                          double& hit_d) {
      // Ignore upward ceiling probe if called.
      if (dir[1] > 0.9) return false;
      ++ray_calls;
      // Vertical wall plane at x = +0.25 m spanning z in [-6.0, -1.0].
      if (dir[0] > 1e-6) {
        const double dist = (0.25 - origin[0]) / dir[0];
        if (dist > 0.0 && dist < max_dist) {
          const double hz = origin[2] + dir[2] * dist;
          if (hz >= -6.0 && hz <= -1.0) {
            hit_d = dist;
            return true;
          }
        }
      }
      return false;
    };
    const auto v_fan = cam.update(1.0 / 60.0, in);
    const double d_fan = std::hypot(v_fan.eye[0], v_fan.eye[2]);
    check(ray_calls == 3, "chase obstacle pull-in casts 3-ray horizontal fan");
    check(d_fan < d_open - 0.5, "3-ray horizontal fan pulls chase eye in for side wall missed by center ray");
  }

  // 4) R3-PHY-15: Nearby vehicle OBB registry intersects box truck within 12 m
  // and ignores distant vehicles beyond 12 m.
  {
    drive::CameraVehicleObb near_truck;
    near_truck.center = {0.0, 1.5, -3.5};
    near_truck.half_extents = {1.15, 1.5, 1.2};
    drive::CameraVehicleObb far_truck;
    far_truck.center = {0.0, 1.5, -18.0};
    far_truck.half_extents = {1.15, 1.5, 2.0};
    drive::set_camera_vehicle_obbs(drive::CameraObstacleSource::kTraffic, {near_truck, far_truck});

    double hit_d = 0.0;
    const bool hit_near = drive::intersect_camera_vehicle_obbs({0.0, 1.3, 0.0}, {0.0, 0.0, -1.0}, 10.0,
                                                               {0.0, 0.0, 0.0}, 12.0, hit_d);
    check(hit_near && hit_d > 2.0 && hit_d < 2.6, "nearby traffic box truck OBB intersects chase ray accurately");

    // Remove near truck; far truck at 18 m (> 12 m player filter) must not trigger pull-in.
    drive::set_camera_vehicle_obbs(drive::CameraObstacleSource::kTraffic, {far_truck});
    const bool hit_far = drive::intersect_camera_vehicle_obbs({0.0, 1.3, 0.0}, {0.0, 0.0, -1.0}, 25.0,
                                                              {0.0, 0.0, 0.0}, 12.0, hit_d);
    check(!hit_far, "vehicle OBBs beyond 12 m player radius are filtered out");
    drive::clear_camera_vehicle_obbs(drive::CameraObstacleSource::kTraffic);
  }
}

}  // namespace

int main() {
  test_opentrack_parser_and_timeout();
  test_camera_editor_and_immersive();
  test_udp_telemetry_layouts();
  test_camera_parity_layers_and_pullin();
  test_camera_collision_restraint_and_restart();
  test_tunnel_ceiling_clearance_and_3ray_fan();
  if (g_failures > 0) {
    std::fprintf(stderr, "camera_telemetry_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("camera_telemetry_tests: PASS\n");
  return 0;
}
