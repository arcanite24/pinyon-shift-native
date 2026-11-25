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

#include <algorithm>
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
  // Optional, per point: surface friction scale (1.0 on dry asphalt, < 1.0 on
  // dirt/gravel/grass). Defaults to 1.0f everywhere when empty.
  std::vector<float> surface_grip;
  float grip_at(size_t j) const {
    if (surface_grip.size() != size() || j >= surface_grip.size()) return 1.0f;
    return std::clamp(surface_grip[j], 0.1f, 2.0f);
  }
  // Fills the four from x, y, z (and `lateral_y`, the left edge's rise
  // over the half width per point, when given; else no bank).
  void plan_geometry(std::span<const float> lateral_y = {});
  double length() const { return dist.empty() ? 0 : dist.back(); }
  size_t size() const { return dist.size(); }

  // From a polyline (game frame): the speed from curvature, lat_accel /
  // curvature under `max_speed`, then limited by `accel` forwards and
  // `decel` backwards along the line (m/s^2).
  static RacingLine from_points(std::span<const float> x, std::span<const float> z, double lat_accel = 9.0,
                                double accel = 4.0, double decel = 8.0, double max_speed = 70.0,
                                std::span<const float> surface_grip = {});
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
  // The steering-saturation grip term's threshold on the grip level: 0.97
  // four-wheel drive, 0.94 otherwise (the same drivetrain test as k_B).
  float sat_level_min = 0.94f;
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
struct AiCarState;
struct AiSteerState;
using AiYawGain = std::array<float, 20>;
// The line controller's steer (-1 to 1) towards the aim point (lx, lz) in
// the car's frame (x right, z forward) with the driven line's unit tangent
// there (gx, gz), the car e m from the driven line and chi_car half widths
// from the line's centre; back: the last steer turned towards the line.
double ai_controller_steer(const AiCarState& c, const AiYawGain& G, const AiCapacity* cap, double chi_car, double e,
                           double lx, double lz, double gx, double gz, bool back, AiSteerState& st);
// The planner's share of the downforce at speed v: 1 to 15 m/s, then
// 1 - 0.22 ((v - 15) / 60.1)^3.8, 0.78 from 75.1 m/s.
double ai_aero_factor(double v);
// The corner speed (m/s) for `cornering` C at curvature kappa (1/m,
// either sign), vertical curvature kappa_v (1/m, + in a dip) and bank
// theta (rad, + leaning into the turn), at most `top`: v^2 = g (sin + mu
// cos) / (|k| (cos - mu sin) - 0.75 k_v (sin + mu cos)), mu = C x
// lateral(v) / g, iterated up to 10 times on the table entry.
// `slope_curvature`: the set's SpeedComputerSlopeCurvatureModifier
// (AIRacing.xml PredictiveControllerRLH; `default` 0.75).
double ai_corner_speed(const AiCapacity& cap, double cornering, double kappa, double kappa_v, double theta, double top,
                       double slope_curvature = 0.75);
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
  float yaw_rate = 0;  // rad/s about up, + turning towards + steer
  // The pedals' inputs (SPEC-RACE-AI, Throttle): the hold throttle
  // (Vehicle::hold_throttle; 0.6 before the first), and each wheel's slip
  // metric, front left, front right, rear left, rear right (0 off the
  // ground). The title's metric is a per-wheel value from its tyre model
  // that was not decoded; ours is the normalised combined slip,
  // hypot(slip ratio, slip angle), 1 at the curves' peaks (Q175).
  float hold = 0.6f;
  std::array<float, 4> slip{};
};
AiCarState ai_state(const Vehicle& v);

// The AI line controller's yaw gain G(v) (AIRacing.xml
// PredictiveControllerRLH; SPEC-RACE-AI, Steering): the yaw rate per unit
// of steer (rad/s), 20 bins of 5 m/s centred at (i + 0.5) x 5, linear in
// between. The title fills it per car from a run of the car's own
// simulation; ours steps the car on flat ground at each bin's speed with a
// small steer (Q125). 4 where the run gives nothing usable.
using AiYawGain = std::array<float, 20>;
AiYawGain ai_yaw_gain(const CarParams& p);
double ai_yaw_gain_at(const AiYawGain& g, double v);
// The line controller's per-car state: the look-ahead (m), the under-steer
// boost and |e| a step ago (the caller keeps abs_e).
struct AiSteerState {
  float look = 7.5f, boost = 1.0f, abs_e = 0;
  // The line controller's grip level (1 + 0.025 x the cheat level; 0:
  // none), which opens the steering-saturation grip term above the car's
  // threshold (AiCapacity::sat_level_min).
  float level = 0;
  // That term (0 to ramp(level, threshold, 1.25)) from the last steer, on both
  // axles' AI grip factor.
  float sat = 0;
};
// The look-ahead along the driven line (m) for a car chi_car half widths
// off the line's centre, with the track's floor (m); updates st.look.
double ai_controller_lookahead(const AiCarState& c, const AiYawGain& G, double chi_car, double floor_track,
                               AiSteerState& st);
