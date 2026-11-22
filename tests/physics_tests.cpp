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

// The scenario gates fail the test (PHYSICS_GATE=0 makes them report-only,
// for experiments). REPORT checks are printed only: their spec questions
// are open as polish (SPEC_QUESTIONS Q34).
int gate_misses = 0, report_misses = 0;
#define REPORT(cond)                                                      \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("report miss %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++report_misses;                                                    \
    }                                                                     \
  } while (0)
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
  double request_t = -1, change_t = -1, change_kmh = 0, relock_t = -1;
  double launch_in = -1, launch_out = -1, launch_out_kmh = 0, locked_at = -1, locked_kmh = 0;
};

const char* state_name(drive::Vehicle::GearState s) {
  using S = drive::Vehicle::GearState;
  switch (s) {
    case S::kOpen: return "open";
    case S::kLaunch: return "launch";
    case S::kEngaged: return "engaged";
    case S::kClutchIn: return "clutch in";
    case S::kGearChange: return "gear change";
    case S::kRevMatch: return "rev-match";
    case S::kClutchOut: return "clutch out";
  }
  return "?";
}

// The axle's mean |normalised slip ratio|.
double axle_slip(const drive::Vehicle& v, int axle) {
  return 0.5 * (std::fabs(v.wheels()[size_t(2 * axle)].slip_ratio) + std::fabs(v.wheels()[size_t(2 * axle + 1)].slip_ratio));
}

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
  const double targets[5] = {20, 40, 60, 80, 100};
  // The spec's launch table (Clutch and gearbox states), at its sample times.
  const double marks[] = {0.0, 0.3, 0.5, 0.6, 0.7, 0.9, 1.1, 1.3, 1.4, 1.5, 1.6, 2.0, 2.5, 3.0};
  size_t mark = 0;
  std::printf("scenario A launch (oracle: open to 0.5 s at 4,857 rpm; launch 0.6-1.4 s, c 0.43-0.65; clutch out\n"
              "  1.5 s; engaged from 1.6 s at 33.1 km/h):\n"
              "     t   km/h  state         c    rpm  out rpm  target  rear  front  throttle\n");
  using S = drive::Vehicle::GearState;
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
    if (mark < sizeof marks / sizeof marks[0] && since >= marks[mark] - 1e-9) {
      std::printf("  %4.2f %6.1f  %-11s %4.2f %6.0f %8.0f %7.0f  %4.2f  %5.2f  %4.2f\n", since, kmh,
                  state_name(v.gear_state()), 1 - v.clutch(), v.rpm(), v.clutch_output_rpm(),
                  v.gear_state() == S::kLaunch ? v.launch_target() : 0.0, axle_slip(v, 1), axle_slip(v, 0),
                  v.engine_throttle());
      ++mark;
    }
    if (c.launch_in < 0 && v.gear_state() == S::kLaunch) c.launch_in = since;
    if (c.launch_in >= 0 && c.launch_out < 0 && v.gear_state() != S::kLaunch) {
      c.launch_out = since;
      c.launch_out_kmh = kmh;
    }
    if (c.locked_at < 0 && v.gear() == 1 && v.gear_state() == S::kEngaged && v.clutch_locked()) {
      c.locked_at = since;
      c.locked_kmh = kmh;
    }
    for (int k = 0; k < 5; ++k)
      if (c.t[k] < 0 && kmh >= targets[k]) {
        c.t[k] = prev_t - launch + (t - prev_t) * (targets[k] - prev_kmh) / (kmh - prev_kmh);
        const drive::Vec3 d = v.centre_of_mass() - at_launch;
        c.d[k] = std::sqrt(drive::dot(d, d));
      }
    if (c.request_t < 0 && v.gear() == 1 && v.gear_state() == S::kClutchIn) c.request_t = since;
    if (c.change_t < 0 && v.gear() == 2) {
      c.change_t = since;
      c.change_kmh = kmh;
    }
    if (c.change_t >= 0 && c.relock_t < 0 && v.gear_state() == S::kEngaged) c.relock_t = since;
    prev_kmh = kmh;
    prev_t = t;
    if (c.t[4] >= 0) break;
  }
  // Run A's crossings; 0-100 is the mean of runs A and C (5.837, 5.851),
  // CAR_PHYSICS cfc4f721.
  const double oracle[5] = {1.122, 1.891, 2.780, 4.557, 5.84};
  std::printf("scenario A (sim time from the launch, gas > 0.05):\n");
  std::printf("  launch state %.3f to %.3f s (left at %.1f km/h), locked at %.3f s, %.1f km/h (oracle ~0.51 to 1.42 s at "
              "29.6 km/h, locked by 1.58 s at 32.7 km/h)\n",
              c.launch_in, c.launch_out, c.launch_out_kmh, c.locked_at, c.locked_kmh);
  for (int k = 0; k < 5; ++k) {
    const double tol = k < 4 ? std::max(0.02 * oracle[k], 0.05) : 0.02 * oracle[k];
    const bool ok = c.t[k] >= 0 && std::fabs(c.t[k] - oracle[k]) <= tol;
    std::printf("  0-%3.0f km/h %.3f s at %.1f m (oracle %.3f s, tolerance %.3f s) %s\n", targets[k], c.t[k], c.d[k],
                oracle[k], tol, ok ? "ok" : "MISS");
    // M3 gate: 0-80 and 0-100 at 2 %; the early crossings report only.
    if (k >= 3) GATE(ok);
    else REPORT(ok);
  }
  std::printf("  1->2: requested %.3f s, gear change %.3f s at %.1f km/h, locked %.3f s (oracle 3.20, 3.55 s at 68 km/h, "
              "3.91 s)\n",
              c.request_t, c.change_t, c.change_kmh, c.relock_t);
  REPORT(near_rel(c.change_kmh, 68, 0.02) && near_rel(c.change_t, 3.55, 0.02));
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
  int abs_steps = 0, steps = 0, braked[2] = {};
  double t_stop = -1, t_down = -1, kmh_down = 0, t_rev = -1, v_rev = 0;
  for (int k = 0; k < 120 * 10; ++k) {
    v.step({0, 0, 1, false}, road);
    if (trace) std::fprintf(trace, "%s\n", drive::trace_row(v).c_str());
    abs_steps += v.abs_active();
    if (t_down < 0 && v.gear() == 1) {
      t_down = v.time() - t0;
      kmh_down = v.speed() * 3.6;
    }
    if (t_rev < 0 && v.gear() == -1) {
      t_rev = v.time() - t0;
      v_rev = v.speed();
    }
    if (t_rev < 0) {
      ++steps;
      for (int a = 0; a < 2; ++a) braked[a] += v.wheels()[size_t(2 * a)].brake > 0;
    }
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
              "(oracle 8.57 +-2 %%, 2.645 s; distance for v0 %.2f m); ABS steps %d of %d\n",
              v0 * 3.6, gear0, t_stop, dist, decel, v0 * v0 / (2 * 8.57), abs_steps, int(std::lround(t_stop * 120)));
  std::printf("  2->1 gear change at +%.3f s, %.1f km/h (oracle +0.50 s, 59.8 km/h); reverse at +%.3f s, %.1f km/h "
              "(oracle +1.31-1.36 s, 22.8-23.0 km/h)\n",
              t_down, kmh_down, t_rev, v_rev * 3.6);
  if (t_rev > 0)
    std::printf("  ABS phase %.2f m/s^2 (oracle 11.9-12.1); braked steps front %.0f %%, rear %.0f %% (oracle 50-54, "
                "74-81); reverse phase %.2f m/s^2 over %.3f s (oracle ~5, 1.26-1.29 s)\n",
                (v0 - v_rev) / t_rev, 100.0 * braked[0] / steps, 100.0 * braked[1] / steps,
                (v_rev - v.speed()) / (t_stop - t_rev), t_stop - t_rev);
  CHECK(t_stop > 0);
  // Slice 1 gate (SPEC-CAR-PHYSICS, Tolerances): Scenario B mean deceleration
  // within 2 % of oracle 8.57 m/s^2, and ABS phase within 12 %.
  GATE(near_rel(decel, 8.57, 0.02));
  GATE(t_rev > 0 && near_rel((v0 - v_rev) / t_rev, 12.0, 0.12));
}

