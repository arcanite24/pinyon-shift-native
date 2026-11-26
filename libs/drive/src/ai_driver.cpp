#include "drive/ai_driver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace drive {

RacingLine RacingLine::from_points(std::span<const float> xs, std::span<const float> zs, double lat_accel, double accel,
                                   double decel, double max_speed, std::span<const float> grip) {
  RacingLine l;
  const size_t n = std::min(xs.size(), zs.size());
  const bool has_grip = grip.size() >= n && n > 0;
  for (size_t i = 0; i < n; ++i) {
    if (!l.x.empty() && std::hypot(xs[i] - l.x.back(), zs[i] - l.z.back()) < 1e-3) continue;
    l.dist.push_back(l.x.empty() ? 0.0f : l.dist.back() + float(std::hypot(xs[i] - l.x.back(), zs[i] - l.z.back())));
    l.x.push_back(xs[i]);
    l.z.push_back(zs[i]);
    if (has_grip) l.surface_grip.push_back(grip[i]);
  }
  const size_t m = l.size();
  l.speed.assign(m, float(max_speed));
  // Curvature from the circle through the points about 5 m either side.
  constexpr float kSpan = 5.0f;
  size_t a = 0, b = 0;
  std::vector<uint8_t> known(m, 0);
  for (size_t i = 0; i < m; ++i) {
    while (a + 1 < i && l.dist[i] - l.dist[a + 1] >= kSpan) ++a;
    b = std::max(b, i);
    while (b + 1 < m && l.dist[b] - l.dist[i] < kSpan) ++b;
    if (a >= i || b <= i) continue;
    known[i] = 1;
    const double ax = l.x[a] - l.x[i], az = l.z[a] - l.z[i], bx = l.x[b] - l.x[i], bz = l.z[b] - l.z[i];
    const double cross = ax * bz - az * bx;
    const double la = std::hypot(ax, az), lb = std::hypot(bx, bz), lc = std::hypot(bx - ax, bz - az);
    const double k = la * lb * lc > 0 ? 2 * std::fabs(cross) / (la * lb * lc) : 0;  // 1 / circumradius
    if (k > 1e-6) l.speed[i] = float(std::min(max_speed, std::sqrt(lat_accel / k)));
  }
  // The ends, short of a whole window, take their nearest neighbour's limit.
  for (size_t i = 1; i < m; ++i)
    if (!known[i] && known[i - 1]) l.speed[i] = l.speed[i - 1], known[i] = 1;
  for (size_t i = m - 1; i-- > 0;)
    if (!known[i] && known[i + 1]) l.speed[i] = l.speed[i + 1], known[i] = 1;
  // What the car can reach: forwards under `accel`, backwards under `decel`.
  for (size_t i = 1; i < m; ++i) {
    const double ds = l.dist[i] - l.dist[i - 1];
    l.speed[i] = float(std::min<double>(l.speed[i], std::sqrt(double(l.speed[i - 1]) * l.speed[i - 1] + 2 * accel * ds)));
  }
  for (size_t i = m - 1; i-- > 0;) {
    const double ds = l.dist[i + 1] - l.dist[i];
    l.speed[i] = float(std::min<double>(l.speed[i], std::sqrt(double(l.speed[i + 1]) * l.speed[i + 1] + 2 * decel * ds)));
  }
  return l;
}

void RacingLine::plan_geometry(std::span<const float> lateral_y) {
  const size_t m = size();
  curvature.assign(m, 0), curvature_v.assign(m, 0), bank_left.assign(m, 0), grade.assign(m, 0);
  if (m < 3) return;
  const bool has_y = y.size() == m;
  // The circle through the points 7 m either side (the spec's points i - 7
  // and i + 7 at its 2 m spacing), by distance so any spacing works.
  constexpr double kHalf = 7.0;
  std::vector<uint8_t> known(m, 0);
  size_t a = 0, b = 0;
  for (size_t i = 0; i < m; ++i) {
    while (a + 1 < i && dist[i] - dist[a + 1] >= kHalf) ++a;
    b = std::max(b, i);
    while (b + 1 < m && dist[b] - dist[i] < kHalf) ++b;
    if (i > 0 && i + 1 < m) {
      const double run = dist[i + 1] - dist[i - 1];
      if (has_y && run > 1e-6) grade[i] = float((y[i + 1] - y[i - 1]) / run);
    }
    if (lateral_y.size() == m) bank_left[i] = float(std::asin(std::clamp(double(lateral_y[i]), -1.0, 1.0)));
    if (a >= i || b <= i || dist[i] - dist[a] < 0.5 * kHalf || dist[b] - dist[i] < 0.5 * kHalf) continue;
    known[i] = 1;
    const double ax = x[a] - x[i], az = z[a] - z[i], bx = x[b] - x[i], bz = z[b] - z[i];
    const double cross = ax * bz - az * bx;  // > 0: turning right
    const double la = std::hypot(ax, az), lb = std::hypot(bx, bz), lc = std::hypot(bx - ax, bz - az);
    if (la * lb * lc > 0) curvature[i] = float(2 * cross / (la * lb * lc));
    if (has_y) {
      const double da = dist[i] - dist[a], db = dist[b] - dist[i];
      curvature_v[i] = float(2 * ((y[b] - y[i]) / db - (y[i] - y[a]) / da) / (da + db));
    }
  }
  for (size_t i = 1; i < m; ++i)
    if (!known[i] && known[i - 1]) curvature[i] = curvature[i - 1], curvature_v[i] = curvature_v[i - 1], known[i] = 1;
  for (size_t i = m - 1; i-- > 0;)
    if (!known[i] && known[i + 1]) curvature[i] = curvature[i + 1], curvature_v[i] = curvature_v[i + 1], known[i] = 1;
}

int AiCapacity::index(double v) { return std::clamp(int(std::floor(0.4 * v)), 0, kEntries - 1); }

double ai_aero_factor(double v) {
  if (v <= 15) return 1;
  if (v >= 75.1) return 0.78;
  return 1 - 0.22 * std::pow((v - 15) / 60.1, 3.8);
}

