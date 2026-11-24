#include "garage/pi_calc.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

#include "drive/from_physics.h"
#include "fh1data/archive.h"
#include "garage/garage.h"

namespace garage {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kMphToMs = 0.44704;
constexpr double kKgf = 9.80665;
constexpr double kMinCurvature = 0.001;
constexpr double kAeroK = 9.80665 / (67.056 * 67.056);

// 26-bin speed table (v = 5..130 m/s, dv = 5 m/s).
constexpr int kSpeedBins = 26;
constexpr double kV0 = 5.0;
constexpr double kDv = 5.0;
constexpr double kStepDt = 0.1;  // integration step on straights & corners

double lerp(double a, double b, double t) { return a + (b - a) * std::clamp(t, 0.0, 1.0); }

double ramp(double x, double x0, double x1) {
  if (x1 == x0) return x >= x1 ? 1.0 : 0.0;
  return std::clamp((x - x0) / (x1 - x0), 0.0, 1.0);
}

bool xml_attr(std::string_view tag, std::string_view key, double& out) {
  const std::string needle = std::string(key) + "=\"";
  size_t pos = 0;
  while ((pos = tag.find(needle, pos)) != std::string_view::npos) {
    if (pos == 0 || std::isspace(static_cast<unsigned char>(tag[pos - 1]))) {
      const size_t start = pos + needle.size();
      const size_t end = tag.find('"', start);
      if (end == std::string_view::npos) return false;
      std::string val(tag.substr(start, end - start));
      char* stop = nullptr;
      out = std::strtod(val.c_str(), &stop);
      return stop != val.c_str();
    }
    ++pos;
  }
  return false;
}

// Engine WOT torque (N*m) at `rpm` with full boost (for PI & power stats).
double engine_wot_torque(const drive::CarParams& p, double rpm) {
  const double r = std::clamp(rpm, std::max(500.0, p.idle_rpm), p.redline_rpm);
  double base = 1.0;
  if (p.torque_curve.empty()) {
    base = r < 2000.0 ? 0.6 + 0.4 * r / 2000.0 : r > 5500.0 ? 1.0 - 0.3 * ramp(r, 5500.0, p.redline_rpm) : 1.0;
  } else {
    const double slots = p.torque_curve_slots > 0
                             ? p.torque_curve_slots
                             : (p.torque_curve.size() > 1 ? double(p.torque_curve.size() - 1) : 1.0);
    const double x = std::max(0.0, r / std::max(1.0, p.torque_curve_max_rpm / slots));
    const size_t last = p.torque_curve.size() - 1;
    if (x >= double(last)) {
      base = p.torque_curve[last];
    } else {
      const size_t idx = size_t(x);
      base = lerp(p.torque_curve[idx], p.torque_curve[idx + 1], x - double(idx));
    }
  }
  const double base_tq = base * p.torque_scale;
  double f_b = p.has_turbo ? p.boost_scale_max : 1.0;
  if (f_b > 1.0 && p.turbo_dropoff_rpm0 != p.turbo_dropoff_rpm1) {
    const double u = ramp(r, std::min(p.turbo_dropoff_rpm0, p.turbo_dropoff_rpm1),
                          std::max(p.turbo_dropoff_rpm0, p.turbo_dropoff_rpm1));
    f_b = lerp(p.turbo_dropoff_scale0 * (f_b - 1.0) + 1.0, p.turbo_dropoff_scale1 * (f_b - 1.0) + 1.0, u);
  }
  double f_s = p.has_supercharger ? lerp(p.sc_scale_zero, p.sc_scale_redline, ramp(r, 0.0, p.redline_rpm)) : 1.0;
  if (f_s > 1.0 && p.sc_dropoff_rpm0 != p.sc_dropoff_rpm1) {
    const double u = ramp(r, std::min(p.sc_dropoff_rpm0, p.sc_dropoff_rpm1),
                          std::max(p.sc_dropoff_rpm0, p.sc_dropoff_rpm1));
    f_s = lerp(p.sc_dropoff_scale0 * (f_s - 1.0) + 1.0, p.sc_dropoff_scale1 * (f_s - 1.0) + 1.0, u);
  }
  return base_tq * (1.0 + (f_b - 1.0) + (f_s - 1.0)) * p.game_torque_scale;
}

double aero_drag_force(const drive::CarParams& p, double v) {
  const double v2 = v * v;
  double drag_c = p.aero_drag_long > 0
                      ? (p.aero_drag_long + p.aero_part_drag_front + p.aero_part_drag_rear)
                      : p.drag_quadratic;
  if (v > p.high_speed_drag_v0 && p.high_speed_drag_v1 > p.high_speed_drag_v0) {
    drag_c *= lerp(p.high_speed_drag_s0, p.high_speed_drag_s1, ramp(v, p.high_speed_drag_v0, p.high_speed_drag_v1));
  }
  return drag_c * v2 + (p.aero_drag_long > 0 ? 0.0 : p.drag_constant);
}

// Steady-state lateral acceleration capacity (m/s^2) at speed v.
double max_lat_accel(const drive::CarParams& p, double v) {
  const double z_f = 0.5 * (p.wheel_centres[0].z + p.wheel_centres[1].z);
  const double z_r = 0.5 * (p.wheel_centres[2].z + p.wheel_centres[3].z);
  const double L = std::max(0.5, std::fabs(z_f - z_r));
  const double front_share = std::clamp((p.centre_of_mass.z - z_r) / L, 0.15, 0.85);
  const double track_f = std::max(0.8, std::fabs(p.wheel_centres[1].x - p.wheel_centres[0].x));
  const double track_r = std::max(0.8, std::fabs(p.wheel_centres[3].x - p.wheel_centres[2].x));
  const double h_roll = std::max(0.20, 0.70 * p.centre_of_mass.y);
  const double df_f = p.aero_df_front + p.aero_part_df_front;
  const double df_r = p.aero_df_rear + p.aero_part_df_rear;

  const double nf = std::max(1.0, p.mass * p.gravity * front_share + df_f * v * v);
  const double nr = std::max(1.0, p.mass * p.gravity * (1.0 - front_share) + df_r * v * v);
  const double k_roll_f = std::max(1.0, p.spring_rate[0] + p.antiroll_rate[0]);
  const double k_roll_r = std::max(1.0, p.spring_rate[1] + p.antiroll_rate[1]);
  const double roll_f = std::clamp(k_roll_f / (k_roll_f + k_roll_r), 0.25, 0.75);

  double a = 10.0;
  for (int it = 0; it < 5; ++it) {
    const double d_f = p.mass * a * h_roll * roll_f / track_f;
    const double d_r = p.mass * a * h_roll * (1.0 - roll_f) / track_r;
    const double n_fl = std::max(1.0, 0.5 * nf - d_f);
    const double n_fr = std::max(1.0, 0.5 * nf + d_f);
    const double n_rl = std::max(1.0, 0.5 * nr - d_r);
    const double n_rr = std::max(1.0, 0.5 * nr + d_r);
    const double fy_f = n_fl * drive::tyre_peak_mu(p, 0, drive::TyrePeak::kLateral, n_fl, v) +
                        n_fr * drive::tyre_peak_mu(p, 0, drive::TyrePeak::kLateral, n_fr, v);
    const double fy_r = n_rl * drive::tyre_peak_mu(p, 1, drive::TyrePeak::kLateral, n_rl, v) +
                        n_rr * drive::tyre_peak_mu(p, 1, drive::TyrePeak::kLateral, n_rr, v);
    const double a_f = fy_f / std::max(1.0, p.mass * front_share);
    const double a_r = fy_r / std::max(1.0, p.mass * (1.0 - front_share));
    const double a_bal = std::min(a_f, a_r);
    const double a_tot = (fy_f + fy_r) / std::max(1.0, p.mass);
    a = 0.50 * a_bal + 0.50 * a_tot;
  }
  return std::max(1.0, a);
}

// Braking deceleration capacity (m/s^2) at speed v (tyres + aero downforce + aero drag).
double max_brake_decel(const drive::CarParams& p, double v) {
  const double z_f = 0.5 * (p.wheel_centres[0].z + p.wheel_centres[1].z);
  const double z_r = 0.5 * (p.wheel_centres[2].z + p.wheel_centres[3].z);
  const double L = std::max(0.5, std::fabs(z_f - z_r));
  const double front_share = std::clamp((p.centre_of_mass.z - z_r) / L, 0.15, 0.85);
  const double h_cg = std::max(0.15, p.centre_of_mass.y);
  const double df_f = p.aero_df_front + p.aero_part_df_front;
  const double df_r = p.aero_df_rear + p.aero_part_df_rear;
  const double nf0 = p.mass * p.gravity * front_share + df_f * v * v;
  const double nr0 = p.mass * p.gravity * (1.0 - front_share) + df_r * v * v;
  const double f_drag = aero_drag_force(p, v);
  double a = 10.0;
  for (int it = 0; it < 6; ++it) {
    const double dn = p.mass * a * h_cg / L;
    const double nf = std::max(1.0, nf0 + dn);
    const double nr = std::max(1.0, nr0 - dn);
    const double ff = nf * drive::tyre_peak_mu(p, 0, drive::TyrePeak::kBraking, 0.5 * nf, v);
    const double fr = nr * drive::tyre_peak_mu(p, 1, drive::TyrePeak::kBraking, 0.5 * nr, v);
    a = (ff + fr + f_drag) / std::max(1.0, p.mass);
  }
  return std::max(1.0, a);
}

// Longitudinal forward acceleration capacity (m/s^2) at speed v, using either the
// PI.xml reference gear redline speeds or the car's own gears, with drive-type
// grip scale `grip_scale` (1.0 straight AWD, etc.).
double max_long_accel(const drive::CarParams& p, double v, double grip_scale,
                      const std::vector<double>* ref_redline_ms) {
  const double z_f = 0.5 * (p.wheel_centres[0].z + p.wheel_centres[1].z);
  const double z_r = 0.5 * (p.wheel_centres[2].z + p.wheel_centres[3].z);
  const double L = std::max(0.5, std::fabs(z_f - z_r));
  const double front_share = std::clamp((p.centre_of_mass.z - z_r) / L, 0.15, 0.85);
  const double h_cg = std::max(0.15, p.centre_of_mass.y);
  const double df_f = p.aero_df_front + p.aero_part_df_front;
  const double df_r = p.aero_df_rear + p.aero_part_df_rear;
  const double r_f = std::max(0.15, p.tyre_radius[0]);
  const double r_r = std::max(0.15, p.tyre_radius[2]);
  const double r_drive = p.layout == drive::CarParams::Layout::kFront ? r_f : r_r;
  const double redline_rad = p.redline_rpm * (2.0 * kPi / 60.0);

  // Best available wheel drive force across gears (selecting gear where v <= v_redline).
  double best_f_eng = 0.0;
  double best_ratio = 0.0;
  auto test_gear = [&](double total_ratio) {
    if (total_ratio <= 0.1) return;
    const double omega_w = std::max(0.1, v) / r_drive;
    const double rpm = omega_w * total_ratio * (60.0 / (2.0 * kPi));
    if (rpm > p.redline_rpm * 1.005) return;
    const double eff_rpm = std::max(rpm, std::max(p.idle_rpm, p.launch_rpm * 0.6));
    const double tq = engine_wot_torque(p, eff_rpm);
    const double f_w = tq * total_ratio / r_drive;
    if (f_w > best_f_eng) {
      best_f_eng = f_w;
      best_ratio = total_ratio;
    }
  };

  if (ref_redline_ms && !ref_redline_ms->empty()) {
    for (double v_red : *ref_redline_ms) {
      if (v_red <= 1.0) continue;
      // Ratio placing engine redline at v_red.
      const double total_ratio = (redline_rad * r_drive) / v_red;
      test_gear(total_ratio);
    }
  } else {
    for (double g : p.gears) test_gear(g * p.final_drive);
  }

  // Rotational inertia equivalent mass (wheels + driveline + engine*G^2).
  const double j_wheels = 4.0 * p.wheel_inertia / (r_drive * r_drive);
  const double j_eng = (p.engine_inertia + p.gearbox_inertia + p.driveline_inertia) *
                       (best_ratio * best_ratio) / (r_drive * r_drive);
  const double m_eff = p.mass + j_wheels + std::min(p.mass * 0.35, j_eng);
  const double f_drag = aero_drag_force(p, v);

  // Traction limit with longitudinal weight transfer.
  const double nf0 = p.mass * p.gravity * front_share + df_f * v * v;
  const double nr0 = p.mass * p.gravity * (1.0 - front_share) + df_r * v * v;
  double a = 0.0;
  for (int it = 0; it < 4; ++it) {
    const double dn = p.mass * std::max(0.0, a) * h_cg / L;
    const double nf = std::max(1.0, nf0 - dn);
    const double nr = std::max(1.0, nr0 + dn);
    const double ff = nf * drive::tyre_peak_mu(p, 0, drive::TyrePeak::kDriving, 0.5 * nf, v) * grip_scale;
    const double fr = nr * drive::tyre_peak_mu(p, 1, drive::TyrePeak::kDriving, 0.5 * nr, v) * grip_scale;
    double f_trac = 0.0;
    if (p.layout == drive::CarParams::Layout::kFront) {
      f_trac = ff;
    } else if (p.layout == drive::CarParams::Layout::kRear) {
      f_trac = fr;
    } else {
      // AWD: torque split limits each axle, plus centre diff transfer.
      const double split_r = std::clamp(p.rear_split, 0.1, 0.9);
      const double f_by_split = std::min(ff / (1.0 - split_r), fr / split_r);
      f_trac = std::min(ff + fr, 0.5 * f_by_split + 0.5 * (ff + fr));
    }
    const double f_net = std::min(best_f_eng, f_trac) - f_drag;
    a = f_net / std::max(1.0, m_eff);
  }
  return a;
}

struct SpeedBin {
  double v = 0.0;
  double a_lat = 1.0;
  double min_radius = 0.0;
  double a_brake = 1.0;
  double brake_cum_dx = 0.0;
  double brake_dt = 0.0;
  double brake_dx = 0.0;
  double a_accel = 0.0;
};

struct SpeedTable {
  SpeedBin bins[kSpeedBins];

