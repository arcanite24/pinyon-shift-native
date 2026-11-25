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
#include "drive/contact.h"
#include "drive/dualsense.h"
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

void synthetic_r2_4_checks() {
  auto make_synth = []() {
    drive::CarParams p;
    const double r = 0.32;
    p.wheel_centres = {drive::Vec3{-0.78, r, 1.30}, drive::Vec3{0.78, r, 1.30},
                       drive::Vec3{-0.79, r, -1.25}, drive::Vec3{0.79, r, -1.25}};
    p.tyre_radius = {r, r, r, r};
    p.centre_of_mass = {0, 0.5, 0.05};
    return p;
  };
  struct CustomSurfaceGround : public drive::Ground {
    drive::SurfaceTyre st;
    drive::Vec3 n{0, 1, 0};
    double g = 1.0;
    bool raycast(const drive::Vec3& o, const drive::Vec3& d, double max_d, drive::Hit& hit) const override {
      if (d.y >= -1e-9) return false;
      const double t = -o.y / d.y;
      if (t < 0 || t > max_d) return false;
      hit.distance = t;
      hit.normal = n;
      hit.grip = g;
      hit.tyre = st;
      return true;
    }
  };
  const drive::FlatGround flat;

  // 1. SurfaceTypes: MaxSteeringLockCap in degrees, LinearDrag & AngularDrag,
  //    CarWorldCollFricMods, and Springiness cutoffs.
  {
    auto p = make_synth();
    p.max_steer = 42.0 * kPi / 180.0;
    CustomSurfaceGround g_cap;
    g_cap.st.max_steering_lock_cap = 20.0;  // 20 degrees (not normalized!)
    g_cap.st.chase_cam_spring_mult = 0.7;
    g_cap.st.steer_torque_scale = 0.6;
    drive::Vehicle v(p);
    for (int i = 0; i < 60; ++i) v.step({1.0, 0.0, 0.0, false}, g_cap);
    CHECK(std::fabs(v.steer_angle() - 20.0 * kPi / 180.0) < 0.02);
    CHECK(std::fabs(v.surface_chase_cam_spring_mult() - 0.7) < 1e-6);
    CHECK(std::fabs(v.surface_steer_torque_scale() - 0.6) < 1e-6);

    // LinearDrag & AngularDrag slow coasting speed and yaw spin.
    CustomSurfaceGround g_mud;
    g_mud.st.linear_drag = 2.0;
    g_mud.st.angular_drag = 3.0;
    drive::Vehicle v_dry(p), v_mud(p);
    v_dry.launch({0, 0, 0}, 0, 25.0);
    v_mud.launch({0, 0, 0}, 0, 25.0);
    v_dry.set_velocity({0, 0, 25.0}, {0, 2.0, 0});
    v_mud.set_velocity({0, 0, 25.0}, {0, 2.0, 0});
    for (int i = 0; i < 60; ++i) {
      v_dry.step({}, flat);
      v_mud.step({}, g_mud);
    }
    CHECK(v_mud.speed() < v_dry.speed() - 5.0);
    CHECK(std::fabs(v_mud.angular_velocity().y) < std::fabs(v_dry.angular_velocity().y) * 0.6);

    // CarWorldCollFricMods scales body_contact_friction.
    drive::Hit h_mod{};
    h_mod.tyre.car_world_coll_fric_mod = 0.25;
    const double mu_base = drive::body_contact_friction(drive::Hit{}, {0, 1, 0}, 0.6);
    const double mu_mod = drive::body_contact_friction(h_mod, {0, 1, 0}, 0.6);
    CHECK(std::fabs(mu_mod - mu_base * 0.25) < 1e-6);

    // Springiness cutoffs: inactive below spring_speed_cutoff, active above.
    CustomSurfaceGround g_spr;
    g_spr.st.is_springy = true;
    g_spr.st.spring_k = 20.0;
    g_spr.st.spring_comp_d = 10.0;
    g_spr.st.spring_reb_d = 10.0;
    g_spr.st.spring_norm_y_cutoff = 0.707;
    g_spr.st.spring_speed_cutoff = 10.0;
    drive::Vehicle v_slow(p), v_fast(p);
    v_slow.launch({0, 0, 0}, 0, 2.0);
    v_fast.launch({0, 0, 0}, 0, 20.0);
    for (int i = 0; i < 20; ++i) {
      v_slow.step({}, g_spr);
      v_fast.step({}, g_spr);
    }
    CHECK(std::isfinite(v_slow.position().y) && std::isfinite(v_fast.position().y));
  }

  // 2. Rally Suspension (Level == 4) lifts static ride height, CoM, and rest_length;
  //    TireMod_FrictionScale, HeatTireScale, List_TireAffectCurve, and StaticToe wire cleanly.
  {
    cars::CarPhysics base_cp, rally_cp;
    cars::PhysicsSettings ps;
    base_cp.values["List_UpgradeCarBodyWeight.Mass"] = 1400.0;
    base_cp.values["List_UpgradeCarBodyWeight.CMHeight"] = 0.48;
    base_cp.values["List_UpgradeSpringDamper.Level"] = 0.0;
    base_cp.values["List_TireCompound.TireMod_FrictionScale"] = 1.0;
    base_cp.values["List_TireCompound.HeatTireScale"] = 1.0;
    rally_cp = base_cp;
    rally_cp.values["List_UpgradeSpringDamper.Level"] = 4.0;
    rally_cp.values["List_TireCompound.TireMod_FrictionScale"] = 1.15;
    rally_cp.values["List_TireCompound.HeatTireScale"] = 2.0;
    rally_cp.curves["tire_affect"] = {0.85, 1.00, 0.90};
    rally_cp.values["List_TireAffectCurve.MinTemp"] = 20.0;
    rally_cp.values["List_TireAffectCurve.MaxTemp"] = 180.0;

    drive::CarParams p_base = make_synth(), p_rally = make_synth();
    drive::apply_physics(base_cp, ps, p_base);
    drive::apply_physics(rally_cp, ps, p_rally);
    CHECK(p_rally.centre_of_mass.y > p_base.centre_of_mass.y + 0.04);
    CHECK(p_rally.rest_length > p_base.rest_length + 0.04);
    CHECK(p_rally.sill_height > p_base.sill_height + 0.04);
    CHECK(std::fabs(p_rally.tire_mod_friction_scale - 1.15) < 1e-6);
    CHECK(std::fabs(p_rally.heat_tire_scale - 2.0) < 1e-6);
    CHECK(drive::tyre_peak_mu(p_rally, 0, drive::TyrePeak::kLateral, 3500.0, 20.0) >
          drive::tyre_peak_mu(p_base, 0, drive::TyrePeak::kLateral, 3500.0, 20.0) * 1.10);
    CHECK(std::fabs(p_rally.tire_affect_curve.evaluate(100.0) - 1.00) < 1e-6);
    CHECK(std::fabs(p_rally.tire_affect_curve.evaluate(20.0) - 0.85) < 1e-6);

    // Static toe rotates front/rear wheel headings symmetrically (toe-out + / toe-in -).
    drive::CarTune tune;
    tune.front_toe_deg = 2.0;
    tune.rear_toe_deg = -1.5;
    drive::apply_tune(tune, base_cp, p_base);
    drive::Vehicle v_toe(p_base);
    v_toe.step({}, flat);
    CHECK(v_toe.wheels()[0].steer < -0.02 && v_toe.wheels()[1].steer > 0.02);
    CHECK(std::fabs(v_toe.wheels()[0].steer + v_toe.wheels()[1].steer) < 1e-9);
    CHECK(v_toe.wheels()[2].steer > 0.02 && v_toe.wheels()[3].steer < -0.02);
  }

  // 3. Coupled zero-lag LSD: transfers torque on the very first step of a split-mu launch.
  {
    struct SplitMu : public drive::Ground {
      bool raycast(const drive::Vec3& o, const drive::Vec3& d, double max_d, drive::Hit& hit) const override {
        if (d.y >= -1e-9) return false;
        const double t = -o.y / d.y;
        if (t < 0 || t > max_d) return false;
        hit.distance = t;
        hit.normal = {0, 1, 0};
        hit.grip = o.x < 0 ? 0.15 : 1.0;
        return true;
      }
    } split;
    auto p_open = make_synth();
    p_open.layout = drive::CarParams::Layout::kRear;
    p_open.torque_scale = 700.0;
    p_open.rear_lsd = {0.0, 0.0, 50.0, 50.0};
    auto p_lsd = p_open;
    p_lsd.rear_lsd = {800.0, 0.0, 50.0, 50.0};
    drive::Vehicle v_open(p_open), v_lsd(p_lsd);
    v_open.set_ai_controls(true);
    v_lsd.set_ai_controls(true);
    v_open.launch({0, 0, 0}, 0, 5.0);
    v_lsd.launch({0, 0, 0}, 0, 5.0);
    // Single step: zero-lag coupled LSD immediately reduces left/right speed divergence.
    v_open.step({0, 1.0, 0, false}, split);
    v_lsd.step({0, 1.0, 0, false}, split);
    const double dw_open = std::fabs(v_open.wheels()[2].omega - v_open.wheels()[3].omega);
    const double dw_lsd = std::fabs(v_lsd.wheels()[2].omega - v_lsd.wheels()[3].omega);
    CHECK(dw_open > 0.05 && dw_lsd < dw_open * 0.85);
  }

  // 4. Manual+Clutch shift dwell, handbrake disconnect, and race-free Auto-Brake assist.
  {
    auto p = make_synth();
    p.layout = drive::CarParams::Layout::kRear;
    drive::Vehicle v(p);
    drive::VehicleAssists a;
    a.shifting = 2;  // Manual + Clutch
    v.set_assists(a);
    v.launch({0, 0, 0}, 0, 20.0);
    const int g0 = v.gear();
    // Shift up with clutch: enters shift dwell (kGearChange) before smoothly re-engaging.
    v.step({0, 0.5, 0, false, 1.0, +1}, flat);
    CHECK(v.gear() == g0 + 1 && v.gear_state() == drive::Vehicle::GearState::kGearChange && v.clutch() == 0.0);
    for (int i = 0; i < 40; ++i) v.step({0, 0.5, 0, false, 0.0, 0}, flat);
    CHECK(v.gear_state() == drive::Vehicle::GearState::kEngaged && v.clutch() > 0.9);
    // Handbrake in RWD Manual+Clutch disconnects the clutch so engine doesn't fight rear lock.
    for (int i = 0; i < 20; ++i) v.step({0, 0.0, 0, true, 0.0, 0}, flat);
    CHECK(v.clutch() < 0.05);

    // Auto-Brake works directly on Vehicle::step without any race-only requirement.
    drive::Vehicle v_ab(p);
    drive::VehicleAssists a_ab;
    a_ab.braking = 0;  // Assisted
    v_ab.set_assists(a_ab);
    v_ab.launch({0, 0, 0}, 0, 35.0);
    drive::Input ab_in{0, 1.0, 0.0, false};
    ab_in.has_auto_brake_target = true;
    ab_in.auto_brake_target_speed = drive::auto_brake_target_for_curvature(0.05);  // ~13 m/s corner
    for (int i = 0; i < 60; ++i) v_ab.step(ab_in, flat);
    CHECK(v_ab.speed() < 32.0);
  }

  // 5. 3D OBB Contact: jumping over another car produces zero phantom mid-air collision.
  {
    auto p = make_synth();
    drive::Vehicle low_car(p), high_car(p);
    low_car.launch({0, 0, 0}, 0, 10.0);
    high_car.launch({0, 3.0, 0}, 0, 10.0);  // 3 m directly above low_car
    double imp = -1.0;
    CHECK(!drive::car_contact(low_car, 0.2, high_car, 0.2, {}, &imp));
    std::vector<drive::ContactBody> bodies{{&low_car, 0.2, 0}, {&high_car, 0.2, 0}};
    CHECK(drive::resolve_contacts(bodies, {}) == 0);
    // Same-elevation overlap still collides normally.
    high_car.launch({0.5, 0.0, 1.0}, 0, -5.0);
    CHECK(drive::resolve_contacts(bodies, {}) == 1);
  }

  // 6. DualSense: lateral slip included in telemetry & reverse pedal swap (L2=gas, R2=brake).
  {
    auto p = make_synth();
    drive::Vehicle v(p);
    v.launch({0, 0, 0}, 0, 28.0);
    // Hard cornering without handbrake/wheelspin generates pure lateral slip.
    for (int i = 0; i < 40; ++i) v.step({0.8, 0.1, 0.0, false}, flat);
    const auto tel = drive::dualsense::telemetry_from_vehicle(v);
    CHECK(tel.valid && (tel.front_slip > 0.3f || tel.rear_slip > 0.3f));

    drive::dualsense::TriggerModel tm;
    drive::dualsense::Settings ds_cfg;
    ds_cfg.trigger_mode = drive::dualsense::TriggerMode::kFull;
    ds_cfg.trigger_strength_pct = 100;
    drive::dualsense::CarTelemetry rev_car;
    rev_car.valid = true;
    rev_car.gear = -1;
    rev_car.driven_rear = true;
    rev_car.abs_active = true;
    rev_car.rear_slip = 2.5f;  // spinning while pressing L2 in reverse
    drive::dualsense::TriggerInputs rev_in;
    rev_in.driving = true;
    rev_in.throttle = 0.8f;  // R2 = brake in reverse!
    rev_in.brake = 0.9f;     // L2 = throttle in reverse!
    rev_in.telemetry = &rev_car;
    drive::dualsense::TriggerEffects fx{};
    for (int i = 0; i < 5; ++i) fx = tm.step(rev_in, ds_cfg, 1.0 / 60.0);
    // R2 receives ABS Machine pulse (family 6), L2 receives wheelspin Vibration (family 3).
    CHECK(drive::dualsense::reported_family(fx.right) == 6);
    CHECK(drive::dualsense::reported_family(fx.left) == 3);
  }

  // 7. R3-PHY-17: Per-axle ride height lowering/rake & roll-induced dynamic camber modulation.
  {
    cars::CarPhysics cp;
    cars::PhysicsSettings ps;
    cp.values["List_UpgradeCarBodyWeight.Mass"] = 1400.0;
    cp.values["List_UpgradeCarBodyWeight.CMHeight"] = 0.48;
    cp.values["FrontSpringDamper.DefRideHeight"] = 0.16;
    cp.values["FrontSpringDamper.MinRideHeight"] = 0.10;
    cp.values["FrontSpringDamper.MaxRideHeight"] = 0.22;
    cp.values["FrontSpringDamper.MaxCompressHeight"] = 0.06;
    cp.values["RearSpringDamper.DefRideHeight"] = 0.16;
    cp.values["RearSpringDamper.MinRideHeight"] = 0.10;
    cp.values["RearSpringDamper.MaxRideHeight"] = 0.22;
    cp.values["RearSpringDamper.MaxCompressHeight"] = 0.06;

    drive::CarParams p_stock = make_synth();
    drive::apply_physics(cp, ps, p_stock);
    drive::CarParams p_lower = p_stock;
    drive::CarTune t_lower;
    t_lower.front_ride_height = 0.0;  // lowest front (-0.06 m)
    t_lower.rear_ride_height = 0.0;   // lowest rear (-0.06 m)
    drive::apply_tune(t_lower, cp, p_lower);
    CHECK(p_lower.ride_height_delta[0] < -0.04 && p_lower.ride_height_delta[1] < -0.04);

    drive::CarParams p_rake = p_stock;
    drive::CarTune t_rake;
    t_rake.front_ride_height = 0.0;   // lowered front (-0.06 m)
    t_rake.rear_ride_height = 1.0;    // raised rear (+0.06 m)
    drive::apply_tune(t_rake, cp, p_rake);
    CHECK(p_rake.ride_height_delta[0] < -0.04 && p_rake.ride_height_delta[1] > 0.04);

    drive::Vehicle v_stock(p_stock), v_lower(p_lower), v_rake(p_rake);
    for (int i = 0; i < 360; ++i) {
      v_stock.step({}, flat);
      v_lower.step({}, flat);
      v_rake.step({}, flat);
    }
    // Lowered car physically settles lower than stock with all wheels grounded.
    CHECK(v_lower.position().y < v_stock.position().y - 0.03);
    for (const auto& w : v_lower.wheels()) CHECK(w.contact);
    // Positive rake physically pitches nose down (forward.y < -0.015).
    const auto fwd_rake = drive::rotate(v_rake.orientation(), {0, 0, 1});
    const auto fwd_stock = drive::rotate(v_stock.orientation(), {0, 0, 1});
    CHECK(fwd_rake.y < fwd_stock.y - 0.015);
    for (const auto& w : v_rake.wheels()) CHECK(w.contact);

    // Dynamic roll camber: with identical static lat_scale, negative camber (-2.5 deg)
    // produces stronger sustained cornering grip under body roll than positive camber (+2.5 deg).
    drive::CarParams p_neg = make_synth(), p_pos = make_synth();
    p_neg.static_camber_deg = {-2.5, -2.0};
    p_pos.static_camber_deg = {2.5, 2.0};
    p_neg.antiroll_rate = {6000.0, 4500.0};  // allow visible body roll
    p_pos.antiroll_rate = {6000.0, 4500.0};
    drive::Vehicle v_neg(p_neg), v_pos(p_pos);
    v_neg.set_ai_controls(true);
    v_pos.set_ai_controls(true);
    v_neg.launch({0, 0, 0}, 0.0, 26.0);
    v_pos.launch({0, 0, 0}, 0.0, 26.0);
    double lat_g_neg = 0.0, lat_g_pos = 0.0;
    for (int i = 0; i < 180; ++i) {
      v_neg.step({0.18, 0.35, 0.0, false}, flat);
      v_pos.step({0.18, 0.35, 0.0, false}, flat);
      if (i >= 90) {
        lat_g_neg += std::fabs(v_neg.lateral_g());
        lat_g_pos += std::fabs(v_pos.lateral_g());
      }
    }
    CHECK(lat_g_neg > lat_g_pos * 1.015);
  }
}

}  // namespace

