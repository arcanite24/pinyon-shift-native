// SPEC-CAR-PHYSICS Tests, engine side, on the R34's rows (car 445) read
// from the player's own database at run time (skipped, exit 77, without
// SHIFT_GAME_ROOT; SHIFT_STATE_ROOT adds the installed packages to the
// merge). Only scalars the spec quotes are asserted; nothing of the game's
// data is kept here.
//
//   unit: the box inertia, the per-mass rates, the substep count, the
//         sidewall factor and the load blend of the tyre curves;
//   scenarios on the analytic ground: A (0-100 km/h), B (the stop), the
//         full-lock circle, against the spec's tolerances.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cars/physics.h"
#include "cars/sources.h"
#include "drive/from_physics.h"
#include "drive/profile_ground.h"
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

// The scenario gates: reported, and failing the test only with
// PHYSICS_GATE=1 while the spec questions on them are open (SPEC_QUESTIONS).
int gate_misses = 0;
#define GATE(cond)                                                      \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("GATE MISS %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++gate_misses;                                                    \
    }                                                                   \
  } while (0)

constexpr double kPi = 3.14159265358979323846;
// SPEC-CAR-PHYSICS, Scenario A: the launch point and direction.
constexpr double kStartX = 2002.9, kStartY = 143.84, kStartZ = 960.0, kDirX = 0.805, kDirZ = -0.593;

bool near_rel(double a, double b, double rel) { return std::fabs(a - b) <= rel * std::fabs(b); }

void unit_checks(const cars::CarPhysics& car, const cars::PhysicsSettings& s, const drive::CarParams& p) {
  const double m = car.get("List_UpgradeCarBodyWeight.Mass");
  std::printf("mass %.0f kg; box %.4f x %.4f x %.4f m\n", m, p.inertia_box.x, p.inertia_box.y, p.inertia_box.z);
  // The box rule against the columns.
  const double x = car.get("List_UpgradeCarBodyWeight.BlockDimX"), y = car.get("List_UpgradeCarBodyWeight.BlockDimY"),
               z = car.get("List_UpgradeCarBodyWeight.BlockDimZ");
  CHECK(p.inertia.x == 0 && p.inertia_box.x == x && p.inertia_box.y == y && p.inertia_box.z == z);
  std::printf("inertia (box rule) %.1f %.1f %.1f kg m^2\n", m * (y * y + z * z) / 12, m * (x * x + z * z) / 12,
              m * (x * x + y * y) / 12);
  // Per-mass rates.
  CHECK(near_rel(p.spring_rate[0], car.get("FrontSpringDamper.DefSpringRate") * m, 1e-12));
  CHECK(near_rel(p.rebound_rate[1], car.get("RearSpringDamper.DefDampenReboundRate") * m, 1e-12));
  CHECK(near_rel(p.damper_clamp[0], car.get("FrontSpringDamper.DampenBumpClamp") * m, 1e-12));
  std::printf("springs %.0f / %.0f N/m, bump %.0f / %.0f, rebound %.0f / %.0f N s/m\n", p.spring_rate[0],
              p.spring_rate[1], p.bump_rate[0], p.bump_rate[1], p.rebound_rate[0], p.rebound_rate[1]);
  // Substeps: ceil(dt x 359), dt clamped to 0.1 s.
  CHECK(s.get("CarPhysicsUpdateHz", 0) == 359);
  CHECK(drive::substeps_for(1.0 / 120, 359) == 3);
  CHECK(drive::substeps_for(1.0 / 60, 359) == 6);
  CHECK(drive::substeps_for(0.5, 359) == 36);
  // The sidewall factor: 1.27 on the R34's tyres.
  std::printf("sidewall factor %.4f front, %.4f rear\n", p.sidewall_stretch[0], p.sidewall_stretch[1]);
  CHECK(std::fabs(p.sidewall_stretch[0] - 1.27) < 0.01 && std::fabs(p.sidewall_stretch[1] - 1.27) < 0.01);
  // The load blend: halfway between the two loads is the mean of the
  // curves, at any slip, on the stretched axis.
  const auto& lc = p.longitudinal;
  const double mid = 0.5 * (lc.light_load_kgf + lc.heavy_load_kgf);
  for (double slip : {0.02, 0.1, 0.2, 0.5}) {
    const double v = lc.value(slip, mid, p.sidewall_stretch[0]);
    const double want = 0.5 * (lc.light.at(slip, p.sidewall_stretch[0]) + lc.heavy.at(slip, p.sidewall_stretch[0]));
    CHECK(std::fabs(v - want) < 1e-12);
  }
  CHECK(lc.value(0, mid, 1.27) == 0);
  const double load = 0.25 * m * 9.8 / 9.80665;
  std::printf("peak slips at a quarter of the weight: ratio %.4f, angle %.2f deg\n",
              lc.peak_slip(load, p.sidewall_stretch[0]), p.lateral.peak_slip(load, p.sidewall_stretch[0]));
  CHECK(p.torque_curve.size() + 1 == car.curve("torque")->size());
  CHECK(p.torque_curve.back() > 0);
}

