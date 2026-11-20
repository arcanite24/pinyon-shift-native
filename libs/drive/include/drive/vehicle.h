// The engine's own car physics for M3 (REIMPLEMENTATION_PLAN M3).
//
// One rigid body with four raycast wheels, stepped at a fixed 1/120 s (the
// step RI-0.5 found deterministic in the oracle) with substeps. Everything
// here is our own placeholder model: the parameters and their defaults are
// the engine's choices until CAR_PHYSICS.md (FORMAT-CAR-PHYSICS, being
// written) gives the game's, and the wheel geometry comes from the baked
// car (FORMAT-CARS, Wheels).
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
// surface normal and a grip factor (1 for tarmac until COLLISION.md gives
// surfaces).
struct Hit {
  double distance = 0;
  Vec3 normal{0, 1, 0};
  double grip = 1;
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

// Wheels in the order LF, RF, LR, RR.
inline constexpr int kWheels = 4;

// Everything the model needs about one car. Geometry is filled from the
// bake (from_bake.h); the rest are the engine's placeholders, to be
// replaced from CAR_PHYSICS.md.
struct CarParams {
  // Geometry (body frame, at rest on flat ground).
  std::array<Vec3, kWheels> wheel_centres{};  // y = the tyre's outer radius
  std::array<double, kWheels> tyre_radius{};  // outer radius, m
  double wheel_inertia = 1.2;                 // kg m^2 per wheel (placeholder)

  // Body (placeholders).
  double mass = 1400;   // kg
  Vec3 centre_of_mass{0, 0.5, 0};  // body frame; z set to mid-wheelbase by from_bake
  Vec3 inertia{0, 0, 0};           // principal moments about x, y, z; 0 = from mass and size
  Vec3 size{1.8, 1.3, 4.3};        // box for the default inertia, m
  // Body contacts (ours, a stand-in until COLLISION.md): the box's eight
  // corners, from the sill to the roof, are pushed out of the ground along
  // world up by a stiff damped spring with friction, so a car that rolls
  // or lands on its body stays on the surface.
  double sill_height = 0.25;      // m above the ground at rest
  double body_stiffness = 150;    // N/m per kg of mass, per corner
  double body_damping = 12;       // N s/m per kg of mass, per corner
  double body_friction = 0.6;

  // Suspension (placeholders): the wheel hangs `rest_length` below its
  // hardpoint at rest; the spring is preloaded so the car rests exactly
  // there on flat ground.
  double rest_length = 0.3;   // m
  double bump_travel = 0.12;  // m of compression available from rest
  double droop_travel = 0.15; // m of extension available from rest
  // The suspension rays start this far above the hardpoints, so a wheel
  // driven into rising ground (or below a step) still finds the surface
  // and the bump stop lifts it, instead of the ray starting underground.
  // Ours, for the stand-in ground. A wheel whose suspension points less
  // than ~78 degrees from straight down finds no ground (rolled over).
  double probe_height = 1.0;  // m
  double frequency = 1.6;     // Hz, sets the spring rate per corner
  double damping_ratio = 0.35;

  // Tyres (placeholder: linear to a peak, then flat, on a friction circle).
  double friction = 1.1;           // peak force / load
  double peak_slip_ratio = 0.10;
  double peak_slip_angle = 0.14;   // rad
  double rolling_resistance = 0.012;

  // Drivetrain (placeholders).
  enum class Layout { kRear, kFront, kAll } layout = Layout::kRear;
  double peak_torque = 420;       // N m, flat from 1,500 rpm to 6,000 rpm
  double idle_rpm = 900, redline_rpm = 7000;
  std::vector<double> gears{3.30, 2.13, 1.55, 1.21, 1.00, 0.82};
  double reverse_gear = 3.4;
  double final_drive = 3.70;
  double drivetrain_efficiency = 0.88;
  double shift_up_rpm = 6600, shift_down_rpm = 2600;
  double shift_time = 0.25;  // s without drive while shifting

  // Brakes (placeholders).
  double brake_torque = 3200;      // N m per front wheel at full brake
  double brake_rear_share = 0.65;  // rear torque / front torque
  double handbrake_torque = 2500;  // N m per rear wheel

  // Steering and aero (placeholders).
  double max_steer = 0.61;          // rad at the front wheels (35 degrees)
  double steer_speed_falloff = 25;  // m/s at which lock halves
  double drag_area = 0.70;          // Cd x A, m^2
  double air_density = 1.225;
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
  bool contact = false;
  Vec3 centre{};            // body frame, rest centre moved by the travel
};

class Vehicle {
 public:
  static constexpr double kStep = 1.0 / 120.0;
  static constexpr int kSubsteps = 10;

  explicit Vehicle(const CarParams& params);

  // Places the body frame's origin at `position` (world), level, at rest.
  void reset(const Vec3& position, double heading = 0);
  // As reset, but the clock runs on (a recovery during a run).
  void place(const Vec3& position, double heading = 0);
  // Advances one fixed step with the given raw input.
  void step(const Input& input, const Ground& ground);

  const CarParams& params() const { return p_; }
  Vec3 position() const;  // the body frame's origin (on the ground at rest)
  Quat orientation() const { return rot_; }
  Vec3 velocity() const { return vel_; }
  Vec3 angular_velocity() const { return ang_; }
  double speed() const;               // m/s along the body's forward axis
  double rpm() const { return rpm_; }
  int gear() const { return gear_; }  // -1 reverse, 0 neutral, 1..n
  const std::array<WheelState, kWheels>& wheels() const { return w_; }
  const Input& shaped_input() const { return shaped_; }
  double time() const { return time_; }
  // The body contacts' total force in the last substep (N): 0 while only
  // the tyres touch the ground.
  double body_contact_force() const { return body_force_; }

 private:
  void substep(double dt, const Ground& ground);
  void shape_input(const Input& raw, double dt);
  void update_gearbox(double dt);
  double engine_torque(double rpm) const;

  CarParams p_;
  Vec3 com_local_{};
  Vec3 inertia_{};
  double spring_rate_[kWheels]{}, damping_[kWheels]{}, preload_[kWheels]{};
  Vec3 pos_{}, vel_{}, ang_{};  // centre of mass (world), its velocity, angular velocity (world)
  Quat rot_{};
  std::array<WheelState, kWheels> w_{};
  Input raw_{}, shaped_{};
  int gear_ = 1;
  double shift_timer_ = 0, rpm_ = 0, time_ = 0;
  double body_force_ = 0;
};

// One line of a trace (CSV header and row), for the determinism check and
// later for tools/physics-behaviour.py once its format is specified.
std::string trace_header();
std::string trace_row(const Vehicle& v);

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
