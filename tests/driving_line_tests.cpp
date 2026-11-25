// Unit tests for drive::build_driving_line (SPEC-CAR-PHYSICS, "3D Driving / Braking Line").
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "drive/ai_driver.h"
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
  // Per-car brake curve mode must use Vehicle::brake_max_decel() even when fallback torques are 0.
  auto per_car = car;
  per_car.use_per_car_brakes = true;
  per_car.brake_torque_slider = 0.5;
  per_car.front_brake_torque = 0.0;
  per_car.rear_brake_torque = 0.0;
  const auto tbl_pc = drive::BrakingTable::build(per_car);
  CHECK(tbl_pc.decel_at(20.0) > 6.0);
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

void test_dirt_mixed_surface_ai_speed_planning() {
  const auto car = test_car();
  const auto cap = drive::ai_capacity(car);
  drive::RacingLine asphalt = make_corner_line();
  drive::RacingLine dirt = make_corner_line();
  // Empty surface_grip defaults to 1.0f everywhere.
  CHECK(near(asphalt.grip_at(0), 1.0, 1e-6));
  // Dirt surface grip 0.64 -> sqrt(0.64) = 0.80 planned corner speed scaling.
  dirt.surface_grip.assign(dirt.size(), 0.64f);
  CHECK(near(dirt.grip_at(50), 0.64, 1e-5));

  // 1. Capacity-table speed planner (set_planner): corner speed scales by sqrt(0.64) = 0.80
  //    while straight-away top speed remains uncapped.
  drive::AiField f_asph(asphalt, 1), f_dirt(dirt, 1);
  f_asph.set_planner(0, &cap, 1.0f, 1.0f);
  f_dirt.set_planner(0, &cap, 1.0f, 1.0f);
  size_t apex_idx = 0;
  double max_k = 0.0;
  for (size_t j = 0; j < asphalt.size(); ++j) {
    if (std::fabs(asphalt.curvature[j]) > max_k) {
      max_k = std::fabs(asphalt.curvature[j]);
      apex_idx = j;
    }
  }
  CHECK(max_k > 0.01);
  const double vc_asph = f_asph.corner_speed(0, apex_idx);
  const double vc_dirt = f_dirt.corner_speed(0, apex_idx);
  CHECK(near(vc_dirt / vc_asph, 0.80, 0.02));
  // Straight point far before braking zone stays at max_speed on both.
  CHECK(near(f_dirt.corner_speed(0, 5), f_asph.corner_speed(0, 5), 1e-3));
  // Planned speed approaching apex is strictly lower on dirt.
  CHECK(f_dirt.planned_speed(0, 190.0) < f_asph.planned_speed(0, 190.0) * 0.90);

  // 2. Radius-limit planner (set_limits): scales corner speed by sqrt(surface_grip).
  asphalt.radius.assign(asphalt.size(), 1000.0f);
  dirt.radius.assign(dirt.size(), 1000.0f);
  for (size_t j = 0; j < asphalt.size(); ++j) {
    if (std::fabs(asphalt.curvature[j]) > 1e-4f) {
      asphalt.radius[j] = 1.0f / std::fabs(asphalt.curvature[j]);
      dirt.radius[j] = 1.0f / std::fabs(dirt.curvature[j]);
    }
  }
  drive::AiField l_asph(asphalt, 1), l_dirt(dirt, 1);
  l_asph.set_limits(0, 10.0f, 12.0f);
  l_dirt.set_limits(0, 10.0f, 12.0f);
  const double pl_asph = l_asph.planned_speed(0, 200.0);
  const double pl_dirt = l_dirt.planned_speed(0, 200.0);
  CHECK(near(pl_dirt / pl_asph, 0.80, 0.03));

  // 3. Fallback line-speed path scales by sqrt(surface_grip).
  drive::AiField s_asph(asphalt, 1), s_dirt(dirt, 1);
  const double ps_asph = s_asph.planned_speed(0, 210.0);
  const double ps_dirt = s_dirt.planned_speed(0, 210.0);
  CHECK(near(ps_dirt / ps_asph, 0.80, 0.01));
}

