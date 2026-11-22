#include "traffic/gps.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace traffic {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = 180.0 / kPi;

bool is_drivable_type(const std::string& t) {
  return t == "dirt" || t == "b" || t == "a" || t == "freeway";
}

// Road preference multiplier (>= 1.0 so Euclidean heuristic stays admissible).
double road_weight(const std::string& t) {
  if (t == "freeway") return 1.00;
  if (t == "a") return 1.02;
  if (t == "b") return 1.06;
  if (t == "dirt") return 1.25;
  return 1.50;
}

double wrap_angle(double a) {
  while (a > kPi) a -= 2.0 * kPi;
  while (a < -kPi) a += 2.0 * kPi;
  return a;
}

// Signed turn angle from unit dir (ax, az) to (bx, bz): + right, - left (game frame +X east, +Z north).
double signed_turn(double ax, double az, double bx, double bz) {
  const double dot = std::clamp(ax * bx + az * bz, -1.0, 1.0);
  // In +X east, +Z north: turning from +Z (0,1) to +X (1,0) is RIGHT (+pi/2).
  // a_z * b_x - a_x * b_z = 1*1 - 0 = +1 (right).
  const double cross = az * bx - ax * bz;
  return std::atan2(cross, dot);
}

GpsTurnKind classify_turn(double signed_rad) {
  const double deg = std::fabs(signed_rad) * kDeg;
  if (deg < 24.0) return GpsTurnKind::kStraight;
  const bool right = signed_rad > 0.0;
  if (deg < 52.0) return right ? GpsTurnKind::kBearRight : GpsTurnKind::kBearLeft;
  if (deg < 120.0) return right ? GpsTurnKind::kRight : GpsTurnKind::kLeft;
  return right ? GpsTurnKind::kSharpRight : GpsTurnKind::kSharpLeft;
}

}  // namespace

const char* gps_turn_label(GpsTurnKind k) {
  switch (k) {
    case GpsTurnKind::kStraight: return "CONTINUE STRAIGHT";
    case GpsTurnKind::kBearLeft: return "BEAR LEFT";
    case GpsTurnKind::kLeft: return "TURN LEFT";
    case GpsTurnKind::kSharpLeft: return "SHARP LEFT";
    case GpsTurnKind::kBearRight: return "BEAR RIGHT";
    case GpsTurnKind::kRight: return "TURN RIGHT";
    case GpsTurnKind::kSharpRight: return "SHARP RIGHT";
    case GpsTurnKind::kUTurn: return "TURN AROUND";
    case GpsTurnKind::kArrive: return "DESTINATION AHEAD";
    case GpsTurnKind::kNone:
    default: return "";
  }
}

void GpsSolver::build(const NavMap& nav) {
  roads_.build(nav);
  const size_t n_nodes = nav.nodes.size();
  const auto& rds = roads_.roads();

  drivable_road_.assign(rds.size(), 0);
  ignore_turn_node_.assign(n_nodes, 0);
  node_xz_.resize(n_nodes);
  adj_.assign(n_nodes, {});

  for (size_t i = 0; i < n_nodes; ++i) {
    node_xz_[i] = {nav.nodes[i].x, nav.nodes[i].z};
    if (nav.node_tag(uint32_t(i), "ignore_turn_flags") == "true") ignore_turn_node_[i] = 1;
  }

  for (size_t ri = 0; ri < rds.size(); ++ri) {
    const Road& r = rds[ri];
    if (!is_drivable_type(r.type) || r.nodes.size() < 2) continue;
    drivable_road_[ri] = 1;
    const double w = road_weight(r.type);
    for (size_t k = 0; k + 1 < r.nodes.size(); ++k) {
      const uint32_t u = r.nodes[k];
      const uint32_t v = r.nodes[k + 1];
      const double len = r.s[k + 1] - r.s[k];
      const double cost = std::max(0.01, len * w);
      adj_[u].push_back({v, int(ri), int(k), true, len, cost, false});
      if (!r.oneway) {
        adj_[v].push_back({u, int(ri), int(k), false, len, cost, false});
      } else {
        // Store wrong-way edge marked wrong_way=true so fallback can recover from
        // isolated one-way stubs without altering normal directed routing.
        adj_[v].push_back({u, int(ri), int(k), false, len, std::max(0.1, len * 8.0), true});
      }
    }
  }
}

