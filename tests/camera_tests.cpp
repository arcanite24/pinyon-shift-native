// SPEC-CAMERAS, Tests: the five driving cameras of the R34 V-Spec II (car
// 445) and the Honda CR-X Si (car 320) parked on flat ground, built from
// the player's own files (CameraSettings.ini, CameraPhysics.xml, the
// CameraOverrides rows, MAXData's collision box, CarPartPositions and the
// body rows) and checked against the spec's rule column, plus the parse
// and the cycle (skipped, exit 77, without SHIFT_GAME_ROOT).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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

// SPEC-CAMERAS, Chase at speed (Q51): the oracle lap's car pose replayed
// through the low chase (SHIFT_CHASE_CSV, the specification side's
// chase-<run>.csv): the final eye, view direction and FOV against the
// game's, per frame with the trace's dt.
void replay_chase(const drive::CameraTuning& t, const Built& b, const char* path) {
  std::ifstream f(path);
  std::string line;
  if (!std::getline(f, line)) {
    std::printf("FAIL chase replay: cannot read %s\n", path);
    ++failures;
    return;
  }
  std::map<std::string, size_t> col;
  {
    std::stringstream ss(line);
    std::string name;
    for (size_t k = 0; std::getline(ss, name, ','); ++k) col[name] = k;
  }
  drive::DriveCamera cam;
  cam.set_tuning(t);
  cam.set_car(b.car);
  cam.set_mode(drive::CameraMode::kFollowLow);
  double eye2 = 0, dir2 = 0, fov2 = 0, eye_worst = 0, dir_worst = 0;
  double last_heading = 0, last_v[3] = {0, 0, 0};
  int n = 0;
  bool first = true;
  while (std::getline(f, line)) {
    std::vector<double> r;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) r.push_back(std::atof(cell.c_str()));
    auto g = [&](const char* k) { return r[col.at(k)]; };
    const double right[3] = {g("car_right_x"), g("car_right_y"), g("car_right_z")};
    const double up[3] = {g("car_up_x"), g("car_up_y"), g("car_up_z")};
    const double fwd[3] = {g("car_fwd_x"), g("car_fwd_y"), g("car_fwd_z")};
    const double com[3] = {g("car_x"), g("car_y"), g("car_z")};
    drive::CameraInput in;
    for (int k = 0; k < 3; ++k) {
      in.car_to_world[k] = right[k], in.car_to_world[4 + k] = up[k], in.car_to_world[8 + k] = fwd[k];
      // The translation puts the car frame's CoM on the trace's.
      in.car_to_world[12 + k] = com[k] - (right[k] * b.car.com[0] + up[k] * b.car.com[1] + fwd[k] * b.car.com[2]);
    }
    const double dt = g("dt");
    const double vel[3] = {g("car_vx"), g("car_vy"), g("car_vz")};
    in.speed = g("speed");
    in.heading = std::atan2(fwd[0], fwd[2]);
    in.pitch_degrees = std::asin(std::clamp(fwd[1], -1.0, 1.0)) * 180 / kPi;
    in.contact = g("contacts") / 4.0;
    // The ground under the wheels at the stock ride height (the trace has
    // no wheel positions; the band reads it).
    in.wheel_ground_y = {-b.ride, -b.ride, -b.ride, -b.ride};
    if (!first && dt > 1e-6) {
      in.yaw_rate = std::remainder(in.heading - last_heading, 2 * kPi) / dt;
      double a[3];
      for (int k = 0; k < 3; ++k) a[k] = (vel[k] - last_v[k]) / dt;
      in.g_longitudinal = (a[0] * fwd[0] + a[1] * fwd[1] + a[2] * fwd[2]) / 9.81;
      in.g_lateral = (a[0] * right[0] + a[1] * right[1] + a[2] * right[2]) / 9.81;
    }
    last_heading = in.heading;
    for (int k = 0; k < 3; ++k) last_v[k] = vel[k];
    const auto v = cam.update(first ? 0 : dt, in);
    first = false;
    const double ex = v.eye[0] - g("eye_x"), ey = v.eye[1] - g("eye_y"), ez = v.eye[2] - g("eye_z");
    const double e = std::sqrt(ex * ex + ey * ey + ez * ez);
    const double dot = v.forward[0] * g("view_fwd_x") + v.forward[1] * g("view_fwd_y") + v.forward[2] * g("view_fwd_z");
    const double d = std::acos(std::clamp(dot, -1.0, 1.0)) * 180 / kPi;
    const double fe = v.fov_degrees - g("fov_deg");
    eye2 += e * e, dir2 += d * d, fov2 += fe * fe, ++n;
    if (std::getenv("SHIFT_CHASE_DEBUG") && n % 100 == 0) {
      // The error along the car's axes, and the game's chase eye against
      // its final eye (the layers' offset).
      const double cx = g("eye_x") - g("chase_eye_x"), cy = g("eye_y") - g("chase_eye_y"), cz = g("eye_z") - g("chase_eye_z");
      std::printf("    t %6.2f v %5.1f err r %+.3f u %+.3f f %+.3f | game layers r %+.3f u %+.3f f %+.3f | dir %.2f\n",
                  g("t"), g("speed"), ex * right[0] + ey * right[1] + ez * right[2], ex * up[0] + ey * up[1] + ez * up[2],
                  ex * fwd[0] + ey * fwd[1] + ez * fwd[2], cx * right[0] + cy * right[1] + cz * right[2],
                  cx * up[0] + cy * up[1] + cz * up[2], cx * fwd[0] + cy * fwd[1] + cz * fwd[2], d);
      std::printf("      game chase eye - car y %.3f, h %.3f; ours eye - car y %.3f\n", g("chase_eye_y") - com[1],
                  g("chase_h"), v.eye[1] - com[1]);
      auto yaw_of = [](double x, double z) { return std::atan2(x, z) * 180 / kPi; };
      std::printf("      view yaw ours-game %+.2f, pitch ours %.2f game %.2f, layer pitch %.2f; up ours %.3f %.3f %.3f game %.3f %.3f %.3f\n",
                  std::remainder(yaw_of(v.forward[0], v.forward[2]) - yaw_of(g("view_fwd_x"), g("view_fwd_z")), 360.0),
                  std::asin(-v.forward[1]) * 180 / kPi, std::asin(-g("view_fwd_y")) * 180 / kPi, g("layer_pitch_deg"),
                  v.up[0], v.up[1], v.up[2], g("view_up_x"), g("view_up_y"), g("view_up_z"));
    }
    eye_worst = std::max(eye_worst, e), dir_worst = std::max(dir_worst, d);
  }
  if (n == 0) return;
  const double eye_rms = std::sqrt(eye2 / n), dir_rms = std::sqrt(dir2 / n), fov_rms = std::sqrt(fov2 / n);
  std::printf("  chase replay (%d frames): eye %.3f m rms (worst %.3f), direction %.2f deg rms (worst %.2f), fov %.3f deg rms\n",
              n, eye_rms, eye_worst, dir_rms, dir_worst, fov_rms);
  // The spec's checks: final direction within 0.5 deg rms, FOV within
  // 0.1 deg; the eye (ours: the g inputs come from the trace's velocity
  // differences) within 4 cm rms.
  CHECK(dir_rms < 0.5);
  CHECK(fov_rms < 0.1);
  CHECK(eye_rms < 0.04);
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
  // (the R34's low chase has a height override: no band, 1.860 m).
  {
    double low[2] = {5.191, 1.860}, high[2] = {5.291, 2.080};
    const double cockpit[3] = {0.360, 0.938, -0.180}, hood[3] = {0, 0.945, 0.940}, bumper[3] = {0, 0.943, 1.450};
    check_car(t, r34, "R34", low, high, cockpit, hood, bumper);
  }
  {
    double low[2] = {4.828, 1.800}, high[2] = {4.928, 2.080};
    const double cockpit[3] = {0.350, 0.820, -0.205}, hood[3] = {0, 0.797, 0.847}, bumper[3] = {0, 0.895, 1.110};
    check_car(t, crx, "CR-X", low, high, cockpit, hood, bumper);
  }
  if (const char* chase = std::getenv("SHIFT_CHASE_CSV")) replay_chase(t, r34, chase);
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
  // CameraEffects == 0 loads CameraPhysicsSansEffects.xml with fewer layers.
  {
    std::string ini_se, xml_se;
    drive::CameraTuning t_se;
    CHECK(cars::load_camera_files(install, ini_se, xml_se, &error, false));
    CHECK(drive::parse_camera_settings(ini_se, t_se, &error));
    CHECK(drive::parse_camera_physics(xml_se, t_se, &error));
    CHECK(t_se.layers[0].size() < t.layers[0].size());
  }
  // Bump, jolt (impulse + attack/decay) and noise shake layers compose cleanly.
  {
    const char* kLayerXml = R"xml(
<CameraPhysics>
  <FollowCam >
    <Layer Enabled="1" InputType="SurfaceBumpOffset_FR" NoiseType="None" OutputType="CameraSpaceYPROffset"
           InputClampMin="-1" InputClampMax="1" InputToParamMapping="Linear"
           Param.x="0" Param.y="5.0" Param.z="0" Param.w="0" Sprung="0"/>
    <Layer Enabled="1" InputType="WorldCollision_Longitudinal" NoiseType="None" OutputType="CarSpaceYPRImpulse"
           InputClampMin="-10" InputClampMax="10" InputMagAttackDecay="1" InputAttack="500" InputDecay="20"
           InputToParamMapping="Linear" ImpulseInputSizeCutoff="0.5"
           Param.x="0" Param.y="10.0" Param.z="0" Param.w="0" Sprung="1" SpringK="200" SpringDPercent="0.5"/>
    <Layer Enabled="1" InputType="SpeedMPH" NoiseType="Sin" OutputType="CameraSpaceXYZOffset"
           InputClampMin="0" InputClampMax="200" InputToNoiseFreqMapping="ConstantOne"
           NoiseFreq="10.0" NoiseSeed="0.0" NoiseOutputRangeMin="-1.0" NoiseOutputRangeMax="1.0"
           InputToParamMapping="Linear" Param.x="0" Param.y="0.01" Param.z="0" Param.w="0" Sprung="0"/>
    <Layer Enabled="1" InputType="ConstantOne" NoiseType="Perlin" OutputType="CarSpaceYPROffset"
           NoiseFreq="5.0" NoiseSeed="1.0" NoiseOutputRangeMin="0.0" NoiseOutputRangeMax="0.0"
           InputToParamMapping="ConstantOne" Param.x="10.0" Param.y="10.0" Param.z="10.0" Param.w="0" Sprung="0"/>
  </FollowCam>
  <FollowCam2 ></FollowCam2>
  <DriverCam >
    <Layer Enabled="1" InputType="Gs_Lateral" NoiseType="None" OutputType="CarSpaceYPROffset"
           InputToParamMapping="Linear" Param.x="0" Param.y="0" Param.z="5.0" Param.w="0" Sprung="0"/>
  </DriverCam>
  <Hood ></Hood>
  <BumperHigh ></BumperHigh>
</CameraPhysics>
)xml";
    drive::CameraTuning lt = t;
    CHECK(drive::parse_camera_physics(kLayerXml, lt, &error));
    CHECK(lt.layers[0].size() == 4);
    drive::DriveCamera cam;
    cam.set_tuning(lt);
    cam.set_car(r34.car);
    cam.set_mode(drive::CameraMode::kFollowLow);
    auto in = parked(r34.ride);
    auto base_v = cam.update(0.0, in);  // settles
    // 0..0 noise range produces zero offset.
    auto next_v = cam.update(0.016, in);
    CHECK(near(base_v.forward[1], next_v.forward[1], 1e-6));
    // Surface bump pitches camera down.
    in.surface_bump[1] = 0.5;
    auto bump_v = cam.update(0.016, in);
    CHECK(bump_v.forward[1] < base_v.forward[1] - 0.02);
    in.surface_bump[1] = 0.0;
    // Collision impulse kicks pitch and decays back.
    in.world_collision_longitudinal = 8.0;
    auto jolt_v = cam.update(0.016, in);
    CHECK(std::fabs(jolt_v.forward[1] - base_v.forward[1]) > 0.01);
    in.world_collision_longitudinal = 0.0;
    for (int k = 0; k < 120; ++k) jolt_v = cam.update(0.016, in);
    CHECK(near(jolt_v.forward[1], base_v.forward[1], 0.01));
    // Speed shake oscillates eye height with Sin noise.
    cam.restart();
    in.speed = 40.0;  // ~89.5 mph
    (void)cam.update(0.0, in);        // settles at phase 0
    auto s1 = cam.update(0.025, in);  // quarter period of 10 Hz -> sin = +1
    auto s2 = cam.update(0.050, in);  // three-quarter period -> sin = -1
    CHECK(s1.eye[1] > s2.eye[1] + 0.5);
    // On-car cockpit roll tilts up vector.
    cam.set_mode(drive::CameraMode::kCockpit);
    auto ci = parked(r34.ride);
    ci.g_lateral = 2.0;  // 10 deg roll
    auto cv = cam.update(0.0, ci);
    CHECK(std::fabs(cv.up[0]) > 0.1);
  }
  std::printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
  return failures ? 1 : 0;
}
