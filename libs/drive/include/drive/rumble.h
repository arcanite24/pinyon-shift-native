// Dual-motor gamepad rumble driven by ControllerRumble.xml (v4's
// controllerrumble.xml when installed). Pure state and arithmetic: the
// caller feeds per-frame vehicle telemetry and gets clamped [0, 1]
// LeftMotor (low-frequency) and RightMotor (high-frequency) outputs for
// SDL_RumbleGamepad.
// Spec: SPEC-CAMERAS, Gamepad rumble (ControllerRumble.xml).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace drive {

// Three-point piecewise-linear response curve (x0, y0) .. (x2, y2).
// Clamped flat beyond [x0, x2].
struct RumbleCurve {
  double x[3]{0, 0, 0};
  double y[3]{0, 0, 0};

  double evaluate(double input) const;
};

struct RumbleMotorPair {
  RumbleCurve left;
  RumbleCurve right;
};

struct RumbleEventPair {
  double left = 0;
  double right = 0;
};

// Parsed <Controller> (or <Wheel>) section of ControllerRumble.xml.
struct RumbleConfig {
  RumbleMotorPair engine_norm_rpm;
  RumbleMotorPair transmission_rpm;
  RumbleMotorPair car_to_car_collision;
  RumbleMotorPair car_to_car_collision_time;
  RumbleMotorPair car_to_world_collision;
  RumbleMotorPair car_to_world_collision_time;
  RumbleMotorPair car_to_object_collision;
  RumbleMotorPair car_to_object_collision_time;
  RumbleMotorPair tire_traction;
  RumbleMotorPair tire_traction_front;
  RumbleMotorPair tire_traction_rear;
  RumbleMotorPair air_friction;
  RumbleMotorPair surface_bumpiness;
  RumbleEventPair on_shift;
  RumbleEventPair on_engine_sputter;
  RumbleEventPair on_backfire;
  double wheel_steer_torque_scale = 1.0;  // <Wheel><SteerTorque> scaling
  bool is_wheel = false;
  bool loaded = false;
};

// Parses ControllerRumble.xml (base or v4). Handles the retail v4 malformed
// float quirk (two decimal points, parsed up to the second point).
bool parse_controller_rumble(std::string_view xml, RumbleConfig& out, std::string* error, bool wheel = false);

// Converts a body-to-world contact force (N) over a physics step into
// ControllerRumble.xml CarToWorldCollision input units (aligned with
// LeftMotor x0 = 20.0, x1 = 70.0, x2 = 350.0 rather than / 35.0 attenuation).
double world_collision_rumble_input(double contact_force_n, double step_dt = 1.0 / 120.0, double mass_kg = 1400.0);

// True when a gear transition should trigger OnShift rumble (suppresses
// transitions into Neutral 0 or Reverse -1).
bool should_trigger_shift_rumble(int prev_gear, int new_gear);

struct RumbleInput {
  double engine_norm_rpm = 0;      // (RPM - idle) / (limiter - idle)
  double transmission_rpm = 0;
  double tire_traction = 0;        // overall load-weighted slip/traction
  double tire_traction_front = 0;  // front axle load-weighted slip/traction
  double tire_traction_rear = 0;   // rear axle load-weighted slip/traction
  double air_friction = 0;
  double surface_bumpiness = 0;    // max across 4 wheels of 2 * bumpiness.rumble * (speed / 22.352)
  // One-shot collision impulse magnitudes (cleared by the caller after reading).
  double car_to_car_impulse = 0;
  double car_to_world_impulse = 0;
  double car_to_object_impulse = 0;
  // Optional raw body-to-world contact force (N) and mass (kg): when
  // car_to_world_impulse == 0 and car_to_world_force_n > 0, converted via
  // world_collision_rumble_input().
  double car_to_world_force_n = 0;
  double car_mass_kg = 1400.0;
  // Event flags.
  bool on_shift = false;
  bool on_engine_sputter = false;
  bool on_backfire = false;
  // Current gear (-1 reverse, 0 neutral, 1..n forward; default 1 for legacy
  // callers). Shifts into Neutral (0) or Reverse (-1) suppress OnShift rumble.
  int gear = 1;
  // Surface <SteerTorque> multiplier (SurfaceTypes.xml, default 1.0).
  double surface_steer_torque_scale = 1.0;
  // Gating: paused/UI menus mute to 0; enabled is the profile vibration toggle.
  bool paused = false;
  bool enabled = true;
  double scale = 1.0;  // ControllerAdvancedOptions/RumbleScale, [0, 1]
  // Optional vehicle speed (m/s) for stationary gating (< 0 means unspecified
  // for legacy callers/tests) and smooth attack/decay envelope filtering.
  double speed_mps = -1.0;
  bool smooth = false;
};

struct RumbleOutput {
  double left = 0;   // low-frequency heavy rotor, [0, 1]
  double right = 0;  // high-frequency light rotor, [0, 1]

  uint16_t left_u16() const;
  uint16_t right_u16() const;
};

class GamepadRumble {
 public:
  void set_config(const RumbleConfig& cfg) { cfg_ = cfg; }
  const RumbleConfig& config() const { return cfg_; }
  void reset();
  RumbleOutput update(double dt, const RumbleInput& in);

  // Brief hold duration for one-shot shift/sputter/backfire flags (seconds).
  static constexpr double kEventHold = 0.08;

 private:
  struct Pulse {
    double amp_left = 0, amp_right = 0;
    double rem_left = 0, rem_right = 0;
    double dur_left = 0, dur_right = 0;
  };
  static void trigger_pulse(Pulse& p, double impulse, const RumbleMotorPair& amp, const RumbleMotorPair& time);
  static void step_pulse(Pulse& p, double dt, double& out_left, double& out_right);

  RumbleConfig cfg_;
  Pulse car_car_;
  Pulse car_world_;
  Pulse car_object_;
  double shift_rem_ = 0;
  double sputter_rem_ = 0;
  double backfire_rem_ = 0;
  double smooth_left_ = 0;
  double smooth_right_ = 0;
};

}  // namespace drive
