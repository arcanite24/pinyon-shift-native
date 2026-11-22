// Racing wheels (single-device SDL shaping + Separate Devices multi-USB input
// binding), 900-degree linear steering mode, H-shifter 1-6/7+R, handbrake,
// physics-driven DirectInput Force Feedback (inertia, friction, damper,
// speed-stiffening spring, dynamic aligning torque, engine/road rumble split,
// test pull), and ffb-trace-<time>.csv recorder.
// Pure C++ (no SDL dependency) so libs/drive and unit tests stay headless.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "drive/vehicle.h"

namespace drive {

// ---- Single-device SDL racing wheel shaping ----

struct WheelShaping {
  // Percent of wheel travel either side of centre that reaches full lock (10-100).
  int steering_saturation = 100;
  // Percent of output skipped off centre to cancel a stick deadzone (0-50).
  int steering_deadzone = 0;
  // Steering curve exponent in percent: 100 = linear, >100 = less sensitive near centre (50-300).
  int steering_curve = 100;
  // Percent of each pedal's travel ignored at rest (0-50).
  int pedal_deadzone = 0;
};

int16_t shape_wheel_steering_i16(int16_t raw, const WheelShaping& shaping);
uint8_t shape_wheel_pedal_u8(uint8_t raw, const WheelShaping& shaping);
double shape_wheel_steering(double raw_signed, const WheelShaping& shaping);
double shape_wheel_pedal(double raw_unit, const WheelShaping& shaping);

// ---- Separate Devices (multi-USB wheel base, pedals, H-shifter, handbrake, button box) ----

enum class AxisDirection : uint8_t {
  kAuto,      // resolved from axis rest value (>0 => inverted)
  kNormal,    // -32768 released, +32767 pressed
  kInverted,  // +32767 released, -32768 pressed
};

// "[device|][+|-]axisN" (or "aN")
struct WheelAxisBinding {
  std::string device;
  int axis = -1;
  AxisDirection direction = AxisDirection::kAuto;

  bool valid() const { return axis >= 0; }
  std::string format() const;
};

// Target actions for Separate Devices button/hat/axis-as-button bindings.
enum class WheelTarget : uint8_t {
  kNone = 0,
  kButtonA,
  kButtonB,
  kButtonX,
  kButtonY,
  kLeftShoulder,
  kRightShoulder,
  kLeftTrigger,
  kRightTrigger,
  kLeftStick,
  kRightStick,
  kBack,
  kStart,
  kDpadUp,
  kDpadDown,
  kDpadLeft,
  kDpadRight,
  kShiftUp,
  kShiftDown,
  kHandbrake,
  kClutch,
  kCamera,
  kRewind,
  kLookBack,
  kGear,  // H-shifter gate (gear = -1 for REVERSE, 1..7 for GEAR1..GEAR7)
};

struct WheelButtonBinding {
  std::string device;
  int button = -1;  // >= 0 for a digital button
  int hat = -1;     // >= 0 for a hat direction
  uint8_t hat_mask = 0;  // 1=up, 2=right, 4=down, 8=left
  int axis = -1;    // >= 0 for an axis used as a digital control (e.g. handbrake lever)
  AxisDirection axis_direction = AxisDirection::kAuto;
  WheelTarget target = WheelTarget::kNone;
  int8_t gear = 0;  // -1 = reverse, 1..7 = H-shifter gate, 0 = non-gear control

