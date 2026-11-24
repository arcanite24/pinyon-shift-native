// Tests for libs/drive on a synthetic car on flat ground (no game data).
// The physics is the engine's placeholder model; these check that it is
// sound (rest pose, determinism, directions), not that it matches the game.
// Spec: FORMAT-CARS (Wheels: the wheel stands on y = 0 at its outer
// radius; the body frame is +X right, +Z forward), REIMPLEMENTATION_PLAN M3.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "drive/from_bake.h"
#include "drive/mesh_ground.h"
#include "drive/ai_driver.h"
#include "drive/contact.h"
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

  drive::LineFollower follower(line, {});
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
    field.place(0, 5.0, 0.0f, 0.95f);
    double x = 0, z = 0, h = 0;
    field.start_pose(0, x, z, h);
    car.reset({x, 0, z}, h);
    float cap = 1.0f;
    std::vector<drive::AiCarState> st(1);
    std::vector<drive::Input> ins(1);
    double max_abs_steer = 0.0;
    for (int s = 0; s < int(20 / drive::Vehicle::kStep); ++s) {
      st[0] = drive::ai_state(car);
      field.drive(st, ins);
      drive::ai_traction(car, cap, drive::Vehicle::kStep, ins[0], !use_slip);
      max_abs_steer = std::max(max_abs_steer, std::fabs(ins[0].steer));
      car.step(ins[0], ground);
    }
    CHECK(std::isfinite(car.position().x) && std::isfinite(car.position().z));
    CHECK(field.progress(0) > 150.0);
    CHECK(max_abs_steer > 0.02);
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
    // At rest, Assisted caps to SteerMaxAngleFiltered while Simulation uses SteerMaxAngle;
    // at mid speed (18 m/s), Simulation allows wider steering lock than Assisted.
    CHECK(v_sim.steer_lock() > v_ast.steer_lock());
    v_ast.launch({0, 0, 0}, 0, 18.0);
    v_sim.launch({0, 0, 0}, 0, 18.0);
    CHECK(v_sim.steer_lock() > v_ast.steer_lock());

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
  test_model_to_world();
  test_oracle_lap();
  test_ai_field();
  test_ai_slip_rule();
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

    drive::Hit h_wall{};
    h_wall.tyre.off_roadness = 0.0;
    const drive::Vec3 wall_n{1.0, 0.0, 0.0};
    CHECK(drive::body_contact_friction(h_wall, wall_n, 0.30) <= 0.12 + 1e-9);
    CHECK(drive::body_contact_damping_scale(h_wall, wall_n) < 1.0);

    drive::Hit h_dirt{};
    h_dirt.tyre.off_roadness = 0.9;
    CHECK(drive::body_contact_friction(h_dirt, wall_n, 0.30) >= 0.45 - 1e-9);
    CHECK(drive::body_contact_damping_scale(h_dirt, wall_n) > 1.0);
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

    // Handbrake on RWD disconnects clutch so the engine does not fight locked rear wheels.
    drive::Vehicle v_hb(p);
    v_hb.launch({0, 0, 0}, 0.0, 20.0);
    for (int i = 0; i < 30; ++i) v_hb.step({0, 0.0, 0.0, true}, flat_g);
    CHECK(v_hb.gear_state() == drive::Vehicle::GearState::kOpen && v_hb.clutch() < 0.05);
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("drive_tests: all passed\n");
  return 0;
}
