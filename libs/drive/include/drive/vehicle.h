// The engine's own car physics for M3 (REIMPLEMENTATION_PLAN M3).
//
// One rigid body with four raycast wheels, stepped at a fixed 1/120 s split
// into n = ceil(dt x CarPhysicsUpdateHz) substeps (SPEC-CAR-PHYSICS, Step
// and rates: 3 at 1/120 s). The parameters come from the car's database
// rows (from_physics.h, SPEC-CAR-PHYSICS items 4 to 6); the wheel geometry
// from the baked car (FORMAT-CARS, Wheels). A CarParams left at its
// defaults is a synthetic car of the engine's own (no game data), for tests.
//
// Frame: the game's body frame, +X right, +Y up, +Z forward (FORMAT-CARS,
// the model-to-body pose mirrors the model's z). The world uses the same
// axes with y up. The renderer applies the mirror to the model; the physics
// never sees it.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace drive {

struct Vec3 {
  double x = 0, y = 0, z = 0;
};
inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
struct Quat {  // unit quaternion (x, y, z, w)
  double x = 0, y = 0, z = 0, w = 1;
};

Vec3 rotate(const Quat& q, const Vec3& v);
Vec3 rotate_inverse(const Quat& q, const Vec3& v);

// The ground the wheels stand on. raycast returns the distance along dir
// (unit) from origin to the first hit within max_distance, with the
// surface normal and a grip factor (the surface's FrictionScale).
// A surface's tyre columns (SPEC-CAR-PHYSICS, Tyre model): the rear
// wheels' multiplier, and how far it moves the lateral peak slip towards
// its own (degrees).
struct SurfaceTyre {
  double rear_grip = 1, off_roadness = 0, off_road_peak_sa = 12;
};
struct Hit {
  double distance = 0;
  Vec3 normal{0, 1, 0};
  double grip = 1;
  int surface = -1;  // the ground's surface index, when it has one
  SurfaceTyre tyre;
  int64_t triangle = -1;  // the ground's triangle index, when it has one
};
class Ground {
 public:
  virtual ~Ground() = default;
  virtual bool raycast(const Vec3& origin, const Vec3& dir, double max_distance, Hit& hit) const = 0;
};
// The plane y = height.
class FlatGround : public Ground {
 public:
  explicit FlatGround(double height = 0) : height_(height) {}
  bool raycast(const Vec3& origin, const Vec3& dir, double max_distance, Hit& hit) const override;

 private:
  double height_;
};

// Wheels in the order LF, RF, LR, RR. Axle 0 is the front, 1 the rear.
inline constexpr int kWheels = 4;
inline int axle_of(int wheel) { return wheel < 2 ? 0 : 1; }

// SPEC-CAR-PHYSICS, Step and rates: the substeps for an outer step dt
// (clamped to 0.1 s): ceil(dt x hz), at least 1.
int substeps_for(double dt, double hz);

// A friction curve (List_TireFrictionCurve): values at evenly spaced slips
// from 0 to max_slip (slip ratio, or slip angle in degrees).
struct FrictionCurve {
  double max_slip = 0;
  std::vector<double> values;
  // The slip of the largest sample (the later one on a tie).
  double peak_slip() const;
  // The value at `slip` on the curve's axis stretched by `stretch`.
  double at(double slip, double stretch) const;
};
// List_TireFrictionMultiCurve: the curve at a light and a heavy load (kgf),
// blended linearly by load between them.
struct TyreCurves {
  FrictionCurve light, heavy;
  double light_load_kgf = 10.1972, heavy_load_kgf = 1019.72, load_clamp_kgf = 5000;
  double blend(double load_kgf) const;  // 0 at the light load, 1 at the heavy
  double peak_slip(double load_kgf, double stretch) const;
  // `scale`: each curve's row FrictionScale (light, heavy).
  double value(double slip, double load_kgf, double stretch, const std::array<double, 2>& scale = {1, 1}) const;
};
// The engine's own stand-in curve for the synthetic car: rises to 1 at
// `peak`, falls to `tail` at the end (no game data).
FrictionCurve synthetic_curve(double max_slip, double peak, double tail);

