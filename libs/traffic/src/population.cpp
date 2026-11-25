#include "traffic/population.h"

#include <algorithm>
#include <cmath>

namespace traffic {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMph = 0.44704;
constexpr double kLane = 2.2;           // m right of the centre line (SPEC-TRAFFIC, Driving a lane)
constexpr double kFastLane = -1.8;      // m right of centre line (left/fast lane on multi-lane one-way freeways)
constexpr double kDespawnMargin = 20;   // past the spawn-out distance (370 -> 390 m)
constexpr double kAwayLerpSpeed = 20;   // AI/Spawning/SpawnAwayChanceLerpSpeed, read as m/s (ours)
constexpr double kBendDecel = 3;        // m/s^2 when slowing for a bend (ours)
constexpr double kCarLength = 6;        // m added to the look-ahead gap, bumper to bumper (ours)
constexpr double kLaneHalfWidth = 2.0;  // m: another car this close to the lane line is in it (ours)
constexpr double kPlayerHalfWidth = 2.4;  // m: the same for the player, who may straddle the lanes (ours)

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

void estimate_traffic_gear_rpm(double speed, const std::array<double, 4>& gear_max_speed, int& gear_out,
                               double& rpm_out) {
  const double v = std::max(0.0, speed);
  const double g0 = std::max(4.0, gear_max_speed[0]);
  const double g1 = std::max(g0 + 2.0, gear_max_speed[1]);
  const double g2 = std::max(g1 + 2.0, gear_max_speed[2]);
  const double g3 = std::max(g2 + 2.0, gear_max_speed[3]);
  double frac = 0.0;
  if (v < g0) {
    gear_out = 1;
    frac = v / g0;
  } else if (v < g1) {
    gear_out = 2;
    frac = (v - g0 * 0.55) / std::max(1.0, g1 - g0 * 0.55);
  } else if (v < g2) {
    gear_out = 3;
    frac = (v - g1 * 0.65) / std::max(1.0, g2 - g1 * 0.65);
  } else if (v < g3) {
    gear_out = 4;
    frac = (v - g2 * 0.70) / std::max(1.0, g3 - g2 * 0.70);
  } else {
    gear_out = 5;
    frac = (v - g3 * 0.75) / std::max(1.0, g3 * 0.45);
  }
  rpm_out = 850.0 + std::clamp(frac, 0.0, 1.0) * 4650.0;
}

Population::Population(const Roads& roads, const TrafficConfig& config, uint64_t seed)
    : roads_(roads), config_(config), rng_(seed) {
  for (const auto& m : config_.models) available_.push_back(m.id);
}

void Population::set_available(std::vector<int64_t> models) {
  if (models.empty())
    for (const auto& m : config_.models) models.push_back(m.id);
  available_ = std::move(models);
}

bool Population::is_multilane(const Road& r) const {
  return r.oneway && (r.type == "freeway" || r.density == 4 || r.density == 9 || r.density == 17);
}

double Population::lane_center(const Road& r, int lane_index) const {
  return (is_multilane(r) && lane_index == 1) ? kFastLane : kLane;
}

double Population::road_speed(const Road& r) const {
  const DensityEntry* e = entry(r);
  const double mph = e && e->speed_mph > 0 ? e->speed_mph : 30;
  return std::clamp(mph * kMph, config_.speed_min, config_.speed_max);
}

double Population::spawn_interval(double player_speed) {
  double wait = uniform(config_.interval_med - config_.interval_variance,
                        config_.interval_med + config_.interval_variance);
  const double sf =
      1.0 + config_.interval_speed_factor * std::clamp(player_speed / std::max(1.0, config_.interval_speed_max), 0.0, 1.0);
  wait /= sf;
  if (target_ < 1.2 && player_speed < 1.0) wait *= 1.25;
  return std::clamp(wait, config_.interval_min, config_.interval_max);
}

void Population::check_overspawn(const P3& player) {
  if (double(cars_.size()) <= target_ + 1.0) return;
  size_t best_idx = cars_.size();
  double best_score = -1.0;
  for (size_t i = 0; i < cars_.size(); ++i) {
    const Car& c = cars_[i];
    if (c.physics || c.off_line) continue;
    const double d = hdist(c.position, player);
    if (d <= config_.medium_lod_distance) continue;
    const double out_dot =
        (c.position.x - player.x) * std::sin(c.heading) + (c.position.z - player.z) * std::cos(c.heading);
    const double score = d + (out_dot > 0 ? 80.0 : 0.0);
    if (score > best_score) {
      best_score = score;
      best_idx = i;
    }
  }
  if (best_idx < cars_.size()) {
    const double d = hdist(cars_[best_idx].position, player);
    stats_.overspawn_culled++;
    if (d >= config_.spawn_out) {
      stats_.despawned++;
      stats_.despawn_distances.push_back(d);
    }
    cars_.erase(cars_.begin() + std::ptrdiff_t(best_idx));
  }
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
  const bool first_measure = within_at_.x > 1e20;
  if (hdist(player, within_at_) > 10) {
    within_ = roads_.traffic_length_within(player.x, player.z, config_.spawn_out);
    within_at_ = player;
  }
  const double v = factor();
  double d = 0;
  for (const auto& [id, m] : within_)
    if (const DensityEntry* e = config_.density(id)) d += density_at(*e, v) * m / 100;
  smoothed_ = first_measure ? d : (d + (smoothed_ - d) * config_.density_decay);
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

bool Population::spawn(const P3& player, double player_speed, bool initial, bool has_heading, double fwd_x,
                       double fwd_z) {
  struct Candidate {
    int road;
    double s, weight;
    bool behind = false;
  };
  std::vector<Candidate> cands;
  double total = 0;
  const double v = factor();
  const auto& roads = roads_.roads();
  auto add_cand = [&](int ri, double s, double w) {
    bool is_behind = false;
    if (!initial && has_heading) {
      const P3 pt = roads_.point_at(ri, s);
      const double dot_fwd = (pt.x - player.x) * fwd_x + (pt.z - player.z) * fwd_z;
      is_behind = dot_fwd < 0.0;
    }
    cands.push_back({ri, s, w, is_behind});
    total += w;
  };
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
        if (d >= config_.initial_min_distance && d <= config_.spawn_in) add_cand(ri, s, w);
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
        add_cand(ri, r.s[k] + t * (r.s[k + 1] - r.s[k]), w);
      }
    }
  }
  // Enforce BehindChance / BehindMaxSpeed when player heading is known:
  // above BehindMaxSpeed, avoid behind-player spawns; at rest/slow speed,
  // allow behind spawns with probability BehindChance.
  std::vector<Candidate> pool;
  double pool_total = 0.0;
  if (!initial && has_heading && !cands.empty()) {
    std::vector<Candidate> ahead_cands, behind_cands;
    double ahead_w = 0.0, behind_w = 0.0;
    for (const auto& x : cands) {
      if (x.behind) {
        behind_cands.push_back(x);
        behind_w += x.weight;
      } else {
        ahead_cands.push_back(x);
        ahead_w += x.weight;
      }
    }
    if (player_speed > config_.behind_max_speed) {
      if (!ahead_cands.empty()) {
        pool = std::move(ahead_cands);
        pool_total = ahead_w;
      } else {
        pool = std::move(cands);
        pool_total = total;
      }
    } else if (!ahead_cands.empty() && !behind_cands.empty()) {
      const bool pick_behind = uniform(0.0, 1.0) < std::clamp(config_.behind_chance, 0.0, 1.0);
      pool = pick_behind ? std::move(behind_cands) : std::move(ahead_cands);
      pool_total = pick_behind ? behind_w : ahead_w;
    } else {
      pool = std::move(cands);
      pool_total = total;
    }
  } else {
    pool = std::move(cands);
    pool_total = total;
  }
  const double gap = gap_needed(player_speed);
  // A few tries: a weighted pick, kept clear of the other cars.
  for (int attempt = 0; attempt < 8 && pool_total > 0; ++attempt) {
    double pick = uniform(0, pool_total);
    const Candidate* c = &pool.back();
    for (const auto& x : pool) {
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
    estimate_traffic_gear_rpm(car.speed, config_.gear_max_speed, car.gear, car.rpm);
    const int lane_idx = (is_multilane(r) && uniform(0, 1) < 0.35) ? 1 : 0;
    if (!place(car, c->road, c->s, dir, lane_idx)) continue;
    const double d = hdist(car.position, player);
    if (initial) {
      stats_.initial++;
    } else {
      stats_.spawned++;
      stats_.spawn_distances.push_back(d);
      if (has_heading && c->behind) stats_.spawned_behind++;
      if (!r.oneway) {
        // Away: its travel takes it farther from the player.
        std::array<double, 2> t;
        roads_.point_at(car.road, car.s, nullptr, &t);
        const double out = (car.position.x - player.x) * t[0] * dir + (car.position.z - player.z) * t[1] * dir;
        stats_.spawned_away += out > 0;
      }
    }
    cars_.push_back(car);
    // Occasional 2-3 car traffic convoys when TrafficGroupSpawns is active.
    if (!initial && config_.traffic_group_spawns && double(cars_.size()) < target_ &&
        uniform(0.0, 1.0) < std::clamp(config_.traffic_convoy_chance, 0.0, 1.0)) {
      const int extra = uniform(0.0, 1.0) < 0.5 ? 1 : 2;
      const double spacing = std::max(14.0, gap * 0.85);
      int added = 0;
      for (int k = 1; k <= extra && double(cars_.size()) < target_ + 0.5; ++k) {
        const double fs = c->s - double(dir) * spacing * double(k);
        if (fs < 2.0 || fs > r.length - 2.0) break;
        const P3 fpt = roads_.point_at(c->road, fs);
        bool fclear = true;
        for (const auto& o : cars_) fclear &= hdist(o.position, fpt) >= 10.0;
        if (!fclear) break;
        const int64_t fmodel = pick_model(*e);
        if (!fmodel) break;
        Car follower;
        follower.id = next_id_++;
        follower.model = fmodel;
        follower.base_speed = car.base_speed * (1.0 + 0.002 * uniform(-1.0, 1.0));
        follower.speed = follower.target = follower.base_speed;
        estimate_traffic_gear_rpm(follower.speed, config_.gear_max_speed, follower.gear, follower.rpm);
        if (!place(follower, c->road, fs, dir, lane_idx)) break;
        stats_.spawned++;
        stats_.spawn_distances.push_back(hdist(follower.position, player));
        if (has_heading && c->behind) stats_.spawned_behind++;
        cars_.push_back(follower);
        ++added;
      }
      if (added > 0) stats_.convoys_spawned++;
    }
    return true;
  }
  return false;
}