  void lookup(double v, int& i0, int& i1, double& frac) const {
    const double vc = std::clamp(v, kV0, kV0 + double(kSpeedBins - 1) * kDv);
    const double u = (vc - kV0) / kDv;
    i0 = std::clamp(int(std::floor(u)), 0, kSpeedBins - 2);
    i1 = i0 + 1;
    frac = std::clamp(u - double(i0), 0.0, 1.0);
  }

  double accel_at(double v) const {
    int i0 = 0, i1 = 1;
    double t = 0.0;
    lookup(v, i0, i1, t);
    return lerp(bins[i0].a_accel, bins[i1].a_accel, t);
  }

  double lat_at(double v) const {
    int i0 = 0, i1 = 1;
    double t = 0.0;
    lookup(v, i0, i1, t);
    return lerp(bins[i0].a_lat, bins[i1].a_lat, t);
  }

  // Max cornering speed for radius R (inverts min_radius = v^2 / a_lat).
  double corner_speed_for_radius(double radius) const {
    for (int i = kSpeedBins - 1; i >= 1; --i) {
      const double r0 = bins[i - 1].min_radius;
      const double r1 = bins[i].min_radius;
      if ((radius - r1) * (radius - r0) <= 0.0) {
        const double denom = r1 - r0;
        if (std::fabs(denom) < 1e-9) return bins[i].v;
        const double t = std::clamp((radius - r0) / denom, 0.0, 1.0);
        return lerp(bins[i - 1].v, bins[i].v, t);
      }
    }
    if (radius >= bins[kSpeedBins - 1].min_radius) return bins[kSpeedBins - 1].v;
    return std::sqrt(std::max(0.1, radius * bins[0].a_lat));
  }

