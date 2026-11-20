#include "traffic/population.h"

#include <algorithm>
#include <cmath>

namespace traffic {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMph = 0.44704;
constexpr double kLane = 2.2;           // m right of the centre line (SPEC-TRAFFIC, Driving a lane)
constexpr double kDespawnMargin = 20;   // past the spawn-out distance (370 -> 390 m)
constexpr double kAwayLerpSpeed = 20;   // AI/Spawning/SpawnAwayChanceLerpSpeed, read as m/s (ours)
constexpr double kBendDecel = 3;        // m/s^2 when slowing for a bend (ours)
constexpr double kCarLength = 6;        // m added to the look-ahead gap, bumper to bumper (ours)
constexpr double kLaneHalfWidth = 2.0;  // m: another car this close to the lane line is in it (ours)

double hdist(const P3& a, const P3& b) { return std::hypot(a.x - b.x, a.z - b.z); }

uint64_t mix(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}

}  // namespace

Population::Population(const Roads& roads, const TrafficConfig& config, uint64_t seed)
    : roads_(roads), config_(config), rng_(seed) {
  for (const auto& m : config_.models) available_.push_back(m.id);
}

void Population::set_available(std::vector<int64_t> models) {
  if (models.empty())
    for (const auto& m : config_.models) models.push_back(m.id);
  available_ = std::move(models);
}

double Population::road_speed(const Road& r) const {
  const DensityEntry* e = entry(r);
  const double mph = e && e->speed_mph > 0 ? e->speed_mph : 30;
  return std::clamp(mph * kMph, config_.speed_min, config_.speed_max);
}

double Population::despawn_distance(const Car& car) const {
  if (car.road < 0) return 0;
  const DensityEntry* e = entry(roads_.roads()[size_t(car.road)]);
  return (e && e->spawn_out > 0 ? e->spawn_out : config_.spawn_out) + kDespawnMargin;
}

double Population::gap_needed(double player_speed) const {
  const double f = std::clamp(player_speed / config_.speed_at_max_gap, 0.0, 1.0);
  return config_.min_gap + (config_.max_gap - config_.min_gap) * f;
}

void Population::measure(const P3& player) {
  if (hdist(player, within_at_) > 10) {
    within_ = roads_.traffic_length_within(player.x, player.z, config_.spawn_out);
    within_at_ = player;
  }
  const double v = factor();
  double d = 0;
  for (const auto& [id, m] : within_)
    if (const DensityEntry* e = config_.density(id)) d += density_at(*e, v) * m / 100;
  smoothed_ = d + (smoothed_ - d) * config_.density_decay;
  target_ = std::min(smoothed_, traffic_cap(config_, v));
}

void Population::rotate_models() {
  std::vector<int64_t> out;
  for (int64_t m : available_)
    if (std::find(loaded_.begin(), loaded_.end(), m) == loaded_.end()) out.push_back(m);
  const size_t want = size_t(std::max(1, config_.max_loaded_models));
  if (loaded_.size() >= want && !loaded_.empty() && !out.empty()) loaded_.erase(loaded_.begin());
  while (loaded_.size() < want && !out.empty()) {
    const size_t k = size_t(uniform(0, double(out.size())));
    loaded_.push_back(out[std::min(k, out.size() - 1)]);
    out.erase(out.begin() + std::ptrdiff_t(std::min(k, out.size() - 1)));
  }
}

int64_t Population::pick_model(const DensityEntry& e) {
  std::vector<int64_t> ok;
  for (int64_t m : loaded_) {
    if (!e.allows(m)) continue;
    int limit = 0;
    for (const auto& tm : config_.models)
      if (tm.id == m) limit = tm.max_active;
    if (limit > 0) {
      int active = 0;
      for (const auto& c : cars_) active += c.model == m;
      if (active >= limit) continue;
    }
    ok.push_back(m);
  }
  if (ok.empty()) return 0;
  return ok[std::min(ok.size() - 1, size_t(uniform(0, double(ok.size()))))];
}