AiCapacity ai_capacity(const CarParams& p, const AiCapacityCalibration& cal) {
  AiCapacity c;
  c.braking_k = p.layout == CarParams::Layout::kAll ? 0.96f : 0.94f;
  c.sat_level_min = p.layout == CarParams::Layout::kAll ? 0.97f : 0.94f;
  c.is_fwd = p.layout == CarParams::Layout::kFront;
  constexpr double kG = 9.80665;
  const double m = p.mass, W = m * kG;
  const auto& wc = p.wheel_centres;
  const double zf = 0.5 * (wc[0].z + wc[1].z), zr = 0.5 * (wc[2].z + wc[3].z);
  const double wb = std::max(0.5, std::fabs(zf - zr));
  const double share = std::fabs(zf - zr) > 0.1 ? std::clamp((p.centre_of_mass.z - zr) / (zf - zr), 0.05, 0.95) : 0.5;
  const double h = std::max(0.0, p.centre_of_mass.y);
  const double track = std::max(0.5, std::fabs(wc[0].x - wc[1].x));
  const double axle_w[2] = {W * share, W * (1 - share)};
  // An axle's force at its load: both wheels at half of it.
  auto axle = [&](int a, TyrePeak k, double load, double v) {
    load = std::max(0.0, load);
    return load * tyre_peak_mu(p, a, k, load / 2, v);
  };
  const bool driven[2] = {p.layout != CarParams::Layout::kRear, p.layout != CarParams::Layout::kFront};
  // Calibrate to SPEC-RACE-AI measured tables: R34 (1531 kg) needs cal.lateral
  // (16.01/14.6), while ultra-light cars (e.g. Mini 1566, 606 kg -> 16.24 lat,
  // 19.12 brk) are already close to exact in the light-load tyre regime.
  const double mass_blend = std::clamp((m - 600.0) / 500.0, 0.0, 1.0);
  const double lat_cal = 1.02 + (cal.lateral - 1.02) * mass_blend;
  const double brk_cal = 1.015 + (cal.braking - 1.015) * mass_blend;
  for (int n = 0; n < AiCapacity::kEntries; ++n) {
    const double v = 2.5 * n;
    // SPEC-RACE-AI: include aerodynamic downforce scaled by ai_aero_factor(v)
    // so high-downforce cars carry authentic speed through fast sweepers.
    const double aero_k = ai_aero_factor(v);
    const double df_f = std::max(0.0, (p.aero_df_front + p.aero_part_df_front) * v * v * aero_k);
    const double df_r = std::max(0.0, (p.aero_df_rear + p.aero_part_df_rear) * v * v * aero_k);
    const double axle_v[2] = {axle_w[0] + df_f, axle_w[1] + df_r};
    // Braking: load moves forwards by the deceleration x the height over
    // the wheelbase.
    double a = 0;
    for (int pass = 0; pass < 6; ++pass) {
      const double t = std::min(m * a * h / wb, axle_v[1]);  // at most the whole rear on the front
      a = (axle(0, TyrePeak::kBraking, axle_v[0] + t, v) + axle(1, TyrePeak::kBraking, axle_v[1] - t, v)) / m;
    }
    c.braking[size_t(n)] = float(brk_cal * (a + p.drag_quadratic * v * v / m));
    // Lateral: each axle's force moves load inner to outer by the force x
    // the CoM height / the track; the rear capped by the yaw balance.
    double force[2] = {0, 0};
    for (int pass = 0; pass < 6; ++pass) {
      double next[2];
      for (int ax = 0; ax < 2; ++ax) {
        // At most the whole axle on the outer wheel (the inner lifts): past
        // that the transfer would make load from nothing.
        const double dn = std::min(force[ax] * h / track, 0.5 * axle_v[ax]);
        double f = 0;
        for (double side : {0.5 * axle_v[ax] + dn, 0.5 * axle_v[ax] - dn}) {
          side = std::max(0.0, side);
          const double kgf = side / kG;
          const double alpha = std::clamp(p.lateral.peak_slip(kgf, p.sidewall_stretch[size_t(ax)]),
                                          p.lateral_peak_min_deg, p.lateral_peak_max_deg);
          f += side * tyre_peak_mu(p, ax, TyrePeak::kLateral, side, v) * std::cos(alpha * 3.14159265358979 / 180);
        }
        next[ax] = f;
      }
      next[1] = std::min(next[1], (1 - share) / share * next[0]);
      force[0] = next[0], force[1] = next[1];
    }
    c.lateral[size_t(n)] = float(lat_cal * (force[0] + force[1]) / m);
    if (n == 8 && std::getenv("SHIFT_CAPACITY_DEBUG")) {
      // The lateral terms at 20 m/s (SHIFT_CAPACITY_DEBUG=1).
      for (int ax = 0; ax < 2; ++ax) {
        const double kgf = 0.5 * axle_v[ax] / kG;
        std::fprintf(stderr,
                     "capacity: axle %d static %.0f kgf mu %.3f width %.4f lat_scale %.3f compound %.3f misc %.3f "
                     "curve scales %.3f %.3f blend %.3f force %.0f N\n",
                     ax, kgf, tyre_peak_mu(p, ax, TyrePeak::kLateral, 0.5 * axle_v[ax], v), p.width_scale[size_t(ax)],
                     p.lat_scale[size_t(ax)], p.lat_compound_scale, p.long_misc_scale, p.lat_curve_scale[0],
                     p.lat_curve_scale[1], p.lateral.blend(kgf), force[ax]);
      }
      std::fprintf(stderr, "capacity: mass %.0f share %.3f h %.3f track %.3f raw %.2f cal %.3f -> %.2f m/s^2\n", m, share, h,
                   track, (force[0] + force[1]) / m, cal.lateral, c.lateral[size_t(n)]);
    }
    // Forward: the driven axles, load moving rearwards (tyre-limited only).
    a = 0;
    for (int pass = 0; pass < 6; ++pass) {
      const double t = std::min(m * a * h / wb, axle_v[0]);  // at most the whole front on the rear
      double f = 0;
      if (driven[0]) f += axle(0, TyrePeak::kDriving, axle_v[0] - t, v);
      if (driven[1]) f += axle(1, TyrePeak::kDriving, axle_v[1] + t, v);
      a = f / m;
    }
    c.forward[size_t(n)] = float(a);
  }
  return c;
}

double ai_braking_left(double full, double v_lim, double v_corner) {
  return full * std::sqrt(1 - std::min(v_lim * v_lim / std::max(1e-6, v_corner * v_corner), 1.0));
}

double ai_brake_carry(double v, double a, double ds) {
  return v >= 10 ? v - a * ds / v : std::sqrt(std::max(0.0, v * v - 2 * a * ds));
}

double ai_corner_speed(const AiCapacity& cap, double cornering, double kappa, double kappa_v, double theta, double top,
                       double slope_curvature) {
  constexpr double kG = 9.81;
  const double k = std::fabs(kappa);
  if (k < 1e-9 && std::fabs(kappa_v) < 1e-9) return top;
  const double s = std::sin(theta), co = std::cos(theta);
  double v = top;
  int idx = -1;
  for (int it = 0; it < 10; ++it) {
    const int n = AiCapacity::index(v);
    if (n == idx) break;
    idx = n;
    const double mu = cornering * cap.lateral[size_t(n)] / kG;
    const double num = kG * (s + mu * co), den = k * (co - mu * s) - slope_curvature * kappa_v * (s + mu * co);
    // A negative result: no limit.
    const double v2 = den > 0 && num > 0 ? num / den : 1e6;
    v = std::min(top, std::sqrt(v2));
  }
  return v;
}

RacingLine RacingLine::from_oracle(const OracleLine& line) {
  RacingLine l;
  for (const auto& s : line.samples) {
    if (s.t < 0) continue;
    if (!l.dist.empty() && s.distance <= l.dist.back() + 1e-3) {
      l.speed.back() = std::max(l.speed.back(), float(s.speed));
      continue;
    }
    l.x.push_back(float(s.x));
    l.z.push_back(float(s.z));
    l.dist.push_back(float(s.distance));
    l.speed.push_back(float(s.speed));
  }
  return l;
}