  double brake_distance(double v_hi, double v_lo) const {
    if (v_hi <= v_lo) return 0.0;
    int a0 = 0, a1 = 1, b0 = 0, b1 = 1;
    double ta = 0.0, tb = 0.0;
    lookup(v_hi, a0, a1, ta);
    lookup(v_lo, b0, b1, tb);
    if (a0 == b0) return std::max(0.0, (ta - tb) * bins[a1].brake_dx);
    double d = ta * bins[a1].brake_dx;
    for (int i = a0; i > b0; --i) d += bins[i].brake_dx;
    d += (1.0 - tb) * bins[b1].brake_dx;
    return std::max(0.0, d);
  }

  double brake_time(double v_hi, double v_lo) const {
    if (v_hi <= v_lo) return 0.0;
    int a0 = 0, a1 = 1, b0 = 0, b1 = 1;
    double ta = 0.0, tb = 0.0;
    lookup(v_hi, a0, a1, ta);
    lookup(v_lo, b0, b1, tb);
    if (a0 == b0) return std::max(0.0, (ta - tb) * bins[a1].brake_dt);
    double t = ta * bins[a1].brake_dt;
    for (int i = a0; i > b0; --i) t += bins[i].brake_dt;
    t += (1.0 - tb) * bins[b1].brake_dt;
    return std::max(0.0, t);
  }
};

double run_single_lap(const drive::CarParams& p, const PiConfig& cfg, bool use_ref_gears) {
  if (cfg.segments.empty()) return cfg.max_pi_time_s;
  const int dt_idx = p.layout == drive::CarParams::Layout::kFront ? 0
                     : p.layout == drive::CarParams::Layout::kAll ? 2 : 1;
  const double straight_grip = cfg.straight_accel_grip[size_t(dt_idx)];
  const double corner_grip = cfg.corner_accel_grip[size_t(dt_idx)];
  const double lap_scale = cfg.lap_time_scale[size_t(dt_idx)];

  // Gear redline speeds in m/s: use PI.xml's 8 reference gears when present,
  // otherwise derive redline speeds from the car's own transmission ratios.
  std::vector<double> redlines_ms;
  if (use_ref_gears && !cfg.gear_redline_mph.empty()) {
    redlines_ms.reserve(cfg.gear_redline_mph.size());
    for (double mph : cfg.gear_redline_mph) redlines_ms.push_back(mph * kMphToMs);
  } else {
    const double r_drive = std::max(0.15, p.layout == drive::CarParams::Layout::kFront ? p.tyre_radius[0] : p.tyre_radius[2]);
    const double redline_rad = p.redline_rpm * (2.0 * kPi / 60.0);
    for (double g : p.gears) {
      const double tr = g * p.final_drive;
      if (tr > 0.1) redlines_ms.push_back(redline_rad * r_drive / tr);
    }
  }
  const std::vector<double>* ref_gears = (use_ref_gears && !redlines_ms.empty()) ? &redlines_ms : nullptr;

  // Shift duration: 0.5 * (clutch_in + clutch_out) + gear_shift_time.
  const double shift_time = 0.5 * (p.clutch_in_time + p.clutch_out_time) + p.gear_shift_time;
  const double coast_drag_ratio = 1.0 / std::max(1.0, p.mass);

  // Build 26-bin speed table.
  SpeedTable tbl;
  for (int i = 0; i < kSpeedBins; ++i) {
    SpeedBin& b = tbl.bins[i];
    b.v = kV0 + double(i) * kDv;
    b.a_lat = max_lat_accel(p, b.v);
    b.min_radius = (b.v * b.v) / std::max(b.a_lat, 0.001);
    b.a_brake = std::max(max_brake_decel(p, b.v), 0.001);
    if (i == 0) {
      b.brake_dt = kV0 / b.a_brake;
      b.brake_dx = std::max(0.0, b.v * b.brake_dt - 0.5 * b.a_brake * b.brake_dt * b.brake_dt);
      b.brake_cum_dx = b.brake_dx;
    } else {
      b.brake_dt = kDv / b.a_brake;
      b.brake_dx = (b.v - 0.5 * kDv) * b.brake_dt;
      b.brake_cum_dx = tbl.bins[i - 1].brake_cum_dx + b.brake_dx;
    }
    b.a_accel = max_long_accel(p, b.v, straight_grip, ref_gears);
  }

  // Precompute max corner apex speed for each segment.
  const size_t nseg = cfg.segments.size();
  std::vector<double> v_corner(nseg, 10.0);
  for (size_t s = 0; s < nseg; ++s) {
    v_corner[s] = std::max(2.0, tbl.corner_speed_for_radius(cfg.segments[s].corner_radius_m));
  }

  // Start lap exiting last corner at sqrt(0.95) * v_corner[last].
  constexpr double kApexFactor = 0.9746794344808963;  // sqrt(0.95)
  double v = kApexFactor * v_corner.back();
  double total_time = 0.0;
  size_t gear = 0;
  auto sync_gear = [&]() {
    gear = 0;
    while (gear + 1 < redlines_ms.size() && v > redlines_ms[gear]) ++gear;
  };
  sync_gear();

  for (size_t s = 0; s < nseg; ++s) {
    const PiSegment& seg = cfg.segments[s];
    const double v_target = kApexFactor * v_corner[s];
    const double straight_len = std::max(10.0, seg.effective_straight_m);

    // --- 1. Straight ---
    double dist = 0.0;
    bool shifting = false;
    double shift_rem = 0.0;
    int guard = 0;
    while (dist < straight_len && guard++ < 4000) {
      const double saved_dist = dist;
      const double saved_v = v;
      const double saved_t = total_time;

      double dt = kStepDt;
      double acc = 0.0;
      if (shifting) {
        dt = std::min(kStepDt, shift_rem);
        acc = -aero_drag_force(p, v) * coast_drag_ratio;
      } else if (gear + 1 < redlines_ms.size() && v > redlines_ms[gear]) {
        shifting = true;
        shift_rem = shift_time;
        dt = std::min(kStepDt, shift_rem);
        acc = -aero_drag_force(p, v) * coast_drag_ratio;
      } else {
        acc = tbl.accel_at(v);
      }

      const double dv = acc * dt;
      const double v_mid = std::max(1.0, v + 0.5 * dv);
      const double v_next = std::max(1.0, v + dv);
      const double dx = v_mid * dt;
      dist += dx;
      v = v_next;
      total_time += dt;

      if (dist > straight_len) {
        const double over = dist - straight_len;
        const double back_dt = std::min(dt, over / std::max(1.0, v_next));
        v = std::max(1.0, v - acc * back_dt);
        total_time -= back_dt;
        dist = straight_len;
      }

      if (shifting) {
        shift_rem -= dt;
        if (shift_rem <= 1e-5) {
          shifting = false;
          shift_rem = 0.0;
          ++gear;
        }
      }

      // Check if we crossed the braking point for the upcoming corner.
      const double rem = straight_len - dist;
      if (v > v_target && tbl.brake_distance(v, v_target) > rem) {
        // 10-iteration bisection on dt within [0, dt].
        double lo = 0.0, hi = dt;
        double best_v = saved_v, best_t = saved_t;
        for (int b = 0; b < 10; ++b) {
          const double mid = 0.5 * (lo + hi);
          const double dv_m = acc * mid;
          const double vm = std::max(1.0, saved_v + dv_m);
          const double dm = saved_dist + std::max(1.0, saved_v + 0.5 * dv_m) * mid;
          const double rem_m = straight_len - dm;
          if (tbl.brake_distance(vm, v_target) <= rem_m) {
            lo = mid;
            best_v = vm;
            best_t = saved_t + mid;
          } else {
            hi = mid;
          }
        }
        total_time = best_t + tbl.brake_time(best_v, v_target);
        v = v_target;
        dist = straight_len;
        shifting = false;
        shift_rem = 0.0;
        sync_gear();
        break;
      }
    }
    v = std::min(v, v_target);

    // --- 2. Corner entry + exit unwind ---
    // Phase 1: entry_arc (constant radius R, target speed sqrt(0.95) * v_corner).
    // Phase 2: exit_arc (curvature unwinds kappa(u) = 1/R + (0.001 - 1/R) * u^p,
    //          accelerating on traction circle using remaining grip * corner_grip).
    for (int phase = 0; phase < 2; ++phase) {
      const double arc_len = phase == 0 ? seg.entry_arc_m : seg.exit_arc_m;
      if (arc_len <= 1e-4) continue;
      double s_pos = 0.0;
      int cguard = 0;
      while (s_pos < arc_len && cguard++ < 4000) {
        double v_cap = v_target;
        double v_max_lat = v_corner[s];
        if (phase == 1) {
          const double u = std::clamp(s_pos / arc_len, 0.0, 1.0);
          const double k0 = 1.0 / std::max(1.0, seg.corner_radius_m);
          const double k = k0 + (kMinCurvature - k0) * std::pow(u, seg.unwind_power);
          const double r_inst = 1.0 / std::max(kMinCurvature, k);
          v_max_lat = std::max(2.0, tbl.corner_speed_for_radius(r_inst));
          v_cap = v_max_lat;
        }
        if (phase == 0 && v >= v_cap - 1e-4) {
          // Cruise remainder of constant-radius entry arc at apex speed.
          const double rem_arc = arc_len - s_pos;
          total_time += rem_arc / std::max(1.0, v);
          s_pos = arc_len;
          break;
        }

        // Traction circle: lateral load fraction q = clamp((v/v_max_lat)^2, 0, 1),
        // available longitudinal grip fraction = max(0, sqrt(1 - q^2) - (1 - corner_grip)).
        const double q = std::clamp((v * v) / std::max(1.0, v_max_lat * v_max_lat), 0.0, 1.0);
        const double avail_grip = std::max(0.0, std::sqrt(std::max(0.0, 1.0 - q * q)) - (1.0 - corner_grip));
        double dt = kStepDt;
        double acc = 0.0;
        if (shifting) {
          dt = std::min(kStepDt, shift_rem);
          acc = -aero_drag_force(p, v) * coast_drag_ratio;
        } else if (gear + 1 < redlines_ms.size() && v > redlines_ms[gear]) {
          shifting = true;
          shift_rem = shift_time;
          dt = std::min(kStepDt, shift_rem);
          acc = -aero_drag_force(p, v) * coast_drag_ratio;
        } else {
          acc = std::max(0.0, max_long_accel(p, v, avail_grip, ref_gears));
        }
        if (v + acc * dt > v_cap) {
          acc = std::max(0.0, (v_cap - v) / std::max(1e-4, dt));
        }

        const double dv = acc * dt;
        const double v_mid = std::max(1.0, v + 0.5 * dv);
        const double v_next = std::min(v_cap, std::max(1.0, v + dv));
        const double dx = v_mid * dt;
        s_pos += dx;
        v = v_next;
        total_time += dt;

        if (s_pos > arc_len) {
          const double over = s_pos - arc_len;
          const double back_dt = std::min(dt, over / std::max(1.0, v_mid));
          v = std::max(1.0, v - acc * back_dt);
          total_time -= back_dt;
          s_pos = arc_len;
        }
        if (shifting) {
          shift_rem -= dt;
          if (shift_rem <= 1e-5) {
            shifting = false;
            shift_rem = 0.0;
            ++gear;
          }
        }
      }
    }
  }

  return total_time * lap_scale;
}

}  // namespace

