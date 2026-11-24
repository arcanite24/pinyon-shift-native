#include "freeroam/festival_ai.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace freeroam {
namespace {

constexpr double kLane = 2.2;
constexpr double kDespawnDist = 420.0;
constexpr double kMphToMps = 0.44704;
constexpr double kSharpTurnCos = 0.8191520442889918;  // cos(35 deg)

double road_speed_mps(std::string_view rt) {
  if (rt == "freeway") return 75.0 * kMphToMps;
  if (rt == "a") return 60.0 * kMphToMps;
  if (rt == "b") return 40.0 * kMphToMps;
  if (rt == "dirt") return 35.0 * kMphToMps;
  return 25.0 * kMphToMps;
}

double h2h_top_speed_mps(int pi) {
  const double pi_d = std::clamp(double(pi), 100.0, 999.0);
  // PI-scaled top speed (m/s): D-class ~41 m/s up to R1/hypercar ~108 m/s (no 58 m/s cap).
  return std::clamp(34.0 + pi_d * 0.074, 40.0, 112.0);
}

double hdist(const traffic::P3& a, const traffic::P3& b) { return std::hypot(a.x - b.x, a.z - b.z); }

uint64_t mix64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}

// Fallback driver labels when gamedb.slt AIPlayers names are not loaded
// (unit tests without game data). Runtime callers populate real AIPlayers via set_driver_names().
constexpr const char* kFallbackFestivalDrivers[] = {
    "Horizon Racer 1", "Horizon Racer 2", "Horizon Racer 3", "Horizon Racer 4", "Horizon Racer 5",
    "Horizon Racer 6", "Horizon Racer 7", "Horizon Racer 8", "Horizon Racer 9", "Horizon Racer 10",
};

double wrap_angle(double a) {
  constexpr double kPi = 3.14159265358979323846;
  while (a > kPi) a -= 2.0 * kPi;
  while (a < -kPi) a += 2.0 * kPi;
  return a;
}

// Estimates realistic 6-speed gear-stepped engine RPM from vehicle speed (m/s).
double estimate_rpm_for_speed(double speed_mps, int pi) {
  const double v = std::fabs(speed_mps);
  const double top = h2h_top_speed_mps(pi);
  const double redline = std::clamp(6800.0 + double(pi) * 2.2, 6800.0, 9200.0);
  const double idle = 900.0;
  if (v < 1.0) return idle;
  // 6 evenly spaced gear bands.
  const double u = std::clamp(v / top, 0.0, 1.0);
  const double band = u * 6.0;
  const double frac = band - std::floor(band);
  return std::clamp(idle + (redline - idle) * (0.38 + 0.58 * frac), idle, redline);
}

}  // namespace

int64_t h2h_reward_credits(int player_pi, int opponent_pi) {
  constexpr int64_t kRewardBaseLevel = 5000;
  constexpr int64_t kRewardPlayerHigherMultiplier = 10;
  constexpr int64_t kRewardPlayerLowerMultiplier = 20;
  const int64_t diff = int64_t(opponent_pi) - int64_t(player_pi);
  int64_t reward = kRewardBaseLevel;
  if (diff > 0) {
    reward += diff * kRewardPlayerLowerMultiplier;
  } else if (diff < 0) {
    reward += diff * kRewardPlayerHigherMultiplier;
  }
  return std::max<int64_t>(500, reward);
}

int64_t h2h_wager_credits(int pi, int /*wristband_level*/) { return h2h_reward_credits(500, pi); }

FestivalManager::FestivalManager(const traffic::NavMap& nav, const traffic::Roads& roads,
                                 const traffic::TrafficConfig& config, uint64_t seed)
    : nav_(nav), roads_(roads), config_(config), rng_(seed) {}

void FestivalManager::set_driver_names(std::vector<std::string> names) {
  driver_names_ = std::move(names);
  if (!driver_names_.empty()) {
    for (size_t i = 0; i < roster_.size(); ++i) {
      if (roster_[i].driver_name.empty() || roster_[i].driver_name.rfind("Horizon Racer ", 0) == 0) {
        roster_[i].driver_name = driver_names_[i % driver_names_.size()];
      }
    }
  }
}