bool Population::place(Car& car, int road, double s, int dir, int lane_index) {
  car.road = road;
  car.s = s;
  car.dir = dir;
  const Road& r = roads_.roads()[size_t(road)];
  car.lane_index = is_multilane(r) ? std::clamp(lane_index, 0, 1) : 0;
  car.target_lateral = lane_center(r, car.lane_index);
  car.lateral = car.target_lateral;
  car.avoid_offset = 0;
  pose(car);
  return true;
}

void Population::add(Car car, int road, double s, int dir) {
  car.id = next_id_++;
  place(car, road, s, dir, car.lane_index);
  cars_.push_back(car);
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
  car.steer_angle = 0.0;
  car.yaw_rate = 0.0;
  car.pose_initialized = true;
}

bool Population::pick_junction_option(const Car& car, uint32_t node, const std::array<double, 2>& in_tangent,
                                      JunctionOption& out) const {
  const auto& roads = roads_.roads();
  auto is_paved = [](const std::string& t) { return t == "a" || t == "b" || t == "freeway"; };
  const bool cur_paved = car.road >= 0 && size_t(car.road) < roads.size() && is_paved(roads[size_t(car.road)].type);
  const std::string cur_type =
      (car.road >= 0 && size_t(car.road) < roads.size()) ? roads[size_t(car.road)].type : std::string();

  struct Cand {
    JunctionOption opt;
    double cos_turn = 0.0;
    bool paved = false;
    bool same_type = false;
    bool allowed = false;
  };
  std::vector<Cand> options, allowed;
  for (const auto& [r2, i2] : roads_.at_node(node)) {
    const Road& o = roads[size_t(r2)];
    if (!o.traffic) continue;
    const int n2 = int(o.points.size());
    const DensityEntry* e = entry(o);
    const bool ok_model = e && e->allows(car.model);
    for (int d2 : {1, -1}) {
      if (d2 > 0 && i2 + 1 >= n2) continue;
      if (d2 < 0 && (i2 == 0 || o.oneway)) continue;
      if (r2 == car.road && d2 == -car.dir) continue;
      int nk = std::clamp(i2 + d2, 0, n2 - 1);
      double ox = o.points[size_t(nk)].x - o.points[size_t(i2)].x;
      double oz = o.points[size_t(nk)].z - o.points[size_t(i2)].z;
      double ol = std::hypot(ox, oz);
      while (ol < 1.0 && nk + d2 >= 0 && nk + d2 < n2) {
        nk += d2;
        ox = o.points[size_t(nk)].x - o.points[size_t(i2)].x;
        oz = o.points[size_t(nk)].z - o.points[size_t(i2)].z;
        ol = std::hypot(ox, oz);
      }
      if (ol > 1e-6) ox /= ol, oz /= ol;
      else ox = in_tangent[0], oz = in_tangent[1];
      const double cos_turn = std::clamp(in_tangent[0] * ox + in_tangent[1] * oz, -1.0, 1.0);
      Cand c{{r2, i2, d2}, cos_turn, is_paved(o.type), o.type == cur_type, ok_model};
      options.push_back(c);
      if (ok_model) allowed.push_back(c);
    }
  }
  const auto& pool = allowed.empty() ? options : allowed;
  if (pool.empty()) return false;

  bool has_non_turnback = false;
  for (const auto& c : pool)
    if (c.cos_turn > -0.45) has_non_turnback = true;
  bool has_paved = false;
  if (cur_paved) {
    for (const auto& c : pool)
      if (c.paved && (!has_non_turnback || c.cos_turn > -0.45)) has_paved = true;
  }

  std::vector<std::pair<JunctionOption, uint64_t>> weighted;
  uint64_t total_w = 0;
  for (const auto& c : pool) {
    if (has_non_turnback && c.cos_turn <= -0.45) continue;
    uint64_t w = 1;
    if (c.cos_turn > 0.80) w = 12;       // straight continuation (< 37 deg)
    else if (c.cos_turn > 0.45) w = 4;   // gentle fork (37..63 deg)
    else if (c.cos_turn > -0.15) w = 2;  // standard intersection turn (63..99 deg)
    else w = 1;
    if (c.opt.road == car.road) w += 6;
    else if (c.same_type) w += 3;
    if (has_paved && !c.paved) w = 1;
    weighted.push_back({c.opt, w});
    total_w += w;
  }
  if (weighted.empty() || total_w == 0) {
    out = pool.front().opt;
    return true;
  }
  // Fixed per car and node so lookahead, indicators, and traversal always agree.
  uint64_t pick = mix(uint64_t(car.id) * 0x9e3779b97f4a7c15ull ^ uint64_t(node)) % total_w;
  for (const auto& [opt, w] : weighted) {
    if (pick < w) {
      out = opt;
      return true;
    }
    pick -= w;
  }
  out = weighted.back().first;
  return true;
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
    const int prev_k = std::clamp(k - car.dir, 0, n - 1);
    double ix = r.points[size_t(k)].x - r.points[size_t(prev_k)].x;
    double iz = r.points[size_t(k)].z - r.points[size_t(prev_k)].z;
    const double il = std::hypot(ix, iz);
    const std::array<double, 2> in_t =
        il > 1e-6 ? std::array<double, 2>{ix / il, iz / il} : std::array<double, 2>{std::sin(car.heading), std::cos(car.heading)};
    JunctionOption o;
    if (!pick_junction_option(car, node, in_t, o)) {
      // A dead end: turn round where the road allows it, else leave.
      if (r.oneway) {
        car.road = -1;
        return;
      }
      car.dir = -car.dir;
      car.last_node = int(node);
      continue;
    }
    car.road = o.road;
    car.dir = o.dir;
    car.s = roads[size_t(o.road)].s[size_t(o.index)] + o.dir * 1e-6;
    car.last_node = int(node);
  }
}