void PiConfig::finalize_segments() {
  // Euler-spiral exit-curve solver and corner overlap subtraction.
  for (PiSegment& seg : segments) {
    const double theta = std::fabs(seg.corner_angle_deg) * kDeg;
    const double R = std::max(2.0, seg.corner_radius_m);
    const double half_theta = 0.5 * theta;
    seg.entry_arc_m = R * half_theta;

    const double k0 = 1.0 / R;
    const double dk = kMinCurvature - k0;
    const double target_lat = std::max(0.5, track_width_m);

    // Exit straight tangent & normal directions at end of circular arc.
    const double exit_ang = kPi * 0.5 - half_theta;
    const double dir_x = std::cos(exit_ang);
    const double dir_y = std::sin(exit_ang);
    const double norm_x = -dir_y;
    const double norm_y = dir_x;
    const double start_ang = kPi - half_theta;
    const double apex_x = R * (std::cos(start_ang) + 1.0);
    const double apex_y = R * std::sin(start_ang);
    const double base_proj = dir_x * apex_x + dir_y * apex_y;
    const double base_lat = norm_x * apex_x + norm_y * apex_y;

    double p = 1.0;
    double p_lo = 0.0, p_hi = -1.0;
    double best_exit_arc = seg.entry_arc_m;
    double best_overlap = 0.0;

    for (int iter = 0; iter < 10; ++iter) {
      double sum_k = 0.0;
      for (int i = 0; i < 40; ++i) {
        const double u = double(i) / 39.0;
        sum_k += k0 + dk * std::pow(u, p);
      }
      const double ds = half_theta / std::max(1e-6, sum_k);
      double heading = 0.0;
      double px = 0.0, py = 0.0;
      double exit_arc = 0.0;
      for (int i = 0; i < 40; ++i) {
        const double u = double(i) / 39.0;
        const double k = std::max(kMinCurvature, k0 + dk * std::pow(u, p));
        const double r = 1.0 / k;
        const double d_ang = k * ds;
        exit_arc += d_ang * r;
        const double phi = kPi * 0.5 - heading;
        const double tx = std::cos(phi);
        const double ty = std::sin(phi);
        const double nx = ty;
        const double ny = -tx;
        const double s_d = std::sin(d_ang);
        const double c_d = 1.0 - std::cos(d_ang);
        px += r * (tx * s_d + nx * c_d);
        py += r * (ty * s_d + ny * c_d);
        heading += d_ang;
      }
      const double lat_off = (norm_x * px + norm_y * py) - base_lat;
      const double fwd_off = (dir_x * px + dir_y * py) - base_proj;
      best_exit_arc = exit_arc;
      best_overlap = fwd_off;
      if (lat_off > target_lat) {
        p_lo = p;
        p = p_hi >= 0.0 ? 0.5 * (p_hi + p) : 2.0 * p;
      } else if (lat_off < target_lat) {
        p_hi = p;
        p = 0.5 * (p_lo + p);
      } else {
        break;
      }
    }
    seg.exit_arc_m = std::max(1.0, best_exit_arc);
    seg.unwind_power = p;
    seg.overlap_m = best_overlap;
  }

  // Subtract previous segment's exit overlap from each segment's straight (min 50 m).
  const size_t n = segments.size();
  for (size_t i = 0; i < n; ++i) {
    const size_t prev = (i + n - 1) % n;
    segments[i].effective_straight_m = std::max(50.0, segments[i].straight_m - segments[prev].overlap_m);
  }
}

