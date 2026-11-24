#include "drive/replay.h"
#include "replay_view.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {

int g_fails = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_fails;
  }
}

drive::ReplayCarSample make_car(uint32_t slot, float x, float y, float z, float yaw_rad, float speed, float rpm,
                                int8_t gear) {
  drive::ReplayCarSample s;
  s.slot = slot;
  s.pos[0] = x;
  s.pos[1] = y;
  s.pos[2] = z;
  s.quat[0] = std::cos(0.5f * yaw_rad);
  s.quat[1] = 0.0f;
  s.quat[2] = std::sin(0.5f * yaw_rad);
  s.quat[3] = 0.0f;
  s.speed_mps = speed;
  s.rpm = rpm;
  s.redline_rpm = 7800.0f;
  s.gear = gear;
  s.throttle_u8 = 210;
  s.brake_u8 = 0;
  s.steer_i8 = 24;
  s.race_place = uint8_t(slot + 1);
  s.lamp_bits = drive::kReplayLampHeadlights | drive::kReplayLampBrake;
  for (int i = 0; i < 4; ++i) {
    s.wheel_steer[i] = (i < 2) ? 0.15f : 0.0f;
    s.wheel_spin[i] = z * 0.4f;
    s.wheel_omega[i] = speed / 0.33f;
    s.wheel_y[i] = -0.35f;
  }
  return s;
}

void test_ring_buffer_and_interpolation() {
  drive::ReplayMetadata meta;
  meta.event_name = "TEST SPRINT";
  meta.is_race = true;
  meta.sample_hz = 60.0;
  meta.roster = {
      {0, 1219, "NIS_SkylineGTRVSpecII_02", "PLAYER", true, false},
      {1, 1031, "SUB_ImprezaWRXSTI_11", "RIVAL", false, false},
  };

  // Ring capacity 120 frames (2 s at 60 Hz); push 180 frames (3 s) to test wrap.
  drive::ReplayRecorder rec;
  rec.begin(meta, 120, true);
  rec.add_marker(0.2, "OLD_DROPPED");
  rec.add_marker(1.5, "SECTOR 1");
  rec.add_marker(2.8, "FINISH");

  for (int i = 0; i < 180; ++i) {
    const double t = double(i) / 60.0;
    std::vector<drive::ReplayCarSample> cars = {
        make_car(0, 0.0f, 10.0f, float(t * 40.0), 0.1f * float(t), 40.0f, 5200.0f + float(i), 3),
        make_car(1, 3.5f, 10.0f, float(t * 38.0), 0.1f * float(t), 38.0f, 4900.0f, 3),
    };
    rec.maybe_record(t, std::move(cars));
  }
  check(rec.frame_count() == 120, "ring buffer capped at 120 frames after wrap");

  const drive::ReplayRecording snap = rec.snapshot();
  check(snap.frame_count() == 120, "snapshot has 120 frames");
  check(std::fabs(snap.start_time()) < 1e-6, "snapshot normalized to start at t=0");
  check(std::fabs(snap.duration() - (119.0 / 60.0)) < 1e-3, "snapshot duration ~1.983s");
  check(snap.meta.markers.size() == 2, "old marker outside ring dropped, 2 in-window markers kept");

  // Mid-frame interpolation at t = 0.50833s (between frames 30 and 31).
  drive::ReplayCarSample s0{};
  check(snap.sample_car(0.508333333, 0, s0), "sample_car succeeds");
  // Original time for ring start is frame 60 = 1.0s -> t_abs = 1.508333s -> z = 40 * 1.508333 = 60.333.
  check(std::fabs(s0.pos[2] - 60.333f) < 0.15f, "interpolated Z matches linear trajectory");

  // Lamp bit pack/unpack round-trip & upright quaternion mapping.
  const auto lamps = drive::unpack_replay_lamps(s0.lamp_bits);
  check(lamps.headlamps_on && lamps.brake_on && !lamps.reverse_on, "lamp bits unpack accurately");
  drive::Vehicle::Pose pose_chk{};
  s0.apply_to_pose(pose_chk);
  const drive::Vec3 up_chk = drive::rotate(pose_chk.rot, {0.0, 1.0, 0.0});
  check(up_chk.y > 0.99, "apply_to_pose keeps car upright (Quat xyzw order)");
}