RoadHit GpsSolver::nearest_road(double x, double z, double max_distance) const {
  RoadHit best;
  const auto& rds = roads_.roads();
  for (size_t ri = 0; ri < rds.size(); ++ri) {
    if (!drivable_road_[ri]) continue;
    const Road& r = rds[ri];
    for (size_t k = 0; k + 1 < r.points.size(); ++k) {
      const P3& a = r.points[k];
      const P3& b = r.points[k + 1];
      const double dx = b.x - a.x, dz = b.z - a.z, len2 = dx * dx + dz * dz;
      double t = len2 > 0.0 ? ((x - a.x) * dx + (z - a.z) * dz) / len2 : 0.0;
      t = std::clamp(t, 0.0, 1.0);
      const double px = a.x + dx * t, pz = a.z + dz * t;
      const double d = std::hypot(x - px, z - pz);
      if (d < best.distance && d <= max_distance) {
        best.road = int(ri);
        best.segment = int(k);
        best.s = r.s[k] + t * (r.s[k + 1] - r.s[k]);
        best.distance = d;
        const double len = std::sqrt(len2);
        const auto right = right_of(len > 0 ? dx / len : 0, len > 0 ? dz / len : 1);
        best.lateral = (x - px) * right[0] + (z - pz) * right[1];
      }
    }
  }
  return best;
}