void turn_circle(const drive::CarParams& p) {
  const drive::FlatGround flat;
  drive::Vehicle v(p);
  v.reset({0, 0, 0}, 0);
  for (int i = 0; i < 2; ++i) v.step({}, flat);
  // SPEC-CAR-PHYSICS, the turn circle's inputs: full left lock, full
  // throttle until about 3 m/s, then the gas released, coasting in first;
  // the radius over the coast at 2 to 3 m/s (3.5 s).
  int i = 0;
  FILE* trace = std::getenv("PHYSICS_TRACE_T") ? std::fopen(std::getenv("PHYSICS_TRACE_T"), "w") : nullptr;
  if (trace) std::fprintf(trace, "%s\n", drive::trace_header().c_str());
  auto step = [&](const drive::Input& in) {
    v.step(in, flat);
    if (trace) std::fprintf(trace, "%s\n", drive::trace_row(v).c_str());
  };
  for (; i < 120 * 10 && v.speed() < 3.0; ++i) step({-1, 1, 0, false});
  const double t_release = v.time();
  // The coast: measured for 3.5 s from the first step under 3 m/s, while
  // the speed stays above 2 m/s.
  double sum = 0, v_min = 1e9, v_max = 0, v_first = -1, v_last = 0;
  int n = 0, coast = 0;
  for (int k = 0; k < 120 * 10 && coast < 420; ++k) {
    step({-1, 0, 0, false});
    const drive::Vec3 vel = v.velocity();
    const double speed = std::hypot(vel.x, vel.z);
    const double yaw_rate = std::fabs(v.angular_velocity().y);
    if (v_first < 0 && speed >= 3.0) continue;
    ++coast;
    if (speed < 2.0) continue;
    if (v_first < 0) v_first = speed;
    v_last = speed;
    if (yaw_rate > 1e-6) {
      sum += speed / yaw_rate;
      ++n;
    }
    v_min = std::min(v_min, speed);
    v_max = std::max(v_max, speed);
  }
  if (trace) std::fclose(trace);
  const double radius = n ? sum / n : 0;
  std::printf("turn circle: released at %.2f s; centre-of-mass path radius %.3f m at %.2f-%.2f m/s (%.2f -> %.2f over "
              "the coast), road wheels %.1f deg, gear %d, %.0f rpm at the end (oracle 3.37 m +-2 %%; 2.84 -> 2.08 m/s "
              "over 3.5 s, 816 rpm)\n",
              t_release, radius, v_min, v_max, v_first, v_last, std::fabs(v.steer_angle()) * 180 / kPi, v.gear(),
              v.rpm());
  GATE(near_rel(radius, 3.37, 0.02));
}

