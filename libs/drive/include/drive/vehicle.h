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
// A surface's tyre and contact columns (SPEC-CAR-PHYSICS, Tyre model &
// Surface drag; COLLISION.md, Runtime SurfaceType evaluation).
struct SurfaceTyre {
  double rear_grip = 1, off_roadness = 0, off_road_peak_sa = 12;
  double handbrake_grip = 1, tcs_speed_scale = 1;
  double max_steering_lock_cap = 0.0, steering_rate_mult = 1.0, aero_drag_scale = 1.0;
  // Extended SurfaceTypes.xml drag, camera, steering torque & wall-friction properties.
  double linear_drag = 0.0;              // <LinearDrag> surface linear damping rate (1/s)
  double angular_drag = 0.0;             // <AngularDrag> surface angular damping rate (1/s)
  double chase_cam_spring_mult = 1.0;    // <ChaseCamSpringMultiplier> chase camera spring scale
  double steer_torque_scale = 1.0;       // <SteerTorque> surface steering/rumble torque scale
  double car_world_coll_fric_mod = 1.0;  // <CarWorldCollFricMods> body-to-world friction scale
  // Friction/VelDepFriction (default: Asphalt 0..8 m/s, 0.01 long, 0.03 lat).
  double vel_peak0 = 0, vel_peak = 8;
  double vel_long0 = 0, vel_long = 0.01;
  double vel_lat0 = 0, vel_lat = 0.03;
  // Bumpiness (COLLISION.md NoiseType 0..9).
  int bump_noise_type = 0;
  double bump_frequency = 0;
  double bump_clamp_min = -1, bump_clamp_max = 1;
  double bump_scale = 0, bump_offset = 0;
  double bump_rumble = 0;
  // Springiness (soft-ground contact).
  bool is_springy = false;
  double spring_k = 0, spring_comp_d = 0, spring_reb_d = 0;
  double spring_norm_y_cutoff = 0.707, spring_speed_cutoff = 4.0;
  bool is_penalty_surface = false;
  bool is_hand_of_god = false;
};

