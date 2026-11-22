// Unit tests for drive::build_driving_line (SPEC-CAR-PHYSICS, "3D Driving / Braking Line").
#include <cmath>
#include <cstdio>
#include <vector>

#include "drive/driving_line.h"

namespace {

int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

bool near(double a, double b, double tol = 1e-2) { return std::fabs(a - b) <= tol; }

drive::CarParams test_car() {
  drive::CarParams p;
  const double r = 0.32;
  p.wheel_centres = {drive::Vec3{-0.78, r, 1.30}, drive::Vec3{0.78, r, 1.30},
                     drive::Vec3{-0.79, r, -1.25}, drive::Vec3{0.79, r, -1.25}};
  p.tyre_radius = {r, r, r, r};
  p.centre_of_mass = {0, 0.5, 0.05};
  return p;
}

// Straight for 200 m along +Z, then a 25 m radius right turn (90 deg), then straight along +X.
drive::RacingLine make_corner_line() {
  std::vector<float> xs, zs;
  for (int i = 0; i <= 100; ++i) {
    xs.push_back(0.0f);
    zs.push_back(float(i * 2.0));
  }
  const double R = 25.0;
  constexpr double kPi = 3.14159265358979323846;
  const int arc_pts = 20;
  for (int i = 1; i <= arc_pts; ++i) {
    const double a = (0.5 * kPi) * double(i) / double(arc_pts);
    xs.push_back(float(R * (1.0 - std::cos(a))));
    zs.push_back(float(200.0 + R * std::sin(a)));
  }
  for (int i = 1; i <= 50; ++i) {
    xs.push_back(float(R + i * 2.0));
    zs.push_back(float(200.0 + R));
  }
  drive::RacingLine rl = drive::RacingLine::from_points(xs, zs);
  rl.y.assign(rl.size(), 1.5f);
  rl.plan_geometry();
  return rl;
}

void test_color_band() {
  // Exact neutral Yellow (192, 164, 52) at dv == 0.
  const auto y_full = drive::driving_line_color(30.0, 30.0, drive::DrivingLineMode::kFull);
  CHECK(y_full.r == 192 && y_full.g == 164 && y_full.b == 52 && y_full.a == 255);

  // Exact Red (192, 39, 52) at dv <= -5.0 m/s.
  const auto red = drive::driving_line_color(20.0, 25.0, drive::DrivingLineMode::kFull);
  CHECK(red.r == 192 && red.g == 39 && red.b == 52 && red.a == 255);
  const auto red_clamp = drive::driving_line_color(10.0, 40.0, drive::DrivingLineMode::kFull);
  CHECK(red_clamp.r == 192 && red_clamp.g == 39 && red_clamp.b == 52 && red_clamp.a == 255);

  // Midpoint dv = -2.5 m/s -> t = 0.5 between Yellow and Red.
  const auto orange = drive::driving_line_color(20.0, 22.5, drive::DrivingLineMode::kFull);
  CHECK(orange.r == 192 && orange.g == 102 && orange.b == 52 && orange.a == 255);

  // Exact Green (44, 147, 44) at dv >= +5.0 m/s in Full mode.
  const auto green = drive::driving_line_color(35.0, 30.0, drive::DrivingLineMode::kFull);
  CHECK(green.r == 44 && green.g == 147 && green.b == 44 && green.a == 255);

  // Braking Only: dv <= 0 stays opaque Yellow/Red; dv > 0 fades out via alpha.
  const auto bo_red = drive::driving_line_color(20.0, 25.0, drive::DrivingLineMode::kBrakingOnly);
  CHECK(bo_red.r == 192 && bo_red.g == 39 && bo_red.b == 52 && bo_red.a == 255);
  const auto bo_mid = drive::driving_line_color(32.5, 30.0, drive::DrivingLineMode::kBrakingOnly);
  CHECK(bo_mid.a > 100 && bo_mid.a < 150);
  const auto bo_safe = drive::driving_line_color(35.0, 30.0, drive::DrivingLineMode::kBrakingOnly);
  CHECK(bo_safe.a == 0);

  // Off mode: transparent.
  const auto off = drive::driving_line_color(20.0, 30.0, drive::DrivingLineMode::kOff);
  CHECK(off.a == 0);
}

void test_braking_table() {
  const auto car = test_car();
  const auto tbl = drive::BrakingTable::build(car);
  CHECK(tbl.lateral_mu > 0.5 && tbl.lateral_mu < 3.0);
  CHECK(tbl.decel_mps2[0] > 5.0 && tbl.decel_mps2[0] < 30.0);
  // Higher speed includes aero drag, so decel at 80 m/s > decel at 0 m/s.
  CHECK(tbl.decel_at(80.0) > tbl.decel_at(0.0));
  // Higher grip car brakes harder.
  auto sticky = car;
  sticky.brake_friction_scale *= 1.5;
  sticky.front_brake_torque *= 2.0;
  sticky.rear_brake_torque *= 2.0;
  const auto tbl_sticky = drive::BrakingTable::build(sticky);
  CHECK(tbl_sticky.decel_at(30.0) > tbl.decel_at(30.0));
}

void test_backward_braking_and_mesh() {
  const auto car = test_car();
  const auto tbl = drive::BrakingTable::build(car);
  const auto line = make_corner_line();

  // Off mode emits nothing.
  std::vector<drive::DrivingLineSample> samples;
  drive::build_driving_line(line, tbl, 50.0, 45.0, drive::DrivingLineMode::kOff, samples);
  CHECK(samples.empty());

  // Approaching the 25 m corner at 45 m/s in Full mode.
  drive::DrivingLineOptions opt;
  opt.fade_ends = false;
  drive::build_driving_line(line, tbl, 80.0, 45.0, drive::DrivingLineMode::kFull, samples, opt);
  CHECK(samples.size() > 50);

  // At the start of the straight (far before corner), v_target > 45 m/s (Green).
  // Inside/just before the corner (around d = 195..215 m), v_target < 25 m/s (Red).
  bool saw_green = false, saw_red = false;
  double v_far = 0.0, v_corner = 1e9;
  for (const auto& s : samples) {
    if (s.dist < 90.0f) v_far = std::max(v_far, double(s.v_target));
    if (s.dist > 195.0f && s.dist < 225.0f) v_corner = std::min(v_corner, double(s.v_target));
    if (s.color.r == 44 && s.color.g == 147 && s.color.b == 44) saw_green = true;
    if (s.color.r == 192 && s.color.g == 39 && s.color.b == 52) saw_red = true;
  }
  CHECK(v_far > 45.0);
  CHECK(v_corner < 25.0);
  CHECK(saw_green);
  CHECK(saw_red);

  // Braking Only: accelerating segments far from corner have alpha == 0, braking zone has alpha == 255.
  std::vector<drive::DrivingLineSample> bo_samples;
  drive::build_driving_line(line, tbl, 80.0, 45.0, drive::DrivingLineMode::kBrakingOnly, bo_samples, opt);
  CHECK(bo_samples.size() == samples.size());
  CHECK(bo_samples.front().color.a == 0);
  bool saw_bo_brake = false;
  for (const auto& s : bo_samples) {
    if (s.color.r == 192 && s.color.g == 39 && s.color.a == 255) saw_bo_brake = true;
  }
  CHECK(saw_bo_brake);

  // Ribbon mesh: triangles lifted above road y = 1.5 m; Braking Only emits fewer triangles than Full.
  std::vector<drive::DrivingLineVertex> mesh_full, mesh_bo;
  drive::build_driving_line_mesh(samples, 0.55, 0.08, mesh_full);
  drive::build_driving_line_mesh(bo_samples, 0.55, 0.08, mesh_bo);
  CHECK(!mesh_full.empty() && mesh_full.size() % 3 == 0);
  CHECK(!mesh_bo.empty() && mesh_bo.size() < mesh_full.size());
  CHECK(near(mesh_full[0].y, 1.58, 1e-3));
}

}  // namespace

int main() {
  test_color_band();
  test_braking_table();
  test_backward_braking_and_mesh();
  if (failures) {
    std::printf("driving_line_tests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("driving_line_tests: all passed\n");
  return 0;
}
