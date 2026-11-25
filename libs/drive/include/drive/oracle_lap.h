// The M3 gate's lap against the oracle (SPEC-CAR-PHYSICS, "Oracle lap for
// the M3 gate"): the oracle's line from its lap.jsonl, the pedals replayed
// against the distance along that line, the steering by pure pursuit on
// it, and the race clock stopped at the start/finish plane.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "drive/vehicle.h"

namespace drive {

// One 60 Hz sample of the oracle's lap.jsonl (game frame).
struct LineSample {
  double t = 0;  // s from the start signal
  double x = 0, y = 0, z = 0, yaw_deg = 0;
  double distance = 0;  // distance_m: the body point's path length from t = 0
  double speed = 0, gas = 0, brake = 0, steer = 0;
  double grip_front = 1, grip_rear = 1;  // ai_grip: the AI's grip factor on dirt
};

struct OracleLine {
  std::vector<LineSample> samples;  // every sample, in time order
  LineSample start;                 // the last sample at or before t = 0: the grid
  double race_seconds = -1;         // the final dlc.rally.race_trace time_72, when it finished
};

// Reads lap.jsonl. False (with `error`) when no sample has t >= 0.
bool parse_oracle_line(const std::string& text, OracleLine& out, std::string* error);

// The start/finish line: the vertical plane through (x, z) normal to the
// horizontal crossing direction (nx, nz), crossed along it.
struct Gate {
  double x = 0, z = 0, nx = 0, nz = 1;
};

// The gate of lap.json ("gate": point_xz and crossing_direction_xz).
bool parse_gate(const std::string& lap_json, Gate& out, std::string* error);

// Follows the oracle's line: the car's progress is its projection on the
// line (moving forward only), the pedals are the oracle's at that
// distance, and the steer is pure pursuit at a look-ahead point.
// SPEC-RACE-AI, Steering at speed (the fitted rule): the road-wheel angle
// (rad) for alpha, the bearing of the look-ahead point less the body's
// heading, and beta, the velocity's direction less the heading (both in
// the steer's sense), clamped only by the full lock: 0.27 alpha + 0.29 beta.
inline double ai_slip_steer(double alpha, double beta, double lock) {
  const double d = 0.27 * alpha + 0.29 * beta;
  return d < -lock ? -lock : d > lock ? lock : d;
}

class LineFollower {
 public:
  struct Options {
    double lookahead_s = 0.5;    // look-ahead: this many seconds at the car's speed...
    double min_lookahead = 5.0;  // ...but at least this many metres
    double search = 60.0;        // m of line ahead searched for the projection
    // Cross-track feedback (ours): atan(k x offset / (v + 1)) added to the
    // pure-pursuit angle, so a corner taken with body slip holds the line
    // (pure pursuit from the body point settles wide). 0: pure pursuit only.
    double cross_gain = 0;
    // SPEC-RACE-AI, Steering at speed: the oracle lap is the title's AI
    // driving, so it steers by the AI's own rule (drive::ai_slip_steer:
    // delta = 0.27 alpha + 0.29 beta with the line point slip_lookahead_s x
    // v ahead, at least 2 m); false: pure pursuit (the M3 interim).
    bool slip_rule = true;
    double slip_lookahead_s = 1.0;
  };
  LineFollower(const OracleLine& line, const Options& options);

  // The input for this step (updates the progress).
  Input input(const Vehicle& v);

  double progress() const { return progress_; }      // m along the oracle's path
  double cross_track() const { return cross_; }      // m from the line, positive to the car's right
  bool saturated() const { return saturated_; }      // the stick asked for more than full lock
  double length() const { return dist_.empty() ? 0 : dist_.back(); }

  // The oracle's pedals at a distance along its path (the last sample at
  // equal distances, so the grid gives the launch's gas).
  void pedals_at(double distance, double& gas, double& brake) const;
  // The AI's grip factor (front, rear) at `distance`, like the pedals.
  void grip_at(double distance, double& front, double& rear) const;
  // The line's point at a distance along it.
  void point_at(double distance, double& x, double& z) const;

 private:
  void project(double x, double z);

  Options options_;
  std::vector<double> px_, pz_, dist_;     // the line's points (strictly increasing distance)
  std::vector<double> pd_, pgas_, pbrake_; // the pedal table
  std::vector<double> pgf_, pgr_;          // the grip factor, on the pedal table's distances
  size_t seg_ = 0;
  double progress_ = 0, cross_ = 0;
  bool saturated_ = false;
};

// Times the gate: the race time at which the point crosses the plane in its
// direction, after `min_progress` m and within `half_width` m of the point.
class GateTimer {
 public:
  GateTimer(const Gate& gate, double min_progress, double half_width = 20)
      : gate_(gate), min_progress_(min_progress), half_width_(half_width) {}
  // The point moved from (x0, z0) at t0 to (x1, z1) at t1. True once it crossed.
  bool update(double t0, double x0, double z0, double t1, double x1, double z1, double progress);
  bool crossed() const { return time_ >= 0; }
  double time() const { return time_; }
  double crossings() const { return crossings_; }  // every crossing in the direction, gated or not

 private:
  double side(double x, double z) const { return (x - gate_.x) * gate_.nx + (z - gate_.z) * gate_.nz; }
  Gate gate_;
  double min_progress_, half_width_;
  double time_ = -1;
  int crossings_ = 0;
};

// A mode-3 fh1.physics_trace sample in the oracle lap's layout: frame and
// t_s on the race clock, cars "0:<car>:0:speed:rpm:gear:gas:brake:steer"
// (gear -1 reverse, 0 neutral), the body point, yaw, the distance and the
// wheels' surfaces and contact.
std::string lap_trace_event(const Vehicle& v, int64_t car_id, double race_t, double distance);

// The final dlc.rally.race_trace event: started 1, ended 1 and end_reason 1
// with time_72 = time_88 = the race time, or ended 0 without a finish.
std::string race_trace_event(bool finished, double race_seconds);

}  // namespace drive