  std::string format() const;
};

std::optional<WheelAxisBinding> parse_wheel_axis_binding(std::string_view text);
std::vector<WheelButtonBinding> parse_wheel_button_bindings(std::string_view text);
std::string format_wheel_button_bindings(const std::vector<WheelButtonBinding>& bindings);

// Matches empty (any), "vvvv:pppp" hex USB IDs, "path:<device path>", or
// case-insensitive substring of the device name.
bool wheel_device_matches(std::string_view match, std::string_view name, uint16_t vendor,
                          uint16_t product, std::string_view path = {});
bool is_known_wheel_vendor(uint16_t vendor);

AxisDirection resolve_axis_direction(AxisDirection dir, int16_t rest_value);
int16_t steering_from_axis(int16_t raw, bool invert, int scale_percent, int deadzone_percent);
uint8_t pedal_from_axis(int16_t raw, AxisDirection dir, int bottom_deadzone_pct, int top_deadzone_pct);

// Raw snapshot of one attached USB HID / DirectInput joystick device.
struct UsbDeviceSnapshot {
  std::string name;
  std::string path;
  uint16_t vendor = 0;
  uint16_t product = 0;
  std::vector<int16_t> axes;
  std::vector<int16_t> rest_axes;  // captured on open for kAuto resolution
  std::vector<uint8_t> buttons;
  std::vector<uint8_t> hats;
};

struct SeparateWheelConfig {
  bool enabled = false;
  bool linear_900_deg = true;      // bypasses gamepad speed narrowing when active
  int wheel_rotation_deg = 900;    // 270..1080
  bool invert_steering = false;
  int steering_scale_pct = 100;    // 10..1000
  int steering_deadzone_pct = 0;   // 0..50

  WheelAxisBinding steering;
  WheelAxisBinding throttle;
  WheelAxisBinding brake;
  WheelAxisBinding clutch;
  WheelAxisBinding handbrake;

  int throttle_dz_bottom = 0, throttle_dz_top = 0;
  int brake_dz_bottom = 0, brake_dz_top = 0;
  int clutch_dz_bottom = 0, clutch_dz_top = 0;
  int handbrake_dz_bottom = 0, handbrake_dz_top = 0;

  std::vector<WheelButtonBinding> buttons;

  std::string ffb_device;  // device matcher for DirectInput FFB base
};

struct SeparateWheelOutput {
  bool active = false;
  double steer = 0.0;        // -1..+1
  double throttle = 0.0;     // 0..1
  double brake = 0.0;        // 0..1
  double clutch = 0.0;       // 0..1
  double handbrake = 0.0;    // 0..1
  bool linear_900 = false;   // true: vehicle should use linear wheel steering
  // H-shifter gate currently engaged: -1 = Reverse, 0 = Neutral/none, 1..7 = Gear 1..7.
  bool has_h_shifter = false;
  int h_shifter_gear = 0;
  // Edge/level button states from wheel & button box.
  bool shift_up = false;
  bool shift_down = false;
  bool button_a = false;
  bool button_b = false;
  bool button_x = false;
  bool button_y = false;
  bool start = false;
  bool back = false;
  bool camera = false;
  bool rewind = false;
  bool look_back = false;
  bool dpad_up = false;
  bool dpad_down = false;
  bool dpad_left = false;
  bool dpad_right = false;
};

// Evaluates all attached USB devices against `cfg` into a unified driving output.
SeparateWheelOutput evaluate_separate_wheel(const SeparateWheelConfig& cfg,
                                            const std::vector<UsbDeviceSnapshot>& devices);

// Resolves shift request (+1/-1/0) given H-shifter state (if any) and paddle presses.
// When an H-shifter is bound and engaged in a gate, drives the vehicle towards that gear;
// paddles still work as a fallback when no gate is engaged or no H-shifter is bound.
int resolve_wheel_shift(const SeparateWheelOutput& out, int current_gear, bool prev_shift_up,
                        bool prev_shift_down);

// ---- Force Feedback (DirectInput FFB + physics model + trace CSV) ----

enum class WheelFfbMode : uint8_t {
  kOff = 0,
  kRumbleOnly = 1,
  kFull = 2,
};

const char* wheel_ffb_mode_name(WheelFfbMode m);
WheelFfbMode parse_wheel_ffb_mode(std::string_view text);

struct WheelFfbConfig {
  WheelFfbMode mode = WheelFfbMode::kOff;
  int strength_percent = 50;       // overall master gain 0..100 (low default for direct-drive safety)
  bool invert = false;             // reverse force direction
  int min_force_percent = 0;       // 0..30 minimum force lift for geared/belted bases
  int inertia_percent = 100;       // 0..200
  int friction_percent = 100;      // 0..200
  int damper_percent = 100;        // 0..200
  int spring_percent = 100;        // 0..200 (speed-stiffening spring)
  int align_percent = 100;         // 0..200 (dynamic aligning / steering torque)
  int engine_rumble_percent = 50;  // 0..100 (grows toward redline)
  int road_rumble_percent = 100;   // 0..100 (bumps, kerbs, impacts)
  bool record_trace = false;
};

struct WheelFfbTelemetry {
  bool driving = false;
  double speed_mps = 0.0;
  double steer_angle_rad = 0.0;      // road wheel angle
  double steer_input = 0.0;          // -1..+1
  double throttle_input = 0.0;       // 0..1
  double brake_input = 0.0;          // 0..1
  double front_slip_angle = 0.0;     // normalised front tyre slip angle (signed)
  double front_slip_ratio = 0.0;     // normalised front tyre longitudinal slip
  double front_load_n = 0.0;         // total front axle vertical load (N)
  double static_front_load_n = 7000.0;
  double lateral_g = 0.0;
  double rpm = 800.0;
  double redline_rpm = 7000.0;
  double surface_bump_rumble = 0.0;  // 0..1
  double body_contact_force = 0.0;   // N
};

// Computed FFB command values in DirectInput [-10000..+10000] / [0..10000] scale.
struct WheelFfbForces {
  bool active = false;
  int32_t constant_force = 0;   // -10000..+10000 (dynamic aligning + road/engine rumble)
  int32_t spring_coeff = 0;     // 0..10000 (speed-stiffening centring spring)
  int32_t damper_coeff = 0;     // 0..10000
  int32_t inertia_coeff = 0;    // 0..10000
  int32_t friction_coeff = 0;   // 0..10000
  double engine_rumble = 0.0;   // 0..1
  double road_rumble = 0.0;     // 0..1
  double aligning_norm = 0.0;   // -1..+1 pre-invert aligning torque
};

// Extracts FFB telemetry directly from a drive::Vehicle.
WheelFfbTelemetry ffb_telemetry_from_vehicle(const Vehicle& v, bool driving, double lateral_g = 0.0);

// Pure-math FFB solver: computes condition and dynamic steering forces for one frame.
class WheelFfbSolver {
 public:
  // Triggers the left-then-right direction test pull (0.4s left, 0.2s pause, 0.4s right).
  void start_test_pull();
  bool test_pull_active() const { return test_timer_ > 0.0; }
  void reset();

