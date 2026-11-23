#include "drive/vehicle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace drive {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kKgf = 9.80665;  // N per kilogram-force (the curves' load unit)
constexpr double kRpm = 60 / (2 * kPi);

double length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 normalized(const Vec3& a) {
  const double l = length(a);
  return l > 1e-12 ? a * (1.0 / l) : Vec3{};
}
double approach(double from, double to, double rate, double dt) {
  const double d = to - from, step = rate * dt;
  return std::fabs(d) <= step ? to : from + (d > 0 ? step : -step);
}
double lerp(double a, double b, double t) { return a + (b - a) * t; }
double ramp(double x, double x0, double x1) {  // 0 at x0, 1 at x1, clamped
  if (x1 == x0) return x >= x1 ? 1 : 0;
  return std::clamp((x - x0) / (x1 - x0), 0.0, 1.0);
}
double sign(double x) { return x > 0 ? 1 : x < 0 ? -1 : 0; }

// Deterministic 2D smooth value noise and analytical gradient for Bumpiness
// NoiseType 1..8 (COLLISION.md, Runtime SurfaceType evaluation).
double hash2d(int ix, int iz) {
  uint32_t h = uint32_t(ix) * 374761393u + uint32_t(iz) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return double(h & 0xFFFFu) / 32767.5 - 1.0;  // [-1, 1]
}
void smooth_noise2d(double x, double z, double& val, double& dx, double& dz) {
  const int ix = int(std::floor(x)), iz = int(std::floor(z));
  const double fx = x - ix, fz = z - iz;
  const double u = fx * fx * (3.0 - 2.0 * fx), du = 6.0 * fx * (1.0 - fx);
  const double v = fz * fz * (3.0 - 2.0 * fz), dv = 6.0 * fz * (1.0 - fz);
  const double a = hash2d(ix, iz), b = hash2d(ix + 1, iz);
  const double c = hash2d(ix, iz + 1), d = hash2d(ix + 1, iz + 1);
  const double ab = a + (b - a) * u, cd = c + (d - c) * u;
  val = ab + (cd - ab) * v;
  dx = ((b - a) * (1.0 - v) + (d - c) * v) * du;
  dz = (cd - ab) * dv;
}

}  // namespace

BumpSample eval_surface_bumpiness(const SurfaceTyre& s, double x, double z) {
  BumpSample out;
  if (s.bump_noise_type <= 0 || s.bump_frequency <= 0 || s.bump_scale == 0) return out;
  const double f = s.bump_frequency;
  const double xf = x * f, zf = z * f;
  double h_raw = 0, dhx = 0, dhz = 0;
  switch (s.bump_noise_type) {
    case 9: {  // SinWave (COLLISION.md exact formula)
      const double u = 2.0 * kPi * xf, v = 2.0 * kPi * zf;
      h_raw = 0.5 * (std::sin(u) + std::sin(v));
      dhx = 0.5 * f * 2.0 * kPi * std::cos(u);
      dhz = 0.5 * f * 2.0 * kPi * std::cos(v);
      break;
    }
    case 3: {  // Ridges (rational remapping, squares h^2, scales gradient by 2*h)
      double n = 0, nx = 0, nz = 0;
      smooth_noise2d(xf, zf, n, nx, nz);
      const double r = 1.0 - std::fabs(n);
      const double rs = n >= 0 ? -1.0 : 1.0;
      h_raw = r * r;
      dhx = 2.0 * r * rs * nx * f;
      dhz = 2.0 * r * rs * nz * f;
      break;
    }
    case 5:
    case 6:
    case 7: {  // Simplex4 / Simplex4Power / Simplex4Abs
      double amp = 0.5, freq_m = 1.0;
      for (int o = 0; o < 4; ++o) {
        double n = 0, nx = 0, nz = 0;
        smooth_noise2d(xf * freq_m, zf * freq_m, n, nx, nz);
        if (s.bump_noise_type == 7) {
          const double sg = n >= 0 ? 1.0 : -1.0;
          h_raw += amp * std::fabs(n);
          dhx += amp * sg * nx * f * freq_m;
          dhz += amp * sg * nz * f * freq_m;
        } else {
          h_raw += amp * n;
          dhx += amp * nx * f * freq_m;
          dhz += amp * nz * f * freq_m;
        }
        amp *= 0.5;
        freq_m *= 2.0;
      }
      if (s.bump_noise_type == 6) {
        const double h0 = h_raw;
        h_raw = h0 * h0;
        dhx *= 2.0 * h0;
        dhz *= 2.0 * h0;
      }
      break;
    }
    case 8: {  // Simplex3Multiplied
      double n1 = 0, n1x = 0, n1z = 0, n2 = 0, n2x = 0, n2z = 0, n3 = 0, n3x = 0, n3z = 0;
      smooth_noise2d(xf, zf, n1, n1x, n1z);
      smooth_noise2d(xf * 2.0, zf * 2.0, n2, n2x, n2z);
      smooth_noise2d(xf * 4.0, zf * 4.0, n3, n3x, n3z);
      h_raw = n1 * n2 * n3;
      dhx = f * (n1x * n2 * n3 + 2.0 * n1 * n2x * n3 + 4.0 * n1 * n2 * n3x);
      dhz = f * (n1z * n2 * n3 + 2.0 * n1 * n2z * n3 + 4.0 * n1 * n2 * n3z);
      break;
    }
    default: {  // HeightGrid (1), Simplex (2), RidgesAndBumps (4)
      double n = 0, nx = 0, nz = 0;
      smooth_noise2d(xf, zf, n, nx, nz);
      h_raw = n;
      dhx = nx * f;
      dhz = nz * f;
      break;
    }
  }
  double h_clamped = h_raw;
  if (h_raw < s.bump_clamp_min) {
    h_clamped = s.bump_clamp_min;
    dhx = dhz = 0;
  } else if (h_raw > s.bump_clamp_max) {
    h_clamped = s.bump_clamp_max;
    dhx = dhz = 0;
  }
  out.height = s.bump_offset + h_clamped * s.bump_scale;
  out.dh_dx = dhx * s.bump_scale;
  out.dh_dz = dhz * s.bump_scale;
  return out;
}

int substeps_for(double dt, double hz) {
  dt = std::min(dt, 0.1);
  // A hair under the product so that dt x hz landing on an integer by
  // rounding does not add a substep.
  return std::max(1, int(std::ceil(dt * hz - 1e-9)));
}

double FrictionCurve::peak_slip() const {
  if (values.size() < 2) return max_slip;
  size_t best = 0;
  for (size_t i = 1; i < values.size(); ++i)
    if (values[i] >= values[best]) best = i;
  return max_slip * double(best) / double(values.size() - 1);
}

double FrictionCurve::at(double slip, double stretch) const {
  if (values.empty()) return 0;
  if (values.size() == 1 || max_slip <= 0) return values.back();
  const double spacing = stretch * max_slip / double(values.size() - 1);
  const double x = std::fabs(slip) / spacing;
  const size_t last = values.size() - 1;
  if (x >= double(last)) return values[last];
  const size_t i = size_t(x);
  return lerp(values[i], values[i + 1], x - double(i));
}

double body_contact_friction(const Hit& hit, const Vec3& normal, double default_friction) {
  const bool is_wall = std::fabs(normal.y) < 0.65;
  const bool is_dirt_bank = hit.tyre.off_roadness >= 0.5 || hit.tyre.is_springy;
  if (is_dirt_bank) return std::max(default_friction, 0.45);
  if (is_wall) return std::min(default_friction, 0.12);  // smooth guardrail / concrete wall-glide
  return default_friction;
}

double body_contact_damping_scale(const Hit& hit, const Vec3& normal) {
  const bool is_wall = std::fabs(normal.y) < 0.65;
  const bool is_dirt_bank = hit.tyre.off_roadness >= 0.5 || hit.tyre.is_springy;
  if (is_dirt_bank) return 1.25;  // absorb impact energy into soft dirt banks
  if (is_wall) return 0.90;       // crisp guardrail / concrete glance
  return 1.0;
}

double TyreCurves::blend(double load_kgf) const {
  return ramp(std::min(load_kgf, load_clamp_kgf), light_load_kgf, heavy_load_kgf);
}
double TyreCurves::peak_slip(double load_kgf, double stretch) const {
  return stretch * lerp(light.peak_slip(), heavy.peak_slip(), blend(load_kgf));
}
double TyreCurves::value(double slip, double load_kgf, double stretch, const std::array<double, 2>& scale) const {
  return lerp(light.at(slip, stretch) * scale[0], heavy.at(slip, stretch) * scale[1], blend(load_kgf));
}

FrictionCurve synthetic_curve(double max_slip, double peak, double tail) {
  FrictionCurve c;
  c.max_slip = max_slip;
  c.values.resize(100);
  for (size_t i = 0; i < c.values.size(); ++i) {
    const double x = max_slip * double(i) / 99.0;
    c.values[i] = x <= peak ? std::sin(0.5 * kPi * x / peak)
                            : lerp(1.0, tail, std::min(1.0, (x - peak) / (0.4 * max_slip)));
  }
  return c;
}

double tyre_peak_mu(const CarParams& p, int axle, TyrePeak kind, double load_n, double speed) {
  const bool lat = kind == TyrePeak::kLateral;
  const TyreCurves& c = lat ? p.lateral : p.longitudinal;
  const auto& scale = lat ? p.lat_curve_scale : p.long_curve_scale;
  const double kgf = std::max(0.0, load_n) / kKgf;
  // The light- and heavy-load curves' peaks (times their FrictionScale),
  // blended by load (SPEC-RACE-AI, the capacity queries).
  auto top = [](const FrictionCurve& f) {
    double m = 0;
    for (double x : f.values) m = std::max(m, x);
    return m;
  };
  double peak = top(c.light) * scale[0] + (top(c.heavy) * scale[1] - top(c.light) * scale[0]) * c.blend(kgf);
  peak *= p.width_scale[size_t(axle)] * p.long_misc_scale;
  if (lat) return peak * p.lat_scale[size_t(axle)] * p.lat_compound_scale;
  peak *= p.long_scale[size_t(axle)];
  if (kind == TyrePeak::kBraking) return peak * p.brake_friction_scale;
  return peak * lerp(p.accel_scale0, p.accel_scale1, ramp(std::fabs(speed), p.accel_speed0, p.accel_speed1));
}

Vec3 rotate(const Quat& q, const Vec3& v) {
  const Vec3 u{q.x, q.y, q.z};
  const Vec3 t = cross(u, v) * 2.0;
  return v + t * q.w + cross(u, t);
}
Vec3 rotate_inverse(const Quat& q, const Vec3& v) { return rotate(Quat{-q.x, -q.y, -q.z, q.w}, v); }

bool FlatGround::raycast(const Vec3& origin, const Vec3& dir, double max_distance, Hit& hit) const {
  if (dir.y > -1e-9) return false;
  const double d = (height_ - origin.y) / dir.y;
  if (d < 0 || d > max_distance) return false;
  hit.distance = d;
  hit.normal = {0, 1, 0};
  hit.grip = 1;
  return true;
}

