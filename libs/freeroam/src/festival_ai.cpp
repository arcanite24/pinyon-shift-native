#include "freeroam/festival_ai.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace freeroam {
namespace {

constexpr double kLane = 2.2;
constexpr double kDespawnDist = 420.0;
constexpr double kPromptDist = 38.0;

double hdist(const traffic::P3& a, const traffic::P3& b) { return std::hypot(a.x - b.x, a.z - b.z); }

uint64_t mix64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}

constexpr const char* kFestivalDrivers[] = {
    "Marcus Vance",  "Elena Rostova", "Devon Brooks", "Sofia Morales", "Liam Takahashi",
    "Chloe Bennett", "Mateo Alvarez", "Hannah Cho",   "Lucas Meyer",   "Zoe Carter",
};

}  // namespace

int64_t h2h_wager_credits(int pi, int wristband_level) {
  const int64_t base = 1500 + std::clamp(wristband_level, 0, 6) * 750 + std::clamp(pi - 300, 0, 700) * 6;
  // Round to nearest 500 CR.
  return ((base + 250) / 500) * 500;
}

FestivalManager::FestivalManager(const traffic::NavMap& nav, const traffic::Roads& roads,
                                 const traffic::TrafficConfig& config, uint64_t seed)
    : nav_(nav), roads_(roads), config_(config), rng_(seed) {}

void FestivalManager::set_roster(std::vector<FestivalRacerSpec> roster) {
  roster_ = std::move(roster);
  for (size_t i = 0; i < roster_.size(); ++i) {
    if (roster_[i].driver_name.empty())
      roster_[i].driver_name = kFestivalDrivers[i % (sizeof(kFestivalDrivers) / sizeof(kFestivalDrivers[0]))];
  }
}

void FestivalManager::pose_roaming(FestivalAiCar& car) const {
  if (car.road < 0 || size_t(car.road) >= roads_.roads().size()) return;
  std::array<double, 2> t;
  const traffic::P3 c = roads_.point_at(car.road, car.s, nullptr, &t);
  t = {t[0] * car.dir, t[1] * car.dir};
  const auto rt = traffic::right_of(t[0], t[1]);
  traffic::P3 p{c.x + rt[0] * car.lateral, c.y, c.z + rt[1] * car.lateral};
  double y = p.y;
  if (ground_ && ground_(p.x, p.z, c.y, y)) p.y = y;
  car.pitch = 0.0;
  if (ground_) {
    double yf, yb;
    if (ground_(p.x + t[0] * 1.4, p.z + t[1] * 1.4, p.y, yf) &&
        ground_(p.x - t[0] * 1.4, p.z - t[1] * 1.4, p.y, yb))
      car.pitch = std::atan2(yf - yb, 2.8);
  }
  car.position = p;
  car.heading = std::atan2(t[0], t[1]);
}

void FestivalManager::advance_roaming(FestivalAiCar& car, double distance) const {
  const auto& roads = roads_.roads();
  for (int guard = 0; distance > 0 && car.road >= 0 && guard < 32; ++guard) {
    const traffic::Road& r = roads[size_t(car.road)];
    const int n = int(r.points.size());
    int k = car.dir > 0 ? int(std::upper_bound(r.s.begin(), r.s.end(), car.s) - r.s.begin())
                        : int(std::lower_bound(r.s.begin(), r.s.end(), car.s) - r.s.begin()) - 1;
    k = std::clamp(k, 0, n - 1);
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
      if (to_node <= 0) car.s += car.dir * 1e-6;
      continue;
    }
    struct Opt {
      int road, idx, dir;
    };
    std::vector<Opt> opts;
    for (const auto& [r2, i2] : roads_.at_node(node)) {
      const auto& o = roads[size_t(r2)];
      if (o.disabled || o.type == "none") continue;
      for (int d2 : {1, -1}) {
        if (d2 > 0 && i2 + 1 >= int(o.points.size())) continue;
        if (d2 < 0 && (i2 == 0 || o.oneway)) continue;
        if (r2 == car.road && d2 == -car.dir) continue;
        opts.push_back({r2, i2, d2});
      }
    }
    if (opts.empty()) {
      if (r.oneway) {
        car.road = -1;
        return;
      }
      car.dir = -car.dir;
      continue;
    }
    const Opt& o = opts[size_t(mix64(uint64_t(car.id) * 0x9e3779b97f4a7c15ull ^ node) % opts.size())];
    car.road = o.road;
    car.dir = o.dir;
    car.s = roads[size_t(o.road)].s[size_t(o.idx)] + o.dir * 1e-6;
  }
}

