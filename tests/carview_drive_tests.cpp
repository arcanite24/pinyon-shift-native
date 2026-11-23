// Non-corpus unit tests for M0b drive helpers and world route collision masks
// (no SHIFT_GAME_ROOT required).
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "drive_helpers.h"
#include "world/collision.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

void test_collision_route_mask() {
  world::Collision col;
  CHECK(col.active_route() == 0);
  col.set_active_route(12);
  CHECK(col.active_route() == 12);
  col.set_active_route(0);
  CHECK(col.active_route() == 0);

  world::CollisionMesh cm;
  cm.surface = {world::kSurfaceAsphalt, world::kSurfaceAsphalt, world::kSurfaceAsphalt,
                world::kSurfaceCameraCollision};
  cm.word = {0x8000, 0x0001, 0x0001, 0x8000};
  cm.active = {0x8000, 0x8001, 0x8000, 0x8000};
  CHECK(world::Collision::collides(cm, 0));   // 0x8000 always collides
  CHECK(world::Collision::collides(cm, 1));   // route bit 0x0001 active
  CHECK(!world::Collision::collides(cm, 2));  // route bit 0x0001 inactive
  CHECK(!world::Collision::collides(cm, 3));  // camera-only surface never collides
}

void test_rig_accel_state() {
  carview::RigAccelState st;
  const drive::Quat identity{0, 0, 0, 1};
  double gl = 99, gt = 99;
  // First sample has no previous velocity -> 0.
  st.sample({0, 0, 0}, identity, 0.016, gl, gt);
  CHECK(near(gl, 0.0) && near(gt, 0.0));

  // Steady +9.81 m/s^2 along +Z -> g_longitudinal == +1.0, g_lateral == 0.
  st.sample({0, 0, 9.81 * 0.1}, identity, 0.1, gl, gt);
  CHECK(near(gl, 1.0, 1e-5) && near(gt, 0.0, 1e-5));

  // Large velocity jump after reset() must NOT spike.
  st.reset();
  st.sample({100, 0, 500}, identity, 0.016, gl, gt);
  CHECK(near(gl, 0.0) && near(gt, 0.0));

  // dt >= 0.25 outputs 0.
  st.sample({100, 0, 550}, identity, 0.25, gl, gt);
  CHECK(near(gl, 0.0) && near(gt, 0.0));

  // Non-finite velocity resets RigAccelState and outputs (0, 0) without poisoning the next finite sample.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  st.sample({nan, 0, 0}, identity, 0.016, gl, gt);
  CHECK(near(gl, 0.0) && near(gt, 0.0) && !st.have_last);
  st.sample({0, inf, 0}, identity, 0.016, gl, gt);
  CHECK(near(gl, 0.0) && near(gt, 0.0) && !st.have_last);
  st.sample({10, 0, 0}, identity, 0.016, gl, gt);
  CHECK(near(gl, 0.0) && near(gt, 0.0) && st.have_last);
}

void test_engine_audio_view() {
  CHECK(carview::engine_audio_view(drive::CameraMode::kFollowLow) == audio::View::kFollow);
  CHECK(carview::engine_audio_view(drive::CameraMode::kFollowHigh) == audio::View::kFollow);
  CHECK(carview::engine_audio_view(drive::CameraMode::kCockpit) == audio::View::kCockpit);
  CHECK(carview::engine_audio_view(drive::CameraMode::kHood) == audio::View::kFront);
  CHECK(carview::engine_audio_view(drive::CameraMode::kBumper) == audio::View::kFront);
}

