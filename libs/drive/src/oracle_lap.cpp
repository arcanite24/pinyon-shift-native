#include "drive/oracle_lap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace drive {
namespace {

constexpr double kPi = 3.14159265358979323846;

// The text after `"key":` (spaces skipped), or npos.
size_t value_at(const std::string& s, const std::string& key, size_t from = 0) {
  const std::string k = "\"" + key + "\":";
  size_t at = s.find(k, from);
  if (at == std::string::npos) return at;
  at += k.size();
  while (at < s.size() && (s[at] == ' ' || s[at] == '\n' || s[at] == '\r' || s[at] == '\t')) ++at;
  return at;
}

bool number(const std::string& s, const std::string& key, double& out, size_t from = 0) {
  size_t at = value_at(s, key, from);
  if (at == std::string::npos) return false;
  if (at < s.size() && s[at] == '"') ++at;  // "time_72":"38.784112"
  char* end = nullptr;
  out = std::strtod(s.c_str() + at, &end);
  return end != s.c_str() + at;
}

bool string_value(const std::string& s, const std::string& key, std::string& out) {
  const size_t at = value_at(s, key);
  if (at == std::string::npos || at >= s.size() || s[at] != '"') return false;
  const size_t end = s.find('"', at + 1);
  if (end == std::string::npos) return false;
  out = s.substr(at + 1, end - at - 1);
  return true;
}

// The two numbers of `"key": [a, b]`.
bool pair(const std::string& s, const std::string& key, double& a, double& b, size_t from) {
  size_t at = value_at(s, key, from);
  if (at == std::string::npos || at >= s.size() || s[at] != '[') return false;
  const char* p = s.c_str() + at + 1;
  char* end = nullptr;
  a = std::strtod(p, &end);
  if (end == p) return false;
  p = end;
  while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
  b = std::strtod(p, &end);
  return end != p;
}

double lerp(double a, double b, double f) { return a + (b - a) * f; }

}  // namespace

bool parse_oracle_line(const std::string& text, OracleLine& out, std::string* error) {
  out = OracleLine{};
  std::istringstream in(text);
  std::string line;
  bool have_start = false;
  while (std::getline(in, line)) {
    std::string event;
    if (!string_value(line, "event", event)) continue;
    if (event == "dlc.rally.race_trace") {
      std::string started, ended, reason;
      double t = -1;
      if (string_value(line, "started", started) && string_value(line, "ended", ended) &&
          string_value(line, "end_reason", reason) && started == "1" && ended == "1" && reason == "1" &&
          number(line, "time_72", t))
        out.race_seconds = t;
      continue;
    }
    if (event != "fh1.physics_trace") continue;
    LineSample s;
    std::string cars;
    if (!number(line, "t_s", s.t) || !number(line, "x", s.x) || !number(line, "y", s.y) ||
        !number(line, "z", s.z) || !number(line, "distance_m", s.distance) || !string_value(line, "cars", cars))
      continue;
    number(line, "yaw_deg", s.yaw_deg);
    pair(line, "ai_grip", s.grip_front, s.grip_rear, 0);
    // 0:R34:0:speed:rpm:gear:gas:brake:steer
    std::vector<std::string> f;
    std::stringstream cs(cars.substr(0, cars.find('|')));
    for (std::string part; std::getline(cs, part, ':');) f.push_back(part);
    if (f.size() != 9) continue;
    s.speed = std::strtod(f[3].c_str(), nullptr);
    s.gas = std::strtod(f[6].c_str(), nullptr);
    s.brake = std::strtod(f[7].c_str(), nullptr);
    s.steer = std::strtod(f[8].c_str(), nullptr);
    if (s.t <= 0) {
      out.start = s;
      have_start = true;
    }
    out.samples.push_back(s);
  }
  const bool any_lap = std::any_of(out.samples.begin(), out.samples.end(), [](const LineSample& s) { return s.t >= 0; });
  if (!any_lap) {
    if (error) *error = "no fh1.physics_trace sample at t_s >= 0";
    return false;
  }
  if (!have_start) out.start = out.samples.front();
  return true;
}

bool parse_gate(const std::string& lap_json, Gate& out, std::string* error) {
  const size_t at = value_at(lap_json, "gate");
  double nx = 0, nz = 0;
  if (at == std::string::npos || !pair(lap_json, "point_xz", out.x, out.z, at) ||
      !pair(lap_json, "crossing_direction_xz", nx, nz, at)) {
    if (error) *error = "no gate with point_xz and crossing_direction_xz";
    return false;
  }
  const double len = std::hypot(nx, nz);
  if (len <= 0) {
    if (error) *error = "the gate's crossing direction is zero";
    return false;
  }
  out.nx = nx / len;
  out.nz = nz / len;
  return true;
}