Vehicle::Vehicle(const CarParams& params) : p_(params) {
  com_local_ = p_.centre_of_mass;
  inertia_ = p_.inertia;
  if (inertia_.x <= 0 || inertia_.y <= 0 || inertia_.z <= 0) {
    // SPEC-CAR-PHYSICS: the mass as a solid box (X across, Y up, Z along).
    const double x = p_.inertia_box.x, y = p_.inertia_box.y, z = p_.inertia_box.z, m = p_.mass / 12.0;
    inertia_ = {m * (y * y + z * z), m * (x * x + z * z), m * (x * x + y * y)};
  }
  substeps_ = substeps_for(kStep, p_.physics_hz);
  // Static load per wheel from the centre of mass between the axles and
  // across each axle; the preloaded springs hold the car at the baked pose.
  const double z_front = (p_.wheel_centres[0].z + p_.wheel_centres[1].z) / 2;
  const double z_rear = (p_.wheel_centres[2].z + p_.wheel_centres[3].z) / 2;
  const double span = z_front - z_rear;
  const double front_share = std::fabs(span) > 1e-6 ? (com_local_.z - z_rear) / span : 0.5;
  for (int i = 0; i < kWheels; ++i) {
    const int a = axle_of(i);
    const double share = a == 0 ? front_share : 1 - front_share;
    const double xl = p_.wheel_centres[a == 0 ? 0 : 2].x, xr = p_.wheel_centres[a == 0 ? 1 : 3].x;
    const double side = std::fabs(xr - xl) > 1e-6 ? (i % 2 == 1 ? (com_local_.x - xl) / (xr - xl)
                                                                 : (xr - com_local_.x) / (xr - xl))
                                                   : 0.5;
    preload_[i] = p_.mass * p_.gravity * share * side;
    droop_[i] = std::min(p_.max_droop[a], preload_[i] / std::max(1.0, p_.spring_rate[a]));
  }
  const double rf_awd = std::max(1e-3, 0.5 * (p_.tyre_radius[0] + p_.tyre_radius[1]));
  const double rr_awd = std::max(1e-3, 0.5 * (p_.tyre_radius[2] + p_.tyre_radius[3]));
  const double awd_r_denom = std::max(1e-6, p_.rear_split * rf_awd + (1.0 - p_.rear_split) * rr_awd);
  for (int i = 0; i < kWheels; ++i) {
    const bool front = axle_of(i) == 0;
    switch (p_.layout) {
      case CarParams::Layout::kRear: weight_[i] = front ? 0 : 0.5; break;
      case CarParams::Layout::kFront: weight_[i] = front ? 0.5 : 0; break;
      case CarParams::Layout::kAll:
        // SPEC-CAR-PHYSICS, AWD: open torque weights scale by rolling-radius ratio
        // R_rear / (s_r * R_front + (1 - s_r) * R_rear) front and R_front / (...) rear.
        weight_[i] = 0.5 * (front ? (1.0 - p_.rear_split) * (rr_awd / awd_r_denom)
                                  : p_.rear_split * (rf_awd / awd_r_denom));
        break;
    }
  }
  // Compute peak boosted power RPM (w_peak_power) for closed-throttle curve
  // (SPEC-CAR-PHYSICS, Engine item 1).
  {
    double best_p = -1.0, best_rpm = std::max(p_.idle_rpm + 500.0, 0.85 * p_.redline_rpm);
    const double slots = p_.torque_curve_slots > 0
                             ? p_.torque_curve_slots
                             : (p_.torque_curve.size() > 1 ? double(p_.torque_curve.size() - 1) : 1.0);
    for (int step = 10; step <= 200; ++step) {
      const double r = p_.redline_rpm * double(step) / 200.0;
      double base = 1.0;
      if (p_.torque_curve.empty()) {
        base = r < 2000 ? 0.6 + 0.4 * r / 2000 : r > 5500 ? 1.0 - 0.3 * ramp(r, 5500, p_.redline_rpm) : 1.0;
      } else {
        const double x = r / (p_.torque_curve_max_rpm / slots);
        const size_t last = p_.torque_curve.size() - 1;
        base = x >= double(last) ? p_.torque_curve[last]
                                 : lerp(p_.torque_curve[size_t(x)], p_.torque_curve[size_t(x) + 1], x - double(size_t(x)));
      }
      double f_b = p_.has_turbo ? p_.boost_scale_max : 1.0;
      if (f_b > 1.0 && p_.turbo_dropoff_rpm0 != p_.turbo_dropoff_rpm1) {
        const double u = ramp(r, std::min(p_.turbo_dropoff_rpm0, p_.turbo_dropoff_rpm1),
                              std::max(p_.turbo_dropoff_rpm0, p_.turbo_dropoff_rpm1));
        f_b = lerp(p_.turbo_dropoff_scale0 * (f_b - 1.0) + 1.0, p_.turbo_dropoff_scale1 * (f_b - 1.0) + 1.0, u);
      }
      double f_s = p_.has_supercharger ? lerp(p_.sc_scale_zero, p_.sc_scale_redline, ramp(r, 0, p_.redline_rpm)) : 1.0;
      if (f_s > 1.0 && p_.sc_dropoff_rpm0 != p_.sc_dropoff_rpm1) {
        const double u = ramp(r, std::min(p_.sc_dropoff_rpm0, p_.sc_dropoff_rpm1),
                              std::max(p_.sc_dropoff_rpm0, p_.sc_dropoff_rpm1));
        f_s = lerp(p_.sc_dropoff_scale0 * (f_s - 1.0) + 1.0, p_.sc_dropoff_scale1 * (f_s - 1.0) + 1.0, u);
      }
      const double power = r * base * p_.torque_scale * (1.0 + (f_b - 1.0) + (f_s - 1.0));
      if (power > best_p) {
        best_p = power;
        best_rpm = r;
      }
    }
    peak_power_rpm_ = std::max(std::max(800.0, p_.idle_rpm) + 100.0, std::min(best_rpm, p_.redline_rpm));
  }
  init_brakes();
  reset({0, 0, 0});
}

void Vehicle::init_brakes() {
  // SPEC-CAR-PHYSICS, Brakes: 6 fixed-point longitudinal load-transfer
  // iterations at 80 mph, 200 mph, 50 mph, and 10 speed samples (0..200 mph).
  const double z_front = 0.5 * (p_.wheel_centres[0].z + p_.wheel_centres[1].z);
  const double z_rear = 0.5 * (p_.wheel_centres[2].z + p_.wheel_centres[3].z);
  const double L = std::max(0.5, std::fabs(z_front - z_rear));
  const double front_share = std::clamp((com_local_.z - z_rear) / L, 0.1, 0.9);
  const double h_cg = std::max(0.1, com_local_.y);
  const double df_f_c = p_.aero_df_front + p_.aero_part_df_front;
  const double df_r_c = p_.aero_df_rear + p_.aero_part_df_rear;

  auto solve_decel = [&](double v, double& ff_out, double& fr_out) {
    const double nf0 = p_.mass * p_.gravity * front_share + df_f_c * v * v;
    const double nr0 = p_.mass * p_.gravity * (1.0 - front_share) + df_r_c * v * v;
    double a = 0.0, ff = 0.0, fr = 0.0;
    for (int it = 0; it < 6; ++it) {
      const double dn = p_.mass * a * h_cg / L;
      const double nf = std::max(1.0, nf0 + dn);
      const double nr = std::max(1.0, nr0 - dn);
      const double mu_f = tyre_peak_mu(p_, 0, TyrePeak::kBraking, 0.5 * nf, v);
      const double mu_r = tyre_peak_mu(p_, 1, TyrePeak::kBraking, 0.5 * nr, v);
      ff = nf * mu_f;
      fr = nr * mu_r;
      a = (ff + fr) / std::max(1.0, p_.mass);
    }
    ff_out = ff;
    fr_out = fr;
    return std::max(0.0980665, a);
  };

  double ff80 = 0, fr80 = 0, ff200 = 0, fr200 = 0, ff50 = 0, fr50 = 0;
  const double a80 = solve_decel(35.7632, ff80, fr80);
  const double a200 = solve_decel(89.408, ff200, fr200);
  const double a50 = solve_decel(22.352, ff50, fr50);
  const double a_max = std::max(a80, a200);
  brake_a_max_ = a_max;
  const double g_max = a_max / kKgf;
  const double p_min = std::min(p_.brake_pressure_min_share, 1.0 - p_.brake_pressure_min_share);
  const double f_tot80 = std::max(1e-4, ff80) + std::max(1e-4, fr80);
  const double bias_opt = std::clamp(std::max(1e-4, ff80) / f_tot80, p_min, 1.0 - p_min);
  const double bias_eff = std::clamp(p_.brake_bias_slider - 0.5 * p_.brake_bias_offset_rear, 0.0, 1.0);
  const double s = std::clamp(p_.brake_torque_slider, 0.0, 1.0);
  const double pressure_mult =
      s <= 0.5 ? lerp(0.0, 1.8181818, s / 0.5) : lerp(1.8181818, 4.3478260, (s - 0.5) / 0.5);
  brake_const_[0] = 4.0 * bias_opt * bias_eff * g_max * pressure_mult;
  brake_const_[1] = 4.0 * (1.0 - bias_opt) * (1.0 - bias_eff) * g_max * pressure_mult;
  handbrake_const_ = std::clamp(p_.bias_handbrake, 0.0, 5.5) * (a50 / kKgf);

  for (int i = 0; i < 10; ++i) {
    const double v = std::max(0.01, double(i) / 0.1006621346);
    double ff = 0, fr = 0;
    decel_table_[size_t(i)] = solve_decel(v, ff, fr) / std::max(1e-4, a_max);
  }
}

double Vehicle::decel_table_at(double speed_ms) const {
  // Catmull-Rom interpolation of decel_table_[0..9] over 0..89.408 m/s.
  const double u = std::clamp(std::fabs(speed_ms) * 0.1006621346, 0.0, 9.0);
  const int i1 = std::min(8, int(std::floor(u)));
  const double t = u - double(i1);
  const int i0 = std::max(0, i1 - 1);
  const int i2 = std::min(9, i1 + 1);
  const int i3 = std::min(9, i1 + 2);
  const double p0 = decel_table_[size_t(i0)], p1 = decel_table_[size_t(i1)];
  const double p2 = decel_table_[size_t(i2)], p3 = decel_table_[size_t(i3)];
  const double t2 = t * t, t3 = t2 * t;
  return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
}

void Vehicle::place(const Vec3& position, double heading) {
  const double t = time_;
  reset(position, heading);
  time_ = t;
}

void Vehicle::launch(const Vec3& position, double heading, double speed, double pitch) {
  place(position, heading);
  const Quat yaw = rot_;
  // Nose up: a rotation about the body's +x by -pitch (z forward, y up).
  const Quat tilt{std::sin(-pitch / 2), 0, 0, std::cos(-pitch / 2)};
  rot_ = {yaw.w * tilt.x + yaw.x * tilt.w + yaw.y * tilt.z - yaw.z * tilt.y,
          yaw.w * tilt.y - yaw.x * tilt.z + yaw.y * tilt.w + yaw.z * tilt.x,
          yaw.w * tilt.z + yaw.x * tilt.y - yaw.y * tilt.x + yaw.z * tilt.w,
          yaw.w * tilt.w - yaw.x * tilt.x - yaw.y * tilt.y - yaw.z * tilt.z};
  pos_ = position + rotate(rot_, com_local_);
  vel_ = rotate(rot_, Vec3{0, 0, speed});
  for (int i = 0; i < kWheels; ++i) w_[i].omega = p_.tyre_radius[i] > 0 ? speed / p_.tyre_radius[i] : 0;
  // The highest gear that keeps the engine above the floor and below the
  // up-shift point (first gear when none does).
  int gear = 1;
  const double floor = std::max(p_.idle_rpm * 1.5, 0.35 * p_.redline_rpm);
  for (int g = 1; g <= int(p_.gears.size()); ++g) {
    const double rpm = speed / std::max(1e-3, p_.tyre_radius[2]) * ratio(g) * kRpm;
    if (rpm < p_.upshift_fraction * p_.redline_rpm && rpm >= floor) gear = g;
  }
  gear_ = target_gear_ = gear_before_ = gear;
  state_ = GearState::kEngaged;
  clutch_ = 1;
  locked_ = true;
  engine_omega_ = std::max(p_.idle_rpm / kRpm + 0.1, speed / std::max(1e-3, p_.tyre_radius[2]) * ratio(gear));
  prev_rpm_ = engine_omega_ * kRpm;
  turbo_power_ = 0;
  boost_psi_ = p_.boost_min_psi;
  sc_psi_ = p_.boost_min_psi;
  bov_open_ = true;
  boost_factor_ = 1.0;
  engine_torque_prev_ = 0;
}

void Vehicle::apply_impulse(const Vec3& point, const Vec3& impulse, double angular_scale) {
  vel_ = vel_ + impulse * (1 / p_.mass);
  const Vec3 tw = cross(point - pos_, impulse) * angular_scale;
  const Vec3 tl = rotate_inverse(rot_, tw);
  ang_ = ang_ + rotate(rot_, Vec3{tl.x / inertia_.x, tl.y / inertia_.y, tl.z / inertia_.z});
}

Vec3 Vehicle::point_velocity(const Vec3& point) const { return vel_ + cross(ang_, point - pos_); }

double Vehicle::inverse_mass_along(const Vec3& point, const Vec3& n, double angular_scale) const {
  const Vec3 rn = rotate_inverse(rot_, cross(point - pos_, n));
  const Vec3 w = rotate(rot_, Vec3{rn.x / inertia_.x, rn.y / inertia_.y, rn.z / inertia_.z});
  return 1 / p_.mass + angular_scale * dot(cross(w, point - pos_), n);
}

void Vehicle::reset(const Vec3& position, double heading) {
  rot_ = {0, std::sin(heading / 2), 0, std::cos(heading / 2)};
  pos_ = position + rotate(rot_, com_local_);  // pos_ holds the centre of mass
  vel_ = ang_ = {};
  gear_ = target_gear_ = gear_before_ = 1;
  state_ = GearState::kOpen;
  state_time_ = 0;
  reverse_hold_time_ = 0;
  pedal_swap_ = false;
  clutch_ = 0;
  locked_ = rev_matching_ = false;
  launch_target_ = 0;
  engine_omega_ = p_.idle_rpm / kRpm + 0.1;
  prev_rpm_ = engine_omega_ * kRpm;
  spool_ = engine_throttle_ = driver_throttle_ = 0;
  turbo_power_ = 0;
  boost_psi_ = p_.boost_min_psi;
  sc_psi_ = p_.boost_min_psi;
  bov_open_ = true;
  boost_factor_ = 1.0;
  engine_torque_prev_ = 0;
  tc_prev_ = 0;
  slip_pos_ = 0;
  time_ = 0;
  steer_angle_ = 0;
  steer_ang_vel_ = 0;
  handbrake_shaped_ = 0;
  abs_flag_ = false;
  tcs_flag_ = false;
  stm_flag_ = false;
  tcs_amount_ = 0;
  stm_amount_ = 0;
  shifted_without_clutch_ = false;
  trans_damage_ = 0;
  stall_timer_ = 0;
  raw_ = shaped_ = {};
  for (int i = 0; i < kWheels; ++i) {
    w_[i] = {};
    w_[i].centre = p_.wheel_centres[i];
    w_[i].contact = true;
    prev_compression_[i] = 0;
    abs_timer_[i] = 0;
    abs_scale_[i] = 1.0;
    vel_dep_long_[i] = 0;
    vel_dep_lat_[i] = 0;
    spring_defl_[i] = 0;
    for (double& s : spring_ring_[i]) s = 0;
    for (double& h : slip_hist_[i]) h = 0;
  }
  spring_ring_pos_ = 0;
  surface_bump_rumble_ = 0;
  hand_of_god_ = false;
}