void Population::sync_cursor(Car& car, double max_advance) const {
  if (car.road < 0) return;
  const double span = std::max(8.0, max_advance);
  double best_ds = 0.0;
  double best_d2 = 1e30;
  constexpr double kCoarse = 0.75;
  for (double ds = 0.0; ds <= span; ds += kCoarse) {
    const P3 pt = lane_point(car, ds);
    const double dx = car.position.x - pt.x, dz = car.position.z - pt.z;
    const double d2 = dx * dx + dz * dz;
    if (d2 < best_d2) {
      best_d2 = d2;
      best_ds = ds;
    }
  }
  const double lo = std::max(0.0, best_ds - kCoarse);
  const double hi = std::min(span, best_ds + kCoarse);
  for (double ds = lo; ds <= hi; ds += 0.15) {
    const P3 pt = lane_point(car, ds);
    const double dx = car.position.x - pt.x, dz = car.position.z - pt.z;
    const double d2 = dx * dx + dz * dz;
    if (d2 < best_d2) {
      best_d2 = d2;
      best_ds = ds;
    }
  }
  if (best_ds > 1e-4) advance(car, best_ds);
}

void Population::steer_kinematic(Car& car, const P3& aim, double dt) const {
  constexpr double kWheelbase = 2.65;
  constexpr double kMaxSteer = 0.58;
  constexpr double kMaxSteerRate = 2.3;
  constexpr double kMaxLatG = 7.0;
  constexpr double kMaxYawRate = 1.45;
  const double fx = std::sin(car.heading), fz = std::cos(car.heading);
  const auto rt = right_of(fx, fz);
  const double dx = aim.x - car.position.x, dz = aim.z - car.position.z;
  const double lx = dx * rt[0] + dz * rt[1];
  const double lz = dx * fx + dz * fz;
  const double ld = std::max(3.0, std::hypot(lx, lz));
  const double alpha = std::atan2(lx, lz);
  double target_steer = std::atan(2.0 * kWheelbase * std::sin(alpha) / ld);
  const double v_eff = std::max(car.speed, 2.0);
  const double grip_steer = std::clamp(std::atan(kWheelbase * kMaxLatG / (v_eff * v_eff)), 0.06, kMaxSteer);
  target_steer = std::clamp(target_steer, -grip_steer, grip_steer);
  car.steer_angle += std::clamp(target_steer - car.steer_angle, -kMaxSteerRate * dt, kMaxSteerRate * dt);

  double target_yaw = (car.speed / kWheelbase) * std::tan(car.steer_angle);
  const double yaw_cap = std::min(kMaxYawRate, kMaxLatG / std::max(car.speed, 3.5));
  target_yaw = std::clamp(target_yaw, -yaw_cap, yaw_cap);
  car.yaw_rate += (target_yaw - car.yaw_rate) * std::min(1.0, dt / 0.08);
  car.heading = std::remainder(car.heading + car.yaw_rate * dt, 2.0 * kPi);

  const double nfx = std::sin(car.heading), nfz = std::cos(car.heading);
  car.position.x += nfx * car.speed * dt;
  car.position.z += nfz * car.speed * dt;

  double y_hint = std::isfinite(car.position.y) ? 0.5 * (car.position.y + aim.y) : aim.y;
  double y = y_hint;
  if (ground_ && ground_(car.position.x, car.position.z, y_hint, y)) {
    car.position.y = y;
  } else {
    car.position.y += (aim.y - car.position.y) * std::min(1.0, dt * 8.0);
  }
  double target_pitch = 0.0;
  if (ground_) {
    double yf = car.position.y, yb = car.position.y;
    constexpr double kHalf = 1.4;
    if (ground_(car.position.x + nfx * kHalf, car.position.z + nfz * kHalf, car.position.y, yf) &&
        ground_(car.position.x - nfx * kHalf, car.position.z - nfz * kHalf, car.position.y, yb)) {
      target_pitch = std::clamp(std::atan2(yf - yb, 2.0 * kHalf), -0.45, 0.45);
    }
  }
  car.pitch += (target_pitch - car.pitch) * std::min(1.0, dt / 0.12);
}

