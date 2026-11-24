// Unit tests for drive::RewindBuffer (SPEC-REWIND, Tests 1-6).
#include <cmath>
#include <cstdio>

#include "drive/ai_driver.h"
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

// P0-4: Monotonic snapshot_id tracking, skill-chain timer preservation,
// VIEW REPLAY request latch, and AiField snapshot/restore parity.
void test_world_snapshot_ids_and_view_replay() {
  drive::Vehicle v(test_car());
  drive::FlatGround g;
  drive::RewindBuffer rb;
  rb.start_session(0.0);

  drive::Input in;
  in.throttle = 0.65;
  std::vector<uint64_t> captured_ids;
  double race_clock = 0.0;
  // Step for 22.0 s (Vehicle::kStep = 1/360 s -> 7920 steps) to capture >= 5 keyframes.
  for (int i = 0; i < 7920; ++i) {
    v.step(in, g);
    race_clock = v.time();
    const uint64_t sid = rb.record_step(v.time(), race_clock, v, make_cam(v.position().z), true,
                                        /*skill_score=*/1500 + static_cast<uint64_t>(i * 10),
                                        /*skill_mult=*/3, /*skill_timer=*/4.25);
    if (sid != 0) captured_ids.push_back(sid);
  }
  CHECK(captured_ids.size() >= 5);
  for (size_t i = 1; i < captured_ids.size(); ++i) CHECK(captured_ids[i] > captured_ids[i - 1]);
  CHECK(!captured_ids.empty() && rb.latest_snapshot_id() == captured_ids.back());

  const auto active = rb.active_snapshot_ids();
  CHECK(active.size() == rb.keyframe_count());

  // Trigger rewind with live skill-chain timer and verify cancel_snapshot_id is assigned.
  CHECK(rb.trigger(v.time(), race_clock, v, make_cam(v.position().z), 12000, 5, 3.75));
  const uint64_t cancel_id = rb.cancel_snapshot_id();
  CHECK(!captured_ids.empty() && cancel_id > captured_ids.back());
  CHECK(rb.selected_snapshot_id() != 0);

  // Advance until paused at keyframe, test VIEW REPLAY latch.
  for (int i = 0; i < 40; ++i) rb.update(0.1);
  CHECK(rb.is_paused_at_keyframe());
  CHECK(!rb.consume_view_replay_request());
  rb.request_view_replay();
  CHECK(rb.consume_view_replay_request());
  CHECK(!rb.consume_view_replay_request());

  // Resume returns selected snapshot_id, race clock, and skill-chain timer.
  const uint64_t expected_resume_id = rb.selected_snapshot_id();
  double out_rt = 0.0, out_rc = 0.0, out_sk_timer = 0.0;
  uint64_t out_sk_score = 0, out_sid = 0;
  int out_sk_mult = 0;
  CHECK(rb.resume(v, nullptr, &out_rt, &out_rc, &out_sk_score, &out_sk_mult, &out_sid, &out_sk_timer));
  CHECK(out_sid == expected_resume_id);
  CHECK(out_sk_score >= 1500);
  CHECK(out_sk_mult == 3);
  CHECK(near(out_sk_timer, 4.25, 1e-6));
  CHECK(out_rc < race_clock);
  CHECK(rb.lap_uncertified());

  // Finish the 1.0 s post-resume settle phase, then re-trigger and Cancel.
  rb.update(1.1);
  CHECK(rb.trigger(v.time(), race_clock, v, make_cam(v.position().z), 9999, 4, 2.5));
  const uint64_t cancel_id2 = rb.cancel_snapshot_id();
  for (int i = 0; i < 40; ++i) rb.update(0.1);
  uint64_t out_cancel_sid = 0;
  double out_cancel_timer = 0.0;
  CHECK(rb.cancel(v, nullptr, nullptr, nullptr, &out_sk_score, &out_sk_mult, &out_cancel_sid, &out_cancel_timer));
  CHECK(out_cancel_sid == cancel_id2);
  CHECK(out_sk_score == 9999 && out_sk_mult == 4);
  CHECK(near(out_cancel_timer, 2.5, 1e-6));

  // AiField::snapshot() / restore() restores progress, lane, skill, and caps.
  drive::RacingLine line;
  for (int i = 0; i < 40; ++i) {
    line.x.push_back(0.0f);
    line.z.push_back(static_cast<float>(i * 10));
    line.dist.push_back(static_cast<float>(i * 10));
    line.speed.push_back(30.0f);
  }
  drive::AiField field(line, 2);
  field.place(0, 20.0, -1.5f, 0.9f);
  field.place(1, 50.0, 1.5f, 1.0f);
  const auto ai_snap = field.snapshot();
  field.place(0, 180.0, 2.0f, 1.2f);
  field.set_speed_cap(0, 5.0f);
  field.restore(ai_snap);
  CHECK(near(field.progress(0), 20.0, 1e-6));
  CHECK(near(field.progress(1), 50.0, 1e-6));
  CHECK(near(field.lane(0), -1.5, 1e-5));
  CHECK(near(field.snapshot().skill[0], 0.9, 1e-5));
  CHECK(near(field.snapshot().cap[0], ai_snap.cap[0], 1e-5));
}

