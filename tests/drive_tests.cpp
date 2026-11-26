// Tests for libs/drive on a synthetic car on flat ground (no game data).
// The physics is the engine's placeholder model; these check that it is
// sound (rest pose, determinism, directions), not that it matches the game.
// Spec: FORMAT-CARS (Wheels: the wheel stands on y = 0 at its outer
// radius; the body frame is +X right, +Z forward), REIMPLEMENTATION_PLAN M3.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cars/physics.h"
#include "drive/from_bake.h"
#include "drive/from_physics.h"
#include "drive/mesh_ground.h"
#include "drive/ai_driver.h"
#include "drive/contact.h"
#include "drive/controls.h"
#include "drive/damage.h"
#include "drive/oracle_lap.h"
#include "drive/save.h"
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

// surfaceTypes.xml's Car/HardWorld row (asphalt, concrete, walls).
drive::SurfaceTyre hard_world() {
  drive::SurfaceTyre t;
  t.coll_elasticity = -0.9;
  t.coll_friction = 0.5;
  t.coll_low_speed_friction = 0.1;
  return t;
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
  CHECK(std::fabs(load - p.mass * p.gravity) < 1.0);
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

// A plane as two big triangles, rising `grade` (rise over run) towards +z.
drive::MeshGround plane(double grade) {
  const float h = 2000, y = float(grade * 2000);
  return drive::MeshGround({{-h, -y, -h}, {h, y, h}, {h, -y, -h}, {-h, -y, -h}, {-h, y, h}, {h, y, h}});
}

double time_to(const drive::CarParams& p, const drive::Ground& ground, double kmh) {
  drive::Vehicle v(p);
  for (int i = 0; i < 120 * 40; ++i) {
    v.step({0, 1, 0, false}, ground);
    if (v.speed() >= kmh / 3.6) return v.time();
  }
  return -1;
}

void test_mesh_ground() {
  const auto p = test_car();
  // A flat mesh is the flat ground: the same run to rounding (the mesh's
  // ray distances differ from the plane's in the last bits).
  const auto flat_mesh = plane(0);
  const drive::FlatGround flat;
  drive::Vehicle a(p), b(p);
  double worst = 0;
  for (int i = 0; i < 120 * 8; ++i) {
    a.step({0.3, 1, 0, false}, flat);
    b.step({0.3, 1, 0, false}, flat_mesh);
    const drive::Vec3 d = a.position() - b.position();
    worst = std::max(worst, std::sqrt(drive::dot(d, d)));
  }
  std::printf("flat mesh against flat ground over 8 s: %.3g m apart at most\n", worst);
  CHECK(worst < 1e-3);
  // Rays: hit from above, miss from below the far end and beside it.
  drive::Hit hit;
  CHECK(flat_mesh.raycast({5, 2, 7}, {0, -1, 0}, 5, hit) && std::fabs(hit.distance - 2) < 1e-6 && hit.normal.y > 0.99);
  CHECK(!flat_mesh.raycast({5, 2, 7}, {0, -1, 0}, 1.5, hit));
  CHECK(!flat_mesh.raycast({2500, 2, 7}, {0, -1, 0}, 5, hit));
  // Up a 7 % grade the car still reaches 60 km/h, a little later.
  const double t_flat = time_to(p, flat, 60), t_hill = time_to(p, plane(0.07), 60);
  std::printf("0-60 km/h: flat %.2f s, 7 %% grade %.2f s\n", t_flat, t_hill);
  CHECK(t_hill > t_flat && t_hill < t_flat * 2);
}

// Q281: with no input the stopped car stays put on a slope (the original
// holds it on the brakes); the gas lets go of it, and once it has stopped
// again it is held again.
void test_rest_hold() {
  const auto p = test_car();
  const auto hill = plane(0.05);  // about the free-roam spawn's 2.7 degrees
  drive::Vehicle v(p);
  for (int i = 0; i < 120 * 10; ++i) v.step({0, 0, 0, false}, hill);
  const drive::Vec3 at = v.position();
  std::printf("rest on a 5 %% grade: %.3f m from the start after 10 s, hold %d\n",
              std::sqrt(at.x * at.x + at.z * at.z), int(v.rest_hold()));
  CHECK(v.rest_hold());
  CHECK(std::fabs(v.speed()) < 0.02 && std::sqrt(at.x * at.x + at.z * at.z) < 0.05);
  CHECK(v.gear() == 1);  // the hold is not the driver's brake: no reverse
  // The gas lets go at once: no brake on the first step with gas.
  v.step({0, 1, 0, false}, hill);
  CHECK(!v.rest_hold() && v.shaped_input().brake == 0);
  for (int i = 0; i < 120 * 2; ++i) v.step({0, 1, 0, false}, hill);
  CHECK(v.speed() > 5);
  // Moving with no input: no hold.
  v.step({0, 0, 0, false}, hill);
  CHECK(!v.rest_hold());
  // Stopped on a light brake (over half a pedal selects reverse at a stop)
  // and released: held again.
  for (int i = 0; i < 120 * 8; ++i) v.step({0, 0, 0.4, false}, hill);
  std::printf("after the stop: %.3f m/s, gear %d\n", v.speed(), v.gear());
  for (int i = 0; i < 120 * 4; ++i) v.step({0, 0, 0, false}, hill);
  CHECK(v.rest_hold());
  CHECK(std::fabs(v.speed()) < 0.02);
  // On the flat: a full brake from the held rest selects reverse and backs
  // off (the brake is the reverse gas, so the hold lets go); let go of it
  // and the car rolls back unheld; then the gas drives forwards.
  const auto flat = plane(0.0);
  for (int i = 0; i < 120 * 4; ++i) v.step({0, 0, 0, false}, flat);
  CHECK(v.rest_hold());
  const drive::Vec3 before_reverse = v.position();
  for (int i = 0; i < 120 * 2; ++i) v.step({0, 0, 1, false}, flat);
  const drive::Vec3 backed = v.position();
  const double backed_m = std::sqrt((backed.x - before_reverse.x) * (backed.x - before_reverse.x) +
                                    (backed.z - before_reverse.z) * (backed.z - before_reverse.z));
  std::printf("full brake from the hold: gear %d, %.3f m/s, %.2f m, hold %d\n", v.gear(), v.speed(), backed_m,
              int(v.rest_hold()));
  CHECK(v.gear() == -1 && !v.rest_hold() && backed_m > 1.0);
  for (int i = 0; i < 120 * 15; ++i) v.step({0, 0, 0, false}, flat);
  std::printf("reverse released: %.3f m/s, gear %d, hold %d\n", v.speed(), v.gear(), int(v.rest_hold()));
  CHECK(!v.rest_hold());  // rolling back with no input: no hold
  for (int i = 0; i < 120 * 3; ++i) v.step({0, 1, 0, false}, flat);
  std::printf("then the gas: gear %d, %.2f m/s\n", v.gear(), v.speed());
  CHECK(v.gear() >= 1 && v.speed() > 3);
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

// SPEC-CAR-PHYSICS, "Oracle lap for the M3 gate": a synthetic oracle lap
// on a 40 m circle (turning towards +x), followed on flat ground.
std::string circle_lap(double radius, double gas) {
  std::string text;
  char buf[512];
  const double length = 2 * 3.14159265358979323846 * radius;
  for (int f = -30; f <= 3000; ++f) {
    const double t = f / 60.0;
    const double s = t > 0 ? 8 * t : 0;  // 8 m/s along the circle
    if (s > length + 40) break;
    const double a = s / radius;
    std::snprintf(buf, sizeof buf,
                  "{\"event\":\"fh1.physics_trace\",\"frame\":%d,\"mode\":\"3\",\"t_s\":%.6f,"
                  "\"cars\":\"0:R34:0:%.3f:800:1:%.3f:0.000:0.000\",\"x\":%.4f,\"y\":0,\"z\":%.4f,"
                  "\"yaw_deg\":%.4f,\"distance_m\":%.4f}\n",
                  f, t, t > 0 ? 8.0 : 0.0, t >= 0 ? gas : 0.0, radius - radius * std::cos(a), radius * std::sin(a),
                  a * 180 / 3.14159265358979323846, s);
    text += buf;
  }
  text += "{\"event\":\"dlc.rally.race_trace\",\"started\":\"1\",\"ended\":\"1\",\"end_reason\":\"1\","
          "\"time_72\":\"31.416\"}\n";
  return text;
}

void test_oracle_lap() {
  drive::OracleLine line;
  std::string error;
  CHECK(drive::parse_oracle_line(circle_lap(40, 0.25), line, &error));
  CHECK(std::fabs(line.race_seconds - 31.416) < 1e-9);
  CHECK(line.start.t == 0 && line.start.x == 0 && line.start.z == 0);
  drive::Gate gate;
  CHECK(drive::parse_gate("{\"gate\": {\"point_xz\": [0.0, 0.0], \"crossing_direction_xz\": [0, 2]}}", gate, &error));
  CHECK(gate.nx == 0 && std::fabs(gate.nz - 1) < 1e-12);
  CHECK(!drive::parse_gate("{\"race\": {}}", gate, &error));

  // SPEC-RACE-AI, Steering at speed: 0.27 alpha + 0.29 beta, only the lock bounds it.
  CHECK(std::fabs(drive::ai_slip_steer(0.1, 0.05, 0.73) - 0.0415) < 1e-12);
  CHECK(drive::ai_slip_steer(3.0, 0.0, 0.73) == 0.73 && drive::ai_slip_steer(-3.0, -1.0, 0.73) == -0.73);
  // The geometry below is pure pursuit's (the slip rule's fit is for 15 m/s
  // and up; this circle runs at 8).
  drive::LineFollower::Options pursuit;
  pursuit.slip_rule = false, pursuit.controller = false;
  drive::LineFollower follower(line, pursuit);
  // The grid's pedals are the last sample at distance 0: the launch's gas.
  double gas = 0, brake = 1;
  follower.pedals_at(0, gas, brake);
  CHECK(std::fabs(gas - 0.25) < 1e-9 && brake == 0);

  // Follow the circle once round, past the gate.
  drive::Vehicle v(test_car());
  const drive::FlatGround ground;
  v.reset({0, 0, 0}, 0);
  drive::GateTimer timer(gate, follower.length() / 2, 5);
  drive::Vec3 prev = v.position();
  double max_cross = 0;
  for (int i = 0; i < int(60 / drive::Vehicle::kStep) && !timer.crossed(); ++i) {
    const double t0 = v.time();
    v.step(follower.input(v), ground);
    const drive::Vec3 at = v.position();
    if (v.time() > 2) max_cross = std::max(max_cross, std::fabs(follower.cross_track()));
    timer.update(t0, prev.x, prev.z, v.time(), at.x, at.z, follower.progress());
    prev = at;
  }
  CHECK(timer.crossed());
  CHECK(max_cross < 1.5);
  CHECK(follower.progress() > 2 * 3.14159265358979323846 * 40 - 5);

  // A crossing before half the lap or against the direction is not the finish.
  drive::GateTimer early(gate, 100);
  CHECK(!early.update(0, 0, -1, 1, 0, 1, 50));
  drive::GateTimer backwards(gate, 0);
  CHECK(!backwards.update(0, 0, 1, 1, 0, -1, 200));
  drive::GateTimer finish(gate, 0);
  CHECK(finish.update(1, 0, -1, 2, 0, 3, 200) && std::fabs(finish.time() - 1.25) < 1e-12);

  const std::string end = drive::race_trace_event(true, 38.5);
  CHECK(end.find("\"ended\":\"1\"") != std::string::npos && end.find("\"time_72\":\"38.500000\"") != std::string::npos);
}

// The drive's save: round trip through text and a file, the version and
// the required lines, and an unknown key skipped.
void test_save() {
  drive::DriveSave s;
  s.car = "NIS_SkylineGTRVSpecII_02";
  s.position = {-1790.46, 34.163, 1863.44};
  s.heading = -1.4358;
  s.has_spawn = true;
  s.spawn = {2002.96, 143.84, 960.0};
  s.spawn_heading = 2.2008;
  s.total_distance = 51234.5;
  s.signs_found = {"flyer_001", "flyer_017"};
  s.barns = {{"barnfind_01", 2, 50000, 51200.25}, {"barnfind_02", 5, 51000, 0}};
  s.garage = {1219};
  s.radio_station = "Radio2";
  s.radio_dj = false;
  s.radio_volume = 0.75;
  drive::DriveSave back;
  std::string error;
  CHECK(drive::save_from_text(drive::save_to_text(s), back, &error));
  CHECK(back.car == s.car && back.has_spawn);
  CHECK(back.radio_station == "Radio2" && !back.radio_dj && std::fabs(back.radio_volume - 0.75) < 1e-4);
  // The free-roam progress (SPEC-FREEROAM-COLLECTIBLES).
  CHECK(std::fabs(back.total_distance - 51234.5) < 1e-6 && back.signs_found == s.signs_found);
  CHECK(back.barns.size() == 2 && back.barns[0].name == "barnfind_01" && back.barns[0].status == 2 &&
        std::fabs(back.barns[0].found_at - 51200.25) < 1e-6 && back.barns[1].status == 5);
  CHECK(back.garage == s.garage);
  CHECK(std::fabs(back.position[0] - s.position[0]) < 1e-5 && std::fabs(back.position[2] - s.position[2]) < 1e-5);
  CHECK(std::fabs(back.heading - s.heading) < 1e-6 && std::fabs(back.spawn_heading - s.spawn_heading) < 1e-6);
  CHECK(drive::save_from_text("shift-drive-save 1\ncar X\nposition 1 2 3\nheading 0.5\nfuture 7\n", back, &error));
  CHECK(back.car == "X" && !back.has_spawn && back.position[1] == 2);
  CHECK(!drive::save_from_text("shift-drive-save 2\ncar X\nposition 1 2 3\nheading 0\n", back, &error));
  CHECK(!drive::save_from_text("shift-drive-save 1\ncar X\nheading 0\n", back, &error));
  CHECK(!drive::save_from_text("something else\n", back, &error));
  const auto dir = std::filesystem::temp_directory_path() / "shift-drive-save-test";
  std::filesystem::remove_all(dir);
  const auto path = dir / "sub" / "save.txt";
  CHECK(!drive::read_save(path, back, &error) && error.empty());  // none yet: not an error
  CHECK(drive::write_save(path, s, &error));
  s.car = "Second";
  CHECK(drive::write_save(path, s, &error));  // replaces it
  CHECK(drive::read_save(path, back, &error) && back.car == "Second");
  CHECK(!std::filesystem::exists(std::filesystem::path(path).concat(".tmp")));
  std::filesystem::remove_all(dir);
}

}  // namespace

// AI drivers (M5 groundwork): a made-up 60 m circle, its speed profile from
// curvature, and four cars in two lanes driving it on flat ground at once.
void test_ai_field() {
  std::vector<float> xs, zs;
  const double r = 60, kPi = 3.14159265358979323846;
  for (int i = 0; i <= 720; ++i) {
    const double a = 2 * kPi * i / 360.0;  // twice round
    xs.push_back(float(r - r * std::cos(a)));
    zs.push_back(float(r * std::sin(a)));
  }
  const drive::RacingLine line = drive::RacingLine::from_points(xs, zs, 6.0, 4.0, 8.0, 40.0);
  CHECK(std::fabs(line.length() - 4 * kPi * r) < 1.0);
  // On a circle the curvature limit is sqrt(a r) all round.
  const double v_curve = std::sqrt(6.0 * r);
  CHECK(std::fabs(line.speed[line.size() / 2] - v_curve) < 0.3);
  CHECK(*std::max_element(line.speed.begin(), line.speed.end()) < v_curve + 0.3);
  CHECK(line.segment(100.0, 0) == line.segment(100.0, line.size() / 2));

  const size_t n = 4;
  drive::AiField field(line, n);
  std::vector<drive::Vehicle> cars;
  for (size_t i = 0; i < n; ++i) {
    cars.emplace_back(test_car());
    field.place(i, 5.0 + 12.0 * double(i), i % 2 ? 2.0f : -2.0f, 0.95f);
    double x = 0, z = 0, h = 0;
    field.start_pose(i, x, z, h);
    cars[i].reset({x, 0, z}, h);
  }
  const drive::FlatGround ground;
  std::vector<drive::AiCarState> states(n);
  std::vector<drive::Input> inputs(n);
  double worst_lane_error = 0;
  for (int s = 0; s < int(40 / drive::Vehicle::kStep); ++s) {
    for (size_t i = 0; i < n; ++i) states[i] = drive::ai_state(cars[i]);
    field.drive(states, inputs);
    for (size_t i = 0; i < n; ++i) cars[i].step(inputs[i], ground);
    if (s * drive::Vehicle::kStep > 10)
      for (size_t i = 0; i < n; ++i)
        worst_lane_error = std::max(worst_lane_error, std::fabs(field.cross_track(i) - (i % 2 ? 2.0 : -2.0)));
  }
  for (size_t i = 0; i < n; ++i) CHECK(field.progress(i) > 300);  // well round the first lap
  std::printf("ai field: progress %.0f %.0f %.0f %.0f m, lane error up to %.2f m\n", field.progress(0),
              field.progress(1), field.progress(2), field.progress(3), worst_lane_error);
  CHECK(worst_lane_error < 1.5);
}

// Exercises AiField::Options::slip_rule = true + ai_traction(..., steer_guard = false)
// against default pursuit + steer_guard = true so both steering paths stay covered.
void test_ai_slip_rule() {
  std::vector<float> xs, zs;
  const double r = 60, kPi = 3.14159265358979323846;
  for (int i = 0; i <= 720; ++i) {
    const double a = 2 * kPi * i / 360.0;
    xs.push_back(float(r - r * std::cos(a)));
    zs.push_back(float(r * std::sin(a)));
  }
  const drive::RacingLine line = drive::RacingLine::from_points(xs, zs, 6.0, 4.0, 8.0, 40.0);
  const drive::FlatGround ground;

  auto run_mode = [&](bool use_slip) {
    drive::AiField::Options opt;
    opt.slip_rule = use_slip;
    drive::AiField field(line, 1, opt);
    drive::Vehicle car(test_car());
    car.set_ai_controls(true);  // as the race puts AI cars (race_view.cpp): no pad filter
    field.place(0, 5.0, 0.0f, 0.95f);
    double x = 0, z = 0, h = 0;
    field.start_pose(0, x, z, h);
    car.reset({x, 0, z}, h);
    float cap = 1.0f;
    std::vector<drive::AiCarState> st(1);
    std::vector<drive::Input> ins(1);
    double max_abs_steer = 0.0;
    double max_steer_step = 0.0;
    double prev_steer = 0.0;
    for (int s = 0; s < int(20 / drive::Vehicle::kStep); ++s) {
      // Sudden lane changes mid-run: pursuit's steer_rate stops 1-tick snaps.
      if (s == 600) field.set_lane(0, 2.5f);
      if (s == 1200) field.set_lane(0, -2.5f);
      st[0] = drive::ai_state(car);
      field.drive(st, ins);
      drive::ai_traction(car, cap, drive::Vehicle::kStep, ins[0], !use_slip);
      max_abs_steer = std::max(max_abs_steer, std::fabs(ins[0].steer));
      if (s > 0) max_steer_step = std::max(max_steer_step, std::fabs(ins[0].steer - prev_steer));
      prev_steer = ins[0].steer;
      car.step(ins[0], ground);
    }
    CHECK(std::isfinite(car.position().x) && std::isfinite(car.position().z));
    CHECK(field.progress(0) > 150.0);
    CHECK(max_abs_steer > 0.02);
    if (!use_slip) CHECK(max_steer_step < 0.08);
    return field.progress(0);
  };

  const double p_pursuit = run_mode(false);
  const double p_slip = run_mode(true);
  CHECK(p_pursuit > 150.0 && p_slip > 150.0);

  // Direct ai_traction steer_guard comparison during a >11 deg body slip angle:
  // steer_guard = true counter-steers towards velocity; steer_guard = false preserves steer.
  drive::Vehicle sliding(test_car());
  sliding.launch({0, 0, 0}, 0.0, 22.0);
  bool got_slip = false;
  for (int i = 0; i < int(2.0 / drive::Vehicle::kStep); ++i) {
    sliding.step({1.0, 0.0, 0.0, true}, ground);
    const drive::Vec3 vb = drive::rotate_inverse(sliding.orientation(), sliding.velocity());
    if (vb.z > 6.0 && std::fabs(std::atan2(vb.x, vb.z)) > 0.20) {
      got_slip = true;
      break;
    }
  }
  CHECK(got_slip);
  drive::Input guarded{0.0, 1.0, 0.0, false};
  drive::Input unguarded = guarded;
  float cap1 = 1.0f, cap2 = 1.0f;
  drive::ai_traction(sliding, cap1, drive::Vehicle::kStep, guarded, true);
  drive::ai_traction(sliding, cap2, drive::Vehicle::kStep, unguarded, false);
  CHECK(std::fabs(guarded.steer) > 0.15);
  CHECK(unguarded.steer == 0.0);
  CHECK(unguarded.throttle < 0.5);
}

// SPEC-RACE-AI, Steering: the title's line controller (decoded). The yaw
// gain is the car's kinematic v x lock / wheelbase at walking pace and
// positive at every bin; an aim point to the right steers right; on a 60 m
// circle the car holds its lane within a metre once settled, and follows a
// lane change.
void test_ai_controller() {
  const drive::CarParams p = test_car();
  const drive::AiYawGain g = drive::ai_yaw_gain(p);
  drive::Vehicle probe(p);
  probe.set_ai_controls(true);
  const auto& wc = p.wheel_centres;
  const double wb = std::fabs((wc[0].z + wc[1].z) / 2 - (wc[2].z + wc[3].z) / 2);
  const double kin = 2.5 * probe.steer_lock() / wb;
  std::printf("controller yaw gain: %.2f at 2.5 m/s (kinematic %.2f), %.2f at 22.5, %.2f at 42.5\n", g[0], kin, g[4],
              g[8]);
  CHECK(std::fabs(g[0] - kin) < 0.25 * kin);
  for (float x : g) CHECK(x > 0.05f);
  {
    drive::AiCarState c;
    c.speed = 20, c.vel_z = 20;
    drive::AiSteerState ss;
    CHECK(drive::ai_controller_steer(c, g, nullptr, 0, 0, 2.0, 10.0, 0, 1, false, ss) > 0.01);
    CHECK(drive::ai_controller_steer(c, g, nullptr, 0, 0, -2.0, 10.0, 0, 1, false, ss) < -0.01);
  }
  std::vector<float> xs, zs;
  const double r = 60, kPi = 3.14159265358979323846;
  for (int i = 0; i <= 720; ++i) {
    const double a = 2 * kPi * i / 360.0;
    xs.push_back(float(r - r * std::cos(a)));
    zs.push_back(float(r * std::sin(a)));
  }
  const drive::RacingLine line = drive::RacingLine::from_points(xs, zs, 6.0, 4.0, 8.0, 40.0);
  const drive::FlatGround ground;
  drive::AiField::Options opt;
  opt.controller = true;
  drive::AiField field(line, 1, opt);
  field.set_yaw_gain(0, g);
  drive::Vehicle car(p);
  car.set_ai_controls(true);
  field.place(0, 5.0, 0.0f, 0.95f);
  double x = 0, z = 0, h = 0;
  field.start_pose(0, x, z, h);
  car.reset({x, 0, z}, h);
  std::vector<drive::AiCarState> st(1);
  std::vector<drive::Input> ins(1);
  double worst = 0, worst_change = 1e9;
  for (int s = 0; s < int(20 / drive::Vehicle::kStep); ++s) {
    if (s == 1200) field.set_lane(0, 2.0f);
    st[0] = drive::ai_state(car);
    field.drive(st, ins);
    const double off = std::fabs(field.cross_track(0) - field.lane(0));
    if (s > 600 && s < 1200) worst = std::max(worst, off);
    if (s > 1900) worst_change = std::min(worst_change, off);
    car.step(ins[0], ground);
  }
  std::printf("controller on a 60 m circle: progress %.0f m, worst %.2f m off the lane, %.2f m after the change\n",
              field.progress(0), worst, worst_change);
  CHECK(field.progress(0) > 150.0);
  CHECK(worst < 1.0);
  CHECK(worst_change < 1.0);
}

// SPEC-RACE-AI, Throttle: the title's pedal states, caps and brake filter
// (decoded), and the hold throttle.
void test_ai_pedals() {
  auto near = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
  constexpr double dt = drive::Vehicle::kStep;
  drive::AiPedalState st;
  drive::AiPedalIn in;
  in.hold = 0.3, in.speed = 20;
  double t = 0, b = 0;
  auto run = [&](double target) {
    in.target = target;
    drive::ai_pedals(in, dt, st, t, b);
    return st.state;
  };
  CHECK(run(25) == 3 && t == 1 && b == 0);              // well under the plan: full gas
  CHECK(run(20.5) == 3 && t == 1);                      // full gas holds above 0.25 m/s
  CHECK(run(20.2) == 2 && near(t, 0.3 * 1.2, 1e-6));    // then the hold band: H (1 + err)
  CHECK(run(19.5) == 2 && near(t, 0.3 * 0.5, 1e-6));
  CHECK(run(18.0) == 5 && t == 0);                      // 2 m/s over: brake 0.6 x the share
  const double k = std::pow(0.75, 60 * dt);
  CHECK(near(b, (1 - k) * 0.6, 1e-6));                  // rising through the brake filter
  CHECK(run(19.5) == 5);                                // braking holds while over the plan
  const double bp = b;
  CHECK(run(20.0) == 2 && t > 0);
  CHECK(near(b, std::pow(0.5, 60 * dt) * bp, 1e-6));    // falling through the filter
  CHECK(run(0) == 1 && b > 0 && t == 0);                // a target of 0 stops
  // Track edge: the last gas eased by 0.016 x the rate a 60 Hz update.
  st = {};
  in.target = 25;
  drive::ai_pedals(in, dt, st, t, b);
  in.edge = true, in.edge_rate = 2;
  drive::ai_pedals(in, dt, st, t, b);
  CHECK(st.state == 4 && near(t, 1 - 0.016 * 2 * 60 * dt, 1e-6));
  in.edge = false;
  // The gas caps: 0.6 turned off the line; 1.05 - the mean metric off the road.
  in.heading_cos = 0.0;
  drive::ai_pedals(in, dt, st, t, b);
  CHECK(near(t, 0.6, 1e-6));
  in.heading_cos = 1, in.chi = 1.2, st.slip_mean = 0.6f;
  drive::ai_pedals(in, dt, st, t, b);
  CHECK(near(t, 0.45, 1e-6));
  // Off the road with the front sliding: no full gas.
  in.chi = 0, st.slip_mean = 0, in.off_track = true, st.slip_front = 1.02f, st.state = 3;
  drive::ai_pedals(in, dt, st, t, b);
  CHECK(st.state == 3 && near(t, 0.3, 1e-6));
  st.slip_front = 1.1f;
  drive::ai_pedals(in, dt, st, t, b);
  CHECK(t == 0);
  // The slip metrics' steady states at 120 Hz.
  drive::AiCarState c;
  c.slip = {0.5f, 0.7f, 0.4f, 0.4f};
  drive::AiPedalState ms;
  for (int i = 0; i < 200; ++i) drive::ai_slip_metrics(c, dt, ms);
  CHECK(near(ms.slip_mean, 0.5, 1e-4) && near(ms.slip_front, 0.7, 1e-4));
  c.slip = {};
  for (int i = 0; i < 200; ++i) drive::ai_slip_metrics(c, dt, ms);
  CHECK(near(ms.slip_mean, 0.25, 1e-4) && near(ms.slip_front, 0.25, 1e-4));  // a wheel at 0 counts 0.25
  // The steering-saturation term's threshold: 0.97 on four-wheel drive.
  {
    drive::CarParams awd = test_car();
    awd.layout = drive::CarParams::Layout::kAll;
    CHECK(drive::ai_capacity(awd).sat_level_min == 0.97f);
    awd.layout = drive::CarParams::Layout::kRear;
    CHECK(drive::ai_capacity(awd).sat_level_min == 0.94f);
    drive::AiCapacity cap;
    cap.lateral.fill(9.81f);
    drive::AiCarState c;
    c.speed = 30;
    drive::AiYawGain g;
    g.fill(4.0f);
    drive::AiSteerState ss;
    ss.level = 0.955f;
    drive::ai_controller_steer(c, g, &cap, 0, 0, 3.0, 20.0, 0, 1, false, ss);
    CHECK(ss.sat > 0);
    cap.sat_level_min = 0.97f;
    drive::ai_controller_steer(c, g, &cap, 0, 0, 3.0, 20.0, 0, 1, false, ss);
    CHECK(ss.sat == 0);
  }
  // The hold throttle: what holds the speed against the drag.
  const drive::CarParams p = test_car();
  drive::Vehicle car(p);
  car.set_ai_controls(true);
  car.launch({0, 0, 0}, 0.0, 30.0);
  const double h = car.hold_throttle();
  std::printf("hold throttle at 30 m/s: %.3f (gear %d)\n", h, car.gear());
  CHECK(h > 0 && h < 1);
  car.reset({0, 0, 0}, 0.0);
  CHECK(car.hold_throttle() < 0.2);
}

// SPEC-RACE-AI, Speed planning (Q53): the spec's synthetic cases.
void test_speed_planner() {
  auto near = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
  CHECK(near(drive::ai_aero_factor(10), 1, 1e-9) && near(drive::ai_aero_factor(15), 1, 1e-9));
  CHECK(near(drive::ai_aero_factor(45.05), 0.9842, 1e-4) && near(drive::ai_aero_factor(60), 0.9267, 1e-4));
  CHECK(near(drive::ai_aero_factor(75.1), 0.78, 1e-9) && near(drive::ai_aero_factor(90), 0.78, 1e-9));
  CHECK(drive::AiCapacity::index(0) == 0 && drive::AiCapacity::index(2.4) == 0);
  CHECK(drive::AiCapacity::index(2.5) == 1 && drive::AiCapacity::index(120) == 39);
  drive::AiCapacity flat;
  flat.lateral.fill(9.81f);
  const double kDeg = 3.14159265358979323846 / 180;
  CHECK(near(drive::ai_corner_speed(flat, 1.0, 0.02, 0, 0, 100), 22.15, 0.01));
  CHECK(near(drive::ai_corner_speed(flat, 1.0, 0.02, 0, 5 * kDeg, 100), 24.18, 0.01));
  CHECK(near(drive::ai_corner_speed(flat, 1.0, 0.02, 0.005, 0, 100), 24.57, 0.01));
  CHECK(near(drive::ai_corner_speed(flat, 1.0, 0, 0, 0, 80), 80, 1e-9));  // no curve: the top speed
  CHECK(near(drive::ai_brake_carry(30, -15, 2), 31.0, 1e-9));
  CHECK(near(drive::ai_brake_carry(8, -15, 2), 11.136, 1e-3));
  CHECK(near(drive::ai_braking_left(14, 20, 25), 8.4, 1e-9));
  CHECK(near(drive::ai_boost(0.90, 1.5, 1.25), 1.25, 1e-9) && near(drive::ai_boost(0.84, 1.5, 1.0), 1.0, 1e-9));
  CHECK(near(drive::ai_boost(1.30, 1.5, 1.25), 1.30, 1e-9));
  drive::CarParams p = test_car();
  p.layout = drive::CarParams::Layout::kRear;
  const drive::AiCapacity rwd = drive::ai_capacity(p);
  p.layout = drive::CarParams::Layout::kAll;
  const drive::AiCapacity awd = drive::ai_capacity(p);
  CHECK(near(rwd.braking_k, 0.94, 1e-6) && near(awd.braking_k, 0.96, 1e-6));
  CHECK(awd.lateral[8] > 5 && awd.braking[8] > 5 && awd.forward[8] > rwd.forward[8] * 0.5);
  // On a circle of 60 m the plan is the corner speed all round.
  std::vector<float> xs, zs;
  const double r = 60;
  for (int i = 0; i <= 720; ++i) {
    const double a = 2 * 3.14159265358979323846 * i / 360.0;
    xs.push_back(float(r - r * std::cos(a)));
    zs.push_back(float(r * std::sin(a)));
  }
  drive::RacingLine line = drive::RacingLine::from_points(xs, zs);
  line.plan_geometry();
  CHECK(near(std::fabs(line.curvature[300]), 1 / r, 0.002));
  drive::AiField field(line, 1);
  field.set_planner(0, &flat, 1.0f, 1.0f);
  CHECK(near(field.planned_speed(0, 300), std::sqrt(9.81 * r), 0.3));
  std::printf("speed planner: test car lateral %.1f braking %.1f m/s^2 at 20 m/s, circle %.2f m/s\n", awd.lateral[8],
              awd.braking[8], field.planned_speed(0, 300));
}

// Car-to-car contact: head-on and side-on at 300 km/h closing, one
// contact pass a step; the cars never pass through each other.
void test_contact_no_tunnelling() {
  const drive::FlatGround ground;
  auto run = [&](double heading_b, double bx, double bz, double speed_a, double speed_b, const char* what) {
    drive::Vehicle a(test_car()), b(test_car());
    a.launch({0, 0, 0}, 0, speed_a);  // towards +z
    b.launch({bx, 0, bz}, heading_b, speed_b);
    std::vector<drive::ContactBody> list{{&a, 1, 0}, {&b, 1, 0}};
    const double side0 = bz != 0 ? bz : bx;
    int touches = 0;
    bool crossed = false;
    for (int i = 0; i < int(3 / drive::Vehicle::kStep); ++i) {
      a.step({}, ground);
      b.step({}, ground);
      touches += drive::resolve_contacts(list, {});
      const double rel = bz != 0 ? b.position().z - a.position().z : b.position().x - a.position().x;
      crossed |= rel * side0 < 0;
    }
    std::printf("contact %s: %d steps in contact, crossed %d\n", what, touches, int(crossed));
    CHECK(touches > 0);
    CHECK(!crossed);
  };
  run(3.14159265358979323846, 0, 40, 41.7, 41.7, "head-on");  // B towards -z
  // T-bone: A drives into B's side (B across the road, still).
  run(3.14159265358979323846 / 2, 0, 30, 83.3, 0, "side-on");
}

// Per-car forced induction (NA vs Turbo vs Supercharger), GameTorqueScale split,
// per-car brakes/downforce/drafting, LSDs, and surface VelDep/Bumpiness/water.
void test_per_car_physics() {
  const drive::FlatGround flat;

  // 1. NA car has zero boost lag and boost_factor == 1.0 throughout.
  {
    auto p = test_car();
    p.has_turbo = false;
    p.has_supercharger = false;
    drive::Vehicle v(p);
    for (int i = 0; i < 60; ++i) v.step({0, 1, 0, false}, flat);
    CHECK(std::fabs(v.boost_factor() - 1.0) < 1e-9);
    CHECK(v.boost_psi() == 0.0);
  }

  // 2. Turbo car spools from MinScale_eff toward MaxScale_eff under WOT and
  // dumps via BOV hysteresis when throttle closes (< 0.01).
  {
    auto p = test_car();
    p.has_turbo = true;
    p.turbo_tau = 0.19;
    p.turbo_power_min = 7457;
    p.turbo_power_max = 90975;
    p.boost_min_psi = 3.675;
    p.boost_max_psi = 27.93;
    p.boost_scale_min = 0.87;
    p.boost_scale_max = 1.48;
    drive::Vehicle v(p);
    v.launch({0, 0, 0}, 0, 20.0);
    v.step({0, 1, 0, false}, flat);
    const double early_boost = v.boost_factor();
    for (int i = 0; i < 120; ++i) v.step({0, 1, 0, false}, flat);
    const double spooled_boost = v.boost_factor();
    CHECK(early_boost < 1.0);
    CHECK(spooled_boost > 1.35);
    for (int i = 0; i < 60; ++i) v.step({0, 0, 0, false}, flat);
    CHECK(v.boost_factor() < spooled_boost - 0.3);
  }

  // 3. Supercharger builds boost smoothly via 15 Hz filter proportional to RPM.
  {
    auto p = test_car();
    p.has_supercharger = true;
    p.sc_zero_psi = 17.0;
    p.sc_redline_psi = 25.0;
    p.sc_scale_zero = 1.08;
    p.sc_scale_redline = 1.35;
    drive::Vehicle v(p);
    v.launch({0, 0, 0}, 0, 30.0);
    for (int i = 0; i < 60; ++i) v.step({0, 1, 0, false}, flat);
    CHECK(v.boost_factor() > 1.10);
    CHECK(v.boost_psi() > 15.0);
  }

  // 4. GameTorqueScale scales wheel drive torque without altering open-clutch free rev.
  {
    auto p1 = test_car(), p2 = test_car();
    p1.game_torque_scale = 1.0;
    p2.game_torque_scale = 0.7;
    drive::Vehicle v1(p1), v2(p2);
    // First 0.3 s clutch is open: identical free-rev RPM!
    for (int i = 0; i < 36; ++i) {
      v1.step({0, 1, 0, false}, flat);
      v2.step({0, 1, 0, false}, flat);
    }
    CHECK(std::fabs(v1.rpm() - v2.rpm()) < 1.0);
    // In gear at 30 m/s (3rd gear, no shift during 1 s): higher GameTorqueScale
    // accelerates faster.
    v1.launch({0, 0, 0}, 0, 30.0);
    v2.launch({0, 0, 0}, 0, 30.0);
    for (int i = 0; i < 120; ++i) {
      v1.step({0, 1, 0, false}, flat);
      v2.step({0, 1, 0, false}, flat);
    }
    CHECK(v1.speed() > v2.speed() + 0.5);
  }

  // 5. Per-car brakes & downforce: higher BrakeTorqueSlider yields higher
  // brake_const, and aero downforce increases wheel loads at speed.
  {
    auto p_lo = test_car(), p_hi = test_car();
    p_lo.use_per_car_brakes = p_hi.use_per_car_brakes = true;
    p_lo.brake_torque_slider = 0.5;
    p_hi.brake_torque_slider = 0.9;
    p_hi.aero_drag_long = 0.40;
    p_hi.aero_df_front = 0.35;
    p_hi.aero_df_rear = 0.55;
    drive::Vehicle vlo(p_lo), vhi(p_hi);
    CHECK(vhi.brake_const(0) > vlo.brake_const(0) * 1.5);
    CHECK(vhi.brake_const(0) > vhi.brake_const(1));
    CHECK(vhi.decel_table_at(40.0) > 0.5 && vhi.decel_table_at(40.0) <= 1.05);
    vhi.launch({0, 0, 0}, 0, 60.0);
    for (int i = 0; i < 30; ++i) vhi.step({0, 0, 0, false}, flat);
    double total_load = 0;
    for (const auto& w : vhi.wheels()) total_load += w.load;
    CHECK(total_load > p_hi.mass * p_hi.gravity + 1500.0);
  }

  // 6. Aerodynamic drafting (40-150 mph window): follower gets -28% drag at 150 mph.
  {
    drive::DraftCar follower{{0, 0, 0}, {0, 0, 1}, 67.056, 1.9, 0.3, 0.5};
    drive::DraftCar leader{{0, 0, 8.0}, {0, 0, 1}, 67.056, 1.9, 0.3, 0.5};
    const auto d = drive::compute_follower_draft(follower, &leader, 1);
    CHECK(std::fabs(d.strength - 1.0) < 1e-6);
    CHECK(std::fabs(d.body_drag - 0.72) < 1e-4);
    CHECK(std::fabs(d.front_df - 0.47) < 1e-4);
    CHECK(std::fabs(d.rear_df - 0.61) < 1e-4);
    CHECK(d.score > 0);
    // Below 40 mph: no drafting.
    follower.speed = 10.0;
    CHECK(drive::compute_follower_draft(follower, &leader, 1).strength == 0.0);
    const auto dl = drive::compute_leader_draft(1.0, 50.0);
    CHECK(std::fabs(dl.body_drag - 0.96) < 1e-4);
  }

  // 7. Surface Bumpiness (NoiseType 9 = SinWave) exact formula + clamp zero-gradient
  // + 2*Rumble, and Hand-of-God trigger on water surface / floor Y.
  {
    drive::SurfaceTyre s;
    s.bump_noise_type = 9;
    s.bump_frequency = 0.5;
    s.bump_clamp_min = -0.5;
    s.bump_clamp_max = 0.5;
    s.bump_scale = 0.02;
    s.bump_offset = 0.005;
    s.bump_rumble = 0.35;
    // At x=0, z=0: sin(0)=0 -> unclamped, dh/dx = 0.5 * 0.5 * 2*pi * 0.02.
    const auto b0 = drive::eval_surface_bumpiness(s, 0.0, 0.0);
    CHECK(std::fabs(b0.height - 0.005) < 1e-9);
    CHECK(std::fabs(b0.dh_dx - 0.5 * 0.5 * 2.0 * 3.14159265358979323846 * 0.02) < 1e-9);
    // At x=0.5, z=0.5: sin(pi/2)=1 -> h_raw=1 > ClampMax(0.5), gradients zeroed!
    const auto bc = drive::eval_surface_bumpiness(s, 0.5, 0.5);
    CHECK(std::fabs(bc.height - (0.005 + 0.5 * 0.02)) < 1e-9);
    CHECK(bc.dh_dx == 0.0 && bc.dh_dz == 0.0);

    class WaterGround : public drive::Ground {
     public:
      bool raycast(const drive::Vec3& o, const drive::Vec3& d, double max_d, drive::Hit& hit) const override {
        if (d.y > -1e-9) return false;
        const double dist = -o.y / d.y;
        if (dist < 0 || dist > max_d) return false;
        hit.distance = dist;
        hit.normal = {0, 1, 0};
        hit.grip = 0.8;
        hit.tyre.is_hand_of_god = true;
        hit.tyre.bump_rumble = 0.4;
        return true;
      }
    };
    drive::Vehicle v(test_car());
    WaterGround wg;
    v.step({}, wg);
    CHECK(v.hand_of_god_triggered());
    CHECK(std::fabs(v.surface_bump_rumble() - 0.8) < 1e-9);
    v.clear_hand_of_god();
    CHECK(!v.hand_of_god_triggered());
  }
}

// FEEL-27: the sputter tunables come from PhysicsSettings.ini.
void test_sputter_settings() {
  drive::CarParams p = test_car();
  CHECK(p.sputter_start_damage == 0.3 && p.sputter_max_per_sec == 1.0);
  cars::PhysicsSettings ps;
  ps.values["EngineDamageLevelStartSputter"] = 0.45;
  ps.values["EngineDamageMaxSputtersPerSec"] = 2.5;
  drive::apply_physics(cars::CarPhysics{}, ps, p);
  CHECK(p.sputter_start_damage == 0.45 && p.sputter_max_per_sec == 2.5);
}

// Q72: the sputter is a 0.08 s throttle cut started at random, at
// EngineDamageMaxSputtersPerSec x (damage - start) / (1 - start), halved
// outside Simulation damage.
void test_engine_sputter() {
  const drive::FlatGround flat;
  auto run = [&](double damage, bool full, double seconds, int* cut_steps, bool* throttle_cut) {
    drive::Vehicle v(test_car());
    v.reset({0, 0, 0}, 0.0);
    drive::Vehicle::DamageMods d;
    d.engine_damage = damage;
    d.sputter_full_rate = full;
    v.set_damage_mods(d);
    drive::Input in{};
    in.throttle = 1.0;
    int cut = 0;
    bool zero = true;
    for (int i = 0; i < int(seconds / drive::Vehicle::kStep); ++i) {
      v.step(in, flat);
      if (v.sputtering()) {
        ++cut;
        zero = zero && v.engine_throttle() == 0.0;
      }
    }
    if (cut_steps) *cut_steps = cut;
    if (throttle_cut) *throttle_cut = zero;
    return v.sputter_count();
  };
  CHECK(run(0.3, true, 60, nullptr, nullptr) == 0);
  CHECK(run(0.0, true, 60, nullptr, nullptr) == 0);
  int cut = 0;
  bool zero = false;
  const uint32_t full = run(1.0, true, 300, &cut, &zero);
  const uint32_t half = run(1.0, false, 300, nullptr, nullptr);
  const uint32_t mid = run(0.65, true, 300, nullptr, nullptr);
  std::printf("sputter: %u in 300 s at full damage (%u halved, %u at 0.65); %d cut steps (%.3f s each)\n", full, half,
              mid, cut, full ? cut * drive::Vehicle::kStep / full : 0.0);
  CHECK(full > 240 && full < 330);  // ~1 a second, less the 0.08 s cuts
  CHECK(half > 110 && half < 190);
  CHECK(mid > 110 && mid < 190);
  CHECK(zero);
  CHECK(full > 0 && std::fabs(cut * drive::Vehicle::kStep / full - 0.0833) < 0.01);
}

// Q100/Q101: the body-contact settings and MAXData spheres reach CarParams.
void test_body_collision_settings() {
  cars::PhysicsSettings ps;
  std::string err;
  CHECK(cars::parse_physics_settings("GimbalLock\\GimbalLockOn false\nGimbalLock\\GimbalSpringK 40\n"
                                     "CarCollSphereMinDistAboveGround 0.12\nCollision\\FrictionHighSpeedMph 45\n"
                                     "CarObjWorldCollTorqueScaleYaw 0.6\n",
                                     ps, &err));
  cars::CarPhysics cp;
  cp.curves["MAXData.CollSpheres"] = {0.0001, 0.3748, 1.734099, 0.31, 2, -0.81, 0.034932, 2.129728, 0.05, 1};
  cp.values["MAXData.BoundingBox.MinY"] = -0.2014;
  cp.values["MAXData.BottomCenterWheelbasePos.z"] = -0.000358;
  drive::CarParams p = test_car();
  drive::apply_physics(cp, ps, p);
  const auto& bc = p.body_collision;
  CHECK(!bc.gimbal_on && bc.gimbal_k == 40 && bc.gimbal_cos0 == 0.87);
  CHECK(bc.min_above_ground == 0.12 && bc.ground_min_above_ground == 0.4);
  CHECK(bc.friction_low_mph == 10 && bc.friction_high_mph == 45);
  CHECK(bc.world_torque_scale.y == 0.6 && bc.world_torque_scale.x == 0.55);
  CHECK(bc.spheres.size() == 2);
  if (bc.spheres.size() == 2) {
    CHECK(std::fabs(bc.spheres[0].centre.y - 0.5762) < 1e-9 && std::fabs(bc.spheres[0].centre.z - 1.734457) < 1e-9);
    CHECK(bc.spheres[0].radius == 0.31 && bc.spheres[0].flags == 2 && bc.spheres[1].flags == 1);
  }
}

// FEEL-11: the original's FrictionTorqueMod law on the shipped table values
// (PhysicsSettings.ini FrictionTorqueMod defaults in CarParams).
void test_friction_torque_mod() {
  // The loader reads every row from the settings (made-up values here).
  {
    cars::PhysicsSettings ps;
    ps.values["FrictionTorqueMod\\Mass0KG"] = 800;
    ps.values["FrictionTorqueMod\\NoAssists1\\TurnClamp1"] = 77;
    ps.values["FrictionTorqueMod\\SimulationSteering0\\CosYawAngle1"] = 0.5;
    ps.values["FrictionTorqueMod\\AWDModifiers0\\StraightenScale0"] = 0.9;
    ps.values["FrictionTorqueMod\\OffRoadModifiers1\\StraightenClamp0"] = 1.3;
    ps.values["FrictionTorqueMod\\FWDModifiers0\\TurnScale0"] = 0.1;  // never taken by the original
    drive::CarParams p = test_car();
    drive::apply_physics(cars::CarPhysics{}, ps, p);
    const auto& m = p.friction_torque_mod;
    CHECK(m.mass0_kg == 800 && m.mass1_kg == drive::CarParams{}.friction_torque_mod.mass1_kg);
    CHECK(m.steering[1][1].turn_clamp1 == 77 && m.steering[1][0].turn_clamp1 == 120);
    CHECK(m.steering[2][0].cos_yaw1 == 0.5);
    CHECK(m.drivetrain[0].straighten_scale0 == 0.9 && m.drivetrain[0].turn_scale0 == 1);
    CHECK(m.off_road[1].straighten_clamp0 == 1.3);
  }
  const drive::CarParams::FrictionTorqueMod m{};
  const double deg = 3.14159265358979323846 / 180.0, i_yaw = 1000;
  auto near = [](double a, double b) { return std::fabs(a - b) < 1e-6 * std::max(1.0, std::fabs(b)); };
  auto ftm = [&](int table, bool off_road, double mass, double v2, double cos_yaw, double yaw_rate, double t) {
    return drive::friction_torque_mod_yaw(m, table, off_road, mass, i_yaw, v2, cos_yaw, yaw_rate, t);
  };
  // Below 8 m/s there is no rescaling, only the low-speed clamp (Clamp deg/s^2
  // times the yaw inertia, times 10).
  CHECK(near(ftm(1, false, 909.1, 50, 0.9, 1, 5000), 5000));
  CHECK(near(ftm(1, false, 909.1, 50, 1.0, 1, 1e6), 180 * deg * i_yaw * 10));
  // From 15 m/s: past the second yaw cosine the Turn/Straighten scale and clamp
  // of index 1 apply in full.
  CHECK(near(ftm(1, false, 909.1, 400, 0.9, 1, 1000), 700));    // turning: TurnScale1 0.7
  CHECK(near(ftm(1, false, 909.1, 400, 0.9, -1, 1000), 800));   // straightening: StraightenScale1 0.8
  CHECK(near(ftm(1, false, 909.1, 400, 0.9, 1, 5000), 120 * deg * i_yaw));   // TurnClamp1
  CHECK(near(ftm(1, false, 909.1, 400, 0.9, 1, -5000), -140 * deg * i_yaw));  // StraightenClamp1
  // Within the first cosine, index 0; midway between them, half of each.
  CHECK(near(ftm(1, false, 909.1, 400, 1.0, 1, 1e6), 180 * deg * i_yaw));
  CHECK(near(ftm(1, false, 909.1, 400, 0.5 * (0.996194 + 0.961261), 1, 1000), 850));
  // Half the speed blend: half the scale.
  CHECK(near(ftm(1, false, 909.1, 64 + 161 * 0.5, 0.9, 1, 1000), 850));
  // The heavy row past Mass1KG.
  CHECK(near(ftm(1, false, 1590.9, 400, 1.0, 1, 1e6), 150 * deg * i_yaw));
  CHECK(near(ftm(1, false, 1250.0, 400, 1.0, 1, 1e6), 165 * deg * i_yaw));
  // With all four wheels off-road the OffRoadModifiers multiply in.
  CHECK(near(ftm(1, true, 909.1, 400, 1.0, 1, 1000), 1200));
  CHECK(near(ftm(1, true, 909.1, 400, 1.0, 1, 1e6), 180 * 1.1111 * deg * i_yaw));
  // Simulation steering: the shipped rows leave the torque alone.
  CHECK(near(ftm(2, false, 909.1, 400, 0.9, 1, 5000), 5000));
  // WithAssists and NoAssists ship the same rows.
  CHECK(near(ftm(0, false, 1200, 300, 0.97, 1, 3000), ftm(1, false, 1200, 300, 0.97, 1, 3000)));
}

// FEEL-15: the pad stick maps linearly onto the speed-sensitive lock at any
// speed (no SteeringWheelFilter shaping on the pad).
void test_pad_steering_linear() {
  const auto p = test_car();
  const drive::FlatGround flat;
  for (double speed : {10.0, 40.0}) {
    drive::Vehicle v(p);
    v.launch({0, 0, 0}, 0.0, speed);
    drive::Input in{};
    in.steer = 0.5;
    in.throttle = 0.3;
    for (int i = 0; i < 120; ++i) v.step(in, flat);
    const auto& w = v.wheels();
    std::printf("pad half stick at %.0f m/s: steer %.4f of lock %.4f (front slips %.3f %.3f)\n", speed,
                v.steer_angle(), v.steer_lock(), w[0].slip_angle, w[1].slip_angle);
    CHECK(std::fabs(v.steer_angle() - 0.5 * v.steer_lock()) < 0.02 * v.steer_lock() + 1e-4);
    // In an ordinary turn the fronts slip against the engine's steer sign,
    // so the pad's front countersteer state (Q231) stays off.
    CHECK(v.steer_angle() * w[0].slip_angle < 0 && v.steer_angle() * w[1].slip_angle < 0);
  }
}

// Q69/Q231: the pad's turning rate at speed is MaxAngVelTurning x the
// SteerSpeed factor x the curve's share of the lock at SlowSpeed (speed
// clamped to [SlowSpeed, FastSpeed]) x the FastRateScale blend; across
// centre the target is the stick times the full lock at the straighten rate.
void test_pad_rate_law() {
  auto p = test_car();
  p.steer_slow_speed = 0.44704;  // the shipped rows: 1 and 120 mph, scale 1
  p.steer_fast_speed = 53.6448;
  p.steer_fast_rate_scale = 1.0;
  p.steer_accel_time = 0.01;  // the shipped SteerAccelTimeToMaxRate
  const drive::FlatGround flat;
  drive::Vehicle v(p);
  v.launch({0, 0, 0}, 0.0, 20.0);
  drive::Input in{};
  in.steer = 0.9;  // under the 0.976 hold, so no lock extension
  in.throttle = 0.3;
  double rate = 0.0;
  for (int i = 0; i < 240; ++i) {
    const double a0 = v.steer_angle();
    v.step(in, flat);
    if (i == 3) rate = (v.steer_angle() - a0) / drive::Vehicle::kStep;
  }
  const double cap = v.steer_full_lock();
  const double share = std::min(cap, v.steer_curve_at(std::hypot(v.velocity().x, v.velocity().y, v.velocity().z))) /
                       std::min(cap, v.steer_curve_at(p.steer_slow_speed));
  const double want = p.steer_rate * p.steer_speed_scale * share;
  std::printf("pad rate at 20 m/s: %.3f rad/s (law %.3f, share %.3f); settled %.4f of 0.9 x lock %.4f\n", rate, want,
              share, v.steer_angle(), 0.9 * v.steer_lock());
  CHECK(std::fabs(rate - want) < 0.05 * want);
  CHECK(std::fabs(v.steer_angle() - 0.9 * v.steer_lock()) < 0.02 * v.steer_lock());
  // Back through centre: the straighten rate, unscaled by the share.
  in.steer = -0.9;
  v.step(in, flat);
  v.step(in, flat);
  const double a0 = v.steer_angle();
  v.step(in, flat);
  const double back = (a0 - v.steer_angle()) / drive::Vehicle::kStep;
  std::printf("pad straighten at 20 m/s: %.3f rad/s (law %.3f)\n", back, p.straighten_rate * p.steer_speed_scale);
  CHECK(back > 0.9 * std::min(p.straighten_rate * p.steer_speed_scale, a0 / drive::Vehicle::kStep));
}

// FEEL-02: no thermal grip factor. The empty curve is 1 at any temperature,
// and even a loaded List_TireAffectCurve far from 1 leaves a long, hot
// skid's grip unchanged.
void test_no_thermal_grip() {
  const drive::CarParams::TireAffectCurve none{};
  CHECK(none.evaluate(20) == 1.0 && none.evaluate(170) == 1.0);
  const auto p = test_car();
  auto hot = p;
  hot.tire_affect_curve.values = {0.6, 0.6, 0.6};
  const drive::FlatGround flat;
  drive::Vehicle a(p), b(hot);
  a.launch({0, 0, 0}, 0.0, 25.0);
  b.launch({0, 0, 0}, 0.0, 25.0);
  for (int i = 0; i < 600; ++i) {
    a.step({0.6, 0.5, 0.0, false}, flat);
    b.step({0.6, 0.5, 0.0, false}, flat);
  }
  CHECK(drive::trace_row(a) == drive::trace_row(b));
}

void test_assists_and_transmission() {
  const drive::FlatGround flat;

  // 1. Manual transmission: does not auto-upshift, responds to explicit shift requests.
  {
    auto p = test_car();
    drive::Vehicle v(p);
    drive::VehicleAssists a;
    a.shifting = 1;  // Manual
    v.set_assists(a);
    for (int i = 0; i < 120 * 5; ++i) v.step({0, 1, 0, false}, flat);
    CHECK(v.gear() == 1);  // stayed in 1st at redline without auto-upshift
    v.step({0, 1, 0, false, 0, +1}, flat);
    for (int i = 0; i < 120; ++i) v.step({0, 1, 0, false}, flat);
    CHECK(v.gear() == 2);
    // Downshift back to 1 and then Reverse.
    v.step({0, 0, 0, false, 0, -1}, flat);
    for (int i = 0; i < 120; ++i) v.step({0, 0, 0, false}, flat);
    CHECK(v.gear() == 1);
  }

  // 2. Manual + Clutch: clutch pedal controls torque engagement; shifting
  // without clutch adds transmission damage while clutched shift adds zero.
  {
    auto p = test_car();
    drive::Vehicle v(p);
    drive::VehicleAssists a;
    a.shifting = 2;  // Manual + Clutch
    v.set_assists(a);
    // Hold clutch down (1.0) and rev engine: engine revs freely, car stays still.
    for (int i = 0; i < 60; ++i) v.step({0, 1, 0, false, 1.0, 0}, flat);
    CHECK(v.rpm() > 3000.0);
    CHECK(std::fabs(v.speed()) < 0.2);
    // Clean shift with clutch pressed (1.0): zero transmission damage.
    v.step({0, 0.5, 0, false, 1.0, +1}, flat);
    CHECK(v.gear() == 2);
    CHECK(v.transmission_damage() == 0.0);
    // Release clutch (0.0): car accelerates forward.
    for (int i = 0; i < 120; ++i) v.step({0, 1, 0, false, 0.0, 0}, flat);
    CHECK(v.speed() > 5.0);
    // Shift without clutch (0.0 <= 0.8): damages transmission.
    v.step({0, 1, 0, false, 0.0, +1}, flat);
    CHECK(v.gear() == 3);
    CHECK(v.transmission_damage() > 0.0);
  }

  // 3. ABS Off (braking == 2): below NoABSBrakeTravel (0.975), no ABS cycling.
  {
    auto p = test_car();
    p.front_brake_torque = 6000;
    p.rear_brake_torque = 4500;
    drive::Vehicle v_on(p), v_off(p);
    drive::VehicleAssists a_off;
    a_off.braking = 2;
    v_off.set_assists(a_off);
    v_on.launch({0, 0, 0}, 0, 30.0);
    v_off.launch({0, 0, 0}, 0, 30.0);
    bool saw_abs_on = false, saw_abs_off = false;
    for (int i = 0; i < 120; ++i) {
      v_on.step({0, 0, 0.90, false}, flat);
      v_off.step({0, 0, 0.90, false}, flat);
      saw_abs_on |= v_on.abs_active();
      saw_abs_off |= v_off.abs_active();
    }
    CHECK(saw_abs_on);
    CHECK(!saw_abs_off);
  }

  // 4. TCS: activates under high-torque RWD wheelspin and trims slip.
  {
    auto p = test_car();
    p.layout = drive::CarParams::Layout::kRear;
    p.torque_scale = 950;  // huge torque to force wheelspin
    drive::Vehicle v_no(p), v_tcs(p);
    drive::VehicleAssists a_tcs;
    a_tcs.traction_control = true;
    v_tcs.set_assists(a_tcs);
    v_no.launch({0, 0, 0}, 0, 10.0);
    v_tcs.launch({0, 0, 0}, 0, 10.0);
    bool saw_tcs = false;
    double peak_slip_no = 0, peak_slip_tcs = 0;
    for (int i = 0; i < 120; ++i) {
      v_no.step({0, 1, 0, false}, flat);
      v_tcs.step({0, 1, 0, false}, flat);
      saw_tcs |= v_tcs.tcs_active();
      peak_slip_no = std::max(peak_slip_no, std::fabs(v_no.wheels()[2].slip_ratio));
      peak_slip_tcs = std::max(peak_slip_tcs, std::fabs(v_tcs.wheels()[2].slip_ratio));
    }
    CHECK(saw_tcs);
    CHECK(peak_slip_tcs < peak_slip_no);
  }

  // 5. STM: activates during high-speed oversteer slide and applies counter-yaw brake.
  {
    auto p = test_car();
    drive::Vehicle v(p);
    drive::VehicleAssists a_stm;
    a_stm.stability_control = true;
    v.set_assists(a_stm);
    v.launch({0, 0, 0}, 0, 28.0);
    // Induce a slide with handbrake + steering.
    bool saw_stm = false;
    for (int i = 0; i < 120; ++i) {
      v.step({0.6, 0.3, 0, i < 40}, flat);
      saw_stm |= v.stm_active();
    }
    CHECK(saw_stm);
  }

  // FEEL-10: the TCS slip targets come from the car's gamedb row
  // (Data_Car.AssistsTCSSlipDefTakeoff / ...Moving; made-up values here).
  {
    cars::CarPhysics cp;
    cp.values["Data_Car.AssistsTCSSlipDefTakeoff"] = 12.5;
    cp.values["Data_Car.AssistsTCSSlipDefMoving"] = 0.65;
    const cars::PhysicsSettings ps;
    drive::CarParams p = test_car();
    drive::apply_physics(cp, ps, p);
    CHECK(std::fabs(p.tcs_slip_takeoff - 12.5) < 1e-12 && std::fabs(p.tcs_slip_moving - 0.65) < 1e-12);
    // A car without the columns keeps the engine's fallbacks.
    drive::CarParams q = test_car();
    std::vector<std::string> notes;
    drive::apply_physics(cars::CarPhysics{}, ps, q, &notes);
    CHECK(q.tcs_slip_takeoff == drive::CarParams{}.tcs_slip_takeoff &&
          q.tcs_slip_moving == drive::CarParams{}.tcs_slip_moving);
    bool noted = false;
    for (const auto& n : notes) noted |= n.find("AssistsTCSSlipDefTakeoff") != std::string::npos;
    CHECK(noted);
    // Behaviour: below the speed threshold the takeoff target applies; a
    // high one (the disc's 33) lets a launch from rest spin far longer
    // before TCS cuts in than the old 1.0.
    auto launch = [](double takeoff) {
      auto c = test_car();
      c.layout = drive::CarParams::Layout::kRear;
      c.torque_scale = 950;
      c.tcs_slip_takeoff = takeoff;
      drive::Vehicle v(c);
      drive::VehicleAssists a;
      a.traction_control = true;
      v.set_assists(a);
      const drive::FlatGround flat;
      int active = 0;
      for (int i = 0; i < 240 && v.speed() < 5.0; ++i) {
        v.step({0, 1, 0, false}, flat);
        active += v.tcs_active();
      }
      return active;
    };
    const int cut_low = launch(1.0), cut_high = launch(33.0);
    std::printf("TCS launch from rest: active %d steps (takeoff 1.0), %d (33.0)\n", cut_low, cut_high);
    CHECK(cut_low > 0 && cut_high < cut_low);
  }

  // The STM window comes from the car's AutoSteerOverrides row
  // (AutoSteerOverrides.STMSlipMin / STMSlipMax; made-up values here).
  {
    cars::CarPhysics cp;
    cp.values["AutoSteerOverrides.STMSlipMin"] = 1.75;
    cp.values["AutoSteerOverrides.STMSlipMax"] = 3.25;
    const cars::PhysicsSettings ps;
    drive::CarParams p = test_car();
    drive::apply_physics(cp, ps, p);
    CHECK(std::fabs(p.stm_slip_min - 1.75) < 1e-12 && std::fabs(p.stm_slip_max - 3.25) < 1e-12);
    // A car without the row keeps the engine's fallbacks, and says so.
    drive::CarParams q = test_car();
    std::vector<std::string> notes;
    drive::apply_physics(cars::CarPhysics{}, ps, q, &notes);
    CHECK(q.stm_slip_min == drive::CarParams{}.stm_slip_min && q.stm_slip_max == drive::CarParams{}.stm_slip_max);
    bool noted = false;
    for (const auto& n : notes) noted |= n.find("AutoSteerOverrides.STMSlipMin") != std::string::npos;
    CHECK(noted);
    // Behaviour: the window gates STM. The slide of test 5 trips the
    // fallback window; one far above any slip it reaches never trips.
    auto slide = [](double lo, double hi) {
      auto c = test_car();
      c.stm_slip_min = lo;
      c.stm_slip_max = hi;
      drive::Vehicle v(c);
      drive::VehicleAssists a;
      a.stability_control = true;
      v.set_assists(a);
      v.launch({0, 0, 0}, 0, 28.0);
      const drive::FlatGround flat;
      int active = 0;
      for (int i = 0; i < 120; ++i) {
        v.step({0.6, 0.3, 0, i < 40}, flat);
        active += v.stm_active();
      }
      return active;
    };
    const int fallback = slide(1.2, 2.5), wide = slide(1e6, 2e6);
    std::printf("STM slide: active %d steps (window 1.2-2.5), %d (1e6-2e6)\n", fallback, wide);
    CHECK(fallback > 0 && wide == 0);
  }

  // 6. Steering assist modes (Assisted vs Normal vs Simulation) + Auto-Brake.
  {
    auto p = test_car();
    p.max_steer = 0.733;
    p.max_steer_filtered = 0.55;
    drive::Vehicle v_ast(p), v_sim(p);
    drive::VehicleAssists a0, a2;
    a0.steering = 0;  // Assisted
    a2.steering = 2;  // Simulation
    v_ast.set_assists(a0);
    v_sim.set_assists(a2);
    // At rest, Assisted caps to SteerMaxAngleFiltered while Simulation uses
    // SteerMaxAngle; at 18 m/s, where the speed-sensitive lock is below both
    // caps, the lock is the same in every mode (FEEL-16 d: no per-mode gees).
    CHECK(v_sim.steer_lock() > v_ast.steer_lock());
    v_ast.launch({0, 0, 0}, 0, 18.0);
    v_sim.launch({0, 0, 0}, 0, 18.0);
    CHECK(v_ast.steer_lock() < p.max_steer_filtered);
    CHECK(std::fabs(v_sim.steer_lock() - v_ast.steer_lock()) < 1e-12);

    drive::Input in{0, 1.0, 0.0, false};
    drive::apply_auto_brake(in, 35.0, 25.0);
    CHECK(in.brake > 0.5 && in.throttle < 0.1);
    drive::Input in_ok{0, 1.0, 0.0, false};
    drive::apply_auto_brake(in_ok, 20.0, 25.0);
    CHECK(in_ok.brake == 0.0 && in_ok.throttle == 1.0);
  }
}

int main() {
  test_save();
  test_rest_pose();
  test_determinism();
  test_launch_and_brake();
  test_steer_right_goes_to_plus_x();
  test_reverse();
  test_script();
  test_mesh_ground();
  test_rest_hold();
  test_model_to_world();
  test_oracle_lap();
  test_ai_field();
  test_ai_pedals();
  test_ai_slip_rule();
  test_ai_controller();
  test_speed_planner();
  test_contact_no_tunnelling();
  // Slice 6: contact torque role scaling (PlayerTorqueScale = 0,
  // RaceCarTorqueScaleVsTrafficInRace = 0.05) and non-finite guards.
  {
    auto p = test_car();
    drive::Vehicle player(p), race_ai(p), traffic(p);
    // Offset front-corner contact (normal along +z, lateral offset x = 0.7 m):
    // player angular_scale 0 -> zero yaw impulse; race AI angular_scale 0.2 -> non-zero yaw.
    player.launch({0, 0, 0}, 0, 10.0);
    race_ai.launch({0.7, 0, 3.8}, 0, -10.0);
    std::vector<drive::ContactBody> bodies{
        {&player, 0.0, 0, drive::ContactRole::kPlayer, 0.0},
        {&race_ai, 0.2, 0, drive::ContactRole::kRaceAi, 0.05},
    };
    const int n1 = drive::resolve_contacts(bodies, {0.0, 0.05});
    CHECK(n1 == 1);
    CHECK(std::fabs(player.angular_velocity().y) < 1e-9);
    CHECK(std::fabs(race_ai.angular_velocity().y) > 1e-4);

    // Race AI vs Traffic uses angular_scale_vs_traffic (0.05 < 0.2).
    drive::Vehicle ai_vs_ai(p), ai_vs_tr(p), peer(p), tr_peer(p);
    ai_vs_ai.launch({0, 0, 0}, 0, 12.0);
    peer.launch({0.7, 0, 3.8}, 0, -12.0);
    ai_vs_tr.launch({0, 0, 0}, 0, 12.0);
    tr_peer.launch({0.7, 0, 3.8}, 0, -12.0);
    std::vector<drive::ContactBody> b_ai{
        {&ai_vs_ai, 0.2, 0, drive::ContactRole::kRaceAi, 0.05},
        {&peer, 0.2, 0, drive::ContactRole::kRaceAi, 0.05},
    };
    std::vector<drive::ContactBody> b_tr{
        {&ai_vs_tr, 0.2, 0, drive::ContactRole::kRaceAi, 0.05},
        {&tr_peer, 0.25, 0, drive::ContactRole::kTraffic, -1.0},
    };
    CHECK(drive::resolve_contacts(b_ai, {0.0, 0.05}) == 1);
    CHECK(drive::resolve_contacts(b_tr, {0.0, 0.05}) == 1);
    CHECK(std::fabs(ai_vs_tr.angular_velocity().y) < std::fabs(ai_vs_ai.angular_velocity().y));

    // Non-finite position must not infect other cars.
    drive::Vehicle nan_car(p), ok_car(p);
    const double qnan = std::numeric_limits<double>::quiet_NaN();
    nan_car.place({qnan, 0, 0}, 0);
    ok_car.launch({0, 0, 0}, 0, 10.0);
    std::vector<drive::ContactBody> b_nan{{&nan_car, 0.2, 0}, {&ok_car, 0.2, 0}};
    CHECK(drive::resolve_contacts(b_nan, {0.0, 0.05}) == 0);
    CHECK(std::isfinite(ok_car.position().x) && std::isfinite(ok_car.velocity().z));
  }
  test_per_car_physics();
  test_friction_torque_mod();
  test_sputter_settings();
  test_engine_sputter();
  test_body_collision_settings();
  test_pad_steering_linear();
  test_pad_rate_law();
  test_no_thermal_grip();
  test_assists_and_transmission();
  // Pillar 1 deep behavioral parity tests:
  // 1) HandbrakeGripMultiplier scales rear k_x/k_y on surface with handbrake_grip < 1.
  {
    class HandbrakeGround : public drive::Ground {
     public:
      explicit HandbrakeGround(double hb_grip) : hb_(hb_grip) {}
      bool raycast(const drive::Vec3& o, const drive::Vec3& d, double max_d, drive::Hit& hit) const override {
        if (d.y >= -1e-9) return false;
        const double t = -o.y / d.y;
        if (t < 0 || t > max_d) return false;
        hit.distance = t;
        hit.normal = {0, 1, 0};
        hit.grip = 1.0;
        hit.tyre.handbrake_grip = hb_;
        return true;
      }
     private:
      double hb_;
    };
    const auto p = test_car();
    const HandbrakeGround g_full(1.0), g_slick(0.35);
    drive::Vehicle v_full(p), v_slick(p);
    v_full.launch({0, 0, 0}, 0, 22.0);
    v_slick.launch({0, 0, 0}, 0, 22.0);
    for (int i = 0; i < 60; ++i) {
      v_full.step({0.35, 0.0, 0.0, true}, g_full);
      v_slick.step({0.35, 0.0, 0.0, true}, g_slick);
    }
    const auto f_full = drive::rotate(v_full.orientation(), {0, 0, 1});
    const auto f_slick = drive::rotate(v_slick.orientation(), {0, 0, 1});
    const double yaw_full = std::fabs(std::atan2(f_full.x, f_full.z));
    const double yaw_slick = std::fabs(std::atan2(f_slick.x, f_slick.z));
    CHECK(yaw_slick > yaw_full * 1.15);
  }
  // 2) Forza dynamic pad steering: SteerAccelTimeToMaxRate ramps angular velocity
  //    smoothly and SteerSpeedSensitiveFastRateScale slows turn-in at highway speed.
  {
    auto p = test_car();
    p.steer_accel_time = 0.20;
    p.steer_slow_speed = 10.0;
    p.steer_fast_speed = 60.0;
    p.steer_fast_rate_scale = 0.35;
    const drive::FlatGround flat;
    drive::Vehicle v_slow(p), v_fast(p);
    v_slow.launch({0, 0, 0}, 0, 8.0);
    v_fast.launch({0, 0, 0}, 0, 55.0);
    // First step: acceleration ramp limits initial step below instant full steer_rate * dt.
    v_slow.step({1.0, 0.0, 0.0, false}, flat);
    const double first_step_angle = v_slow.steer_angle();
    CHECK(first_step_angle > 0.0);
    CHECK(first_step_angle < p.steer_rate * drive::Vehicle::kStep * 0.95);
    // Over 0.15 s at 55 m/s vs 8 m/s, fast speed steers less aggressively.
    for (int i = 0; i < 18; ++i) {
      v_slow.step({1.0, 0.0, 0.0, false}, flat);
      v_fast.step({1.0, 0.0, 0.0, false}, flat);
    }
    CHECK(v_fast.steer_angle() < v_slow.steer_angle());
  }
  // 3) Aerodynamic drafting: follower behind leader at 50 m/s accelerates faster with set_draft.
  {
    auto p = test_car();
    p.aero_drag_long = 0.55;
    p.aero_df_front = 0.25;
    p.aero_df_rear = 0.35;
    p.drag_quadratic = 0.55;
    p.drag_constant = 0.0;
    const drive::FlatGround flat;
    drive::Vehicle v_clean(p), v_draft(p);
    v_clean.launch({0, 0, 0}, 0, 48.0);
    v_draft.launch({0, 0, 0}, 0, 48.0);
    drive::DraftCar leader{{0, 0, 12.0}, {0, 0, 1}, 48.0, 1.9, 0.25, 0.35};
    drive::DraftCar follower{{0, 0, 0}, {0, 0, 1}, 48.0, 1.9, 0.25, 0.35};
    const auto ds = drive::compute_follower_draft(follower, &leader, 1);
    CHECK(ds.strength > 0.8 && ds.body_drag < 0.85 && ds.front_df < 0.70);
    v_draft.set_draft(ds);
    for (int i = 0; i < 360; ++i) {
      v_clean.step({0, 1.0, 0, false}, flat);
      v_draft.step({0, 1.0, 0, false}, flat);
    }
    CHECK(v_draft.speed() > v_clean.speed() + 0.35);
  }
  // 4) LSD reverse lock uses signed T_drive (T_decel = 0 in reverse, not T_accel)
  //    and RWD launch assist reduces low-speed excessive rear slip.
  {
    class SplitMuGround : public drive::Ground {
     public:
      bool raycast(const drive::Vec3& o, const drive::Vec3& d, double max_d, drive::Hit& hit) const override {
        if (d.y >= -1e-9) return false;
        const double t = -o.y / d.y;
        if (t < 0 || t > max_d) return false;
        hit.distance = t;
        hit.normal = {0, 1, 0};
        hit.grip = o.x < 0 ? 0.25 : 1.0;  // left wheels on ice, right wheels on dry asphalt
        return true;
      }
    };
    auto p_lsd = test_car();
    p_lsd.layout = drive::CarParams::Layout::kRear;
    p_lsd.rear_lsd = {600.0, 0.0, 50.0, 50.0};  // high accel lock, zero decel lock
    auto p_open = p_lsd;
    p_open.rear_lsd = {0.0, 0.0, 50.0, 50.0};
    const SplitMuGround split_g;
    drive::Vehicle v_lsd_rev(p_lsd), v_open_rev(p_open);
    // Drive in reverse on split-mu: signed T_drive <= 0 selects decel=0 so LSD matches open diff.
    for (int i = 0; i < 180; ++i) {
      v_lsd_rev.step({0, 0, 1.0, false}, split_g);
      v_open_rev.step({0, 0, 1.0, false}, split_g);
    }
    CHECK(v_lsd_rev.gear() == -1);
    const double dw_lsd = std::fabs(v_lsd_rev.wheels()[2].omega - v_lsd_rev.wheels()[3].omega);
    const double dw_open = std::fabs(v_open_rev.wheels()[2].omega - v_open_rev.wheels()[3].omega);
    CHECK(std::fabs(dw_lsd - dw_open) < 1.0);

    auto p = test_car();
    p.layout = drive::CarParams::Layout::kRear;
    p.torque_scale = 750.0;
    p.enable_rwd_launch_assist = true;
    auto p_no_assist = p;
    p_no_assist.enable_rwd_launch_assist = false;
    const drive::FlatGround flat;
    drive::Vehicle v_assist(p), v_raw(p_no_assist);
    double max_slip_assist = 0.0, max_slip_raw = 0.0;
    for (int i = 0; i < 90; ++i) {
      v_assist.step({0, 1.0, 0, false}, flat);
      v_raw.step({0, 1.0, 0, false}, flat);
      max_slip_assist = std::max(max_slip_assist, std::fabs(v_assist.wheels()[2].slip_ratio));
      max_slip_raw = std::max(max_slip_raw, std::fabs(v_raw.wheels()[2].slip_ratio));
    }
    CHECK(max_slip_raw > 1.0);
    CHECK(max_slip_assist <= max_slip_raw + 0.05);
  }
  // 5) Pillar 4: ai_capacity incorporates aero downforce scaled by ai_aero_factor(v).
  {
    auto p_low = test_car();
    auto p_df = p_low;
    p_df.aero_df_front = 0.35;
    p_df.aero_df_rear = 0.55;
    const auto cap_low = drive::ai_capacity(p_low);
    const auto cap_df = drive::ai_capacity(p_df);
    // At 60 m/s (entry 24), high-downforce car has substantially higher lateral & braking capacity.
    CHECK(cap_df.lateral[24] > cap_low.lateral[24] + 1.0f);
    CHECK(cap_df.braking[24] > cap_low.braking[24] + 1.0f);
  }
  // 6) Round 3 P1-2: Strict bumpstop wheel-arch clamp, per-surface wall glide vs dirt bank friction,
  //    and fractional off-road yaw stabilization.
  {
    const auto p = test_car();
    const drive::FlatGround flat;
    drive::Vehicle v(p);
    v.reset({0, 3.0, 0}, 0);
    for (int i = 0; i < 180; ++i) {
      v.step({0, 0, 0, false}, flat);
      for (int w = 0; w < drive::kWheels; ++w) {
        const int a = drive::axle_of(w);
        CHECK(v.wheels()[w].centre.y <= p.wheel_centres[w].y + p.bump_travel[a] + 1e-9);
      }
    }
  }
  // 7) LAB-87: Automatic gearbox downshifts cleanly from high gear (5) to gear 1
  //    when speed drops to 0 km/h (coasting or sudden stop) and never locks out throttle.
  {
    const auto p = test_car();
    const drive::FlatGround flat;
    drive::Vehicle v(p);
    // Launch at 55 m/s (~198 km/h) so vehicle starts in high gear (gear 5).
    v.launch({0, 0, 0}, 0, 55.0);
    CHECK(v.gear() >= 4);
    // Sudden stop (simulating crash/spinout stopping wheels in high gear).
    v.set_velocity({0, 0, 0}, {0, 0, 0});
    for (int i = 0; i < 60; ++i) v.step({0, 0, 0, false}, flat);
    CHECK(v.gear() == 1);
    // Pressing throttle must immediately drive the car forward without needing reset().
    double max_thr = 0.0;
    for (int i = 0; i < 120 * 2; ++i) {
      v.step({0, 1.0, 0, false}, flat);
      max_thr = std::max(max_thr, v.engine_throttle());
    }
    CHECK(v.speed() > 10.0);
    CHECK(max_thr > 0.5);
  }
  // 8) LAB-87: Post-crash throttle recovery & soft wall/fence impact response
  //    (no kClutchOut stall deadlock, no vertical launch or wild spin).
  {
    const auto p = test_car();
    // Flat floor + angled vertical wall/fence across z = 20 m.
    const float h = 200.0f;
    drive::MeshGround arena({
        // Ground plane y = 0
        {-h, 0, -h}, {h, 0, h}, {h, 0, -h}, {-h, 0, -h}, {-h, 0, h}, {h, 0, h},
        // Vertical wall at z = 20 (angled slightly)
        {-30, 0, 20}, {30, 0, 22}, {30, 4, 22}, {-30, 0, 20}, {30, 4, 22}, {-30, 4, 20},
    });
    drive::Vehicle v(p);
    v.launch({0, 0, 10.0}, 0.0, 28.0);  // ~100 km/h into angled wall
    double max_y = 0.0, max_ang = 0.0;
    for (int i = 0; i < 120; ++i) {
      v.step({0, 0, 0, false}, arena);
      max_y = std::max(max_y, v.position().y);
      const auto a = v.angular_velocity();
      max_ang = std::max(max_ang, std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z));
    }
    // Must not launch into the air or spin explosively.
    CHECK(max_y < 1.5);
    CHECK(max_ang < 6.0);
    // After crashing and coming to a low speed, reverse away then drive forward:
    // throttle must NEVER lock out and gearbox must recover cleanly to gear 1.
    for (int i = 0; i < 120 * 2; ++i) v.step({0, 0, 1.0, false}, arena);
    CHECK(v.gear() == -1);
    CHECK(v.speed() < -1.0);
    // Now press moderate throttle (0.4): must cleanly exit reverse to gear 1 and accelerate.
    for (int i = 0; i < 120 * 2; ++i) v.step({-0.6, 0.4, 0, false}, arena);
    CHECK(v.gear() >= 1);
    CHECK(v.engine_throttle() > 0.2);
  }
  // 9) Phase 1 parity: DamageMods, linear wheel steer bypass, and RWD handbrake clutch open.
  {
    const auto p = test_car();
    drive::FlatGround flat_g;
    drive::Vehicle healthy(p), damaged(p);
    healthy.reset({0, 0, 0}, 0.0);
    damaged.reset({0, 0, 0}, 0.0);
    drive::Vehicle::DamageMods dm{};
    dm.engine_torque = 0.45;
    dm.aero_drag = 1.4;
    dm.steer_pull_rad = 0.08;
    damaged.set_damage_mods(dm);
    for (int i = 0; i < 120 * 3; ++i) {
      healthy.step({0, 1.0, 0, false}, flat_g);
      damaged.step({0, 1.0, 0, false}, flat_g);
    }
    CHECK(damaged.speed() < healthy.speed() * 0.85);
    CHECK(std::fabs(damaged.position().x) > 0.5);

    // Racing wheel linear_steer bypasses gamepad speed-narrowing at highway speed.
    drive::Vehicle v_pad(p), v_wheel(p);
    v_pad.launch({0, 0, 0}, 0.0, 45.0);
    v_wheel.launch({0, 0, 0}, 0.0, 45.0);
    drive::Input pad_in{0.5, 0.2, 0.0, false};
    drive::Input whl_in{0.5, 0.2, 0.0, false};
    whl_in.linear_steer = true;
    for (int i = 0; i < 60; ++i) {
      v_pad.step(pad_in, flat_g);
      v_wheel.step(whl_in, flat_g);
    }
    CHECK(std::fabs(v_wheel.wheels()[0].steer) > std::fabs(v_pad.wheels()[0].steer) + 0.02);

    // WheelMapping (Q70): the 900-degree table is linear to lock; the
    // default table maps 0.5 to 0.07 + 0.36 x 0.93 / 0.86 and narrows to 0.35
    // by 30 m/s; the road wheels slew at up to 4 pi rad/s.
    {
      drive::Vehicle v900(p), v_def(p), v_slow(p);
      v900.launch({0, 0, 0}, 0.0, 45.0);
      v_def.launch({0, 0, 0}, 0.0, 45.0);
      drive::Input w900{0.5, 0.0, 0.0, false};
      w900.linear_steer = true;
      drive::Input wdef = w900;
      wdef.wheel_900 = false;
      v900.step(w900, flat_g);
      v_def.step(wdef, flat_g);
      const double first = v900.steer_angle();
      CHECK(first > 0 && first <= 4 * 3.14159265358979 / 120.0 + 1e-6);  // one 120 Hz frame of slew
      for (int i = 0; i < 30; ++i) {
        v900.step(w900, flat_g);
        v_def.step(wdef, flat_g);
      }
      const double mapped = 0.07 + (0.5 - 0.14) * 0.93 / 0.86;
      CHECK(std::fabs(v900.steer_angle() - 0.5 * p.max_steer) < 0.01);
      CHECK(std::fabs(v_def.steer_angle() - 0.35 * mapped * p.max_steer) < 0.01);
      // Standing still, the default table does not narrow.
      for (int i = 0; i < 60; ++i) v_slow.step(wdef, flat_g);
      CHECK(std::fabs(v_slow.steer_angle() - mapped * p.max_steer) < 0.01);
    }

    // Handbrake on RWD disconnects clutch so the engine does not fight locked rear wheels.
    drive::Vehicle v_hb(p);
    v_hb.launch({0, 0, 0}, 0.0, 20.0);
    for (int i = 0; i < 30; ++i) v_hb.step({0, 0.0, 0.0, true}, flat_g);
    CHECK(v_hb.gear_state() == drive::Vehicle::GearState::kOpen && v_hb.clutch() < 0.05);
  }
  // 10) P0-P4: FrictionTorqueMod SI 100x scale (Normal vs Simulation yaw damping) + Assisted lane centering.
  {
    const auto p = test_car();
    drive::FlatGround flat_g;
    drive::Vehicle v_norm(p), v_sim(p);
    drive::VehicleAssists a_norm, a_sim;
    a_norm.steering = 1;  // Normal
    a_sim.steering = 2;   // Simulation
    v_norm.set_assists(a_norm);
    v_sim.set_assists(a_sim);
    // Start sliding sideways at 25 m/s with yaw rate 1.2 rad/s (> 5 deg yaw slip).
    v_norm.launch({0, 0, 0}, 0.35, 25.0);
    v_sim.launch({0, 0, 0}, 0.35, 25.0);
    v_norm.set_velocity({0.0, 0.0, 25.0}, {0.0, 1.2, 0.0});
    v_sim.set_velocity({0.0, 0.0, 25.0}, {0.0, 1.2, 0.0});
    for (int i = 0; i < 20; ++i) {
      v_norm.step({0.0, 0.2, 0.0, false}, flat_g);
      v_sim.step({0.0, 0.2, 0.0, false}, flat_g);
    }
    // FEEL-11: the original's FrictionTorqueMod is no yaw damper. Past the
    // second yaw cosine it scales the tyres' straightening torque by 0.8 and
    // clamps it, so in Normal steering the slide's yaw rate falls off (slightly) less
    // than in Simulation, whose rows leave the torque alone.
    std::printf("FTM slide: yaw rate normal %.4f sim %.4f\n", v_norm.angular_velocity().y, v_sim.angular_velocity().y);
    CHECK(v_norm.angular_velocity().y > v_sim.angular_velocity().y + 1e-3);

    // FEEL-16: lane centring is an opt-in engine helper. With it on, a car
    // right of lane centre (+2.5 m) with zero stick steers gently left in
    // Assisted; Simulation ignores lane guidance; Assisted without the
    // helper (the default, as in the original) ignores it too.
    drive::Vehicle v_ast(p), v_sim2(p), v_ast_plain(p);
    drive::VehicleAssists a_ast;
    a_ast.steering = 0;  // Assisted
    drive::VehicleAssists a_ast_plain = a_ast;
    CHECK(!a_ast_plain.engine_steer_assist);
    a_ast.engine_steer_assist = true;
    drive::VehicleAssists a_sim_helpers = a_sim;
    a_sim_helpers.engine_steer_assist = true;
    v_ast.set_assists(a_ast);
    v_sim2.set_assists(a_sim_helpers);
    v_ast_plain.set_assists(a_ast_plain);
    v_ast.launch({0, 0, 0}, 0.0, 20.0);
    v_sim2.launch({0, 0, 0}, 0.0, 20.0);
    v_ast_plain.launch({0, 0, 0}, 0.0, 20.0);
    drive::Input lane_in{};
    lane_in.throttle = 0.2;
    lane_in.has_assist_lane = true;
    lane_in.assist_lateral_offset_m = 2.5;
    lane_in.assist_heading_error_rad = 0.0;
    for (int i = 0; i < 30; ++i) {
      v_ast.step(lane_in, flat_g);
      v_sim2.step(lane_in, flat_g);
      v_ast_plain.step(lane_in, flat_g);
    }
    CHECK(v_ast.steer_angle() < -0.02);
    CHECK(std::fabs(v_sim2.steer_angle()) < 1e-6);
    CHECK(std::fabs(v_ast_plain.steer_angle()) < 1e-6);
  }
  // 11) P0-K2: PendingBarnRestoration serialization/deserialization round-trip.
  {
    drive::DriveSave ds;
    ds.car = "VW_Corrado_95";
    ds.position = {100.0, 10.0, 200.0};
    ds.heading = 0.25;
    ds.pending_barn_restorations.push_back({1, 1044, 32.5f});
    ds.pending_barn_restorations.push_back({4, 1089, 12.25f});
    const std::string text = drive::save_to_text(ds);
    drive::DriveSave parsed;
    std::string err;
    CHECK(drive::save_from_text(text, parsed, &err));
    CHECK(parsed.pending_barn_restorations.size() == 2);
    CHECK(parsed.pending_barn_restorations[0].barn_id == 1 && parsed.pending_barn_restorations[0].car_id == 1044 &&
          std::fabs(parsed.pending_barn_restorations[0].remaining_sec - 32.5f) < 1e-3f);
    CHECK(parsed.pending_barn_restorations[1].barn_id == 4 && parsed.pending_barn_restorations[1].car_id == 1089 &&
          std::fabs(parsed.pending_barn_restorations[1].remaining_sec - 12.25f) < 1e-3f);
  }
  // 12) R2-3A: FastTravelAnywhere / CoDriver / SavedCarHistory round-trip, atomic_write_file,
  //     and corrupt-save automatic backup recovery (load_save_with_recovery).
  {
    namespace fs = std::filesystem;
    drive::DriveSave ds;
    ds.car = "NIS_SkylineGTRVSpecII_02";
    ds.position = {10.0, 20.0, 30.0};
    ds.heading = 0.5;
    ds.fast_travel_anywhere = true;
    ds.codriver_enabled = false;
    ds.car_history.push_back({7, 445, 12345.5, 678.25, 45000, 1200, 9, 6, 8});
    drive::DriveSave rt;
    std::string err;
    CHECK(drive::save_from_text(drive::save_to_text(ds), rt, &err));
    CHECK(rt.fast_travel_anywhere && !rt.codriver_enabled);
    CHECK(rt.car_history.size() == 1 && rt.car_history[0].garage_id == 7 && rt.car_history[0].car_id == 445 &&
          std::fabs(rt.car_history[0].distance_driven - 12345.5) < 1e-3 &&
          std::fabs(rt.car_history[0].time_driven - 678.25) < 1e-3 && rt.car_history[0].total_winnings == 45000 &&
          rt.car_history[0].total_repairs == 1200 && rt.car_history[0].num_races == 9 &&
          rt.car_history[0].num_victories == 6 && rt.car_history[0].num_podiums == 8);

    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "shift_drive_atomic_recovery_test";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // Atomic write creates target, overwrites cleanly, and leaves no stray .tmp file.
    const fs::path target = root / "nested" / "atomic.txt";
    CHECK(drive::atomic_write_file(target, "first-version", &err));
    CHECK(fs::exists(target) && !fs::exists(fs::path(target).concat(".tmp")));
    CHECK(drive::atomic_write_file(target, "second-version-longer", &err));
    CHECK(fs::exists(target) && !fs::exists(fs::path(target).concat(".tmp")));
    {
      std::ifstream in(target, std::ios::binary);
      const std::string got((std::istreambuf_iterator<char>(in)), {});
      CHECK(got == "second-version-longer");
    }

    // Corrupt-save recovery: primary corrupt, newest snapshot corrupt, second-newest valid.
    const fs::path save_path = root / "drive" / "save.txt";
    const fs::path backups_dir = root / "backups" / "saves";
    auto make_save = [](const std::string& car, int64_t cr) {
      drive::DriveSave s;
      s.car = car;
      s.position = {1.0, 2.0, 3.0};
      s.heading = 0.25;
      s.credits = cr;
      return s;
    };
    // Older valid snapshot (t=1700000100, 11000 cr)
    const fs::path snap1 = backups_dir / "20261124-010000-start";
    CHECK(drive::write_save(snap1 / "save.txt", make_save("VW_Corrado_95", 11000), &err));
    CHECK(drive::atomic_write_file(snap1 / "backup.txt", "shift-save-backup 1\nreason start\ntime 1700000100\n", &err));
    // Newer valid snapshot (t=1700000200, 22000 cr)
    const fs::path snap2 = backups_dir / "20261124-020000-autosave";
    CHECK(drive::write_save(snap2 / "save.txt", make_save("NIS_SkylineGTRVSpecII_02", 22000), &err));
    CHECK(drive::atomic_write_file(snap2 / "backup.txt", "shift-save-backup 1\nreason autosave\ntime 1700000200\n", &err));
    // Newest snapshot (t=1700000300) is corrupt/truncated and must be skipped!
    const fs::path snap3 = backups_dir / "20261124-030000-exit";
    CHECK(drive::atomic_write_file(snap3 / "save.txt", "TRUNCATED_CORRUPT_BACKUP", &err));
    CHECK(drive::atomic_write_file(snap3 / "backup.txt", "shift-save-backup 1\nreason exit\ntime 1700000300\n", &err));

    // Corrupt primary save.txt.
    const std::string corrupt_payload = "CORRUPTED_PRIMARY_HALF_WRITTEN";
    CHECK(drive::atomic_write_file(save_path, corrupt_payload, &err));

    drive::DriveSave recovered;
    fs::path from;
    CHECK(drive::load_save_with_recovery(save_path, backups_dir, recovered, &from, &err));
    CHECK(recovered.credits == 22000 && recovered.car == "NIS_SkylineGTRVSpecII_02");
    CHECK(from == snap2 / "save.txt");
    // Corrupt primary was preserved as save.txt.corrupt for inspection.
    const fs::path corrupt_copy = fs::path(save_path).concat(".corrupt");
    CHECK(fs::exists(corrupt_copy));
    {
      std::ifstream in(corrupt_copy, std::ios::binary);
      const std::string kept((std::istreambuf_iterator<char>(in)), {});
      CHECK(kept == corrupt_payload);
    }
    // Primary save.txt is now restored and loads directly without recovery!
    drive::DriveSave reloaded;
    fs::path from_again;
    CHECK(drive::load_save_with_recovery(save_path, backups_dir, reloaded, &from_again, &err));
    CHECK(reloaded.credits == 22000 && from_again.empty());
    CHECK(!fs::exists(fs::path(save_path).concat(".tmp")));

    // Empty 0-byte primary with sibling .bak fallback.
    const fs::path bak_save = root / "bak_only" / "save.txt";
    CHECK(drive::atomic_write_file(bak_save, "", &err));
    CHECK(drive::write_save(fs::path(bak_save).concat(".bak"), make_save("AUD_SportQuattro_83", 33000), &err));
    drive::DriveSave bak_rec;
    fs::path bak_from;
    CHECK(drive::load_save_with_recovery(bak_save, {}, bak_rec, &bak_from, &err));
    CHECK(bak_rec.credits == 33000 && bak_rec.car == "AUD_SportQuattro_83" &&
          bak_from == fs::path(bak_save).concat(".bak"));
    CHECK(fs::exists(fs::path(bak_save).concat(".corrupt")));

    // Missing primary with no backups -> false with empty error.
    const fs::path missing_save = root / "empty" / "save.txt";
    drive::DriveSave none_rec;
    fs::path none_from;
    CHECK(!drive::load_save_with_recovery(missing_save, root / "no_backups", none_rec, &none_from, &err) && err.empty());

    fs::remove_all(root, ec);
  }
  // 12) PT-27 (no vertical catapult on steep banks/curbs while preserving ramp climbs: now the
  //     body's sphere contacts, which push out by position only, Q101),
  //     PT-30 (retail FH1 Medium default assists), and PT-A (O(log N) RacingLine::segment).
  {
    const auto p = test_car();
    const float h = 200.0f;

    // PT-27a: High-speed impact into a steep (~63 deg, normal.y ~ 0.45 < 0.65) roadside bank
    // must deflect horizontally without catapulting the car into the air.
    {
      drive::MeshGround bank({
          {-h, 0, -h}, {h, 0, h}, {h, 0, -h}, {-h, 0, -h}, {-h, 0, h}, {h, 0, h},
          {-30, 0, 20}, {30, 3.0f, 21.5f}, {30, 0, 20},
          {-30, 0, 20}, {-30, 3.0f, 21.5f}, {30, 3.0f, 21.5f},
      }, {}, {0, 0, 0, 0});
      bank.set_surface_tyres({hard_world()});
      drive::Vehicle v(p);
      v.launch({0, 0, 8.0}, 0.12, 38.0);  // ~137 km/h angled into steep bank
      double max_y = 0.0, max_vy = 0.0;
      for (int i = 0; i < 150; ++i) {
        v.step({0, 0.2, 0, false}, bank);
        max_y = std::max(max_y, v.position().y);
        max_vy = std::max(max_vy, v.velocity().y);
      }
      std::printf("pt27a bank: max_y=%.3f m, max_vy=%.3f m/s\n", max_y, max_vy);
      CHECK(max_y < 1.0);
      CHECK(max_vy < 3.2);
    }

    // PT-27b, now the original's wheel query (Q101): a vertical step.
    // - The step's face does not carry a wheel (normal . up < 0.17).
    // - A 0.35 m step is within the wheel's reach (the tyre sphere at the
    //   top of its sweep is at most 0.3 m into it), so the car rides up on
    //   its bump stops. Their damping acts on the contact's closing speed,
    //   so the step itself adds no damping spike: the launch is the bump
    //   stop's spring alone.
    // - A 0.7 m step is out of reach: the wheels stay on the road and the
    //   body's spheres meet the face, with no launch.
    {
      auto step_ground = [&](float top) {
        drive::MeshGround g({
            {-h, 0, -h}, {h, 0, 12}, {h, 0, -h}, {-h, 0, -h}, {-h, 0, 12}, {h, 0, 12},
            {-30, 0, 12}, {30, top, 12}, {30, 0, 12},
            {-30, 0, 12}, {-30, top, 12}, {30, top, 12},
            {-h, top, 12}, {h, top, h}, {h, top, 12},
            {-h, top, 12}, {-h, top, h}, {h, top, h},
        }, {}, {0, 0, 0, 0, 0, 0});
        g.set_surface_tyres({hard_world()});
        return g;
      };
      auto run = [&](const drive::MeshGround& g, const drive::CarParams& cp, double& max_y, double& max_vy) {
        drive::Vehicle v(cp);
        v.launch({0, 0, 4.0}, 0.0, 25.0);
        max_y = 0.0, max_vy = 0.0;
        for (int i = 0; i < 120; ++i) {
          v.step({0, 0.4, 0, false}, g);
          max_y = std::max(max_y, v.position().y);
          max_vy = std::max(max_vy, v.velocity().y);
        }
      };
      const drive::MeshGround curb = step_ground(0.35f);
      double max_y = 0, max_vy = 0, undamped_y = 0, undamped_vy = 0;
      run(curb, p, max_y, max_vy);
      drive::CarParams no_damp = p;
      no_damp.bumpstop_damping = {0, 0};
      run(curb, no_damp, undamped_y, undamped_vy);
      std::printf("pt27b curb 0.35 m: max_y=%.3f m, max_vy=%.3f m/s (bump-stop damping off: %.3f m, %.3f m/s)\n", max_y,
                  max_vy, undamped_y, undamped_vy);
      CHECK(max_y > 0.35);                    // it rides up the step
      CHECK(max_vy < undamped_vy * 1.05);     // no damping spike from the step
      CHECK(max_y < 3.0);
      const drive::MeshGround wall = step_ground(0.7f);
      double wall_y = 0, wall_vy = 0;
      run(wall, p, wall_y, wall_vy);
      std::printf("pt27b step 0.7 m: max_y=%.3f m, max_vy=%.3f m/s\n", wall_y, wall_vy);
      CHECK(wall_y < 0.3);
      CHECK(wall_vy < 1.0);
    }

    // PT-27c: Natural 18-degree ramp (normal.y ~ 0.95 >= 0.65) still climbs and launches cleanly.
    {
      drive::MeshGround ramp({
          {-h, 0, -h}, {h, 0, 10}, {h, 0, -h}, {-h, 0, -h}, {-h, 0, 10}, {h, 0, 10},
          {-20, 0, 10}, {20, 3.8f, 22}, {20, 0, 10},
          {-20, 0, 10}, {-20, 3.8f, 22}, {20, 3.8f, 22},
      }, {}, {0, 0, 0, 0});
      ramp.set_surface_tyres({hard_world()});
      drive::Vehicle v(p);
      v.launch({0, 0, 2.0}, 0.0, 28.0);
      double max_y = 0.0, max_vy = 0.0;
      for (int i = 0; i < 100; ++i) {
        v.step({0, 0.8, 0, false}, ramp);
        max_y = std::max(max_y, v.position().y);
        max_vy = std::max(max_vy, v.velocity().y);
      }
      CHECK(max_y > 2.5);
      CHECK(max_vy > 3.0);
    }

    // Q100/Q101 regressions: the body's spheres against a fence and a wall, with
    // surfaceTypes.xml's Car/HardWorld row (Elasticity -0.9, friction 0.1 -> 0.5).
    {
      const drive::SurfaceTyre hard = hard_world();
      auto wall_ground = [&](float z, float top) {
        drive::MeshGround g({
                                {-h, 0, -h}, {h, 0, h}, {h, 0, -h}, {-h, 0, -h}, {-h, 0, h}, {h, 0, h},
                                {-100, 0, z}, {100, top, z}, {100, 0, z},
                                {-100, 0, z}, {-100, top, z}, {100, top, z},
                            },
                            {}, {0, 0, 1, 1});
        g.set_surface_tyres({drive::SurfaceTyre{}, hard});
        return g;
      };
      // A low-speed nudge into a 1.1 m fence (one face, as the .fiz fences are):
      // no launch, no pass-through, one impact.
      {
        const drive::MeshGround fence = wall_ground(10.0f, 1.1f);
        drive::Vehicle v(p);
        v.launch({0, 0, 0.0}, 0.0, 7.0);  // 25 km/h
        const uint64_t n0 = v.last_body_impact().count;
        double max_y = 0.0, max_vy = 0.0, max_z = -1e9, first = -1;
        for (int i = 0; i < 240; ++i) {
          v.step({0, 0.3, 0, false}, fence);
          if (first < 0 && v.last_body_impact().count > n0) first = v.last_body_impact().speed;
          max_y = std::max(max_y, v.position().y);
          max_vy = std::max(max_vy, v.velocity().y);
          max_z = std::max(max_z, v.position().z);
        }
        const double front = p.centre_of_mass.z + 0.5 * p.size.z;
        std::printf("fence 25 km/h: max_y %.3f m (start %.3f), max_vy %.3f m/s, nose at most %.3f m past the fence, "
                    "impacts %llu (closing %.2f m/s), speed after %.2f m/s\n",
                    max_y, 0.0, max_vy, max_z + front - 10.0,
                    (unsigned long long)(v.last_body_impact().count - n0), first, v.speed());
        CHECK(max_vy < 0.5);
        CHECK(max_y < 0.15);
        CHECK(max_z + front < 10.0 + 0.25);
        CHECK(v.last_body_impact().count - n0 >= 1);
        CHECK(first > 5.0 && first < 10.0);
      }
      // A 150 km/h head-on into a 3 m wall: stopped, no launch, no flip, little
      // rebound (the original's -0.9 Elasticity takes a tenth of the closing
      // speed per contact step; the push-out takes the rest).
      {
        const drive::MeshGround wall = wall_ground(40.0f, 3.0f);
        drive::Vehicle v(p);
        v.launch({0, 0, 0.0}, 0.0, 41.7);
        double max_y = 0.0, max_vy = 0.0, max_z = -1e9, min_vz = 1e9, min_up = 1.0, first = -1;
        const uint64_t n0 = v.last_body_impact().count;
        for (int i = 0; i < 360; ++i) {
          v.step({0, 0, 0, false}, wall);
          if (first < 0 && v.last_body_impact().count > n0) first = v.last_body_impact().speed;
          max_y = std::max(max_y, v.position().y);
          max_vy = std::max(max_vy, v.velocity().y);
          max_z = std::max(max_z, v.position().z);
          min_vz = std::min(min_vz, v.velocity().z);
          min_up = std::min(min_up, v.up_y());
        }
        const double front = p.centre_of_mass.z + 0.5 * p.size.z;
        std::printf("wall 150 km/h: max_y %.3f m, max_vy %.3f m/s, nose at most %.3f m past the wall, rebound %.2f m/s, "
                    "min up.y %.3f, closing %.2f m/s\n",
                    max_y, max_vy, max_z + front - 40.0, -min_vz, min_up, first);
        // 1.1 m/s with the old bump-stop caps; the uncapped bump stop
        // (Q101) lifts the nose a little more as the front dives.
        CHECK(max_vy < 2.0);
        CHECK(max_y < 0.5);
        CHECK(max_z + front < 40.0 + 0.5);
        CHECK(-min_vz < 5.0);
        CHECK(min_up > 0.8);
        CHECK(first > 35.0);
      }
      // A 20-degree glance at 108 km/h: deflected along the wall, most of the
      // speed kept, no launch.
      {
        const drive::MeshGround wall = wall_ground(30.0f, 3.0f);
        drive::Vehicle v(p);
        v.launch({0, 0, 24.0}, 1.22, 30.0);
        double max_vy = 0.0, max_z = -1e9;
        for (int i = 0; i < 240; ++i) {
          v.step({0, 0.3, 0, false}, wall);
          max_vy = std::max(max_vy, v.velocity().y);
          max_z = std::max(max_z, v.position().z);
        }
        std::printf("wall glance 108 km/h: max_vy %.3f m/s, speed after %.2f m/s, vz %.2f\n", max_vy, v.speed(),
                    v.velocity().z);
        CHECK(max_vy < 1.0);
        CHECK(v.speed() > 15.0);
        CHECK(max_z < 30.0);
      }
    }

    // Q102: the damage difficulties on List_DamageModDef. Cosmetic: no part
    // damage; Limited and Simulation: the rows' ranges, Limited at half;
    // tyre wear and fuel in Simulation only.
    {
      auto crash = [](drive::DamageMode mode, float mps, int hits) {
        drive::CarDamageState d;
        d.mode = mode;
        for (int i = 0; i < hits; ++i) d.record_impact(0.0f, 0.5f, 1.9f, mps);
        return d;
      };
      const auto cos = crash(drive::DamageMode::kCosmetic, 40.0f, 20);
      CHECK(cos.zone_front > 0.5f && cos.engine_damage == 0.0f && cos.multipliers().engine_torque == 1.0);
      // Below every row's DamageMinImpact (5 mph): nothing.
      const auto tap = crash(drive::DamageMode::kSimulation, 2.0f, 20);
      for (float pd : tap.part_damage) CHECK(pd == 0.0f);
      // 134 mph hits: every part past its DamageMaxImpact (with RandomImpact), chance 1.
      const auto sim = crash(drive::DamageMode::kSimulation, 60.0f, 1);
      const auto lim = crash(drive::DamageMode::kLimited, 60.0f, 1);
      CHECK(std::fabs(sim.part_damage[drive::kDamageEngine] - 1.0f) < 1e-6f);
      CHECK(std::fabs(lim.part_damage[drive::kDamageEngine] - 0.5f) < 1e-6f);
      CHECK(sim.part_damage[drive::kDamageTires] == 0.0f);  // DamageMax 0
      const auto ms = sim.multipliers();
      CHECK(std::fabs(ms.engine_torque - 0.05) < 1e-9);        // Engine ModFullDamage
      CHECK(std::fabs(ms.shift_time_scale - 8.0) < 1e-9);      // GearBox
      CHECK(std::fabs(ms.aero_drag - 2.0) < 1e-9);             // BodyDrag
      CHECK(std::fabs(ms.aero_downforce_front - 0.1) < 1e-9);  // FrontDownforce
      CHECK(std::fabs(ms.brake_power - 0.1) < 1e-9);           // Brakes
      CHECK(std::fabs(ms.suspension_scale[0] - 0.2) < 1e-9);   // Suspension
      const auto ml = lim.multipliers();
      CHECK(std::fabs(ml.engine_torque - 0.525) < 1e-9);
      // The sputter rumble and the audio read engine_damage: the Engine part.
      CHECK(sim.engine_damage == 1.0f && std::fabs(lim.engine_damage - 0.5f) < 1e-6f);
      const std::array<double, 4> slip{0.5, 0.5, 0.5, 0.5};
      auto drive_for = [&](drive::DamageMode mode) {
        drive::CarDamageState d;
        d.mode = mode;
        for (int i = 0; i < 600; ++i) d.step(1.0 / 60, 30.0, false, 1.0, 0.8, slip);
        return d;
      };
      CHECK(drive_for(drive::DamageMode::kSimulation).fuel_fraction < 1.0f);
      CHECK(drive_for(drive::DamageMode::kLimited).fuel_fraction == 1.0f);
      CHECK(drive_for(drive::DamageMode::kLimited).tyre_wear[0] == 0.0f);
    }

    // PT-30: Retail FH1 Medium default driving assists on fresh/new campaign profiles.
    {
      const drive::AssistSettings def{};
      const drive::AssistSettings retail = drive::AssistSettings::retail_defaults();
      CHECK(def.braking == 1 && retail.braking == 1);          // ABS ON
      CHECK(def.steering == 1 && retail.steering == 1);        // Normal
      CHECK(def.traction_control && retail.traction_control);  // TCS ON
      CHECK(!def.stability_control && !retail.stability_control);  // STM OFF (FEEL-12)
      CHECK(def.shifting == 0 && retail.shifting == 0);        // Automatic
      CHECK(def.driving_line == 2 && retail.driving_line == 2);  // Full
      CHECK(def.rewind && retail.rewind);                      // Rewind ON
    }

    // Bonus PT-A: O(log N) RacingLine::segment() matches exact linear scan across
    // below-start, knots, midpoints, duplicate distances, and past-end, with any hint.
    {
      drive::RacingLine line;
      constexpr size_t kPts = 2000;
      line.x.resize(kPts);
      line.z.resize(kPts);
      line.dist.resize(kPts);
      line.speed.resize(kPts, 30.0f);
      float acc = 0.0f;
      for (size_t i = 0; i < kPts; ++i) {
        if (i > 0 && i != 500) acc += 2.0f + float(i % 7) * 0.5f;  // duplicate knot at i==500
        line.x[i] = 0.0f;
        line.z[i] = acc;
        line.dist[i] = acc;
      }
      auto ref_segment = [&](double d) -> size_t {
        size_t s = 0;
        while (s + 2 < line.size() && double(line.dist[s + 1]) <= d) ++s;
        return s;
      };
      const double probes[] = {
          -10.0,
          0.0,
          1.25,
          double(line.dist[1]),
          double(line.dist[499]) + 0.1,
          double(line.dist[500]),  // duplicate knot
          double(line.dist[1234]) + 0.75,
          double(line.dist[kPts - 2]),
          double(line.dist[kPts - 1]),
          double(line.dist[kPts - 1]) + 100.0,
      };
      size_t sequential_hint = 0;
      for (double d : probes) {
        const size_t want = ref_segment(d);
        CHECK(line.segment(d, 0) == want);
        CHECK(line.segment(d, sequential_hint) == want);
        CHECK(line.segment(d, kPts - 2) == want);
        sequential_hint = want;
      }
    }
  }
  // 13) Round 3 Phase R3-B parity tests (R3-PHY-01, 06, 07, 08, 16, 18):
  {
    const auto p = test_car();
    const drive::FlatGround flat;
    constexpr double kPi = 3.14159265358979323846;

    // R3-PHY-01: Vehicle::blend smooth long-range cubic Hermite + slerp scrub over > 5 m
    // (2.5 s cornering arc ~ 70 m chord) without teleporting, while snapping past 250 m.
    {
      drive::Vehicle::Pose pa{}, pb{};
      const double R = 50.0;
      const double speed_arc = (0.5 * kPi * R) / 2.5;  // ~31.4 m/s around 90 deg arc in 2.5 s
      pa.pos = {0.0, 0.5, 0.0};
      pa.vel = {0.0, 0.0, speed_arc};  // heading 0 (+Z)
      pa.rot = {0.0, 0.0, 0.0, 1.0};
      pb.pos = {R, 0.5, R};            // 90 deg right turn (chord = 70.7 m > old 5 m guard!)
      pb.vel = {speed_arc, 0.0, 0.0};  // heading +pi/2 (+X)
      pb.rot = {0.0, std::sin(kPi / 4.0), 0.0, std::cos(kPi / 4.0)};

      const auto mid = drive::Vehicle::blend(pa, pb, 0.5);
      // Must NOT snap to pb!
      CHECK(mid.pos.x < pb.pos.x - 10.0 && mid.pos.z < pb.pos.z - 5.0);
      // Hermite follows outer corner curve: at t=0.5 on right turn from (0,0) facing +Z to (R,R) facing +X,
      // linear midpoint is (25, 25), true arc midpoint is (14.64, 35.36) -> Hermite bends toward (x < 22, z > 28)!
      CHECK(mid.pos.x < 22.0 && mid.pos.z > 28.0);
      // Slerp midpoint heading is 45 deg (pi/4).
      const auto mid_fwd = drive::rotate(mid.rot, {0, 0, 1});
      const double mid_hdg = std::atan2(mid_fwd.x, mid_fwd.z);
      CHECK(std::fabs(mid_hdg - kPi / 4.0) < 0.02);

      // Jump past 250 m (> 62500 m^2) still snaps cleanly to b.
      drive::Vehicle::Pose far_b = pb;
      far_b.pos = {300.0, 0.5, 0.0};
      const auto snapped = drive::Vehicle::blend(pa, far_b, 0.5);
      CHECK(std::fabs(snapped.pos.x - far_b.pos.x) < 1e-6);
    }

    // R3-PHY-08: the rev limiter (Q341) cuts the throttle above halfway from
    // the redline to TorqueCurveMaxRPM and holds it for 0.04 s, so a free rev
    // bounces under that speed and never reaches the curve's end.
    {
      drive::Vehicle v(p);
      drive::VehicleAssists a;
      a.shifting = 2;  // Manual + Clutch so we can free-rev with clutch held
      v.set_assists(a);
      int cut_transitions = 0;
      bool prev_cut = false;
      double min_post_cut_rpm = 1e9, max_rpm = 0.0;
      bool hit_redline = false;
      for (int i = 0; i < 120 * 2; ++i) {
        v.step({0.0, 1.0, 0.0, false, 1.0, 0}, flat);
        const double r = v.rpm();
        max_rpm = std::max(max_rpm, r);
        if (v.rev_limiter_active()) hit_redline = true;
        if (hit_redline) min_post_cut_rpm = std::min(min_post_cut_rpm, r);
        if (v.rev_limiter_active() != prev_cut) {
          ++cut_transitions;
          prev_cut = v.rev_limiter_active();
        }
      }
      CHECK(hit_redline);
      CHECK(cut_transitions >= 6);  // rhythmic on/off bounces, not a flat pin
      CHECK(min_post_cut_rpm < v.rev_cut_rpm() - 60.0);
      CHECK(max_rpm > v.rev_cut_rpm());
      CHECK(max_rpm < p.torque_curve_max_rpm);
    }

    // R3-PHY-07: Keyboard digital steering progressive 0-120 ms tap ramp, highway cap & crisp center return.
    {
      drive::Vehicle v_kb(p), v_pad(p);
      v_kb.launch({0, 0, 0}, 0.0, 14.0);
      v_pad.launch({0, 0, 0}, 0.0, 14.0);
      drive::Input kb_in{1.0, 0.2, 0.0, false};
      kb_in.input_is_keyboard = true;
      drive::Input pad_in{1.0, 0.2, 0.0, false};
      pad_in.input_is_keyboard = false;
      // Short 25 ms tap (3 steps): keyboard tap ramp is much gentler than instant full-stick pad input.
      for (int i = 0; i < 3; ++i) {
        v_kb.step(kb_in, flat);
        v_pad.step(pad_in, flat);
      }
      const double tap_early = v_kb.steer_angle();
      CHECK(tap_early > 0.0 && tap_early < v_pad.steer_angle() * 0.75);
      // Sustained hold past 120 ms (18 more steps = 175 ms total): ramps up substantially.
      for (int i = 0; i < 18; ++i) v_kb.step(kb_in, flat);
      const double tap_held = v_kb.steer_angle();
      CHECK(tap_held > tap_early * 3.0);
      // Release key: crisp return toward center within 150 ms.
      kb_in.steer = 0.0;
      for (int i = 0; i < 18; ++i) v_kb.step(kb_in, flat);
      CHECK(std::fabs(v_kb.steer_angle()) < tap_held * 0.35);

      // Highway speed cap: at 55 m/s sustained keyboard steer is capped lower than at 10 m/s.
      drive::Vehicle v_hi(p), v_lo(p);
      v_hi.launch({0, 0, 0}, 0.0, 55.0);
      v_lo.launch({0, 0, 0}, 0.0, 10.0);
      kb_in.steer = 1.0;
      for (int i = 0; i < 30; ++i) {
        v_hi.step(kb_in, flat);
        v_lo.step(kb_in, flat);
      }
      CHECK(v_hi.steer_angle() < v_lo.steer_angle() * 0.5);
    }

    // R3-PHY-06 / FEEL-16: the countersteer bias is an opt-in engine helper.
    // With it on, Normal countersteers into the slide, Simulation stays raw
    // and the handbrake blends it out; Normal without it (the default, as in
    // the original, whose countersteer is only a faster steering rate) does
    // not steer by itself.
    {
      drive::Vehicle v_norm(p), v_sim(p), v_hb(p), v_plain(p);
      drive::VehicleAssists a_norm, a_sim;
      a_norm.steering = 1;  // Normal
      a_sim.steering = 2;   // Simulation
      const drive::VehicleAssists a_plain = a_norm;
      a_norm.engine_steer_assist = true;
      a_sim.engine_steer_assist = true;
      v_norm.set_assists(a_norm);
      v_sim.set_assists(a_sim);
      v_hb.set_assists(a_norm);
      v_plain.set_assists(a_plain);
      // Oversteer slide: nose pointed +0.30 rad (~17 deg) right of velocity at 22 m/s -> rear slip < 0 -> countersteer left (< 0).
      v_norm.launch({0, 0, 0}, 0.30, 22.0);
      v_sim.launch({0, 0, 0}, 0.30, 22.0);
      v_hb.launch({0, 0, 0}, 0.30, 22.0);
      v_plain.launch({0, 0, 0}, 0.30, 22.0);
      v_norm.set_velocity({0.0, 0.0, 22.0}, {0.0, 0.4, 0.0});
      v_sim.set_velocity({0.0, 0.0, 22.0}, {0.0, 0.4, 0.0});
      v_hb.set_velocity({0.0, 0.0, 22.0}, {0.0, 0.4, 0.0});
      v_plain.set_velocity({0.0, 0.0, 22.0}, {0.0, 0.4, 0.0});
      for (int i = 0; i < 25; ++i) {
        v_norm.step({0.0, 0.2, 0.0, false}, flat);
        v_sim.step({0.0, 0.2, 0.0, false}, flat);
        v_hb.step({0.0, 0.2, 0.0, true}, flat);
        v_plain.step({0.0, 0.2, 0.0, false}, flat);
      }
      CHECK(v_norm.steer_angle() < -0.08);                // Normal actively countersteers left into slide
      CHECK(std::fabs(v_sim.steer_angle()) < 1e-6);       // Simulation stays completely raw
      CHECK(std::fabs(v_plain.steer_angle()) < 1e-6);     // no helper: the stick alone steers
      CHECK(std::fabs(v_hb.steer_angle()) < std::fabs(v_norm.steer_angle()) * 0.35);  // handbrake blends it out
    }

    // R3-PHY-18: Clutch-grind damage accumulation + capacity scaling & per-axle brake effectiveness degradation.
    {
      drive::Vehicle v_grind(p);
      drive::VehicleAssists a_mc;
      a_mc.shifting = 2;  // Manual + Clutch
      v_grind.set_assists(a_mc);
      v_grind.launch({0, 0, 0}, 0.0, 20.0);
      CHECK(v_grind.clutch_damage() == 0.0);
      // Clean clutched shift: zero clutch damage.
      v_grind.step({0.0, 0.5, 0.0, false, 1.0, +1}, flat);
      CHECK(v_grind.clutch_damage() == 0.0);
      // Grind without clutch: accumulates clutch_damage.
      v_grind.step({0.0, 0.5, 0.0, false, 0.0, -1}, flat);
      CHECK(v_grind.clutch_damage() > 0.0);

      // CarDamageState Simulation mode: clutch grind reduces clutch_capacity.
      drive::CarDamageState dmg;
      dmg.mode = drive::DamageMode::kSimulation;
      dmg.record_clutch_grind(0.25f);
      dmg.record_clutch_grind(0.25f);
      const auto mul = dmg.multipliers();
      CHECK(mul.clutch_capacity < 0.80);

      // Per-axle brake degradation in Vehicle::DamageMods slows front-damaged car less than healthy car under braking.
      drive::Vehicle v_brk_ok(p), v_brk_dmg(p);
      drive::Vehicle::DamageMods dm_brk{};
      dm_brk.brake_power_axle = {0.40, 1.0};  // severely faded front brakes, intact rear
      v_brk_dmg.set_damage_mods(dm_brk);
      v_brk_ok.launch({0, 0, 0}, 0.0, 35.0);
      v_brk_dmg.launch({0, 0, 0}, 0.0, 35.0);
      for (int i = 0; i < 120; ++i) {
        v_brk_ok.step({0.0, 0.0, 0.6, false}, flat);
        v_brk_dmg.step({0.0, 0.0, 0.6, false}, flat);
      }
      CHECK(v_brk_dmg.speed() > v_brk_ok.speed() + 1.0);
    }
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("drive_tests: all passed\n");
  return 0;
}