P3 Population::lane_point(const Car& car, double ahead) const {
  Car c = car;
  advance(c, ahead);
  if (c.road < 0) return car.position;
  pose(c);
  return c.position;
}

bool Population::next_junction(const Car& car, double max_dist, uint32_t& node_out, double& dist_out, P3& pos_out,
                               double& turn_angle_out) const {
  if (car.road < 0) return false;
  const auto& roads = roads_.roads();
  const Road& r = roads[size_t(car.road)];
  const int n = int(r.points.size());
  if (n < 2) return false;
  double walked = 0;
  double cur_s = car.s;
  int k = car.dir > 0 ? int(std::upper_bound(r.s.begin(), r.s.end(), cur_s) - r.s.begin())
                      : int(std::lower_bound(r.s.begin(), r.s.end(), cur_s) - r.s.begin()) - 1;
  while (k >= 0 && k < n) {
    const double step = std::fabs(r.s[size_t(k)] - cur_s);
    walked += step;
    if (walked > max_dist) return false;
    cur_s = r.s[size_t(k)];
    const uint32_t node = r.nodes[size_t(k)];
    const bool at_end = car.dir > 0 ? k == n - 1 : k == 0;
    if (at_end || roads_.degree(node) >= 3) {
      // Incoming and outgoing tangents at the junction node.
      const int prev_k = std::clamp(k - car.dir, 0, n - 1);
      double ix = r.points[size_t(k)].x - r.points[size_t(prev_k)].x;
      double iz = r.points[size_t(k)].z - r.points[size_t(prev_k)].z;
      const double il = std::hypot(ix, iz);
      if (il > 1e-6) ix /= il, iz /= il;
      else ix = std::sin(car.heading), iz = std::cos(car.heading);
      JunctionOption o;
      if (!pick_junction_option(car, node, {ix, iz}, o)) return false;
      const Road& nr = roads[size_t(o.road)];
      const int next_k = std::clamp(o.index + o.dir, 0, int(nr.points.size()) - 1);
      double ox = nr.points[size_t(next_k)].x - nr.points[size_t(o.index)].x;
      double oz = nr.points[size_t(next_k)].z - nr.points[size_t(o.index)].z;
      const double ol = std::hypot(ox, oz);
      if (ol > 1e-6) ox /= ol, oz /= ol;
      else ox = ix, oz = iz;
      // Signed turn angle (+ right, - left) in (+X right, +Z forward).
      const double cross = iz * ox - ix * oz;
      const double dot = ix * ox + iz * oz;
      node_out = node;
      dist_out = walked;
      pos_out = r.points[size_t(k)];
      turn_angle_out = std::atan2(cross, dot);
      return true;
    }
    k += car.dir;
  }
  return false;
}

uint32_t Population::junction_holder(uint32_t node) const {
  const auto it = junction_reservations_.find(node);
  return it == junction_reservations_.end() ? 0u : it->second.holder_id;
}