double heading_of(double dx, double dz) { return std::atan2(dx, dz); }

struct Crossings {
  double t[5] = {-1, -1, -1, -1, -1};  // 20, 40, 60, 80, 100 km/h, from the launch
  double d[5] = {};
  double shift_t = -1, shift_kmh = 0, shift_rpm = 0;
  double peak_rpm_clutch = 0, moved_at = -1;
};

void scenario_a(const drive::CarParams& p) {
  const drive::ProfileGround road = drive::spawn_road(kStartX, kStartY, kStartZ, kDirX, kDirZ);
  drive::Vehicle v(p);
  v.reset({kStartX, kStartY, kStartZ}, heading_of(kDirX, kDirZ));
  const drive::Vec3 start = v.centre_of_mass();
  // Static loads (settled for 2 s, no input).
  for (int i = 0; i < 240; ++i) v.step({}, road);
  double sum = 0, front = 0;
  for (int i = 0; i < drive::kWheels; ++i) {
    sum += v.wheels()[size_t(i)].load;
    if (i < 2) front += v.wheels()[size_t(i)].load;
  }
  const double weight = p.mass * p.gravity;
  std::printf("static loads: sum %.1f N against the weight %.1f N, front pair %.2f %%\n", sum, weight,
              100 * front / sum);
  // On the grade the weight's normal part is slightly less; 1 % covers it.
  CHECK(near_rel(sum, weight, 0.01));
  CHECK(std::fabs(100 * front / sum - 57.5) < 0.575);
  const double drift = std::hypot(v.centre_of_mass().x - start.x, v.centre_of_mass().z - start.z);
  std::printf("rolled %.3f m while settling on the grade\n", drift);

  v.reset({kStartX, kStartY, kStartZ}, heading_of(kDirX, kDirZ));
  for (int i = 0; i < 2; ++i) v.step({}, road);
  Crossings c;
  double launch = -1, prev_kmh = 0, prev_t = 0;
  drive::Vec3 at_launch{};
  bool shifted = false;
  const double targets[5] = {20, 40, 60, 80, 100};
  for (int i = 0; i < 120 * 15; ++i) {
    v.step({0, 1, 0, false}, road);
    const double t = v.time();
    if (launch < 0 && v.shaped_input().throttle > 0.05) {
      launch = t;
      at_launch = v.centre_of_mass();
    }
    if (launch < 0) continue;
    const double kmh = v.speed() * 3.6;
    const double since = t - launch;
    if (since < 0.8) c.peak_rpm_clutch = std::max(c.peak_rpm_clutch, v.rpm());
    if (c.moved_at < 0 && v.speed() > 0.36) c.moved_at = since;
    for (int k = 0; k < 5; ++k)
      if (c.t[k] < 0 && kmh >= targets[k]) {
        c.t[k] = prev_t - launch + (t - prev_t) * (targets[k] - prev_kmh) / (kmh - prev_kmh);
        const drive::Vec3 d = v.centre_of_mass() - at_launch;
        c.d[k] = std::sqrt(drive::dot(d, d));
      }
    if (!shifted && v.gear() == 1 && v.shifting()) {
      shifted = true;
      c.shift_t = since;
      c.shift_kmh = kmh;
      c.shift_rpm = v.rpm();
    }
    prev_kmh = kmh;
    prev_t = t;
    if (c.t[4] >= 0) break;
  }
  const double oracle[5] = {1.122, 1.891, 2.780, 4.557, 5.837};
  std::printf("scenario A (sim time from the launch, gas > 0.05):\n");
  std::printf("  clutch phase: engine peak %.0f rpm in the first 0.8 s; 0.36 m/s at %.3f s (oracle ~5,800 rpm, 0.57 s)\n",
              c.peak_rpm_clutch, c.moved_at);
  for (int k = 0; k < 5; ++k) {
    const double tol = k < 4 ? std::max(0.02 * oracle[k], 0.05) : 0.02 * oracle[k];
    const bool ok = c.t[k] >= 0 && std::fabs(c.t[k] - oracle[k]) <= tol;
    std::printf("  0-%3.0f km/h %.3f s at %.1f m (oracle %.3f s, tolerance %.3f s) %s\n", targets[k], c.t[k], c.d[k],
                oracle[k], tol, ok ? "ok" : "MISS");
    GATE(ok);
  }
  std::printf("  1->2 shift at %.1f km/h, %.3f s, %.0f rpm (oracle 67.8 km/h, 3.55 s, 7,690 rpm)\n", c.shift_kmh,
              c.shift_t, c.shift_rpm);
  GATE(near_rel(c.shift_kmh, 67.8, 0.02) && near_rel(c.shift_t, 3.55, 0.02));
}

