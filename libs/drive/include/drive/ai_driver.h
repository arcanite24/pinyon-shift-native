// AI drivers for M5's groundwork (no spec yet: SPEC-RACE-AI is being
// written): N cars follow one shared racing line on the existing physics.
// Data-oriented: the line is one immutable set of arrays every car reads;
// the field keeps each car's state in arrays (struct of arrays) and updates
// all cars in one pass, with O(1) work per car (a bounded projection from
// the last segment, one look-ahead lookup, two controllers).
// Ours, to be replaced by the spec's rules: pure pursuit on the line with a
// lane offset, a speed target from the line's speed profile scaled by the
// car's skill, proportional throttle and brake. No rubber-banding, no
// avoidance between cars.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "drive/oracle_lap.h"
#include "drive/vehicle.h"

namespace drive {

// A line several cars share: points with their distance along it and a
// target speed (m/s) at each.
struct RacingLine {
  std::vector<float> x, z, dist, speed;
  // Optional, per point (SPEC-RACE-AI lines): the height, the radius of
  // the line's curve (m; large on a straight) for the per-car speed plan,
  // and the room from the line to the road's right and left edges (m).
  std::vector<float> y, radius, room_right, room_left;
  double length() const { return dist.empty() ? 0 : dist.back(); }
  size_t size() const { return dist.size(); }

  // From a polyline (game frame): the speed from curvature, lat_accel /
  // curvature under `max_speed`, then limited by `accel` forwards and
  // `decel` backwards along the line (m/s^2).
  static RacingLine from_points(std::span<const float> x, std::span<const float> z, double lat_accel = 9.0,
                                double accel = 4.0, double decel = 8.0, double max_speed = 70.0);
  // From the oracle's lap: its path and its speed at each point.
  static RacingLine from_oracle(const OracleLine& line);

  // The segment holding `distance`, searched forward from `hint`.
  size_t segment(double distance, size_t hint) const;
  // The point `lane` m right of the line at `distance`, its height (the
  // line's y, else 0) and heading (0 faces +z, positive towards +x).
  void pose(double distance, double lane, double& x, double& y, double& z, double& heading) const;
  // The distance along the line of the point nearest (x, z), searched over
  // [from, to] m of the line.
  double nearest(double x, double z, double from, double to) const;
  // The room from the line to the road's edge on the right (lane > 0) or
  // the left at `distance`; 1e9 when the line has none.
  double room(double distance, bool right) const;
};

// A car's state as the controller reads it (game frame).
struct AiCarState {
  float x = 0, z = 0;
  float fwd_x = 0, fwd_z = 1;  // horizontal forward, unit
  float speed = 0;             // m/s along forward
  float wheelbase = 2.6f;      // m
  float steer_lock = 0.6f;     // rad at full stick
  // The widest road-wheel angle the controller asks for: the pad's lock at
  // this speed (ours: past it the fronts slide and the car runs wide).
  float max_delta = 0.6f;
};
AiCarState ai_state(const Vehicle& v);

class AiField {
 public:
  struct Options {
    float lookahead_s = 0.6f;     // steering look-ahead, s of speed...
    float min_lookahead = 6.0f;   // ...but at least this many metres
    float speed_lookahead_s = 0.4f;  // the speed target this far ahead
    float search = 40.0f;         // m of line searched ahead for the projection
    float throttle_gain = 0.35f;  // per m/s under the target
    float brake_gain = 0.25f;     // per m/s over the target
    float deadband = 0.5f;        // m/s
    // SPEC-RACE-AI, Throttle: full gas below the planned speed, lift then
    // brake above it (the AI's gas jumps between 1 and 0).
    bool full_gas = false;
    float max_speed = 90.0f;  // m/s, the plan's cap
    float off_line_slack = 1.0f;  // m off the lane before the car slows...
    float off_line_slow = 0.0f;   // ...by this fraction a metre (0: never)
  };
  AiField(const RacingLine& line, size_t cars, const Options& options);
  AiField(const RacingLine& line, size_t cars) : AiField(line, cars, Options{}) {}

  size_t size() const { return progress_.size(); }
  // Starts car `i` at `distance` along the line, `lane` m to the right of
  // it, driving at `skill` times the line's speed.
  void place(size_t i, double distance, float lane, float skill = 1.0f);
  // Where car i's start puts it: the point, and the heading (drive
  // convention: 0 faces +z, positive towards +x).
  void start_pose(size_t i, double& x, double& z, double& heading) const;

  // Starts car i at the line point nearest (x, z) within [from, to] m.
  void place_near(size_t i, double x, double z, double from, double to, float skill = 1.0f);
  // SPEC-RACE-AI, Speed: car i plans its speed from the line's curve radius
  // with a corner acceleration `corner` and braking `decel` (m/s^2); 0 keeps
  // the line's own speed x skill. Needs the line's radius.
  void set_limits(size_t i, float corner, float decel) { corner_[i] = corner, decel_[i] = decel; }
  void set_lane(size_t i, float lane) { lane_[i] = lane; }
  float lane(size_t i) const { return lane_[i]; }
  // Moves car i's progress (a reset, or a car moved along the line).
  void set_progress(size_t i, double distance);

  // One pass over every car: reads `states`, writes `inputs` (same count).
  void drive(std::span<const AiCarState> states, std::span<Input> inputs);

  double progress(size_t i) const { return progress_[i]; }
  float cross_track(size_t i) const { return cross_[i]; }
  float target_speed(size_t i) const { return target_[i]; }
  // The speed car i plans at `distance` along the line (m/s).
  double planned_speed(size_t i, double distance) const;

 private:
  const RacingLine& line_;
  Options o_;
  std::vector<uint32_t> seg_;
  std::vector<double> progress_;
  std::vector<float> lane_, skill_, cross_, corner_, decel_, target_;
};

}  // namespace drive