struct AiCapacity;

// SPEC-RACE-AI, Throttle (the title's line controller's pedals, decoded).
// The per-car state: the pedal state (0 none yet, 1 stop, 2 hold, 3 full
// gas, 4 track edge, 5 brake), the last gas and brake, and the smoothed
// wheel slip metrics (the mean over the four wheels and the larger front).
struct AiPedalState {
  int state = 0;
  float throttle = 0, brake = 0;
  float slip_mean = 0, slip_front = 0;
};
// The slip metrics' update, once a step before the pedals. At the title's
// 60 Hz: mean <- 0.2 mean + 0.2 (sum + 0.25 x the wheels at 0), front <-
// 0.2 front + 0.8 max(front left, front right) (0.25 when both are 0); per
// step of dt the old value keeps 0.2^(60 dt) and the steady states hold.
void ai_slip_metrics(const AiCarState& c, double dt, AiPedalState& st);
// What the pedals read: the speed target (m/s, the plan with its caps; 0
// stops), the forward speed, the hold throttle H, the friction circle's
// share left for braking at the target's point (0 to 1), whether
// track-edge avoidance holds this step with |e|'s growth rate (m/s),
// whether the car is off the road, and the heading's cosine to the line
// and chi.
struct AiPedalIn {
  double target = 0, speed = 0, hold = 0.6, friction = 1;
  bool edge = false;
  double edge_rate = 0;
  bool off_track = false;
  double heading_cos = 1, chi = 0;
};
// The state from err = target - speed: 1 at a target of 0; 4 while edge
// avoidance holds and err > -1; else 3 stays while err > 0.25 and 5 while
// err < 0; else 2 within 1 m/s, 5 below, 3 above. The pedals: 1 brakes
// clamp(|speed|, 0.5, 1); 2 holds H (1 + err); 3 is full gas; 4 eases the
// last gas by 0.016 x the rate (at 60 Hz) above the plan and brakes as 5
// at or below it; 5 brakes 0.6 x the friction share. Off the road with the
// front metric at 1 or more: no gas in 2, and in 3 H (0 from 1.05). Then
// the gas is capped at 0.6 at a heading cosine under 0.1 and at
// clamp(1.05 - mean metric, 0.2, 0.9) past |chi| 1 with the mean above
// 0.25, and the brake is filtered: rising from under 0.95, 0.75 old + 0.25
// new; falling from over 0.05, 0.5 old + 0.5 new (at 60 Hz; per step of
// dt the old share is that to the 60 dt).
void ai_pedals(const AiPedalIn& in, double dt, AiPedalState& st, double& throttle, double& brake);

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
    // The race's AIRacing.xml set's SpeedComputerSlopeCurvatureModifier
    // (the vertical curvature's weight in the corner speed).
    float slope_curvature = 0.75f;
    float off_line_slack = 1.0f;  // m off the lane before the car slows...
    float off_line_slow = 0.0f;   // ...by this fraction a metre (0: never)
    // SPEC-RACE-AI, Steering at speed: delta = 0.27 alpha + 0.29 beta, alpha
    // the bearing of the line point slip_lookahead_s x v ahead (at least
    // 2 m) less the heading, beta the velocity's direction less the
    // heading, clamped only by the full lock (off: pure pursuit under the
    // pad's speed-narrowed lock).
    bool slip_rule = false;
    float slip_lookahead_s = 1.0f;
    // Pure pursuit only (ours): the steering command's maximum rate, in
    // full-lock units per second (0: unlimited). The slip rule has none.
    float steer_rate = 3.8f;
    // The title's line controller (AIRacing.xml PredictiveControllerRLH,
    // decoded; SPEC-RACE-AI, Steering): a look-ahead of v x 1.2 / mean G,
    // the tangent of a Hermite curve to the line there, a yaw-rate demand
    // 2 v sin(alpha) / l capped by the car's lateral capacity, over the
    // car's yaw gain, less a yaw-rate damping, and an under-steer boost.
    // Takes precedence over slip_rule.
    bool controller = false;
    // DisableTrackEdgeAvoidance false: the gas eases off while the car,
    // steering back, still runs towards the road's edge.
    bool track_edge_avoidance = true;
    // The look-ahead's floor above 17 m/s, m (the title's is 6 to 10 m by
    // track, from a measure not decoded; Q125).
    float min_lookahead_track = 8.0f;
    // The title's pedals (SPEC-RACE-AI, Throttle; ai_pedals) under the line
    // controller, in place of full_gas: the target from the plan 0.67 of a
    // line point past the car's, interpolated; the heading speed cap (0.2
    // of max_speed with the heading's cosine under 0.1, 0.1 under -0.5);
    // while steering back, the target cut by k off the road (0.02 at |e|
    // 0.25 m to 0.12 plus the metres past a metre outside at 3 m) or far
    // off the line on the road (|e| over 2.5 m, |chi| over 0.25, the front
    // metric over 1.5: 0 at 2.5 m to 0.1 at 10 m).
    bool title_pedals = false;
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
  // Car i's yaw gain for the line controller (Options::controller).
  void set_yaw_gain(size_t i, const AiYawGain& g) { yaw_gain_[i] = g; }
  // Car i's steering-saturation grip term (0 to ramp(level, threshold,
  // 1.25): 0.19 at the default level 1 on a two-wheel-drive car) from its
  // last drive.
  float grip_sat(size_t i) const { return sat_[i]; }
  // Car i's grip level for that term (SPEC-RACE-AI, Steering): the line
  // controller's level, 1 + 0.025 n for the driver's cheat level n
  // (race::ai_cheat_level, decoded), 1 by default (an interim: the title's
  // oracle lap argues for less, Q125). The planner's C and its boosts do
  // not reach it.
  void set_grip_level(size_t i, float level) { level_[i] = level; }
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
    std::vector<float> lane, skill, cross, corner, decel, target, cap, prev_steer;
    std::vector<float> look, boost, abs_e, chi, prev_throttle;
    std::vector<AiPedalState> pedal;
  };
  Snapshot snapshot() const {
    return {seg_, progress_, lane_, skill_, cross_, corner_, decel_, target_, cap_, prev_steer_,
            look_, boost_, abs_e_, chi_, prev_throttle_, pedal_};
  }
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
    if (s.prev_steer.size() == prev_steer_.size()) prev_steer_ = s.prev_steer;
    if (s.look.size() == look_.size()) {
      look_ = s.look, boost_ = s.boost, abs_e_ = s.abs_e, chi_ = s.chi, prev_throttle_ = s.prev_throttle;
    }
    if (s.pedal.size() == pedal_.size()) pedal_ = s.pedal;
  }
  // Car i's pedal state (AiPedalState::state; 0 off the title's pedals).
  int pedal_state(size_t i) const { return pedal_[i].state; }
  const AiPedalState& pedals(size_t i) const { return pedal_[i]; }
  // The speed car i plans at `distance` along the line (m/s).
  double planned_speed(size_t i, double distance) const;
  // The planner's corner speed of car i at line point j (m/s; the corner pass).
  double corner_speed(size_t i, size_t j) const { return corner_at(i, j); }

 private:
  const RacingLine& line_;
  Options o_;
  std::vector<uint32_t> seg_;
  std::vector<double> progress_;
  std::vector<float> lane_, skill_, cross_, corner_, decel_, target_, cap_, prev_steer_;
  // The line controller's per car: the look-ahead (m), the under-steer
  // boost, |e| and chi a step ago, and the last gas.
  std::vector<float> look_, boost_, abs_e_, chi_, prev_throttle_, sat_;
  std::vector<AiPedalState> pedal_;
  std::vector<float> level_;
  std::vector<AiYawGain> yaw_gain_;
  // The planner's per car: the tables, C, B and the corner speeds by line
  // point for the current C (NaN: not computed yet).
  std::vector<const AiCapacity*> capacity_;
  std::vector<float> plan_c_, plan_b_;
  mutable std::vector<std::vector<float>> corner_speed_;
  double corner_at(size_t i, size_t j) const;
  // The plan at line point j (m/s) and, with the planner, the friction
  // circle's share left there for braking (1 without it).
  double plan_point(size_t i, size_t j, double* friction) const;
};

}  // namespace drive