bool parse_pi_xml(std::string_view xml, PiConfig& out, std::string* error) {
  out = PiConfig{};
  size_t pos = 0;
  while ((pos = xml.find('<', pos)) != std::string_view::npos) {
    const size_t end = xml.find('>', pos + 1);
    if (end == std::string_view::npos) break;
    const std::string_view tag = xml.substr(pos + 1, end - pos - 1);
    pos = end + 1;
    if (tag.empty() || tag[0] == '/' || tag[0] == '!' || tag[0] == '?') continue;
    if (tag.rfind("Misc", 0) == 0) {
      xml_attr(tag, "MinPITimeSeconds", out.min_pi_time_s);
      xml_attr(tag, "MaxPITimeSeconds", out.max_pi_time_s);
      xml_attr(tag, "TrackWidthMeters", out.track_width_m);
      xml_attr(tag, "FWDAccelFrictionScaleWhileCornering", out.corner_accel_grip[0]);
      xml_attr(tag, "RWDAccelFrictionScaleWhileCornering", out.corner_accel_grip[1]);
      xml_attr(tag, "AWDAccelFrictionScaleWhileCornering", out.corner_accel_grip[2]);
      xml_attr(tag, "FWDAccelFrictionScaleWhileStraight", out.straight_accel_grip[0]);
      xml_attr(tag, "RWDAccelFrictionScaleWhileStraight", out.straight_accel_grip[1]);
      xml_attr(tag, "AWDAccelFrictionScaleWhileStraight", out.straight_accel_grip[2]);
      xml_attr(tag, "FWDLapTimeScale", out.lap_time_scale[0]);
      xml_attr(tag, "RWDLapTimeScale", out.lap_time_scale[1]);
      xml_attr(tag, "AWDLapTimeScale", out.lap_time_scale[2]);
    } else if (tag.rfind("Gear ", 0) == 0 || tag.rfind("Gear\t", 0) == 0) {
      double mph = 0.0;
      if (xml_attr(tag, "RedlineSpeedMPH", mph) && mph > 0.0) out.gear_redline_mph.push_back(mph);
    } else if (tag.rfind("Segment", 0) == 0) {
      PiSegment seg;
      if (xml_attr(tag, "StraightLengthMeters", seg.straight_m) &&
          xml_attr(tag, "CornerRadiusMeters", seg.corner_radius_m) &&
          xml_attr(tag, "CornerAngleDegrees", seg.corner_angle_deg)) {
        out.segments.push_back(seg);
      }
    }
  }
  if (out.segments.empty()) {
    if (error) *error = "PI.xml: no track segments";
    return false;
  }
  out.loaded_from_xml = true;
  out.finalize_segments();
  return true;
}