void test_sampled_snapshot_bracket_for_visual_scrub() {
  const drive::FlatGround flat;
  drive::Vehicle v(test_car());
  v.launch({0, 0, 0}, 0, 20.0);
  drive::RewindBuffer rb;
  rb.start_session(0.0);
  double race_clock = 0.0;
  std::vector<uint64_t> kf_ids;
  for (int i = 0; i < 120 * 18; ++i) {
    v.step({0, 0.5, 0, false}, flat);
    race_clock += drive::Vehicle::kStep;
    const uint64_t cid = rb.record_step(v.time(), race_clock, v, make_cam(v.position().z), true);
    if (cid != 0u) kf_ids.push_back(cid);
  }
  CHECK(kf_ids.size() >= 3);
  CHECK(rb.trigger(v.time(), race_clock, v, make_cam(v.position().z)));
  const uint64_t cancel_id = rb.cancel_snapshot_id();
  CHECK(cancel_id != 0u);

  // Immediately after trigger, playback_time is at cancel_snapshot (alpha near 1.0).
  drive::RewindBuffer::RewindBracket br0{};
  CHECK(rb.sampled_snapshot_bracket(br0));
  CHECK(br0.snap_b == cancel_id && br0.alpha >= 0.99);

  // Mid-scrub: alpha moves smoothly into (0, 1) between adjacent snapshot ids.
  rb.update(0.8);
  drive::RewindBuffer::RewindBracket br_mid{};
  CHECK(rb.sampled_snapshot_bracket(br_mid));
  CHECK(br_mid.snap_a != 0u && br_mid.snap_b != 0u);
  CHECK(br_mid.alpha >= 0.0 && br_mid.alpha <= 1.0);
  CHECK(br_mid.alpha < br0.alpha || br_mid.snap_b != br0.snap_b);
}