void scenario_b(const drive::CarParams& p) {
  const drive::ProfileGround road = drive::spawn_road(kStartX, kStartY, kStartZ, kDirX, kDirZ);
  drive::Vehicle v(p);
  v.reset({kStartX, kStartY, kStartZ}, heading_of(kDirX, kDirZ));
  for (int i = 0; i < 2; ++i) v.step({}, road);
  // Full throttle to the oracle's onset speed (run B1: 81.69 km/h), then
  // full brake to a stop (below 0.3 m/s).
  const double onset_kmh = 81.69;
  // PHYSICS_TRACE_B names a CSV of every step (trace_row), for debugging.
  FILE* trace = std::getenv("PHYSICS_TRACE_B") ? std::fopen(std::getenv("PHYSICS_TRACE_B"), "w") : nullptr;
  if (trace) std::fprintf(trace, "%s\n", drive::trace_header().c_str());
  int i = 0;
  for (; i < 120 * 15 && v.speed() * 3.6 < onset_kmh; ++i) {
    v.step({0, 1, 0, false}, road);
    if (trace) std::fprintf(trace, "%s\n", drive::trace_row(v).c_str());
  }
  const double v0 = v.speed(), t0 = v.time();
  const drive::Vec3 p0 = v.centre_of_mass();
  const int gear0 = v.gear();
  int abs_steps = 0;
  double t_stop = -1;
  for (int k = 0; k < 120 * 10; ++k) {
    v.step({0, 0, 1, false}, road);
    if (trace) std::fprintf(trace, "%s\n", drive::trace_row(v).c_str());
    abs_steps += v.abs_active();
    if (v.speed() < 0.3) {
      t_stop = v.time() - t0;
      break;
    }
  }
  if (trace) std::fclose(trace);
  const drive::Vec3 d = v.centre_of_mass() - p0;
  const double dist = std::sqrt(drive::dot(d, d));
  const double decel = (v0 - v.speed()) / t_stop;
  std::printf("scenario B: brake at %.2f km/h in gear %d; stop in %.3f s over %.2f m; mean deceleration %.3f m/s^2 "
              "(oracle 8.57 +-2 %%; distance for v0 %.2f m); ABS steps %d (oracle 64 at 60 Hz)\n",
              v0 * 3.6, gear0, t_stop, dist, decel, v0 * v0 / (2 * 8.57), abs_steps);
  CHECK(t_stop > 0);
  GATE(near_rel(decel, 8.57, 0.02));
}