LineFollower::LineFollower(const OracleLine& line, const Options& options) : options_(options) {
  for (const auto& s : line.samples) {
    if (s.t < 0) continue;
    // The pedal table: the last sample at equal distances.
    if (!pd_.empty() && s.distance <= pd_.back()) {
      pgas_.back() = s.gas;
      pbrake_.back() = s.brake;
      pgf_.back() = s.grip_front;
      pgr_.back() = s.grip_rear;
    } else {
      pd_.push_back(s.distance);
      pgas_.push_back(s.gas);
      pbrake_.push_back(s.brake);
      pgf_.push_back(s.grip_front);
      pgr_.push_back(s.grip_rear);
    }
    // The line: a point each time the path has moved on.
    if (dist_.empty() || s.distance > dist_.back() + 1e-3) {
      px_.push_back(s.x);
      pz_.push_back(s.z);
      dist_.push_back(s.distance);
    }
  }
  if (!dist_.empty()) progress_ = dist_.front();
}

void LineFollower::pedals_at(double distance, double& gas, double& brake) const {
  gas = brake = 0;
  if (pd_.empty()) return;
  const auto it = std::upper_bound(pd_.begin(), pd_.end(), distance);
  if (it == pd_.begin()) {
    gas = pgas_.front();
    brake = pbrake_.front();
    return;
  }
  const size_t i = size_t(it - pd_.begin()) - 1;
  if (i + 1 >= pd_.size()) {
    gas = pgas_.back();
    brake = pbrake_.back();
    return;
  }
  const double f = (distance - pd_[i]) / (pd_[i + 1] - pd_[i]);
  gas = lerp(pgas_[i], pgas_[i + 1], f);
  brake = lerp(pbrake_[i], pbrake_[i + 1], f);
}

void LineFollower::grip_at(double distance, double& front, double& rear) const {
  front = rear = 1;
  if (pd_.empty()) return;
  const auto it = std::upper_bound(pd_.begin(), pd_.end(), distance);
  if (it == pd_.begin()) {
    front = pgf_.front(), rear = pgr_.front();
    return;
  }
  const size_t i = size_t(it - pd_.begin()) - 1;
  if (i + 1 >= pd_.size()) {
    front = pgf_.back(), rear = pgr_.back();
    return;
  }
  const double f = (distance - pd_[i]) / (pd_[i + 1] - pd_[i]);
  front = lerp(pgf_[i], pgf_[i + 1], f);
  rear = lerp(pgr_[i], pgr_[i + 1], f);
}

void LineFollower::point_at(double distance, double& x, double& z) const {
  if (dist_.size() < 2) {
    x = px_.empty() ? 0 : px_[0];
    z = pz_.empty() ? 0 : pz_[0];
    return;
  }
  // Past the end, the last segment carries on straight.
  auto it = std::upper_bound(dist_.begin(), dist_.end(), distance);
  size_t i = it == dist_.begin() ? 0 : size_t(it - dist_.begin()) - 1;
  i = std::min(i, dist_.size() - 2);
  const double f = (distance - dist_[i]) / (dist_[i + 1] - dist_[i]);
  x = lerp(px_[i], px_[i + 1], f);
  z = lerp(pz_[i], pz_[i + 1], f);
}

void LineFollower::project(double x, double z) {
  if (dist_.size() < 2) return;
  double best = 1e300;
  size_t best_seg = seg_;
  double best_f = 0;
  for (size_t i = seg_; i + 1 < dist_.size() && dist_[i] <= dist_[seg_] + options_.search; ++i) {
    const double ex = px_[i + 1] - px_[i], ez = pz_[i + 1] - pz_[i];
    const double len2 = ex * ex + ez * ez;
    double f = len2 > 0 ? ((x - px_[i]) * ex + (z - pz_[i]) * ez) / len2 : 0;
    f = std::clamp(f, 0.0, 1.0);
    const double dx = x - (px_[i] + f * ex), dz = z - (pz_[i] + f * ez);
    const double d2 = dx * dx + dz * dz;
    if (d2 < best) best = d2, best_seg = i, best_f = f;
  }
  seg_ = best_seg;
  const double p = lerp(dist_[seg_], dist_[seg_ + 1], best_f);
  progress_ = std::max(progress_, p);  // forward only
  // Signed: positive when the car is to the right of the line's direction.
  // At heading h the body's forward is (sin h, cos h) and its right (+x)
  // is (cos h, -sin h): the right of a direction (ex, ez) is (ez, -ex).
  const double ex = px_[seg_ + 1] - px_[seg_], ez = pz_[seg_ + 1] - pz_[seg_];
  const double len = std::hypot(ex, ez);
  const double ox = x - lerp(px_[seg_], px_[seg_ + 1], best_f), oz = z - lerp(pz_[seg_], pz_[seg_ + 1], best_f);
  cross_ = len > 0 ? (ox * ez - oz * ex) / len : 0;
}