struct CarParams;
// The tyre's peak friction (force over load) on `axle` at `load_n` N on the
// wheel and `speed` m/s, through the same chain of scales the step uses
// (the curves' FrictionScale, the width, axle, compound and misc scales;
// braking: the braking friction scale; driving: the accel blend), on
// asphalt with no AI grip factor. For the AI's capacity tables.
enum class TyrePeak { kLateral, kBraking, kDriving };
double tyre_peak_mu(const CarParams& p, int axle, TyrePeak kind, double load_n, double speed);

// Everything the model needs about one car. See from_physics.h for where
// each comes from on a real car; the defaults are the synthetic car's.
struct CarParams {
  // Geometry (body frame, at rest on flat ground).
  std::array<Vec3, kWheels> wheel_centres{};  // y = the tyre's outer radius
  std::array<double, kWheels> tyre_radius{};  // outer radius, m

  // Body.
  double mass = 1400;               // kg
  Vec3 centre_of_mass{0, 0.5, 0};   // body frame; from_bake puts z at mid-wheelbase
  Vec3 inertia{0, 0, 0};            // principal moments about x, y, z; 0 = a box of `inertia_box`
  Vec3 inertia_box{1.8, 1.3, 4.3};  // SPEC-CAR-PHYSICS: the mass as a solid box of these sides, m
  // The height above the centre of mass's ground point at which the tyres'
  // longitudinal forces act on the body's pitch, as an arm below the centre
  // of mass (m); <= 0: at the contact patch.
  double longitudinal_pitch_arm = 0;
  // The same for the tyres' lateral forces on the body's roll (SPEC-CAR-
  // PHYSICS, Check: the first corner, load transfer); <= 0: at the contact
  // patch.
  double lateral_roll_arm = 0;
  Vec3 size{1.8, 1.3, 4.3};         // the body contacts' box, m (ours)
  double gravity = 9.8;             // the spec's weight: 15.6 x 9.8
  double airborne_gravity_scale = 1.15, per_tyre_off_gravity_scale = 1.05;
  // Body contacts (ours, until the collision spec gives the car's shape):
  // 26 points on the box from the sill to the roof (corners, edge and face
  // middles), each probed by a ray from the box's centre, are pushed out of
  // whatever surface they pass along its normal by a stiff damped spring
  // with friction. The constants are per corner: the force per point is
  // scaled by 8/26.
  double sill_height = 0.25;      // m above the ground at rest
  double body_stiffness = 150;    // N/m per kg of mass, per corner
  double body_damping = 12;       // N s/m per kg of mass, per corner
  double body_friction = 0.6;

  // Suspension per axle (0 front, 1 rear). The wheel rests where the bake
  // stands it; the spring is preloaded to carry the static load there and
  // reaches its free length `static load / rate` below.
  double rest_length = 0.3;                  // m, hardpoint above the wheel centre (ours)
  std::array<double, 2> spring_rate{60000, 50000};    // N/m
  std::array<double, 2> bump_rate{4000, 3500};        // N s/m, compressing
  std::array<double, 2> rebound_rate{6000, 5000};     // N s/m, extending
  std::array<double, 2> damper_clamp{14000, 14000};   // N, the damper force's limit
  std::array<double, 2> bump_travel{0.08, 0.08};      // m of compression from rest to the bump stop
  std::array<double, 2> max_droop{0.5, 0.5};          // m, extension limit from rest
  std::array<double, 2> bumpstop_rate{280000, 280000};  // N/m
  std::array<double, 2> bumpstop_damping{2800, 2800};   // N s/m
  std::array<double, 2> antiroll_rate{13000, 9000};     // N/m on the compression difference
  std::array<double, 2> antiroll_damping{2800, 2800};   // N s/m
  // The suspension rays start this far above the hardpoints, so a wheel
  // driven into rising ground still finds the surface (ours).
  double probe_height = 1.0;  // m