void test_smooth_gradient_entry_and_exit() {
  const auto car = test_car();
  const auto tbl = drive::BrakingTable::build(car);
  const auto line = make_corner_line();

  // Compute braking-heat scalar in [0, 1] matching world_frag.glsl reconstruction:
  // 0 = pure green throttle, 0.5 = neutral yellow coast, 1 = pure red braking.
  auto heat_of = [](const drive::DrivingLineColor& c) {
    const double r = double(c.r) / 255.0;
    const double g = double(c.g) / 255.0;
    const double t_lo = std::clamp((r - 44.0 / 255.0) / ((192.0 - 44.0) / 255.0), 0.0, 1.0);
    const double g_exp = (147.0 + (164.0 - 147.0) * t_lo) / 255.0;
    const double t_hi = std::clamp((g_exp - g) / ((164.0 - 39.0) / 255.0), 0.0, 1.0);
    return 0.5 * (t_lo + t_hi);
  };

  // Drive through entry, apex, and exit of the 25 m corner at 25 m/s so the line covers
  // Green -> Yellow -> Red on entry AND Red -> Yellow -> Green on corner exit.
  drive::DrivingLineOptions opt;
  opt.fade_ends = false;
  opt.ahead_m = 200.0;
  std::vector<drive::DrivingLineSample> samples;
  drive::build_driving_line(line, tbl, 140.0, 25.0, drive::DrivingLineMode::kFull, samples, opt);
  CHECK(samples.size() > 80);

  int max_rgb_delta = 0;
  bool saw_entry_mid = false;
  bool saw_exit_mid = false;
  size_t peak_idx = 0;
  double peak_heat = -1.0;
  for (size_t i = 0; i < samples.size(); ++i) {
    const double h = heat_of(samples[i].color);
    if (h > peak_heat) {
      peak_heat = h;
      peak_idx = i;
    }
    if (i > 0) {
      const auto& a = samples[i - 1].color;
      const auto& b = samples[i].color;
      const int dr = std::abs(int(b.r) - int(a.r));
      const int dg = std::abs(int(b.g) - int(a.g));
      const int db = std::abs(int(b.b) - int(a.b));
      max_rgb_delta = std::max(max_rgb_delta, std::max(dr, std::max(dg, db)));
    }
  }
  CHECK(peak_heat > 0.95);
  // Bounded per-step RGB delta guarantees smooth transitions with zero abrupt binary jumps.
  CHECK(max_rgb_delta <= 32);

  // Entry heat is monotonically non-decreasing into the braking apex (within 1-step tolerance),
  // and passes through intermediate yellow/amber tones before reaching red.
  for (size_t i = 1; i <= peak_idx; ++i) {
    const double h0 = heat_of(samples[i - 1].color);
    const double h1 = heat_of(samples[i].color);
    CHECK(h1 + 0.02 >= h0);
    if (h1 > 0.25 && h1 < 0.75) saw_entry_mid = true;
  }
  // Exit heat unwinds monotonically back toward green through intermediate yellow/amber tones.
  for (size_t i = peak_idx + 1; i < samples.size(); ++i) {
    const double h0 = heat_of(samples[i - 1].color);
    const double h1 = heat_of(samples[i].color);
    CHECK(h1 <= h0 + 0.02);
    if (h1 > 0.25 && h1 < 0.75) saw_exit_mid = true;
  }
  CHECK(saw_entry_mid);
  CHECK(saw_exit_mid);
}

