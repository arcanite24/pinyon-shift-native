#include "traffic/gps.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace traffic {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = 180.0 / kPi;
constexpr double kMphToMps = 0.44704;
// Travel-time routing constants (GlobalRegistry.xml road speeds, OneWayMultiplier, TurnDelaySeconds).
constexpr double kMphFreeway = 75.0;
constexpr double kMphARoad = 60.0;
constexpr double kMphBRoad = 40.0;
constexpr double kMphDirtRoad = 35.0;
constexpr double kMphNone = 1.0;
constexpr double kMaxSpeedMps = kMphFreeway * kMphToMps;
constexpr double kOneWayMultiplier = 999.0;
constexpr double kTurnDelaySeconds = 5.0;
// Sharp-turn threshold (~35 degrees): cos(35 deg) ~= 0.819152.
constexpr double kSharpTurnCos = 0.8191520442889918;

bool is_drivable_type(const std::string& t) {
  return t == "dirt" || t == "b" || t == "a" || t == "freeway";
}

double road_speed_mps(const std::string& t) {
  if (t == "freeway") return kMphFreeway * kMphToMps;
  if (t == "a") return kMphARoad * kMphToMps;
  if (t == "b") return kMphBRoad * kMphToMps;
  if (t == "dirt") return kMphDirtRoad * kMphToMps;
  return kMphNone * kMphToMps;
}

double edge_travel_time(double len_m, const std::string& t, bool wrong_way = false) {
  const double base = std::max(1e-4, len_m / road_speed_mps(t));
  return wrong_way ? (base * kOneWayMultiplier) : base;
}

double turn_penalty_s(double in_dx, double in_dz, double out_dx, double out_dz, bool ignore_turn) {
  if (ignore_turn) return 0.0;
  const double dot = in_dx * out_dx + in_dz * out_dz;
  return dot < kSharpTurnCos ? kTurnDelaySeconds : 0.0;
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

GpsLookahead gps_lookahead_for_speed(double speed_mps) {
  const double v = std::clamp(std::fabs(speed_mps), 0.0, 105.0);
  GpsLookahead th;
  th.now_m = std::clamp(22.0 + v * 1.4, 30.0, 160.0);
  th.near_m = std::clamp(90.0 + v * 4.0, 110.0, 460.0);
  th.far_m = std::clamp(220.0 + v * 8.0, 260.0, 960.0);
  th.extended_m = std::clamp(450.0 + v * 14.0, 520.0, 1750.0);
  return th;
}

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
    case GpsTurnKind::kRoundabout: return "ENTER ROUNDABOUT";
    case GpsTurnKind::kNone:
    default: return "";
  }
}

size_t GpsSolver::roundabout_node_count() const {
  size_t c = 0;
  for (uint8_t v : roundabout_node_) c += (v != 0);
  return c;
}

size_t GpsSolver::uturn_node_count() const {
  size_t c = 0;
  for (uint8_t v : uturn_node_) c += (v != 0);
  return c;
}