  // Tyres (SPEC-CAR-PHYSICS, Tyre model).
  TyreCurves longitudinal{synthetic_curve(1.1, 0.15, 0.85), synthetic_curve(1.1, 0.13, 0.85)};
  TyreCurves lateral{synthetic_curve(49.5, 7.5, 0.85), synthetic_curve(49.5, 6.0, 0.85)};
  std::array<double, 2> sidewall_stretch{1.2, 1.2};  // per axle
  double lateral_peak_min_deg = 4, lateral_peak_max_deg = 18;
  std::array<double, 2> long_scale{1, 1}, lat_scale{1, 1};  // the body's friction scales per axle
  // Each curve's row FrictionScale (light-load and heavy-load curve),
  // applied before the load blend.
  std::array<double, 2> long_curve_scale{1, 1}, lat_curve_scale{1, 1};
  // The tyre-width scale per axle (TireFricScale0..1 over TireFricWidth0..1).
  std::array<double, 2> width_scale{1, 1};
  // The compound's lateral scale (TorqueFreeLatFrictionScale).
  double lat_compound_scale = 1;
  // Under drive: accel_scale0 at accel_speed0 m/s to accel_scale1 at accel_speed1, clamped.
  double accel_scale0 = 1.2, accel_scale1 = 1.1, accel_speed0 = 5, accel_speed1 = 15;
  double long_misc_scale = 1.01;   // the spec's unidentified constant, on both forces
  // Rear lateral FixListing multiplier: `rear_fix_scale` up to the
  // normalised slip angle fix_slip0, faded to 1 by fix_slip1, and cancelled
  // as the road wheels' steer angle goes from fix_steer0 to fix_steer1
  // degrees.
  double rear_fix_scale = 1, fix_slip0 = 0.5, fix_slip1 = 1, fix_steer0 = 0.5, fix_steer1 = 1;
  double slip_speed_floor = 1.0;   // m/s, v_min in the slips (ours)
  double wheel_inertia = 1.6;      // kg m^2 per wheel
  // On a wheel whose applied torque slows it (brake, or engine braking
  // through the driveline): TorqueFreeLongFrictionScaleBrake x
  // GameFrictionScaleBraking, in place of the accel blend.
  double brake_friction_scale = 1.8;

  // Engine.
  std::vector<double> torque_curve;     // normalised samples, sentinel removed; empty = synthetic
  double torque_curve_max_rpm = 9000;   // rpm of the curve's last sample slot (n-1 spacing over the full row)
  double torque_curve_slots = 0;        // NumTorqueValues - 1 (the spacing's divisor); 0 = values.size() - 1
  double torque_scale = 380;            // N m at the curve's 1.0
  double engine_braking = 120;          // N m at the redline with the throttle shut (ours, shape)
  double idle_rpm = 800, redline_rpm = 7000;
  double launch_rpm = 4000;             // StartRPM: the launch controller's starting engine speed
  double engine_inertia = 0.2;          // kg m^2, engine + flywheel

  // Drivetrain.
  enum class Layout { kRear, kFront, kAll } layout = Layout::kRear;
  double rear_split = 0.7;  // AWD torque share to the rear
  // AWD centre differential's locking torque (N m at its input, x the
  // final drive at the wheels) while the engine drives / while it brakes.
  double centre_lsd_accel = 0, centre_lsd_decel = 0;
  std::vector<double> gears{3.30, 2.13, 1.55, 1.21, 1.00, 0.82};
  double reverse_gear = 3.4;  // magnitude
  double final_drive = 3.70;
  double gearbox_inertia = 0.014, driveline_inertia = 0.004;  // kg m^2
  double gear_shift_time = 0.16, clutch_in_time = 0.15, clutch_out_time = 0.3;  // s
  double clutch_max_torque = 900;  // N m
  double clutch_torque_factor = 0.72;  // capacity = engagement x ClutchMaxTorque x this (measured fit)
  double upshift_fraction = 0.945;  // of the redline (7,560 on the R34)
  // Braking: down when the lower gear would put the clutch output under
  // this x the redline (6,400 on the R34; SPEC-CAR-PHYSICS, Gearbox).
  double downshift_brake_fraction = 0.8;
  double reverse_select_speed = 6.7056;  // m/s: MaxForwardVelShiftReverseMPH (15 mph)
  // The launch controller (SPEC-CAR-PHYSICS, Clutch and gearbox states).
  double launch_max_output_rpm = 700;  // the clutch output speed under which a start is a launch
  double launch_fall_time = 1.5;       // s for the target to fall from its start to its end
  double launch_gain = 53;             // clutch rate per (rpm / redline)
  double launch_lookahead = 0.04;      // s of engine acceleration in the servo's error
  double launch_exit_slip = 0.7;       // the driven wheels' mean |normalised slip| under which it ends
  double rev_match_time = 0.05;        // s
  // The engine's effective throttle builds like boost: a first-order lag of
  // the drive trigger with this time constant, capped by the pedal (ours, a
  // fit to the spec's 0.3 / 0.6 / 0.83 / 1.0 at 0.1 / 0.25 / 0.5 / 0.9 s).
  // 0 = off.
  double boost_lag = 0.28;