Vec3 Vehicle::position() const { return pos_ - rotate(rot_, com_local_); }

Vehicle::Pose Vehicle::blend(const Pose& a, const Pose& b, double t) {
  t = std::clamp(t, 0.0, 1.0);
  const Vec3 d = b.pos - a.pos;
  if (t >= 1 || dot(d, d) > 25) return b;
  auto mix = [t](const Vec3& x, const Vec3& y) { return x + (y - x) * t; };
  Pose p = b;
  p.pos = mix(a.pos, b.pos), p.vel = mix(a.vel, b.vel), p.ang = mix(a.ang, b.ang);
  // The shorter way round, then normalised (the step is too small for slerp to differ).
  const double s = a.rot.x * b.rot.x + a.rot.y * b.rot.y + a.rot.z * b.rot.z + a.rot.w * b.rot.w < 0 ? -1.0 : 1.0;
  Quat q{a.rot.x + (s * b.rot.x - a.rot.x) * t, a.rot.y + (s * b.rot.y - a.rot.y) * t,
         a.rot.z + (s * b.rot.z - a.rot.z) * t, a.rot.w + (s * b.rot.w - a.rot.w) * t};
  const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  p.rot = {q.x / n, q.y / n, q.z / n, q.w / n};
  for (size_t i = 0; i < p.wheels.size(); ++i) {
    const WheelState &wa = a.wheels[i], &wb = b.wheels[i];
    WheelState& w = p.wheels[i];
    w.centre = mix(wa.centre, wb.centre);
    w.compression = wa.compression + (wb.compression - wa.compression) * t;
    w.steer = wa.steer + (wb.steer - wa.steer) * t;
    w.spin = wa.spin + std::remainder(wb.spin - wa.spin, 2 * kPi) * t;
  }
  return p;
}

double Vehicle::speed() const { return dot(vel_, rotate(rot_, {0, 0, 1})); }

double Vehicle::rpm() const { return engine_omega_ * kRpm; }

double Vehicle::ratio(int gear) const {
  if (gear >= 1 && size_t(gear) <= p_.gears.size()) return p_.gears[size_t(gear - 1)] * p_.final_drive;
  if (gear == -1) return -p_.reverse_gear * p_.final_drive;
  return 0;
}

double Vehicle::clutch_output_rpm() const { return ratio(gear_) * input_omega() * kRpm; }

double Vehicle::input_omega() const {
  double o = 0;
  for (int i = 0; i < kWheels; ++i) o += weight_[i] * w_[i].omega;
  return o;
}

void Vehicle::shape_input(const Input& raw, double dt) {
  // SPEC-CAR-PHYSICS, Throttle, brake and steering from the pad & Reverse
  // selection and pedal swap: when in reverse with pedal_swap active, pedals
  // swap upstream of control smoothing (AccelDelaySpeed = 5/s, BrakeDelaySpeed = 20/s).
  double gas_in = std::clamp(raw.throttle, 0.0, 1.0);
  double brake_in = std::clamp(raw.brake, 0.0, 1.0);
  if (assists_.shifting == 0 && gear_ == -1 && pedal_swap_) {
    if (brake_in > 0.0 && gas_in == 0.0) {
      gas_in = brake_in;
      brake_in = 0.0;
    } else if (gas_in > 0.0) {
      brake_in = gas_in;
      gas_in = 0.0;
    }
  }
  if (ai_) {
    // The AI's controls skip the shaping: the road wheels at steer x
    // SteerMaxAngle at once.
    shaped_.throttle = gas_in;
    shaped_.brake = brake_in;
    shaped_.steer = std::clamp(raw.steer, -1.0, 1.0);
    shaped_.handbrake = raw.handbrake;
    shaped_.clutch = std::clamp(raw.clutch, 0.0, 1.0);
    shaped_.shift = raw.shift;
    handbrake_shaped_ = raw.handbrake ? 1.0 : 0.0;
    steer_angle_ = shaped_.steer * p_.max_steer;
    steer_ang_vel_ = 0.0;
    return;
  }
  shaped_.throttle = approach(shaped_.throttle, gas_in, p_.throttle_rate, dt);
  shaped_.brake = approach(shaped_.brake, brake_in, p_.brake_rate, dt);
  shaped_.steer = std::clamp(raw.steer, -1.0, 1.0);  // the traced steer control steps to the stick
  shaped_.handbrake = raw.handbrake;
  shaped_.clutch = std::clamp(raw.clutch, 0.0, 1.0);
  shaped_.shift = raw.shift;
  handbrake_shaped_ = approach(handbrake_shaped_, raw.handbrake ? 1.0 : 0.0, p_.handbrake_rate, dt);
  // Forza dynamic pad steering filter (SPEC-CAR-PHYSICS, Steering-at-speed filter
  // and Assisted / Normal / Simulation steering):
  // - SteeringWheelFilter speed-dependent stick shaping
  // - Speed-sensitive rate interpolation across [SlowSpeed, FastSpeed] toward FastRateScale
  // - Dynamic peak-slip tracking (SteerAngVelDynFindPeak) in Normal/Assisted modes
  // - Countersteer boost above rear slip threshold
  // - Acceleration-ramped angular velocity over SteerAccelTimeToMaxRate
  const double v_abs = std::fabs(speed());
  const double lock = steer_lock();
  const double u_sw = assists_.steering == 2
                          ? ramp(v_abs, p_.sw_filter_corr_speed0, p_.sw_filter_corr_speed1)
                          : ramp(v_abs, p_.sw_filter_speed0, p_.sw_filter_speed1);
  const double sw_scale = assists_.steering == 2
                              ? lerp(p_.sw_filter_corr_scale0, p_.sw_filter_corr_scale1, u_sw)
                              : lerp(p_.sw_filter_scale0, p_.sw_filter_scale1, u_sw);
  // Subtle highway-speed center precision from SteeringWheelFilter while keeping
  // full mid-stick authority at cornering speeds.
  const double sw_blend = std::clamp(0.08 * (sw_scale - 1.0) * u_sw, 0.0, 0.25);
  const double stick_abs = std::fabs(shaped_.steer);
  const double stick_shaped = sign(shaped_.steer) * lerp(stick_abs, stick_abs * stick_abs, sw_blend);
  double target = stick_shaped * lock;
  const bool centring = std::fabs(target) < std::fabs(steer_angle_) && target * steer_angle_ >= 0;
  const double mean_rear_sa = 0.5 * (w_[2].slip_angle + w_[3].slip_angle);
  const bool countersteering =
      std::fabs(mean_rear_sa) > 0.5 && (shaped_.steer * mean_rear_sa > 0);  // steer into slide
  const double u_spd = ramp(v_abs, p_.steer_slow_speed, p_.steer_fast_speed);
  const double speed_rate_scale = lerp(1.0, p_.steer_fast_rate_scale, u_spd);
  const double share = p_.max_steer > 0 ? std::clamp(lock / p_.max_steer, p_.steer_rate_min_share, 1.0) : 1.0;
  double rate_use = centring ? p_.straighten_rate : p_.steer_rate * lerp(share, share * p_.steer_fast_rate_scale, u_spd);
  // Dynamic peak-slip tracking: in Normal/Assisted modes (and not during
  // handbrake drift initiation), when front tyres wash past peak lateral slip,
  // pull target angle smoothly toward peak front grip via SteerAngVelDynFindPeak.
  if (assists_.steering != 2 && !countersteering && handbrake_shaped_ < 0.1 && v_abs > 5.0 && stick_abs > 0.05) {
    const double mean_front_sa = 0.5 * (std::fabs(w_[0].slip_angle) + std::fabs(w_[1].slip_angle));
    if (mean_front_sa > 1.2) {
      const double peak_pull = std::clamp(1.2 / mean_front_sa, 0.78, 1.0);
      target *= peak_pull;
      rate_use = std::max(rate_use, p_.steer_dyn_find_peak_rate * speed_rate_scale);
    }
  }
  if (countersteering) rate_use = std::max(rate_use, p_.steer_countersteer_rate);
  if (assists_.steering == 2) rate_use = std::max(rate_use, p_.steer_rate * std::max(0.5, share));
  if (p_.steer_accel_time > 1e-4 && dt > 0.0) {
    const double err = target - steer_angle_;
    const double desired_vel = std::clamp(err / dt, -rate_use, rate_use);
    const double accel = rate_use / p_.steer_accel_time;
    steer_ang_vel_ = approach(steer_ang_vel_, desired_vel, accel, dt);
    const double next = steer_angle_ + steer_ang_vel_ * dt;
    if ((err > 0 && next >= target) || (err < 0 && next <= target)) {
      steer_angle_ = target;
      steer_ang_vel_ = 0.0;
    } else {
      steer_angle_ = next;
    }
  } else {
    steer_angle_ = approach(steer_angle_, target, rate_use, dt);
    steer_ang_vel_ = 0.0;
  }
}

double Vehicle::steer_lock() const {
  const double wheelbase = std::fabs((p_.wheel_centres[0].z + p_.wheel_centres[1].z) / 2 -
                                     (p_.wheel_centres[2].z + p_.wheel_centres[3].z) / 2);
  const double v = std::fabs(speed());
  const double max_a = (!ai_ && assists_.steering == 0 && p_.max_steer_filtered > 0)
                           ? std::min(p_.max_steer, p_.max_steer_filtered)
                           : p_.max_steer;
  if (ai_ || v <= 0.1) return max_a;
  const double mode_gees = assists_.steering == 2 ? 1.25 : (assists_.steering == 0 ? 0.90 : 1.0);
  return std::clamp(std::atan(p_.steer_gees_fit * p_.steer_max_gees * mode_gees * p_.gravity * wheelbase / (v * v)),
                    std::min(p_.steer_min_lock, max_a), max_a);
}

void Vehicle::update_boost(double dt, double throttle, double rpm_now) {
  auto apply_dropoff = [](double f, double rpm_val, double r0, double r1, double s0, double s1) {
    if (f <= 1.0 || r0 == r1) return f;
    const double lo = std::min(r0, r1), span = std::fabs(r1 - r0);
    const double u = std::clamp((rpm_val - lo) / span, 0.0, 1.0);
    const double excess = f - 1.0;
    return lerp(s0 * excess + 1.0, s1 * excess + 1.0, u);
  };
  double f_boost = 1.0;
  if (p_.has_turbo) {
    if (throttle > 0.05) bov_open_ = false;
    else if (throttle < 0.01) bov_open_ = true;
    const double p_eng = engine_omega_ * engine_torque_prev_;
    const double p_target = bov_open_ ? 0.0 : std::clamp(p_eng, 7457.0, 745700.0);
    const double tau = std::max(1e-3, p_.turbo_tau);
    turbo_power_ = std::clamp(turbo_power_ + ((p_target - turbo_power_) / tau) * dt, 0.0, p_.turbo_power_max);
    double b_target = p_.boost_min_psi;
    if (!bov_open_) {
      const double u_p = ramp(turbo_power_, p_.turbo_power_min, p_.turbo_power_max);
      b_target = lerp(p_.boost_min_psi, p_.boost_max_psi, u_p);
    }
    boost_psi_ += (b_target - boost_psi_) * std::min(1.0, 15.0 * dt);
    const double b_lo = std::min(p_.boost_min_psi, p_.boost_max_psi);
    const double b_hi = std::max(p_.boost_min_psi, p_.boost_max_psi);
    const double u_b = b_hi > b_lo ? std::clamp((boost_psi_ - b_lo) / (b_hi - b_lo), 0.0, 1.0) : 0.0;
    f_boost = lerp(p_.boost_scale_min, p_.boost_scale_max, u_b);
    f_boost = apply_dropoff(f_boost, rpm_now, p_.turbo_dropoff_rpm0, p_.turbo_dropoff_rpm1, p_.turbo_dropoff_scale0,
                            p_.turbo_dropoff_scale1);
  }
  double f_sc = 1.0;
  if (p_.has_supercharger) {
    const double u_rpm = std::clamp(rpm_now / std::max(1.0, p_.redline_rpm), 0.0, 1.0);
    const double b_wot = lerp(p_.sc_zero_psi, p_.sc_redline_psi, u_rpm);
    const double b_target = lerp(3.675, b_wot, std::clamp(throttle, 0.0, 1.0));
    sc_psi_ += (b_target - sc_psi_) * std::min(1.0, 15.0 * dt);
    const double b_lo = std::min(3.675, p_.sc_redline_psi);
    const double b_hi = std::max(3.675, p_.sc_redline_psi);
    const double u_sc = b_hi > b_lo ? std::clamp((sc_psi_ - b_lo) / (b_hi - b_lo), 0.0, 1.0) : 0.0;
    f_sc = lerp(std::min(1.0, p_.sc_scale_zero), p_.sc_scale_redline, u_sc);
    f_sc = apply_dropoff(f_sc, rpm_now, p_.sc_dropoff_rpm0, p_.sc_dropoff_rpm1, p_.sc_dropoff_scale0,
                         p_.sc_dropoff_scale1);
  }
  boost_factor_ = 1.0 + (f_sc - 1.0) + (f_boost - 1.0);
}