GpsRoute GpsSolver::run_astar(const RoadHit& sh, const RoadHit& gh, double goal_x, double goal_z,
                              bool allow_wrong_way) const {
  GpsRoute out;
  if (sh.road < 0 || gh.road < 0) return out;
  const auto& rds = roads_.roads();
  const Road& sr = rds[size_t(sh.road)];
  const Road& gr = rds[size_t(gh.road)];
  const P3 sp = roads_.point_at(sh.road, sh.s);
  const P3 gp = roads_.point_at(gh.road, gh.s);

  // Direct same-road candidate (when on the same way and direction permits).
  GpsRoute direct;
  double direct_cost = std::numeric_limits<double>::infinity();
  if (sh.road == gh.road) {
    const bool forward = gh.s >= sh.s;
    if (forward || !sr.oneway || allow_wrong_way) {
      const double w = (!forward && sr.oneway) ? 8.0 : road_weight(sr.type);
      direct_cost = std::fabs(gh.s - sh.s) * w;
      std::vector<int> pnodes;
      direct.points.push_back(sp);
      pnodes.push_back(-1);
      if (forward) {
        for (int k = sh.segment + 1; k <= gh.segment; ++k) {
          direct.points.push_back(sr.points[size_t(k)]);
          pnodes.push_back(int(sr.nodes[size_t(k)]));
        }
      } else {
        for (int k = sh.segment; k >= gh.segment + 1; --k) {
          direct.points.push_back(sr.points[size_t(k)]);
          pnodes.push_back(int(sr.nodes[size_t(k)]));
        }
      }
      direct.points.push_back(gp);
      pnodes.push_back(-1);
      direct.s.resize(direct.points.size(), 0.0);
      for (size_t i = 1; i < direct.points.size(); ++i) {
        direct.s[i] = direct.s[i - 1] + std::hypot(direct.points[i].x - direct.points[i - 1].x,
                                                   direct.points[i].z - direct.points[i - 1].z);
      }
      direct.total_length = direct.s.back();
      direct.dest_x = goal_x;
      direct.dest_z = goal_z;
      direct.snapped_dest_x = gp.x;
      direct.snapped_dest_z = gp.z;
      direct.valid = true;
      extract_turns(direct, pnodes);
      // Fast-return if very short on the same road and going legally.
      if (direct.total_length < 25.0 && (forward || !sr.oneway)) return direct;
    }
  }

  const uint32_t n_nodes = uint32_t(adj_.size());
  const uint32_t kGoalVirtual = n_nodes;
  const double kInf = std::numeric_limits<double>::infinity();

  std::vector<double> g_score(n_nodes + 1, kInf);
  std::vector<int> parent(n_nodes + 1, -1);
  std::vector<uint8_t> closed(n_nodes + 1, 0);

  struct OpenItem {
    double f;
    uint32_t u;
    bool operator>(const OpenItem& o) const { return f > o.f; }
  };
  std::priority_queue<OpenItem, std::vector<OpenItem>, std::greater<OpenItem>> open;

  auto h = [&](uint32_t u) -> double {
    if (u == kGoalVirtual) return 0.0;
    return std::hypot(node_xz_[u][0] - gp.x, node_xz_[u][1] - gp.z);
  };

  // Seed start segment endpoints.
  const double sw = road_weight(sr.type);
  const uint32_t s_a = sr.nodes[size_t(sh.segment)];
  const uint32_t s_b = sr.nodes[size_t(sh.segment + 1)];
  const double d_sb = std::max(0.0, sr.s[size_t(sh.segment + 1)] - sh.s);
  const double d_sa = std::max(0.0, sh.s - sr.s[size_t(sh.segment)]);

  // Forward along start segment -> s_b
  {
    const double c = d_sb * sw;
    g_score[s_b] = c;
    parent[s_b] = -2;  // seeded from start
    open.push({c + h(s_b), s_b});
  }
  // Backward along start segment -> s_a (if two-way or fallback)
  if (!sr.oneway || allow_wrong_way) {
    const double c = d_sa * (sr.oneway ? 8.0 : sw);
    if (c < g_score[s_a]) {
      g_score[s_a] = c;
      parent[s_a] = -2;
      open.push({c + h(s_a), s_a});
    }
  }

  // Goal segment entry endpoints: reaching g_a can finish forward to gp;
  // reaching g_b can finish backward to gp if !gr.oneway (or fallback).
  const double gw = road_weight(gr.type);
  const uint32_t g_a = gr.nodes[size_t(gh.segment)];
  const uint32_t g_b = gr.nodes[size_t(gh.segment + 1)];
  const double d_ga = std::max(0.0, gh.s - gr.s[size_t(gh.segment)]);
  const double d_gb = std::max(0.0, gr.s[size_t(gh.segment + 1)] - gh.s);

  while (!open.empty()) {
    const auto cur = open.top();
    open.pop();
    const uint32_t u = cur.u;
    if (closed[u]) continue;
    closed[u] = 1;
    if (u == kGoalVirtual) break;
    const double gu = g_score[u];
    if (gu >= direct_cost && direct.valid) break;

    // Virtual edges into goal.
    if (u == g_a) {
      const double c = gu + d_ga * gw;
      if (c < g_score[kGoalVirtual]) {
        g_score[kGoalVirtual] = c;
        parent[kGoalVirtual] = int(u);
        open.push({c, kGoalVirtual});
      }
    }
    if (u == g_b && (!gr.oneway || allow_wrong_way)) {
      const double c = gu + d_gb * (gr.oneway ? 8.0 : gw);
      if (c < g_score[kGoalVirtual]) {
        g_score[kGoalVirtual] = c;
        parent[kGoalVirtual] = int(u);
        open.push({c, kGoalVirtual});
      }
    }

    for (const Edge& e : adj_[u]) {
      if (e.wrong_way && !allow_wrong_way) continue;
      if (closed[e.to]) continue;
      const double ng = gu + e.cost;
      if (ng < g_score[e.to]) {
        g_score[e.to] = ng;
        parent[e.to] = int(u);
        open.push({ng + h(e.to), e.to});
      }
    }
  }

  if (direct.valid && direct_cost <= g_score[kGoalVirtual]) return direct;
  if (!std::isfinite(g_score[kGoalVirtual]) || parent[kGoalVirtual] < 0) return direct;

  // Reconstruct node sequence.
  std::vector<uint32_t> rev_nodes;
  for (int cur = parent[kGoalVirtual]; cur >= 0; cur = parent[size_t(cur)]) {
    rev_nodes.push_back(uint32_t(cur));
  }
  std::reverse(rev_nodes.begin(), rev_nodes.end());

  std::vector<int> pnodes;
  auto push_pt = [&](const P3& pt, int nid) {
    if (!out.points.empty()) {
      const double d = std::hypot(pt.x - out.points.back().x, pt.z - out.points.back().z);
      if (d < 1e-3) return;
    }
    out.points.push_back(pt);
    pnodes.push_back(nid);
  };

  push_pt(sp, -1);
  for (uint32_t nid : rev_nodes) {
    // Look up full 3D point from any incident road.
    const auto& inc = roads_.at_node(nid);
    if (!inc.empty()) {
      const auto [ri, idx] = inc[0];
      push_pt(rds[size_t(ri)].points[size_t(idx)], int(nid));
    } else {
      push_pt({node_xz_[nid][0], sp.y, node_xz_[nid][1]}, int(nid));
    }
  }
  push_pt(gp, -1);

  out.s.assign(out.points.size(), 0.0);
  for (size_t i = 1; i < out.points.size(); ++i) {
    out.s[i] = out.s[i - 1] +
               std::hypot(out.points[i].x - out.points[i - 1].x, out.points[i].z - out.points[i - 1].z);
  }
  out.total_length = out.s.empty() ? 0.0 : out.s.back();
  out.dest_x = goal_x;
  out.dest_z = goal_z;
  out.snapped_dest_x = gp.x;
  out.snapped_dest_z = gp.z;
  out.valid = out.points.size() >= 2;
  extract_turns(out, pnodes);
  return out;
}