void test_blend_traffic_pose() {
  const carview::TrafficPose a{{0, 0, 0}, {0, 0, 0, 1}};
  const double s45 = std::sin(3.14159265358979323846 / 8.0);
  const double c45 = std::cos(3.14159265358979323846 / 8.0);
  const carview::TrafficPose b{{2, 0, 4}, {0, s45, 0, c45}};

  const auto p0 = carview::blend_traffic_pose(a, b, 0.0);
  CHECK(near(p0.pos.x, 0.0) && near(p0.pos.z, 0.0) && near(p0.rot.w, 1.0));

  const auto p5 = carview::blend_traffic_pose(a, b, 0.5);
  CHECK(near(p5.pos.x, 1.0) && near(p5.pos.z, 2.0));
  const double qnorm = std::sqrt(p5.rot.x * p5.rot.x + p5.rot.y * p5.rot.y + p5.rot.z * p5.rot.z + p5.rot.w * p5.rot.w);
  CHECK(near(qnorm, 1.0, 1e-6) && p5.rot.y > 0.0 && p5.rot.w > 0.0);

  // Antipodal quaternion -b.rot must take shortest arc (positive w/y midpoint, not degenerate).
  const carview::TrafficPose b_neg{{2, 0, 4}, {0, -s45, 0, -c45}};
  const auto p5_neg = carview::blend_traffic_pose(a, b_neg, 0.5);
  CHECK(near(std::fabs(p5_neg.rot.y), std::fabs(p5.rot.y), 1e-6));
  CHECK(near(std::fabs(p5_neg.rot.w), std::fabs(p5.rot.w), 1e-6));

  const auto p1 = carview::blend_traffic_pose(a, b, 1.0);
  CHECK(near(p1.pos.x, 2.0) && near(p1.pos.z, 4.0) && near(p1.rot.y, s45));

  // > 5 m displacement snaps to b even at alpha == 0.5.
  const carview::TrafficPose far_b{{6.0, 0, 0}, {0, s45, 0, c45}};
  const auto snapped = carview::blend_traffic_pose(a, far_b, 0.5);
  CHECK(near(snapped.pos.x, 6.0) && near(snapped.rot.y, s45));

  // Non-finite alpha falls back to t = 0.0 (start-of-step pose `a`).
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (double bad_alpha : {nan, inf, -inf}) {
    const auto p_bad = carview::blend_traffic_pose(a, b, bad_alpha);
    CHECK(std::isfinite(p_bad.pos.x) && std::isfinite(p_bad.pos.y) && std::isfinite(p_bad.pos.z));
    CHECK(std::isfinite(p_bad.rot.x) && std::isfinite(p_bad.rot.y) && std::isfinite(p_bad.rot.z) &&
          std::isfinite(p_bad.rot.w));
    CHECK(near(p_bad.pos.x, 0.0) && near(p_bad.pos.z, 0.0) && near(p_bad.rot.w, 1.0));
  }
}

void test_sync_picker_index() {
  // Before a garage exists, baked_cars holds all baked cars and pick_ids is empty.
  const std::vector<std::string> full_baked = {
      "aud_r8_09", "bug_veyronss_11", "fer_250gto_62", "nis_skylinegtrvspecii_02", "sub_imprezawrxsti_11"};
  const size_t initial_pick = carview::sync_picker_index(full_baked, {}, 0, "nis_skylinegtrvspecii_02");
  CHECK(initial_pick == 3);

  // After Barn Find award_garage_car + garage_picker(), baked_cars shrinks to 1-2 garage cars.
  // Re-syncing must move stale index 3 onto the active garage car (< shrunk size).
  const std::vector<std::string> garage_two = {"fer_250gto_62", "nis_skylinegtrvspecii_02"};
  const std::vector<int64_t> garage_two_ids = {2, 1};
  const size_t synced_two = carview::sync_picker_index(garage_two, garage_two_ids, 1, "nis_skylinegtrvspecii_02");
  CHECK(synced_two == 1 && synced_two < garage_two.size());

  // Single-car garage shrinks to size 1 -> index 0.
  const std::vector<std::string> garage_one = {"fer_250gto_62"};
  const std::vector<int64_t> garage_one_ids = {2};
  CHECK(carview::sync_picker_index(garage_one, garage_one_ids, 2, "fer_250gto_62") == 0);

  // Missing selected_garage_id falls back to active_car_lower, else 0; empty baked_cars returns 0.
  CHECK(carview::sync_picker_index(garage_two, garage_two_ids, 999, "nis_skylinegtrvspecii_02") == 1);
  CHECK(carview::sync_picker_index(garage_two, garage_two_ids, 999, "unknown_car") == 0);
  CHECK(carview::sync_picker_index({}, {}, 1, "nis_skylinegtrvspecii_02") == 0);
}

void test_cli_size_parsing() {
  uint32_t w = 960, h = 540;
  CHECK(carview::parse_size_arg("1920x1080", w, h));
  CHECK(w == 1920 && h == 1080);
  CHECK(carview::parse_size_arg("1280x720", w, h));
  CHECK(w == 1280 && h == 720);
  CHECK(carview::parse_size_arg("3840x2160", w, h));
  CHECK(w == 3840 && h == 2160);

  // Invalid inputs must fail without corrupting previous dimensions.
  CHECK(!carview::parse_size_arg("0x1080", w, h));
  CHECK(!carview::parse_size_arg("1920x0", w, h));
  CHECK(!carview::parse_size_arg("1920", w, h));
  CHECK(!carview::parse_size_arg("bad", w, h));
  CHECK(!carview::parse_size_arg("1920x1080px", w, h));
  CHECK(!carview::parse_size_arg("", w, h));
  CHECK(!carview::parse_size_arg(nullptr, w, h));
  CHECK(w == 3840 && h == 2160);
}

}  // namespace

int main() {
  test_collision_route_mask();
  test_rig_accel_state();
  test_engine_audio_view();
  test_blend_traffic_pose();
  test_sync_picker_index();
  test_cli_size_parsing();
  std::printf("carview_drive_tests: %d failure(s)\n", failures);
  return failures ? 1 : 0;
}