void test_near_far_fades_width_and_braking_only() {
  const auto car = test_car();
  const auto tbl = drive::BrakingTable::build(car);
  const auto line = make_corner_line();

  // Sleek retail FH1 default proportions: narrow ribbon, starts ahead of car (nothing behind).
  drive::DrivingLineOptions opt;
  CHECK(opt.half_width_m >= 0.32 && opt.half_width_m <= 0.44);
  CHECK(opt.behind_m <= 1e-6);
  CHECK(opt.near_fade_start_m >= 2.5 && opt.near_fade_end_m > opt.near_fade_start_m);
  CHECK(opt.far_fade_m >= 25.0);

  const double car_dist = 60.0;
  std::vector<drive::DrivingLineSample> samples;
  drive::build_driving_line(line, tbl, car_dist, 35.0, drive::DrivingLineMode::kFull, samples, opt);
  CHECK(samples.size() > 80);

  // Near-car clearance: samples at/under the car (ahead <= near_fade_start_m) stay invisible,
  // then ramp monotonically to full opacity by near_fade_end_m.
  CHECK(samples.front().color.a == 0);
  uint8_t prev_near_a = 0;
  bool reached_full = false;
  for (const auto& s : samples) {
    const double ahead = double(s.dist) - car_dist;
    if (ahead <= opt.near_fade_start_m + 1e-3) {
      CHECK(s.color.a == 0);
    }
    if (ahead <= opt.near_fade_end_m + opt.sample_step_m) {
      CHECK(s.color.a >= prev_near_a);
      prev_near_a = s.color.a;
    }
    if (ahead > opt.near_fade_end_m + 2.0 && ahead < opt.ahead_m - opt.far_fade_m - 2.0) {
      if (s.color.a == 255) reached_full = true;
    }
  }
  CHECK(reached_full);

  // Far-horizon fade-out: monotonic decay to 0 at visible_end_d.
  CHECK(samples.back().color.a == 0);
  const double visible_end = std::min(line.length(), car_dist + opt.ahead_m);
  uint8_t prev_far_a = 255;
  for (const auto& s : samples) {
    if (visible_end - double(s.dist) <= opt.far_fade_m) {
      CHECK(s.color.a <= prev_far_a);
      prev_far_a = s.color.a;
    }
  }

  // Built ribbon mesh uses sleek width (~2 * half_width_m) and starts ahead of car bumper.
  std::vector<drive::DrivingLineVertex> mesh;
  drive::build_driving_line_mesh(samples, opt.half_width_m, opt.lift_m, mesh);
  CHECK(mesh.size() >= 6 && mesh.size() % 6 == 0);
  CHECK(mesh[0].z >= float(car_dist + opt.near_fade_start_m - opt.sample_step_m - 0.1));
  const double quad_width = std::hypot(double(mesh[1].x - mesh[0].x), double(mesh[1].z - mesh[0].z));
  CHECK(near(quad_width, 2.0 * opt.half_width_m, 1e-2));

  // Braking Only with fade_ends = true hides pure throttle segments and glides smoothly
  // into approaching braking zones without abrupt 0 -> 255 alpha pops.
  drive::DrivingLineOptions bo_opt = opt;
  bo_opt.ahead_m = 190.0;
  std::vector<drive::DrivingLineSample> bo;
  drive::build_driving_line(line, tbl, 50.0, 40.0, drive::DrivingLineMode::kBrakingOnly, bo, bo_opt);
  CHECK(!bo.empty());
  int max_bo_alpha_step = 0;
  bool saw_bo_hidden = false;
  bool saw_bo_fade_mid = false;
  bool saw_bo_opaque_brake = false;
  const double bo_end = std::min(line.length(), 50.0 + bo_opt.ahead_m);
  for (size_t i = 0; i < bo.size(); ++i) {
    const double ahead = double(bo[i].dist) - 50.0;
    const double to_end = bo_end - double(bo[i].dist);
    if (ahead > 10.0 && ahead < 40.0 && bo[i].color.a == 0) saw_bo_hidden = true;
    if (ahead > bo_opt.near_fade_end_m && to_end > bo_opt.far_fade_m) {
      if (bo[i].color.a > 20 && bo[i].color.a < 235) saw_bo_fade_mid = true;
      if (bo[i].color.a == 255 && bo[i].color.r == 192 && bo[i].color.g == 39) saw_bo_opaque_brake = true;
      if (i > 0 && double(bo[i - 1].dist) - 50.0 > bo_opt.near_fade_end_m) {
        max_bo_alpha_step = std::max(max_bo_alpha_step, std::abs(int(bo[i].color.a) - int(bo[i - 1].color.a)));
      }
    }
  }
  CHECK(saw_bo_hidden);
  CHECK(saw_bo_fade_mid);
  CHECK(saw_bo_opaque_brake);
  CHECK(max_bo_alpha_step <= 45);
}

