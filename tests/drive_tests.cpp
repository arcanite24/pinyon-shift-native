// Tests for libs/drive on a synthetic car on flat ground (no game data).
// The physics is the engine's placeholder model; these check that it is
// sound (rest pose, determinism, directions), not that it matches the game.
// Spec: FORMAT-CARS (Wheels: the wheel stands on y = 0 at its outer
// radius; the body frame is +X right, +Z forward), REIMPLEMENTATION_PLAN M3.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "drive/from_bake.h"
#include "drive/vehicle.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

drive::CarParams test_car() {
  drive::CarParams p;
  const double r = 0.32;
  p.wheel_centres = {drive::Vec3{-0.78, r, 1.30}, drive::Vec3{0.78, r, 1.30}, drive::Vec3{-0.79, r, -1.25},
                     drive::Vec3{0.79, r, -1.25}};
  p.tyre_radius = {r, r, r, r};
  p.centre_of_mass = {0, 0.5, 0.05};
  return p;
}

std::vector<std::string> run(const drive::CarParams& p, const drive::Input& in, int steps) {
  drive::Vehicle v(p);
  const drive::FlatGround ground;
  std::vector<std::string> rows;
  for (int i = 0; i < steps; ++i) {
    v.step(in, ground);
    rows.push_back(drive::trace_row(v));
  }
  return rows;
}

void test_rest_pose() {
  const auto p = test_car();
  drive::Vehicle v(p);
  const drive::FlatGround ground;
  for (int i = 0; i < 600; ++i) v.step({}, ground);  // 5 s, no input
  const drive::Vec3 pos = v.position();
  CHECK(std::fabs(pos.y) < 1e-3);
  CHECK(std::fabs(pos.x) < 1e-6 && std::fabs(pos.z) < 1e-6);
  const drive::Vec3 up = drive::rotate(v.orientation(), {0, 1, 0});
  CHECK(up.y > 1 - 1e-6);
  for (int i = 0; i < drive::kWheels; ++i) {
    const auto& w = v.wheels()[size_t(i)];
    CHECK(w.contact);
    // The wheel centre at the tyre's outer radius, within 1 mm.
    CHECK(std::fabs(w.centre.y + pos.y - p.tyre_radius[size_t(i)]) < 1e-3);
  }
  double load = 0;
  for (const auto& w : v.wheels()) load += w.load;
  CHECK(std::fabs(load - p.mass * 9.81) < 1.0);
}

void test_determinism() {
  const auto p = test_car();
  const drive::Input in{0.4, 1.0, 0.0, false};
  CHECK(run(p, in, 600) == run(p, in, 600));
}

void test_launch_and_brake() {
  const auto p = test_car();
  drive::Vehicle v(p);
  const drive::FlatGround ground;
  double t100 = -1;
  for (int i = 0; i < 120 * 30 && t100 < 0; ++i) {
    v.step({0, 1, 0, false}, ground);
    if (v.speed() >= 100 / 3.6) t100 = v.time();
  }
  CHECK(t100 > 2 && t100 < 15);
  CHECK(v.position().z > 10);                  // forward is +z
  CHECK(std::fabs(v.position().x) < 0.5);      // straight
  CHECK(v.gear() >= 2);
  for (int i = 0; i < 120 * 10 && v.speed() > 0.05; ++i) v.step({0, 0, 1, false}, ground);
  CHECK(std::fabs(v.speed()) < 0.1);
}

void test_steer_right_goes_to_plus_x() {
  const auto p = test_car();
  drive::Vehicle v(p);
  const drive::FlatGround ground;
  for (int i = 0; i < 120 * 4; ++i) v.step({0.5, 0.4, 0, false}, ground);
  CHECK(v.position().x > 1);
  CHECK(v.wheels()[0].steer > 0);
  // Yaw towards +x: the forward axis has turned to positive x.
  CHECK(drive::rotate(v.orientation(), {0, 0, 1}).x > 0.1);
}

void test_reverse() {
  const auto p = test_car();
  drive::Vehicle v(p);
  const drive::FlatGround ground;
  for (int i = 0; i < 120 * 3; ++i) v.step({0, 0, 1, false}, ground);
  CHECK(v.gear() == -1);
  CHECK(v.position().z < -0.5);
}

void test_script() {
  std::vector<drive::ScriptLine> s;
  std::string error;
  CHECK(drive::parse_script("# start\n0 0 0 0\n1.5 0.5 1 0 1\n\n3 0 0 1\n", s, &error));
  CHECK(s.size() == 3);
  CHECK(drive::script_input(s, 1.0).throttle == 0);
  CHECK(drive::script_input(s, 2.0).throttle == 1 && drive::script_input(s, 2.0).handbrake);
  CHECK(drive::script_input(s, 9.0).brake == 1);
  CHECK(!drive::parse_script("1 0 0 0\n0 0 0 0\n", s, &error));
  CHECK(!drive::parse_script("1 0\n", s, &error));
}

void test_model_to_world() {
  // At rest at the origin, model (0,0,0) maps to (0, lift, 0) and model +z
  // (rear) to body -z.
  cars::bake::CarInfo car;
  car.lift = 0.15f;
  car.pitch = 0;
  for (int i = 0; i < 4; ++i) car.wheel_centres_body[size_t(i)] = {i % 2 ? 0.8f : -0.8f, 0.3f, i < 2 ? 1.3f : -1.2f};
  const auto p = drive::params_from_bake(car);
  CHECK(std::fabs(p.wheel_centres[1].x - 0.8) < 1e-6 && std::fabs(p.tyre_radius[0] - 0.3) < 1e-6);
  drive::Vehicle v(p);
  float m[16];
  drive::model_to_world(car, v, m);
  CHECK(std::fabs(m[13] - 0.15f) < 1e-6f && std::fabs(m[12]) < 1e-6f && std::fabs(m[14]) < 1e-6f);
  CHECK(m[10] < -0.99f && m[0] > 0.99f && m[5] > 0.99f);
}

}  // namespace

int main() {
  test_rest_pose();
  test_determinism();
  test_launch_and_brake();
  test_steer_right_goes_to_plus_x();
  test_reverse();
  test_script();
  test_model_to_world();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("drive_tests: all passed\n");
  return 0;
}
