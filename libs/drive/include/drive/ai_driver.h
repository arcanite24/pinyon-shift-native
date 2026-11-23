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

#include <array>
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
  // Optional, per point, the speed planner's geometry (SPEC-RACE-AI, Speed
  // planning): the horizontal curvature from 14 m chords (1/m, positive
  // turning right, towards +x when facing +z), the vertical curvature
  // (1/m, positive in a dip), the bank (rad, the left edge up) and the
  // grade (rise over run).
  std::vector<float> curvature, curvature_v, bank_left, grade;
  // Fills the four from x, y, z (and `lateral_y`, the left edge's rise
  // over the half width per point, when given; else no bank).
  void plan_geometry(std::span<const float> lateral_y = {});
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

// SPEC-RACE-AI, Speed planning (Q53): the car's capacity tables, 40
// entries over speed (entry n at 2.5 n m/s), looked up at floor(0.4 v)
// clamped, no interpolation; m/s^2.
struct AiCapacity {
  static constexpr int kEntries = 40;
  std::array<float, kEntries> lateral{}, braking{}, forward{};
  float braking_k = 0.94f;  // k_B: 0.96 four-wheel drive, 0.94 otherwise
  static int index(double v);
};
// From the car's own physics (the spec's interim: the engine's tyre model
// at the 40 speeds, the title's terms), six fixed-point passes each:
// - lateral: each wheel's load x its peak friction x cos(the peak slip
//   angle), the rear axle capped at (1 - w) / w x the front (w the front
//   weight share: the yaw balance), load moved inner to outer by each
//   axle's force x the CoM height / its track; over the mass;
// - braking: load moved forwards by the deceleration x the CoM height /
//   the wheelbase, with the braking friction scale, plus the aero drag;
//   over the mass.
// The engine's car model has no downforce, so the aero factor
// (ai_aero_factor) has nothing to scale yet. Pass the AI car's mass (the
// title's AI cars weigh 0.98 of the stock car on its races).
// Then times a calibration (ours): the engine's tyre model gives the AI's
// R34 14.6 and 17.1 m/s^2 at 20 m/s against the title's measured 16.01
// and 18.26 (SPEC-RACE-AI, Measured tables), so every car's tables are
// scaled by that and keep their differences from the car's own physics.
struct AiCapacityCalibration {
  double lateral = 16.01 / 14.6, braking = 18.26 / 17.1;
};
AiCapacity ai_capacity(const CarParams& p, const AiCapacityCalibration& cal = {});
// The planner's share of the downforce at speed v: 1 to 15 m/s, then
// 1 - 0.22 ((v - 15) / 60.1)^3.8, 0.78 from 75.1 m/s.
double ai_aero_factor(double v);
// The corner speed (m/s) for `cornering` C at curvature kappa (1/m,
// either sign), vertical curvature kappa_v (1/m, + in a dip) and bank
// theta (rad, + leaning into the turn), at most `top`: v^2 = g (sin + mu
// cos) / (|k| (cos - mu sin) - 0.75 k_v (sin + mu cos)), mu = C x
// lateral(v) / g, iterated up to 10 times on the table entry.
double ai_corner_speed(const AiCapacity& cap, double cornering, double kappa, double kappa_v, double theta, double top);
// The braking pass's two steps. The braking a corner leaves at v_lim
// under its corner speed v_c (the friction circle), from the deceleration
// `full` = B x A_brk: full x sqrt(1 - min(v_lim^2 / v_c^2, 1)). The carry
// one step of `ds` m back under acceleration a (< 0 decelerating): from
// 10 m/s v - a ds / v, below sqrt(v^2 - 2 a ds).
double ai_braking_left(double full, double v_lim, double v_corner);
double ai_brake_carry(double v, double a, double ds);
// The nemesis or start boost's clamp on a scalar s: min(m s, max(s, cap)).
inline double ai_boost(double s, double m, double cap) { return s >= cap ? s : std::min(m * s, cap); }