void test_extra_state_and_invalid_anchor_rejection() {
  const drive::FlatGround flat;
  drive::Vehicle v(test_car());
  drive::RewindBuffer rb;
  rb.start_session(0.0);

  drive::Input in;
  in.throttle = 0.6;
  // Drive to 9.95 s (just before 10.0 s first keyframe).
  drive_for(v, flat, rb, 9.95, in);
  CHECK(rb.keyframe_count() == 0);

  // Place vehicle mid-air (all 4 wheels off ground): stream records, but NO keyframe captured.
  v.place({0.0, 25.0, 100.0}, 0.0);
  for (int i = 0; i < 60; ++i) {
    v.step(in, flat);
    const uint64_t sid = rb.record_step(v.time(), v.time(), v, make_cam(v.position().z), true);
    CHECK(sid == 0u);
  }
  CHECK(rb.keyframe_count() == 0);

  // Place vehicle upside-down (rolled over, up_y < 0.5): still refused as resume anchor.
  v.place({0.0, 0.5, 100.0}, 0.0);
  auto rolled_pose = v.pose();
  rolled_pose.rot = {0.0, 0.0, 1.0, 0.0};  // 180 deg roll about Z -> up_y == -1.0
  v.set_pose(rolled_pose);
  CHECK(v.is_rolled_over());
  CHECK(rb.record_step(v.time() + 0.1, v.time() + 0.1, v, make_cam(v.position().z), true) == 0u);
  CHECK(rb.keyframe_count() == 0);

  // Place upright on flat ground with populated RewindExtraState -> captures valid keyframe immediately.
  v.place({0.0, 0.0, 100.0}, 0.0);
  v.step({}, flat);
  drive::RewindExtraState ex_kf;
  ex_kf.rotor_heat = {0.65f, 0.70f, 0.30f, 0.35f};
  ex_kf.prev_rumble_gear = 3;
  ex_kf.cam_state = 2;
  ex_kf.prev_audio_checkpoint = 7;
  ex_kf.prev_audio_lap = 2;
  ex_kf.speed_trap_state = {1, 0, 1};
  ex_kf.pr_stunt_state = {2, 4};
  ex_kf.signs_state = {0xFF, 0x0A};
  const uint64_t sid = rb.record_step(11.0, 11.0, v, make_cam(v.position().z), true, 500, 2, 1.5, &ex_kf);
  CHECK(sid != 0u);
  CHECK(rb.keyframe_count() == 1);
  CHECK(rb.keyframe(0).prev_rumble_gear == 3 && rb.keyframe(0).cam_state == 2);
  CHECK(rb.keyframe(0).prev_audio_checkpoint == 7 && rb.keyframe(0).prev_audio_lap == 2);
  CHECK(rb.keyframe(0).signs_state.size() == 2 && rb.keyframe(0).signs_state[1] == 0x0A);

  // Trigger with distinct cancel extra state, test Cancel round-trip then Resume round-trip.
  drive::RewindExtraState ex_cancel;
  ex_cancel.rotor_heat = {0.9f, 0.9f, 0.8f, 0.8f};
  ex_cancel.prev_rumble_gear = 5;
  ex_cancel.cam_state = 4;
  ex_cancel.prev_audio_checkpoint = 9;
  ex_cancel.prev_audio_lap = 3;
  ex_cancel.speed_trap_state = {1, 1, 1};
  ex_cancel.pr_stunt_state = {5};
  ex_cancel.signs_state = {0xFF, 0xFF};
  CHECK(rb.trigger(12.5, 12.5, v, make_cam(v.position().z), 900, 3, 2.0, &ex_cancel));
  drive::RewindExtraState out_ex;
  CHECK(rb.cancel(v, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &out_ex));
  CHECK(near(out_ex.rotor_heat[0], 0.9, 1e-5) && out_ex.prev_rumble_gear == 5 && out_ex.cam_state == 4);
  CHECK(out_ex.prev_audio_checkpoint == 9 && out_ex.prev_audio_lap == 3);
  CHECK(out_ex.speed_trap_state == ex_cancel.speed_trap_state && out_ex.pr_stunt_state == ex_cancel.pr_stunt_state &&
        out_ex.signs_state == ex_cancel.signs_state);

  CHECK(rb.trigger(12.5, 12.5, v, make_cam(v.position().z), 900, 3, 2.0, &ex_cancel));
  for (int i = 0; i < 40; ++i) rb.update(0.05);
  CHECK(rb.is_paused_at_keyframe());
  drive::RewindExtraState res_ex;
  CHECK(rb.resume(v, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &res_ex));
  CHECK(near(res_ex.rotor_heat[1], 0.70, 1e-5) && res_ex.prev_rumble_gear == 3 && res_ex.cam_state == 2);
  CHECK(res_ex.prev_audio_checkpoint == 7 && res_ex.prev_audio_lap == 2);
  CHECK(res_ex.speed_trap_state == ex_kf.speed_trap_state && res_ex.pr_stunt_state == ex_kf.pr_stunt_state &&
        res_ex.signs_state == ex_kf.signs_state);
}

}  // namespace

int main() {
  test_cadence_and_wrap();
  test_first_keyframe_ignore();
  test_rewind_further_and_clamp();
  test_resume_and_cancel();
  test_clears_and_lockouts();
  test_audio_and_hints();
  test_world_snapshot_ids_and_view_replay();
  test_sampled_snapshot_bracket_for_visual_scrub();
  test_extra_state_and_invalid_anchor_rejection();
  if (failures) {
    std::printf("rewind_tests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("rewind_tests: all passed\n");
  return 0;
}
