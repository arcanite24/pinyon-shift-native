// SPEC-CAMERAS, Tests: the five driving cameras of the R34 V-Spec II (car
// 445) and the Honda CR-X Si (car 320) parked on flat ground, built from
// the player's own files (CameraSettings.ini, CameraPhysics.xml, the
// CameraOverrides rows, MAXData's collision box, CarPartPositions and the
// body rows) and checked against the spec's rule column, plus the parse
// and the cycle (skipped, exit 77, without SHIFT_GAME_ROOT).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "cars/database.h"
#include "cars/physics.h"
#include "cars/sources.h"
#include "drive/cameras.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

constexpr double kPi = 3.14159265358979323846;

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

struct Built {
  drive::CameraCar car;
  double ride = 0;
};

bool build(const cars::Install& install, const cars::Database& db, int64_t id, Built& out) {
  std::vector<cars::CarPhysics> rows;
  std::string error;
  if (!cars::load_car_physics(install, {id}, rows, &error)) {
    std::printf("FAIL car %lld: %s\n", (long long)id, error.c_str());
    return false;
  }
  const auto& p = rows[0];
  cars::CarCameraRow cam;
  if (!cars::load_car_camera(install, id, p.media_name, cam, &error)) {
    std::printf("FAIL car %lld: %s\n", (long long)id, error.c_str());
    return false;
  }
  const cars::CarRow* row = nullptr;
  for (const auto& r : db.cars)
    if (r.id == id) row = &r;
  if (!row || !row->has_part_position[7] || !row->has_part_position[8] || !row->has_part_position[9]) {
    std::printf("FAIL car %lld: no CarPartPositions rows 7, 8, 9\n", (long long)id);
    return false;
  }
  const double shift = p.get("Data_CarBody.BottomCenterWheelbasePosZ");
  out.ride = (p.get("Data_CarBody.ModelFrontStockRideHeight") + p.get("Data_CarBody.ModelRearStockRideHeight")) / 2;
  auto& c = out.car;
  c.box_min = {cam.box[0], cam.box[1], cam.box[2] - shift};
  c.box_max = {cam.box[3], cam.box[4], cam.box[5] - shift};
  const double rear_axle = (row->part_positions[7][2] + row->part_positions[8][2]) / 2;
  c.com = {0, p.get("List_UpgradeCarBodyWeight.CMHeight") - out.ride,
           rear_axle + p.get("Data_CarBody.ModelWheelbase") * p.get("List_UpgradeCarBodyWeight.CMBackFront")};
  c.steering_wheel = row->part_positions[9];
  c.overrides = cam.overrides;
  std::printf("car %lld %s: box z %.4f..%.4f top %.4f, CoM (%.4f, %.4f), ride %.4f\n", (long long)id,
              p.media_name.c_str(), c.box_min[2], c.box_max[2], c.box_max[1], c.com[1], c.com[2], out.ride);
  return true;
}

drive::CameraInput parked(double ride) {
  drive::CameraInput in;
  in.wheel_ground_y = {-ride, -ride, -ride, -ride};
  return in;
}

void check_car(const drive::CameraTuning& t, const Built& b, const char* name, double chase_low[2],
               double chase_high[2], const double cockpit[3], const double hood[3], const double bumper[3]) {
  drive::DriveCamera cam;
  cam.set_tuning(t);
  cam.set_car(b.car);
  const auto in = parked(b.ride);
  const double fovs[5] = {48.5, 58.0, 62.0, 66.0, 53.0};
  for (int m = 0; m < drive::kCameraModes; ++m) {
    cam.set_mode(drive::CameraMode(m));
    const auto v = cam.update(0, in);
    CHECK(near(v.fov_degrees, fovs[m], 0.01));
    if (m < 2) {
      const double d = std::hypot(v.eye[0] - b.car.com[0], v.eye[2] - b.car.com[2]);
      const double h = v.eye[1] + b.ride;
      const double* want = m == 0 ? chase_low : chase_high;
      std::printf("  %s %s: %.3f m from the CoM, %.3f m up (rule %.3f, %.3f); fov %.2f\n", name,
                  drive::camera_mode_name(drive::CameraMode(m)), d, h, want[0], want[1], v.fov_degrees);
      CHECK(near(d, want[0], 0.003));
      CHECK(near(h, want[1], 0.003));
    } else {
      const double* want = m == 2 ? cockpit : m == 3 ? hood : bumper;
      std::printf("  %s %s: eye (%.3f, %.3f, %.3f), want (%.3f, %.3f, %.3f); fov %.2f, pitch %.2f deg\n", name,
                  drive::camera_mode_name(drive::CameraMode(m)), v.eye[0], v.eye[1], v.eye[2], want[0], want[1],
                  want[2], v.fov_degrees, std::asin(-v.forward[1]) * 180 / kPi);
      for (int k = 0; k < 3; ++k) CHECK(near(v.eye[size_t(k)], want[k], 0.003));
      if (m == 2) CHECK(near(std::asin(-v.forward[1]) * 180 / kPi, 1.11, 0.01));
      if (m == 3) CHECK(near(std::asin(-v.forward[1]) * 180 / kPi, 7.20, 0.01));
    }
  }
}

}  // namespace