bool Population::spawn(const P3& player, double player_speed, bool initial) {
  struct Candidate {
    int road;
    double s, weight;
  };
  std::vector<Candidate> cands;
  double total = 0;
  const double v = factor();
  const auto& roads = roads_.roads();
  for (int ri = 0; ri < int(roads.size()); ++ri) {
    const Road& r = roads[size_t(ri)];
    const DensityEntry* e = entry(r);
    if (!e) continue;
    const double w = density_at(*e, v);
    if (w <= 0) continue;
    if (initial) {
      // Points every 10 m along the road between the initial minimum
      // distance and the spawn-in distance.
      for (double s = 0; s <= r.length; s += 10) {
        const double d = hdist(roads_.point_at(ri, s), player);
        if (d >= config_.initial_min_distance && d <= config_.spawn_in) cands.push_back({ri, s, w}), total += w;
      }
      continue;
    }
    const double radius = e->spawn_in > 0 ? e->spawn_in : config_.spawn_in;
    for (size_t k = 0; k + 1 < r.points.size(); ++k) {
      const P3 &a = r.points[k], &b = r.points[k + 1];
      const double dx = b.x - a.x, dz = b.z - a.z, ax = a.x - player.x, az = a.z - player.z;
      const double qa = dx * dx + dz * dz, qb = 2 * (ax * dx + az * dz), qc = ax * ax + az * az - radius * radius;
      const double disc = qb * qb - 4 * qa * qc;
      if (qa <= 0 || disc < 0) continue;
      for (double sign : {-1.0, 1.0}) {
        const double t = (-qb + sign * std::sqrt(disc)) / (2 * qa);
        if (t < 0 || t > 1) continue;
        cands.push_back({ri, r.s[k] + t * (r.s[k + 1] - r.s[k]), w});
        total += w;
      }
    }
  }
  const double gap = gap_needed(player_speed);
  // A few tries: a weighted pick, kept clear of the other cars.
  for (int attempt = 0; attempt < 8 && total > 0; ++attempt) {
    double pick = uniform(0, total);
    const Candidate* c = &cands.back();
    for (const auto& x : cands) {
      if (pick < x.weight) {
        c = &x;
        break;
      }
      pick -= x.weight;
    }
    const Road& r = roads[size_t(c->road)];
    const DensityEntry* e = entry(r);
    std::array<double, 2> tangent;
    const P3 at = roads_.point_at(c->road, c->s, nullptr, &tangent);
    bool clear = true;
    for (const auto& o : cars_) clear &= hdist(o.position, at) >= gap;
    if (!clear) continue;
    const int64_t model = pick_model(*e);
    if (!model) return false;
    int dir;
    bool away = false;
    if (r.oneway) {
      dir = 1;
    } else if (initial) {
      dir = uniform(0, 1) < 0.5 ? 1 : -1;
    } else {
      const double towards = (player.x - at.x) * tangent[0] + (player.z - at.z) * tangent[1];
      const double f = std::clamp(player_speed / kAwayLerpSpeed, 0.0, 1.0);
      const double chance =
          config_.away_chance_min + (std::min(e->away_chance, config_.away_chance_max) - config_.away_chance_min) * f;
      away = uniform(0, 1) < chance;
      dir = (towards > 0) != away ? 1 : -1;
    }
    Car car;
    car.id = next_id_++;
    car.model = model;
    car.base_speed = road_speed(r) * (1 + config_.speed_variance * uniform(-1, 1));
    car.speed = car.target = car.base_speed;
    if (!place(car, c->road, c->s, dir)) continue;
    const double d = hdist(car.position, player);
    if (initial) {
      stats_.initial++;
    } else {
      stats_.spawned++;
      stats_.spawn_distances.push_back(d);
      if (!r.oneway) {
        // Away: its travel takes it farther from the player.
        std::array<double, 2> t;
        roads_.point_at(car.road, car.s, nullptr, &t);
        const double out = (car.position.x - player.x) * t[0] * dir + (car.position.z - player.z) * t[1] * dir;
        stats_.spawned_away += out > 0;
      }
    }
    cars_.push_back(car);
    return true;
  }
  return false;
}

bool Population::place(Car& car, int road, double s, int dir) {
  car.road = road;
  car.s = s;
  car.dir = dir;
  car.lateral = kLane;
  pose(car);
  return true;
}

void Population::pose(Car& car) const {
  if (car.road < 0) return;
  std::array<double, 2> t;
  const P3 c = roads_.point_at(car.road, car.s, nullptr, &t);
  t = {t[0] * car.dir, t[1] * car.dir};
  const auto right = right_of(t[0], t[1]);
  P3 p{c.x + right[0] * car.lateral, c.y, c.z + right[1] * car.lateral};
  double y = p.y;
  if (ground_ && ground_(p.x, p.z, c.y, y)) p.y = y;
  car.pitch = 0;
  if (ground_) {
    double yf, yb;
    constexpr double kHalf = 1.4;
    if (ground_(p.x + t[0] * kHalf, p.z + t[1] * kHalf, p.y, yf) && ground_(p.x - t[0] * kHalf, p.z - t[1] * kHalf, p.y, yb))
      car.pitch = std::atan2(yf - yb, 2 * kHalf);
  }
  car.position = p;
  car.heading = std::atan2(t[0], t[1]);
}