bool load_pi_config(const std::filesystem::path& physics_zip, PiConfig& out, std::string* error) {
  auto archive = fh1::Archive::open(physics_zip, error);
  if (!archive) return false;
  auto r = archive->read("PI.xml");
  if (!r.ok()) {
    if (error) *error = physics_zip.string() + "!PI.xml: " + r.error;
    return false;
  }
  const std::string_view text(reinterpret_cast<const char*>(r.data.data()), r.data.size());
  return parse_pi_xml(text, out, error);
}

PiConfig synthetic_pi_config() {
  PiConfig cfg;
  cfg.min_pi_time_s = 138.24;
  cfg.max_pi_time_s = 279.90;
  cfg.track_width_m = 9.0;
  cfg.gear_redline_mph = {45, 72, 100, 130, 155, 180, 215, 255};
  cfg.segments = {
      {1200.0, 180.0, 55.0}, {400.0, 90.0, 90.0}, {300.0, 28.0, 90.0}, {600.0, 130.0, 50.0},
      {500.0, 70.0, 110.0},  {400.0, 45.0, 75.0}, {650.0, 100.0, 65.0}, {300.0, 65.0, 160.0},
      {250.0, 25.0, 90.0},   {200.0, 25.0, 110.0},
  };
  cfg.finalize_segments();
  return cfg;
}