void GpsSolver::extract_turns(GpsRoute& route, const std::vector<int>& point_nodes) const {
  route.turns.clear();
  if (route.points.size() < 3) return;

  // Compute smoothed incoming/outgoing horizontal tangent ~18 m before/after vertex i.
  auto dir_before = [&](size_t i) -> std::array<double, 2> {
    const double target_s = std::max(0.0, route.s[i] - 18.0);
    size_t j = i;
    while (j > 0 && route.s[j] > target_s) --j;
    const double dx = route.points[i].x - route.points[j].x;
    const double dz = route.points[i].z - route.points[j].z;
    const double len = std::hypot(dx, dz);
    return len > 1e-4 ? std::array<double, 2>{dx / len, dz / len} : std::array<double, 2>{0.0, 1.0};
  };
  auto dir_after = [&](size_t i) -> std::array<double, 2> {
    const double target_s = std::min(route.total_length, route.s[i] + 18.0);
    size_t j = i;
    while (j + 1 < route.points.size() && route.s[j] < target_s) ++j;
    const double dx = route.points[j].x - route.points[i].x;
    const double dz = route.points[j].z - route.points[i].z;
    const double len = std::hypot(dx, dz);
    return len > 1e-4 ? std::array<double, 2>{dx / len, dz / len} : std::array<double, 2>{0.0, 1.0};
  };

  for (size_t i = 1; i + 1 < route.points.size(); ++i) {
    const int nid = i < point_nodes.size() ? point_nodes[i] : -1;
    if (nid >= 0 && size_t(nid) < ignore_turn_node_.size() && ignore_turn_node_[size_t(nid)]) continue;

    const bool is_junction = nid >= 0 && size_t(nid) < roads_.node_count() && roads_.degree(uint32_t(nid)) >= 3;
    const auto d0 = dir_before(i);
    const auto d1 = dir_after(i);
    const double ang = signed_turn(d0[0], d0[1], d1[0], d1[1]);
    const double deg = std::fabs(ang) * kDeg;

    // Emit a turn at junctions when deflection >= 24 deg, or at sharp non-junction corners >= 58 deg.
    if ((!is_junction && deg < 58.0) || deg < 24.0) continue;
    const GpsTurnKind kind = classify_turn(ang);
    if (kind == GpsTurnKind::kStraight || kind == GpsTurnKind::kNone) continue;

    // Merge consecutive vertices belonging to the same junction within 30 m.
    if (!route.turns.empty() && route.s[i] - route.turns.back().route_s < 30.0) {
      if (std::fabs(ang) > std::fabs(route.turns.back().angle_rad)) {
        route.turns.back() = {route.s[i], kind, ang, route.points[i].x, route.points[i].z};
      }
      continue;
    }
    route.turns.push_back({route.s[i], kind, ang, route.points[i].x, route.points[i].z});
  }
}

GpsRoute GpsSolver::solve(double start_x, double start_z, double goal_x, double goal_z) const {
  if (!ready()) return {};
  const RoadHit sh = nearest_road(start_x, start_z, 2500.0);
  const RoadHit gh = nearest_road(goal_x, goal_z, 2500.0);
  if (sh.road < 0 || gh.road < 0) return {};

  GpsRoute r = run_astar(sh, gh, goal_x, goal_z, false);
  if (!r.valid) r = run_astar(sh, gh, goal_x, goal_z, true);
  return r;
}

// ---------------------------------------------------------------------------
// GpsNavigator
// ---------------------------------------------------------------------------

void GpsNavigator::reset_prompt_milestones() {
  last_seg_hint_ = 0;
  announced_far_turn_ = -1;
  announced_near_turn_ = -1;
  announced_now_turn_ = -1;
  announced_near_dest_ = false;
  wrong_dir_time_ = 0.0;
}

