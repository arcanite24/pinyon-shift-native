// Unit tests for drive::RewindBuffer (SPEC-REWIND, Tests 1-6).
#include <cmath>
#include <cstdio>

#include "drive/rewind.h"
#include "drive/vehicle.h"

namespace {

int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

bool near(double a, double b, double tol = 1e-3) { return std::fabs(a - b) <= tol; }

drive::CarParams test_car() {
  drive::CarParams p;
  const double r = 0.32;
  p.wheel_centres = {drive::Vec3{-0.78, r, 1.30}, drive::Vec3{0.78, r, 1.30},
                     drive::Vec3{-0.79, r, -1.25}, drive::Vec3{0.79, r, -1.25}};
  p.tyre_radius = {r, r, r, r};
  p.centre_of_mass = {0, 0.5, 0.05};
  return p;
}

drive::RewindCameraState make_cam(double z) {
  drive::RewindCameraState c;
  c.has_view = true;
  c.view.eye = {0.0, 2.0, z - 5.0};
  c.view.forward = {0.0, 0.0, 1.0};
  c.view.up = {0.0, 1.0, 0.0};
  c.view.fov_degrees = 50.0;
  c.look_angle = 0.0;
  return c;
}

void drive_for(drive::Vehicle& v, const drive::FlatGround& g, drive::RewindBuffer& rb,
               double duration_s, const drive::Input& in, bool capture_allowed = true) {
  const int steps = int(std::llround(duration_s / drive::Vehicle::kStep));
  for (int i = 0; i < steps; ++i) {
    v.step(in, g);
    rb.record_step(v.time(), v.time(), v, make_cam(v.position().z), capture_allowed);
  }
}

// Test 1: Keyframe cadence (10 s initial, 2.5 s periodic) & 8-slot FIFO wrap.
void test_cadence_and_wrap() {
  drive::Vehicle v(test_car());
  drive::FlatGround g;
  drive::RewindBuffer rb;
  rb.start_session(0.0);

  drive::Input in;
  in.throttle = 0.7;

  // Before 10 s: 0 keyframes, cannot trigger.
  drive_for(v, g, rb, 9.9, in);
  CHECK(rb.keyframe_count() == 0);
  CHECK(!rb.can_trigger());

  // Cross 10.0 s: 1st keyframe captured.
  drive_for(v, g, rb, 0.2, in);
  CHECK(rb.keyframe_count() == 1);
  CHECK(near(rb.keyframe(0).replay_time, 10.0, 0.02));
  CHECK(rb.can_trigger());

  // Advance by 2.5 s increments up to 8 slots (t = 10.0 .. 27.5).
  for (int k = 2; k <= 8; ++k) {
    drive_for(v, g, rb, 2.5, in);
    CHECK(rb.keyframe_count() == k);
    CHECK(near(rb.keyframe(k - 1).replay_time, 10.0 + (k - 1) * 2.5, 0.02));
  }

  // Past 30 s: holds at 8 slots and overwrites oldest (FIFO).
  const double old_first = rb.keyframe(0).replay_time;
  drive_for(v, g, rb, 5.0, in);
  CHECK(rb.keyframe_count() == 8);
  CHECK(rb.keyframe(0).replay_time > old_first + 4.5);
  CHECK(near(rb.keyframe(7).replay_time - rb.keyframe(0).replay_time, 7 * 2.5, 0.05));
}

// Test 2: First-keyframe ignore (< 1.0 s when count > 2) vs normal (>= 1.0 s).
void test_first_keyframe_ignore() {
  drive::Vehicle v(test_car());
  drive::FlatGround g;
  drive::RewindBuffer rb;
  rb.start_session(0.0);

  drive::Input in;
  in.throttle = 0.6;
  // Drive to t = 15.4 s: keyframes at 10.0, 12.5, 15.0 (count == 3, delta == 0.4 s < 1.0 s).
  drive_for(v, g, rb, 15.4, in);
  CHECK(rb.keyframe_count() == 3);
  CHECK(rb.trigger(v.time(), v.time(), v, make_cam(v.position().z)));
  // Should skip slot 2 (15.0 s) and target slot 1 (12.5 s) -> ~2.9 s back.
  CHECK(rb.target_slot() == 1);
  const double target_t = rb.keyframe(1).replay_time;
  CHECK(near(v.time() - target_t, 2.9, 0.1));

  // Cancel and advance to t = 16.5 s (delta == 1.5 s >= 1.0 s): should target slot 2 (15.0 s).
  CHECK(rb.cancel(v));
  drive_for(v, g, rb, 1.1, in);
  CHECK(rb.keyframe_count() == 3);
  CHECK(rb.trigger(v.time(), v.time(), v, make_cam(v.position().z)));
  CHECK(rb.target_slot() == 2);
}

// Test 3: Multi-step Rewind Further & slot 0 end-of-buffer clamp.
void test_rewind_further_and_clamp() {
  drive::Vehicle v(test_car());
  drive::FlatGround g;
  drive::RewindBuffer rb;
  rb.start_session(0.0);

  drive::Input in;
  in.throttle = 0.5;
  // Keyframes at 10.0, 12.5, 15.0, 17.5 -> trigger at 19.0 (targets slot 3 = 17.5).
  drive_for(v, g, rb, 19.0, in);
  CHECK(rb.keyframe_count() == 4);
  CHECK(rb.trigger(v.time(), v.time(), v, make_cam(v.position().z)));
  CHECK(rb.target_slot() == 3);

  // Scrub to slot 3.
  for (int i = 0; i < 40; ++i) rb.update(0.05);
  CHECK(rb.is_paused_at_keyframe());
  CHECK(rb.selected_slot() == 3);
  CHECK(rb.target_slot() == drive::RewindConfig::kIdleSlot);
  CHECK(rb.ui_state() == drive::RewindUiState::kStopRewind);

  // Rewind further -> slot 2 -> slot 1 -> slot 0.
  for (int expected = 2; expected >= 0; --expected) {
    CHECK(rb.rewind_further());
    CHECK(rb.is_rewinding());
    CHECK(rb.ui_state() == drive::RewindUiState::kLoopRewind);
    CHECK(rb.target_slot() == expected);
    for (int i = 0; i < 60; ++i) rb.update(0.05);
    CHECK(rb.is_paused_at_keyframe());
    CHECK(rb.selected_slot() == expected);
  }

  // At slot 0: Rewind Further is clamped and returns false.
  CHECK(rb.at_oldest_slot());
  CHECK(!rb.rewind_further());
  CHECK(rb.selected_slot() == 0);
}

// Test 4: Resume vs Cancel, pose interpolation, 1.0 s settle, clean-lap invalidation.
void test_resume_and_cancel() {
  drive::Vehicle v(test_car());
  drive::FlatGround g;
  drive::RewindBuffer rb;
  rb.start_session(0.0);

  drive::Input in;
  in.throttle = 0.8;
  drive_for(v, g, rb, 16.5, in);
  CHECK(rb.keyframe_count() == 3);

  const drive::Vec3 pos_at_trigger = v.position();
  const double speed_at_trigger = v.speed();
  const double rpm_at_trigger = v.rpm();
  const double time_at_trigger = v.time();

  // Trigger and scrub halfway -> verify interpolated pose is strictly between kf2 and trigger.
  CHECK(rb.trigger(v.time(), v.time(), v, make_cam(v.position().z)));
  rb.update(0.75);  // halfway from 16.5 to 15.0
  CHECK(rb.is_rewinding());
  const auto mid_pose = rb.sampled_pose();
  const double kf2_z = rb.keyframe(2).vehicle.position().z;
  CHECK(mid_pose.pos.z > kf2_z && mid_pose.pos.z < pos_at_trigger.z);
  drive::CameraView cam_mid{};
  CHECK(rb.sampled_camera(cam_mid));
  CHECK(cam_mid.eye[2] < pos_at_trigger.z);

  // Cancel returns exact trigger state and does NOT mark lap uncertified.
  drive::RewindCameraState restored_cam;
  double restored_t = 0.0;
  CHECK(rb.cancel(v, &restored_cam, &restored_t));
  CHECK(!rb.active());
  CHECK(!rb.lap_uncertified());
  CHECK(near(v.position().z, pos_at_trigger.z, 1e-6));
  CHECK(near(v.speed(), speed_at_trigger, 1e-6));
  CHECK(near(v.rpm(), rpm_at_trigger, 1e-6));
  CHECK(near(restored_t, time_at_trigger, 1e-6));

  // Trigger again, scrub to slot 2, then Resume -> restores kf2, settles 1.0 s, marks lap dirty.
  CHECK(rb.trigger(v.time(), v.time(), v, make_cam(v.position().z)));
  for (int i = 0; i < 40; ++i) rb.update(0.05);
  CHECK(rb.is_paused_at_keyframe());
  const drive::Vec3 kf2_pos = rb.keyframe(2).vehicle.position();
  const double kf2_speed = rb.keyframe(2).vehicle.speed();
  const double kf2_time = rb.keyframe(2).replay_time;

  CHECK(rb.resume(v, &restored_cam, &restored_t));
  CHECK(rb.is_settling());
  CHECK(rb.lap_uncertified());
  CHECK(near(v.position().z, kf2_pos.z, 1e-6));
  CHECK(near(v.speed(), kf2_speed, 1e-6));
  CHECK(near(restored_t, kf2_time, 1e-6));
  CHECK(near(rb.next_capture_time(), kf2_time + 2.5, 1e-6));

  // Still settling at 0.9 s; idle at 1.0 s.
  rb.update(0.9);
  CHECK(rb.is_settling());
  rb.update(0.15);
  CHECK(!rb.active());
  CHECK(rb.state() == drive::RewindState::kIdle);
  CHECK(rb.ui_state() == drive::RewindUiState::kHidden);
}

// Test 5: History clears, lockouts, Rewind OFF, capture suppression.
void test_clears_and_lockouts() {
  drive::Vehicle v(test_car());
  drive::FlatGround g;
  drive::RewindBuffer rb;
  rb.start_session(0.0);

  drive::Input in;
  in.throttle = 0.5;

  // Capture suppressed -> no keyframes recorded past 10 s.
  drive_for(v, g, rb, 11.0, in, /*capture_allowed=*/false);
  CHECK(rb.keyframe_count() == 0);
  CHECK(!rb.can_trigger());

  // Re-allow capture -> captures immediately on next step.
  drive_for(v, g, rb, 0.05, in, /*capture_allowed=*/true);
  CHECK(rb.keyframe_count() == 1);
  CHECK(rb.can_trigger());

  // Rewind OFF blocks trigger.
  rb.set_enabled(false);
  CHECK(!rb.can_trigger());
  CHECK(!rb.trigger(v.time(), v.time(), v, make_cam(v.position().z)));
  rb.set_enabled(true);
  CHECK(rb.can_trigger());

  // CDisableMulligan lockout blocks trigger.
  rb.set_disabled(true);
  CHECK(!rb.can_trigger());
  rb.set_disabled(false);
  CHECK(rb.can_trigger());

  // CResetMulliganHistory clears buffer and reschedules first keyframe (+10 s).
  rb.reset_history(v.time());
  CHECK(rb.keyframe_count() == 0);
  CHECK(rb.target_slot() == drive::RewindConfig::kIdleSlot);
  CHECK(!rb.can_trigger());
  CHECK(near(rb.next_capture_time(), v.time() + 10.0, 1e-6));
}

// Test 6: Audio mixer/DSP/TapeStart/vehicle state & tutorial hint cooldown.
void test_audio_and_hints() {
  drive::Vehicle v(test_car());
  drive::FlatGround g;
  drive::RewindBuffer rb;
  rb.start_session(0.0);

  drive::Input in;
  in.throttle = 0.5;
  drive_for(v, g, rb, 13.5, in);

  // Audio DSP constants match SPEC-REWIND section 6.
  CHECK(!rb.audio().dsp.bypass);
  CHECK(near(rb.audio().dsp.gain, 0.30, 1e-6));
  CHECK(near(rb.audio().dsp.bandwidth, 3.359995, 1e-6));
  CHECK(near(rb.audio().dsp.center_hz, 3772.0, 1e-6));

  // Trigger activates snapshot, DSP, TapeStart cue, vehicle audio state 104.
  CHECK(rb.trigger(v.time(), v.time(), v, make_cam(v.position().z)));
  CHECK(rb.audio().snapshot_active);
  CHECK(rb.audio().dsp_active);
  CHECK(rb.audio().vehicle_audio_state == 104);
  CHECK(rb.consume_tape_start_cue());
  CHECK(!rb.consume_tape_start_cue());

  // After 0.2 s fade-in: full ducking gains match AudioMixerSnapshots.xml.
  rb.update(0.2);
  CHECK(near(rb.audio().blend, 1.0, 1e-6));
  CHECK(near(rb.audio().focus_car_gain(), 0.175, 1e-6));
  CHECK(near(rb.audio().game_music_gain(), 0.25, 1e-6));
  CHECK(near(rb.audio().effects_gain(), 0.0, 1e-6));
  CHECK(rb.audio().vehicle_audio_state == 10);

  // Reach keyframe -> vehicle audio state 0.
  for (int i = 0; i < 30; ++i) rb.update(0.05);
  CHECK(rb.is_paused_at_keyframe());
  CHECK(rb.audio().vehicle_audio_state == 0);

  // Cancel -> snapshot/DSP off, vehicle audio state 1, 0.1 s return fade.
  CHECK(rb.cancel(v));
  CHECK(!rb.audio().snapshot_active);
  CHECK(!rb.audio().dsp_active);
  CHECK(rb.audio().vehicle_audio_state == 1);
  rb.update(0.1);
  CHECK(near(rb.audio().blend, 0.0, 1e-6));
  CHECK(near(rb.audio().focus_car_gain(), 1.0, 1e-6));

  // Tutorial hint cooldown (30 s cooldown, 5 s visible, 10 max shows).
  CHECK(rb.hints_remaining() == 10);
  CHECK(rb.request_hint(10.0));
  CHECK(rb.hints_remaining() == 9);
  CHECK(rb.hint_visible(12.0));
  CHECK(!rb.hint_visible(15.1));
  CHECK(!rb.request_hint(25.0));  // still in 30 s cooldown
  CHECK(rb.request_hint(40.0));   // cooldown elapsed
  CHECK(rb.hints_remaining() == 8);
}

}  // namespace

int main() {
  test_cadence_and_wrap();
  test_first_keyframe_ignore();
  test_rewind_further_and_clamp();
  test_resume_and_cancel();
  test_clears_and_lockouts();
  test_audio_and_hints();
  if (failures) {
    std::printf("rewind_tests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("rewind_tests: all passed\n");
  return 0;
}