double simulate_pi_lap(const drive::CarParams& p, const cars::CarPhysics* car, const PiConfig& cfg) {
  // Use PI.xml's 8 reference gear redline speeds (SPEC-GARAGE-UPGRADES: gearing does not move PI).
  const bool use_ref_gears = !cfg.gear_redline_mph.empty();
  // Evaluate up to 3 aero tunes (default, min downforce 0.0, max downforce 1.0).
  double best_t = run_single_lap(p, cfg, use_ref_gears);
  if (car && (car->has("FrontBumperAero.Drag0") || car->has("RearWingAero.Drag0"))) {
    for (double slider : {0.0, 1.0}) {
      drive::CarParams pa = p;
      if (car->has("FrontBumperAero.Drag0")) {
        pa.aero_part_drag_front =
            lerp(car->get("FrontBumperAero.Drag0", 0), car->get("FrontBumperAero.Drag1", 0), slider) * kAeroK;
        pa.aero_part_df_front =
            lerp(car->get("FrontBumperAero.Downforce0", 0), car->get("FrontBumperAero.Downforce1", 0), slider) * kAeroK;
      }
      if (car->has("RearWingAero.Drag0")) {
        pa.aero_part_drag_rear =
            lerp(car->get("RearWingAero.Drag0", 0), car->get("RearWingAero.Drag1", 0), slider) * kAeroK;
        pa.aero_part_df_rear =
            lerp(car->get("RearWingAero.Downforce0", 0), car->get("RearWingAero.Downforce1", 0), slider) * kAeroK;
      }
      if (pa.aero_drag_long > 0) {
        pa.drag_quadratic = pa.aero_drag_long + pa.aero_part_drag_front + pa.aero_part_drag_rear;
      }
      best_t = std::min(best_t, run_single_lap(pa, cfg, use_ref_gears));
    }
  }
  return best_t;
}

double lap_time_to_pi(double lap_time_s, const PiConfig& cfg, double pi_floor) {
  const double t = std::clamp(lap_time_s, cfg.min_pi_time_s, cfg.max_pi_time_s);
  const double span = std::max(1e-6, cfg.max_pi_time_s - cfg.min_pi_time_s);
  const double raw = pi_floor + (1.0 - pi_floor) * (cfg.max_pi_time_s - t) / span;
  return std::clamp(raw, 0.0, 1.0);
}

CarStats compute_car_stats(const drive::CarParams& p, const cars::CarPhysics& car) {
  CarStats s;
  s.curb_weight_kg = p.mass;
  s.weight_dist_front = car.get("List_UpgradeCarBodyWeight.CMBackFront", 0.5);

  // Peak power (kW) & peak torque (N*m) over RPM sweep.
  for (int i = 10; i <= 200; ++i) {
    const double rpm = p.redline_rpm * double(i) / 200.0;
    const double tq = engine_wot_torque(p, rpm);
    const double kw = tq * (rpm * 2.0 * kPi / 60.0) / 1000.0;
    s.peak_torque_nm = std::max(s.peak_torque_nm, tq);
    s.peak_power_kw = std::max(s.peak_power_kw, kw);
  }

  // Straight-line acceleration sweep: 0-60 mph (26.8224 m/s), 0-100 mph (44.704 m/s), top speed.
  const double dt = 0.05;
  double v = 0.5, t = 0.0;
  double t60 = -1.0, t100 = -1.0;
  const double grip_straight = p.layout == drive::CarParams::Layout::kAll ? 1.0
                               : p.layout == drive::CarParams::Layout::kFront ? 0.955 : 0.925;
  for (int step = 0; step < 1200; ++step) {
    const double a = max_long_accel(p, v, grip_straight, nullptr);
    if (a <= 0.02 && v > 15.0) break;
    v = std::max(0.5, v + a * dt);
    t += dt;
    if (t60 < 0.0 && v >= 26.8224) t60 = t;
    if (t100 < 0.0 && v >= 44.704) t100 = t;
  }
  s.top_speed_ms = v;
  s.time_0_60_s = t60 > 0.0 ? t60 : 18.0;
  s.time_0_100_s = t100 > 0.0 ? t100 : 35.0;

  // 60-0 mph braking distance (m).
  double vb = 26.8224, dist_b = 0.0;
  while (vb > 0.2) {
    const double ab = max_brake_decel(p, vb);
    const double dv = std::min(vb, ab * 0.02);
    dist_b += (vb - 0.5 * dv) * (dv / ab);
    vb -= dv;
  }
  s.brake_60_0_m = dist_b;

  // Lateral G at 60 mph and 120 mph.
  s.lat_g_60 = max_lat_accel(p, 26.8224) / kKgf;
  s.lat_g_120 = max_lat_accel(p, 53.6448) / kKgf;

  // FH1 0.0..10.0 rating scales calibrated to Data_Car.*Rating ranges.
  const double top_mph = s.top_speed_ms / kMphToMs;
  s.speed_rating = std::clamp((top_mph - 85.0) / 145.0 * 10.0, 1.0, 10.0);
  const double avg_lat_g = 0.55 * s.lat_g_60 + 0.45 * s.lat_g_120;
  s.handling_rating = std::clamp((avg_lat_g - 0.45) / 0.85 * 10.0, 1.0, 10.0);
  const double acc_score = (11.5 - s.time_0_60_s) / 9.2 * 5.0 + (24.0 - s.time_0_100_s) / 19.5 * 5.0;
  s.accel_rating = std::clamp(acc_score, 1.0, 10.0);
  const double launch_bonus = p.layout == drive::CarParams::Layout::kAll ? 0.9
                              : p.layout == drive::CarParams::Layout::kFront ? -0.4 : 0.0;
  s.launch_rating = std::clamp((10.5 - s.time_0_60_s) / 8.0 * 10.0 + launch_bonus, 1.0, 10.0);
  s.braking_rating = std::clamp((46.0 - s.brake_60_0_m) / 21.0 * 10.0, 1.0, 10.0);

  // Displacement & Aspiration metadata for Garage / Upgrade Shop spec displays.
  s.displacement_cc = car.get("List_UpgradeEngineDisplacement.Disp", 0.0);
  if (p.has_turbo && p.has_supercharger) {
    s.aspiration = "Twincharged";
  } else if (p.has_turbo) {
    s.aspiration = car.has("List_UpgradeEngineTwinTurbo.Id") ? "Twin Turbo" : "Turbo";
  } else if (p.has_supercharger) {
    s.aspiration = "Supercharged";
  } else {
    s.aspiration = "Naturally Aspirated";
  }

  return s;
}