size_t RacingLine::segment(double distance, size_t hint) const {
  const size_t n = size();
  if (n < 2) return 0;
  // Fast O(1) check for sequential callers passing a valid segment hint.
  const size_t last = n - 2;
  if (hint <= last && double(dist[hint]) <= distance) {
    if (hint == last || distance < double(dist[hint + 1])) return hint;
    const size_t next = hint + 1;
    if (next == last || distance < double(dist[next + 1])) return next;
  }
  // O(log N) binary search over cumulative arc length.
  const auto it = std::upper_bound(dist.begin(), dist.end(), distance,
                                   [](double val, float elem) { return val < double(elem); });
  const size_t ub = size_t(it - dist.begin());
  return ub <= 1 ? 0 : std::min(ub - 1, last);
}

void RacingLine::pose(double distance, double lane, double& px, double& py, double& pz, double& heading) const {
  px = py = pz = heading = 0;
  if (size() < 2) return;
  const size_t s = segment(distance, 0);
  const double ex = x[s + 1] - x[s], ez = z[s + 1] - z[s];
  const double len = std::hypot(ex, ez);
  const double f = std::clamp((distance - dist[s]) / std::max(1e-6, double(dist[s + 1] - dist[s])), 0.0, 1.0);
  px = x[s] + f * ex + (len > 0 ? lane * ez / len : 0);
  pz = z[s] + f * ez - (len > 0 ? lane * ex / len : 0);
  py = y.size() == size() ? y[s] + f * (y[s + 1] - y[s]) : 0;
  heading = std::atan2(ex, ez);
}

double RacingLine::nearest(double px, double pz, double from, double to) const {
  if (size() < 2) return 0;
  size_t s = segment(from, 0);
  double best = 1e300, at = from;
  for (; s + 1 < size() && dist[s] <= to; ++s) {
    const double ex = x[s + 1] - x[s], ez = z[s + 1] - z[s];
    const double l2 = ex * ex + ez * ez;
    const double f = l2 > 0 ? std::clamp(((px - x[s]) * ex + (pz - z[s]) * ez) / l2, 0.0, 1.0) : 0.0;
    const double dx = px - (x[s] + f * ex), dz = pz - (z[s] + f * ez);
    const double d2 = dx * dx + dz * dz;
    if (d2 < best) best = d2, at = dist[s] + f * (dist[s + 1] - dist[s]);
  }
  return at;
}

double RacingLine::room(double distance, bool right) const {
  const auto& r = right ? room_right : room_left;
  if (r.size() != size() || size() < 2) return 1e9;
  return r[segment(distance, 0)];
}

void ai_traction(const Vehicle& v, float& cap, double dt, Input& in, bool steer_guard) {
  double spin = 0;
  for (const auto& w : v.wheels())
    if (w.contact) spin = std::max(spin, w.slip_ratio);
  if (in.throttle > 0 && spin > 1.15) cap = std::max(0.45f, cap - float(3.5 * dt));
  else cap = std::min(1.0f, cap + float(5.0 * dt));
  double t = std::min(in.throttle, double(cap));
  // The body's slip angle: the velocity's direction in the body frame
  // (z forward, x towards positive steer).
  const Vec3 vb = rotate_inverse(v.orientation(), v.velocity());
  const double sp = std::hypot(vb.x, vb.z);
  if (sp > 5 && vb.z > 0.5) {
    const double beta = std::atan2(vb.x, vb.z);
    const double a = std::fabs(beta);
    constexpr double kDeg = 3.14159265358979 / 180;
    if (a > 7 * kDeg) t *= std::max(0.0, 1 - (a - 7 * kDeg) / (7 * kDeg));
    // Active counter-steer past 7 deg body slip (normal cornering sits under
    // 7 deg): steers into the slide without cutting braking in braking zones.
    const double w = std::clamp((a - 7.0 * kDeg) / (8.0 * kDeg), 0.0, 1.0);
    if (w > 0 && steer_guard) {
      const double lock = std::max(0.05, v.steer_lock());
      const double yaw_damping = -0.08 * v.angular_velocity().y;
      const double counter = std::clamp((beta + yaw_damping) / lock, -1.0, 1.0);
      in.steer = (1 - w) * in.steer + w * counter;
    }
  }
  in.throttle = t;
}

AiCarState ai_state(const Vehicle& v) {
  AiCarState s;
  const Vec3 p = v.position();
  const Vec3 f = rotate(v.orientation(), {0, 0, 1});
  const double h = std::hypot(f.x, f.z);
  s.x = float(p.x);
  s.z = float(p.z);
  s.fwd_x = h > 1e-6 ? float(f.x / h) : 0.0f;
  s.fwd_z = h > 1e-6 ? float(f.z / h) : 1.0f;
  s.speed = float(v.speed());
  {
    const Vec3 vel = v.velocity();
    s.vel_x = float(vel.x), s.vel_z = float(vel.z);
  }
  const auto& c = v.params().wheel_centres;
  s.wheelbase = float(std::fabs((c[0].z + c[1].z) / 2 - (c[2].z + c[3].z) / 2));
  s.steer_lock = float(v.steer_lock());
  {
    const auto& cp = v.params();
    const double sp = std::fabs(v.speed());
    s.max_delta = sp <= 0.1 ? float(cp.max_steer)
                            : float(std::clamp(std::atan(cp.steer_gees_fit * cp.steer_max_gees * cp.gravity * s.wheelbase / (sp * sp)),
                                               std::min(cp.steer_min_lock, cp.max_steer), cp.max_steer));
  }
  s.yaw_rate = float(v.angular_velocity().y);
  s.hold = float(v.hold_throttle());
  for (size_t k = 0; k < 4; ++k) {
    const auto& w = v.wheels()[k];
    s.slip[k] = w.contact ? float(std::hypot(w.slip_ratio, w.slip_angle)) : 0.0f;
  }
  return s;
}

void ai_slip_metrics(const AiCarState& c, double dt, AiPedalState& st) {
  double sum = 0;
  int zeros = 0;
  for (float m : c.slip) sum += m, zeros += m == 0.0f;
  double front = std::max(c.slip[0], c.slip[1]);
  if (front == 0) front = 0.25;
  const double a = std::pow(0.2, 60 * dt);
  st.slip_mean = float(a * st.slip_mean + (1 - a) * 0.25 * (sum + 0.25 * zeros));
  st.slip_front = float(a * st.slip_front + (1 - a) * front);
}