// The skidpad (report only, ours; Q40's corner): under the AI's controls,
// held at a speed by the gas, the road wheels at a fixed angle; the steady
// lateral acceleration (speed x yaw rate) and both axles' normalised slip
// angles after 4 s. Q40's oracle turns at 12 to 14 m/s^2 on 3 to 4 deg.
void skidpad(const drive::CarParams& p) {
  const drive::FlatGround flat;
  for (double speed : {20.0, 28.0}) {
    std::printf("skidpad at %.0f m/s:", speed);
    for (double deg : {1.0, 2.0, 3.0, 4.0, 6.0, 8.0}) {
      drive::Vehicle v(p);
      v.set_ai_controls(true);
      v.reset({0, 0, 0}, 0);
      for (int i = 0; i < 2; ++i) v.step({}, flat);
      const double steer = deg * kPi / 180 / p.max_steer;
      double ay = 0, sy_f = 0, sy_r = 0;
      int n = 0, since = 0;
      for (int k = 0; k < 120 * 40; ++k) {
        const double sp = v.speed();
        const bool cornering = sp >= speed - 0.3 || k > 120 * 25;
        const double gas = std::clamp((speed - sp) * 0.8, 0.0, 1.0);
        v.step({cornering ? steer : 0.0, gas, 0, false}, flat);
        if (!cornering) since = 0;
        else if (++since > 120 * 4 && since <= 120 * 6) {
          ay += v.speed() * std::fabs(v.angular_velocity().y);
          sy_f += 0.5 * (std::fabs(v.wheels()[0].slip_angle) + std::fabs(v.wheels()[1].slip_angle));
          sy_r += 0.5 * (std::fabs(v.wheels()[2].slip_angle) + std::fabs(v.wheels()[3].slip_angle));
          ++n;
        }
        if (since > 120 * 6) break;
      }
      if (n) std::printf("  %.0f deg: %.1f m/s^2 (slip f %.2f r %.2f, %.1f m/s)", deg, ay / n, sy_f / n, sy_r / n,
                         v.speed());
      else std::printf("  %.0f deg: -", deg);
    }
    std::printf("\n");
  }
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
  // Experiments (not the gate): PHYSICS_FRONT_BRAKE and PHYSICS_REAR_BRAKE
  // set the brake torques (N m), PHYSICS_ABS 0 turns the ABS off,
  // PHYSICS_WHEEL_J sets the wheel inertia, PHYSICS_BOOST_LAG the boost lag.
  if (const char* e = std::getenv("PHYSICS_FRONT_BRAKE")) { p.front_brake_torque = std::atof(e); p.use_per_car_brakes = false; }
  if (const char* e = std::getenv("PHYSICS_REAR_BRAKE")) { p.rear_brake_torque = std::atof(e); p.use_per_car_brakes = false; }
  if (const char* e = std::getenv("PHYSICS_ABS")) p.abs = std::atoi(e) != 0;
  if (const char* e = std::getenv("PHYSICS_WHEEL_J")) p.wheel_inertia = std::atof(e);
  if (const char* e = std::getenv("PHYSICS_BOOST_LAG")) p.boost_lag = std::atof(e);
  if (const char* e = std::getenv("PHYSICS_ENGINE_J")) p.engine_inertia = std::atof(e);
  if (const char* e = std::getenv("PHYSICS_CM_HEIGHT")) p.centre_of_mass.y = std::atof(e);
  drive::Vehicle probe_v(p);
  const double unit_f = p.mass * (9.80665 / 4.0) * p.tyre_radius[0];
  const double unit_r = p.mass * (9.80665 / 4.0) * p.tyre_radius[2];
  std::printf("engine inertia %.4f kg m^2, brake const front %.2f (%.0f N m @0.81) rear %.2f (%.0f N m @0.81), "
              "braking friction scale %.4f, aero drag %.4f N/(m/s)^2\n",
              p.engine_inertia, probe_v.brake_const(0), probe_v.brake_const(0) * 0.81 * unit_f, probe_v.brake_const(1),
              probe_v.brake_const(1) * 0.81 * unit_r, p.brake_friction_scale, p.aero_drag_long);

  unit_checks(r34, settings, p);
  scenario_a(p);
  scenario_b(p);
  turn_circle(p);
  skidpad(p);
  const char* g = std::getenv("PHYSICS_GATE");
  const bool gate = !g || std::atoi(g) != 0;
  std::printf("physics_tests: %d failures; %d scenario gate misses%s; %d report-only misses (SPEC_QUESTIONS)\n",
              failures, gate_misses, gate ? "" : " (PHYSICS_GATE=0: not fatal)", report_misses);
  return failures || (gate && gate_misses) ? 1 : 0;
}
