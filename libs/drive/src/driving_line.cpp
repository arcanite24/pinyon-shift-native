#include "drive/driving_line.h"

#include <algorithm>
#include <cmath>

namespace drive {

namespace {

uint8_t lerp_u8(uint8_t a, uint8_t b, double t) {
  const double v = double(a) + (double(b) - double(a)) * t;
  return uint8_t(std::clamp(std::lround(v), 0l, 255l));
}

double sample_curvature(const RacingLine& line, size_t seg, double d) {
  const size_t n = line.size();
  if (n < 3) return 0.0;
  if (line.curvature.size() == n && seg + 1 < n) {
    const double span = line.dist[seg + 1] - line.dist[seg];
    const double u = span > 1e-6 ? std::clamp((d - line.dist[seg]) / span, 0.0, 1.0) : 0.0;
    return std::fabs(double(line.curvature[seg]) + (double(line.curvature[seg + 1]) - double(line.curvature[seg])) * u);
  }
  if (line.radius.size() == n && seg + 1 < n) {
    const double span = line.dist[seg + 1] - line.dist[seg];
    const double u = span > 1e-6 ? std::clamp((d - line.dist[seg]) / span, 0.0, 1.0) : 0.0;
    const double r0 = std::max(1.0, double(line.radius[seg]));
    const double r1 = std::max(1.0, double(line.radius[seg + 1]));
    const double k0 = 1.0 / r0, k1 = 1.0 / r1;
    return k0 + (k1 - k0) * u;
  }
  // Fallback: 3-point Menger curvature across +-7 m chord.
  double x0 = 0, y0 = 0, z0 = 0, h0 = 0;
  double xm = 0, ym = 0, zm = 0, hm = 0;
  double x1 = 0, y1 = 0, z1 = 0, h1 = 0;
  const double chord = 7.0;
  const double len = line.length();
  const double da = std::max(0.0, d - chord);
  const double db = std::min(len, d + chord);
  if (db - da < 1.0) return 0.0;
  line.pose(da, 0.0, x0, y0, z0, h0);
  line.pose(d, 0.0, xm, ym, zm, hm);
  line.pose(db, 0.0, x1, y1, z1, h1);
  const double ax = x0 - xm, az = z0 - zm;
  const double bx = x1 - xm, bz = z1 - zm;
  const double la = std::hypot(ax, az);
  const double lb = std::hypot(bx, bz);
  const double lc = std::hypot(bx - ax, bz - az);
  if (la * lb * lc <= 1e-9) return 0.0;
  return std::fabs(2.0 * (ax * bz - az * bx) / (la * lb * lc));
}

}  // namespace

DrivingLineColor driving_line_color(double v_target, double v_car, DrivingLineMode mode) {
  if (mode == DrivingLineMode::kOff) return {0, 0, 0, 0};

  const double dv = v_target - v_car;
  const double t = std::clamp(std::fabs(dv) * 0.2, 0.0, 1.0);  // +-5.0 m/s band

  if (dv <= 0.0) {
    // Too fast / braking: Yellow -> Red, full opacity.
    return {
        lerp_u8(kDrivingLineYellow.r, kDrivingLineRed.r, t),
        lerp_u8(kDrivingLineYellow.g, kDrivingLineRed.g, t),
        lerp_u8(kDrivingLineYellow.b, kDrivingLineRed.b, t),
        255,
    };
  }

  // Safe / accelerating (dv > 0).
  if (mode == DrivingLineMode::kFull) {
    return {
        lerp_u8(kDrivingLineYellow.r, kDrivingLineGreen.r, t),
        lerp_u8(kDrivingLineYellow.g, kDrivingLineGreen.g, t),
        lerp_u8(kDrivingLineYellow.b, kDrivingLineGreen.b, t),
        255,
    };
  }

  // Braking Only: fade out accelerating segments via alpha attenuation while
  // keeping Yellow/Red braking zones visible.
  return {
      kDrivingLineYellow.r,
      kDrivingLineYellow.g,
      kDrivingLineYellow.b,
      uint8_t(std::clamp(std::lround(255.0 * (1.0 - t)), 0l, 255l)),
  };
}

BrakingTable BrakingTable::build(const CarParams& p) {
  BrakingTable tbl;
  constexpr double kG = DrivingLineOptions::kGravity;
  const double m = std::max(100.0, p.mass);
  const double weight = m * kG;

  const auto& wc = p.wheel_centres;
  const double zf = 0.5 * (wc[0].z + wc[1].z);
  const double zr = 0.5 * (wc[2].z + wc[3].z);
  const double wb = std::max(0.5, std::fabs(zf - zr));
  const double front_share =
      std::fabs(zf - zr) > 0.1 ? std::clamp((p.centre_of_mass.z - zr) / (zf - zr), 0.05, 0.95) : 0.5;
  const double h = std::max(0.0, p.centre_of_mass.y);
  const double axle_w[2] = {weight * front_share, weight * (1.0 - front_share)};

  const double rf = std::max(0.15, 0.5 * (p.tyre_radius[0] + p.tyre_radius[1]));
  const double rr = std::max(0.15, 0.5 * (p.tyre_radius[2] + p.tyre_radius[3]));
  // Maximum mechanical brake force per axle (2 wheels per axle; 0.81 full-press pedal curve).
  const double max_brake_f = 2.0 * p.front_brake_torque * 0.81 / rf;
  const double max_brake_r = 2.0 * p.rear_brake_torque * 0.81 / rr;

  // Lateral grip mu_lat at 20 m/s weighted by axle load share.
  const double mu_lat_f = tyre_peak_mu(p, 0, TyrePeak::kLateral, 0.5 * axle_w[0], 20.0);
  const double mu_lat_r = tyre_peak_mu(p, 1, TyrePeak::kLateral, 0.5 * axle_w[1], 20.0);
  tbl.lateral_mu = std::max(0.3, front_share * mu_lat_f + (1.0 - front_share) * mu_lat_r);

  // 10-point speed-vs-deceleration table (0, 10, ..., 90 m/s).
  // When per-car brake calibration is active, query Vehicle's exact Catmull-Rom
  // decel table (6-pass load transfer + aero downforce) instead of fallback torques.
  if (p.use_per_car_brakes) {
    Vehicle probe(p);
    const double a_max = std::max(1.0, probe.brake_max_decel());
    for (int i = 0; i < kPoints; ++i) {
      const double v = i * kSpeedStepMps;
      tbl.decel_mps2[size_t(i)] = std::max(1.0, a_max * probe.decel_table_at(v));
    }
    return tbl;
  }
  for (int i = 0; i < kPoints; ++i) {
    const double v = i * kSpeedStepMps;
    double a = 0.0;
    for (int pass = 0; pass < 6; ++pass) {
      const double transfer = std::min(m * a * h / wb, axle_w[1]);
      const double lf = std::max(0.0, axle_w[0] + transfer);
      const double lr = std::max(0.0, axle_w[1] - transfer);
      const double tyre_f = lf * tyre_peak_mu(p, 0, TyrePeak::kBraking, 0.5 * lf, v);
      const double tyre_r = lr * tyre_peak_mu(p, 1, TyrePeak::kBraking, 0.5 * lr, v);
      const double bf = max_brake_f > 0.0 ? std::min(tyre_f, max_brake_f) : tyre_f;
      const double br = max_brake_r > 0.0 ? std::min(tyre_r, max_brake_r) : tyre_r;
      a = (bf + br) / m;
    }
    const double aero_drag = (p.drag_quadratic * v * v + (v > 0.1 ? p.drag_constant : 0.0)) / m;
    tbl.decel_mps2[size_t(i)] = std::max(1.0, a + aero_drag);
  }
  return tbl;
}

double BrakingTable::decel_at(double speed_mps) const {
  if (speed_mps <= 0.0) return decel_mps2[0];
  const double max_v = (kPoints - 1) * kSpeedStepMps;
  if (speed_mps >= max_v) return decel_mps2[kPoints - 1];
  const double f = speed_mps / kSpeedStepMps;
  const int i0 = std::clamp(int(std::floor(f)), 0, kPoints - 2);
  const double u = f - i0;
  return decel_mps2[size_t(i0)] + (decel_mps2[size_t(i0 + 1)] - decel_mps2[size_t(i0)]) * u;
}

void build_driving_line(const RacingLine& line, const BrakingTable& table, double car_dist,
                        double car_speed, DrivingLineMode mode,
                        std::vector<DrivingLineSample>& out,
                        const DrivingLineOptions& opt) {
  out.clear();
  if (mode == DrivingLineMode::kOff || line.size() < 2) return;

  const double len = line.length();
  if (len <= 1e-3) return;

  const double step = std::max(0.5, opt.sample_step_m);
  const double start_d = std::clamp(car_dist - opt.behind_m, 0.0, len);
  const double visible_end_d = std::clamp(car_dist + opt.ahead_m, start_d, len);
  const double total_end_d = std::clamp(visible_end_d + opt.brake_lookahead_m, visible_end_d, len);
  if (visible_end_d - start_d < 0.5) return;

  const double a_lat = std::max(1.0, table.lateral_mu * DrivingLineOptions::kGravity);
  const double safety = std::clamp(opt.safety_factor, 0.5, 1.25);

  std::vector<DrivingLineSample> work;
  const size_t approx = size_t((total_end_d - start_d) / step) + 2;
  work.reserve(approx);

  size_t seg_hint = line.segment(start_d, 0);
  for (double d = start_d; d <= total_end_d + 1e-6; d += step) {
    const double clamped_d = std::min(d, total_end_d);
    seg_hint = line.segment(clamped_d, seg_hint);

    double px = 0, py = 0, pz = 0, heading = 0;
    line.pose(clamped_d, 0.0, px, py, pz, heading);

    // Heading 0 faces +z, positive toward +x -> forward = (sin h, cos h), right = (cos h, -sin h).
    const double rx = std::cos(heading);
    const double rz = -std::sin(heading);

    const double kappa = sample_curvature(line, seg_hint, clamped_d);
    double v_corner = opt.max_speed_mps;
    if (kappa > 1e-5) {
      const double R = 1.0 / kappa;
      v_corner = std::min(opt.max_speed_mps, safety * std::sqrt(a_lat * R));
    }

    DrivingLineSample s;
    s.x = float(px);
    s.y = float(py);
    s.z = float(pz);
    s.right_x = float(rx);
    s.right_z = float(rz);
    s.dist = float(clamped_d);
    s.v_corner = float(v_corner);
    s.v_target = float(v_corner);
    work.push_back(s);
    if (clamped_d >= total_end_d - 1e-6) break;
  }

  if (work.size() < 2) return;

  // Backward braking integration along the spline at fixed step 1/75 s
  // (0.0133333 s) against the car's 10-point speed-vs-deceleration table.
  constexpr double dt = DrivingLineOptions::kBrakeStepS;
  for (size_t i = work.size() - 1; i-- > 0;) {
    const double ds = double(work[i + 1].dist) - double(work[i].dist);
    if (ds <= 1e-6) {
      work[i].v_target = std::min(work[i].v_target, work[i + 1].v_target);
      continue;
    }
    double v = double(work[i + 1].v_target);
    double rem = ds;
    while (rem > 1e-6 && v < opt.max_speed_mps) {
      const double a = table.decel_at(v);
      const double step_ds = v * dt + 0.5 * a * dt * dt;
      if (step_ds >= rem) {
        v = std::sqrt(v * v + 2.0 * a * rem);
        rem = 0.0;
      } else {
        v += a * dt;
        rem -= step_ds;
      }
      if (v > opt.max_speed_mps) v = opt.max_speed_mps;
    }
    work[i].v_target = float(std::min(double(work[i].v_corner), v));
  }

  // Emit only the visible window [start_d, visible_end_d] with colours.
  out.reserve(work.size());
  for (auto& s : work) {
    if (double(s.dist) > visible_end_d + 1e-3) break;
    s.color = driving_line_color(double(s.v_target), car_speed, mode);
    if (opt.fade_ends) {
      double end_alpha = 1.0;
      const double from_start = double(s.dist) - start_d;
      const double to_end = visible_end_d - double(s.dist);
      if (from_start < 4.0) end_alpha = std::min(end_alpha, std::clamp(from_start / 4.0, 0.0, 1.0));
      if (to_end < 20.0) end_alpha = std::min(end_alpha, std::clamp(to_end / 20.0, 0.0, 1.0));
      s.color.a = uint8_t(std::clamp(std::lround(double(s.color.a) * end_alpha), 0l, 255l));
    }
    out.push_back(s);
  }
}

void build_driving_line_mesh(const std::vector<DrivingLineSample>& samples, double half_width_m,
                             double lift_m, std::vector<DrivingLineVertex>& out_triangles) {
  out_triangles.clear();
  if (samples.size() < 2) return;
  const float hw = float(std::max(0.05, half_width_m));
  const float lift = float(lift_m);

  // Precompute left/right ribbon edges and guard inner-edge vertices against backward folding
  // on tight corners where the ribbon half-width exceeds the inner turn radius.
  struct EdgePair {
    float lx, lz;
    float rx, rz;
  };
  std::vector<EdgePair> edges(samples.size());
  for (size_t i = 0; i < samples.size(); ++i) {
    const auto& s = samples[i];
    edges[i] = {s.x - s.right_x * hw, s.z - s.right_z * hw, s.x + s.right_x * hw, s.z + s.right_z * hw};
    if (i > 0) {
      const float fx = s.x - samples[i - 1].x;
      const float fz = s.z - samples[i - 1].z;
      if (fx * fx + fz * fz > 1e-6f) {
        if ((edges[i].lx - edges[i - 1].lx) * fx + (edges[i].lz - edges[i - 1].lz) * fz < 0.0f) {
          edges[i].lx = edges[i - 1].lx;
          edges[i].lz = edges[i - 1].lz;
        }
        if ((edges[i].rx - edges[i - 1].rx) * fx + (edges[i].rz - edges[i - 1].rz) * fz < 0.0f) {
          edges[i].rx = edges[i - 1].rx;
          edges[i].rz = edges[i - 1].rz;
        }
      }
    }
  }

  out_triangles.reserve((samples.size() - 1) * 6);
  for (size_t i = 0; i + 1 < samples.size(); ++i) {
    const auto& a = samples[i];
    const auto& b = samples[i + 1];
    if (a.color.a == 0 && b.color.a == 0) continue;

    const DrivingLineVertex al{edges[i].lx, a.y + lift, edges[i].lz, a.color.r, a.color.g, a.color.b, a.color.a};
    const DrivingLineVertex ar{edges[i].rx, a.y + lift, edges[i].rz, a.color.r, a.color.g, a.color.b, a.color.a};
    const DrivingLineVertex bl{edges[i + 1].lx, b.y + lift, edges[i + 1].lz, b.color.r, b.color.g, b.color.b, b.color.a};
    const DrivingLineVertex br{edges[i + 1].rx, b.y + lift, edges[i + 1].rz, b.color.r, b.color.g, b.color.b, b.color.a};

    out_triangles.push_back(al);
    out_triangles.push_back(ar);
    out_triangles.push_back(br);

    out_triangles.push_back(al);
    out_triangles.push_back(br);
    out_triangles.push_back(bl);
  }
}

}  // namespace drive