void ai_pedals(const AiPedalIn& in, double dt, AiPedalState& st, double& throttle, double& brake) {
  const double err = in.target - in.speed;
  int s;
  if (in.target == 0) s = 1;
  else if (in.edge && err > -1) s = 4;
  else if (st.state == 3 && err > 0.25) s = 3;
  else if (st.state == 5 && err < 0) s = 5;
  else if (std::fabs(err) <= 1) s = 2;
  else s = err < 0 ? 5 : 3;
  st.state = s;
  const bool slipping_off = in.off_track && st.slip_front >= 1;
  double t = 0, b = 0;
  switch (s) {
    case 1: b = std::clamp(std::fabs(in.speed), 0.5, 1.0); break;
    case 2: t = slipping_off ? 0.0 : in.hold * (1 + err); break;
    case 3: t = !slipping_off ? 1.0 : st.slip_front >= 1.05 ? 0.0 : in.hold; break;
    case 4:
      if (err > 0) t = double(st.throttle) - 0.016 * in.edge_rate * 60 * dt;
      else b = 0.6 * in.friction;
      break;
    default: b = 0.6 * in.friction; break;
  }
  t = std::clamp(t, 0.0, 1.0), b = std::clamp(b, 0.0, 1.0);
  // The post-processing: the gas caps and the brake's filter.
  double cap = in.heading_cos < 0.1 ? 0.6 : 1.0;
  if (st.slip_mean > 0.25 && std::fabs(in.chi) >= 1) cap = std::min(cap, std::clamp(1.05 - st.slip_mean, 0.2, 0.9));
  t = std::min(t, cap);
  const double prev = st.brake;
  if (b > prev) {
    if (prev < 0.95) {
      const double k = std::pow(0.75, 60 * dt);
      b = k * prev + (1 - k) * b;
    }
  } else if (prev > 0.05) {
    const double k = std::pow(0.5, 60 * dt);
    b = k * prev + (1 - k) * b;
  }
  st.throttle = float(t), st.brake = float(b);
  throttle = st.throttle, brake = st.brake;
}

AiYawGain ai_yaw_gain(const CarParams& p) {
  AiYawGain g;
  g.fill(4.0f);
  const FlatGround flat;
  const auto& wc = p.wheel_centres;
  const double wb = std::max(0.5, std::fabs((wc[0].z + wc[1].z) / 2 - (wc[2].z + wc[3].z) / 2));
  for (int b = 0; b < int(g.size()); ++b) {
    const double v = (b + 0.5) * 5;
    Vehicle car(p);
    car.set_ai_controls(true);
    car.launch({0, 0, 0}, 0.0, v);
    // A small steer: about 1.5 m/s^2 on the kinematic path, inside the
    // tyres' linear range.
    const double lock = std::max(0.05, car.steer_lock());
    const double s = std::clamp(1.5 * wb / (v * v) / lock, 0.004, 0.1);
    double sum = 0;
    int n = 0;
    for (int k = 0; k < 150; ++k) {
      Input in;
      in.steer = s;
      const double err = v - car.speed();
      if (err > 0) in.throttle = std::clamp(0.3 + 0.2 * err, 0.0, 1.0);
      else in.brake = std::clamp(-0.1 * err, 0.0, 1.0);
      car.step(in, flat);
      if (k >= 90) sum += car.angular_velocity().y, ++n;
    }
    const double r = sum / std::max(1, n) / s;
    if (std::isfinite(r) && r > 0.05 && r < 40) g[size_t(b)] = float(r);
  }
  return g;
}

double ai_yaw_gain_at(const AiYawGain& g, double v) {
  const double x = std::max(0.0, v / 5.0 - 0.5);
  const int i = std::min(int(x), int(g.size()) - 2);
  const double f = std::clamp(x - i, 0.0, 1.0);
  return g[size_t(i)] + f * (g[size_t(i + 1)] - g[size_t(i)]);
}

namespace {
double mean_gain(const AiYawGain& G) {
  double gbar = 0;
  for (int b = 3; b <= 8; ++b) gbar += G[size_t(b)];
  return std::max(0.5, gbar / 6);
}
}  // namespace

double ai_controller_lookahead(const AiCarState& c, const AiYawGain& G, double chi_car, double floor_track,
                               AiSteerState& st) {
  constexpr double dt = Vehicle::kStep;
  // v x T, T = 1.2 / mean G (at most 1 s), stretched up to 4 x in a slide
  // (heading . velocity 0.98 or less); the floor runs from 0.25 m at 7 m/s
  // to the track's at 17 m/s and falls at most 0.2 m a 30 Hz frame; the
  // look-ahead grows at most 0.9 v dt a step; 40 m more off the road.
  const double T = std::clamp(0.6 * 2.0 / mean_gain(G), 0.0, 1.0);
  const double vlen = std::hypot(c.vel_x, c.vel_z);
  const double dcos = vlen > 0.5 ? (c.vel_x * c.fwd_x + c.vel_z * c.fwd_z) / vlen : 1.0;
  const double slide = dcos <= 0.98 ? 4.0 : dcos >= 1.0 ? 1.0 : 1.0 + 3.0 * (1.0 - dcos) / 0.02;
  const double v = std::fabs(c.speed);
  const double hi_floor = std::max(double(st.look) - 0.2 * 30 * dt, floor_track);
  const double lower = v <= 7 ? 0.25 : v >= 17 ? hi_floor : 0.25 + (hi_floor - 0.25) * (v - 7) / 10;
  const double upper = std::min(double(st.look) + 0.9 * v * dt, 100.0);
  const double Lk = std::min(std::max(v * T * slide, lower), upper);
  st.look = float(Lk);
  return std::clamp(Lk + 40 * std::clamp(std::fabs(chi_car) - 1, 0.0, 1.0), 6.0, 100.0);
}