bool FestivalManager::spawn_roaming(const traffic::P3& player, bool initial) {
  if (roster_.empty() || roads_.roads().empty()) return false;
  const double v = traffic::time_factor(hour_, config_);
  const auto& roads = roads_.roads();
  struct Cand {
    int road;
    double s, w;
  };
  std::vector<Cand> cands;
  double total = 0.0;
  const double rmin = initial ? 45.0 : 180.0;
  const double rmax = initial ? 240.0 : 320.0;
  for (int ri = 0; ri < int(roads.size()); ++ri) {
    const auto& r = roads[size_t(ri)];
    if (r.disabled || r.type == "none" || r.points.size() < 2) continue;
    double w = 0.2;
    if (const auto* fe = config_.festival_density(r.density)) w = traffic::density_at(*fe, v);
    if (w <= 0.0) continue;
    for (double s = 0; s <= r.length; s += 25.0) {
      const double d = hdist(roads_.point_at(ri, s), player);
      if (d >= rmin && d <= rmax) {
        cands.push_back({ri, s, w});
        total += w;
      }
    }
  }
  if (cands.empty() || total <= 0.0) return false;
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  double pick = u01(rng_) * total;
  const Cand* chosen = &cands.back();
  for (const auto& c : cands) {
    if (pick < c.w) {
      chosen = &c;
      break;
    }
    pick -= c.w;
  }
  const auto& r = roads[size_t(chosen->road)];
  FestivalAiCar car;
  car.id = next_id_++;
  car.spec = roster_[size_t(rng_() % roster_.size())];
  car.road = chosen->road;
  car.s = chosen->s;
  car.dir = r.oneway ? 1 : (u01(rng_) < 0.5 ? 1 : -1);
  car.lateral = kLane;
  car.base_speed = std::clamp(22.0 + double(car.spec.pi) * 0.02, 20.0, 38.0);
  car.speed = car.base_speed;
  pose_roaming(car);
  cars_.push_back(std::move(car));
  return true;
}

uint32_t FestivalManager::spawn_ahead(const traffic::P3& player_pos, double player_heading, double distance_m) {
  if (roster_.empty()) return 0;
  const double fx = std::sin(player_heading), fz = std::cos(player_heading);
  const double tx = player_pos.x + fx * distance_m;
  const double tz = player_pos.z + fz * distance_m;
  const auto hit = roads_.nearest(tx, tz, false, 80.0);
  if (hit.road < 0) return 0;
  const auto& r = roads_.roads()[size_t(hit.road)];
  std::array<double, 2> td;
  roads_.point_at(hit.road, hit.s, nullptr, &td);
  const int dir = r.oneway ? 1 : ((fx * td[0] + fz * td[1]) >= 0.0 ? 1 : -1);

  FestivalAiCar car;
  car.id = next_id_++;
  car.spec = roster_[size_t(rng_() % roster_.size())];
  car.road = hit.road;
  car.s = hit.s;
  car.dir = dir;
  car.lateral = kLane;
  car.base_speed = std::clamp(26.0 + double(car.spec.pi) * 0.025, 24.0, 42.0);
  car.speed = car.base_speed;
  pose_roaming(car);
  const uint32_t id = car.id;
  cars_.push_back(std::move(car));
  return id;
}

