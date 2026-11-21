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
  drive::DriveSave back;
  std::string error;
  CHECK(drive::save_from_text(drive::save_to_text(s), back, &error));
  CHECK(back.car == s.car && back.has_spawn);
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
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("drive_tests: all passed\n");
  return 0;
}