void Population::update_junction_reservations() {
  // 1. Release reservations whose holder despawned, went off-line, timed out,
  //    or cleanly exited the intersection box.
  for (auto it = junction_reservations_.begin(); it != junction_reservations_.end();) {
    const uint32_t node = it->first;
    const JunctionReservation& res = it->second;
    const Car* holder = nullptr;
    for (const auto& c : cars_) {
      if (c.id == res.holder_id) {
        holder = &c;
        break;
      }
    }
    bool release = !holder || holder->road < 0 || holder->off_line || time_ >= res.expires_at;
    if (!release) {
      uint32_t nj_node = 0;
      double nj_dist = 0.0, nj_turn = 0.0;
      P3 nj_pos{};
      const bool has_nj = next_junction(*holder, config_.junction_max_collision_check_distance, nj_node, nj_dist,
                                        nj_pos, nj_turn);
      const bool still_approaching = has_nj && nj_node == node;
      const double dist_to_j = hdist(holder->position, res.junction_pos);
      // Past the node and driving away from it at 1 m/s or more: the
      // crossing check (JunctionManoeuvreSafeTimeGap) no longer counts the
      // holder, so neither does its reservation. A holder stopped in the box
      // keeps it until it is 7.5 m clear.
      const double hx = holder->position.x - res.junction_pos.x, hz = holder->position.z - res.junction_pos.z;
      const double away =
          dist_to_j > 1e-3
              ? (std::sin(holder->heading) * hx + std::cos(holder->heading) * hz) * holder->speed / dist_to_j
              : 0.0;
      if ((!still_approaching && (dist_to_j > 7.5 || away >= 1.0)) ||
          dist_to_j > config_.junction_max_collision_check_distance + 12.0) {
        release = true;
      }
    }
    it = release ? junction_reservations_.erase(it) : std::next(it);
  }

  // 2. For any unreserved junction approached by traffic cars within the reservation
  //    window, grant priority to the earliest-arriving car (tie-break by distance, then id).
  struct Claim {
    double eta = 1e30;
    double dist = 1e30;
    uint32_t id = 0;
    int road = -1;
    int dir = 1;
    P3 pos{};
  };
  std::map<uint32_t, Claim> best_claims;
  for (const auto& car : cars_) {
    if (car.road < 0 || car.off_line) continue;
    uint32_t jnode = 0;
    double jdist = 0.0, turn_angle = 0.0;
    P3 jpos{};
    if (!next_junction(car, config_.junction_max_collision_check_distance, jnode, jdist, jpos, turn_angle)) continue;
    if (junction_reservations_.count(jnode)) continue;
    const double reserve_dist =
        std::min(config_.junction_max_collision_check_distance, std::max(28.0, car.speed * 2.6 + 10.0));
    if (jdist > reserve_dist) continue;
    const double eta = jdist <= 3.0 ? 0.0 : (jdist / std::max(2.0, car.speed));
    auto it = best_claims.find(jnode);
    const bool better = (it == best_claims.end()) || (eta < it->second.eta - 1e-6) ||
                        (std::fabs(eta - it->second.eta) <= 1e-6 &&
                         (jdist < it->second.dist - 1e-6 ||
                          (std::fabs(jdist - it->second.dist) <= 1e-6 && car.id < it->second.id)));
    if (better) {
      best_claims[jnode] = {eta, jdist, car.id, car.road, car.dir, jpos};
    }
  }
  for (const auto& [jnode, cl] : best_claims) {
    junction_reservations_[jnode] = {cl.id, cl.road, cl.dir, cl.pos, time_, time_ + 8.0};
  }
}