bool FestivalManager::build_h2h_route(const traffic::P3& start, double heading, double min_len, double max_len) {
  if (nav_.nodes.empty()) return false;
  struct Edge {
    uint32_t to;
    float len;
  };
  const size_t N = nav_.nodes.size();
  std::vector<std::vector<Edge>> adj(N);
  std::vector<uint8_t> on_road(N, 0);
  for (uint32_t w = 0; w < nav_.ways.size(); ++w) {
    const std::string_view rt = nav_.way_tag(w, "road_type");
    if (rt.empty() || rt == "none") continue;
    const uint32_t nc = nav_.ways[w].node_count;
    for (uint32_t k = 0; k + 1 < nc; ++k) {
      const uint32_t a = nav_.way_node(w, k), b = nav_.way_node(w, k + 1);
      if (a >= N || b >= N || a == b) continue;
      const float l = std::hypot(nav_.nodes[b].x - nav_.nodes[a].x, nav_.nodes[b].z - nav_.nodes[a].z);
      adj[a].push_back({b, l});
      adj[b].push_back({a, l});
      on_road[a] = on_road[b] = 1;
    }
  }
  uint32_t src = std::numeric_limits<uint32_t>::max();
  double best_d = 1e30;
  for (uint32_t i = 0; i < N; ++i) {
    if (!on_road[i]) continue;
    const double d = std::hypot(nav_.nodes[i].x - start.x, nav_.nodes[i].z - start.z);
    if (d < best_d) best_d = d, src = i;
  }
  if (src == std::numeric_limits<uint32_t>::max() || best_d > 120.0) return false;

  // Dijkstra outward up to max_len, preferring a destination ahead along heading.
  std::vector<float> dist(N, std::numeric_limits<float>::infinity());
  std::vector<uint32_t> prev(N, std::numeric_limits<uint32_t>::max());
  using Q = std::pair<float, uint32_t>;
  std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
  dist[src] = 0.0f;
  q.push({0.0f, src});
  const double fx = std::sin(heading), fz = std::cos(heading);
  uint32_t best_dst = std::numeric_limits<uint32_t>::max();
  double best_score = -1e30;
  while (!q.empty()) {
    const auto [d, u] = q.top();
    q.pop();
    if (d > dist[u] || d > float(max_len)) continue;
    if (d >= float(min_len)) {
      const double dx = nav_.nodes[u].x - start.x, dz = nav_.nodes[u].z - start.z;
      const double hd = std::max(1.0, std::hypot(dx, dz));
      const double forward_align = (dx * fx + dz * fz) / hd;
      const double score = forward_align * 1000.0 + double(d) * 0.2;
      if (score > best_score) {
        best_score = score;
        best_dst = u;
      }
    }
    for (const auto& e : adj[u]) {
      const float nd = d + e.len;
      if (nd < dist[e.to] && nd <= float(max_len)) {
        dist[e.to] = nd;
        prev[e.to] = u;
        q.push({nd, e.to});
      }
    }
  }
  if (best_dst == std::numeric_limits<uint32_t>::max()) return false;

  std::vector<uint32_t> chain;
  for (uint32_t v = best_dst; v != std::numeric_limits<uint32_t>::max(); v = prev[v]) {
    chain.push_back(v);
    if (v == src) break;
  }
  std::reverse(chain.begin(), chain.end());
  if (chain.size() < 2) return false;

  h2h_pts_.clear();
  h2h_s_.clear();
  h2h_route_2d_.clear();
  double accum = 0.0;
  for (size_t i = 0; i < chain.size(); ++i) {
    const auto& n = nav_.nodes[chain[i]];
    traffic::P3 p{n.x, n.y, n.z};
    if (!h2h_pts_.empty()) {
      const double step = hdist(h2h_pts_.back(), p);
      if (step < 0.5) continue;
      accum += step;
    }
    h2h_pts_.push_back(p);
    h2h_s_.push_back(accum);
    h2h_route_2d_.push_back({p.x, p.z});
  }
  if (h2h_pts_.size() < 2) return false;
  h2h_length_ = h2h_s_.back();
  h2h_finish_ = {h2h_pts_.back().x, h2h_pts_.back().y, h2h_pts_.back().z};
  return true;
}

double FestivalManager::project_on_h2h(double x, double z, double hint_s) const {
  if (h2h_pts_.size() < 2) return 0.0;
  double best_d = 1e30, best_s = hint_s;
  for (size_t i = 0; i + 1 < h2h_pts_.size(); ++i) {
    if (h2h_s_[i + 1] < hint_s - 80.0 || h2h_s_[i] > hint_s + 220.0) continue;
    const auto &a = h2h_pts_[i], &b = h2h_pts_[i + 1];
    const double dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz;
    const double t = l2 > 1e-6 ? std::clamp(((x - a.x) * dx + (z - a.z) * dz) / l2, 0.0, 1.0) : 0.0;
    const double px = a.x + dx * t, pz = a.z + dz * t;
    const double d = std::hypot(x - px, z - pz);
    if (d < best_d) {
      best_d = d;
      best_s = h2h_s_[i] + t * (h2h_s_[i + 1] - h2h_s_[i]);
    }
  }
  return best_s;
}