void test_file_roundtrip_and_crc() {
  drive::ReplayMetadata meta;
  meta.event_name = "COLORADO CANYON";
  meta.world_name = "Colorado";
  meta.is_race = true;
  meta.player_place = 1;
  meta.field_size = 8;
  meta.roster.push_back({0, 1219, "NIS_SkylineGTRVSpecII_02", "PLAYER", true, false});

  drive::ReplayRecorder rec;
  rec.begin(meta, 300, false);
  for (int i = 0; i < 90; ++i) {
    const double t = double(i) / 60.0;
    rec.record_frame(t, {make_car(0, 10.0f, 20.0f, float(t * 30.0), 0.25f, 30.0f, 6000.0f, 4)});
  }
  rec.add_marker(0.75, "LAP 2");
  const auto orig = rec.snapshot();

  const auto tmp_dir = std::filesystem::temp_directory_path() / "shift_replay_tests";
  std::error_code ec;
  std::filesystem::remove_all(tmp_dir, ec);

  std::filesystem::path saved;
  std::string err;
  check(drive::save_replay_auto(tmp_dir, orig, saved, &err), "save_replay_auto succeeds");
  check(std::filesystem::exists(saved), "saved .shrep file exists");

  drive::ReplayRecording loaded;
  check(drive::load_replay_file(saved, loaded, &err), "load_replay_file succeeds");
  check(loaded.meta.event_name == "COLORADO CANYON", "event name preserved");
  check(loaded.frame_count() == orig.frame_count(), "frame count preserved");
  check(loaded.meta.markers.size() == 1 && loaded.meta.markers[0].label == "LAP 2", "markers preserved");
  check(std::fabs(loaded.frames.back().cars[0].pos[2] - orig.frames.back().cars[0].pos[2]) < 1e-4f,
        "final car Z preserved");

  const auto listed = drive::list_replay_files(tmp_dir);
  check(listed.size() == 1 && listed[0].event_name == "COLORADO CANYON", "list_replay_files finds saved replay");

  // Flip one byte in the payload -> CRC32 must reject it.
  {
    std::fstream f(saved, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(28);
    char b = 0;
    f.read(&b, 1);
    b ^= 0x5A;
    f.seekp(28);
    f.write(&b, 1);
  }
  drive::ReplayRecording bad;
  check(!drive::load_replay_file(saved, bad, &err), "corrupted replay file rejected by CRC32");

  std::filesystem::remove_all(tmp_dir, ec);
}

void test_replay_viewer_controls_cameras_and_hud() {
  drive::ReplayRecorder rec;
  drive::ReplayMetadata meta;
  meta.event_name = "FESTIVAL SHOWDOWN";
  meta.is_race = true;
  meta.field_size = 2;
  meta.roster = {
      {0, 1219, "NIS_SkylineGTRVSpecII_02", "PLAYER", true, false},
      {1, 1031, "SUB_ImprezaWRXSTI_11", "RIVAL", false, false},
  };
  rec.begin(meta, 300, false);
  for (int i = 0; i < 180; ++i) {
    const double t = double(i) / 60.0;
    rec.record_frame(t, {make_car(0, 0.0f, 0.0f, float(t * 35.0), 0.0f, 35.0f, 5500.0f, 3),
                         make_car(1, 4.0f, 0.0f, float(t * 34.0), 0.0f, 34.0f, 5100.0f, 3)});
  }

  carview::ReplayViewer viewer;
  viewer.open(rec.snapshot(), true);
  check(viewer.active() && !viewer.paused(), "viewer opens playing");

  for (int i = 0; i < 5; ++i) viewer.update(0.1);
  check(std::fabs(viewer.time() - 0.5) < 0.02, "advances 0.5s at 1x");

  viewer.set_slow_motion();
  check(std::fabs(viewer.speed() - 0.5) < 1e-6, "slow-motion first press -> 0.5x");
  viewer.set_slow_motion();
  check(std::fabs(viewer.speed() - 0.25) < 1e-6, "slow-motion second press -> 0.25x");
  for (int i = 0; i < 4; ++i) viewer.update(0.1);
  check(std::fabs(viewer.time() - 0.6) < 0.02, "advances 0.1s over 0.4s at 0.25x");

  viewer.trigger_rewind();
  check(viewer.speed() < 0.0, "rewind sets negative speed");
  for (int i = 0; i < 2; ++i) viewer.update(0.1);
  check(viewer.time() < 0.5, "rewind moves playback time backward");

  viewer.step_frame(+1);
  check(viewer.paused(), "step_frame pauses playback");

  viewer.cycle_focus_car(+1);
  check(viewer.focus_slot() == 1, "cycle_focus_car switches to opponent slot 1");

  // Exercise all 8 camera modes (including Cockpit & Hood).
  check(int(carview::ReplayCameraMode::kCount) == 8, "ReplayViewer exposes all 8 camera modes");
  for (int m = 0; m < int(carview::ReplayCameraMode::kCount); ++m) {
    viewer.set_camera_mode(static_cast<carview::ReplayCameraMode>(m));
    const auto cam = viewer.compute_camera(0.016, 0.2f, 0.1f, 0.0f);
    check(std::isfinite(cam.eye[0]) && std::isfinite(cam.forward[2]) && cam.fov_degrees > 10.0,
          "camera mode produces valid finite CameraView");
    if (static_cast<carview::ReplayCameraMode>(m) == carview::ReplayCameraMode::kCockpit) {
      check(cam.interior, "Cockpit replay camera sets interior flag");
    }
  }

  // Phase 6B: DVR Repeat looping and 5-page Telemetry cycling.
  viewer.set_repeat(true);
  viewer.set_paused(false);
  viewer.set_speed(1.0);
  viewer.seek(viewer.duration() - 0.02);
  viewer.update(0.05);
  check(!viewer.paused() && viewer.time() < 0.1, "DVR Repeat loops playback back to 0.0s cleanly");
  for (int tp = 0; tp < int(carview::ReplayTelemetryPage::kCount); ++tp) {
    viewer.cycle_telemetry_page(+1);
    carview::ui::List tlist;
    viewer.build_hud(tlist, 1280.0f, 720.0f);
    check(!tlist.empty(), "Replay telemetry page builds valid HUD");
  }

  // Verify HUD emits UI instances.
  carview::ui::List list;
  viewer.build_hud(list, 1280.0f, 720.0f);
  check(!list.empty(), "ReplayViewer::build_hud emits timeline & telemetry UI shapes");
}

void test_real_telemetry_v3_and_units() {
  // 1. Real physics telemetry capture via ReplayCarSample::from_vehicle + CarDamageState.
  drive::CarParams params;
  params.wheel_centres = {drive::Vec3{-0.8, 0.33, 1.35}, drive::Vec3{0.8, 0.33, 1.35},
                          drive::Vec3{-0.8, 0.33, -1.35}, drive::Vec3{0.8, 0.33, -1.35}};
  params.tyre_radius = {0.33, 0.33, 0.33, 0.33};
  params.has_turbo = true;
  params.boost_min_psi = 2.0;
  params.boost_max_psi = 18.0;
  drive::Vehicle veh(params);
  drive::FlatGround ground(0.0);
  veh.reset({0.0, 0.0, 0.0}, 0.0);
  drive::Input inp{};
  inp.throttle = 1.0;
  inp.steer = 0.35;
  for (int i = 0; i < 60; ++i) veh.step(inp, ground);

  drive::CarDamageState dmg;
  dmg.mode = drive::DamageMode::kSimulation;
  dmg.engine_damage = 0.40f;
  dmg.trans_damage = 0.25f;
  dmg.suspension_damage = {0.30f, 0.10f, 0.20f, 0.05f};
  dmg.aero_damage = 0.50f;
  drive::CarLampState lamps{};

  const drive::ReplayCarSample s_veh =
      drive::ReplayCarSample::from_vehicle(0u, veh, lamps, inp.throttle, 0.0, 0.0, inp.steer, 1u, &dmg);
  check(s_veh.engine_torque_nm > 10.0f, "from_vehicle records real positive engine torque");
  check(s_veh.engine_power_kw > 5.0f, "from_vehicle records real positive engine power (kW)");
  check(s_veh.boost_max_psi >= 17.5f, "from_vehicle records forced induction boost_max_psi");
  check(std::fabs(s_veh.damage[0] - 0.40f) < 1e-4f, "from_vehicle records real engine damage");
  check(s_veh.damage[1] >= 0.249f, "from_vehicle records real transmission damage");
  check(std::fabs(s_veh.damage[2] - 0.30f) < 1e-4f, "from_vehicle records real suspension damage");
  check(std::fabs(s_veh.damage[3] - 0.50f) < 1e-4f, "from_vehicle records real aero/body damage");
  check(s_veh.wheel_compression[0] > 0.0f && s_veh.wheel_compression[0] <= 1.0f,
        "from_vehicle records normalized wheel suspension compression");
  check(s_veh.wheel_temp_c[0] >= 20.0f, "from_vehicle records real tyre temperature (deg C)");

  // 2. Interpolation and v3 binary file round-trip preserve all extended telemetry fields.
  drive::ReplayCarSample a = make_car(0, 0.0f, 0.0f, 0.0f, 0.0f, 20.0f, 4000.0f, 3);
  a.wheel_slip[0] = 0.20f;
  a.wheel_slip[1] = 0.40f;
  a.wheel_slip[2] = 0.60f;
  a.wheel_slip[3] = 0.80f;
  a.wheel_compression[0] = 0.30f;
  a.wheel_compression[1] = 0.50f;
  a.wheel_compression[2] = 0.40f;
  a.wheel_compression[3] = 0.60f;
  a.wheel_temp_c[0] = 70.0f;
  a.wheel_temp_c[1] = 75.0f;
  a.wheel_temp_c[2] = 80.0f;
  a.wheel_temp_c[3] = 85.0f;
  a.damage[0] = 0.20f;
  a.damage[1] = 0.10f;
  a.damage[2] = 0.30f;
  a.damage[3] = 0.40f;
  a.engine_power_kw = 200.0f;
  a.engine_torque_nm = 400.0f;
  a.boost_psi = 8.0f;
  a.boost_max_psi = 20.0f;

  drive::ReplayCarSample b = a;
  b.pos[2] = 20.0f;
  b.wheel_slip[0] = 0.60f;
  b.wheel_compression[0] = 0.70f;
  b.wheel_temp_c[0] = 90.0f;
  b.damage[0] = 0.60f;
  b.engine_power_kw = 300.0f;
  b.engine_torque_nm = 500.0f;
  b.boost_psi = 16.0f;

  drive::ReplayRecorder rec;
  drive::ReplayMetadata meta;
  meta.event_name = "TELEMETRY V3 TEST";
  meta.roster = {{0, 1219, "NIS_SkylineGTRVSpecII_02", "PLAYER", true, false}};
  rec.begin(meta, 120, false);
  rec.record_frame(0.0, {a});
  rec.record_frame(1.0, {b});
  const auto snap = rec.snapshot();

  drive::ReplayCarSample mid{};
  check(snap.sample_car(0.5, 0, mid), "midpoint telemetry sample succeeds");
  check(std::fabs(mid.wheel_slip[0] - 0.40f) < 1e-3f, "wheel_slip interpolates linearly");
  check(std::fabs(mid.wheel_compression[0] - 0.50f) < 1e-3f, "wheel_compression interpolates linearly");
  check(std::fabs(mid.wheel_temp_c[0] - 80.0f) < 1e-2f, "wheel_temp_c interpolates linearly");
  check(std::fabs(mid.damage[0] - 0.40f) < 1e-3f, "damage interpolates linearly");
  check(std::fabs(mid.engine_power_kw - 250.0f) < 1e-2f, "engine_power_kw interpolates linearly");
  check(std::fabs(mid.engine_torque_nm - 450.0f) < 1e-2f, "engine_torque_nm interpolates linearly");
  check(std::fabs(mid.boost_psi - 12.0f) < 1e-2f, "boost_psi interpolates linearly");

  const auto tmp_dir = std::filesystem::temp_directory_path() / "shift_replay_v3_tests";
  std::error_code ec;
  std::filesystem::remove_all(tmp_dir, ec);
  std::filesystem::path saved;
  std::string err;
  check(drive::save_replay_auto(tmp_dir, snap, saved, &err), "save_replay_auto v3 succeeds");
  drive::ReplayRecording loaded;
  check(drive::load_replay_file(saved, loaded, &err), "load_replay_file v3 succeeds");
  check(loaded.frame_count() == 2 && !loaded.frames[0].cars.empty(), "v3 file restored 2 frames");
  if (loaded.frame_count() == 2 && !loaded.frames[0].cars.empty()) {
    const auto& lf = loaded.frames[0].cars[0];
    check(std::fabs(lf.wheel_slip[3] - 0.80f) < 1e-4f, "v3 roundtrip preserves wheel_slip");
    check(std::fabs(lf.wheel_compression[1] - 0.50f) < 1e-4f, "v3 roundtrip preserves wheel_compression");
    check(std::fabs(lf.wheel_temp_c[2] - 80.0f) < 1e-3f, "v3 roundtrip preserves wheel_temp_c");
    check(std::fabs(lf.damage[3] - 0.40f) < 1e-4f, "v3 roundtrip preserves damage");
    check(std::fabs(lf.engine_power_kw - 200.0f) < 1e-3f, "v3 roundtrip preserves engine_power_kw");
    check(std::fabs(lf.engine_torque_nm - 400.0f) < 1e-3f, "v3 roundtrip preserves engine_torque_nm");
    check(std::fabs(lf.boost_psi - 8.0f) < 1e-3f && std::fabs(lf.boost_max_psi - 20.0f) < 1e-3f,
          "v3 roundtrip preserves boost_psi and boost_max_psi");
  }
  std::filesystem::remove_all(tmp_dir, ec);

  // 3. Metric vs Imperial telemetry formatting (KM/H vs MPH, BAR vs PSI, kW vs HP, Nm vs LB-FT, C vs F).
  const std::string spd_kmh = carview::format_replay_speed(27.7778f, true);   // 100 km/h
  const std::string spd_mph = carview::format_replay_speed(27.7778f, false);  // 62 mph
  check(spd_kmh.find("100") != std::string::npos && spd_kmh.find("KM/H") != std::string::npos,
        "metric replay speed formats as KM/H");
  check(spd_mph.find("62") != std::string::npos && spd_mph.find("MPH") != std::string::npos,
        "imperial replay speed formats as MPH");

  const std::string bst_bar = carview::format_replay_boost(14.5038f, true, true);   // ~1.00 BAR
  const std::string bst_psi = carview::format_replay_boost(14.5038f, true, false);  // 14.5 PSI
  check(bst_bar.find("1.00") != std::string::npos && bst_bar.find("BAR") != std::string::npos,
        "metric replay boost formats as BAR");
  check(bst_psi.find("14.5") != std::string::npos && bst_psi.find("PSI") != std::string::npos,
        "imperial replay boost formats as PSI");
  check(carview::format_replay_boost(0.0f, false, true) == "N/A", "NA boost formats as N/A");

  const std::string pwr_kw = carview::format_replay_power(200.0f, true);
  const std::string pwr_hp = carview::format_replay_power(200.0f, false);
  check(pwr_kw.find("200") != std::string::npos && pwr_kw.find("kW") != std::string::npos,
        "metric replay power formats as kW");
  check(pwr_hp.find("268") != std::string::npos && pwr_hp.find("HP") != std::string::npos,
        "imperial replay power formats as HP");

  const std::string trq_nm = carview::format_replay_torque(400.0f, true);
  const std::string trq_lb = carview::format_replay_torque(400.0f, false);
  check(trq_nm.find("400") != std::string::npos && trq_nm.find("Nm") != std::string::npos,
        "metric replay torque formats as Nm");
  check(trq_lb.find("295") != std::string::npos && trq_lb.find("LB-FT") != std::string::npos,
        "imperial replay torque formats as LB-FT");

  const std::string tmp_c = carview::format_replay_temp(100.0f, true);
  const std::string tmp_f = carview::format_replay_temp(100.0f, false);
  check(tmp_c.find("100") != std::string::npos && tmp_c.find("C") != std::string::npos,
        "metric replay tyre temp formats in deg C");
  check(tmp_f.find("212") != std::string::npos && tmp_f.find("F") != std::string::npos,
        "imperial replay tyre temp formats in deg F");

  // Exercise all 5 ReplayViewer telemetry pages under both metric and imperial modes.
  carview::ReplayViewer viewer;
  viewer.open(snap, true);
  for (bool metric : {true, false}) {
    viewer.set_metric_units(metric);
    check(viewer.metric_units() == metric, "ReplayViewer stores metric_units flag");
    for (int tp = 0; tp < int(carview::ReplayTelemetryPage::kCount); ++tp) {
      viewer.set_telemetry_page(static_cast<carview::ReplayTelemetryPage>(tp));
      carview::ui::List tl;
      viewer.build_hud(tl, 1280.0f, 720.0f);
      check(!tl.empty(), "ReplayViewer builds real telemetry HUD page cleanly");
    }
  }
}

}  // namespace

int main() {
  test_ring_buffer_and_interpolation();
  test_file_roundtrip_and_crc();
  test_replay_viewer_controls_cameras_and_hud();
  test_real_telemetry_v3_and_units();
  if (g_fails == 0) {
    std::printf("replay_tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "replay_tests: %d failed\n", g_fails);
  return 1;
}