void Population::drive(Car& car, double dt, const P3& player, const P3& player_velocity,
                       std::span<const ExternalVehicle> obstacles) {
  const Road& r = roads_.roads()[size_t(car.road)];
  double target = car.base_speed;
  if (config_.gear_max_speed[3] > config_.speed_min) target = std::min(target, config_.gear_max_speed[3]);
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
  // Indicators decay unless refreshed by a junction turn or lane change.
  if (car.indicator_hold > 0) {
    car.indicator_hold = std::max(0.0, car.indicator_hold - dt);
    if (car.indicator_hold <= 0) car.turn_left = car.turn_right = false;
  }
  // Headlight flash timers (AI/Traffic/CollisionResponse).
  if (car.flash_timer > 0) {
    car.flash_timer = std::max(0.0, car.flash_timer - dt);
    car.flash_headlamps = car.flash_timer > 0;
  } else {
    car.flash_headlamps = false;
  }
  if (car.flash_cooldown > 0) car.flash_cooldown = std::max(0.0, car.flash_cooldown - dt);

  bool emergency_brake = false;

  // Junction approach: turn indicators, SpeedAtGivewayJunctions, per-junction right-of-way
  // reservation, and cross-traffic give-way checks.
  {
    uint32_t jnode = 0;
    double jdist = 0, turn_angle = 0;
    P3 jpos;
    if (next_junction(car, config_.junction_max_collision_check_distance, jnode, jdist, jpos, turn_angle)) {
      const double ind_dist = std::max(config_.min_distance_before_indicating,
                                       car.speed * config_.time_before_indicating);
      if (jdist <= ind_dist && std::fabs(turn_angle) > 0.35) {
        car.turn_left = turn_angle < 0;
        car.turn_right = turn_angle > 0;
        car.indicator_hold = config_.time_to_keep_indicating;
      }
      // Slow smoothly to SpeedAtGivewayJunctions for junction turns or forced give-way intersections.
      const double v_giveway = std::clamp(config_.giveway_speed, config_.speed_min, car.base_speed);
      if (std::fabs(turn_angle) > 0.30) {
        const double vturn =
            std::min(v_giveway, std::clamp(16.0 - 7.0 * std::fabs(turn_angle), config_.speed_min, car.base_speed));
        const double brk_d = std::max(0.0, jdist - 5.0);
        target = std::min(target, std::sqrt(vturn * vturn + 2.0 * kBendDecel * brk_d));
      } else if (config_.junction_force_give_way && jdist < 30.0) {
        const double brk_d = std::max(0.0, jdist - 4.0);
        target = std::min(target, std::sqrt(v_giveway * v_giveway + 2.0 * kBendDecel * brk_d));
      }
      // Per-junction right-of-way reservation: first approaching car holds priority
      // until it clears the junction; cross-traffic approaching from another road
      // yields cleanly at the stop line instead of ghosting through simultaneously.
      bool holds_reservation = false;
      bool reserved_by_cross = false;
      if (const auto rit = junction_reservations_.find(jnode); rit != junction_reservations_.end()) {
        if (rit->second.holder_id == car.id) {
          holds_reservation = true;
        } else {
          const bool same_stream = (rit->second.holder_road == car.road && rit->second.holder_dir == car.dir);
          if (!same_stream) reserved_by_cross = true;
        }
      }
      if (reserved_by_cross && !car.off_line && jdist > 1.5) {
        const double stop_d = std::max(0.0, jdist - 7.0);
        const double vstop = stop_d < 1.2 ? 0.0 : std::sqrt(2.0 * config_.decel_max * stop_d);
        target = std::min(target, std::min(vstop, v_giveway));
        if (jdist < 14.0) emergency_brake = true;
      }
      // Give-way against approaching cross-traffic, player, or Race AI opponents.
      if (jdist > 2.5 && !car.off_line) {
        const double t_me = jdist / std::max(2.0, car.speed);
        auto check_cross = [&](const P3& op, double ovx, double ovz, bool priority) {
          const double dx = jpos.x - op.x, dz = jpos.z - op.z;
          const double od = std::hypot(dx, dz);
          if (od < 1.0 || od > config_.junction_max_collision_check_distance) return;
          const double ov = (ovx * dx + ovz * dz) / od;
          if (ov < 1.0) return;
          const double t_other = od / ov;
          const double gap = config_.junction_safe_time_gap;
          if (t_other < t_me + gap && t_me < t_other + gap) {
            const bool yield = config_.junction_force_give_way || priority || od < jdist - 2.0 ||
                               (std::fabs(turn_angle) > 0.35 && od < jdist + 8.0);
            if (yield) {
              const double stop_d = std::max(0.0, jdist - 6.0);
              const double vstop = stop_d < 1.5 ? 0.0 : std::sqrt(2.0 * config_.decel_max * stop_d);
              target = std::min(target, std::min(vstop, v_giveway));
            }
          }
        };
        if (!holds_reservation) {
          for (const auto& o : cars_) {
            if (o.id == car.id || o.road == car.road) continue;
            check_cross(o.position, std::sin(o.heading) * o.speed, std::cos(o.heading) * o.speed, false);
          }
        }
        check_cross(player, player_velocity.x, player_velocity.z, true);
        for (const auto& obs : obstacles) check_cross(obs.position, obs.velocity.x, obs.velocity.z, true);
      }
    }
  }

  // The car, player, or Race AI ahead in the lane: evaluated in the road-lane frame so a
  // temporarily yawed car recovering from a nudge does not false-brake for
  // vehicles parked off the shoulder, while accurately stopping for true
  // in-lane obstacles using kinematic stopping distance.
  std::array<double, 2> lane_t{};
  const P3 road_c = roads_.point_at(car.road, car.s, nullptr, &lane_t);
  const double fx = lane_t[0] * car.dir, fz = lane_t[1] * car.dir;
  const auto right = right_of(fx, fz);
  const P3 lane_origin{road_c.x + right[0] * car.lateral, road_c.y, road_c.z + right[1] * car.lateral};
  bool blocked_by_traffic = false;
  auto ahead = [&](const P3& p, double vx, double vz, double search, double half_width, bool is_traffic) {
    const double rx = p.x - lane_origin.x, rz = p.z - lane_origin.z;
    const double d = rx * fx + rz * fz, lat = rx * right[0] + rz * right[1];
    if (d <= 0 || d > search || std::fabs(lat) > half_width) return;
    const double along = vx * fx + vz * fz;
    const double v_lead = std::max(0.0, along);
    const double eff_decel = std::max(2.0, config_.decel_max * 0.72);
    const double stop_dist =
        car.speed > v_lead ? (car.speed * car.speed - v_lead * v_lead) / (2.0 * eff_decel) : 0.0;
    const double time_gap = std::max(config_.min_look_ahead, car.speed * config_.look_ahead_time);
    const double need = std::max(time_gap, stop_dist) + kCarLength + 2.0;
    if (is_traffic && along < car.base_speed - 1.5) blocked_by_traffic = true;
    const bool oncoming_headon = !is_traffic && along < -1.0;
    if (oncoming_headon && d < need + 14.0) {
      // Oncoming player or Race AI directly in our lane: brake sharply while swerving aside.
      target = std::min(target, std::max(0.0, (d - 10.0) * 0.35));
      emergency_brake = true;
      return;
    }
    if (d <= kCarLength + 3.0) {
      target = 0.0;
      emergency_brake = true;
    } else if (d < need) {
      const double room = std::max(0.0, d - (kCarLength + 2.5));
      const double v_safe = std::sqrt(v_lead * v_lead + 2.0 * (config_.decel_max * 0.85) * room);
      target = std::min(target, d < 0.55 * need ? std::min(v_safe, std::max(0.0, v_lead - 1.5)) : v_safe);
      if (d < 0.65 * need && v_lead < car.speed - 2.5) emergency_brake = true;
    } else if (d < 1.6 * need && v_lead < target) {
      target = std::min(target, v_lead + 2.0);
    }
  };
  for (const auto& o : cars_) {
    if (o.id == car.id) continue;
    ahead(o.position, std::sin(o.heading) * o.speed, std::cos(o.heading) * o.speed, config_.traffic_search,
          kLaneHalfWidth, true);
  }
  ahead(player, player_velocity.x, player_velocity.z, config_.player_search, kPlayerHalfWidth, false);
  for (const auto& obs : obstacles) {
    ahead(obs.position, obs.velocity.x, obs.velocity.z, config_.player_search, kPlayerHalfWidth, false);
  }

  // Multi-lane freeway driving & LaneChangeBehavior.
  car.since_lane_change += dt;
  if (is_multilane(r) && !car.off_line) {
    if (blocked_by_traffic) car.blocked_time += dt;
    else car.blocked_time = 0;
    const bool can_change = car.since_lane_change >= config_.lane_change_min_interval;
    const bool want_overtake = car.blocked_time >= config_.lane_change_time_blocked;
    const bool want_return = car.lane_index == 1 && !blocked_by_traffic &&
                             uniform(0, 1) < config_.lane_change_chance * dt;
    if (can_change && (want_overtake || want_return)) {
      const int new_lane = 1 - car.lane_index;
      const double delta_lat = lane_center(r, new_lane) - car.lateral;
      bool lane_clear = true;
      auto check_lane = [&](const P3& p) {
        const double rx = p.x - car.position.x, rz = p.z - car.position.z;
        const double d = rx * fx + rz * fz, lat = rx * right[0] + rz * right[1];
        if (d > -12.0 && d < 28.0 && std::fabs(lat - delta_lat) < 2.2) lane_clear = false;
      };
      for (const auto& o : cars_)
        if (o.id != car.id) check_lane(o.position);
      check_lane(player);
      for (const auto& obs : obstacles) check_lane(obs.position);
      if (lane_clear) {
        car.lane_index = new_lane;
        car.since_lane_change = 0;
        car.blocked_time = 0;
        car.turn_left = new_lane == 1;
        car.turn_right = new_lane == 0;
        car.indicator_hold = config_.time_to_keep_indicating;
      }
    }
  } else {
    car.lane_index = 0;
    car.blocked_time = 0;
  }
  car.target_lateral = lane_center(r, car.lane_index);

  // Player / Race AI swerve avoidance (MaxAvoidOffset / MaxAvoidDistance / MaxAvoidSpeed)
  // and CollisionResponse headlight flashing on oncoming lane encroachments.
  // Evaluated in road-tangent coordinates so avoid_offset always pushes away from the encroacher.
  {
    double want_avoid = 0.0;
    double best_d = 1e30;
    bool flash_trigger = false;
    auto check_avoid = [&](const P3& p, const P3& vel) {
      const double rx = p.x - (road_c.x + right[0] * car.target_lateral);
      const double rz = p.z - (road_c.z + right[1] * car.target_lateral);
      const double d = rx * fx + rz * fz, lat = rx * right[0] + rz * right[1];
      const double along = vel.x * fx + vel.z * fz;
      const double p_speed = std::hypot(vel.x, vel.z);
      if (!car.off_line && d > 0 && d < config_.avoid_max_distance && d < best_d && std::fabs(lat) < 2.6) {
        const bool oncoming = along < -1.0;
        const bool slow_blocker = p_speed < config_.avoid_max_speed && d < 42.0 && std::fabs(lat) < 2.5;
        if (oncoming || slow_blocker) {
          best_d = d;
          const double dir_sign = lat <= 0.2 ? 1.0 : -1.0;
          const double max_off = std::min(config_.avoid_max_offset, 2.2);
          const double mag = std::clamp((2.6 - std::fabs(lat)) * 0.9, 0.5, max_off);
          want_avoid = dir_sign * mag;
          flash_trigger = oncoming;
        }
      }
    };
    check_avoid(player, player_velocity);
    for (const auto& obs : obstacles) check_avoid(obs.position, obs.velocity);
    if (flash_trigger && car.flash_cooldown <= 0) {
      car.flash_cooldown = config_.time_between_light_flashes;
      if (uniform(0, 1) < config_.light_flash_chance) {
        car.flash_timer = config_.light_flash_time;
        car.flash_headlamps = true;
      }
    }
    const double avoid_step = 2.6 * dt;
    car.avoid_offset += std::clamp(want_avoid - car.avoid_offset, -avoid_step, avoid_step);
    const double desired_lat = car.target_lateral + car.avoid_offset;
    const double lat_rate = std::fabs(car.avoid_offset) > 0.05 ? 2.6 : std::max(1.2, config_.lane_change_rate * 4.5);
    car.lateral += std::clamp(desired_lat - car.lateral, -lat_rate * dt, lat_rate * dt);
  }

  // Hold cornering speed while still heavily steered mid-turn so cars don't
  // gun the throttle and swing wide out of 90-degree intersections.
  if (std::fabs(car.steer_angle) > 0.16) {
    const double turn_cap = std::clamp(13.0 - 15.0 * std::fabs(car.steer_angle), config_.speed_min, car.base_speed);
    target = std::min(target, turn_cap);
  }
  car.target = target;
  if (car.physics && bodies_ && car.body) return;  // the host's body moves it
  const double prev_speed = car.speed;
  const double decel = emergency_brake ? config_.decel_max * 1.45 : config_.decel_max;
  if (target > car.speed) car.speed = std::min(target, car.speed + config_.accel_max * dt);
  else car.speed = std::max(target, car.speed - decel * dt);
  car.accel = dt > 1e-6 ? (car.speed - prev_speed) / dt : 0.0;
  estimate_traffic_gear_rpm(car.speed, config_.gear_max_speed, car.gear, car.rpm);
  if (!car.pose_initialized) pose(car);
  if (car.speed > 0.02) {
    const double look_d = std::clamp(car.speed * 0.70, 9.0, 22.0);
    const P3 aim = lane_point(car, look_d);
    steer_kinematic(car, aim, dt);
    sync_cursor(car, car.speed * dt * 3.0 + 10.0);
  } else {
    car.yaw_rate *= std::exp(-dt * 8.0);
  }
}