void FestivalManager::pose_on_h2h(double s, double lateral, traffic::P3& pos, double& heading, double& pitch) const {
  if (h2h_pts_.size() < 2) return;
  const double sc = std::clamp(s, 0.0, h2h_length_);
  auto it = std::upper_bound(h2h_s_.begin(), h2h_s_.end(), sc);
  size_t hi = size_t(std::clamp<std::ptrdiff_t>(it - h2h_s_.begin(), 1, std::ptrdiff_t(h2h_pts_.size()) - 1));
  size_t lo = hi - 1;
  const double seg = std::max(1e-6, h2h_s_[hi] - h2h_s_[lo]);
  const double t = std::clamp((sc - h2h_s_[lo]) / seg, 0.0, 1.0);
  const auto &a = h2h_pts_[lo], &b = h2h_pts_[hi];
  double dx = b.x - a.x, dz = b.z - a.z;
  const double l = std::max(1e-6, std::hypot(dx, dz));
  dx /= l, dz /= l;
  const auto rt = traffic::right_of(dx, dz);
  pos = {a.x + (b.x - a.x) * t + rt[0] * lateral, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t + rt[1] * lateral};
  double y = pos.y;
  if (ground_ && ground_(pos.x, pos.z, pos.y, y)) pos.y = y;
  heading = std::atan2(dx, dz);
  pitch = std::atan2(b.y - a.y, l);
}

bool FestivalManager::start_head_to_head(const traffic::P3& player_pos, double player_heading, double /*time*/,
                                         uint32_t car_id) {
  if (h2h_active_) return false;
  const uint32_t target_id = car_id ? car_id : prompt_id_;
  FestivalAiCar* opp = nullptr;
  for (auto& c : cars_)
    if (c.id == target_id) opp = &c;
  if (!opp) return false;
  if (!build_h2h_route(player_pos, player_heading, 1800.0, 3400.0)) return false;

  h2h_active_ = true;
  h2h_car_id_ = opp->id;
  h2h_wager_ = h2h_wager_credits(opp->spec.pi, wristband_level_);
  h2h_elapsed_ = 0.0;
  h2h_player_s_ = project_on_h2h(player_pos.x, player_pos.z, 0.0);
  opp->in_h2h = true;
  opp->h2h_progress = std::max(h2h_player_s_ + 10.0, project_on_h2h(opp->position.x, opp->position.z, 0.0));
  opp->speed = std::max(opp->speed, 32.0);
  prompt_id_ = 0;
  return true;
}

void FestivalManager::cancel_head_to_head() {
  for (auto& c : cars_) c.in_h2h = false;
  h2h_active_ = false;
  h2h_car_id_ = 0;
  h2h_pts_.clear();
  h2h_s_.clear();
  h2h_route_2d_.clear();
}