  // Brakes.
  double front_brake_torque = 3500;  // N m per front wheel at full press
  double rear_brake_torque = 3100;   // N m per rear wheel at full press
  double handbrake_torque = 2500;    // N m per rear wheel
  bool abs = true;
  double abs_front_no_steer = 3.0, abs_front_full_steer = 0.5;
  double abs_rear_no_steer = 2.5, abs_rear_full_steer = 2.0;
  double abs_duration = 0.05;  // s
  int abs_smoothing = 5;       // substeps in the slip mean the ABS tests
  // The AI's brakes (SPEC-CAR-PHYSICS, "What the AI's controls mean"):
  // linear torque, ai_brake_scale x the full-press torque per unit of brake
  // (6,970 and 2,850 N m on the R34: 1.59 x 4,390 and 1,790), and one ABS
  // release point for both axles (ReleasePointABS) with no steering blend,
  // releasing for one step.
  double ai_brake_scale = 1.59;
  double abs_ai_release = 0.71;

  // Input shaping (SPEC-CAR-PHYSICS, Throttle, brake and steering).
  double throttle_rate = 5, brake_rate = 20, handbrake_rate = 12;  // full travels per second
  double max_steer = 0.733;          // rad at the road wheels (42 degrees)
  double steer_rate = 1.83;          // rad/s towards the target
  double straighten_rate = 1.83;     // rad/s back towards centre
  double steer_max_gees = 1.0;       // SteerSpeedSensitiveMaxGees
  // The pad's lock at speed: atan(steer_gees_fit x steer_max_gees x g x
  // wheelbase / v^2), the measured 2,410 / v^2 degrees per unit on the R34
  // at 19 to 22 m/s (the rule is open: a placeholder, SPEC_QUESTIONS Q40).
  double steer_gees_fit = 1.61;
  double steer_min_lock = 0.087;     // rad, the narrowest lock
  // At speed the wheels also turn slower: the rate scales with the lock's
  // share of the full lock, down to this share (20 to 38 deg/s measured at
  // 19 to 22 m/s, against 105 at walking pace; placeholder).
  double steer_rate_min_share = 0.25;

  // Resistance: F = drag_quadratic v^2 + drag_constant, against the motion
  // (the spec's fit; the constant fades in over the first 1 m/s, ours).
  double drag_quadratic = 0.21, drag_constant = 240;

  double physics_hz = 359;  // PhysicsSettings.ini CarPhysicsUpdateHz
};

// Raw input, as the player gives it: steer -1 (left) to +1 (right),
// throttle and brake 0 to 1, handbrake on or off.
struct Input {
  double steer = 0, throttle = 0, brake = 0;
  bool handbrake = false;
};

// Per-wheel state for the renderer and the trace.
struct WheelState {
  double compression = 0;   // m above rest (positive: compressed)
  double steer = 0;         // rad, positive towards +x
  double spin = 0;          // rad, rolling forward is positive
  double omega = 0;         // rad/s
  double load = 0;          // N
  double slip_ratio = 0;    // normalised (1 = the curve's peak), signed: negative locking
  double slip_angle = 0;    // normalised, signed
  double brake = 0;         // the brake fraction this wheel got (0 while the ABS releases it)
  double fx = 0;            // N, the tyre's longitudinal force on the body (last substep)
  bool contact = false;
  int surface = -1;         // the ground's surface index under it (-1: none or unknown)
  Vec3 centre{};            // body frame, rest centre moved by the travel
};

class Vehicle {
 public:
  static constexpr double kStep = 1.0 / 120.0;

  explicit Vehicle(const CarParams& params);