  WheelFfbForces step(const WheelFfbTelemetry& tel, const WheelFfbConfig& cfg, double dt_s);

 private:
  double rumble_phase_ = 0.0;
  double smooth_align_ = 0.0;
  double test_timer_ = 0.0;
};

// Writes `ffb-trace-<time>.csv` rows when `record_trace` is enabled.
class FfbTraceRecorder {
 public:
  ~FfbTraceRecorder();
  bool start(const std::filesystem::path& directory, std::string* created_path = nullptr);
  void stop();
  bool is_recording() const { return file_.is_open(); }
  void record(double time_s, WheelFfbMode mode, const WheelFfbTelemetry& tel,
              const WheelFfbForces& forces);
  uint64_t rows_written() const { return rows_; }

 private:
  std::ofstream file_;
  uint64_t rows_ = 0;
};

// Windows DirectInput8 Force Feedback backend (dynamically loads dinput8.dll on
// Windows; clean no-op on non-Windows or when no FFB wheel is attached).
class DirectInputFfbDevice {
 public:
  DirectInputFfbDevice();
  ~DirectInputFfbDevice();
  DirectInputFfbDevice(const DirectInputFfbDevice&) = delete;
  DirectInputFfbDevice& operator=(const DirectInputFfbDevice&) = delete;

  // Enumerates attached FFB-capable DirectInput wheels and opens the first matching `device_match`.
  bool open(void* native_hwnd, std::string_view device_match = {});
  void close();
  bool is_open() const;
  std::string device_name() const;

  // Sends the latest forces to the wheel; calls stop_all() if !forces.active.
  void apply(const WheelFfbForces& forces);
  // Immediately zeroes & stops all actuators (menus, pause, focus loss, exit).
  void stop_all();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drive