Input LineFollower::input(const Vehicle& v) {
  const Vec3 at = v.position();
  project(at.x, at.z);
  Input in;
  pedals_at(progress_, in.throttle, in.brake);
  // Pure pursuit: the arc through the look-ahead point from the body point.
  const double speed = std::fabs(v.speed());
  const double look = std::max(options_.min_lookahead, options_.lookahead_s * speed);
  double tx = 0, tz = 0;
  point_at(progress_ + look, tx, tz);
  const Vec3 rel = rotate_inverse(v.orientation(), {tx - at.x, 0, tz - at.z});
  const double ld = std::hypot(rel.x, rel.z);
  const auto& c = v.params().wheel_centres;
  const double wheelbase = std::fabs((c[0].z + c[1].z) / 2 - (c[2].z + c[3].z) / 2);
  const double alpha = std::atan2(rel.x, rel.z);
  double delta = ld > 1e-6 ? std::atan(2 * wheelbase * std::sin(alpha) / ld) : 0;
  delta -= std::atan(options_.cross_gain * cross_ / (speed + 1));
  const double lock = v.steer_lock();
  const double stick = lock > 0 ? delta / lock : 0;
  saturated_ = std::fabs(stick) > 1;
  in.steer = std::clamp(stick, -1.0, 1.0);
  return in;
}

bool GateTimer::update(double t0, double x0, double z0, double t1, double x1, double z1, double progress) {
  if (crossed()) return true;
  const double s0 = side(x0, z0), s1 = side(x1, z1);
  if (!(s0 < 0 && s1 >= 0)) return false;
  ++crossings_;
  const double f = s1 > s0 ? -s0 / (s1 - s0) : 0;
  const double x = lerp(x0, x1, f), z = lerp(z0, z1, f);
  const double lateral = (x - gate_.x) * -gate_.nz + (z - gate_.z) * gate_.nx;
  if (progress < min_progress_ || std::fabs(lateral) > half_width_) return false;
  time_ = lerp(t0, t1, f);
  return true;
}

std::string lap_trace_event(const Vehicle& v, int64_t car_id, double race_t, double distance) {
  const Vec3 p = v.position();
  const Vec3 f = rotate(v.orientation(), {0, 0, 1});
  const Vec3 vel = v.velocity();
  const auto& w = v.wheels();
  const auto& in = v.shaped_input();
  char buf[640];
  std::snprintf(buf, sizeof buf,
                "{\"event\":\"fh1.physics_trace\",\"frame\":%lld,\"mode\":\"3\",\"t_s\":%.6f,"
                "\"cars\":\"0:%lld:0:%.3f:%.0f:%d:%.3f:%.3f:%.3f\",\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,"
                "\"yaw_deg\":%.3f,\"vx\":%.4f,\"vy\":%.4f,\"vz\":%.4f,\"distance_m\":%.3f,"
                "\"surfaces\":[%d,%d,%d,%d],\"contact\":[%d,%d,%d,%d]}",
                static_cast<long long>(std::llround(race_t * 60)), race_t, static_cast<long long>(car_id),
                std::fabs(v.speed()), v.rpm(), v.gear(), in.throttle, in.brake, in.steer, p.x, p.y, p.z,
                std::atan2(f.x, f.z) * 180 / kPi, vel.x, vel.y, vel.z, distance, w[0].surface, w[1].surface,
                w[2].surface, w[3].surface, int(w[0].contact), int(w[1].contact), int(w[2].contact),
                int(w[3].contact));
  return buf;
}

std::string race_trace_event(bool finished, double race_seconds) {
  char buf[256];
  if (finished)
    std::snprintf(buf, sizeof buf,
                  "{\"schema\":1,\"event\":\"dlc.rally.race_trace\",\"driver_type\":\"0\",\"mode\":\"3\","
                  "\"route\":\"0\",\"started\":\"1\",\"ended\":\"1\",\"end_reason\":\"1\","
                  "\"time_72\":\"%.6f\",\"time_88\":\"%.6f\"}",
                  race_seconds, race_seconds);
  else
    std::snprintf(buf, sizeof buf,
                  "{\"schema\":1,\"event\":\"dlc.rally.race_trace\",\"driver_type\":\"0\",\"mode\":\"3\","
                  "\"route\":\"0\",\"started\":\"1\",\"ended\":\"0\",\"end_reason\":\"0\"}");
  return buf;
}

}  // namespace drive
