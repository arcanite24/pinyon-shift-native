// The drivable network of a `.nav` map (SPEC-TRAFFIC, Tags): the ways
// tagged `road_type`, each as a polyline in node order with its density
// id and flags, the nodes they share, and a grid to find the road nearest
// a point.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "traffic/config.h"
#include "traffic/nav.h"

namespace traffic {

struct P3 {
  double x = 0, y = 0, z = 0;
};

struct Road {
  uint32_t way = 0;
  std::string type;          // road_type
  int density = 0;           // traffic_density id (0: none)
  bool disabled = false;     // traffic_disabled or ai_disabled
  bool oneway = false;       // one direction, in node order
  bool traffic = false;      // a density id, not disabled
  std::vector<uint32_t> nodes;  // node indices
  std::vector<P3> points;
  std::vector<double> s;     // arc length (horizontal) at each point
  std::vector<double> radius;  // the bend radius at each point (m; large on straights and at the ends)
  double length = 0;         // horizontal, m
  double length3 = 0;        // along the 3-D segments, m
};

// Where a point lies relative to a road: the nearest point of the centre
// line, its arc length, and the signed horizontal offset (positive: right
// of travel in node order).
struct RoadHit {
  int road = -1;
  int segment = 0;
  double s = 0;
  double distance = 1e30;  // horizontal, m
  double lateral = 0;
};

// The right of travel along (dx, dz), in the game's frame (+X right when
// facing +Z).
inline std::array<double, 2> right_of(double dx, double dz) { return {dz, -dx}; }

class Roads {
 public:
  void build(const NavMap& map);
  const std::vector<Road>& roads() const { return roads_; }
  // The roads meeting at a node: (road, index of the node in it).
  const std::vector<std::pair<int, int>>& at_node(uint32_t node) const { return at_node_[node]; }
  // Road segments incident on a node (2 on a plain point, 1 at an end, 3+ at a junction).
  int degree(uint32_t node) const { return degree_[node]; }
  size_t node_count() const { return degree_.size(); }

  // The nearest road (traffic roads only when asked) within `max_distance`.
  RoadHit nearest(double x, double z, bool traffic_only, double max_distance = 60) const;
  // A point of a road at arc length s (clamped), its height interpolated,
  // and the unit horizontal direction of node order there.
  P3 point_at(int road, double s, int* segment = nullptr, std::array<double, 2>* dir = nullptr) const;
  // The horizontal length of traffic road within `radius` of (x, z), by
  // density id.
  std::map<int, double> traffic_length_within(double x, double z, double radius) const;

 private:
  std::vector<Road> roads_;
  std::vector<std::vector<std::pair<int, int>>> at_node_;
  std::vector<int> degree_;
  // The grid: segments (road, segment) by cell.
  double cell_ = 50, min_x_ = 0, min_z_ = 0;
  int nx_ = 0, nz_ = 0;
  std::vector<std::vector<std::pair<int, int>>> grid_;
};

}  // namespace traffic