double ai_controller_steer(const AiCarState& c, const AiYawGain& G, const AiCapacity* cap, double chi_car, double e,
                           double lx, double lz, double gx, double gz, bool back, AiSteerState& st) {
  const double v = std::fabs(c.speed);
  const double gbar = mean_gain(G);
  const double D = std::max(0.1, std::hypot(lx, lz));
  // More than a metre off the line, the tangent turns towards the point.
  if (std::fabs(e) > 1) {
    const double w = std::clamp((std::fabs(e) - 1) / 4, 0.0, 1.0);
    gx = (1 - w) * gx + w * lx / D, gz = (1 - w) * gz + w * lz / D;
    const double gl = std::max(1e-6, std::hypot(gx, gz));
    gx /= gl, gz /= gl;
  }
  // The Hermite curve from the car (tangent D f) to the point (tangent
  // D g), at s = 0.8: its tangent's angle and the chord's length.
  const double hx = 0.96 * lx + 0.32 * D * gx, hz = 0.96 * lz - 0.28 * D + 0.32 * D * gz;
  const double cx = 0.896 * lx - 0.128 * D * gx, cz = 0.896 * lz + 0.032 * D - 0.128 * D * gz;
  const double ell = std::max(0.1, std::hypot(cx, cz));
  const double a = std::atan2(hx, hz);
  // If the car has spun > 90 deg past the aim point (hz < 0), sin(a) shrinks
  // toward 0 near +-pi and leaves the car steering straight ahead backwards.
  // Saturate the turning sine to +-1 in the direction of the aim point.
  const double sin_a = (hz < 0.0) ? (hx >= 0.0 ? 1.0 : -1.0) : std::sin(a);
  double wd = std::fabs(chi_car) < 1.1 ? 2 * v * sin_a / ell : a;
  // The steering-saturation grip term, open while the driver's grip level
  // is above the car's threshold (0.97 four-wheel drive, else 0.94): the
  // curve's curvature over the lateral limit's (the table without the
  // skill), past 0.8, up to ramp(level, threshold, 1.25); recomputed every
  // update.
  st.sat = 0;
  const double kLevelMin = cap ? double(cap->sat_level_min) : 0.94, kLevelMax = 1.25;
  if (cap && v > 1 && st.level > kLevelMin) {
    const double alat = double(cap->lateral[size_t(AiCapacity::index(v))]) * ai_aero_factor(v);
    if (alat > 0) {
      const double r = std::fabs(std::sin(a)) / ell * v * v / alat;
      const double top = std::min(1.0, (double(st.level) - kLevelMin) / (kLevelMax - kLevelMin));
      if (r > 0.8) st.sat = float(std::min(r - 0.8, top));
    }
  }
  // The lateral limit: the curve's curvature capped at the car's lateral
  // capacity (the planner's table x the aero factor) over v^2.
  if (std::fabs(chi_car) < 0.9 && cap && v > 1) {
    const double alat = double(cap->lateral[size_t(AiCapacity::index(v))]) * ai_aero_factor(v);
    const double q = std::fabs(std::sin(a)) / ell, lim = alat / (v * v);
    if (alat > 0 && q > lim) wd /= q / lim;
  }
  double s = wd / std::max(0.2, ai_yaw_gain_at(G, v));
  // Yaw-rate damping, never past straight ahead.
  const double sd = s - double(c.yaw_rate) * 0.75 * (2 / gbar) * (2 / gbar);
  s = s != 0 && sd * s < 0 ? 0.0 : sd;
  s = std::clamp(s, -1.0, 1.0);
  // The under-steer boost: while steering back, the steer grows by the
  // growth of |e| (1 to 2 x).
  const double ae = std::fabs(e);
  st.boost = back ? float(std::clamp(double(st.boost) + (ae - double(st.abs_e)), 1.0, 2.0)) : 1.0f;
  return std::clamp(s * double(st.boost), -1.0, 1.0);
}

AiField::AiField(const RacingLine& line, size_t cars, const Options& options)
    : line_(line),
      o_(options),
      seg_(cars, 0),
      progress_(cars, 0),
      lane_(cars, 0),
      skill_(cars, 1),
      cross_(cars, 0),
      corner_(cars, 0),
      decel_(cars, 0),
      target_(cars, 0),
      cap_(cars, 1e9f),
      prev_steer_(cars, 0),
      look_(cars, 7.5f),
      boost_(cars, 1.0f),
      abs_e_(cars, 0),
      chi_(cars, 0),
      prev_throttle_(cars, 0),
      sat_(cars, 0),
      pedal_(cars),
      level_(cars, 1.0f),
      yaw_gain_(cars, [] {
        AiYawGain g;
        g.fill(4.0f);
        return g;
      }()),
      capacity_(cars, nullptr),
      plan_c_(cars, 0),
      plan_b_(cars, 0),
      corner_speed_(cars) {}

void AiField::set_planner(size_t i, const AiCapacity* cap, float cornering, float braking) {
  if (cap != capacity_[i] || cornering != plan_c_[i]) corner_speed_[i].clear();
  capacity_[i] = cap, plan_c_[i] = cornering, plan_b_[i] = braking;
}

double AiField::corner_at(size_t i, size_t j) const {
  auto& cache = corner_speed_[i];
  if (cache.size() != line_.size()) cache.assign(line_.size(), std::numeric_limits<float>::quiet_NaN());
  float& v = cache[j];
  if (std::isnan(v)) {
    const double kappa = line_.curvature[j];
    const double theta = kappa > 0 ? line_.bank_left[j] : -line_.bank_left[j];
    double vc =
        ai_corner_speed(*capacity_[i], plan_c_[i], kappa, line_.curvature_v[j], theta, o_.max_speed, o_.slope_curvature);
    const double grip = double(line_.grip_at(j));
    if (std::fabs(kappa) > 1e-6 && std::fabs(grip - 1.0) > 1e-4) {
      vc = std::min<double>(o_.max_speed, vc * std::sqrt(grip));
    }
    v = float(vc);
  }
  return v;
}

void AiField::place_near(size_t i, double x, double z, double from, double to, float skill) {
  place(i, line_.nearest(x, z, from, to), 0.0f, skill);
}

void AiField::set_progress(size_t i, double distance) {
  seg_[i] = uint32_t(line_.segment(distance, 0));
  progress_[i] = distance;
  prev_steer_[i] = 0.0f;
  look_[i] = 7.5f, boost_[i] = 1.0f, abs_e_[i] = 0, chi_[i] = 0, prev_throttle_[i] = 0, sat_[i] = 0;
  pedal_[i] = AiPedalState{};
}

double AiField::plan_point(size_t i, size_t j0, double* friction) const {
  const RacingLine& L = line_;
  if (friction) *friction = 1;
  if (L.size() < 2) return 0;
  j0 = std::min(j0, L.size() - 1);
  if (!capacity_[i] || L.curvature.size() != L.size()) return planned_speed(i, L.dist[j0]);
  // SPEC-RACE-AI, Speed planning, the braking pass: from the farthest
  // point within 420 m back to j0, each point's limit is the carried
  // speed or the three-point mean of the corner speeds; the braking the
  // corner leaves (the friction circle) and the grade carry it back one
  // segment. a < 0 decelerates; the carry v - a ds / v (from 10 m/s) or
  // sqrt(v^2 - 2 a ds) gives the spec's 30 -> 31.0 and 8 -> 11.136 m/s for
  // a = -15 over 2 m.
  size_t jmax = j0;
  while (jmax + 1 < L.size() && L.dist[jmax + 1] - L.dist[j0] <= 420) ++jmax;
  const AiCapacity& c = *capacity_[i];
  const double B = plan_b_[i] * c.braking_k;
  const size_t last = L.size() - 1;
  double carry = 1000;
  for (size_t j = jmax;; --j) {
    const double vc = corner_at(i, j);
    const double mean = (corner_at(i, j > 0 ? j - 1 : j) + vc + corner_at(i, std::min(j + 1, last))) / 3;
    const double vlim = std::max(0.001, std::min(carry, mean));
    if (j == j0 || j == 0) {
      // The share of the friction circle the corner leaves at the point's
      // limit: sqrt(1 - min(v_lim^2 / v_c^2, 1)).
      if (friction) *friction = ai_braking_left(1.0, vlim, vc);
      return vlim;
    }
    const double grip = double(L.grip_at(j));
    const double a = -ai_braking_left(B * c.braking[size_t(AiCapacity::index(vlim))] * grip, vlim, vc) - 9.81 * L.grade[j];
    carry = ai_brake_carry(vlim, a, L.dist[j] - L.dist[j - 1]);
  }
}

