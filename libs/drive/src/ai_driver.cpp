#include "drive/ai_driver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace drive {

RacingLine RacingLine::from_points(std::span<const float> xs, std::span<const float> zs, double lat_accel, double accel,
                                   double decel, double max_speed) {
  RacingLine l;
  const size_t n = std::min(xs.size(), zs.size());
  for (size_t i = 0; i < n; ++i) {
    if (!l.x.empty() && std::hypot(xs[i] - l.x.back(), zs[i] - l.z.back()) < 1e-3) continue;
    l.dist.push_back(l.x.empty() ? 0.0f : l.dist.back() + float(std::hypot(xs[i] - l.x.back(), zs[i] - l.z.back())));
    l.x.push_back(xs[i]);
    l.z.push_back(zs[i]);
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
  for (int n = 0; n < AiCapacity::kEntries; ++n) {
    const double v = 2.5 * n;
    // Braking: load moves forwards by the deceleration x the height over
    // the wheelbase.
    double a = 0;
    for (int pass = 0; pass < 6; ++pass) {
      const double t = std::min(m * a * h / wb, axle_w[1]);  // at most the whole rear on the front
      a = (axle(0, TyrePeak::kBraking, axle_w[0] + t, v) + axle(1, TyrePeak::kBraking, axle_w[1] - t, v)) / m;
    }
    c.braking[size_t(n)] = float(cal.braking * (a + p.drag_quadratic * v * v / m));
    // Lateral: each axle's force moves load inner to outer by the force x
    // the CoM height / the track; the rear capped by the yaw balance.
    double force[2] = {0, 0};
    for (int pass = 0; pass < 6; ++pass) {
      double next[2];
      for (int ax = 0; ax < 2; ++ax) {
        // At most the whole axle on the outer wheel (the inner lifts): past
        // that the transfer would make load from nothing.
        const double dn = std::min(force[ax] * h / track, 0.5 * axle_w[ax]);
        double f = 0;
        for (double side : {0.5 * axle_w[ax] + dn, 0.5 * axle_w[ax] - dn}) {
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
    c.lateral[size_t(n)] = float(cal.lateral * (force[0] + force[1]) / m);
    if (n == 8 && std::getenv("SHIFT_CAPACITY_DEBUG")) {
      // The lateral terms at 20 m/s (SHIFT_CAPACITY_DEBUG=1).
      for (int ax = 0; ax < 2; ++ax) {
        const double kgf = 0.5 * axle_w[ax] / kG;
        std::fprintf(stderr,
                     "capacity: axle %d static %.0f kgf mu %.3f width %.4f lat_scale %.3f compound %.3f misc %.3f "
                     "curve scales %.3f %.3f blend %.3f force %.0f N\n",
                     ax, kgf, tyre_peak_mu(p, ax, TyrePeak::kLateral, 0.5 * axle_w[ax], v), p.width_scale[size_t(ax)],
                     p.lat_scale[size_t(ax)], p.lat_compound_scale, p.long_misc_scale, p.lat_curve_scale[0],
                     p.lat_curve_scale[1], p.lateral.blend(kgf), force[ax]);
      }
      std::fprintf(stderr, "capacity: mass %.0f share %.3f h %.3f track %.3f raw %.2f cal %.3f -> %.2f m/s^2\n", m, share, h,
                   track, (force[0] + force[1]) / m, cal.lateral, c.lateral[size_t(n)]);
    }
    // Forward: the driven axles, load moving rearwards (tyre-limited only).
    a = 0;
    for (int pass = 0; pass < 6; ++pass) {
      const double t = std::min(m * a * h / wb, axle_w[0]);  // at most the whole front on the rear
      double f = 0;
      if (driven[0]) f += axle(0, TyrePeak::kDriving, axle_w[0] - t, v);
      if (driven[1]) f += axle(1, TyrePeak::kDriving, axle_w[1] + t, v);
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

double ai_corner_speed(const AiCapacity& cap, double cornering, double kappa, double kappa_v, double theta, double top) {
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
    const double num = kG * (s + mu * co), den = k * (co - mu * s) - 0.75 * kappa_v * (s + mu * co);
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
  if (size() < 2) return 0;
  size_t i = std::min(hint, size() - 2);
  while (i > 0 && dist[i] > distance) --i;
  while (i + 2 < size() && dist[i + 1] <= distance) ++i;
  return i;
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
  if (in.throttle > 0 && spin > 1.1) cap = std::max(0.25f, cap - float(4 * dt));
  else cap = std::min(1.0f, cap + float(2 * dt));
  double t = std::min(in.throttle, double(cap));
  // The body's slip angle: the velocity's direction in the body frame
  // (z forward, x towards positive steer).
  const Vec3 vb = rotate_inverse(v.orientation(), v.velocity());
  const double sp = std::hypot(vb.x, vb.z);
  if (sp > 6 && vb.z > 0) {
    const double beta = std::atan2(vb.x, vb.z);
    const double a = std::fabs(beta);
    constexpr double kDeg = 3.14159265358979 / 180;
    if (a > 7 * kDeg) t *= std::max(0.0, 1 - (a - 7 * kDeg) / (6 * kDeg));
    // Past 5 degrees the steer turns towards the velocity (a counter-steer,
    // free of the speed-narrowed lock), all of it by 12 degrees.
    const double w = std::clamp((a - 5 * kDeg) / (7 * kDeg), 0.0, 1.0);
    if (w > 0 && steer_guard) {
      const double lock = std::max(0.05, v.steer_lock());
      in.steer = (1 - w) * in.steer + w * std::clamp(beta / lock, -1.0, 1.0);
      in.brake = std::min(in.brake, 0.3 * (1 - w) + 0.0);
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
  return s;
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
    v = float(ai_corner_speed(*capacity_[i], plan_c_[i], kappa, line_.curvature_v[j], theta, o_.max_speed));
  }
  return v;
}

void AiField::place_near(size_t i, double x, double z, double from, double to, float skill) {
  place(i, line_.nearest(x, z, from, to), 0.0f, skill);
}

void AiField::set_progress(size_t i, double distance) {
  seg_[i] = uint32_t(line_.segment(distance, 0));
  progress_[i] = distance;
}

double AiField::planned_speed(size_t i, double p) const {
  const RacingLine& L = line_;
  if (L.size() < 2) return 0;
  const size_t s0 = L.segment(p, seg_[i]);
  if (capacity_[i] && L.curvature.size() == L.size()) {
    // SPEC-RACE-AI, Speed planning, the braking pass: from the farthest
    // point within 420 m back to the car's, each point's limit is the
    // carried speed or the three-point mean of the corner speeds; the
    // braking the corner leaves (the friction circle) and the grade carry
    // it back one segment. a < 0 decelerates; the carry v - a ds / v (from
    // 10 m/s) or sqrt(v^2 - 2 a ds) gives the spec's 30 -> 31.0 and
    // 8 -> 11.136 m/s for a = -15 over 2 m.
    size_t j0 = s0;
    if (j0 + 1 < L.size() && L.dist[j0 + 1] - p < p - L.dist[j0]) ++j0;
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
      if (j == j0 || j == 0) return vlim;
      const double a = -ai_braking_left(B * c.braking[size_t(AiCapacity::index(vlim))], vlim, vc) - 9.81 * L.grade[j];
      carry = ai_brake_carry(vlim, a, L.dist[j] - L.dist[j - 1]);
    }
  }
  const double a = corner_[i], d = decel_[i];
  if (a <= 0 || L.radius.size() != L.size()) {
    const float vf = std::clamp(float((p - L.dist[s0]) / std::max(1e-6f, L.dist[s0 + 1] - L.dist[s0])), 0.0f, 1.0f);
    return skill_[i] * (L.speed[s0] + vf * (L.speed[s0 + 1] - L.speed[s0]));
  }
  // v^2 = min over the points ahead of a x radius + 2 d x the distance to
  // them, until the braking term alone passes the best.
  double best = double(o_.max_speed) * o_.max_speed;
  for (size_t s = s0 + 1; s < L.size(); ++s) {
    const double ds = std::max(0.0, L.dist[s] - p);
    const double brake = 2 * d * ds;
    if (brake >= best) break;
    best = std::min(best, a * L.radius[s] + brake);
  }
  return std::sqrt(best);
}

void AiField::place(size_t i, double distance, float lane, float skill) {
  seg_[i] = uint32_t(line_.segment(distance, 0));
  progress_[i] = distance;
  lane_[i] = lane;
  skill_[i] = skill;
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
    const float speed = std::fabs(c.speed);
    const double look = progress_[i] + (o_.slip_rule ? std::max<double>(2.0, o_.slip_lookahead_s * speed)
                                                     : std::max<double>(o_.min_lookahead, o_.lookahead_s * speed));
    const size_t ts = L.segment(look, s);
    const float tf = std::clamp(float((look - L.dist[ts]) / std::max(1e-6f, L.dist[ts + 1] - L.dist[ts])), 0.0f, 1.0f);
    const float tex = L.x[ts + 1] - L.x[ts], tez = L.z[ts + 1] - L.z[ts];
    const float tlen = std::sqrt(tex * tex + tez * tez);
    const float tx = L.x[ts] + tf * tex + (tlen > 0 ? lane_[i] * tez / tlen : 0.0f);
    const float tz = L.z[ts] + tf * tez - (tlen > 0 ? lane_[i] * tex / tlen : 0.0f);
    const float rx = tx - c.x, rz = tz - c.z;
    const float lz = rx * c.fwd_x + rz * c.fwd_z, lx = rx * c.fwd_z - rz * c.fwd_x;  // car frame: x right
    const float ld = std::sqrt(lx * lx + lz * lz);
    const float alpha = std::atan2(lx, lz);
    float delta = std::clamp(ld > 1e-4f ? std::atan(2 * c.wheelbase * std::sin(alpha) / ld) : 0.0f, -c.max_delta,
                             c.max_delta);
    if (o_.slip_rule) {
      // The velocity's direction in the car frame (x right), from 1 m/s.
      const float vz = c.vel_x * c.fwd_x + c.vel_z * c.fwd_z, vx = c.vel_x * c.fwd_z - c.vel_z * c.fwd_x;
      const float beta = std::hypot(vx, vz) > 1.0f && vz > 0 ? std::atan2(vx, vz) : 0.0f;
      delta = std::clamp(0.27f * alpha + 0.29f * beta, -c.steer_lock, c.steer_lock);
    }
    Input& in = inputs[i];
    in = Input{};
    in.steer = c.steer_lock > 0 ? std::clamp(delta / c.steer_lock, -1.0f, 1.0f) : 0.0f;
    // Speed: the plan a little ahead. Past the line's end: stop.
    const double ahead = progress_[i] + o_.speed_lookahead_s * speed;
    // Off its lane by more than off_line_slack m, the car slows (ours: the
    // plan's corner speed is the R34's, and a car that cannot hold it
    // runs wide).
    const float off = std::fabs(cross_[i] - lane_[i]);
    const float scale = std::clamp(1.0f - o_.off_line_slow * std::max(0.0f, off - o_.off_line_slack), 0.6f, 1.0f);
    const float target =
        progress_[i] >= L.length() - 1 ? 0.0f : std::min(cap_[i], scale * float(planned_speed(i, ahead)));
    target_[i] = target;
    const float err = target - c.speed;
    if (o_.full_gas) {
      if (err >= 0) in.throttle = 1.0f;
      else if (err < -o_.deadband) in.brake = std::clamp(o_.brake_gain * (-err - o_.deadband) + 0.2f, 0.0f, 1.0f);
    } else if (err >= 0) in.throttle = std::clamp(0.15f + o_.throttle_gain * err, 0.0f, 1.0f);
    else if (err < -o_.deadband) in.brake = std::clamp(o_.brake_gain * (-err - o_.deadband), 0.0f, 1.0f);
    else in.throttle = 0.1f;
  }
}

}  // namespace drive