// Evaluates procedural surface Bumpiness height and horizontal gradient at
// world (x, z) (COLLISION.md, Runtime SurfaceType evaluation).
struct BumpSample {
  double height = 0;
  double dh_dx = 0;
  double dh_dz = 0;
};
BumpSample eval_surface_bumpiness(const SurfaceTyre& s, double x, double z);
struct Hit {
  double distance = 0;
  Vec3 normal{0, 1, 0};
  double grip = 1;
  int surface = -1;  // the ground's surface index, when it has one
  SurfaceTyre tyre;
  int64_t triangle = -1;  // the ground's triangle index, when it has one
};
// Per-surface body-contact friction & damping scale (low-friction wall glide on
// guardrails/concrete vs heavier drag/absorption on dirt banks).
double body_contact_friction(const Hit& hit, const Vec3& normal, double default_friction);
double body_contact_damping_scale(const Hit& hit, const Vec3& normal);
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
  std::array<double, 2> damper_clamp{14000, 14000};          // N, bump damper force limit
  std::array<double, 2> rebound_damper_clamp{14000, 14000};  // N, rebound damper force limit
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
  // Compound / global tyre friction modifier (TireMod_FrictionScale), heating
  // scale (HeatTireScale), and thermal grip curve (List_TireAffectCurve).
  double tire_mod_friction_scale = 1.0;
  double heat_tire_scale = 1.0;
  struct TireAffectCurve {
    double temp_min_c = 20.0;
    double temp_max_c = 180.0;
    std::vector<double> values;  // empty = default thermal window curve
    double evaluate(double temp_c) const;
  };
  TireAffectCurve tire_affect_curve{};
  // Static toe angle per axle in radians (Forza convention: + = toe-out, - = toe-in).
  std::array<double, 2> static_toe_rad{0.0, 0.0};

  // Engine (SPEC-CAR-PHYSICS, Engine torque, forced induction and
  // GameTorqueScale).
  std::vector<double> torque_curve;     // normalised samples, sentinel removed; empty = synthetic
  double torque_curve_max_rpm = 9000;   // rpm of the curve's last sample slot (n-1 spacing over the full row)
  double torque_curve_slots = 0;        // NumTorqueValues - 1 (the spacing's divisor); 0 = values.size() - 1
  double torque_scale = 380;            // N m at the curve's 1.0: TorqueScale x S_upg (T_wot_base)
  // ZeroThrottleTorqueScale, N m: the closed-throttle torque is -1 x this up
  // to max(800 rpm, idle), falling linearly to -2.63 x this at the redline.
  double engine_braking = 50;
  double idle_rpm = 800, redline_rpm = 7000;
  double launch_rpm = 4000;             // StartRPM: the launch controller's starting engine speed
  double engine_inertia = 0.2;          // kg m^2, Data_Engine + flywheel MomentInertia
  // Data_Car.GameTorqueScale (clamped to 0.5..1.5): on the drive torque the
  // clutch passes to the wheels only, not on the engine's own speed.
  double game_torque_scale = 1;
  // Forced induction (SPEC-CAR-PHYSICS, Engine torque, forced induction and
  // GameTorqueScale). Turbo: turbine power lag + 15 Hz boost pressure filter.
  // Supercharger (DSC/CSC): RPM-linear boost through the same 15 Hz filter.
  // Neither set (naturally aspirated): zero boost lag, f_total = 1.
  bool has_turbo = false;
  double turbo_tau = 0.19;                                  // s, 0.01 x MomentInertia
  double turbo_power_min = 7457, turbo_power_max = 90975;   // W, 745.7 x PowerMin/MaxScale
  double boost_min_psi = 3.675;                             // 0.25 atm (BOV dump pressure)
  double boost_max_psi = 27.93;                             // 14.7 x (2 x (MaxScale - 1) + 1)
  double boost_scale_min = 1, boost_scale_max = 1;          // MinScale_eff, MaxScale_eff
  double turbo_dropoff_rpm0 = 0, turbo_dropoff_rpm1 = 0;    // TorqueDropOffRPM0/1 (equal: none)
  double turbo_dropoff_scale0 = 1, turbo_dropoff_scale1 = 1;
  bool has_supercharger = false;
  double sc_zero_psi = 14.7, sc_redline_psi = 14.7;         // 14.7 x (2 x (Scale - 1) + 1)
  double sc_scale_zero = 1, sc_scale_redline = 1;           // ZeroRPMScale_eff, RedlineRPMScale_eff
  double sc_dropoff_rpm0 = 0, sc_dropoff_rpm1 = 0;
  double sc_dropoff_scale0 = 1, sc_dropoff_scale1 = 1;

  // Drivetrain.
  enum class Layout { kRear, kFront, kAll } layout = Layout::kRear;
  double rear_split = 0.7;  // AWD torque share to the rear
  // Limited-slip differential (SPEC-CAR-PHYSICS, AWD and LSDs):
  // T_lock = clamp(dw / rel_vel_clamp, -1, 1) * lerp(decel, accel, clamp(T_in / accel_def_in, 0, 1)).
  struct LsdParams {
    double accel = 0;           // N m, LimitedSlipTorqueAccel
    double decel = 0;           // N m, LimitedSlipTorqueDecel
    double rel_vel_clamp = 50;  // rad/s, LimitedSlipRelVelClamp
    double accel_def_in = 50;   // N m, LimitedSlipAccelDefInputTorque
  };
  LsdParams front_lsd{}, rear_lsd{}, center_lsd{};
  std::vector<double> gears{3.30, 2.13, 1.55, 1.21, 1.00, 0.82};
  double reverse_gear = 3.4;  // magnitude
  double final_drive = 3.70;
  double gearbox_inertia = 0.014, driveline_inertia = 0.004;  // kg m^2
  double gear_shift_time = 0.16, clutch_in_time = 0.15, clutch_out_time = 0.3;  // s
  double clutch_max_torque = 900;  // N m
  double clutch_torque_factor = 1.0;  // capacity = engagement x ClutchMaxTorque x this
  double upshift_fraction = 0.945;  // of the redline (7,560 on the R34)
  // Braking: down when the lower gear would put the clutch output under
  // this x the redline (6,400 on the R34; SPEC-CAR-PHYSICS, Gearbox).
  double downshift_brake_fraction = 0.8;
  double reverse_select_speed = 6.7056;      // m/s: MaxForwardVelShiftReverseMPH (15 mph)
  double reverse_auto_enter_speed = 6.7056;  // m/s: MaxForwardSpeedMphToAutoEnterReverse
  double shift_reverse_pause = 0.0;          // s: ShiftReversePause
  // The launch controller (SPEC-CAR-PHYSICS, Clutch and gearbox states).
  double launch_max_output_rpm = 700;  // the clutch output speed under which a start is a launch
  double launch_fall_time = 1.5;       // s for the target to fall from its start to its end
  double launch_gain = 53;             // clutch rate per (rpm / target_rpm)
  double launch_lookahead = 0.04;      // s of engine acceleration in the servo's error
  double launch_exit_slip = 0.7;       // the driven wheels' mean |normalised slip| under which it ends
  double rev_match_time = 0.05;        // s
  // Legacy synthetic throttle lag (0 = disabled; real cars use has_turbo/has_supercharger).
  double boost_lag = 0.0;

  // Brakes (SPEC-CAR-PHYSICS, Brakes). When use_per_car_brakes is true (set by
  // from_physics.cpp), Vehicle::init computes per-axle brake_const, rear
  // handbrake_const, and the 10-point speed-vs-deceleration table from the
  // car's actual tyres, mass, geometry, aero downforce, and brake sliders.
  // When false (synthetic test car), uses front/rear/handbrake_torque directly.
  bool use_per_car_brakes = false;
  double brake_torque_slider = 0.5;   // List_UpgradeBrakes.BrakeTorqueSlider [0, 1]
  double brake_bias_slider = 0.5;     // List_UpgradeBrakes.BrakeBiasSlider [0, 1]
  double brake_bias_offset_rear = 0.03;  // PhysicsSettings BrakeBiasOffsetRear
  double brake_pressure_min_share = 0.25;  // BrakePressureMaxPercentFrontOrRear
  double bias_handbrake = 1.5;        // List_UpgradeBrakes.BiasHandbrake [0, 5.5]
  double brake_input_in[4] = {0, 0.001, 0.90, 1.0};
  double brake_input_out[4] = {0, 0.12, 0.70, 0.81};
  double front_brake_torque = 3500;  // N m per front wheel (fallback / override when > 0 & !use_per_car_brakes)
  double rear_brake_torque = 3100;   // N m per rear wheel
  double handbrake_torque = 2500;    // N m per rear wheel
  bool abs = true;
  double abs_front_no_steer = 3.0, abs_front_full_steer = 0.5;
  double abs_rear_no_steer = 2.5, abs_rear_full_steer = 2.0;
  double abs_duration = 0.05;  // s
  int abs_smoothing = 5;       // substeps in the slip mean the ABS tests
  double ai_brake_scale = 1.59;
  double abs_ai_release = 0.71;
  // No-ABS regulator (SPEC-CAR-PHYSICS, ABS and No-ABS).
  double no_abs_brake_travel = 0.975;
  double no_abs_disable_time = 0.0;
  double no_abs_front_no_steer = 1.0, no_abs_front_full_steer = 0.71;
  double no_abs_rear_no_steer = 1.0, no_abs_rear_full_steer = 0.71;
  double abs_off_braking_friction_scale = 1.05;

  // TCS and STM (SPEC-CAR-PHYSICS, Traction control & Stability control).
  double tcs_speed_mph = 30.0;
  double tcs_slip_takeoff = 1.0, tcs_slip_moving = 0.85;
  double tcs_steer_scale_rwd = 1.1, tcs_steer_scale_fwd = 1.2, tcs_steer_scale_awd = 2.0;
  double tcs_fric_diff = 0.25;
  double tcs_yaw_min_deg = 20.0, tcs_yaw_max_deg = 50.0;
  double stm_speed_mph = 30.0;
  double stm_slip_min = 1.2, stm_slip_max = 2.5;

  // Manual + Clutch (SPEC-CAR-PHYSICS, Manual with Clutch).
  double clutch_pedal_dead = 0.05;
  double clutch_torque_exp = 1.5;
  double clutch_max_vel = 20.0;
  double clutch_vel_no_clutch = 2.5;
  double clutch_stall_timeout = 0.5;
  double shift_without_clutch_damage = 0.05;

  // Input shaping and steering (SPEC-CAR-PHYSICS, Throttle, brake and steering;
  // Assisted / Normal / Simulation steering).
  double throttle_rate = 5, brake_rate = 20, handbrake_rate = 12;  // full travels per second
  double max_steer = 0.733;          // rad at the road wheels (42 degrees)
  double max_steer_filtered = 0.733; // Data_Car.SteerMaxAngleFiltered (rad)
  double steer_rate = 1.83;          // rad/s towards the target
  double straighten_rate = 1.83;     // rad/s back towards centre
  double steer_countersteer_rate = 1.83;  // rad/s (SteerAngVelCountersteer)
  double steer_dyn_find_peak_rate = 1.83; // rad/s (Data_Car.SteerAngVelDynFindPeak)
  double steer_accel_time = 0.08;         // s (Data_Car.SteerAccelTimeToMaxRate)
  double steer_max_gees = 1.0;            // SteerSpeedSensitiveMaxGees
  double steer_gees_fit = 1.61;
  double steer_min_lock = 0.087;          // rad, the narrowest lock
  double steer_slow_speed = 13.4112;      // m/s (Data_Car.SteerSpeedSensitiveSlowSpeed, 30 mph)
  double steer_fast_speed = 67.056;       // m/s (Data_Car.SteerSpeedSensitiveFastSpeed, 150 mph)
  double steer_fast_rate_scale = 0.45;    // Data_Car.SteerSpeedSensitiveFastRateScale
  double steer_rate_min_share = 0.25;
  // SteeringWheelFilter (PhysicsSettings.ini): default (Assisted/Normal) and
  // corrected (Simulation) speed-dependent stick/wheel shaping ramps.
  double sw_filter_speed0 = 10.0, sw_filter_scale0 = 1.5;
  double sw_filter_speed1 = 50.0, sw_filter_scale1 = 3.0;
  double sw_filter_corr_speed0 = 5.0, sw_filter_corr_scale0 = 1.0;
  double sw_filter_corr_speed1 = 25.0, sw_filter_corr_scale1 = 2.5;
  // RWD Launch Assist (PhysicsSettings.ini EnableRWDLaunchAssist).
  bool enable_rwd_launch_assist = true;

  // Aerodynamics (SPEC-CAR-PHYSICS, Aerodynamics): all force coefficients in
  // N/(m/s)^2 (DB kgf @ 150 mph x K_aero, K_aero = 9.80665 / 67.056^2).
  // When aero_drag_long > 0 the 3D body + wing aero model runs; when 0
  // (synthetic test car), falls back to drag_quadratic / drag_constant.
  double aero_drag_long = 0;       // BodyAeroLongitudinalDrag x K_aero x GameDragScale x part drag scales
  double aero_drag_lat_front = 0;  // BodyAeroLateralDragFront x K_aero
  double aero_drag_lat_rear = 0;   // BodyAeroLateralDragRear x K_aero
  double aero_drag_vert = 0;       // BodyAeroVerticalDrag x K_aero
  double aero_df_front = 0;        // (BodyAeroForwardDownforceFront + RearBumper delta) x K_aero
  double aero_df_rear = 0;         // (BodyAeroForwardDownforceRear + RearBumper delta) x K_aero
  double aero_zero_df_rad = 0.7853981633974483;  // BodyAeroAngleZeroDownforce (rad, default 45 deg)
  double aero_part_drag_front = 0, aero_part_df_front = 0;  // FrontBumper List_AeroPhysics
  double aero_part_drag_rear = 0, aero_part_df_rear = 0;    // RearWing List_AeroPhysics
  double aero_part_lat_drag = 0;                            // FrontBumper + RearWing LateralDrag x K_aero
  double high_speed_drag_v0 = 119.3, high_speed_drag_v1 = 122.9;  // m/s
  double high_speed_drag_s0 = 1.0, high_speed_drag_s1 = 1.5;

  // Fallback resistance for synthetic CarParams (when aero_drag_long == 0);
  // on real cars drag_quadratic holds the total forward drag coeff (for AI
  // speed planner) and drag_constant is 0 (rolling resistance is VelDepFriction).
  double drag_quadratic = 0.21, drag_constant = 240;

  // Surface drag slew rate (VelDepFriction MaxVelDepCoeffVel) and world
  // floor/water Y threshold for Hand-of-God respawn (COLLISION.md).
  double max_vel_dep_coeff_vel = 5.0;  // 1/s
  double water_floor_y = -46.0;        // m (COLLISION.md, global floor/water Y)

  double physics_hz = 359;  // PhysicsSettings.ini CarPhysicsUpdateHz
};