  // Places the body frame's origin at `position` (world), level, at rest.
  void reset(const Vec3& position, double heading = 0);
  // As reset, but the clock runs on (a recovery during a run).
  void place(const Vec3& position, double heading = 0);
  // As place, already moving: the body at `speed` along its heading
  // (pitched by `pitch`, nose up positive), the wheels rolling with it, the
  // highest gear that keeps the engine above idle (below the up-shift
  // point) and the clutch in. For a traffic car taken into full physics
  // on the move (SPEC-TRAFFIC, Physics and level of detail; ours).
  void launch(const Vec3& position, double heading, double speed, double pitch = 0);
  // Contacts with other bodies (SPEC-TRAFFIC, Collision with the player;
  // ours): an impulse (N s, world) at a world point, its angular part
  // scaled by `angular_scale`; the velocity of a world point of the body;
  // the inverse of the body's effective mass along unit `n` at the point
  // (the same scale on the angular part); and a shift of the body.
  void apply_impulse(const Vec3& point, const Vec3& impulse, double angular_scale = 1);
  Vec3 point_velocity(const Vec3& point) const;
  double inverse_mass_along(const Vec3& point, const Vec3& n, double angular_scale = 1) const;
  void translate(const Vec3& d) { pos_ = pos_ + d; }
  // Sets the body's linear (centre of mass) and angular velocity (world):
  // the traffic lane hold (SPEC-TRAFFIC, Driving; ours).
  void set_velocity(const Vec3& linear, const Vec3& angular) { vel_ = linear, ang_ = angular; }
  // Advances one fixed step with the given raw input.
  void step(const Input& input, const Ground& ground);
  // The AI's input mode (SPEC-CAR-PHYSICS, "What the AI's controls mean"):
  // gas and brake unshaped, the steer a fraction of SteerMaxAngle at the
  // road wheels (no narrowing, no rate), the brakes linear with the AI's
  // ABS. Off by default (the pad's path).
  void set_ai_controls(bool on) { ai_ = on; }
  // The AI's grip factor (front axle, rear axle) on both tyre forces, seen
  // only on dirt under the AI; the oracle lap replays it from its file.
  // 1, 1 by default and in pad play.
  void set_ai_grip(double front, double rear) { ai_grip_ = {front, rear}; }
  bool ai_controls() const { return ai_; }
  // The AI's grip on dirt (SPEC-CAR-PHYSICS, grip on dirt): while a wheel
  // stands on a surface with off-roadness and the car moves, both tyre
  // forces of every wheel x 1 + 0.5 k (front) and 1 + 0.55 k (rear); k is 1
  // below 10 m/s, else k_yaw (0 under a yaw rate of 0.3 rad/s, 1 over 0.6)
  // x k_time (1 for the first second on dirt, 0.5 from 3 s); the factor a
  // step writes applies from the next. On only under the AI's controls.
  void set_ai_dirt_grip(bool on) { dirt_rule_ = on; }
  double dirt_k() const { return dirt_k_; }
  // A factor on the engine's drive torque (SPEC-RACE-AI: the rubber-band
  // torque cut and the race-start boost); 1 by default.
  void set_torque_factor(double f) { torque_factor_ = f; }
  double torque_factor() const { return torque_factor_; }

  const CarParams& params() const { return p_; }
  Vec3 position() const;  // the body frame's origin (on the ground at rest)
  Vec3 centre_of_mass() const { return pos_; }
  Quat orientation() const { return rot_; }
  Vec3 velocity() const { return vel_; }
  Vec3 angular_velocity() const { return ang_; }
  double speed() const;               // m/s along the body's forward axis
  double rpm() const;                 // engine speed
  int gear() const { return gear_; }  // -1 reverse, 0 neutral, 1..n
  double clutch() const { return clutch_; }  // engagement 0..1 (the spec's c is 1 - this)
  bool clutch_locked() const { return locked_; }  // the clutch is not slipping
  // SPEC-CAR-PHYSICS, Clutch and gearbox states.
  enum class GearState { kOpen, kLaunch, kEngaged, kClutchIn, kGearChange, kRevMatch, kClutchOut };
  GearState gear_state() const { return state_; }
  bool shifting() const {
    return state_ == GearState::kClutchIn || state_ == GearState::kGearChange ||
           (target_gear_ != gear_before_ && (state_ == GearState::kRevMatch || state_ == GearState::kClutchOut));
  }
  double engine_throttle() const { return engine_throttle_; }  // what the engine sees, 0..1
  // The engine's torque (N m) at an rpm and throttle, before the clutch.
  double engine_torque(double rpm, double throttle) const;
  double launch_target() const { return launch_target_; }      // rpm, in the launch state
  // The clutch output's speed (driven-wheel speed x gear x final drive), rpm, signed.
  double clutch_output_rpm() const;
  bool abs_active() const { return abs_flag_; }  // a wheel was released this step
  int substeps() const { return substeps_; }
  const std::array<WheelState, kWheels>& wheels() const { return w_; }
  const Input& shaped_input() const { return shaped_; }
  double steer_angle() const { return steer_angle_; }  // road wheels, rad
  // The road-wheel angle a full stick asks for at the current speed (the
  // lock narrows with speed; see shape_input).
  double steer_lock() const;
  double time() const { return time_; }
  // The body contacts' total force in the last substep (N): 0 while only
  // the tyres touch the ground.
  double body_contact_force() const { return body_force_; }
  // The strongest single body contact over the last step (all substeps):
  // where the ray met the surface, its normal and the Hit's surface and
  // triangle. force 0: none.
  struct BodyContact {
    Vec3 point{}, normal{};
    double force = 0;
    int surface = -1;
    int64_t triangle = -1;
  };
  const BodyContact& strongest_body_contact() const { return strongest_; }
  // The static load the springs are preloaded with, per wheel (N).
  double static_load(int wheel) const { return preload_[wheel]; }