void test_temporal_smoother() {
  drive::DrivingLineSmoother sm;
  // First sample initializes directly.
  CHECK(near(sm.update(30.0, 1.0 / 60.0), 30.0, 1e-6));

  // Rapid +-2.0 m/s throttle jitter at 60 Hz is damped so braking boundary cannot flash.
  double min_out = 1e9, max_out = -1e9;
  for (int f = 0; f < 60; ++f) {
    const double jitter = (f % 2 == 0) ? 32.0 : 28.0;
    const double v = sm.update(jitter, 1.0 / 60.0);
    min_out = std::min(min_out, v);
    max_out = std::max(max_out, v);
  }
  CHECK((max_out - min_out) < 0.6);

  // Paused frame (dt == 0) holds exact smoothed speed without snapping.
  const double before_pause = sm.speed_mps;
  CHECK(near(sm.update(35.0, 0.0), before_pause, 1e-6));

  // Accelerating toward a braking zone responds faster than lifting off throttle.
  drive::DrivingLineSmoother sm_up, sm_down;
  sm_up.update(30.0, 0.016);
  sm_down.update(30.0, 0.016);
  const double dv_up = sm_up.update(40.0, 0.05) - 30.0;
  const double dv_down = 30.0 - sm_down.update(20.0, 0.05);
  CHECK(dv_up > dv_down);

  // Large teleport / respawn delta snaps immediately.
  CHECK(near(sm.update(0.0, 1.0 / 60.0), 0.0, 1e-6));
}

}  // namespace

void test_auto_brake_target() {
  // FEEL-14: Assisted braking's target is the driving line's, from the car's
  // own braking table, not a fixed 0.85 g / 7.5 m/s^2 corner rule.
  const auto car = test_car();
  const auto tbl = drive::BrakingTable::build(car);
  auto weak = tbl;
  for (double& d : weak.decel_mps2) d *= 0.5;
  const auto line = make_corner_line();
  double v_strong = 0, v_weak = 0, v_far = 0;
  CHECK(drive::auto_brake_target(line, tbl, 150.0, 40.0, v_strong));
  CHECK(drive::auto_brake_target(line, weak, 150.0, 40.0, v_weak));
  CHECK(drive::auto_brake_target(line, tbl, 10.0, 40.0, v_far));
  std::printf("auto-brake target 50 m before the corner: %.2f m/s (table), %.2f m/s (half decel); 190 m: %.2f\n",
              v_strong, v_weak, v_far);
  CHECK(v_weak < v_strong - 2.0);  // a weaker-braking car is asked to slow sooner
  CHECK(v_far > v_strong);
  // At the corner the target is the corner speed, whatever the table.
  double v_apex = 0, v_apex_weak = 0;
  CHECK(drive::auto_brake_target(line, tbl, 215.0, 20.0, v_apex));
  CHECK(drive::auto_brake_target(line, weak, 215.0, 20.0, v_apex_weak));
  CHECK(std::fabs(v_apex - v_apex_weak) < 1.0 && v_apex < 25.0);
  // The free-roam form: the same path given as points from the car.
  std::vector<float> xs, zs;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line.dist[i] < 150.0) continue;
    xs.push_back(line.x[i]);
    zs.push_back(line.z[i]);
  }
  double v_path = 0;
  CHECK(drive::auto_brake_target_on_path(xs, zs, tbl, 40.0, v_path));
  CHECK(std::fabs(v_path - v_strong) < 3.0);
  std::vector<float> one{0.0f};
  CHECK(!drive::auto_brake_target_on_path(one, one, tbl, 40.0, v_path));
}

int main() {
  test_color_band();
  test_braking_table();
  test_backward_braking_and_mesh();
  test_dirt_mixed_surface_ai_speed_planning();
  test_smooth_gradient_entry_and_exit();
  test_near_far_fades_width_and_braking_only();
  test_temporal_smoother();
  test_auto_brake_target();
  if (failures) {
    std::printf("driving_line_tests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("driving_line_tests: all passed\n");
  return 0;
}