double Vehicle::engine_torque(double rpm, double throttle) const {
  throttle = std::clamp(throttle, 0.0, 1.0);
  double full;
  if (p_.torque_curve.empty()) {
    // The synthetic car's curve: rising from idle, flat 2,000-5,500 rpm,
    // falling to the redline.
    const double r = std::max(rpm, 0.0);
    full = r < 2000 ? 0.6 + 0.4 * r / 2000 : r > 5500 ? 1 - 0.3 * ramp(r, 5500, p_.redline_rpm) : 1.0;
  } else {
    // Sample i at i x TorqueCurveMaxRPM / (NumTorqueValues - 1).
    const double slots = p_.torque_curve_slots > 0 ? p_.torque_curve_slots : double(p_.torque_curve.size() - 1);
    const double x = std::max(rpm, 0.0) / (p_.torque_curve_max_rpm / slots);
    const size_t last = p_.torque_curve.size() - 1;
    if (x >= double(last)) full = p_.torque_curve[last];
    else {
      const size_t i = size_t(x);
      full = lerp(p_.torque_curve[i], p_.torque_curve[i + 1], x - double(i));
    }
  }
  const double k_limiter = rpm >= p_.redline_rpm ? 0.05 : 1.0;
  const double wot = full * p_.torque_scale * boost_factor_ * k_limiter * torque_factor_;
  // SPEC-CAR-PHYSICS, Engine item 1 (confirmed): for ICE, c_closed ramps
  // linearly 0 -> -1.0 from 800 rpm to peak-power RPM, stays flat -1.0
  // through RedlineRPM, and ramps -1.0 -> -2.63 above RedlineRPM toward
  // TorqueCurveMaxRPM (below 800 rpm, small positive idle-assist 0.10 -> 0).
  const double r_low = 800.0;
  const double r_peak = std::min(peak_power_rpm_, p_.redline_rpm);
  double c_closed = 0.0;
  if (rpm < r_low) {
    c_closed = 0.10 * (1.0 - std::max(0.0, rpm) / r_low);
  } else if (rpm < r_peak) {
    c_closed = -(rpm - r_low) / std::max(1.0, r_peak - r_low);
  } else if (rpm <= p_.redline_rpm) {
    c_closed = -1.0;
  } else {
    const double over_span = p_.torque_curve_max_rpm - p_.redline_rpm;
    c_closed = over_span > 1.0 ? -1.0 - 1.63 * std::clamp((rpm - p_.redline_rpm) / over_span, 0.0, 1.0) : -2.63;
  }
  const double closed = c_closed * p_.engine_braking;
  return throttle * (wot - closed) + closed;
}

void Vehicle::update_gearbox(double dt) {
  // SPEC-CAR-PHYSICS, Clutch and gearbox states. clutch_ is the engagement
  // (the spec's c is 1 - clutch_). Reverse pedal swap already ran upstream
  // of smoothing in shape_input when pedal_swap_ is active.
  const double v = speed();
  const int shifting_mode = ai_ ? 0 : assists_.shifting;  // 0 Auto, 1 Manual, 2 Manual+Clutch
  const bool swapped = shifting_mode == 0 && gear_ == -1 && pedal_swap_;
  const double drive_pedal = (shifting_mode == 0 && gear_ == -1 && !swapped) ? shaped_.brake : shaped_.throttle;
  const double brake_pedal = (shifting_mode == 0 && gear_ == -1 && !swapped) ? shaped_.throttle : shaped_.brake;
  // Legacy synthetic throttle lag (off on real cars where boost_lag == 0).
  const double trigger = std::clamp((shifting_mode == 0 && gear_ == -1) ? raw_.brake : raw_.throttle, 0.0, 1.0);
  if (p_.boost_lag > 0 && trigger > spool_) spool_ += (trigger - spool_) * (1 - std::exp(-dt / p_.boost_lag));
  else spool_ = trigger;
  const double driver = p_.boost_lag > 0 ? std::min(drive_pedal, spool_) : drive_pedal;
  driver_throttle_ = driver;
  const double rpm_now = rpm();
  const double rpm_rate = (rpm_now - prev_rpm_) / dt;
  prev_rpm_ = rpm_now;
  const double idle = p_.idle_rpm, start = p_.launch_rpm, red = p_.redline_rpm;
  const int top = int(p_.gears.size());
  auto out_rpm = [&] { return ratio(gear_) * input_omega() * kRpm; };  // the clutch output, signed
  state_time_ += dt;
  auto enter = [&](GearState s) {
    state_ = s;
    state_time_ = 0;
  };
  auto open = [&] {
    enter(GearState::kOpen);
    clutch_ = 0;
  };
  auto start_shift = [&](int target) {
    gear_before_ = gear_;
    target_gear_ = target;
    enter(GearState::kClutchIn);
  };
  auto next_manual_gear = [&](int dir) {
    if (dir > 0) return gear_ == -1 ? 1 : (gear_ < top ? gear_ + 1 : gear_);
    if (dir < 0) return gear_ == 1 ? -1 : (gear_ > 1 ? gear_ - 1 : gear_);
    return gear_;
  };
  // Manual with Clutch (Shifting == 2, SPEC-CAR-PHYSICS, Manual with Clutch):
  // direct pedal engagement curve f_clutch, shift-without-clutch penalty +
  // slower engagement rate, stall timer, and emergent clutch-kick.
  if (shifting_mode == 2) {
    pedal_swap_ = false;
    reverse_hold_time_ = 0;
    const double c_pedal = std::clamp(raw_.clutch, 0.0, 1.0);
    const double dead = std::clamp(p_.clutch_pedal_dead, 0.0, 0.9);
    const double f_clutch =
        std::pow(std::clamp((1.0 - c_pedal - dead) / (1.0 - dead), 0.0, 1.0), std::max(0.1, p_.clutch_torque_exp));
    if (raw_.shift != 0) {
      const int tgt = next_manual_gear(raw_.shift);
      if (tgt != gear_) {
        gear_before_ = gear_;
        gear_ = target_gear_ = tgt;
        locked_ = false;
        if (c_pedal <= 0.8) {
          shifted_without_clutch_ = true;
          trans_damage_ = std::clamp(trans_damage_ + p_.shift_without_clutch_damage, 0.0, 1.0);
          clutch_ = 0.0;
        } else {
          shifted_without_clutch_ = false;
        }
      }
    }
    if (c_pedal > 0.1) {
      locked_ = false;
      stall_timer_ = 0;
    }
    const double vel = shifted_without_clutch_ ? p_.clutch_vel_no_clutch : p_.clutch_max_vel;
    clutch_ = approach(clutch_, f_clutch, vel, dt);
    if (clutch_ >= 0.99 && f_clutch >= 0.99) shifted_without_clutch_ = false;
    // Stall detection: engaged in gear near/at idle with car stopped.
    if (clutch_ > 0.5 && rpm_now <= idle + 20.0 && std::fabs(v) < 1.0 && drive_pedal < 0.05) {
      stall_timer_ += dt;
      if (stall_timer_ >= p_.clutch_stall_timeout) {
        clutch_ = 0;
        locked_ = false;
        state_ = GearState::kOpen;
      }
    } else {
      stall_timer_ = 0;
      state_ = clutch_ > 0.05 ? GearState::kEngaged : GearState::kOpen;
    }
    engine_throttle_ = driver;
    return;
  }
  // The gearbox's own throttle in a rev-match: towards the clutch output's
  // speed in the new gear (ours: proportional over 200 rpm).
  auto rev_request = [&] { return std::clamp((std::fabs(out_rpm()) - rpm_now) / 200, 0.0, 1.0); };
  const bool in_shift = state_ == GearState::kClutchIn || state_ == GearState::kGearChange ||
                        state_ == GearState::kRevMatch || state_ == GearState::kClutchOut;

  // Manual (Shifting == 1): explicit shift requests, no auto-shifts or reverse swap.
  if (shifting_mode == 1) {
    pedal_swap_ = false;
    reverse_hold_time_ = 0;
    if (!in_shift && raw_.shift != 0) {
      const int tgt = next_manual_gear(raw_.shift);
      if (tgt != gear_) start_shift(tgt);
    }
  } else if (!in_shift) {
    // Automatic reverse selection and pedal swap (SPEC-CAR-PHYSICS):
    // requires all four conditions simultaneously.
    const bool want_rev = gear_ >= 1 && raw_.brake > 0.5 && raw_.throttle == 0.0 && v < p_.reverse_select_speed &&
                          (std::fabs(v) < p_.reverse_auto_enter_speed || pedal_swap_);
    if (want_rev) {
      reverse_hold_time_ += dt;
      if (reverse_hold_time_ >= p_.shift_reverse_pause) {
        gear_before_ = gear_;
        gear_ = target_gear_ = -1;
        pedal_swap_ = true;
        // Carry existing brake pedal magnitude into reverse gas_cmd on the
        // swap (the player is already holding the trigger); clear brake.
        shaped_.throttle = shaped_.brake;
        shaped_.brake = 0;
        reverse_hold_time_ = 0;
        open();
      }
    } else {
      reverse_hold_time_ = 0;
      if (gear_ == -1 && raw_.throttle > 0.5 && raw_.brake < 0.1 && v > -p_.reverse_select_speed) {
        gear_before_ = gear_;
        gear_ = target_gear_ = 1;
        pedal_swap_ = false;
        shaped_.throttle = 0;
        shaped_.brake = 0;
        open();
      } else if (gear_ == 0) {
        gear_ = target_gear_ = 1;
        pedal_swap_ = false;
        open();
      }
    }
  } else {
    reverse_hold_time_ = 0;
  }

  switch (state_) {
    case GearState::kOpen: {
      clutch_ = 0;
      engine_throttle_ = driver;
      if (drive_pedal > 0.1 && brake_pedal < 0.1) {
        const double s0 = idle + 0.2 * (start - idle);
        if (out_rpm() > p_.launch_max_output_rpm) {
          gear_before_ = target_gear_ = gear_;  // rolling: match the engine, then close
          rev_matching_ = true;
          enter(GearState::kRevMatch);
        } else if (rpm_now > lerp(s0, start, drive_pedal)) {
          enter(GearState::kLaunch);
        }
      }
      break;
    }
    case GearState::kLaunch: {
      engine_throttle_ = driver;
      if (brake_pedal > 0.1) {
        open();
        break;
      }
      const double tau = std::min(1.0, state_time_ / p_.launch_fall_time);
      const double high = lerp(start + 0.2 * (red - start), idle + 0.3 * (start - idle), tau);
      const double low = lerp(idle + 0.2 * (start - idle), idle + 0.01 * (start - idle), tau);
      const double target = lerp(low, high, drive_pedal);
      launch_target_ = target;
      // dc/dt = gain x (target - (rpm + lookahead x rpm rate)) / target.
      const double c_rate = p_.launch_gain * (target - (rpm_now + p_.launch_lookahead * rpm_rate)) / std::max(1.0, target);
      clutch_ = std::clamp(clutch_ - c_rate * dt, 0.0, 1.0);
      double slip = 0, n = 0;
      for (int i = 0; i < kWheels; ++i)
        if (weight_[i] > 0) slip += std::fabs(w_[i].slip_ratio), n += 1;
      if (n > 0) slip /= n;
      if (out_rpm() > target && slip < p_.launch_exit_slip) {
        rev_matching_ = false;
        enter(GearState::kClutchOut);
      }
      break;
    }
    case GearState::kEngaged: {
      engine_throttle_ = driver;
      clutch_ = approach(clutch_, 1, 1 / p_.clutch_out_time, dt);
      const double road = std::fabs(out_rpm());
      if (road < 0.9 * idle) {
        open();  // the engine would be dragged under idle (ours)
        break;
      }
      // Automatic shifts (SPEC-CAR-PHYSICS, Gears): disabled in Manual mode.
      if (shifting_mode == 0) {
        const double up_at = lerp(0.5, p_.upshift_fraction, std::clamp(drive_pedal, 0.0, 1.0)) * red;
        const double next_rpm = gear_ >= 1 && gear_ < top ? road * ratio(gear_ + 1) / ratio(gear_) : 0;
        const bool part = !ai_ && rpm_now >= up_at && next_rpm >= 0.4 * red;
        if (gear_ >= 1 && gear_ < top && locked_ && (rpm_now >= p_.upshift_fraction * red || part)) {
          start_shift(gear_ + 1);
        } else if (gear_ > 1) {
          const double lower = road * ratio(gear_ - 1) / ratio(gear_);
          if ((brake_pedal > 0.1 && lower < p_.downshift_brake_fraction * red) ||
              (drive_pedal > 0.1 && road < 0.35 * red && lower < 0.75 * red))
            start_shift(gear_ - 1);
        }
      }
      break;
    }
    case GearState::kClutchIn:
      engine_throttle_ = 0;  // the power cut
      locked_ = false;       // SPEC-CAR-PHYSICS: kClutchIn unlocks driveline immediately
      clutch_ = std::max(0.0, clutch_ - dt / p_.clutch_in_time);
      if (clutch_ <= 0) enter(GearState::kGearChange);
      break;
    case GearState::kGearChange:
      engine_throttle_ = 0;
      clutch_ = 0;
      if (state_time_ >= p_.gear_shift_time - 1e-9) {
        gear_ = target_gear_;  // the gear number changes at the end
        rev_matching_ = true;
        enter(GearState::kRevMatch);
      }
      break;
    case GearState::kRevMatch:
      clutch_ = 0;
      engine_throttle_ = rev_request();
      if (state_time_ >= p_.rev_match_time - 1e-9) enter(GearState::kClutchOut);
      break;
    case GearState::kClutchOut: {
      clutch_ = std::min(1.0, clutch_ + dt / p_.clutch_out_time);
      // Power comes back as the clutch closes: lerp(driver, the box's
      // request, c). Out of a launch the box asks for nothing of its own.
      const double request = rev_matching_ ? rev_request() : driver;
      engine_throttle_ = lerp(driver, request, 1 - clutch_);
      if (clutch_ >= 1) enter(GearState::kEngaged);
      break;
    }
  }
}