 private:
  void substep(double dt, const Ground& ground);
  void shape_input(const Input& raw, double dt);
  void update_gearbox(double dt);
  double ratio(int gear) const;  // gear x final drive, signed (reverse negative)
  double input_omega() const;    // the gearbox output side's speed in wheel rad/s (diff-weighted)

  CarParams p_;
  Vec3 com_local_{};
  Vec3 inertia_{};
  double preload_[kWheels]{}, droop_[kWheels]{};
  double weight_[kWheels]{};  // the differential's share of each wheel (sums to 1)
  int substeps_ = 3;
  Vec3 pos_{}, vel_{}, ang_{};  // centre of mass (world), its velocity, angular velocity (world)
  Quat rot_{};
  std::array<WheelState, kWheels> w_{};
  double prev_compression_[kWheels]{};
  Input raw_{}, shaped_{};
  double steer_angle_ = 0;
  int gear_ = 1, target_gear_ = 1;
  int gear_before_ = 1;        // the gear a shift started from
  GearState state_ = GearState::kOpen;
  double state_time_ = 0;      // s in the current state
  double clutch_ = 0;          // engagement
  double prev_rpm_ = 0;        // engine rpm at the last step (the launch servo's rate)
  double spool_ = 0;           // the boost lag's state
  double engine_throttle_ = 0;
  double driver_throttle_ = 0;  // the driver's effective drive throttle this step
  double slip_hist_[kWheels][8]{};
  int slip_pos_ = 0;
  double tc_prev_ = 0;
  bool rev_matching_ = false;  // the clutch-out blend asks for the rev-match throttle
  double launch_target_ = 0;   // the launch controller's engine target (rpm)         // the clutch torque in the last substep (N m, engine side)
  bool locked_ = false;        // the clutch is not slipping
  double engine_omega_ = 0;    // rad/s
  double abs_timer_[kWheels]{};
  bool abs_flag_ = false;
  bool ai_ = false;
  double torque_factor_ = 1;
  bool dirt_rule_ = false, any_dirt_ = false, on_dirt_ = false;
  double dirt_k_ = 0, dirt_time_ = 0;
  std::array<double, 2> ai_grip_{1, 1};
  double time_ = 0;
  double body_force_ = 0;
  BodyContact strongest_;
};

// One line of a trace (CSV header and row), for the determinism check.
std::string trace_header();
std::string trace_row(const Vehicle& v);

// SPEC-CAR-PHYSICS, The trace format: one fh1.physics_trace event (JSONL)
// for the player's car, `frame` = round(sim time x clock_hz).
std::string physics_trace_event(const Vehicle& v, int64_t car_id, double clock_hz, int mode = 17);

// A scripted input: piecewise constant from each line's time.
//   # comment
//   <time s> <steer> <throttle> <brake> [handbrake 0/1]
struct ScriptLine {
  double time = 0;
  Input input;
};
bool parse_script(const std::string& text, std::vector<ScriptLine>& out, std::string* error);
Input script_input(const std::vector<ScriptLine>& script, double time);

}  // namespace drive