double AiField::planned_speed(size_t i, double p) const {
  const RacingLine& L = line_;
  if (L.size() < 2) return 0;
  const size_t s0 = L.segment(p, seg_[i]);
  if (capacity_[i] && L.curvature.size() == L.size()) {
    // The plan at the line point nearest p (plan_point's braking pass).
    size_t j0 = s0;
    if (j0 + 1 < L.size() && L.dist[j0 + 1] - p < p - L.dist[j0]) ++j0;
    return plan_point(i, j0, nullptr);
  }
  const double a = corner_[i], d = decel_[i];
  if (a <= 0 || L.radius.size() != L.size()) {
    const float vf = std::clamp(float((p - L.dist[s0]) / std::max(1e-6f, L.dist[s0 + 1] - L.dist[s0])), 0.0f, 1.0f);
    const double v0 = double(L.speed[s0]) * std::sqrt(double(L.grip_at(s0)));
    const double v1 = double(L.speed[s0 + 1]) * std::sqrt(double(L.grip_at(s0 + 1)));
    return skill_[i] * (v0 + vf * (v1 - v0));
  }
  // v^2 = min over the points ahead of a x grip x radius + 2 d x grip x ds,
  // until the braking term alone passes the best.
  double best = double(o_.max_speed) * o_.max_speed;
  double brake = 0.0;
  for (size_t s = s0 + 1; s < L.size(); ++s) {
    const double ds = std::max(0.0, L.dist[s] - (s == s0 + 1 ? p : double(L.dist[s - 1])));
    const double g = double(L.grip_at(s));
    brake += 2.0 * d * g * ds;
    if (brake >= best) break;
    best = std::min(best, a * g * L.radius[s] + brake);
  }
  return std::sqrt(best);
}

void AiField::place(size_t i, double distance, float lane, float skill) {
  seg_[i] = uint32_t(line_.segment(distance, 0));
  progress_[i] = distance;
  lane_[i] = lane;
  skill_[i] = skill;
  prev_steer_[i] = 0.0f;
  look_[i] = 7.5f, boost_[i] = 1.0f, abs_e_[i] = 0, chi_[i] = 0, prev_throttle_[i] = 0, sat_[i] = 0;
  pedal_[i] = AiPedalState{};
}

void AiField::start_pose(size_t i, double& x, double& z, double& heading) const {
  x = z = heading = 0;
  if (line_.size() < 2) return;
  const size_t s = seg_[i];
  const double ex = line_.x[s + 1] - line_.x[s], ez = line_.z[s + 1] - line_.z[s];
  const double len = std::hypot(ex, ez);
  const double f = std::clamp((progress_[i] - line_.dist[s]) / std::max(1e-6, double(line_.dist[s + 1] - line_.dist[s])), 0.0, 1.0);
  x = line_.x[s] + f * ex + (len > 0 ? lane_[i] * ez / len : 0);
  z = line_.z[s] + f * ez - (len > 0 ? lane_[i] * ex / len : 0);
  heading = std::atan2(ex, ez);
}