void Vehicle::step(const Input& input, const Ground& ground) {
  raw_ = input;
  shape_input(input, kStep);
  update_gearbox(kStep);
  abs_flag_ = false;
  tcs_flag_ = false;
  stm_flag_ = false;
  tcs_amount_ = 0;
  stm_amount_ = 0;
  strongest_ = {};
  const double dt = kStep / substeps_;
  any_dirt_ = false;
  for (int s = 0; s < substeps_; ++s) substep(dt, ground);
  time_ += kStep;
  // The AI's grip on dirt for the next step (SPEC-CAR-PHYSICS).
  const double speed = std::sqrt(vel_.x * vel_.x + vel_.y * vel_.y + vel_.z * vel_.z);
  if (any_dirt_ && speed > 0.5) {
    if (!on_dirt_) dirt_time_ = 0;
    on_dirt_ = true;
    dirt_time_ += kStep;
    const double yaw = std::fabs(ang_.y);
    const double k_yaw = std::clamp((yaw - 0.3) / 0.3, 0.0, 1.0);
    const double k_time = dirt_time_ <= 1 ? 1.0 : dirt_time_ >= 3 ? 0.5 : 1 - 0.25 * (dirt_time_ - 1);
    dirt_k_ = speed < 10 ? 1.0 : k_yaw * k_time;
  } else {
    on_dirt_ = false;
    dirt_k_ = 0;
  }
}