GpsPromptEvent GpsNavigator::set_destination(double dest_x, double dest_z, GpsDestKind kind, const std::string& label,
                                             double car_x, double car_z) {
  dest_x_ = dest_x;
  dest_z_ = dest_z;
  guidance_ = GpsGuidance{};
  guidance_.dest_kind = kind;
  guidance_.dest_label = label;
  reset_prompt_milestones();
  reroute_cooldown_ = 1.0;
  uturn_cooldown_ = 2.0;

  if (!solver_ || !solver_->ready()) return {};
  route_ = solver_->solve(car_x, car_z, dest_x, dest_z);
  if (!route_.valid) return {};

  guidance_.active = true;
  guidance_.dist_to_dest_m = route_.total_length;
  guidance_.next_turn = route_.turns.empty() ? GpsTurnKind::kArrive : route_.turns[0].kind;
  guidance_.dist_to_turn_m = route_.turns.empty() ? route_.total_length : route_.turns[0].route_s;
  return {GpsPromptEvent::Kind::kDestinationSet, GpsTurnKind::kNone, kind};
}

void GpsNavigator::clear() {
  route_ = GpsRoute{};
  guidance_ = GpsGuidance{};
  reset_prompt_milestones();
}

GpsPromptEvent GpsNavigator::update(double car_x, double car_z, double car_heading_rad, double speed_mps, double dt) {
  if (!guidance_.active || !route_.valid || route_.points.size() < 2) return {};
  dt = std::max(0.0, dt);
  reroute_cooldown_ = std::max(0.0, reroute_cooldown_ - dt);
  uturn_cooldown_ = std::max(0.0, uturn_cooldown_ - dt);

  // Arrival check against both exact target and snapped road destination.
  const double d_exact = std::hypot(car_x - dest_x_, car_z - dest_z_);
  const double d_snap = std::hypot(car_x - route_.snapped_dest_x, car_z - route_.snapped_dest_z);
  if (d_exact <= kArrivalMeters || d_snap <= 24.0) {
    const GpsDestKind dk = guidance_.dest_kind;
    clear();
    guidance_.arrived = true;
    return {GpsPromptEvent::Kind::kArrived, GpsTurnKind::kArrive, dk};
  }

  // Project car onto closest segment of the active route.
  double best_d = std::numeric_limits<double>::infinity();
  double best_s = guidance_.route_progress_m;
  int best_seg = 0;
  double seg_dx = 0.0, seg_dz = 1.0;

  for (size_t i = 0; i + 1 < route_.points.size(); ++i) {
    const P3& a = route_.points[i];
    const P3& b = route_.points[i + 1];
    const double dx = b.x - a.x, dz = b.z - a.z, len2 = dx * dx + dz * dz;
    double t = len2 > 0.0 ? ((car_x - a.x) * dx + (car_z - a.z) * dz) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double px = a.x + dx * t, pz = a.z + dz * t;
    const double d = std::hypot(car_x - px, car_z - pz);
    if (d < best_d) {
      best_d = d;
      best_s = route_.s[i] + t * (route_.s[i + 1] - route_.s[i]);
      best_seg = int(i);
      const double len = std::sqrt(len2);
      if (len > 1e-4) seg_dx = dx / len, seg_dz = dz / len;
    }
  }

  last_seg_hint_ = best_seg;
  guidance_.route_progress_m = best_s;
  guidance_.off_route_m = best_d;
  guidance_.dist_to_dest_m = std::max(0.0, route_.total_length - best_s);

  // Live re-routing if the car deviates significantly off the route.
  if (best_d > kOffRouteMeters && reroute_cooldown_ <= 0.0 && solver_) {
    GpsRoute fresh = solver_->solve(car_x, car_z, dest_x_, dest_z_);
    if (fresh.valid) {
      route_ = std::move(fresh);
      reset_prompt_milestones();
      reroute_cooldown_ = kRerouteCooldownSec;
      guidance_.route_progress_m = 0.0;
      guidance_.off_route_m = 0.0;
      guidance_.dist_to_dest_m = route_.total_length;
      guidance_.next_turn = route_.turns.empty() ? GpsTurnKind::kArrive : route_.turns[0].kind;
      guidance_.dist_to_turn_m = route_.turns.empty() ? route_.total_length : route_.turns[0].route_s;
      return {GpsPromptEvent::Kind::kRecalculating, GpsTurnKind::kNone, guidance_.dest_kind};
    }
  }

  // Wrong-direction / U-turn check when driving along the route backwards.
  const double fx = std::sin(car_heading_rad), fz = std::cos(car_heading_rad);
  const double align = fx * seg_dx + fz * seg_dz;
  if (align < -0.55 && best_d < 35.0 && speed_mps > 3.5) {
    wrong_dir_time_ += dt;
    guidance_.next_turn = GpsTurnKind::kUTurn;
    guidance_.dist_to_turn_m = 0.0;
    if (wrong_dir_time_ > 1.2 && uturn_cooldown_ <= 0.0) {
      uturn_cooldown_ = kUTurnCooldownSec;
      return {GpsPromptEvent::Kind::kUTurn, GpsTurnKind::kUTurn, guidance_.dest_kind};
    }
    return {};
  }
  wrong_dir_time_ = 0.0;

  // Find next upcoming turn ahead of `best_s`.
  int next_idx = -1;
  for (size_t i = 0; i < route_.turns.size(); ++i) {
    if (route_.turns[i].route_s > best_s + 8.0) {
      next_idx = int(i);
      break;
    }
  }

  if (next_idx >= 0) {
    const GpsTurn& t = route_.turns[size_t(next_idx)];
    const double d_turn = std::max(0.0, t.route_s - best_s);
    guidance_.next_turn = t.kind;
    guidance_.dist_to_turn_m = d_turn;

    if (d_turn <= 50.0 && announced_now_turn_ != next_idx) {
      announced_now_turn_ = next_idx;
      announced_near_turn_ = next_idx;
      announced_far_turn_ = next_idx;
      return {GpsPromptEvent::Kind::kTurnNow, t.kind, guidance_.dest_kind};
    }
    if (d_turn <= 185.0 && d_turn > 65.0 && announced_near_turn_ != next_idx) {
      announced_near_turn_ = next_idx;
      announced_far_turn_ = next_idx;
      return {GpsPromptEvent::Kind::kTurnNear, t.kind, guidance_.dest_kind};
    }
    if (d_turn <= 420.0 && d_turn > 220.0 && announced_far_turn_ != next_idx) {
      announced_far_turn_ = next_idx;
      return {GpsPromptEvent::Kind::kTurnFar, t.kind, guidance_.dest_kind};
    }
  } else {
    guidance_.next_turn = GpsTurnKind::kArrive;
    guidance_.dist_to_turn_m = guidance_.dist_to_dest_m;
    if (guidance_.dist_to_dest_m <= 220.0 && guidance_.dist_to_dest_m > kArrivalMeters && !announced_near_dest_) {
      announced_near_dest_ = true;
      return {GpsPromptEvent::Kind::kArrivedNear, GpsTurnKind::kArrive, guidance_.dest_kind};
    }
  }
  return {};
}