bool FestivalManager::step(double dt, double time, const traffic::P3& player_pos, double player_heading,
                           double player_speed, HeadToHeadResult* finished_out) {
  if (dt <= 0.0) return false;
  if (!started_) {
    started_ = true;
    const int init_n = std::clamp(config_.initial_festival_cars, 1, int(config_.max_festival));
    for (int i = 0; i < init_n; ++i) spawn_roaming(player_pos, true);
    next_spawn_ = time + 4.0;
  }

  // Maintain roaming population of min_festival..max_festival (2..7 cars).
  const double v = traffic::time_factor(hour_, config_);
  const int target_n = std::clamp(int(std::lround(config_.min_festival + (config_.max_festival - config_.min_festival) * v)),
                                  1, 7);
  for (size_t i = cars_.size(); i-- > 0;) {
    if (cars_[i].in_h2h) continue;
    if (cars_[i].road < 0 || hdist(cars_[i].position, player_pos) > kDespawnDist) {
      cars_.erase(cars_.begin() + std::ptrdiff_t(i));
    }
  }
  if (!h2h_active_ && time >= next_spawn_ && int(cars_.size()) < target_n) {
    spawn_roaming(player_pos, false);
    next_spawn_ = time + 5.0;
  }

  // Update all cars.
  prompt_id_ = 0;
  prompt_dist_ = 1e9;
  const double pfx = std::sin(player_heading), pfz = std::cos(player_heading);

  for (auto& car : cars_) {
    if (car.in_h2h && h2h_active_ && car.id == h2h_car_id_) {
      // Racing pace along the Head-to-Head nav route (competitive with slight elasticity).
      const double base_race_spd = std::clamp(34.0 + double(car.spec.pi) * 0.032, 32.0, 58.0);
      const double gap = car.h2h_progress - h2h_player_s_;
      double want_spd = base_race_spd;
      if (gap > 80.0) want_spd *= 0.88;
      else if (gap < -80.0) want_spd *= 1.10;
      car.speed += std::clamp(want_spd - car.speed, -10.0 * dt, 8.0 * dt);
      car.h2h_progress += car.speed * dt;
      pose_on_h2h(car.h2h_progress, 1.5, car.position, car.heading, car.pitch);
    } else if (car.road >= 0) {
      car.speed += std::clamp(car.base_speed - car.speed, -6.0 * dt, 5.0 * dt);
      advance_roaming(car, car.speed * dt);
      pose_roaming(car);
      if (!h2h_active_) {
        const double dx = car.position.x - player_pos.x, dz = car.position.z - player_pos.z;
        const double d = std::hypot(dx, dz);
        const double ahead = dx * pfx + dz * pfz;
        if (d <= kPromptDist && ahead > -5.0 && d < prompt_dist_) {
          prompt_dist_ = d;
          prompt_id_ = car.id;
        }
      }
    }
  }

  // Active Head-to-Head progress & finish check.
  if (h2h_active_) {
    h2h_elapsed_ += dt;
    h2h_player_s_ = project_on_h2h(player_pos.x, player_pos.z, h2h_player_s_);
    FestivalAiCar* opp = nullptr;
    for (auto& c : cars_)
      if (c.id == h2h_car_id_) opp = &c;
    const double d_fin_player = std::hypot(player_pos.x - h2h_finish_[0], player_pos.z - h2h_finish_[2]);
    const bool player_done = h2h_player_s_ >= h2h_length_ - 18.0 || d_fin_player <= 22.0;
    const bool opp_done = opp && opp->h2h_progress >= h2h_length_ - 10.0;
    if (player_done || opp_done) {
      HeadToHeadResult res;
      res.won = player_done;
      res.opponent_name = opp ? opp->spec.driver_name : "FESTIVAL RIVAL";
      res.wager_credits = h2h_wager_;
      res.payout_credits = res.won ? h2h_wager_ : 0;
      res.popularity_points = res.won ? 1000 : 250;
      res.driver_xp = res.won ? 1500 : 400;
      res.elapsed_s = h2h_elapsed_;
      last_won_ = res.won;
      last_payout_ = res.payout_credits;
      last_opp_ = res.opponent_name;
      finish_banner_at_ = time;
      cancel_head_to_head();
      if (finished_out) *finished_out = res;
      return true;
    }
    (void)player_speed;
  }

  return false;
}

HeadToHeadHud FestivalManager::hud(double time) const {
  HeadToHeadHud h;
  if (!h2h_active_ && prompt_id_ != 0) {
    for (const auto& c : cars_) {
      if (c.id == prompt_id_) {
        h.prompt_visible = true;
        h.prompt_car_id = c.id;
        h.prompt_driver = c.spec.driver_name;
        h.prompt_car_class = c.spec.car_class;
        h.prompt_pi = c.spec.pi;
        h.prompt_wager = h2h_wager_credits(c.spec.pi, wristband_level_);
        h.prompt_distance_m = prompt_dist_;
        break;
      }
    }
  }
  if (h2h_active_) {
    h.active = true;
    h.wager_credits = h2h_wager_;
    h.route_length_m = h2h_length_;
    h.player_remaining_m = std::max(0.0, h2h_length_ - h2h_player_s_);
    h.elapsed_s = h2h_elapsed_;
    h.progress = h2h_length_ > 1.0 ? std::clamp(h2h_player_s_ / h2h_length_, 0.0, 1.0) : 0.0;
    for (const auto& c : cars_) {
      if (c.id == h2h_car_id_) {
        h.opponent_name = c.spec.driver_name;
        h.opponent_class = c.spec.car_class;
        h.opponent_pi = c.spec.pi;
        h.gap_m = h2h_player_s_ - c.h2h_progress;
        h.place = h.gap_m >= 0.0 ? 1 : 2;
        break;
      }
    }
  }
  const double since_fin = time - finish_banner_at_;
  if (since_fin >= 0.0 && since_fin < 5.5 && !last_opp_.empty()) {
    h.finish_banner = true;
    h.finish_won = last_won_;
    h.finish_credits = last_payout_;
    h.finish_opponent = last_opp_;
    h.finish_at = finish_banner_at_;
  }
  return h;
}

}  // namespace freeroam
