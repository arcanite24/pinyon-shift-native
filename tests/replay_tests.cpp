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

  // Exercise all 6 camera modes.
  for (int m = 0; m < int(carview::ReplayCameraMode::kCount); ++m) {
    viewer.set_camera_mode(static_cast<carview::ReplayCameraMode>(m));
    const auto cam = viewer.compute_camera(0.016, 0.2f, 0.1f, 0.0f);
    check(std::isfinite(cam.eye[0]) && std::isfinite(cam.forward[2]) && cam.fov_degrees > 10.0,
          "camera mode produces valid finite CameraView");
  }

  // Verify HUD emits UI instances.
  carview::ui::List list;
  viewer.build_hud(list, 1280.0f, 720.0f);
  check(!list.empty(), "ReplayViewer::build_hud emits timeline & telemetry UI shapes");
}

}  // namespace

int main() {
  test_ring_buffer_and_interpolation();
  test_file_roundtrip_and_crc();
  test_replay_viewer_controls_cameras_and_hud();
  if (g_fails == 0) {
    std::printf("replay_tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "replay_tests: %d failed\n", g_fails);
  return 1;
}