// Aerodynamic drafting scales applied to a vehicle (SPEC-CAR-PHYSICS,
// Aerodynamic drafting). All multipliers default to 1.0 (no drafting).
struct DraftState {
  double strength = 0;      // d in [0, 1], max over leading cars
  double body_drag = 1;
  double front_drag = 1;
  double rear_drag = 1;
  double front_df = 1;
  double rear_df = 1;
  double score = 0;         // [0, 1000] drafting score (|F_downforce_saved|)
};

// Evaluates drafting multipliers for a follower behind one or more leaders
// (SPEC-CAR-PHYSICS, Aerodynamic drafting, 40-150 mph window).
struct DraftCar {
  Vec3 position{};
  Vec3 forward{0, 0, 1};
  double speed = 0;
  double width = 1.9;
  double front_df_coeff = 0;  // N/(m/s)^2 (for drafting score)
  double rear_df_coeff = 0;
};
DraftState compute_follower_draft(const DraftCar& follower, const DraftCar* leaders, size_t leader_count);
DraftState compute_leader_draft(double strength, double speed);

// Raw input, as the player gives it: steer -1 (left) to +1 (right),
// throttle and brake 0 to 1, handbrake on or off, optional manual clutch
// pedal (0 released .. 1 pressed) and shift request (+1 up, -1 down, 0 none).
struct Input {
  double steer = 0, throttle = 0, brake = 0;
  bool handbrake = false;
  double clutch = 0;
  int shift = 0;
  bool linear_steer = false;  // true for 900-deg steering wheel: bypass gamepad speed narrowing
  // Optional road/lane guidance for Assisted steering mode (assists_.steering == 0):
  // when has_assist_lane is true, Assisted steering adds a gentle centering bias
  // toward the lane (assist_heading_error_rad: signed angle to road tangent,
  // positive right; assist_lateral_offset_m: signed distance right of lane center).
  bool has_assist_lane = false;
  double assist_heading_error_rad = 0.0;
  double assist_lateral_offset_m = 0.0;
  // Optional target speed (m/s) for Auto-Brake assist (assists_.braking == 0),
  // usable in both races and free roam whenever provided.
  bool has_auto_brake_target = false;
  double auto_brake_target_speed = 0.0;
};

