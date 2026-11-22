// Free-roam GPS road-graph pathfinder & turn-by-turn navigator over colorado.nav.
// Respects one-way ways and junction connectivity, supports live re-routing on
// deviation, and emits discrete turn/arrival events for HUD & Anna Sat-Nav VO.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "traffic/nav.h"
#include "traffic/roads.h"

namespace traffic {

enum class GpsTurnKind : uint8_t {
  kNone = 0,
  kStraight,
  kBearLeft,
  kLeft,
  kSharpLeft,
  kBearRight,
  kRight,
  kSharpRight,
  kUTurn,
  kArrive,
};

enum class GpsDestKind : uint8_t {
  kGeneric = 0,
  kFestival,
  kFestivalRace,
  kShowcase,
  kOutpost,
  kWorkshop,
  kAutoshow,
  kPaintShop,
  kRaceCentral,
  kStreetRace,
};

struct GpsTurn {
  double route_s = 0.0;  // arc length from start of route (m)
  GpsTurnKind kind = GpsTurnKind::kNone;
  double angle_rad = 0.0;  // signed: positive right, negative left
  double x = 0.0, z = 0.0;
};

struct GpsRoute {
  bool valid = false;
  std::vector<P3> points;
  std::vector<double> s;  // cumulative horizontal distance along `points` (m)
  std::vector<GpsTurn> turns;
  double total_length = 0.0;
  double dest_x = 0.0, dest_z = 0.0;
  double snapped_dest_x = 0.0, snapped_dest_z = 0.0;
};

struct GpsGuidance {
  bool active = false;
  bool arrived = false;
  GpsTurnKind next_turn = GpsTurnKind::kNone;
  double dist_to_turn_m = 0.0;
  double dist_to_dest_m = 0.0;
  double off_route_m = 0.0;
  double route_progress_m = 0.0;
  std::string dest_label;
  GpsDestKind dest_kind = GpsDestKind::kGeneric;
};

struct GpsPromptEvent {
  enum class Kind : uint8_t {
    kNone = 0,
    kDestinationSet,
    kRecalculating,
    kTurnFar,
    kTurnNear,
    kTurnNow,
    kUTurn,
    kArrivedNear,
    kArrived,
  };
  Kind kind = Kind::kNone;
  GpsTurnKind turn = GpsTurnKind::kNone;
  GpsDestKind dest_kind = GpsDestKind::kGeneric;
};

const char* gps_turn_label(GpsTurnKind k);

class GpsSolver {
 public:
  // Builds the directed road graph from `nav` (uses drawn drivable roads:
  // dirt, b, a, freeway; respects oneway and junction connectivity).
  void build(const NavMap& nav);
  bool ready() const { return !adj_.empty() && !roads_.roads().empty(); }
  const Roads& roads() const { return roads_; }

  // Nearest drivable road point within `max_distance` (m).
  RoadHit nearest_road(double x, double z, double max_distance = 1200.0) const;

  // Computes the shortest path from (start_x, start_z) to (goal_x, goal_z).
  GpsRoute solve(double start_x, double start_z, double goal_x, double goal_z) const;

 private:
  struct Edge {
    uint32_t to = 0;
    int road = -1;
    int seg = 0;      // segment index on `road`
    bool forward = true;  // true: seg -> seg+1, false: seg+1 -> seg
    double length = 0.0;
    double cost = 0.0;
    bool wrong_way = false;
  };

  GpsRoute run_astar(const RoadHit& start_hit, const RoadHit& goal_hit, double goal_x, double goal_z,
                     bool allow_wrong_way) const;
  void extract_turns(GpsRoute& route, const std::vector<int>& point_nodes) const;

  Roads roads_;
  std::vector<uint8_t> drivable_road_;      // 1 if road_type in {dirt, b, a, freeway}
  std::vector<uint8_t> ignore_turn_node_;   // 1 if node tagged ignore_turn_flags=true
  std::vector<std::array<double, 2>> node_xz_;
  std::vector<std::vector<Edge>> adj_;
};

class GpsNavigator {
 public:
  explicit GpsNavigator(const GpsSolver* solver = nullptr) : solver_(solver) {}
  void set_solver(const GpsSolver* solver) { solver_ = solver; }

  bool active() const { return guidance_.active; }
  const GpsRoute& route() const { return route_; }
  const GpsGuidance& guidance() const { return guidance_; }

  // Sets a destination and solves an immediate route from (car_x, car_z).
  GpsPromptEvent set_destination(double dest_x, double dest_z, GpsDestKind kind, const std::string& label,
                                 double car_x, double car_z);
  void clear();

  // Advances GPS tracking for one frame. May trigger live re-routing or arrival
  // and returns any discrete prompt event for VO/HUD.
  GpsPromptEvent update(double car_x, double car_z, double car_heading_rad, double speed_mps, double dt);

  // Writes the remaining route polyline (from the car's projected position to
  // destination) as (x, z) points for minimap and fullscreen map rendering.
  void remaining_polyline(std::vector<std::array<double, 2>>& out, double max_ahead_m = -1.0) const;

  static constexpr double kOffRouteMeters = 65.0;
  static constexpr double kArrivalMeters = 35.0;
  static constexpr double kRerouteCooldownSec = 2.5;
  static constexpr double kUTurnCooldownSec = 8.0;

 private:
  void reset_prompt_milestones();

  const GpsSolver* solver_ = nullptr;
  GpsRoute route_{};
  GpsGuidance guidance_{};
  double dest_x_ = 0.0, dest_z_ = 0.0;
  int last_seg_hint_ = 0;
  int announced_far_turn_ = -1;
  int announced_near_turn_ = -1;
  int announced_now_turn_ = -1;
  bool announced_near_dest_ = false;
  double reroute_cooldown_ = 0.0;
  double uturn_cooldown_ = 0.0;
  double wrong_dir_time_ = 0.0;
};

}  // namespace traffic