void FestivalManager::set_roster(std::vector<FestivalRacerSpec> roster) {
  roster_ = std::move(roster);
  for (size_t i = 0; i < roster_.size(); ++i) {
    if (roster_[i].driver_name.empty()) {
      if (!driver_names_.empty()) {
        roster_[i].driver_name = driver_names_[i % driver_names_.size()];
      } else {
        roster_[i].driver_name =
            kFallbackFestivalDrivers[i % (sizeof(kFallbackFestivalDrivers) / sizeof(kFallbackFestivalDrivers[0]))];
      }
    }
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
  car.steer_angle = 0.0;
  car.yaw_rate = 0.0;
  car.pose_initialized = true;
}

bool FestivalManager::pick_roaming_branch(uint32_t car_id, int cur_road, int cur_dir, uint32_t node,
                                          const std::array<double, 2>& in_tangent, BranchOption& out) const {
  const auto& roads = roads_.roads();
  auto is_paved = [](const std::string& t) { return t == "a" || t == "b" || t == "freeway"; };
  const bool cur_paved = cur_road >= 0 && size_t(cur_road) < roads.size() && is_paved(roads[size_t(cur_road)].type);
  const std::string cur_type =
      (cur_road >= 0 && size_t(cur_road) < roads.size()) ? roads[size_t(cur_road)].type : std::string();

  struct CandOpt {
    BranchOption opt;
    double cos_turn = 0.0;
    bool paved = false;
    bool same_type = false;
    bool shortcut = false;
  };
  std::vector<CandOpt> cands;
  for (const auto& [r2, i2] : roads_.at_node(node)) {
    const auto& o = roads[size_t(r2)];
    if (o.disabled || o.type == "none") continue;
    const int n2 = int(o.points.size());
    for (int d2 : {1, -1}) {
      if (d2 > 0 && i2 + 1 >= n2) continue;
      if (d2 < 0 && (i2 == 0 || o.oneway)) continue;
      if (r2 == cur_road && d2 == -cur_dir) continue;
      // Compute outgoing unit tangent over at least ~1 m if available.
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
      cands.push_back({{r2, i2, d2}, cos_turn, is_paved(o.type), o.type == cur_type, o.type == "shortcut"});
    }
  }
  if (cands.empty()) return false;

  // Avoid abrupt acute turnbacks (>117 deg) whenever a forward/normal branch exists.
  bool has_non_turnback = false;
  for (const auto& c : cands)
    if (c.cos_turn > -0.45) has_non_turnback = true;

  // On paved roads, avoid randomly darting onto dirt shortcuts when paved continuations exist.
  bool has_paved = false;
  if (cur_paved) {
    for (const auto& c : cands)
      if (c.paved && (!has_non_turnback || c.cos_turn > -0.45)) has_paved = true;
  }

  std::vector<std::pair<BranchOption, uint64_t>> weighted;
  uint64_t total_w = 0;
  for (const auto& c : cands) {
    if (has_non_turnback && c.cos_turn <= -0.45) continue;
    if (has_paved && c.shortcut) continue;
    uint64_t w = 1;
    if (c.cos_turn > 0.80) w = 12;       // straight continuation (< 37 deg)
    else if (c.cos_turn > 0.45) w = 4;   // gentle fork (37..63 deg)
    else if (c.cos_turn > -0.15) w = 2;  // standard intersection turn (63..99 deg)
    else w = 1;
    if (c.opt.road == cur_road) w += 6;
    else if (c.same_type) w += 3;
    if (has_paved && !c.paved) w = 1;
    weighted.push_back({c.opt, w});
    total_w += w;
  }
  if (weighted.empty() || total_w == 0) {
    out = cands.front().opt;
    return true;
  }
  uint64_t pick = mix64(uint64_t(car_id) * 0x9e3779b97f4a7c15ull ^ uint64_t(node) ^ 0x517cc1b727220a95ull) % total_w;
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
    // Incoming unit tangent approaching the node.
    const int prev_k = std::clamp(k - car.dir, 0, n - 1);
    double ix = r.points[size_t(k)].x - r.points[size_t(prev_k)].x;
    double iz = r.points[size_t(k)].z - r.points[size_t(prev_k)].z;
    const double il = std::hypot(ix, iz);
    std::array<double, 2> in_t = il > 1e-6 ? std::array<double, 2>{ix / il, iz / il}
                                           : std::array<double, 2>{std::sin(car.heading), std::cos(car.heading)};
    BranchOption chosen;
    if (!pick_roaming_branch(car.id, car.road, car.dir, node, in_t, chosen)) {
      if (r.oneway) {
        car.road = -1;
        return;
      }
      car.dir = -car.dir;
      continue;
    }
    car.road = chosen.road;
    car.dir = chosen.dir;
    car.s = roads[size_t(chosen.road)].s[size_t(chosen.idx)] + chosen.dir * 1e-6;
  }
}

bool FestivalManager::sample_roaming_ahead(const FestivalAiCar& car, double dist, double lateral,
                                           traffic::P3& pos_out, std::array<double, 2>& tangent_out) const {
  if (car.road < 0 || size_t(car.road) >= roads_.roads().size()) return false;
  FestivalAiCar probe;
  probe.id = car.id;
  probe.road = car.road;
  probe.dir = car.dir;
  probe.s = car.s;
  probe.heading = car.heading;
  if (dist > 0.0) advance_roaming(probe, dist);
  if (probe.road < 0 || size_t(probe.road) >= roads_.roads().size()) return false;
  std::array<double, 2> t{};
  const traffic::P3 c = roads_.point_at(probe.road, probe.s, nullptr, &t);
  tangent_out = {t[0] * probe.dir, t[1] * probe.dir};
  const auto rt = traffic::right_of(tangent_out[0], tangent_out[1]);
  pos_out = {c.x + rt[0] * lateral, c.y, c.z + rt[1] * lateral};
  return true;
}

void FestivalManager::sync_roaming_cursor(FestivalAiCar& car, double max_advance) const {
  if (car.road < 0 || size_t(car.road) >= roads_.roads().size()) return;
  const double span = std::max(8.0, max_advance);
  double best_ds = 0.0;
  double best_d2 = 1e30;
  traffic::P3 pt{};
  std::array<double, 2> tg{};
  // Coarse scan along deterministic path (seamlessly crossing junctions).
  constexpr double kCoarse = 0.75;
  for (double ds = 0.0; ds <= span; ds += kCoarse) {
    if (!sample_roaming_ahead(car, ds, car.lateral, pt, tg)) break;
    const double dx = car.position.x - pt.x, dz = car.position.z - pt.z;
    const double d2 = dx * dx + dz * dz;
    if (d2 < best_d2) {
      best_d2 = d2;
      best_ds = ds;
    }
  }
  // Fine local refinement around best_ds.
  const double lo = std::max(0.0, best_ds - kCoarse);
  const double hi = std::min(span, best_ds + kCoarse);
  for (double ds = lo; ds <= hi; ds += 0.15) {
    if (!sample_roaming_ahead(car, ds, car.lateral, pt, tg)) break;
    const double dx = car.position.x - pt.x, dz = car.position.z - pt.z;
    const double d2 = dx * dx + dz * dz;
    if (d2 < best_d2) {
      best_d2 = d2;
      best_ds = ds;
    }
  }
  if (best_ds > 1e-4) advance_roaming(car, best_ds);
}