PiResult compute_pi(const cars::CarPhysics& fitted_phys, const cars::PhysicsSettings& settings, const PiConfig& cfg,
                    const std::vector<ClassBand>& bands, const cars::CarPhysics* stock_phys) {
  drive::CarParams p_fit;
  drive::geometry_from_physics(fitted_phys, p_fit);
  drive::apply_physics(fitted_phys, settings, p_fit);

  PiResult res;
  res.lap_time_s = simulate_pi_lap(p_fit, &fitted_phys, cfg);
  res.raw_lap_pi = lap_time_to_pi(res.lap_time_s, cfg, 0.0);
  res.stats = compute_car_stats(p_fit, fitted_phys);

  // Physics-consistent stock-anchored PI mapping:
  // - Exact Data_Car.PerformanceIndex when the car is stock (|dt| < 1e-6).
  // - Strictly monotonic headroom mapping toward 1.0 as virtual-lap time approaches
  //   MinPITimeSeconds (power-to-weight, grip, braking & aero), preventing stock-anchor
  //   bias carry-over drift or premature saturation on heavily upgraded builds.
  if (stock_phys && stock_phys->has("Data_Car.PerformanceIndex")) {
    const double p_stock = std::clamp(stock_phys->get("Data_Car.PerformanceIndex", res.raw_lap_pi), 0.005, 1.0);
    drive::CarParams p_stk;
    drive::geometry_from_physics(*stock_phys, p_stk);
    drive::apply_physics(*stock_phys, settings, p_stk);
    const double t_stock = simulate_pi_lap(p_stk, stock_phys, cfg);
    const double dt = t_stock - res.lap_time_s;
    if (std::fabs(dt) < 1e-6) {
      res.raw_pi = p_stock;
    } else if (dt > 0.0) {
      const double t_headroom = std::max(1.0, t_stock - cfg.min_pi_time_s);
      const double u = std::clamp(dt / t_headroom, 0.0, 1.0);
      res.raw_pi = std::clamp(p_stock + u * (1.0 - p_stock), 0.005, 1.0);
    } else {
      const double t_floor_room = std::max(1.0, cfg.max_pi_time_s - t_stock);
      const double u = std::clamp((-dt) / t_floor_room, 0.0, 1.0);
      res.raw_pi = std::clamp(p_stock * (1.0 - u), 0.005, 1.0);
    }
    // Monotonic headroom-anchored stat ratings: exact Data_Car ratings on stock cars,
    // converging smoothly to 10.0 on heavily upgraded builds without bias capping.
    const CarStats s_stk = compute_car_stats(p_stk, *stock_phys);
    auto anchor = [](double fit_v, double stk_v, const cars::CarPhysics& sp, const char* key) {
      if (!sp.has(key)) return std::clamp(fit_v, 0.0, 10.0);
      const double db_stk = std::clamp(sp.get(key, fit_v), 0.0, 10.0);
      if (std::fabs(fit_v - stk_v) < 1e-6) return db_stk;
      if (fit_v > stk_v) {
        const double denom = std::max(0.5, 10.0 - stk_v);
        const double u = std::clamp((fit_v - stk_v) / denom, 0.0, 1.0);
        return std::clamp(db_stk + u * (10.0 - db_stk), 0.0, 10.0);
      }
      const double denom = std::max(0.5, stk_v);
      const double u = std::clamp(fit_v / denom, 0.0, 1.0);
      return std::clamp(u * db_stk, 0.0, 10.0);
    };
    res.stats.speed_rating = anchor(res.stats.speed_rating, s_stk.speed_rating, *stock_phys, "Data_Car.SpeedRating");
    res.stats.handling_rating =
        anchor(res.stats.handling_rating, s_stk.handling_rating, *stock_phys, "Data_Car.HandlingRating");
    res.stats.accel_rating =
        anchor(res.stats.accel_rating, s_stk.accel_rating, *stock_phys, "Data_Car.AccelerationRating");
    res.stats.launch_rating =
        anchor(res.stats.launch_rating, s_stk.launch_rating, *stock_phys, "Data_Car.LaunchRating");
    res.stats.braking_rating =
        anchor(res.stats.braking_rating, s_stk.braking_rating, *stock_phys, "Data_Car.BrakingRating");
  } else {
    res.raw_pi = std::clamp(res.raw_lap_pi, 0.005, 1.0);
  }

  res.class_id = bands.empty() ? 0 : class_of(res.raw_pi, bands);
  res.shown_pi = bands.empty() ? int(std::ceil(res.raw_pi * 999.0)) : shown_index(res.raw_pi, bands);
  return res;
}

bool compute_pi_for_car(const cars::PartsDb& db, int64_t car_id, const cars::FittedParts& fit,
                        const cars::PhysicsSettings& settings, const PiConfig& cfg,
                        const std::vector<ClassBand>& bands, PiResult& out, std::string* error) {
  cars::CarPhysics stock_phys, fitted_phys;
  if (!db.physics(car_id, {}, stock_phys, error)) return false;
  if (fit.empty()) {
    out = compute_pi(stock_phys, settings, cfg, bands, &stock_phys);
    return true;
  }
  if (!db.physics(car_id, fit, fitted_phys, error)) return false;
  out = compute_pi(fitted_phys, settings, cfg, bands, &stock_phys);
  return true;
}

}  // namespace garage