int main() {
  const char* root = std::getenv("SHIFT_GAME_ROOT");
  if (!root) {
    std::printf("SHIFT_GAME_ROOT not set: skipped\n");
    return 77;
  }
  const char* state = std::getenv("SHIFT_STATE_ROOT");
  cars::Install install;
  std::string error;
  if (!cars::discover_install(root, state ? state : "", "", install, &error)) {
    std::printf("FAIL %s\n", error.c_str());
    return 1;
  }
  std::string ini, xml;
  drive::CameraTuning t;
  if (!cars::load_camera_files(install, ini, xml, &error) || !drive::parse_camera_settings(ini, t, &error) ||
      !drive::parse_camera_physics(xml, t, &error)) {
    std::printf("FAIL %s\n", error.c_str());
    return 1;
  }
  CHECK(near(t.chase[0].fov, 48.5, 1e-9) && near(t.chase[1].fov, 58, 1e-9));
  CHECK(near(t.chase[0].height_min, 1.8, 1e-9) && near(t.chase[1].height_max, 2.08, 1e-9));
  for (int m = 0; m < drive::kCameraModes; ++m)
    std::printf("%s: %zu layers kept, %d left out\n", drive::camera_mode_name(drive::CameraMode(m)),
                t.layers[size_t(m)].size(), t.layers_left_out[size_t(m)]);

  cars::Database db;
  if (!cars::load_database(install, db, &error)) {
    std::printf("FAIL %s\n", error.c_str());
    return 1;
  }
  Built r34, crx;
  if (!build(install, db, 445, r34) || !build(install, db, 320, crx)) return 1;
  // SPEC-CAMERAS, Tests: "Rule against oracle, parked", the rule column
  // (the R34's low chase on the band: 1.800 m, the oracle's 1.858 m is an
  // open failed check).
  {
    double low[2] = {5.191, 1.800}, high[2] = {5.291, 2.080};
    const double cockpit[3] = {0.360, 0.938, -0.180}, hood[3] = {0, 0.945, 0.940}, bumper[3] = {0, 0.943, 1.450};
    check_car(t, r34, "R34", low, high, cockpit, hood, bumper);
  }
  {
    double low[2] = {4.828, 1.800}, high[2] = {4.928, 2.080};
    const double cockpit[3] = {0.350, 0.820, -0.205}, hood[3] = {0, 0.797, 0.847}, bumper[3] = {0, 0.895, 1.110};
    check_car(t, crx, "CR-X", low, high, cockpit, hood, bumper);
  }
  // Looking, parked: the bumper back and right on the R34, the low chase
  // back and left on the CR-X.
  {
    drive::DriveCamera cam;
    cam.set_tuning(t);
    cam.set_car(r34.car);
    cam.set_mode(drive::CameraMode::kBumper);
    cam.set_look(kPi);
    auto v = cam.update(0, parked(r34.ride));
    std::printf("  R34 bumper back: z %.3f (rule -1.595)\n", v.eye[2]);
    CHECK(near(v.eye[2], -1.595, 0.003));
    cam.set_look(kPi / 2);
    v = cam.update(1, parked(r34.ride));
    std::printf("  R34 bumper right: x %.3f z %.3f (rule 0.236, 1.450)\n", v.eye[0], v.eye[2]);
    CHECK(near(v.eye[0], 0.236, 0.003) && near(v.eye[2], 1.450, 0.003));
    cam.set_car(crx.car);
    cam.set_mode(drive::CameraMode::kFollowLow);
    cam.set_look(kPi);
    v = cam.update(0, parked(crx.ride));
    std::printf("  CR-X low chase back: %.3f m ahead of the CoM (rule 4.83)\n", v.eye[2] - crx.car.com[2]);
    CHECK(near(v.eye[2] - crx.car.com[2], 4.828, 0.003));
    cam.set_look(-kPi / 2);
    v = cam.update(1, parked(crx.ride));
    std::printf("  CR-X low chase left: %.3f m to the right (rule 4.83)\n", v.eye[0] - crx.car.com[0]);
    CHECK(near(v.eye[0] - crx.car.com[0], 4.828, 0.003));
  }
  // FOV at speed, the steady layer (SPEC-CAMERAS, FOV at speed: the rule).
  {
    drive::DriveCamera cam;
    cam.set_tuning(t);
    cam.set_car(crx.car);
    auto in = parked(crx.ride);
    in.speed = 13.6 / 2.2369362920544;
    const auto v = cam.update(1, in);
    std::printf("  CR-X low chase at 13.6 mph: +%.2f deg (rule +1.20)\n", v.fov_degrees - 48.5);
    CHECK(near(v.fov_degrees - 48.5, 1.20, 0.02));
    cam.set_car(r34.car);
    cam.set_mode(drive::CameraMode::kHood);
    in.speed = 30.3 / 2.2369362920544;
    const auto h = cam.update(1, in);
    std::printf("  R34 hood at 30.3 mph: +%.2f deg (rule +0.52)\n", h.fov_degrees - 66);
    CHECK(near(h.fov_degrees - 66, 0.52, 0.02));
  }
  // The cycle: FollowCam, FollowCam2, cockpit, hood, bumper, FollowCam.
  {
    drive::DriveCamera cam;
    for (int k = 0; k < 6; ++k) {
      CHECK(int(cam.mode()) == k % 5);
      cam.cycle();
    }
  }
  std::printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
  return failures ? 1 : 0;
}