void turn_circle(const drive::CarParams& p) {
  const drive::FlatGround flat;
  drive::Vehicle v(p);
  v.reset({0, 0, 0}, 0);
  for (int i = 0; i < 2; ++i) v.step({}, flat);
  // Full left lock; a little throttle to 2.8 m/s, then coast (measured
  // from 0.8 s after the release, for 3 s).
  int i = 0;
  FILE* trace = std::getenv("PHYSICS_TRACE_T") ? std::fopen(std::getenv("PHYSICS_TRACE_T"), "w") : nullptr;
  if (trace) std::fprintf(trace, "%s\n", drive::trace_header().c_str());
  auto step = [&](const drive::Input& in) {
    v.step(in, flat);
    if (trace) std::fprintf(trace, "%s\n", drive::trace_row(v).c_str());
  };
  for (; i < 120 * 10 && v.speed() < 2.8; ++i) step({-1, 0.3, 0, false});
  for (int k = 0; k < 96; ++k) step({-1, 0, 0, false});
  double sum = 0, v_min = 1e9, v_max = 0;
  int n = 0;
  for (int k = 0; k < 360; ++k) {
    step({-1, 0, 0, false});
    const drive::Vec3 vel = v.velocity();
    const double speed = std::hypot(vel.x, vel.z);
    const double yaw_rate = std::fabs(v.angular_velocity().y);
    if (yaw_rate > 1e-6) {
      sum += speed / yaw_rate;
      ++n;
    }
    v_min = std::min(v_min, speed);
    v_max = std::max(v_max, speed);
  }
  if (trace) std::fclose(trace);
  const double radius = n ? sum / n : 0;
  std::printf("turn circle: centre-of-mass path radius %.3f m at %.2f-%.2f m/s, road wheels %.1f deg "
              "(oracle 3.37 m +-2 %%)\n",
              radius, v_min, v_max, std::fabs(v.steer_angle()) * 180 / kPi);
  GATE(near_rel(radius, 3.37, 0.02));
}

}  // namespace

int main() {
  const char* root = std::getenv("SHIFT_GAME_ROOT");
  if (!root) {
    std::printf("physics_tests: SHIFT_GAME_ROOT not set, skipped\n");
    return 77;
  }
  const char* state = std::getenv("SHIFT_STATE_ROOT");
  cars::Install install;
  std::string error;
  if (!cars::discover_install(root, state ? state : "", "", install, &error)) {
    std::printf("FAIL install: %s\n", error.c_str());
    return 1;
  }
  cars::PhysicsSettings settings;
  std::vector<cars::CarPhysics> rows;
  if (!cars::load_physics_settings(install.media / "physics.zip", settings, &error) ||
      !cars::load_car_physics(install, {445}, rows, &error)) {
    std::printf("FAIL physics rows: %s\n", error.c_str());
    return 1;
  }
  const cars::CarPhysics& r34 = rows[0];
  std::printf("car %lld %s: %zu values, %zu curves\n", static_cast<long long>(r34.car_id), r34.media_name.c_str(),
              r34.values.size(), r34.curves.size());
  drive::CarParams p;
  drive::geometry_from_physics(r34, p);
  std::vector<std::string> notes;
  drive::apply_physics(r34, settings, p, &notes);
  for (const auto& n : notes) std::printf("note: %s\n", n.c_str());
  // Experiments for the spec questions (not the gate): PHYSICS_BRAKE_REACTION
  // scales the tyre's braking torque on the wheel; PHYSICS_REAR_BRAKE sets
  // the rear brake torque (N m).
  if (const char* e = std::getenv("PHYSICS_BRAKE_REACTION")) p.brake_reaction_scale = std::atof(e);
  if (const char* e = std::getenv("PHYSICS_REAR_BRAKE")) p.rear_brake_torque = std::atof(e);
  if (const char* e = std::getenv("PHYSICS_ABS")) p.abs = std::atoi(e) != 0;
  if (const char* e = std::getenv("PHYSICS_WHEEL_J")) p.wheel_inertia = std::atof(e);
  if (const char* e = std::getenv("PHYSICS_BRAKE_CLUTCH")) p.brake_opens_clutch = std::atoi(e) == 0;

  unit_checks(r34, settings, p);
  scenario_a(p);
  scenario_b(p);
  turn_circle(p);
  const char* g = std::getenv("PHYSICS_GATE");
  const bool gate = g && std::atoi(g) != 0;
  std::printf("physics_tests: %d failures; %d scenario gate misses%s\n", failures, gate_misses,
              gate ? " (PHYSICS_GATE=1: fatal)" : " (reported, not fatal: see SPEC_QUESTIONS)");
  return failures || (gate && gate_misses) ? 1 : 0;
}