void GpsNavigator::remaining_polyline(std::vector<std::array<double, 2>>& out, double max_ahead_m) const {
  out.clear();
  if (!guidance_.active || !route_.valid || route_.points.size() < 2) return;
  const double cur_s = std::clamp(guidance_.route_progress_m, 0.0, route_.total_length);
  const double end_s = max_ahead_m > 0.0 ? std::min(route_.total_length, cur_s + max_ahead_m) : route_.total_length;

  auto sample_at = [&](double s) -> std::array<double, 2> {
    size_t k = size_t(std::upper_bound(route_.s.begin(), route_.s.end(), s) - route_.s.begin());
    k = std::clamp<size_t>(k, 1, route_.points.size() - 1) - 1;
    const double l = route_.s[k + 1] - route_.s[k];
    const double t = l > 1e-6 ? std::clamp((s - route_.s[k]) / l, 0.0, 1.0) : 0.0;
    return {route_.points[k].x + (route_.points[k + 1].x - route_.points[k].x) * t,
            route_.points[k].z + (route_.points[k + 1].z - route_.points[k].z) * t};
  };

  out.push_back(sample_at(cur_s));
  for (size_t i = 0; i < route_.points.size(); ++i) {
    if (route_.s[i] <= cur_s + 0.5) continue;
    if (route_.s[i] >= end_s - 0.5) break;
    out.push_back({route_.points[i].x, route_.points[i].z});
  }
  out.push_back(sample_at(end_s));
}

}  // namespace traffic