void Population::follow(size_t index, const P3& body, double heading, double speed, double up_y) {
  Car& car = cars_[index];
  car.up_y = up_y;
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
  const double inst_accel = (speed - car.speed) * 120.0;
  car.accel += (inst_accel - car.accel) * 0.25;
  car.speed = speed;
  estimate_traffic_gear_rpm(car.speed, config_.gear_max_speed, car.gear, car.rpm);
  car.position = body;
  car.heading = heading;
  // Off its line: well away from the lane or across it; back once close
  // and pointing along it (ours).
  std::array<double, 2> t;
  if (car.road >= 0) roads_.point_at(car.road, car.s, nullptr, &t);
  else t = {std::sin(heading), std::cos(heading)};
  const double lane_heading = std::atan2(t[0] * car.dir, t[1] * car.dir);
  const double herr = std::fabs(std::remainder(heading - lane_heading, 2 * kPi));
  const double lerr = std::fabs(best_lat - car.lateral);
  if (best > 30) car.off_line = true;
  else if (car.off_line) car.off_line = !(lerr < 0.75 && herr < 0.2);
  else car.off_line = lerr > 2.8 || herr > 0.85;
}

void Population::step(double dt, const P3& player, const P3& player_velocity, double player_heading,
                      std::span<const ExternalVehicle> obstacles) {
  time_ += dt;
  const double player_speed = std::hypot(player_velocity.x, player_velocity.z);
  bool has_heading = false;
  double fwd_x = 0.0, fwd_z = 1.0;
  if (std::isfinite(player_heading)) {
    has_heading = true;
    fwd_x = std::sin(player_heading);
    fwd_z = std::cos(player_heading);
  } else if (player_speed > 0.5) {
    has_heading = true;
    fwd_x = player_velocity.x / player_speed;
    fwd_z = player_velocity.z / player_speed;
  }
  if (!started_) {
    started_ = true;
    rotate_models();
    next_rotation_ = time_ + config_.model_rotation_time;
    next_overspawn_ = time_ + config_.overspawn_check_time;
    measure(player);
    const int cap_init = target_ > 0 ? std::max(1, int(std::ceil(target_))) : 0;
    const int n = std::min({config_.initial_cars, config_.initial_max_cars, cap_init});
    for (int i = 0; i < n; ++i) spawn(player, player_speed, true, has_heading, fwd_x, fwd_z);
    next_test_ = time_ + config_.density_test_interval;
    next_spawn_ = time_ + spawn_interval(player_speed);
  }
  while (time_ >= next_test_) {
    measure(player);
    next_test_ += config_.density_test_interval;
  }
  if (time_ >= next_rotation_) {
    rotate_models();
    next_rotation_ += config_.model_rotation_time;
  }
  if (time_ >= next_overspawn_) {
    check_overspawn(player);
    next_overspawn_ += config_.overspawn_check_time;
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
      const bool ok = spawn(player, player_speed, false, has_heading, fwd_x, fwd_z);
      next_spawn_ = time_ + (ok ? spawn_interval(player_speed) : config_.interval_min);
    } else {
      next_spawn_ = time_ + (target_ < 1.5 ? spawn_interval(player_speed) : config_.interval_min);
    }
  }
  for (auto& car : cars_) {
    car.age += dt;
    // DeadBehavior: hazards after HazardLightDelay, reanimate after ReanimateDelay once stopped & upright.
    if (car.off_line) {
      car.dead_timer += dt;
      if (car.dead_timer >= config_.dead_hazard_delay) car.hazard_on = true;
      if (car.speed <= config_.dead_reanimate_max_speed) car.stopped_dead_timer += dt;
      else car.stopped_dead_timer = 0;
      if (car.stopped_dead_timer >= config_.dead_reanimate_delay && car.up_y > 0.5) {
        const auto hit = roads_.nearest(car.position.x, car.position.z, true, 60);
        if (hit.road >= 0) {
          const Road& nr = roads_.roads()[size_t(hit.road)];
          std::array<double, 2> td;
          roads_.point_at(hit.road, hit.s, nullptr, &td);
          const int ndir =
              nr.oneway ? 1 : ((std::sin(car.heading) * td[0] + std::cos(car.heading) * td[1]) >= 0 ? 1 : -1);
          car.road = hit.road;
          car.s = hit.s;
          car.dir = ndir;
          car.lane_index = 0;
          car.target_lateral = kLane;
          car.avoid_offset = 0;
          car.off_line = false;
          car.hazard_on = false;
          car.dead_timer = 0;
          car.stopped_dead_timer = 0;
          if (!car.body) {
            car.lateral = kLane;
            pose(car);
          }
        }
      }
    } else {
      car.dead_timer = 0;
      car.stopped_dead_timer = 0;
      car.hazard_on = false;
    }
    const double pdist = hdist(car.position, player);
    const double t_fade = config_.fade_in_time > 1e-3 ? std::clamp(car.age / config_.fade_in_time, 0.0, 1.0) : 1.0;
    const double d_out = std::max(config_.max_full_alpha_distance + 10.0, despawn_distance(car));
    const double d_fade = pdist <= config_.max_full_alpha_distance
                              ? 1.0
                              : std::clamp((d_out - pdist) / (d_out - config_.max_full_alpha_distance), 0.0, 1.0);
    car.alpha = float(t_fade * d_fade);
    bool near_actor = pdist < config_.high_lod_distance;
    if (!near_actor) {
      for (const auto& obs : obstacles) {
        if (obs.race_ai && hdist(car.position, obs.position) < config_.high_lod_distance) {
          near_actor = true;
          break;
        }
      }
    }
    car.physics = car.off_line || near_actor;
  }
  update_junction_reservations();
  for (auto& car : cars_) {
    if (car.road >= 0) drive(car, dt, player, player_velocity, obstacles);
  }
}

}  // namespace traffic