void AiField::drive(std::span<const AiCarState> states, std::span<Input> inputs) {
  const RacingLine& L = line_;
  const size_t n = std::min({states.size(), inputs.size(), size()});
  if (L.size() < 2) return;
  const size_t last = L.size() - 2;
  for (size_t i = 0; i < n; ++i) {
    const AiCarState& c = states[i];
    // The projection: a hill climb from the last segment (a few steps a
    // frame), bounded by `search` metres.
    auto d2 = [&](size_t s, float& f) {
      const float ex = L.x[s + 1] - L.x[s], ez = L.z[s + 1] - L.z[s];
      const float len2 = ex * ex + ez * ez;
      f = len2 > 0 ? std::clamp(((c.x - L.x[s]) * ex + (c.z - L.z[s]) * ez) / len2, 0.0f, 1.0f) : 0.0f;
      const float dx = c.x - (L.x[s] + f * ex), dz = c.z - (L.z[s] + f * ez);
      return dx * dx + dz * dz;
    };
    size_t s = std::min<size_t>(seg_[i], last);
    float f = 0, fn = 0;
    float best = d2(s, f);
    const float limit = L.dist[s] + o_.search;
    while (s < last && L.dist[s + 1] <= limit) {
      const float next = d2(s + 1, fn);
      if (next > best) break;
      best = next, f = fn, ++s;
    }
    if (s > 0) {
      float fb = 0;
      const float back = d2(s - 1, fb);
      if (back < best) best = back, f = fb, --s;
    }
    seg_[i] = uint32_t(s);
    const double p = L.dist[s] + f * (L.dist[s + 1] - L.dist[s]);
    progress_[i] = std::max(progress_[i], p);
    {
      const float ex = L.x[s + 1] - L.x[s], ez = L.z[s + 1] - L.z[s];
      const float len = std::sqrt(ex * ex + ez * ez);
      const float ox = c.x - (L.x[s] + f * ex), oz = c.z - (L.z[s] + f * ez);
      cross_[i] = len > 0 ? (ox * ez - oz * ex) / len : 0.0f;
    }
    // Steering: pure pursuit to the look-ahead point, moved to the lane.
    // SPEC-RACE-AI: AI cars have no gamepad speed-narrowed lock; keep a
    // responsive bounded look-ahead (so tight hairpins aren't cut across) and
    // generous steering authority so light/fast cars track curves & recover.
    const float speed = std::fabs(c.speed);
    double look_dist = 0.0;
    if (o_.slip_rule) {
      // SPEC-RACE-AI, Steering at speed (interim): the line point T v
      // ahead, T = 1.0 s, at least 2 m.
      look_dist = std::max(2.0, double(o_.slip_lookahead_s) * speed);
    } else {
      look_dist = std::clamp<double>(o_.lookahead_s * speed, double(o_.min_lookahead), 24.0);
    }
    const double look = progress_[i] + look_dist;
    const size_t ts = L.segment(look, s);
    const float tf = std::clamp(float((look - L.dist[ts]) / std::max(1e-6f, L.dist[ts + 1] - L.dist[ts])), 0.0f, 1.0f);
    const float tex = L.x[ts + 1] - L.x[ts], tez = L.z[ts + 1] - L.z[ts];
    const float tlen = std::sqrt(tex * tex + tez * tez);
    // Keep the look-ahead aim point strictly inside the walls at `look`
    // (clip_room already subtracts half a car width = 1.0 m).
    float aim_lane = lane_[i];
    if (L.room_right.size() == L.size() && L.room_left.size() == L.size()) {
      const float rmax = std::max(0.0f, L.room_right[ts] - 0.35f);
      const float lmax = std::max(0.0f, L.room_left[ts] - 0.35f);
      aim_lane = std::clamp(aim_lane, -lmax, rmax);
    }
    const float tx = L.x[ts] + tf * tex + (tlen > 0 ? aim_lane * tez / tlen : 0.0f);
    const float tz = L.z[ts] + tf * tez - (tlen > 0 ? aim_lane * tex / tlen : 0.0f);
    const float rx = tx - c.x, rz = tz - c.z;
    const float lz = rx * c.fwd_x + rz * c.fwd_z, lx = rx * c.fwd_z - rz * c.fwd_x;  // car frame: x right
    const float ld = std::sqrt(lx * lx + lz * lz);
    const float alpha = std::atan2(lx, lz);
    // Pure pursuit scaled for front tyre slip angle, with a speed-safe cap.
    const float ai_cap = std::min(c.steer_lock, std::max(c.max_delta * 1.55f, 0.08f));
    float delta = std::clamp(ld > 1e-4f ? 1.55f * std::atan(2 * c.wheelbase * std::sin(alpha) / ld) : 0.0f, -ai_cap, ai_cap);
    if (o_.slip_rule) {
      // SPEC-RACE-AI, Steering at speed: delta = 0.27 alpha + 0.29 beta,
      // clamped only by the full lock (no speed-narrowed turn-in cap).
      const float vz = c.vel_x * c.fwd_x + c.vel_z * c.fwd_z, vx = c.vel_x * c.fwd_z - c.vel_z * c.fwd_x;
      const float beta = std::hypot(vx, vz) > 1.0f && vz > 0 ? std::atan2(vx, vz) : 0.0f;
      delta = float(ai_slip_steer(alpha, beta, c.steer_lock));
    }
    Input& in = inputs[i];
    in = Input{};
    float raw_steer = c.steer_lock > 0 ? std::clamp(delta / c.steer_lock, -1.0f, 1.0f) : 0.0f;
    // The title's line controller (decoded, SPEC-RACE-AI Steering).
    const double dt = Vehicle::kStep;
    const double e = double(cross_[i]) - double(lane_[i]);  // m from the driven line
    double chi_car = 0;
    bool back = false;
    if (o_.controller) {
      const bool rooms = L.room_right.size() == L.size() && L.room_left.size() == L.size();
      const double hw_side = rooms ? double(cross_[i] >= 0 ? L.room_right[s] : L.room_left[s]) : 4.5;
      chi_car = cross_[i] / std::max(1.0, hw_side);
      const AiYawGain& G = yaw_gain_[i];
      // The grip level: the line controller's own, 1 + 0.025 x the cheat
      // level (set_grip_level; 1 by default), not the planner's C (0
      // without a set_planner plan).
      AiSteerState ss{look_[i], boost_[i], abs_e_[i], capacity_[i] ? level_[i] : 0.0f};
      const double Ltot = ai_controller_lookahead(c, G, chi_car, o_.min_lookahead_track, ss);
      back = double(prev_steer_[i]) * e < 0;  // steering back towards the line
      // The aim point on the driven line, moved 0.2 e towards the car when
      // more than a metre off it.
      double lane_aim = lane_[i];
      if (std::fabs(chi_car) < 0.9 && (std::fabs(e) > 1 || back)) lane_aim += 0.2 * e;
      const double la = progress_[i] + Ltot;
      const size_t ks = L.segment(la, s);
      const double kf = std::clamp((la - L.dist[ks]) / std::max(1e-6, double(L.dist[ks + 1] - L.dist[ks])), 0.0, 1.0);
      const double kex = L.x[ks + 1] - L.x[ks], kez = L.z[ks + 1] - L.z[ks];
      const double klen = std::max(1e-6, std::hypot(kex, kez));
      const double ptx = L.x[ks] + kf * kex + lane_aim * kez / klen, ptz = L.z[ks] + kf * kez - lane_aim * kex / klen;
      // In the car's frame (x right, z forward).
      const double rx0 = ptx - c.x, rz0 = ptz - c.z;
      const double lx0 = rx0 * c.fwd_z - rz0 * c.fwd_x, lz0 = rx0 * c.fwd_x + rz0 * c.fwd_z;
      const double gx = (kex * c.fwd_z - kez * c.fwd_x) / klen, gz = (kex * c.fwd_x + kez * c.fwd_z) / klen;
      raw_steer = float(ai_controller_steer(c, G, capacity_[i], chi_car, e, lx0, lz0, gx, gz, back, ss));
      look_[i] = ss.look, boost_[i] = ss.boost, sat_[i] = ss.sat;
    }
    float steer_out = raw_steer;
    // No rate limit on the slip rule (SPEC-RACE-AI: the wheels follow the
    // steer at every speed); pure pursuit keeps its own.
    if (o_.steer_rate > 0.0f && !o_.slip_rule && !o_.controller) {
      const float vz_b = c.vel_x * c.fwd_x + c.vel_z * c.fwd_z;
      const float vx_b = c.vel_x * c.fwd_z - c.vel_z * c.fwd_x;
      const bool sliding = std::hypot(vx_b, vz_b) > 5.0f && vz_b > 1.0f && std::fabs(std::atan2(vx_b, vz_b)) > 0.12f;
      const float rate = sliding ? o_.steer_rate * 1.6f : o_.steer_rate;
      const float max_d = rate * float(Vehicle::kStep);
      steer_out = prev_steer_[i] + std::clamp(raw_steer - prev_steer_[i], -max_d, max_d);
    }
    prev_steer_[i] = steer_out;
    in.steer = steer_out;
    if (o_.controller && o_.title_pedals) {
      // SPEC-RACE-AI, Throttle (decoded): the plan 0.67 of a line point
      // past the car's, between the two points around it.
      const double t = std::clamp(double(f), 0.0, 1.0);
      const size_t j = std::min<size_t>(t >= 0.33 ? s + 1 : s, L.size() - 2);
      const double fr = t >= 0.33 ? t - 0.33 : t + 0.67;
      double fric = 1;
      const double v0 = plan_point(i, j, nullptr), v1 = plan_point(i, j + 1, &fric);
      double target = progress_[i] >= L.length() - 1 ? 0.0 : (1 - fr) * v0 + fr * v1;
      // The heading (its cosine with the line's direction).
      const double sx = L.x[s + 1] - L.x[s], sz = L.z[s + 1] - L.z[s];
      const double sl = std::hypot(sx, sz);
      const double hcos = sl > 1e-6 ? (c.fwd_x * sx + c.fwd_z * sz) / sl : 1.0;
      // The speed caps: the passing search's, and 0.2 of the plan's top
      // speed turned 84 degrees off the line, 0.1 at 120.
      double cap = cap_[i];
      const int turned = (hcos < 0.1) + (hcos < -0.5);
      if (turned > 0) cap = std::min(cap, (3 - turned) * 0.1 * double(o_.max_speed));
      AiPedalState& ps = pedal_[i];
      ai_slip_metrics(c, dt, ps);
      const double ae = std::fabs(e), dae = ae - double(abs_e_[i]);
      const double rate = dae / dt;
      // Off the road: past the room on the car's side.
      const bool rooms = L.room_right.size() == L.size() && L.room_left.size() == L.size();
      const double edge_in = rooms ? double(cross_[i] >= 0 ? L.room_right[s] : L.room_left[s]) - std::fabs(cross_[i]) : 1.0;
      const bool off_track = edge_in < 0;
      bool edge = false;
      if (back) {
        const double room = chi_car > double(chi_[i]) ? 1 - chi_car : 1 + chi_car;
        edge = o_.track_edge_avoidance && dae > 0 && room > 0 && room / rate < 1.25 && ae > 0.5;
        // Running wide: the target cut by k.
        double k = 0;
        const double a_e = ae + std::min(dae, 0.0);
        if (off_track) {
          if (ae > 0.25) {
            const double k3 = (edge_in < -1 ? -1 - edge_in : 0.0) + 0.12;
            k = a_e >= 3 ? k3 : a_e <= 0.25 ? 0.02 : 0.02 + (k3 - 0.02) * (a_e - 0.25) / 2.75;
          }
        } else if (ae > 2.5 && std::fabs(chi_car) > 0.25 && ps.slip_front > 1.5) {
          k = a_e >= 10 ? 0.1 : a_e <= 2.5 ? 0.0 : (a_e - 2.5) * 0.1 / 7.5;
        }
        // k at most 1 (ours: the road-edge distance is our interim, Q175).
        if (k > 0) cap = std::min(cap, (1 - std::min(k, 1.0)) * target);
      }
      target = std::min(target, cap);
      target_[i] = float(target);
      AiPedalIn pi;
      pi.target = target, pi.speed = c.speed, pi.hold = c.hold, pi.friction = fric;
      pi.edge = edge, pi.edge_rate = rate, pi.off_track = off_track, pi.heading_cos = hcos, pi.chi = chi_car;
      ai_pedals(pi, dt, ps, in.throttle, in.brake);
      abs_e_[i] = float(ae), chi_[i] = float(chi_car), prev_throttle_[i] = float(in.throttle);
      continue;
    }
    // Speed: the plan a little ahead. Past the line's end: stop.
    const double ahead = progress_[i] + o_.speed_lookahead_s * speed;
    // Slow down when far off its lane (approaching road edge / wall), not on
    // clean overtakes.
    const float off = std::fabs(cross_[i] - lane_[i]);
    const float scale = std::clamp(1.0f - o_.off_line_slow * std::max(0.0f, off - o_.off_line_slack), 0.65f, 1.0f);
    const float target =
        progress_[i] >= L.length() - 1 ? 0.0f : std::min(cap_[i], scale * float(planned_speed(i, ahead)));
    target_[i] = target;
    const float err = target - c.speed;
    if (o_.full_gas) {
      if (err >= 1.0f) in.throttle = 1.0f;
      else if (err >= 0.0f) {
        // SPEC-RACE-AI, Throttle hold band: smoothly blend from hold throttle at
        // target speed up to full gas at 1 m/s below target (continuous at
        // err == 1.0 so leading cars do not step-drop throttle in front of packs).
        const double h = std::clamp(double(c.hold), 0.25, 0.85);
        in.throttle = std::clamp(h + (1.0 - h) * double(err), 0.0, 1.0);
      } else if (err < -o_.deadband) {
        in.brake = std::clamp(o_.brake_gain * (-err - o_.deadband) + 0.2f, 0.0f, 1.0f);
      }
    } else if (err >= 0) in.throttle = std::clamp(0.15f + o_.throttle_gain * err, 0.0f, 1.0f);
    else if (err < -o_.deadband) in.brake = std::clamp(o_.brake_gain * (-err - o_.deadband), 0.0f, 1.0f);
    else in.throttle = 0.1f;
    if (o_.controller) {
      // Body-slip oversteer guard (under controller + full_gas, race_view skips
      // ai_traction): on asphalt, RWD/AWD peak rear-tyre slip sits near 7-8 deg,
      // so feather throttle and blend counter-steer past 7.5 deg to catch spinouts
      // (e.g. NR02 Challenger); on dirt (grip < 0.9), allow wider 13 deg drifts;
      // on FWD cars, never cut throttle (which would induce lift-off oversteer)
      // and only counter-steer severe slides past 14 deg.
      const double vz_b = c.vel_x * c.fwd_x + c.vel_z * c.fwd_z;
      const double vx_b = c.vel_x * c.fwd_z - c.vel_z * c.fwd_x;
      if (std::hypot(vx_b, vz_b) > 5.0 && vz_b > 1.0) {
        const double beta = std::atan2(vx_b, vz_b);
        const double ab = std::fabs(beta);
        constexpr double kDeg = 3.14159265358979323846 / 180.0;
        const bool is_fwd = capacity_[i] && capacity_[i]->is_fwd;
        const bool on_dirt = double(L.grip_at(s)) < 0.9;
        const double onset_deg = is_fwd ? 14.0 : (on_dirt ? 13.0 : 7.5);
        if (ab > onset_deg * kDeg) {
          if (!is_fwd) {
            const double span_t = on_dirt ? 12.0 : 8.5;
            const double t_keep = std::clamp(1.0 - (ab - onset_deg * kDeg) / (span_t * kDeg), 0.0, 1.0);
            in.throttle = std::min(in.throttle, t_keep);
          }
          const double span_s = on_dirt ? 13.0 : 9.5;
          const double w = std::clamp((ab - onset_deg * kDeg) / (span_s * kDeg), 0.0, 1.0);
          const double lock = std::max(0.05, double(c.steer_lock));
          const double counter = std::clamp((beta - 0.08 * double(c.yaw_rate)) / lock, -1.0, 1.0);
          in.steer = std::clamp((1.0 - w) * in.steer + w * counter, -1.0, 1.0);
          prev_steer_[i] = float(in.steer);
        }
      }
      // Track-edge avoidance: steering back but still running out, with
      // the edge under 1.25 (half widths per m/s) away, below the plan:
      // ease the gas by 0.016 x d|e|/dt.
      const double ae = std::fabs(e);
      const double de = (ae - double(abs_e_[i])) / dt;
      if (o_.track_edge_avoidance && back && de > 0 && ae > 0.5) {
        const double room = chi_car > double(chi_[i]) ? 1 - chi_car : 1 + chi_car;
        if (room > 0 && room / de < 1.25 && err > -1 && err > 0) {
          in.throttle = float(std::clamp(double(prev_throttle_[i]) - 0.016 * de, 0.0, 1.0));
          in.brake = 0;
        }
      }
      // Turned 84 degrees or more off the line (the heading's dot with the
      // line's direction under 0.1): at most 0.6 gas.
      {
        const double sx = L.x[s + 1] - L.x[s], sz = L.z[s + 1] - L.z[s];
        const double sl = std::hypot(sx, sz);
        if (sl > 1e-6 && (c.fwd_x * sx + c.fwd_z * sz) / sl < 0.1) in.throttle = std::min(float(in.throttle), 0.6f);
      }
      abs_e_[i] = float(ae), chi_[i] = float(chi_car), prev_throttle_[i] = float(in.throttle);
    }
  }
}

}  // namespace drive