void Population::advance(Car& car, double distance) const {
  const auto& roads = roads_.roads();
  for (int guard = 0; distance > 0 && car.road >= 0 && guard < 64; ++guard) {
    const Road& r = roads[size_t(car.road)];
    const int n = int(r.points.size());
    int k;
    if (car.dir > 0) {
      k = int(std::upper_bound(r.s.begin(), r.s.end(), car.s) - r.s.begin());
      if (k >= n) k = n - 1;
    } else {
      k = int(std::lower_bound(r.s.begin(), r.s.end(), car.s) - r.s.begin()) - 1;
      if (k < 0) k = 0;
    }
    const double to_node = std::fabs(r.s[size_t(k)] - car.s);
    if (distance < to_node) {
      car.s += car.dir * distance;
      return;
    }
    distance -= to_node;
    car.s = r.s[size_t(k)];
    const uint32_t node = r.nodes[size_t(k)];
    const bool at_end = car.dir > 0 ? k == n - 1 : k == 0;
    if (!at_end && roads_.degree(node) < 3) {
      if (to_node <= 0) car.s += car.dir * 1e-6;  // past a repeated point
      continue;
    }
    // A junction or the road's end: any way traffic may use at the node,
    // in a direction a one-way allows, never back the way it came.
    struct Option {
      int road, index, dir;
    };
    std::vector<Option> options, allowed;
    for (const auto& [r2, i2] : roads_.at_node(node)) {
      const Road& o = roads[size_t(r2)];
      if (!o.traffic) continue;
      for (int d2 : {1, -1}) {
        if (d2 > 0 && i2 + 1 >= int(o.points.size())) continue;
        if (d2 < 0 && (i2 == 0 || o.oneway)) continue;
        if (r2 == car.road && d2 == -car.dir) continue;
        options.push_back({r2, i2, d2});
        const DensityEntry* e = entry(o);
        if (e && e->allows(car.model)) allowed.push_back({r2, i2, d2});
      }
    }
    const auto& pickfrom = allowed.empty() ? options : allowed;
    if (pickfrom.empty()) {
      // A dead end: turn round where the road allows it, else leave.
      if (r.oneway) {
        car.road = -1;
        return;
      }
      car.dir = -car.dir;
      car.last_node = int(node);
      continue;
    }
    // The choice is fixed per car and node, so a look ahead along the
    // lane takes the same way the car will.
    const Option& o = pickfrom[size_t(mix(uint64_t(car.id) * 0x9e3779b97f4a7c15ull ^ node) % pickfrom.size())];
    car.road = o.road;
    car.dir = o.dir;
    car.s = roads[size_t(o.road)].s[size_t(o.index)] + o.dir * 1e-6;
    car.last_node = int(node);
  }
}

P3 Population::lane_point(const Car& car, double ahead) const {
  Car c = car;
  advance(c, ahead);
  if (c.road < 0) return car.position;
  pose(c);
  return c.position;
}

void Population::drive(Car& car, double dt, const P3& player, const P3& player_velocity) {
  const Road& r = roads_.roads()[size_t(car.road)];
  double target = car.base_speed;
  // Bends ahead: the speed for each point's radius, reached at kBendDecel.
  {
    const double reach = car.speed * car.speed / (2 * kBendDecel) + 20;
    const int n = int(r.points.size());
    for (int k = 0; k < n; ++k) {
      const double d = (r.s[size_t(k)] - car.s) * car.dir;
      if (d < 0 || d > reach) continue;
      const double f = std::clamp((r.radius[size_t(k)] - config_.radius_for_min_speed) /
                                      (config_.radius_for_max_speed - config_.radius_for_min_speed),
                                  0.0, 1.0);
      const double vb = config_.speed_min + (car.base_speed - config_.speed_min) * f;
      target = std::min(target, std::sqrt(vb * vb + 2 * kBendDecel * d));
    }
  }
  // The car or player ahead in the lane: match its speed, stop short of it.
  const double fx = std::sin(car.heading), fz = std::cos(car.heading);
  const auto right = right_of(fx, fz);
  auto ahead = [&](const P3& p, double vx, double vz, double search) {
    const double rx = p.x - car.position.x, rz = p.z - car.position.z;
    const double d = rx * fx + rz * fz, lat = rx * right[0] + rz * right[1];
    if (d <= 0 || d > search || std::fabs(lat) > kLaneHalfWidth) return;
    const double need = std::max(config_.min_look_ahead, car.speed * config_.look_ahead_time) + kCarLength;
    const double along = vx * fx + vz * fz;
    if (d < need) target = std::min(target, d < 0.6 * need ? 0.0 : std::max(0.0, along - 2.0));
    else if (d < 2 * need) target = std::min(target, std::max(0.0, along) + 2.0);
  };
  for (const auto& o : cars_) {
    if (o.id == car.id) continue;
    ahead(o.position, std::sin(o.heading) * o.speed, std::cos(o.heading) * o.speed, config_.traffic_search);
  }
  ahead(player, player_velocity.x, player_velocity.z, config_.player_search);
  car.target = target;
  if (car.physics && bodies_) return;  // the host's body moves it
  if (target > car.speed) car.speed = std::min(target, car.speed + config_.accel_max * dt);
  else car.speed = std::max(target, car.speed - config_.decel_max * dt);
  advance(car, car.speed * dt);
  pose(car);
}