void GpsSolver::build(const NavMap& nav) {
  roads_.build(nav);
  const size_t n_nodes = nav.nodes.size();
  const auto& rds = roads_.roads();

  drivable_road_.assign(rds.size(), 0);
  ignore_turn_node_.assign(n_nodes, 0);
  roundabout_node_.assign(n_nodes, 0);
  uturn_node_.assign(n_nodes, 0);
  node_xz_.resize(n_nodes);
  adj_.assign(n_nodes, {});
  edge_to_node_.clear();
  edge_dir_.clear();

  for (size_t i = 0; i < n_nodes; ++i) {
    node_xz_[i] = {nav.nodes[i].x, nav.nodes[i].z};
    if (nav.node_tag(uint32_t(i), "ignore_turn_flags") == "true") ignore_turn_node_[i] = 1;
    if (nav.node_tag(uint32_t(i), "sat_nav_junction") == "roundabout") roundabout_node_[i] = 1;
    const std::string_view ut = nav.node_tag(uint32_t(i), "uturn");
    if (!ut.empty() && ut != "0" && ut != "false") uturn_node_[i] = 1;
  }

  auto push_edge = [&](uint32_t u, uint32_t v, int ri, int seg, bool forward, double len, double cost,
                       bool wrong_way) {
    const double dx = node_xz_[v][0] - node_xz_[u][0];
    const double dz = node_xz_[v][1] - node_xz_[u][1];
    const double hlen = std::hypot(dx, dz);
    const double ux = hlen > 1e-6 ? (dx / hlen) : 0.0;
    const double uz = hlen > 1e-6 ? (dz / hlen) : 1.0;
    const uint32_t eid = uint32_t(edge_to_node_.size());
    edge_to_node_.push_back(v);
    edge_dir_.push_back({ux, uz});
    adj_[u].push_back({eid, u, v, ri, seg, forward, len, cost, ux, uz, wrong_way});
  };

  for (size_t ri = 0; ri < rds.size(); ++ri) {
    const Road& r = rds[ri];
    if (!is_drivable_type(r.type) || r.nodes.size() < 2) continue;
    drivable_road_[ri] = 1;
    for (size_t k = 0; k + 1 < r.nodes.size(); ++k) {
      const uint32_t u = r.nodes[k];
      const uint32_t v = r.nodes[k + 1];
      const double len = r.s[k + 1] - r.s[k];
      const double cost = edge_travel_time(len, r.type, false);
      push_edge(u, v, int(ri), int(k), true, len, cost, false);
      if (!r.oneway) {
        push_edge(v, u, int(ri), int(k), false, len, cost, false);
      } else {
        // Store wrong-way edge marked wrong_way=true with OneWayMultiplier=999.0
        // so fallback can recover from isolated one-way stubs while strongly
        // preferring any legal detour.
        push_edge(v, u, int(ri), int(k), false, len, edge_travel_time(len, r.type, true), true);
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
      const bool wrong_way = !forward && sr.oneway;
      direct_cost = edge_travel_time(std::fabs(gh.s - sh.s), sr.type, wrong_way);
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
      // Include sharp-turn penalties along direct same-road vertices so comparison
      // against A* travel time is consistent.
      for (size_t i = 1; i + 1 < direct.points.size(); ++i) {
        const double ax = direct.points[i].x - direct.points[i - 1].x;
        const double az = direct.points[i].z - direct.points[i - 1].z;
        const double bx = direct.points[i + 1].x - direct.points[i].x;
        const double bz = direct.points[i + 1].z - direct.points[i].z;
        const double la = std::hypot(ax, az), lb = std::hypot(bx, bz);
        if (la > 1e-4 && lb > 1e-4) {
          const int nid = pnodes[i];
          const bool ign = nid >= 0 && size_t(nid) < ignore_turn_node_.size() && ignore_turn_node_[size_t(nid)];
          direct_cost += turn_penalty_s(ax / la, az / la, bx / lb, bz / lb, ign);
        }
      }
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

  // Edge-state A* in travel-time seconds:
  // States 0..n_edges-1 represent arriving via directed edge `eid` (preserving exact
  // incoming tangent for TurnDelaySeconds); n_edges = seed s_b, n_edges+1 = seed s_a,
  // n_edges+2 = virtual goal.
  const uint32_t n_edges = uint32_t(edge_to_node_.size());
  const uint32_t kSeedSb = n_edges;
  const uint32_t kSeedSa = n_edges + 1;
  const uint32_t kGoalVirtual = n_edges + 2;
  const uint32_t n_states = n_edges + 3;
  const double kInf = std::numeric_limits<double>::infinity();

  std::vector<double> g_score(n_states, kInf);
  std::vector<int> parent(n_states, -1);
  std::vector<uint8_t> closed(n_states, 0);

  struct OpenItem {
    double f;
    uint32_t st;
    bool operator>(const OpenItem& o) const { return f > o.f; }
  };
  std::priority_queue<OpenItem, std::vector<OpenItem>, std::greater<OpenItem>> open;

  auto h_node = [&](uint32_t u) -> double {
    return std::hypot(node_xz_[u][0] - gp.x, node_xz_[u][1] - gp.z) / kMaxSpeedMps;
  };

  auto unit_dir = [](double x0, double z0, double x1, double z1, bool& valid) -> std::array<double, 2> {
    const double dx = x1 - x0, dz = z1 - z0, l = std::hypot(dx, dz);
    if (l > 1e-4) {
      valid = true;
      return {dx / l, dz / l};
    }
    valid = false;
    return {0.0, 1.0};
  };

  // Seed start segment endpoints.
  const uint32_t s_a = sr.nodes[size_t(sh.segment)];
  const uint32_t s_b = sr.nodes[size_t(sh.segment + 1)];
  const double d_sb = std::max(0.0, sr.s[size_t(sh.segment + 1)] - sh.s);
  const double d_sa = std::max(0.0, sh.s - sr.s[size_t(sh.segment)]);
  bool sb_dir_ok = false, sa_dir_ok = false;
  const auto sb_dir = unit_dir(node_xz_[s_a][0], node_xz_[s_a][1], node_xz_[s_b][0], node_xz_[s_b][1], sb_dir_ok);
  const auto sa_dir = unit_dir(node_xz_[s_b][0], node_xz_[s_b][1], node_xz_[s_a][0], node_xz_[s_a][1], sa_dir_ok);

  // Forward along start segment -> s_b
  {
    const double c = d_sb > 1e-4 ? edge_travel_time(d_sb, sr.type, false) : 0.0;
    g_score[kSeedSb] = c;
    parent[kSeedSb] = -2;
    open.push({c + h_node(s_b), kSeedSb});
  }
  // Backward along start segment -> s_a (if two-way or fallback)
  if (!sr.oneway || allow_wrong_way) {
    const double c = d_sa > 1e-4 ? edge_travel_time(d_sa, sr.type, sr.oneway) : 0.0;
    g_score[kSeedSa] = c;
    parent[kSeedSa] = -2;
    open.push({c + h_node(s_a), kSeedSa});
  }

  // Goal segment entry endpoints.
  const uint32_t g_a = gr.nodes[size_t(gh.segment)];
  const uint32_t g_b = gr.nodes[size_t(gh.segment + 1)];
  const double d_ga = std::max(0.0, gh.s - gr.s[size_t(gh.segment)]);
  const double d_gb = std::max(0.0, gr.s[size_t(gh.segment + 1)] - gh.s);
  bool ga_dir_ok = false, gb_dir_ok = false;
  const auto ga_dir = unit_dir(node_xz_[g_a][0], node_xz_[g_a][1], node_xz_[g_b][0], node_xz_[g_b][1], ga_dir_ok);
  const auto gb_dir = unit_dir(node_xz_[g_b][0], node_xz_[g_b][1], node_xz_[g_a][0], node_xz_[g_a][1], gb_dir_ok);

  auto state_node = [&](uint32_t st) -> uint32_t {
    if (st == kSeedSb) return s_b;
    if (st == kSeedSa) return s_a;
    return edge_to_node_[st];
  };
  auto state_dir = [&](uint32_t st, bool& has_dir) -> std::array<double, 2> {
    if (st == kSeedSb) {
      has_dir = sb_dir_ok;
      return sb_dir;
    }
    if (st == kSeedSa) {
      has_dir = sa_dir_ok;
      return sa_dir;
    }
    has_dir = true;
    return edge_dir_[st];
  };

  while (!open.empty()) {
    const auto cur = open.top();
    open.pop();
    const uint32_t st = cur.st;
    if (closed[st]) continue;
    closed[st] = 1;
    if (st == kGoalVirtual) break;
    const double gu = g_score[st];
    if (gu >= direct_cost && direct.valid) break;

    const uint32_t u = state_node(st);
    bool has_in_dir = false;
    const auto in_dir = state_dir(st, has_in_dir);
    const bool ign_turn = u < ignore_turn_node_.size() && ignore_turn_node_[u] != 0;

    // Virtual edges into goal.
    if (u == g_a) {
      double tp = 0.0;
      if (has_in_dir && ga_dir_ok && d_ga > 1e-4) tp = turn_penalty_s(in_dir[0], in_dir[1], ga_dir[0], ga_dir[1], ign_turn);
      const double c = gu + (d_ga > 1e-4 ? edge_travel_time(d_ga, gr.type, false) : 0.0) + tp;
      if (c < g_score[kGoalVirtual]) {
        g_score[kGoalVirtual] = c;
        parent[kGoalVirtual] = int(st);
        open.push({c, kGoalVirtual});
      }
    }
    if (u == g_b && (!gr.oneway || allow_wrong_way)) {
      double tp = 0.0;
      if (has_in_dir && gb_dir_ok && d_gb > 1e-4) tp = turn_penalty_s(in_dir[0], in_dir[1], gb_dir[0], gb_dir[1], ign_turn);
      const double c = gu + (d_gb > 1e-4 ? edge_travel_time(d_gb, gr.type, gr.oneway) : 0.0) + tp;
      if (c < g_score[kGoalVirtual]) {
        g_score[kGoalVirtual] = c;
        parent[kGoalVirtual] = int(st);
        open.push({c, kGoalVirtual});
      }
    }

    for (const Edge& e : adj_[u]) {
      if (e.wrong_way && !allow_wrong_way) continue;
      if (closed[e.id]) continue;
      const double tp = has_in_dir ? turn_penalty_s(in_dir[0], in_dir[1], e.dir_x, e.dir_z, ign_turn) : 0.0;
      const double ng = gu + e.cost + tp;
      if (ng < g_score[e.id]) {
        g_score[e.id] = ng;
        parent[e.id] = int(st);
        open.push({ng + h_node(e.to), e.id});
      }
    }
  }

  if (direct.valid && direct_cost <= g_score[kGoalVirtual]) return direct;
  if (!std::isfinite(g_score[kGoalVirtual]) || parent[kGoalVirtual] < 0) return direct;

  // Reconstruct node sequence from edge states.
  std::vector<uint32_t> rev_nodes;
  for (int cur = parent[kGoalVirtual]; cur >= 0; cur = parent[size_t(cur)]) {
    rev_nodes.push_back(state_node(uint32_t(cur)));
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

  // Compute smoothed incoming/outgoing horizontal tangent `span_m` before/after vertex i.
  auto dir_before = [&](size_t i, double span_m = 18.0) -> std::array<double, 2> {
    const double target_s = std::max(0.0, route.s[i] - span_m);
    size_t j = i;
    while (j > 0 && route.s[j] > target_s) --j;
    const double dx = route.points[i].x - route.points[j].x;
    const double dz = route.points[i].z - route.points[j].z;
    const double len = std::hypot(dx, dz);
    return len > 1e-4 ? std::array<double, 2>{dx / len, dz / len} : std::array<double, 2>{0.0, 1.0};
  };
  auto dir_after = [&](size_t i, double span_m = 18.0) -> std::array<double, 2> {
    const double target_s = std::min(route.total_length, route.s[i] + span_m);
    size_t j = i;
    while (j + 1 < route.points.size() && route.s[j] < target_s) ++j;
    const double dx = route.points[j].x - route.points[i].x;
    const double dz = route.points[j].z - route.points[i].z;
    const double len = std::hypot(dx, dz);
    return len > 1e-4 ? std::array<double, 2>{dx / len, dz / len} : std::array<double, 2>{0.0, 1.0};
  };

  for (size_t i = 1; i + 1 < route.points.size(); ++i) {
    const int nid = i < point_nodes.size() ? point_nodes[i] : -1;
    const bool is_roundabout = nid >= 0 && size_t(nid) < roundabout_node_.size() && roundabout_node_[size_t(nid)] != 0;
    const bool is_uturn_tag = nid >= 0 && size_t(nid) < uturn_node_.size() && uturn_node_[size_t(nid)] != 0;
    if (!is_roundabout && !is_uturn_tag && nid >= 0 && size_t(nid) < ignore_turn_node_.size() &&
        ignore_turn_node_[size_t(nid)]) {
      continue;
    }

    const bool is_junction = nid >= 0 && size_t(nid) < roads_.node_count() && roads_.degree(uint32_t(nid)) >= 3;
    const auto d0 = dir_before(i);
    const auto d1 = dir_after(i);
    const double ang = signed_turn(d0[0], d0[1], d1[0], d1[1]);
    const double deg = std::fabs(ang) * kDeg;

    if (is_roundabout) {
      // Sample wider 45 m entry/exit headings to determine roundabout exit direction for VO.
      const auto d0w = dir_before(i, 45.0);
      const auto d1w = dir_after(i, 45.0);
      const double exit_ang = signed_turn(d0w[0], d0w[1], d1w[0], d1w[1]);
      GpsTurnKind exit_kind = classify_turn(exit_ang);
      if (exit_kind == GpsTurnKind::kStraight || exit_kind == GpsTurnKind::kNone) {
        exit_kind = GpsTurnKind::kBearRight;
      }
      if (!route.turns.empty() && route.turns.back().is_roundabout && route.s[i] - route.turns.back().route_s < 65.0) {
        continue;
      }
      route.turns.push_back(
          {route.s[i], GpsTurnKind::kRoundabout, exit_ang, route.points[i].x, route.points[i].z, true, true, exit_kind});
      continue;
    }

    if (is_uturn_tag) {
      // Only announce a U-turn when the route actually reverses direction through the tagged node
      // (either sharply at the node or across a two-segment median turnaround).
      double wide_deg = deg;
      if (deg >= 75.0 && deg < 115.0) {
        const auto d0w = dir_before(i, 45.0);
        const auto d1w = dir_after(i, 45.0);
        wide_deg = std::fabs(signed_turn(d0w[0], d0w[1], d1w[0], d1w[1])) * kDeg;
      }
      if (deg >= 115.0 || wide_deg >= 135.0) {
        if (!route.turns.empty() && route.turns.back().kind == GpsTurnKind::kUTurn &&
            route.s[i] - route.turns.back().route_s < 55.0) {
          continue;
        }
        route.turns.push_back({route.s[i], GpsTurnKind::kUTurn, ang, route.points[i].x, route.points[i].z, is_junction,
                               false, GpsTurnKind::kUTurn});
        continue;
      }
    }

    // Emit a turn at junctions when deflection >= 24 deg, or at sharp non-junction corners >= 58 deg.
    if ((!is_junction && deg < 58.0) || deg < 24.0) continue;
    const GpsTurnKind kind = classify_turn(ang);
    if (kind == GpsTurnKind::kStraight || kind == GpsTurnKind::kNone) continue;

    // Merge consecutive vertices belonging to the same junction within 30 m.
    if (!route.turns.empty() && route.s[i] - route.turns.back().route_s < 30.0) {
      if (!route.turns.back().is_roundabout && route.turns.back().kind != GpsTurnKind::kUTurn &&
          std::fabs(ang) > std::fabs(route.turns.back().angle_rad)) {
        route.turns.back() = {route.s[i], kind, ang, route.points[i].x, route.points[i].z, is_junction, false,
                              GpsTurnKind::kNone};
      }
      continue;
    }
    route.turns.push_back(
        {route.s[i], kind, ang, route.points[i].x, route.points[i].z, is_junction, false, GpsTurnKind::kNone});
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
  announced_ext_turn_ = -1;
  announced_far_turn_ = -1;
  announced_near_turn_ = -1;
  announced_now_turn_ = -1;
  announced_ext_dest_ = false;
  announced_far_dest_ = false;
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

  if (!solver_ || !solver_->ready()) {
    return {GpsPromptEvent::Kind::kDestinationCannotBeSet, GpsTurnKind::kNone, kind};
  }
  route_ = solver_->solve(car_x, car_z, dest_x, dest_z);
  if (!route_.valid) {
    return {GpsPromptEvent::Kind::kDestinationCannotBeSet, GpsTurnKind::kNone, kind};
  }

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

  // Continuous speed-scaled lookahead thresholds (zero dead zones, scales up to 200+ mph).
  const GpsLookahead th = gps_lookahead_for_speed(speed_mps);

  if (next_idx >= 0) {
    const GpsTurn& t = route_.turns[size_t(next_idx)];
    const double d_turn = std::max(0.0, t.route_s - best_s);
    guidance_.next_turn = t.kind;
    guidance_.dist_to_turn_m = d_turn;

    if (d_turn <= th.now_m && announced_now_turn_ != next_idx) {
      announced_now_turn_ = next_idx;
      announced_near_turn_ = next_idx;
      announced_far_turn_ = next_idx;
      announced_ext_turn_ = next_idx;
      const auto ev_kind = (t.kind == GpsTurnKind::kUTurn) ? GpsPromptEvent::Kind::kUTurn : GpsPromptEvent::Kind::kTurnNow;
      return {ev_kind, t.kind, guidance_.dest_kind, t.exit_turn};
    }
    if (d_turn <= th.near_m && d_turn > th.now_m && announced_near_turn_ != next_idx) {
      announced_near_turn_ = next_idx;
      announced_far_turn_ = next_idx;
      announced_ext_turn_ = next_idx;
      const bool next_junc = t.at_junction && !t.is_roundabout && t.kind != GpsTurnKind::kUTurn && next_idx > 0 &&
                             (t.route_s - route_.turns[size_t(next_idx - 1)].route_s) <= th.far_m;
      const auto ev_kind = next_junc ? GpsPromptEvent::Kind::kTurnNextJunction : GpsPromptEvent::Kind::kTurnNear;
      return {ev_kind, t.kind, guidance_.dest_kind, t.exit_turn};
    }
    if (d_turn <= th.far_m && d_turn > th.near_m && announced_far_turn_ != next_idx) {
      announced_far_turn_ = next_idx;
      announced_ext_turn_ = next_idx;
      return {GpsPromptEvent::Kind::kTurnFar, t.kind, guidance_.dest_kind, t.exit_turn};
    }
    if (d_turn <= th.extended_m && d_turn > th.far_m && announced_ext_turn_ != next_idx) {
      announced_ext_turn_ = next_idx;
      return {GpsPromptEvent::Kind::kTurnExtended, t.kind, guidance_.dest_kind, t.exit_turn};
    }
  } else {
    guidance_.next_turn = GpsTurnKind::kArrive;
    guidance_.dist_to_turn_m = guidance_.dist_to_dest_m;
    const double d_dest = guidance_.dist_to_dest_m;
    if (d_dest <= th.near_m && d_dest > kArrivalMeters && !announced_near_dest_) {
      announced_near_dest_ = true;
      announced_far_dest_ = true;
      announced_ext_dest_ = true;
      return {GpsPromptEvent::Kind::kArrivedNear, GpsTurnKind::kArrive, guidance_.dest_kind};
    }
    if (d_dest <= th.far_m && d_dest > th.near_m && !announced_far_dest_) {
      announced_far_dest_ = true;
      announced_ext_dest_ = true;
      return {GpsPromptEvent::Kind::kArrivedFar, GpsTurnKind::kArrive, guidance_.dest_kind};
    }
    if (d_dest <= th.extended_m && d_dest > th.far_m && !announced_ext_dest_) {
      announced_ext_dest_ = true;
      return {GpsPromptEvent::Kind::kArrivedExtended, GpsTurnKind::kArrive, guidance_.dest_kind};
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