void Vehicle::substep(double dt, const Ground& ground) {
  const Vec3 up = rotate(rot_, {0, 1, 0});
  const Vec3 down = up * -1.0;
  const CarParams& p = p_;
  Vec3 force{};
  Vec3 torque{};

  // 1. Suspension and contact.
  Vec3 contact[kWheels]{}, normal[kWheels]{};
  double grip[kWheels]{}, rate[kWheels]{};
  SurfaceTyre surf[kWheels]{};
  int off = 0;
  double max_bump_rumble = 0;
  for (int i = 0; i < kWheels; ++i) {
    WheelState& w = w_[i];
    const int a = axle_of(i);
    const double radius = p.tyre_radius[i];
    const Vec3 hard_local{p.wheel_centres[i].x, p.wheel_centres[i].y + p.rest_length, p.wheel_centres[i].z};
    const Vec3 hard = pos_ + rotate(rot_, hard_local - com_local_);
    // Ackermann (ours: the spec has no steering geometry): each front wheel
    // points at one turn centre on the rear axle's line, steer_angle_ being
    // the angle of a wheel on the centre line.
    w.steer = 0;
    if (a == 0 && std::fabs(steer_angle_) > 1e-9) {
      const double z_rear = 0.5 * (p.wheel_centres[2].z + p.wheel_centres[3].z);
      const double base = p.wheel_centres[i].z - z_rear;
      const double centre_x = base / std::tan(steer_angle_);
      w.steer = std::atan(base / (centre_x - p.wheel_centres[i].x));
      if (w.steer * steer_angle_ < 0) w.steer += steer_angle_ > 0 ? kPi : -kPi;  // past 90 degrees
    }
    Hit hit;
    const double reach = p.probe_height + p.rest_length + droop_[i] + radius;
    const Vec3 probe = hard - down * p.probe_height;
    double compression = -droop_[i];
    w.contact = up.y > 0.2 && ground.raycast(probe, down, reach, hit);
    w.surface = w.contact ? hit.surface : -1;
    if (w.contact) {
      compression = p.rest_length - (hit.distance - p.probe_height - radius);
      if (compression < -droop_[i]) w.contact = false, compression = -droop_[i];
    }
    if (w.contact) {
      normal[i] = hit.normal;
      grip[i] = hit.grip;
      surf[i] = hit.tyre;
      if (hit.tyre.off_roadness > 0) any_dirt_ = true;
      contact[i] = probe + down * hit.distance;
      // Bumpiness height & gradient perturbation + rumble (COLLISION.md).
      // Keep the surface-entry jump out of damper differentiation so rolling
      // onto dirt/kerb perturbs spring load and normal without launching the
      // wheel via a 359 Hz derivative spike.
      double bump_h = 0;
      if (surf[i].bump_noise_type > 0 && surf[i].bump_scale != 0) {
        const BumpSample bs = eval_surface_bumpiness(surf[i], contact[i].x, contact[i].z);
        bump_h = std::clamp(bs.height, -0.05, 0.05);
        const Vec3 n_pert = normalized(normal[i] - Vec3{bs.dh_dx, 0, bs.dh_dz});
        if (dot(n_pert, normal[i]) > 0.1) normal[i] = n_pert;
      }
      max_bump_rumble = std::max(max_bump_rumble, 2.0 * surf[i].bump_rumble);
      // Hand-of-God trigger on water / trigger surfaces or below world floor Y.
      if (surf[i].is_hand_of_god || contact[i].y < p.water_floor_y) hand_of_god_ = true;
      // Springiness soft-ground contact (COLLISION.md): slew-clamped deflection
      // and 3-sample moving-average ring buffer.
      if (surf[i].is_springy && surf[i].spring_k > 0) {
        const double target_defl = std::max(0.0, compression) *
                                   std::clamp(p.spring_rate[a] / (p.spring_rate[a] + surf[i].spring_k * p.mass), 0.0, 0.8);
        const double slew = (target_defl > spring_defl_[i] ? std::max(0.5, surf[i].spring_comp_d)
                                                           : std::max(0.5, surf[i].spring_reb_d)) *
                            dt;
        spring_defl_[i] = std::clamp(target_defl, spring_defl_[i] - slew, spring_defl_[i] + slew);
        spring_ring_[i][spring_ring_pos_] = spring_defl_[i];
        const double smooth_defl = (spring_ring_[i][0] + spring_ring_[i][1] + spring_ring_[i][2]) / 3.0;
        compression -= smooth_defl;
      } else {
        spring_defl_[i] = 0;
        spring_ring_[i][spring_ring_pos_] = 0;
      }
      // Suspension rate from geometric compression; add bump height to spring
      // compression only (not the 359 Hz derivative on surface transition).
      rate[i] = (compression - prev_compression_[i]) / dt;
      prev_compression_[i] = compression;
      compression += bump_h;
    } else {
      rate[i] = (compression - prev_compression_[i]) / dt;
      prev_compression_[i] = compression;
    }
    w.compression = compression;
    off += !w.contact;
    double load = 0;
    if (w.contact) {
      double damper = rate[i] * (rate[i] > 0 ? p.bump_rate[a] : p.rebound_rate[a]);
      damper = std::clamp(damper, -p.damper_clamp[a], p.damper_clamp[a]);
      load = preload_[i] + p.spring_rate[a] * compression + damper;
      if (compression > p.bump_travel[a])
        load += p.bumpstop_rate[a] * (compression - p.bump_travel[a]) + p.bumpstop_damping[a] * std::max(0.0, rate[i]);
    }
    w.load = load;
    w.centre = {p.wheel_centres[i].x, p.wheel_centres[i].y + std::min(compression, p.bump_travel[a]),
                p.wheel_centres[i].z};
  }
  spring_ring_pos_ = (spring_ring_pos_ + 1) % 3;
  surface_bump_rumble_ = max_bump_rumble;
  // Anti-roll bars: equal and opposite on an axle's two wheels.
  for (int a = 0; a < 2; ++a) {
    const int l = 2 * a, r = 2 * a + 1;
    if (!w_[l].contact || !w_[r].contact) continue;
    const double f = p.antiroll_rate[a] * (w_[l].compression - w_[r].compression) +
                     p.antiroll_damping[a] * (rate[l] - rate[r]);
    w_[l].load += f;
    w_[r].load -= f;
  }
  for (auto& w : w_) w.load = std::max(0.0, w.load);

  const double g_scale = off == kWheels ? p.airborne_gravity_scale : std::pow(p.per_tyre_off_gravity_scale, off);
  force.y -= p.mass * p.gravity * g_scale;

  // 2. Brakes with ABS / No-ABS and STM (SPEC-CAR-PHYSICS, Brakes, ABS, STM).
  const double drive_in = engine_throttle_;
  const bool auto_swap = (!ai_ && assists_.shifting == 0) || ai_;
  const double brake_in = (auto_swap && gear_ == -1 && !pedal_swap_) ? shaped_.throttle : shaped_.brake;
  const bool abs_on = ai_ ? p.abs : (p.abs && assists_.braking != 2);
  const bool abs_off_boost = !ai_ && !abs_on;
  // Direct BrakeInputScale curve with NO / 0.81 normalization when using
  // per-car brake_const; synthetic fallback normalizes by kBrakeOut[3].
  double brake_curve_raw = 0;
  if (brake_in > 0) {
    for (int k = 0; k < 3; ++k) {
      if (brake_in <= p.brake_input_in[k + 1] || k == 2) {
        brake_curve_raw = lerp(p.brake_input_out[k], p.brake_input_out[k + 1],
                               ramp(brake_in, p.brake_input_in[k], p.brake_input_in[k + 1]));
        break;
      }
    }
  }
  const double decel_scale = p.use_per_car_brakes && !ai_ ? decel_table_at(speed()) : 1.0;
  const double brake_frac = p.use_per_car_brakes
                                ? (ai_ ? brake_in : brake_curve_raw * decel_scale)
                                : (ai_ ? brake_in * p.ai_brake_scale
                                       : brake_curve_raw / std::max(1e-6, p.brake_input_out[3]));
  // Stability control (STM, SPEC-CAR-PHYSICS, Stability control): asymmetric
  // outside-of-slide brake demand when moving forward above StabilityManageSpeed.
  double stm_wheel_delta[kWheels]{};
  if (!ai_ && assists_.stability_control && speed() >= 0) {
    double surf_scale = 0;
    int grounded = 0;
    for (int i = 0; i < kWheels; ++i)
      if (w_[i].contact) surf_scale += surf[i].tcs_speed_scale, ++grounded;
    surf_scale = grounded > 0 ? surf_scale / grounded : 1.0;
    const double v_thresh = p.stm_speed_mph * 0.44704 * surf_scale;
    const double v2 = dot(vel_, vel_);
    if (v2 >= v_thresh * v_thresh) {
      const double mean_rear_sa = 0.5 * (w_[2].slip_angle + w_[3].slip_angle);
      if (std::fabs(mean_rear_sa) >= p.stm_slip_min) {
        const double amt = std::clamp(
            (std::fabs(mean_rear_sa) - p.stm_slip_min) / std::max(1e-3, p.stm_slip_max - p.stm_slip_min), 0.0, 1.0);
        stm_flag_ = true;
        stm_amount_ = std::max(stm_amount_, amt);
        if (mean_rear_sa > 0) {
          stm_wheel_delta[1] = stm_wheel_delta[3] = amt;
          stm_wheel_delta[0] = stm_wheel_delta[2] = -0.5 * amt;
        } else {
          stm_wheel_delta[0] = stm_wheel_delta[2] = amt;
          stm_wheel_delta[1] = stm_wheel_delta[3] = -0.5 * amt;
        }
      }
    }
  }
  const double stick = std::fabs(shaped_.steer);
  const double G = ratio(gear_);
  double brake[kWheels]{};
  bool slowing[kWheels]{};
  const int hist_n = std::clamp(p.abs_smoothing, 1, 8);
  for (int i = 0; i < kWheels; ++i) {
    WheelState& w = w_[i];
    const int a = axle_of(i);
    double frac = std::clamp(brake_frac + stm_wheel_delta[i], 0.0, 1.0);
    const bool no_abs_reg = !abs_on && !ai_ && brake_in >= p.no_abs_brake_travel;
    if ((abs_on || no_abs_reg) && frac > 0) {
      if (abs_timer_[i] <= 0 && w.contact) {
        double mean = 0;
        for (int k = 0; k < hist_n; ++k) mean += slip_hist_[i][k];
        mean /= hist_n;
        double release = 1.0;
        double dur = p.abs_duration;
        if (ai_) {
          release = p.abs_ai_release;
          dur = kStep;
        } else if (abs_on) {
          release = a == 0 ? lerp(p.abs_front_no_steer, p.abs_front_full_steer, stick)
                           : lerp(p.abs_rear_no_steer, p.abs_rear_full_steer, stick);
        } else {
          release = a == 0 ? lerp(p.no_abs_front_no_steer, p.no_abs_front_full_steer, stick)
                           : lerp(p.no_abs_rear_no_steer, p.no_abs_rear_full_steer, stick);
          dur = p.no_abs_disable_time;
        }
        if ((ai_ ? w.slip_ratio : mean) < -release) abs_timer_[i] = dur > 0 ? dur : dt;
      }
      if (abs_timer_[i] > 0) {
        abs_scale_[i] = 0.0;
        frac = 0.0;
        if (abs_on) abs_flag_ = true;
      } else if (abs_scale_[i] < 1.0) {
        // SPEC-CAR-PHYSICS, ABS: linear brake pressure recovery proportional to
        // (wheel_inertia / R) / 2.4516625 after release hold expires.
        const double rec = (p.wheel_inertia / std::max(0.1, p.tyre_radius[i])) / 2.4516625;
        abs_scale_[i] = std::min(1.0, abs_scale_[i] + rec * 12.0 * dt);
        frac *= abs_scale_[i];
      }
    } else {
      abs_scale_[i] = 1.0;
    }
    if (abs_timer_[i] > 0) abs_timer_[i] -= dt;
    w.brake = frac;
    if (p.use_per_car_brakes) {
      double t_trace = frac * brake_const_[a];
      if (a == 1 && handbrake_shaped_ > 0) t_trace += handbrake_const_ * handbrake_shaped_;
      brake[i] = p.mass * (kKgf / 4.0) * p.tyre_radius[i] * t_trace;
    } else {
      brake[i] = frac * (a == 0 ? p.front_brake_torque : p.rear_brake_torque);
      if (a == 1 && shaped_.handbrake) brake[i] += p.handbrake_torque;
    }
    slowing[i] = brake_frac > 0 || brake[i] > 0 || tc_prev_ * G * weight_[i] * w.omega < 0;
  }

  // 3. Tyres: forces at the current wheel speeds, with the longitudinal
  // force's slope against wheel speed for a linearised-implicit spin update.

  Vec3 fwd[kWheels]{}, lat[kWheels]{};
  double vx[kWheels]{}, vy[kWheels]{}, fx0[kWheels]{}, fy[kWheels]{}, slope[kWheels]{};
  auto tyre = [&](int i, double omega, double* fx_out, double* fy_out, double* sx_out, double* sy_out) {
    const int a = axle_of(i);
    const double n = w_[i].load, kgf = n / kKgf, stretch = p.sidewall_stretch[a];
    const double sig_pk = std::max(1e-4, p.longitudinal.peak_slip(kgf, stretch));
    const double alpha_pk = std::clamp(p.lateral.peak_slip(kgf, stretch), p.lateral_peak_min_deg, p.lateral_peak_max_deg);
    // An off-road surface moves the normalising peak towards its own; the
    // curve is still read at the road's (SPEC-CAR-PHYSICS, Off-road surfaces).
    const double alpha_norm = alpha_pk + surf[i].off_roadness * (surf[i].off_road_peak_sa - alpha_pk);
    const double denom = std::max(std::fabs(vx[i]), p.slip_speed_floor);
    const double sigma = (omega * p.tyre_radius[i] - vx[i]) / denom;
    // Below the floor speed the angle becomes the linear term v_y / v_min.
    const double alpha = std::atan2(vy[i], denom) * 180 / kPi;
    double sx = sigma / sig_pk;
    const double sy = alpha / std::max(alpha_norm, 1e-3);
    // SPEC-CAR-PHYSICS, RWD Launch Assist (EnableRWDLaunchAssist): damps
    // longitudinal slip ratio on RWD rear wheels below 11 m/s when sx > 1.7.
    if (!ai_ && p.enable_rwd_launch_assist && p.layout == CarParams::Layout::kRear && a == 1 &&
        std::fabs(vx[i]) < 11.0 && sx > 1.7) {
      const double launch_fade = 1.0 - ramp(std::fabs(vx[i]), 5.0, 11.0);
      sx = 1.7 + (sx - 1.7) * lerp(1.0, 0.35, launch_fade);
    }
    const double s = std::max(std::sqrt(sx * sx + sy * sy), 1e-4);
    // Each curve times its row's FrictionScale, blended by load, times the
    // width scale.
    const double mu_x = p.longitudinal.value(s * sig_pk, kgf, stretch, p.long_curve_scale) * p.width_scale[a];
    const double mu_y = p.lateral.value(s * alpha_pk, kgf, stretch, p.lat_curve_scale) * p.width_scale[a];
    const double rear = a == 1 ? surf[i].rear_grip : 1.0;
    // SPEC-CAR-PHYSICS, Handbrake: multiply rear k_x and k_y by the surface's
    // HandbrakeGripMultiplier while the handbrake is held.
    const double hb_grip =
        (a == 1 && handbrake_shaped_ > 0.0) ? lerp(1.0, surf[i].handbrake_grip, handbrake_shaped_) : 1.0;
    // The AI's grip factor on dirt (an off-road surface): the rear's
    // excess over 1 is 1.1 x the front's.
    const double dirt = ai_ && dirt_rule_ ? std::clamp(1 + (a == 0 ? 0.5 : 0.55) * dirt_k_, 0.0, 2.0) : 1.0;
    const double ai = ai_grip_[a] * dirt;
    // Braking friction on a slowed wheel, else the speed-blended accel
    // scale (SPEC-CAR-PHYSICS, Brakes: Braking friction).
    double kx = grip[i] * rear * hb_grip * ai * p.long_scale[a] * p.long_misc_scale;
    kx *= slowing[i] ? (p.brake_friction_scale * (abs_off_boost ? p.abs_off_braking_friction_scale : 1.0))
                     : lerp(p.accel_scale0, p.accel_scale1, ramp(std::fabs(vx[i]), p.accel_speed0, p.accel_speed1));
    double ky = grip[i] * rear * hb_grip * ai * p.lat_scale[a] * p.lat_compound_scale * p.long_misc_scale;
    // The rear-grip fix: g + d (1 - g), g the scale faded out by the
    // normalised slip angle, d by the road wheels' steer angle in degrees.
    if (a == 1) {
      const double g = lerp(p.rear_fix_scale, 1.0, ramp(std::fabs(sy), p.fix_slip0, p.fix_slip1));
      const double d = ramp(std::fabs(steer_angle_) * 180 / kPi, p.fix_steer0, p.fix_steer1);
      ky *= g + d * (1 - g);
    }
    if (fx_out) *fx_out = mu_x * (sx / s) * n * kx;
    if (fy_out) *fy_out = -mu_y * (sy / s) * n * ky;
    if (sx_out) *sx_out = sx;
    if (sy_out) *sy_out = sy;
  };
  for (int i = 0; i < kWheels; ++i) {
    WheelState& w = w_[i];
    if (!w.contact || w.load <= 0) {
      w.slip_ratio = w.slip_angle = 0;
      continue;
    }
    const Vec3 heading_local{std::sin(w.steer), 0, std::cos(w.steer)};
    Vec3 f = rotate(rot_, heading_local);
    fwd[i] = normalized(f - normal[i] * dot(f, normal[i]));
    lat[i] = normalized(cross(normal[i], fwd[i]));  // +x on flat ground with fwd +z
    const Vec3 v_contact = vel_ + cross(ang_, contact[i] - pos_);
    vx[i] = dot(v_contact, fwd[i]);
    vy[i] = dot(v_contact, lat[i]);
    tyre(i, w.omega, &fx0[i], &fy[i], &w.slip_ratio, &w.slip_angle);
    const double d_omega = 1e-3 * std::max(1.0, std::fabs(w.omega));
    double fx1 = 0;
    tyre(i, w.omega + d_omega, &fx1, nullptr, nullptr, nullptr);
    slope[i] = std::max(0.0, (fx1 - fx0[i]) / d_omega);
  }
  for (int i = 0; i < kWheels; ++i) slip_hist_[i][slip_pos_] = w_[i].slip_ratio;
  slip_pos_ = (slip_pos_ + 1) % hist_n;

  // 4. Drivetrain: the engine, the clutch and the wheels. A locked clutch's
  // torque is solved so the engine and the diff-weighted wheel speed stay
  // together; past its capacity it slips at the capacity. GameTorqueScale
  // scales only the drive torque passed to the wheels (not T_engine or
  // open-clutch engine acceleration).
  // Traction control (TCS, SPEC-CAR-PHYSICS, Traction control): engine torque
  // cut (1 - tcs_amount) and launch wheel-omega trim when driven slip exceeds
  // the speed/steer/yaw-adjusted target.
  double tcs_cut = 1.0;
  double tcs_avg_slip = 0.0;
  if (!ai_ && assists_.traction_control && drive_in > 0.05 && G != 0) {
    double sum_sr = 0, sum_sa = 0, sum_surf = 0;
    int n_drv = 0;
    for (int i = 0; i < kWheels; ++i) {
      if (weight_[i] > 0 && w_[i].contact) {
        sum_sr += std::max(0.0, w_[i].slip_ratio * (G > 0 ? 1.0 : -1.0));
        sum_sa += std::fabs(w_[i].slip_angle);
        sum_surf += surf[i].tcs_speed_scale;
        ++n_drv;
      }
    }
    if (n_drv > 0) {
      const double avg_sr = sum_sr / n_drv;
      const double avg_sa = sum_sa / n_drv;
      const double avg_surf = sum_surf / n_drv;
      tcs_avg_slip = avg_sr;
      const double v_thresh = p.tcs_speed_mph * 0.44704 * avg_surf;
      const double s_base =
          dot(vel_, vel_) > 0.19984475 * v_thresh * v_thresh ? p.tcs_slip_moving : p.tcs_slip_takeoff;
      const double steer_scale = p.layout == CarParams::Layout::kAll
                                     ? p.tcs_steer_scale_awd
                                     : (p.layout == CarParams::Layout::kFront ? p.tcs_steer_scale_fwd
                                                                              : p.tcs_steer_scale_rwd);
      const double s_target =
          s_base * lerp(1.0, steer_scale, std::clamp(avg_sa, 0.0, 1.0)) + p.tcs_fric_diff * avg_surf;
      // Yaw angle fade over [YawMinAngleForTCS, YawMaxAngleForTCS].
      const Vec3 fwd_w = rotate(rot_, {0, 0, 1});
      const double sp_xz = std::hypot(vel_.x, vel_.z);
      double yaw_fade = 1.0;
      if (sp_xz > 1.0) {
        const double cos_y = std::clamp((fwd_w.x * vel_.x + fwd_w.z * vel_.z) / sp_xz, -1.0, 1.0);
        const double yaw_deg = std::acos(cos_y) * 180.0 / kPi;
        yaw_fade = 1.0 - ramp(yaw_deg, p.tcs_yaw_min_deg, p.tcs_yaw_max_deg);
      }
      const double amt =
          std::clamp((avg_sr - s_target) / std::max(0.1, p.tcs_fric_diff), 0.0, 1.0) * yaw_fade;
      if (amt >= 0.1) tcs_flag_ = true;
      tcs_amount_ = std::max(tcs_amount_, amt);
      tcs_cut = 1.0 - amt;
    }
  }
  update_boost(dt, drive_in, rpm());
  const double cap = G != 0 ? p.clutch_max_torque * p.clutch_torque_factor * clutch_ : 0;
  const double je = p.engine_inertia;
  const double te_raw = engine_torque(rpm(), drive_in);
  const double te = te_raw > 0 ? te_raw * tcs_cut : te_raw;
  engine_torque_prev_ = te;
  const double reflected = G != 0 ? p.gearbox_inertia * G * G + p.driveline_inertia * p.final_drive * p.final_drive : 0;
  double j_eff[kWheels]{}, resist[kWheels]{};
  // Locked clutch: (J_w + G^2 J_e) a_w = GameTorqueScale * G * T_e - R,
  // preserving full reflected engine inertia G^2 J_e while scaling T_e by
  // GameTorqueScale (SPEC-CAR-PHYSICS, Engine item 5).
  double num = (p.game_torque_scale * te) / je, den = 1 / je;
  for (int i = 0; i < kWheels; ++i) {
    // The tyre's torque back on the wheel is F_x x r (SPEC-CAR-PHYSICS);
    // braking torque opposes spin continuously via clamp(omega / pi, -1, 1).
    j_eff[i] = p.wheel_inertia + reflected * weight_[i] + dt * p.tyre_radius[i] * slope[i];
    resist[i] = p.tyre_radius[i] * fx0[i] + std::clamp(w_[i].omega / kPi, -1.0, 1.0) * brake[i];
  }
  // Limited-slip differentials (SPEC-CAR-PHYSICS, AWD and LSDs, confirmed):
  // RelVelClamp is in rpm (* pi/30 -> rad/s). Every diff evaluates against the
  // same unsplit signed T_drive (so reverse and engine braking select T_decel).
  double lsd[kWheels]{};
  auto lsd_lock = [&](const CarParams::LsdParams& d, double t_in, double dw, double max_step_t) {
    if (d.accel <= 0 && d.decel <= 0) return 0.0;
    const double lo = std::min(0.0, d.accel_def_in);
    const double hi = std::max(0.0, d.accel_def_in);
    const double t_max = (hi <= lo) ? d.accel : lerp(d.decel, d.accel, std::clamp((t_in - lo) / (hi - lo), 0.0, 1.0));
    const double clamp_rad = std::max(1e-3, d.rel_vel_clamp / kRpm);
    const double t = std::clamp(dw / clamp_rad, -1.0, 1.0) * t_max;
    return std::clamp(t, -max_step_t, max_step_t);
  };
  const double t_in_total = G != 0 ? tc_prev_ * G * p.game_torque_scale : 0.0;
  if (p.layout == CarParams::Layout::kAll) {
    const double r_ratio = p.tyre_radius[2] > 1e-6 ? p.tyre_radius[0] / p.tyre_radius[2] : 1.0;
    const double dw_c = 0.5 * (w_[0].omega + w_[1].omega) * r_ratio - 0.5 * (w_[2].omega + w_[3].omega);
    const double jf = j_eff[0] + j_eff[1], jr = j_eff[2] + j_eff[3];
    // Per-wheel torque t_c transfers 2 * t_c axle-to-axle.
    const double max_c = 0.5 * std::fabs(dw_c) / (dt * (1.0 / jf + 1.0 / jr));
    const double t_c = lsd_lock(p.center_lsd, t_in_total, dw_c, max_c);
    lsd[0] -= t_c;
    lsd[1] -= t_c;
    lsd[2] += t_c;
    lsd[3] += t_c;
    const double dw_f = w_[0].omega - w_[1].omega;
    const double max_f = std::fabs(dw_f) / (dt * (1.0 / j_eff[0] + 1.0 / j_eff[1]));
    const double t_f = lsd_lock(p.front_lsd, t_in_total, dw_f, max_f);
    lsd[0] -= t_f;
    lsd[1] += t_f;
    const double dw_r = w_[2].omega - w_[3].omega;
    const double max_r = std::fabs(dw_r) / (dt * (1.0 / j_eff[2] + 1.0 / j_eff[3]));
    const double t_r = lsd_lock(p.rear_lsd, t_in_total, dw_r, max_r);
    lsd[2] -= t_r;
    lsd[3] += t_r;
  } else if (p.layout == CarParams::Layout::kFront) {
    const double dw_f = w_[0].omega - w_[1].omega;
    const double max_f = std::fabs(dw_f) / (dt * (1.0 / j_eff[0] + 1.0 / j_eff[1]));
    const double t_f = lsd_lock(p.front_lsd, t_in_total, dw_f, max_f);
    lsd[0] -= t_f;
    lsd[1] += t_f;
  } else {
    const double dw_r = w_[2].omega - w_[3].omega;
    const double max_r = std::fabs(dw_r) / (dt * (1.0 / j_eff[2] + 1.0 / j_eff[3]));
    const double t_r = lsd_lock(p.rear_lsd, t_in_total, dw_r, max_r);
    lsd[2] -= t_r;
    lsd[3] += t_r;
  }
  for (int i = 0; i < kWheels; ++i) {
    resist[i] -= lsd[i];
    num += G * weight_[i] * resist[i] / j_eff[i];
    den += G * G * weight_[i] * weight_[i] / j_eff[i];
  }
  const double t_lock_wheel_side = num / den;  // equivalent engine-side torque * GameTorqueScale
  const double t_lock = t_lock_wheel_side / std::max(1e-6, p.game_torque_scale);
  const double slip = engine_omega_ - G * input_omega();
  double tc = 0;
  double tc_wheel_side = 0;
  bool lock_now = false;
  if (cap > 0) {
    if ((locked_ || std::fabs(slip) < 0.5) && std::fabs(t_lock) <= cap) {
      tc = t_lock;
      tc_wheel_side = t_lock_wheel_side;
      lock_now = true;
    } else {
      tc = (slip != 0 ? sign(slip) : sign(t_lock)) * cap;
      tc_wheel_side = tc * p.game_torque_scale;
    }
  }
  double omega_e = engine_omega_ + dt * (te - tc) / je;
  double omega_new[kWheels];
  for (int i = 0; i < kWheels; ++i) {
    WheelState& w = w_[i];
    double o = w.omega + dt * (tc_wheel_side * G * weight_[i] + lsd[i] - p.tyre_radius[i] * fx0[i]) / j_eff[i];
    // TCS stage 2: wheel speed trim during launch when driven slip exceeds takeoff target.
    if (tcs_cut < 1.0 && state_ == GearState::kLaunch && weight_[i] > 0 && tcs_avg_slip > p.tcs_slip_takeoff) {
      const double trim = (1.0 - p.tcs_slip_takeoff / std::max(1e-3, tcs_avg_slip)) *
                          std::fabs(tc_wheel_side * G * weight_[i]) * dt / std::max(0.1, p.wheel_inertia);
      o -= sign(o) * std::min(std::fabs(o), trim);
    }
    const double b = brake[i] * dt / j_eff[i];
    o = std::fabs(o) <= b ? 0 : o - sign(o) * b;
    if (!w.contact) o *= 1 - 0.5 * dt;  // bearing drag in the air
    omega_new[i] = o;
  }
  double in_new = 0;
  for (int i = 0; i < kWheels; ++i) in_new += weight_[i] * omega_new[i];
  if (cap > 0 && !lock_now && sign(omega_e - G * in_new) != sign(slip)) lock_now = true;  // synchronised
  if (lock_now) omega_e = G * in_new;
  // SPEC-CAR-PHYSICS, StallRPM idle clamp: prevents T_closed < 0 at
  // throttle = 0 from stalling below StallRPM.
  const double w_stall = p.idle_rpm / kRpm;
  if (omega_e <= w_stall) {
    lock_now = false;
    clutch_ = 0;
    if (state_ == GearState::kEngaged) {
      state_ = GearState::kOpen;
      state_time_ = 0;
    }
    omega_e = w_stall;
  }
  const double w_red = p.redline_rpm / kRpm;
  if (!lock_now && omega_e > w_red) omega_e = w_red;
  locked_ = lock_now;
  tc_prev_ = tc;
  engine_omega_ = omega_e;

  // 5. Forces on the body.
  for (int i = 0; i < kWheels; ++i) {
    WheelState& w = w_[i];
    if (w.contact && w.load > 0) {
      // The tyre at the new wheel speed: the linearisation only steadies
      // the spin update; past the curve's linear part it would overshoot.
      double fx = 0, fy_new = 0;
      tyre(i, omega_new[i], &fx, &fy_new, nullptr, nullptr);
      // Surface drag (SPEC-CAR-PHYSICS, VelDepFriction): slew-clamped
      // velocity-opposing forces along wheel local axes.
      if (p.aero_drag_long > 0) {
        const double u_x = ramp(std::fabs(vx[i]), surf[i].vel_peak0, surf[i].vel_peak);
        const double u_y = ramp(std::fabs(vy[i]), surf[i].vel_peak0, surf[i].vel_peak);
        const double cross_u = std::clamp(0.25 * (std::fabs(vx[i]) - 4.0), 0.0, 1.0);
        const double tgt_long = lerp(surf[i].vel_long0, surf[i].vel_long, u_x);
        const double tgt_lat = lerp(surf[i].vel_lat0, surf[i].vel_lat, u_y) * cross_u;
        const double slew = p.max_vel_dep_coeff_vel * dt;
        vel_dep_long_[i] = approach(vel_dep_long_[i], tgt_long, p.max_vel_dep_coeff_vel, dt);
        vel_dep_lat_[i] = approach(vel_dep_lat_[i], tgt_lat, p.max_vel_dep_coeff_vel, dt);
        (void)slew;
        const double fx_vd = -sign(vx[i]) * std::min(1.0, std::fabs(vx[i]) / 0.2) * vel_dep_long_[i] * w.load;
        const double fy_vd = -sign(vy[i]) * std::min(1.0, std::fabs(vy[i]) / 0.2) * vel_dep_lat_[i] * w.load;
        fx += fx_vd;
        fy_new += fy_vd;
      }
      const Vec3 f = normal[i] * w.load + fwd[i] * fx + lat[i] * fy_new;
      w.fx = fx;
      force = force + f;
      torque = torque + cross(contact[i] - pos_, f);
      // The longitudinal force pitches the body through the measured arm
      // instead of the contact patch's height below the centre of mass
      // (SPEC-CAR-PHYSICS scenario B, load transfer): the same force at a
      // point raised along the body's up axis.
      // The lateral force rolls it through its own arm likewise.
      const double below = dot(pos_ - contact[i], up);
      if (p.longitudinal_pitch_arm > 0 && below > p.longitudinal_pitch_arm)
        torque = torque + cross(up * (below - p.longitudinal_pitch_arm), fwd[i] * fx);
      if (p.lateral_roll_arm > 0 && below > p.lateral_roll_arm)
        torque = torque + cross(up * (below - p.lateral_roll_arm), lat[i] * fy_new);
    }
    if (!w.contact || w.load <= 0) w.fx = 0;
    w.omega = omega_new[i];
    w.spin = std::fmod(w.spin + w.omega * dt, 2 * kPi);
  }

  // Body contacts: 26 points on the box (corners, edge and face middles),
  // each probed by a ray from the box's centre. A point past the first
  // surface along its ray is pushed out along that surface's normal, so
  // floors, walls, barriers and ceilings all act on the body. The force per
  // point is scaled so that the eight corners carry what they did when they
  // were the only contacts.
  body_force_ = 0;
  constexpr int kPoints = 26;
  constexpr double kShare = 8.0 / kPoints;
  const Vec3 centre_local{0, p.sill_height + 0.5 * p.size.y, p.centre_of_mass.z};
  const Vec3 centre = pos_ + rotate(rot_, centre_local - com_local_);
  for (int k = 0; k < 27; ++k) {
    const int ix = k % 3 - 1, iy = k / 3 % 3 - 1, iz = k / 9 - 1;
    if (ix == 0 && iy == 0 && iz == 0) continue;
    const Vec3 point_local = centre_local + Vec3{0.5 * ix * p.size.x, 0.5 * iy * p.size.y, 0.5 * iz * p.size.z};
    const Vec3 point = pos_ + rotate(rot_, point_local - com_local_);
    const Vec3 ray = point - centre;
    const double reach = length(ray);
    const Vec3 dir = ray * (1 / reach);
    Hit hit;
    if (!ground.raycast(centre, dir, reach, hit)) continue;
    // The normal faces against the ray: out of the surface, towards the box.
    const Vec3 n = dot(hit.normal, dir) > 0 ? hit.normal * -1.0 : hit.normal;
    const double depth = (reach - hit.distance) * -dot(dir, n);
    const Vec3 v = vel_ + cross(ang_, point - pos_);
    const double vn = dot(v, n);
    const double damp_scale = body_contact_damping_scale(hit, n);
    const double fn = kShare * std::max(0.0, p.mass * (p.body_stiffness * depth - (p.body_damping * damp_scale) * vn));
    if (fn <= 0) continue;
    Vec3 f = n * fn;
    const Vec3 vt = v - n * vn;
    const double vt_len = length(vt);
    if (vt_len > 1e-6) {
      const double mu = body_contact_friction(hit, n, p.body_friction);
      const double ft = std::min(mu * fn, p.mass / kPoints * vt_len / dt);
      f = f - vt * (ft / vt_len);
    }
    force = force + f;
    torque = torque + cross(point - pos_, f);
    body_force_ += length(f);
    if (fn > strongest_.force) strongest_ = {centre + dir * hit.distance, n, fn, hit.surface, hit.triangle};
  }

  // Aerodynamics (SPEC-CAR-PHYSICS, Aerodynamics): 3D body drag, lateral drag
  // at front/rear axles, vertical drag, front/rear downforce with pitch AoA
  // falloff, high-speed drag ramp, and active drafting multipliers.
  if (p.aero_drag_long > 0) {
    const Vec3 vl = rotate_inverse(rot_, vel_);
    const double hs = lerp(p.high_speed_drag_s0, p.high_speed_drag_s1,
                           ramp(std::fabs(vl.z), p.high_speed_drag_v0, p.high_speed_drag_v1));
    const double c_long = vl.z >= 0 ? p.aero_drag_long * hs * draft_.body_drag +
                                          p.aero_part_drag_front * draft_.front_drag +
                                          p.aero_part_drag_rear * draft_.rear_drag
                                    : p.aero_drag_long * hs;
    const double fz = -sign(vl.z) * c_long * vl.z * vl.z;
    const double fy_drag = -sign(vl.y) * p.aero_drag_vert * vl.y * vl.y;
    const double fx_f = -sign(vl.x) * p.aero_drag_lat_front * vl.x * vl.x;
    const double fx_r = -sign(vl.x) * (p.aero_drag_lat_rear + p.aero_part_lat_drag) * vl.x * vl.x;
    double df_f = 0, df_r = 0;
    if (vl.z > 0.1) {
      const double alpha_pitch = std::atan2(vl.y, vl.z);
      const double aoa = std::max(0.0, 1.0 - std::fabs(alpha_pitch) / std::max(0.05, p.aero_zero_df_rad));
      df_f = (p.aero_df_front + p.aero_part_df_front) * aoa * vl.z * vl.z * draft_.front_df;
      df_r = (p.aero_df_rear + p.aero_part_df_rear) * aoa * vl.z * vl.z * draft_.rear_df;
    }
    const double z_f = 0.5 * (p.wheel_centres[0].z + p.wheel_centres[1].z) - com_local_.z;
    const double z_r = 0.5 * (p.wheel_centres[2].z + p.wheel_centres[3].z) - com_local_.z;
    const Vec3 f_body_local{fx_f + fx_r, fy_drag - df_f - df_r, fz};
    force = force + rotate(rot_, f_body_local);
    // Axle-applied downforce pitches the body; axle-applied lateral drag damps yaw.
    const Vec3 t_aero_local{z_f * df_f + z_r * df_r, z_f * fx_f + z_r * fx_r, 0};
    torque = torque + rotate(rot_, t_aero_local);
  } else {
    // Fallback resistance for synthetic test CarParams.
    const double v_abs = length(vel_);
    if (v_abs > 1e-6) {
      const double r = p.drag_quadratic * v_abs * v_abs + p.drag_constant * std::min(1.0, v_abs);
      force = force - vel_ * (r / v_abs);
    }
  }

  // FrictionTorqueMod yaw stabilization (SPEC-CAR-PHYSICS, Steering assist
  // modes): active for Assisted (0) / Normal (1), disabled in Simulation (2).
  // Applies TurnScale/TurnClamp when widening yaw slip vs StraightenScale/
  // StraightenClamp when recovering, plus FWDModifiers and OffRoadModifiers.
  if (!ai_ && assists_.steering != 2 && off < kWheels) {
    const Vec3 fwd_w = rotate(rot_, {0, 0, 1});
    const double sp_xz = std::hypot(vel_.x, vel_.z);
    if (sp_xz > 5.0) {
      const double cos_yaw = std::clamp((fwd_w.x * vel_.x + fwd_w.z * vel_.z) / sp_xz, -1.0, 1.0);
      if (cos_yaw < 0.996194) {  // > 5 deg yaw slip
        const double u_mass = ramp(p.mass, 909.1, 1590.9);
        const double u_yaw = ramp(0.996194 - cos_yaw, 0.0, 0.996194 - 0.961261);
        const double assist_gain = assists_.steering == 0 ? 1.5 : 0.6;
        // Cross product in XZ indicates slip sign; yaw rate of opposite sign straightens.
        const double yaw_cross = fwd_w.x * vel_.z - fwd_w.z * vel_.x;
        const bool straightening = (ang_.y * yaw_cross) < 0.0;
        const double mode_scale = straightening ? lerp(1.0, 0.8, u_yaw) : lerp(1.0, 0.7, u_yaw);
        const double base_clamp = straightening
                                      ? lerp(lerp(145.0, 140.0, u_yaw), lerp(115.0, 120.0, u_yaw), u_mass)
                                      : lerp(lerp(180.0, 120.0, u_yaw), lerp(150.0, 100.0, u_yaw), u_mass);
        int n_grounded = 0, n_offroad = 0;
        for (int i = 0; i < kWheels; ++i) {
          if (w_[i].contact) {
            ++n_grounded;
            if (surf[i].off_roadness >= 0.5) ++n_offroad;
          }
        }
        const double offroad_frac = n_grounded > 0 ? double(n_offroad) / double(n_grounded) : 0.0;
        const double offroad_mod = lerp(1.0, 1.2, offroad_frac);
        const double fwd_mod = p.layout == CarParams::Layout::kFront ? 0.9 : 1.0;
        const double clamp_t = base_clamp * assist_gain * offroad_mod;
        const double yaw_damp =
            std::clamp(-ang_.y * inertia_.y * 2.5 * mode_scale * fwd_mod * offroad_mod * assist_gain, -clamp_t, clamp_t);
        torque.y += yaw_damp;
      }
    }
  }

  // Integrate (semi-implicit Euler).
  vel_ = vel_ + force * (dt / p.mass);
  const Vec3 tl = rotate_inverse(rot_, torque);
  Vec3 wl = rotate_inverse(rot_, ang_);
  const Vec3 iw{inertia_.x * wl.x, inertia_.y * wl.y, inertia_.z * wl.z};
  const Vec3 gyro = cross(wl, iw);
  wl = wl + Vec3{(tl.x - gyro.x) / inertia_.x, (tl.y - gyro.y) / inertia_.y, (tl.z - gyro.z) / inertia_.z} * dt;
  ang_ = rotate(rot_, wl);
  pos_ = pos_ + vel_ * dt;
  const Quat dq{ang_.x, ang_.y, ang_.z, 0};
  Quat q{rot_.x + 0.5 * dt * (dq.w * rot_.x + dq.x * rot_.w + dq.y * rot_.z - dq.z * rot_.y),
         rot_.y + 0.5 * dt * (dq.w * rot_.y - dq.x * rot_.z + dq.y * rot_.w + dq.z * rot_.x),
         rot_.z + 0.5 * dt * (dq.w * rot_.z + dq.x * rot_.y - dq.y * rot_.x + dq.z * rot_.w),
         rot_.w + 0.5 * dt * (dq.w * rot_.w - dq.x * rot_.x - dq.y * rot_.y - dq.z * rot_.z)};
  const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  rot_ = {q.x / n, q.y / n, q.z / n, q.w / n};
}