// A car's state as the controller reads it (game frame).
struct AiCarState {
  float x = 0, z = 0;
  float fwd_x = 0, fwd_z = 1;  // horizontal forward, unit
  float speed = 0;             // m/s along forward
  float vel_x = 0, vel_z = 0;  // the horizontal velocity, m/s
  float wheelbase = 2.6f;      // m
  float steer_lock = 0.6f;     // rad at full stick
  // The widest road-wheel angle the controller asks for: the pad's lock at
  // this speed (ours: past it the fronts slide and the car runs wide).
  float max_delta = 0.6f;
};
AiCarState ai_state(const Vehicle& v);

// The AI's traction and slide guard (ours; SPEC-CAR-PHYSICS leaves open
// which assists the AI-driven car has, and traces a traction-control
// signal on it, SPEC_QUESTIONS Q49): the throttle is capped while a wheel
// spins past the tyre curve's peak (the cap falls at 4 a second and comes
// back at 2), and is cut as the body's slip angle grows past 7 degrees, to
// nothing at 13. `cap` is the car's state, 1 at rest.
// `steer_guard` false leaves the steer alone (the SPEC-RACE-AI steering
// rule counter-steers by itself).
void ai_traction(const Vehicle& v, float& cap, double dt, Input& in, bool steer_guard = true);

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
    // SPEC-RACE-AI, Steering at speed: delta = 0.27 alpha + 0.29 beta, alpha
    // the bearing of the line point slip_lookahead_s x v ahead (at least
    // 2 m) less the heading, beta the velocity's direction less the
    // heading, clamped only by the full lock (off: pure pursuit under the
    // pad's speed-narrowed lock).
    bool slip_rule = false;
    float slip_lookahead_s = 1.0f;
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
  // SPEC-RACE-AI, Speed planning: car i plans from its capacity tables
  // and its cornering C and braking B scalars (the corner pass and the
  // braking pass over the 420 m ahead); `cap` null: set_limits' plan.
  // Needs the line's plan_geometry.
  void set_planner(size_t i, const AiCapacity* cap, float cornering, float braking);
  void set_lane(size_t i, float lane) { lane_[i] = lane; }
  // A cap on car i's planned speed, m/s (a car in the way; 1e9: none).
  void set_speed_cap(size_t i, float cap) { cap_[i] = cap; }
  float lane(size_t i) const { return lane_[i]; }
  // Moves car i's progress (a reset, or a car moved along the line).
  void set_progress(size_t i, double distance);

  // One pass over every car: reads `states`, writes `inputs` (same count).
  void drive(std::span<const AiCarState> states, std::span<Input> inputs);

  double progress(size_t i) const { return progress_[i]; }
  float cross_track(size_t i) const { return cross_[i]; }
  float target_speed(size_t i) const { return target_[i]; }
  // Rewind snapshot/restore of per-car spline tracking & dynamic targets.
  struct Snapshot {
    std::vector<uint32_t> seg;
    std::vector<double> progress;
    std::vector<float> lane, skill, cross, corner, decel, target, cap;
  };
  Snapshot snapshot() const { return {seg_, progress_, lane_, skill_, cross_, corner_, decel_, target_, cap_}; }
  void restore(const Snapshot& s) {
    if (s.seg.size() != seg_.size()) return;
    seg_ = s.seg;
    progress_ = s.progress;
    lane_ = s.lane;
    skill_ = s.skill;
    cross_ = s.cross;
    corner_ = s.corner;
    decel_ = s.decel;
    target_ = s.target;
    cap_ = s.cap;
  }
  // The speed car i plans at `distance` along the line (m/s).
  double planned_speed(size_t i, double distance) const;
  // The planner's corner speed of car i at line point j (m/s; the corner pass).
  double corner_speed(size_t i, size_t j) const { return corner_at(i, j); }

 private:
  const RacingLine& line_;
  Options o_;
  std::vector<uint32_t> seg_;
  std::vector<double> progress_;
  std::vector<float> lane_, skill_, cross_, corner_, decel_, target_, cap_;
  // The planner's per car: the tables, C, B and the corner speeds by line
  // point for the current C (NaN: not computed yet).
  std::vector<const AiCapacity*> capacity_;
  std::vector<float> plan_c_, plan_b_;
  mutable std::vector<std::vector<float>> corner_speed_;
  double corner_at(size_t i, size_t j) const;
};

}  // namespace drive