void FestivalManager::steer_towards(FestivalAiCar& car, const traffic::P3& aim, double dt, double max_lat_g) const {
  constexpr double kWheelbase = 2.65;
  constexpr double kMaxSteer = 0.52;      // ~30 deg road wheel lock
  constexpr double kMaxSteerRate = 2.2;   // rad/s road wheel slew limit
  constexpr double kMaxYawRate = 1.65;    // rad/s hard ceiling
  const double fx = std::sin(car.heading), fz = std::cos(car.heading);
  const auto rt = traffic::right_of(fx, fz);
  const double dx = aim.x - car.position.x;
  const double dz = aim.z - car.position.z;
  const double lx = dx * rt[0] + dz * rt[1];
  const double lz = dx * fx + dz * fz;
  const double ld = std::max(3.5, std::hypot(lx, lz));
  const double alpha = std::atan2(lx, lz);
  double target_steer = std::atan(2.0 * kWheelbase * std::sin(alpha) / ld);
  // Speed-dependent lateral grip steering cap so high-speed lane changes never jerk.
  const double v_eff = std::max(car.speed, 2.0);
  const double grip_steer = std::clamp(std::atan(kWheelbase * max_lat_g / (v_eff * v_eff)), 0.06, kMaxSteer);
  target_steer = std::clamp(target_steer, -grip_steer, grip_steer);
  car.steer_angle += std::clamp(target_steer - car.steer_angle, -kMaxSteerRate * dt, kMaxSteerRate * dt);

  double target_yaw = (car.speed / kWheelbase) * std::tan(car.steer_angle);
  const double yaw_cap = std::min(kMaxYawRate, max_lat_g / std::max(car.speed, 4.0));
  target_yaw = std::clamp(target_yaw, -yaw_cap, yaw_cap);
  const double yaw_alpha = std::min(1.0, dt / 0.08);
  car.yaw_rate += (target_yaw - car.yaw_rate) * yaw_alpha;
  car.heading = wrap_angle(car.heading + car.yaw_rate * dt);

  const double nfx = std::sin(car.heading), nfz = std::cos(car.heading);
  car.position.x += (nfx * car.speed + car.knockback_vx) * dt;
  car.position.z += (nfz * car.speed + car.knockback_vz) * dt;

  // Smooth ground elevation & pitch tracking.
  double y_hint = aim.y;
  if (std::isfinite(car.position.y)) y_hint = 0.5 * (car.position.y + aim.y);
  double y = y_hint;
  if (ground_ && ground_(car.position.x, car.position.z, y_hint, y)) {
    car.position.y = y;
  } else {
    car.position.y += (aim.y - car.position.y) * std::min(1.0, dt * 8.0);
  }
  double target_pitch = 0.0;
  if (ground_) {
    double yf = car.position.y, yb = car.position.y;
    if (ground_(car.position.x + nfx * 1.4, car.position.z + nfz * 1.4, car.position.y, yf) &&
        ground_(car.position.x - nfx * 1.4, car.position.z - nfz * 1.4, car.position.y, yb)) {
      target_pitch = std::clamp(std::atan2(yf - yb, 2.8), -0.45, 0.45);
    }
  }
  car.pitch += (target_pitch - car.pitch) * std::min(1.0, dt / 0.12);
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
  constexpr double kMinFestivalSpawnGap = 42.0;
  for (int ri = 0; ri < int(roads.size()); ++ri) {
    const auto& r = roads[size_t(ri)];
    if (r.disabled || r.type == "none" || r.type == "shortcut" || r.points.size() < 2) continue;
    double w = 0.2;
    if (const auto* fe = config_.festival_density(r.density)) w = traffic::density_at(*fe, v);
    if (w <= 0.0) continue;
    for (double s = 0; s <= r.length; s += 25.0) {
      const traffic::P3 pt = roads_.point_at(ri, s);
      const double d = hdist(pt, player);
      if (d < rmin || d > rmax) continue;
      bool clear = true;
      for (const auto& other : cars_) {
        if (hdist(other.position, pt) < kMinFestivalSpawnGap) {
          clear = false;
          break;
        }
      }
      if (clear) {
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
  // Prefer a driver/car from the roster not currently active so duplicates don't clump.
  std::vector<size_t> unused_roster;
  for (size_t i = 0; i < roster_.size(); ++i) {
    bool used = false;
    for (const auto& other : cars_) {
      if (other.spec.driver_name == roster_[i].driver_name) {
        used = true;
        break;
      }
    }
    if (!used) unused_roster.push_back(i);
  }
  const size_t ridx =
      !unused_roster.empty() ? unused_roster[size_t(rng_() % unused_roster.size())] : size_t(rng_() % roster_.size());
  car.spec = roster_[ridx];
  car.road = chosen->road;
  car.s = chosen->s;
  car.dir = r.oneway ? 1 : (u01(rng_) < 0.5 ? 1 : -1);
  car.lateral = kLane;
  car.target_lateral = kLane;
  car.base_speed = std::clamp(22.0 + double(car.spec.pi) * 0.02 + u01(rng_) * 3.0 - 1.5, 20.0, 38.0);
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
    uint32_t id;
    uint32_t to;
    float len;
    float cost_s;
    float dx;
    float dz;
    bool wrong_way;
  };
  const size_t N = nav_.nodes.size();
  std::vector<std::vector<Edge>> adj(N);
  std::vector<uint8_t> on_road(N, 0);
  std::vector<uint8_t> ignore_turn(N, 0);
  for (uint32_t i = 0; i < N; ++i) {
    if (nav_.node_tag(i, "ignore_turn_flags") == "true") ignore_turn[i] = 1;
  }

  std::vector<uint32_t> edge_to;
  std::vector<std::array<float, 2>> edge_dir;
  auto push_edge = [&](uint32_t u, uint32_t v, float len, float cost_s, bool wrong_way) {
    const float dx = nav_.nodes[v].x - nav_.nodes[u].x;
    const float dz = nav_.nodes[v].z - nav_.nodes[u].z;
    const float inv = len > 1e-4f ? (1.0f / len) : 0.0f;
    const float ux = dx * inv, uz = dz * inv;
    const uint32_t eid = uint32_t(edge_to.size());
    edge_to.push_back(v);
    edge_dir.push_back({ux, uz});
    adj[u].push_back({eid, v, len, cost_s, ux, uz, wrong_way});
  };

  for (uint32_t w = 0; w < nav_.ways.size(); ++w) {
    const std::string_view rt = nav_.way_tag(w, "road_type");
    if (rt.empty() || rt == "none" || rt == "shortcut") continue;
    if (nav_.way_tag(w, "ai_disabled") == "true") continue;
    const bool oneway = (nav_.way_tag(w, "oneway") == "true" || nav_.way_tag(w, "oneway") == "1");
    const double spd = road_speed_mps(rt);
    const uint32_t nc = nav_.ways[w].node_count;
    for (uint32_t k = 0; k + 1 < nc; ++k) {
      const uint32_t a = nav_.way_node(w, k), b = nav_.way_node(w, k + 1);
      if (a >= N || b >= N || a == b) continue;
      const float l = std::hypot(nav_.nodes[b].x - nav_.nodes[a].x, nav_.nodes[b].z - nav_.nodes[a].z);
      if (l < 1e-4f) continue;
      const float base_s = float(std::max(1e-4, double(l) / spd));
      push_edge(a, b, l, base_s, false);
      if (!oneway) {
        push_edge(b, a, l, base_s, false);
      } else {
        // Retail OneWayMultiplier = 999.0: never treat one-way freeways as normal bidirectional edges.
        push_edge(b, a, l, float(double(base_s) * kH2hOneWayMultiplier), true);
      }
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

  const double fx = std::sin(heading), fz = std::cos(heading);
  const double optimal_len = std::clamp(kH2hOptimalLength, min_len, max_len);

  auto run_search = [&](bool allow_wrong_way, std::vector<uint32_t>& out_chain) -> bool {
    const uint32_t n_edges = uint32_t(edge_to.size());
    const uint32_t kSeedState = n_edges;
    const uint32_t n_states = n_edges + 1;
    const float kInf = std::numeric_limits<float>::infinity();

    std::vector<float> best_cost(n_states, kInf);
    std::vector<float> state_phys(n_states, kInf);
    std::vector<int> prev_state(n_states, -1);
    using Q = std::pair<float, uint32_t>;
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;

    best_cost[kSeedState] = 0.0f;
    state_phys[kSeedState] = 0.0f;
    q.push({0.0f, kSeedState});

    uint32_t best_dst_state = std::numeric_limits<uint32_t>::max();
    double best_score = -1e30;

    while (!q.empty()) {
      const auto [cost_u, st] = q.top();
      q.pop();
      if (cost_u > best_cost[st]) continue;
      const float phys_u = state_phys[st];
      if (phys_u > float(max_len)) continue;

      const uint32_t u = (st == kSeedState) ? src : edge_to[st];
      const double in_dx = (st == kSeedState) ? fx : double(edge_dir[st][0]);
      const double in_dz = (st == kSeedState) ? fz : double(edge_dir[st][1]);

      if (st != kSeedState && phys_u >= float(min_len)) {
        const double dx = nav_.nodes[u].x - start.x, dz = nav_.nodes[u].z - start.z;
        const double hd = std::max(1.0, std::hypot(dx, dz));
        const double forward_align = (dx * fx + dz * fz) / hd;
        const double dist_err = std::fabs(double(phys_u) - optimal_len);
        const double score = forward_align * 1200.0 - dist_err * 0.45 - double(cost_u) * 2.0;
        if (score > best_score) {
          best_score = score;
          best_dst_state = st;
        }
      }

      const bool ign_u = (u < N && ignore_turn[u] != 0);
      for (const auto& e : adj[u]) {
        if (e.wrong_way && !allow_wrong_way) continue;
        const float n_phys = phys_u + e.len;
        if (n_phys > float(max_len)) continue;
        const double dot = in_dx * double(e.dx) + in_dz * double(e.dz);
        const float turn_pen = (!ign_u && dot < kSharpTurnCos) ? float(kH2hTurnDelaySeconds) : 0.0f;
        const float n_cost = cost_u + e.cost_s + turn_pen;
        if (n_cost < best_cost[e.id]) {
          best_cost[e.id] = n_cost;
          state_phys[e.id] = n_phys;
          prev_state[e.id] = int(st);
          q.push({n_cost, e.id});
        }
      }
    }

    if (best_dst_state == std::numeric_limits<uint32_t>::max()) return false;
    out_chain.clear();
    for (int cur = int(best_dst_state); cur >= 0; cur = prev_state[size_t(cur)]) {
      const uint32_t u = (uint32_t(cur) == kSeedState) ? src : edge_to[size_t(cur)];
      out_chain.push_back(u);
      if (uint32_t(cur) == kSeedState) break;
    }
    std::reverse(out_chain.begin(), out_chain.end());
    return out_chain.size() >= 2;
  };

  std::vector<uint32_t> chain;
  // Legal one-way pass first; only fall back to heavily penalized wrong-way edges if isolated.
  if (!run_search(false, chain)) {
    if (!run_search(true, chain)) return false;
  }

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
  if (!build_h2h_route(player_pos, player_heading, kH2hMinLength, kH2hMaxLength)) return false;

  h2h_active_ = true;
  h2h_car_id_ = opp->id;
  h2h_wager_ = h2h_reward_credits(player_pi_, opp->spec.pi);
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
                           double player_speed, HeadToHeadResult* finished_out,
                           const std::vector<traffic::Car>* traffic_cars) {
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

  // Update all cars with cross-junction curvature lookahead braking, full
  // car-to-car separation & overtaking (Festival AI, Traffic, Player), smooth
  // Ackermann bicycle steering, knockback damping, and realistic velocity/RPM.
  const uint32_t prev_prompt_id = prompt_id_;
  prompt_id_ = 0;
  prompt_dist_ = 1e9;
  const double pfx = std::sin(player_heading), pfz = std::cos(player_heading);
  const auto prt = traffic::right_of(pfx, pfz);
  const double pvx = pfx * player_speed, pvz = pfz * player_speed;

  // Snapshot previous frame poses so inter-Festival avoidance is order-independent.
  struct OtherSnap {
    uint32_t id = 0;
    traffic::P3 pos{};
    double vx = 0.0, vz = 0.0;
    double speed = 0.0;
  };
  std::vector<OtherSnap> fest_snaps;
  fest_snaps.reserve(cars_.size());
  for (const auto& c : cars_) {
    const double cfx = std::sin(c.heading), cfz = std::cos(c.heading);
    fest_snaps.push_back({c.id, c.position, cfx * c.speed, cfz * c.speed, c.speed});
  }

  auto eval_avoidance = [&](FestivalAiCar& car, double& want_spd, double default_lat) {
    const double fx = std::sin(car.heading), fz = std::cos(car.heading);
    const auto rt = traffic::right_of(fx, fz);
    car.target_lateral = default_lat;
    bool blocked_right = false, blocked_left = false;
    bool need_pass = false;
    double nudge_lat = 0.0;

    auto check_obstacle = [&](const traffic::P3& op, double ovx, double ovz, double ospd, bool is_festival,
                              uint32_t other_id) {
      const double rx = op.x - car.position.x, rz = op.z - car.position.z;
      const double ahead = rx * fx + rz * fz;
      const double lat = rx * rt[0] + rz * rt[1];
      const double along = ovx * fx + ovz * fz;
      const bool oncoming = along < -1.5;

      // Oncoming vehicles in the left/overtaking lane block pulling out to pass.
      if (oncoming && ahead > -4.0 && ahead < 75.0) {
        if (lat < 0.8 && lat > -5.5) blocked_left = true;
        if (lat > -0.8 && lat < 5.5 && ahead < 45.0) {
          // Oncoming directly in our lane: pull right onto shoulder & brake.
          blocked_left = true;
          nudge_lat = std::max(nudge_lat, 1.2);
          want_spd = std::min(want_spd, std::max(4.0, (ahead - 10.0) * 0.45));
        }
        return;
      }

      const double search = std::max(46.0, car.speed * 2.2);
      if (ahead < -7.0 || ahead > search) return;

      // Side-by-side / overlapping proximity guard (prevents fusing/clipping).
      if (std::fabs(ahead) < 6.0 && std::fabs(lat) < 2.65) {
        if (lat >= 0.0) blocked_right = true;
        else blocked_left = true;
        // Trailing car (or higher id if tied) yields speed & nudges away laterally.
        const bool i_yield = (ahead > 0.25) || (std::fabs(ahead) <= 0.25 && is_festival && car.id > other_id);
        if (i_yield) {
          want_spd = std::min(want_spd, std::max(0.0, std::max(0.0, along) - 3.5));
          nudge_lat += (lat >= 0.0 ? -1.4 : 1.4);
        } else if (is_festival) {
          nudge_lat += (lat >= 0.0 ? -0.6 : 0.6);
        }
        return;
      }

      if (ahead <= 0.0) {
        // Adjacent car slightly behind us blocks changing into its lane.
        if (lat < -0.5 && lat > -5.2) blocked_left = true;
        else if (lat > 0.5 && lat < 5.2) blocked_right = true;
        return;
      }

      // Same-lane vehicle ahead.
      if (std::fabs(lat) < 2.55) {
        if (car.lateral >= 0.0) blocked_right = true;
        else blocked_left = true;
        if (along < want_spd - 1.5 || ahead < 22.0) need_pass = true;
        const double v_lead = std::max(0.0, along);
        const double safe_gap = std::max(10.0, car.speed * 0.95) + 6.0;
        if (ahead < safe_gap) {
          const double room = std::max(0.0, ahead - 9.0);
          double v_safe = std::sqrt(v_lead * v_lead + 2.0 * 7.0 * room);
          if (ahead < 11.0) v_safe = std::min(v_safe, std::max(0.0, v_lead - (11.0 - ahead) * 1.6));
          want_spd = std::min(want_spd, v_safe);
        } else if (ahead < safe_gap * 1.5 && v_lead < want_spd) {
          want_spd = std::min(want_spd, v_lead + 2.0);
        }
      } else if (lat < -0.5 && lat > -5.2 && ahead < 34.0) {
        blocked_left = true;
      } else if (lat > 0.5 && lat < 5.2 && ahead < 34.0) {
        blocked_right = true;
      }
      (void)ospd;
    };

    for (const auto& fs : fest_snaps) {
      if (fs.id == car.id) continue;
      check_obstacle(fs.pos, fs.vx, fs.vz, fs.speed, true, fs.id);
    }
    if (traffic_cars) {
      for (const auto& tc : *traffic_cars) {
        const double tfx = std::sin(tc.heading), tfz = std::cos(tc.heading);
        check_obstacle(tc.position, tfx * tc.speed, tfz * tc.speed, tc.speed, false, 0);
      }
    }
    if (!car.in_h2h) check_obstacle(player_pos, pvx, pvz, player_speed, false, 0);

    if (need_pass && blocked_right && !blocked_left) car.target_lateral = -1.8;
    else if (need_pass && blocked_left && !blocked_right) car.target_lateral = default_lat;
    if (std::fabs(nudge_lat) > 1e-3) {
      car.target_lateral = std::clamp(car.lateral + nudge_lat, -3.2, 3.6);
    }
    const double lat_rate = car.in_h2h ? 2.6 : 1.9;
    car.lateral += std::clamp(car.target_lateral - car.lateral, -lat_rate * dt, lat_rate * dt);
  };

  for (auto& car : cars_) {
    const traffic::P3 prev_pos = car.position;
    // Decay any collision knockback velocity via lateral/longitudinal tyre grip.
    const double kb_decay = std::exp(-dt * 4.5);
    car.knockback_vx *= kb_decay;
    car.knockback_vz *= kb_decay;
    if (std::hypot(car.knockback_vx, car.knockback_vz) < 0.05) car.knockback_vx = car.knockback_vz = 0.0;

    const double pi_d = std::clamp(double(car.spec.pi), 100.0, 999.0);
    const double mass_kg = std::clamp(car.spec.mass_kg > 100.0 ? car.spec.mass_kg : 1480.0, 700.0, 3200.0);
    const double mass_factor = std::clamp(1480.0 / mass_kg, 0.70, 1.35);
    const double max_lat_g = std::clamp(9.5 + pi_d * 0.0055, 9.5, 15.0);
    car.turn_left = false;
    car.turn_right = false;
    car.reversing = false;
    if (car.in_h2h && h2h_active_ && car.id == h2h_car_id_) {
      // Racing pace along the Head-to-Head nav route scaled by opponent PI & car mass
      // (no 58 m/s hypercar cap) with curvature lookahead braking.
      const double base_race_spd = h2h_top_speed_mps(car.spec.pi);
      const double gap = car.h2h_progress - h2h_player_s_;
      double want_spd = base_race_spd;
      if (gap > 80.0) want_spd *= 0.88;
      else if (gap < -80.0) want_spd *= 1.10;

      // Sample curvature over [10 m, 110 m] ahead along H2H route to brake cleanly into corners.
      double h_prev = car.heading, p_tmp = 0.0;
      double d_prev = 0.0;
      traffic::P3 pos_tmp{};
      for (double look_d : {10.0, 22.0, 38.0, 58.0, 82.0, 110.0}) {
        const double s1 = std::min(h2h_length_, car.h2h_progress + look_d);
        double h1 = h_prev;
        pose_on_h2h(s1, car.lateral, pos_tmp, h1, p_tmp);
        const double seg_d = std::max(1.0, look_d - d_prev);
        const double dth_seg = std::fabs(wrap_angle(h1 - h_prev));
        const double dth_tot = std::fabs(wrap_angle(h1 - car.heading));
        const double kappa = std::max(dth_seg / seg_d, dth_tot / std::max(look_d, 1.0));
        if (kappa > 0.004 || dth_tot > 0.45) {
          double v_corner = std::sqrt((max_lat_g * 0.85) / std::max(kappa, 1e-4));
          if (dth_tot > 0.75) v_corner = std::min(v_corner, std::clamp(20.0 - 6.0 * dth_tot, 9.0, 18.0));
          const double v_brake = std::sqrt(v_corner * v_corner + 2.0 * 9.5 * std::max(0.0, look_d - 7.0));
          want_spd = std::min(want_spd, std::max(9.0, v_brake));
        }
        h_prev = h1;
        d_prev = look_d;
      }
      eval_avoidance(car, want_spd, 1.5);
      car.braking = want_spd < car.speed - 1.2;
      const double pi_accel_base = std::clamp(3.8 + (pi_d - 100.0) * 0.0115, 3.8, 14.5);
      const double speed_ratio = std::clamp(car.speed / std::max(base_race_spd, 1.0), 0.0, 1.0);
      const double max_accel = std::max(1.8, pi_accel_base * mass_factor * (1.0 - 0.55 * speed_ratio * speed_ratio));
      const double max_decel = std::clamp((10.5 + (pi_d - 100.0) * 0.006) * std::sqrt(mass_factor), 9.5, 17.5);
      car.speed += std::clamp(want_spd - car.speed, -max_decel * dt, max_accel * dt);
      car.speed = std::max(0.0, car.speed);

      if (!car.pose_initialized) {
        pose_on_h2h(car.h2h_progress, car.lateral, car.position, car.heading, car.pitch);
        car.pose_initialized = true;
      }
      const double look_d = std::clamp(car.speed * 0.55, 8.0, 38.0);
      traffic::P3 aim{};
      double aim_h = car.heading, aim_p = 0.0;
      pose_on_h2h(std::min(h2h_length_, car.h2h_progress + look_d), car.lateral, aim, aim_h, aim_p);
      steer_towards(car, aim, dt, max_lat_g);
      const double proj_s = project_on_h2h(car.position.x, car.position.z, car.h2h_progress);
      car.h2h_progress = std::max(car.h2h_progress, proj_s);
    } else if (car.road >= 0) {
      if (!car.pose_initialized) pose_roaming(car);
      double want_spd = car.base_speed;
      // Cross-junction curvature & intersection turn lookahead so Festival AI
      // smoothly brakes BEFORE bends and 90-degree junctions instead of snapping.
      traffic::P3 probe_p{};
      std::array<double, 2> probe_t{};
      double h0 = car.heading;
      if (sample_roaming_ahead(car, 0.0, car.lateral, probe_p, probe_t)) {
        h0 = std::atan2(probe_t[0], probe_t[1]);
      }
      double h_prev = h0;
      double d_prev = 0.0;
      double signed_turn_ahead = 0.0;
      for (double look_d : {8.0, 16.0, 26.0, 40.0, 58.0, 80.0}) {
        if (!sample_roaming_ahead(car, look_d, car.lateral, probe_p, probe_t)) break;
        const double h1 = std::atan2(probe_t[0], probe_t[1]);
        const double seg_d = std::max(1.0, look_d - d_prev);
        const double dth_signed = wrap_angle(h1 - h0);
        const double dth_seg = std::fabs(wrap_angle(h1 - h_prev));
        const double dth_tot = std::fabs(dth_signed);
        if (look_d <= 45.0 && std::fabs(dth_signed) > std::fabs(signed_turn_ahead)) {
          signed_turn_ahead = dth_signed;
        }
        const double kappa = std::max(dth_seg / seg_d, dth_tot / std::max(look_d, 1.0));
        if (kappa > 0.004 || dth_tot > 0.35) {
          double v_corner = std::sqrt((max_lat_g * 0.70) / std::max(kappa, 1e-4));
          if (dth_tot > 0.65) {
            // Sharp junction turn: slow realistically to ~7-11 m/s before turning in.
            v_corner = std::min(v_corner, std::clamp(17.5 - 6.2 * dth_tot, 6.5, 14.0));
          }
          const double v_brake = std::sqrt(v_corner * v_corner + 2.0 * 7.5 * std::max(0.0, d_prev - 4.0));
          want_spd = std::min(want_spd, std::max(6.5, v_brake));
        }
        h_prev = h1;
        d_prev = look_d;
      }
      eval_avoidance(car, want_spd, kLane);
      // Drive realistic turn indicators on lane changes or upcoming junction turns.
      const double lat_delta = car.target_lateral - car.lateral;
      if (lat_delta < -0.35 || signed_turn_ahead < -0.45) car.turn_left = true;
      else if (lat_delta > 0.35 || signed_turn_ahead > 0.45) car.turn_right = true;
      car.braking = want_spd < car.speed - 1.0;
      const double cruise_accel = std::clamp(5.0 + (pi_d - 300.0) * 0.005, 4.5, 8.5) * mass_factor;
      car.speed += std::clamp(want_spd - car.speed, -10.5 * dt, cruise_accel * dt);
      car.speed = std::max(0.0, car.speed);

      const double look_d = std::clamp(car.speed * 0.65, 7.5, 28.0);
      traffic::P3 aim{};
      std::array<double, 2> aim_t{};
      if (sample_roaming_ahead(car, look_d, car.lateral, aim, aim_t)) {
        steer_towards(car, aim, dt, max_lat_g);
        sync_roaming_cursor(car, car.speed * dt * 3.0 + 12.0);
      } else {
        car.road = -1;
      }
      if (!h2h_active_ && car.road >= 0) {
        // Retail Activity/RaceEncounter trigger box gates with gentle hysteresis once locked:
        // MinActivationDistance=5 m, MaxActivationDistance=20 m, ActivationBoxWidth=5 m (|lat|<=2.5 m),
        // same heading (dot > 0.85), MaxSpeedDifference=6.71 m/s (15 mph), MinTargetCarSpeed=11.18 m/s (25 mph).
        const bool locked = (car.id == prev_prompt_id);
        const double min_dist = locked ? 3.5 : kMinActivationDistance;
        const double max_dist = locked ? 25.0 : kMaxActivationDistance;
        const double half_w = locked ? 3.2 : kActivationBoxHalfWidth;
        const double min_dot = locked ? 0.80 : kMinHeadingDot;
        const double max_dv = locked ? 8.0 : kMaxSpeedDifference;
        const double min_tgt_spd = locked ? 9.5 : kMinTargetCarSpeed;

        const double dx = car.position.x - player_pos.x, dz = car.position.z - player_pos.z;
        const double d = std::hypot(dx, dz);
        const double ahead = dx * pfx + dz * pfz;
        const double lat = dx * prt[0] + dz * prt[1];
        const double cfx = std::sin(car.heading), cfz = std::cos(car.heading);
        const double heading_dot = pfx * cfx + pfz * cfz;
        const double speed_diff = std::fabs(car.speed - std::fabs(player_speed));
        if (ahead >= min_dist && ahead <= max_dist && std::fabs(lat) <= half_w &&
            heading_dot > min_dot && car.speed >= min_tgt_spd && speed_diff <= max_dv && d < prompt_dist_) {
          prompt_dist_ = d;
          prompt_id_ = car.id;
        }
      }
    }
    car.vx = (car.position.x - prev_pos.x) / dt;
    car.vy = (car.position.y - prev_pos.y) / dt;
    car.vz = (car.position.z - prev_pos.z) / dt;
    const double v_along = car.vx * std::sin(car.heading) + car.vz * std::cos(car.heading);
    car.reversing = (v_along < -0.5);
    car.rpm = estimate_rpm_for_speed(car.speed, car.spec.pi);
  }

  // Final inter-Festival anti-overlap safety pass so converging cars at a
  // junction can never visually fuse or drive inside each other.
  constexpr double kMinBodyW = 2.15;
  constexpr double kMinBodyL = 4.75;
  for (size_t i = 0; i < cars_.size(); ++i) {
    for (size_t j = i + 1; j < cars_.size(); ++j) {
      auto& a = cars_[i];
      auto& b = cars_[j];
      const double dx = b.position.x - a.position.x;
      const double dz = b.position.z - a.position.z;
      const double dist = std::hypot(dx, dz);
      if (dist > 5.2 || dist < 1e-5) continue;
      const double fx = std::sin(a.heading), fz = std::cos(a.heading);
      const auto rt = traffic::right_of(fx, fz);
      const double loc_f = dx * fx + dz * fz;
      const double loc_r = dx * rt[0] + dz * rt[1];
      const double pen_f = kMinBodyL - std::fabs(loc_f);
      const double pen_r = kMinBodyW - std::fabs(loc_r);
      if (pen_f <= 0.0 || pen_r <= 0.0) continue;
      double nx = 0.0, nz = 0.0, pen = 0.0;
      if (pen_r < pen_f) {
        const double sg = loc_r >= 0.0 ? 1.0 : -1.0;
        nx = rt[0] * sg;
        nz = rt[1] * sg;
        pen = pen_r;
      } else {
        const double sg = loc_f >= 0.0 ? 1.0 : -1.0;
        nx = fx * sg;
        nz = fz * sg;
        pen = pen_f;
      }
      const double push = std::min(pen * 0.5, 0.35);
      a.position.x -= nx * push;
      a.position.z -= nz * push;
      b.position.x += nx * push;
      b.position.z += nz * push;
      // Gently slow the trailing car to rebuild clean spacing.
      if (loc_f > 0.0) a.speed = std::min(a.speed, std::max(0.0, b.speed - 2.0));
      else b.speed = std::min(b.speed, std::max(0.0, a.speed - 2.0));
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
    const double d_fin_opp =
        opp ? std::hypot(opp->position.x - h2h_finish_[0], opp->position.z - h2h_finish_[2]) : 1e9;
    const bool player_done = d_fin_player <= kH2hFinishTriggerRadius;
    const bool opp_done =
        opp && (d_fin_opp <= kH2hFinishTriggerRadius || opp->h2h_progress >= h2h_length_ - kH2hFinishTriggerRadius);
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
        h.prompt_wager = h2h_reward_credits(player_pi_, c.spec.pi);
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

FestivalCollisionImpulse FestivalManager::resolve_player_collision(const traffic::P3& player_pos,
                                                                   const std::array<double, 3>& player_vel,
                                                                   double player_mass_kg, double dt) {
  FestivalCollisionImpulse out{};
  if (dt <= 0.0 || cars_.empty()) return out;
  const double m_player = std::max(500.0, player_mass_kg);
  constexpr double kHalfW = 0.98;
  constexpr double kHalfL = 2.25;
  constexpr double kRestitution = 0.35;

  for (auto& car : cars_) {
    const double ai_mass = std::clamp(car.spec.mass_kg > 100.0 ? car.spec.mass_kg : 1480.0, 700.0, 3200.0);
    const double dy = std::fabs(player_pos.y - car.position.y);
    if (dy > 2.2) continue;
    const double dx = player_pos.x - car.position.x;
    const double dz = player_pos.z - car.position.z;
    const double dist = std::hypot(dx, dz);
    if (dist > 4.8 || dist < 1e-4) continue;
    // Project separation into the AI car's local XZ frame for tight OBB overlap.
    const double fx = std::sin(car.heading), fz = std::cos(car.heading);
    const double rx = fz, rz = -fx;
    const double loc_f = dx * fx + dz * fz;
    const double loc_r = dx * rx + dz * rz;
    const double pen_f = (2.0 * kHalfL) - std::fabs(loc_f);
    const double pen_r = (2.0 * kHalfW) - std::fabs(loc_r);
    if (pen_f <= 0.0 || pen_r <= 0.0) continue;

    // Pick minimum-penetration separation normal pointing from AI -> player.
    double nx = 0.0, nz = 0.0, pen = 0.0;
    if (pen_r < pen_f) {
      const double sg = loc_r >= 0.0 ? 1.0 : -1.0;
      nx = rx * sg;
      nz = rz * sg;
      pen = pen_r;
    } else {
      const double sg = loc_f >= 0.0 ? 1.0 : -1.0;
      nx = fx * sg;
      nz = fz * sg;
      pen = pen_f;
    }
    const double rvx = player_vel[0] - car.vx;
    const double rvz = player_vel[2] - car.vz;
    const double vn = rvx * nx + rvz * nz;
    const double inv_m = 1.0 / m_player + 1.0 / ai_mass;
    // Push AI laterally/longitudinally out of overlap immediately.
    const double push_ai = pen * 0.55;
    car.position.x -= nx * push_ai;
    car.position.z -= nz * push_ai;
    car.lateral = std::clamp(car.lateral - (nx * rx + nz * rz) * push_ai, -4.5, 4.5);

    double j_mag = 0.0;
    if (vn < 0.0) {
      j_mag = -(1.0 + kRestitution) * vn / inv_m;
    } else {
      // Pure positional separation impulse if already separating slowly.
      j_mag = std::min(pen * 2.5, 4.0) / inv_m;
    }
    const double ai_dv_x = -(j_mag / ai_mass) * nx;
    const double ai_dv_z = -(j_mag / ai_mass) * nz;
    car.knockback_vx += ai_dv_x;
    car.knockback_vz += ai_dv_z;
    const double kb_mag = std::hypot(car.knockback_vx, car.knockback_vz);
    if (kb_mag > 25.0) {
      car.knockback_vx *= 25.0 / kb_mag;
      car.knockback_vz *= 25.0 / kb_mag;
    }
    // Impart off-center yaw kick on AI car proportional to lever arm and lateral impulse.
    const double ai_lat_imp = -(j_mag * (nx * rx + nz * rz));
    const double ai_inertia = std::max(1200.0, ai_mass * 1.9);
    car.yaw_rate = std::clamp(car.yaw_rate + (loc_f * ai_lat_imp * 0.55) / ai_inertia, -2.5, 2.5);
    car.speed = std::max(0.0, car.speed + ai_dv_x * fx + ai_dv_z * fz);
    if (j_mag > out.impulse_magnitude) {
      out.hit = true;
      out.car_id = car.id;
      out.contact_point = {0.5 * (player_pos.x + car.position.x), 0.5 * (player_pos.y + car.position.y) + 0.5,
                           0.5 * (player_pos.z + car.position.z)};
      out.player_impulse = {j_mag * nx, 0.0, j_mag * nz};
      out.impulse_magnitude = j_mag;
    }
  }
  return out;
}

}  // namespace freeroam