void apply_auto_brake(Input& in, double current_speed, double target_speed) {
  if (target_speed <= 0.0) return;
  const double excess = current_speed - target_speed;
  if (excess <= 0.5) return;
  const double auto_brk = std::clamp((excess - 0.5) / 8.0, 0.0, 1.0);
  in.brake = std::max(in.brake, auto_brk);
  in.throttle *= (1.0 - std::clamp((excess - 0.5) / 3.0, 0.0, 1.0));
}

DraftState compute_follower_draft(const DraftCar& follower, const DraftCar* leaders, size_t leader_count) {
  DraftState out;
  constexpr double kDraftSpeed0 = 17.8816;  // 40 mph
  constexpr double kDraftSpeed1 = 67.056;   // 150 mph
  if (follower.speed <= kDraftSpeed0 || !leaders || leader_count == 0) return out;
  const double u_v = std::clamp((follower.speed - kDraftSpeed0) / (kDraftSpeed1 - kDraftSpeed0), 0.0, 1.0);
  const double start_z_box = 5.0;
  const double start_z_fade = lerp(6.0, 9.0, u_v);
  const double end_z_fade = lerp(30.0, 60.0, u_v);
  const Vec3 fwd = normalized(follower.forward);
  const Vec3 right = normalized(cross({0, 1, 0}, fwd));
  double d = 0.0;
  for (size_t i = 0; i < leader_count; ++i) {
    const Vec3 rel = leaders[i].position - follower.position;
    const double z = dot(rel, fwd);
    if (z <= 0.0 || z >= end_z_fade) continue;
    const double x = std::fabs(dot(rel, right));
    const double narrow = z < start_z_box ? z / start_z_box : 1.0;
    const double start_x = 1.0 * leaders[i].width * narrow;
    const double end_x = 2.5 * leaders[i].width * narrow;
    if (x >= end_x || end_x <= start_x) continue;
    const double w_z = std::clamp((end_z_fade - z) / (end_z_fade - start_z_fade), 0.0, 1.0);
    const double w_x = std::clamp((end_x - x) / (end_x - start_x), 0.0, 1.0);
    d = std::max(d, w_z * w_x);
  }
  if (d <= 0.0) return out;
  out.strength = d;
  auto scale = [&](double s40, double s150) { return 1.0 + d * (lerp(s40, s150, u_v) - 1.0); };
  out.body_drag = scale(0.93, 0.72);
  out.front_drag = scale(0.93, 0.72);
  out.rear_drag = scale(0.93, 0.72);
  out.front_df = scale(0.93, 0.47);
  out.rear_df = scale(0.97, 0.61);
  const double v2 = follower.speed * follower.speed;
  const double df_saved =
      std::fabs(follower.front_df_coeff * v2 * (1.0 - out.front_df) + follower.rear_df_coeff * v2 * (1.0 - out.rear_df));
  out.score = std::clamp(df_saved, 0.0, 1000.0);
  return out;
}