void Population::follow(size_t index, const P3& body, double heading, double speed) {
  Car& car = cars_[index];
  if (car.road < 0) return;
  // Its place on its road near where it was (or the road it has reached).
  const Road& r = roads_.roads()[size_t(car.road)];
  double best = 1e30, best_s = car.s, best_lat = 0;
  for (size_t k = 0; k + 1 < r.points.size(); ++k) {
    if (std::fabs(r.s[k] - car.s) > 60 && std::fabs(r.s[k + 1] - car.s) > 60) continue;
    const P3 &a = r.points[k], &b = r.points[k + 1];
    const double dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz;
    const double t = l2 > 0 ? std::clamp(((body.x - a.x) * dx + (body.z - a.z) * dz) / l2, 0.0, 1.0) : 0;
    const double px = a.x + dx * t, pz = a.z + dz * t, d = std::hypot(body.x - px, body.z - pz);
    if (d < best) {
      best = d;
      best_s = r.s[k] + t * (r.s[k + 1] - r.s[k]);
      const double l = std::sqrt(l2);
      const auto rt = right_of(l > 0 ? dx / l : 0, l > 0 ? dz / l : 1);
      best_lat = ((body.x - px) * rt[0] + (body.z - pz) * rt[1]) * car.dir;
    }
  }
  const double moved = (best_s - car.s) * car.dir;
  if (moved > 0) advance(car, moved);
  car.speed = speed;
  car.position = body;
  car.heading = heading;
  // Off its line: well away from the lane or across it; back once close
  // and pointing along it (ours).
  std::array<double, 2> t;
  if (car.road >= 0) roads_.point_at(car.road, car.s, nullptr, &t);
  else t = {std::sin(heading), std::cos(heading)};
  const double lane_heading = std::atan2(t[0] * car.dir, t[1] * car.dir);
  const double herr = std::fabs(std::remainder(heading - lane_heading, 2 * kPi));
  const double lerr = std::fabs(best_lat - kLane);
  if (best > 30) car.off_line = true;
  else if (car.off_line) car.off_line = !(lerr < 0.75 && herr < 0.2);
  else car.off_line = lerr > 2.0 || herr > 0.6;
}

void Population::step(double dt, const P3& player, const P3& player_velocity) {
  time_ += dt;
  const double player_speed = std::hypot(player_velocity.x, player_velocity.z);
  if (!started_) {
    started_ = true;
    rotate_models();
    next_rotation_ = time_ + config_.model_rotation_time;
    measure(player);
    const int n = std::min(config_.initial_cars, config_.initial_max_cars);
    for (int i = 0; i < n; ++i) spawn(player, player_speed, true);
    next_test_ = time_ + config_.density_test_interval;
    next_spawn_ = time_ + uniform(config_.interval_med - config_.interval_variance,
                                  config_.interval_med + config_.interval_variance);
  }
  while (time_ >= next_test_) {
    measure(player);
    next_test_ += config_.density_test_interval;
  }
  if (time_ >= next_rotation_) {
    rotate_models();
    next_rotation_ += config_.model_rotation_time;
  }
  // Despawn past the spawn-out distance (and cars that left the network).
  for (size_t i = cars_.size(); i-- > 0;) {
    const double d = hdist(cars_[i].position, player);
    if (cars_[i].road < 0 || d > despawn_distance(cars_[i])) {
      if (cars_[i].road >= 0) {
        stats_.despawned++;
        stats_.despawn_distances.push_back(d);
      }
      cars_.erase(cars_.begin() + std::ptrdiff_t(i));
    }
  }
  if (time_ >= next_spawn_) {
    if (double(cars_.size()) < target_) {
      const bool ok = spawn(player, player_speed, false);
      const double wait = ok ? std::clamp(uniform(config_.interval_med - config_.interval_variance,
                                                  config_.interval_med + config_.interval_variance),
                                          config_.interval_min, config_.interval_max)
                             : config_.interval_min;
      next_spawn_ = time_ + wait;
    } else {
      next_spawn_ = time_ + config_.interval_min;
    }
  }
  for (auto& car : cars_) {
    car.age += dt;
    car.physics = car.off_line || hdist(car.position, player) < config_.high_lod_distance;
    if (car.road >= 0) drive(car, dt, player, player_velocity);
  }
}

}  // namespace traffic
