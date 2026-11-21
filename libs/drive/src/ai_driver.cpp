#include "drive/ai_driver.h"

#include <algorithm>
#include <cmath>

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
  const auto& c = v.params().wheel_centres;
  s.wheelbase = float(std::fabs((c[0].z + c[1].z) / 2 - (c[2].z + c[3].z) / 2));
  s.steer_lock = float(v.steer_lock());
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
      target_(cars, 0) {}

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
    const double look = progress_[i] + std::max<double>(o_.min_lookahead, o_.lookahead_s * speed);
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
    const float delta = ld > 1e-4f ? std::atan(2 * c.wheelbase * std::sin(alpha) / ld) : 0.0f;
    Input& in = inputs[i];
    in = Input{};
    in.steer = c.steer_lock > 0 ? std::clamp(delta / c.steer_lock, -1.0f, 1.0f) : 0.0f;
    // Speed: the plan a little ahead. Past the line's end: stop.
    const double ahead = progress_[i] + o_.speed_lookahead_s * speed;
    const float target = progress_[i] >= L.length() - 1 ? 0.0f : float(planned_speed(i, ahead));
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