DraftState compute_leader_draft(double strength, double speed) {
  DraftState out;
  constexpr double kDraftSpeed0 = 17.8816;  // 40 mph
  if (speed <= kDraftSpeed0 || strength <= 0.0) return out;
  const double d = std::clamp(strength, 0.0, 1.0);
  out.strength = d;
  out.body_drag = 1.0 + d * (0.96 - 1.0);
  out.front_drag = 1.0 + d * (0.96 - 1.0);
  out.rear_drag = 1.0 + d * (0.96 - 1.0);
  out.front_df = 1.0 + d * (0.99 - 1.0);
  out.rear_df = 1.0 + d * (0.95 - 1.0);
  return out;
}

std::string trace_header() {
  std::string h = "t,x,y,z,vx,vy,vz,speed,yaw,pitch,roll,rpm,gear,steer,throttle,brake,body,clutch,abs";
  for (int i = 0; i < kWheels; ++i) {
    const std::string n = std::to_string(i);
    h += ",comp" + n + ",omega" + n + ",load" + n + ",sx" + n + ",sy" + n + ",brk" + n + ",fx" + n;
  }
  return h;
}

std::string trace_row(const Vehicle& v) {
  char buf[128];
  std::string row;
  auto add = [&](double x) {
    std::snprintf(buf, sizeof buf, "%s%.9g", row.empty() ? "" : ",", x);
    row += buf;
  };
  const Vec3 p = v.position(), vel = v.velocity();
  const Vec3 fwd = rotate(v.orientation(), {0, 0, 1}), up = rotate(v.orientation(), {0, 1, 0});
  const Vec3 right = rotate(v.orientation(), {1, 0, 0});
  add(v.time());
  for (double x : {p.x, p.y, p.z, vel.x, vel.y, vel.z, v.speed()}) add(x);
  add(std::atan2(fwd.x, fwd.z));
  add(std::asin(std::clamp(fwd.y, -1.0, 1.0)));
  add(std::asin(std::clamp(right.y, -1.0, 1.0)) * (up.y < 0 ? -1 : 1));
  add(v.rpm());
  add(v.gear());
  add(v.shaped_input().steer);
  add(v.shaped_input().throttle);
  add(v.shaped_input().brake);
  add(v.body_contact_force());
  add(v.clutch());
  add(v.abs_active());
  for (const auto& w : v.wheels()) {
    add(w.compression);
    add(w.omega);
    add(w.load);
    add(w.slip_ratio);
    add(w.slip_angle);
    add(w.brake);
    add(w.fx);
  }
  return row;
}

std::string physics_trace_event(const Vehicle& v, int64_t car_id, double clock_hz, int mode) {
  // gear: 0 reverse, 1..8 forward, 9 neutral.
  const int gear = v.gear() == -1 ? 0 : v.gear() == 0 ? 9 : v.gear();
  char buf[320];
  std::snprintf(buf, sizeof buf,
                "{\"event\":\"fh1.physics_trace\",\"frame\":%lld,\"t_ms\":%.3f,\"mode\":%d,"
                "\"cars\":\"0:%lld:0:%.4f:%.1f:%d:%.4f:%.4f:%.4f\"}",
                static_cast<long long>(std::llround(v.time() * clock_hz)), v.time() * 1000, mode,
                static_cast<long long>(car_id), std::fabs(v.speed()), v.rpm(), gear, v.shaped_input().throttle,
                v.shaped_input().brake, v.shaped_input().steer);
  return buf;
}

bool parse_script(const std::string& text, std::vector<ScriptLine>& out, std::string* error) {
  out.clear();
  std::istringstream in(text);
  in.imbue(std::locale::classic());
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::istringstream l(line);
    l.imbue(std::locale::classic());
    ScriptLine s;
    if (!(l >> s.time)) continue;  // blank
    int hand = 0;
    if (!(l >> s.input.steer >> s.input.throttle >> s.input.brake)) {
      if (error) *error = "script line " + std::to_string(number) + ": need time steer throttle brake";
      return false;
    }
    if (l >> hand) s.input.handbrake = hand != 0;
    if (!out.empty() && s.time < out.back().time) {
      if (error) *error = "script line " + std::to_string(number) + ": times must not go back";
      return false;
    }
    out.push_back(s);
  }
  return true;
}

Input script_input(const std::vector<ScriptLine>& script, double time) {
  Input in;
  for (const auto& s : script) {
    if (s.time > time + 1e-9) break;
    in = s.input;
  }
  return in;
}

}  // namespace drive