// Runtime driving assists matching ForzaProfile.sch enums
// (SPEC-CAR-PHYSICS, Assists and input shaping).
struct VehicleAssists {
  int braking = 1;            // 0 Assisted (Auto-Brake), 1 ABS On, 2 ABS Off
  int steering = 1;           // 0 Assisted, 1 Normal, 2 Simulation
  bool traction_control = false;
  bool stability_control = false;
  int shifting = 0;           // 0 Automatic, 1 Manual, 2 Manual + Clutch
};

// Auto-Brake upstream helper (Braking == 0 / Assisted, SPEC-CAR-PHYSICS):
// injects brake demand and cuts throttle when current_speed > target_speed.
void apply_auto_brake(Input& in, double current_speed, double target_speed);
// Computes a safe corner target speed (m/s) from signed road curvature (1/m)
// for Auto-Brake assist in any mode (free roam or race).
double auto_brake_target_for_curvature(double curvature_inv_m, double lateral_grip_g = 0.85, double max_speed = 90.0);

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
  // The drawn state between two steps (ours, rendering): the body's pose
  // and velocities and the wheels. A window takes one before each step and
  // draws blend(before, now, alpha), alpha the frame's fraction of a step
  // past the last one, so the car and the camera move every frame by the
  // frame's own time. set_pose puts a pose back (after the draw).
  struct Pose {
    Vec3 pos{}, vel{}, ang{};
    Quat rot{};
    std::array<WheelState, 4> wheels{};
  };
  Pose pose() const { return {pos_, vel_, ang_, rot_, w_}; }
  void set_pose(const Pose& p) { pos_ = p.pos, vel_ = p.vel, ang_ = p.ang, rot_ = p.rot, w_ = p.wheels; }
  // a at t = 0, b at t = 1; b alone when the body jumped (over 5 m: a reset).
  static Pose blend(const Pose& a, const Pose& b, double t);
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
  // Aerodynamic drafting multipliers for the next step (SPEC-CAR-PHYSICS).
  void set_draft(const DraftState& d) { draft_ = d; }
  const DraftState& draft() const { return draft_; }
  // Runtime driving assists & transmission mode (SPEC-CAR-PHYSICS, Assists).
  void set_assists(const VehicleAssists& a) { assists_ = a; }
  const VehicleAssists& assists() const { return assists_; }
  // Runtime Simulation-mode mechanical damage multipliers (all 1.0 / 0.0 by default).
  struct DamageMods {
    double engine_torque = 1.0;
    double shift_time_add = 0.0;
    double brake_power = 1.0;
    double aero_drag = 1.0;
    double aero_downforce = 1.0;
    std::array<double, 4> tyre_grip{1.0, 1.0, 1.0, 1.0};
    std::array<double, 4> suspension_scale{1.0, 1.0, 1.0, 1.0};
    double steer_pull_rad = 0.0;
  };
  void set_damage_mods(const DamageMods& d) { damage_ = d; }
  const DamageMods& damage_mods() const { return damage_; }

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
  // The engine's torque (N m) at an rpm and throttle, before the clutch (uses current boost).
  double engine_torque(double rpm, double throttle) const;
  // Current boost pressure (psi) and combined boost multiplier (1.0 on NA cars).
  double boost_psi() const { return p_.has_turbo ? boost_psi_ : (p_.has_supercharger ? sc_psi_ : 0.0); }
  bool bov_open() const { return bov_open_; }
  double boost_factor() const { return boost_factor_; }
  double launch_target() const { return launch_target_; }      // rpm, in the launch state
  // The clutch output's speed (driven-wheel speed x gear x final drive), rpm, signed.
  double clutch_output_rpm() const;
  bool abs_active() const { return abs_flag_; }  // a wheel was released this step
  bool tcs_active() const { return tcs_flag_; }  // TCS intervention >= 0.1 this step
  bool stm_active() const { return stm_flag_; }  // STM intervention active this step
  double tcs_amount() const { return tcs_amount_; }
  double stm_amount() const { return stm_amount_; }
  double transmission_damage() const { return trans_damage_; }  // [0, 1] from shifts without clutch
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
  // Surface bumpiness rumble: max across grounded wheels of 2 * Bumpiness.Rumble
  // (COLLISION.md, Runtime SurfaceType evaluation; consumed by ControllerRumble).
  double surface_bump_rumble() const { return surface_bump_rumble_; }
  double surface_chase_cam_spring_mult() const { return surf_chase_cam_spring_mult_; }
  double surface_steer_torque_scale() const { return surf_steer_torque_scale_; }
  double tyre_temp_c(int wheel) const {
    return (wheel >= 0 && wheel < kWheels) ? tyre_temp_c_[size_t(wheel)] : 80.0;
  }
  // True if a grounded wheel touched an IsHandOfGodTriggerSurface or fell
  // below water_floor_y (COLLISION.md); caller clears after respawning.
  bool hand_of_god_triggered() const { return hand_of_god_; }
  void clear_hand_of_god() { hand_of_god_ = false; }
  void set_water_floor_y(double y) { p_.water_floor_y = y; }
  // Computed per-car brake constants (trace units) and 10-point normalized
  // speed-vs-deceleration table (0..200 mph, SPEC-CAR-PHYSICS, Brakes).
  double brake_const(int axle) const { return brake_const_[axle]; }
  double handbrake_const() const { return handbrake_const_; }
  double brake_max_decel() const { return brake_a_max_; }  // m/s^2
  const std::array<double, 10>& decel_table() const { return decel_table_; }
  double decel_table_at(double speed_ms) const;
  // The static load the springs are preloaded with, per wheel (N).
  double static_load(int wheel) const { return preload_[wheel]; }
  // Body-frame signed lateral acceleration in g (+X right in a right turn).
  double lateral_g() const { return lateral_g_; }
  // World-space Y component of the body's +Y up vector (< 0.5 indicates rollover).
  double up_y() const { return rotate(rot_, {0, 1, 0}).y; }
  bool is_rolled_over() const { return up_y() < 0.5; }

 private:
  void substep(double dt, const Ground& ground);
  void shape_input(const Input& raw, double dt);
  void update_gearbox(double dt);
  void update_boost(double dt, double throttle, double rpm_now);
  void init_brakes();
  double ratio(int gear) const;  // gear x final drive, signed (reverse negative)
  double input_omega() const;    // the gearbox output side's speed in wheel rad/s (diff-weighted)

  CarParams p_;
  Vec3 com_local_{};
  Vec3 inertia_{};
  double preload_[kWheels]{}, droop_[kWheels]{};
  double weight_[kWheels]{};  // the differential's share of each wheel (sums to 1)
  double peak_power_rpm_ = 6500;
  double brake_const_[2]{0, 0};
  double handbrake_const_ = 0;
  double brake_a_max_ = 10.0;
  std::array<double, 10> decel_table_{1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  double handbrake_shaped_ = 0;
  int substeps_ = 3;
  Vec3 pos_{}, vel_{}, ang_{};  // centre of mass (world), its velocity, angular velocity (world)
  Quat rot_{};
  std::array<WheelState, kWheels> w_{};
  double prev_compression_[kWheels]{};
  Input raw_{}, shaped_{};
  double steer_angle_ = 0;
  double steer_ang_vel_ = 0;
  int gear_ = 1, target_gear_ = 1;
  int gear_before_ = 1;        // the gear a shift started from
  GearState state_ = GearState::kOpen;
  double state_time_ = 0;      // s in the current state
  double reverse_hold_time_ = 0;  // s brake held at low speed for reverse entry
  bool pedal_swap_ = false;    // reverse pedal swap active (SPEC-CAR-PHYSICS)
  double clutch_ = 0;          // engagement
  double prev_rpm_ = 0;        // engine rpm at the last step (the launch servo's rate)
  double spool_ = 0;           // legacy boost lag state (used only if boost_lag > 0)
  double turbo_power_ = 0;     // W, turbine power lag ODE state
  double boost_psi_ = 3.675;   // psi, 15 Hz filtered turbo boost pressure
  double sc_psi_ = 3.675;      // psi, 15 Hz filtered supercharger boost pressure
  bool bov_open_ = true;       // blow-off valve hysteresis state
  double boost_factor_ = 1.0;  // combined forced-induction multiplier f_total
  double engine_torque_prev_ = 0;  // N m, previous substep T_engine for P_engine
  double engine_throttle_ = 0;
  double driver_throttle_ = 0;  // the driver's effective drive throttle this step
  double slip_hist_[kWheels][8]{};
  int slip_pos_ = 0;
  bool rev_matching_ = false;  // the clutch-out blend asks for the rev-match throttle
  double launch_target_ = 0;   // the launch controller's engine target (rpm)
  bool locked_ = false;        // the clutch is not slipping
  double engine_omega_ = 0;    // rad/s
  double abs_timer_[kWheels]{};
  double abs_scale_[kWheels]{1, 1, 1, 1};
  double vel_dep_long_[kWheels]{};
  double vel_dep_lat_[kWheels]{};
  double spring_defl_[kWheels]{};
  double spring_ring_[kWheels][3]{};
  int spring_ring_pos_ = 0;
  double surface_bump_rumble_ = 0;
  double surf_steer_lock_cap_rad_ = 1e9;
  double surf_steer_rate_mult_ = 1.0;
  double surf_chase_cam_spring_mult_ = 1.0;
  double surf_steer_torque_scale_ = 1.0;
  double tyre_temp_c_[kWheels]{80.0, 80.0, 80.0, 80.0};
  bool hand_of_god_ = false;
  bool abs_flag_ = false;
  bool tcs_flag_ = false;
  bool stm_flag_ = false;
  double tcs_amount_ = 0;
  double stm_amount_ = 0;
  bool shifted_without_clutch_ = false;
  double manual_shift_dwell_ = 0;
  double trans_damage_ = 0;
  double stall_timer_ = 0;
  VehicleAssists assists_{};
  bool ai_ = false;
  double torque_factor_ = 1;
  bool dirt_rule_ = false, any_dirt_ = false, on_dirt_ = false;
  double dirt_k_ = 0, dirt_time_ = 0;
  std::array<double, 2> ai_grip_{1, 1};
  double time_ = 0;
  DraftState draft_{};
  DamageMods damage_{};
  double body_force_ = 0;
  double lateral_g_ = 0;
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
