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
};

// A car's state as the controller reads it (game frame).
struct AiCarState {
  float x = 0, z = 0;
  float fwd_x = 0, fwd_z = 1;  // horizontal forward, unit
  float speed = 0;             // m/s along forward
  float wheelbase = 2.6f;      // m
  float steer_lock = 0.6f;     // rad at full stick
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

  // One pass over every car: reads `states`, writes `inputs` (same count).
  void drive(std::span<const AiCarState> states, std::span<Input> inputs);

  double progress(size_t i) const { return progress_[i]; }
  float cross_track(size_t i) const { return cross_[i]; }

 private:
  const RacingLine& line_;
  Options o_;
  std::vector<uint32_t> seg_;
  std::vector<double> progress_;
  std::vector<float> lane_, skill_, cross_;
};

}  // namespace drive