// FEEL-10: each car's TCS slip targets from its own gamedb row, through the
// engine's car-parameter resolution (the R34 and two cars with other
// AutoSteerOverrideIDs).
void per_car_assists(const cars::Install& install, const cars::PhysicsSettings& settings) {
  std::vector<cars::CarPhysics> rows;
  std::string error;
  const std::vector<int64_t> ids = {445, 282, 249};  // R34, Beetle 04, 250 GTO
  if (!cars::load_car_physics(install, ids, rows, &error) || rows.size() != ids.size()) {
    std::printf("FAIL per-car assists: %s\n", error.c_str());
    ++failures;
    return;
  }
  for (const auto& car : rows) {
    drive::CarParams p;
    drive::geometry_from_physics(car, p);
    drive::apply_physics(car, settings, p);
    CHECK(car.has("Data_Car.AssistsTCSSlipDefTakeoff") && car.has("Data_Car.AssistsTCSSlipDefMoving"));
    CHECK(p.tcs_slip_takeoff == car.get("Data_Car.AssistsTCSSlipDefTakeoff", -1));
    CHECK(p.tcs_slip_moving == car.get("Data_Car.AssistsTCSSlipDefMoving", -1));
    std::printf("car %lld %s: TCS slip takeoff %.2f moving %.2f\n", static_cast<long long>(car.car_id),
                car.media_name.c_str(), p.tcs_slip_takeoff, p.tcs_slip_moving);
  }
  // The R34's row (SPEC-CAR-PHYSICS, Traction control; gamedb Data_Car 445).
  CHECK(rows[0].get("Data_Car.AssistsTCSSlipDefTakeoff", -1) == 33.0);
  CHECK(rows[0].get("Data_Car.AssistsTCSSlipDefMoving", -1) == 0.5);
}

int main() {
  synthetic_r2_4_checks();
  if (failures) {
    std::printf("physics_tests: %d synthetic failure(s)\n", failures);
    return 1;
  }
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
  per_car_assists(install, settings);
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
