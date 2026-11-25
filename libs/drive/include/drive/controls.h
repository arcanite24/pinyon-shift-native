// Rebindable keyboard keys, controller layout presets (ControllerLayouts
// layouts 0..4), inside/outside axis deadzones (ControllerAdvancedOptions),
// rumble/camera-effects toggles, and assist settings.
// Pure C++ (no SDL dependency) so libs/drive and unit tests stay headless.
// Spec: SPEC-CAR-PHYSICS (Throttle, brake and steering from the pad;
// Assists and input shaping), SPEC-CAMERAS (The camera set, the button and
// the default; Gamepad rumble).
#pragma once

#include <array>
#include <string>
#include <string_view>

namespace drive {

enum class ControlAction {
  kSteerLeft = 0,
  kSteerRight,
  kThrottle,
  kBrake,
  kHandbrake,
  kClutch,
  kShiftUp,
  kShiftDown,
  kSwitchCamera,
  kLookBack,
  kRewind,
  kReset,
  kMap,
  kRadioPrev,
  kRadioNext,
  kActivate,
  kLookLeft,
  kLookRight,
  kPhotoMode,
  kTelemetry,
  kCount,
};

const char* control_action_name(ControlAction a);
bool parse_control_action(std::string_view name, ControlAction& out);

// Inside/outside deadzones from ForzaProfile.sch ControllerAdvancedOptions.
struct ControlDeadzones {
  double steering_inside = 0.24, steering_outside = 0.95;
  double throttle_inside = 0.15, throttle_outside = 0.90;
  double brake_inside = 0.15, brake_outside = 0.95;
  double clutch_inside = 0.10, clutch_outside = 1.00;
  double handbrake_inside = 0.10, handbrake_outside = 1.00;

  void clamp_valid();
  // A racing wheel's own window, ForzaProfile.sch WheelAdvancedOptions
  // (FEEL-18): steering 0.01/0.95, throttle 0.15/1.00, brake 0.15/0.95,
  // clutch 0.15/0.90, handbrake 0.10/1.00.
  static ControlDeadzones wheel_defaults();
};

// Rescales an axis from [inside, outside] to [0, 1] and clamps.
// Signed preserves sign (steering); unsigned clamps negative inputs to 0 (triggers).
double apply_deadzone_signed(double raw, double inside, double outside);

// The right stick's look target (SPEC-CAMERAS, look-around: LookBack,
// LookLeft, LookRight; FEEL-26), in radians of look yaw, right positive:
// pi when the stick is pushed down (SDL +y), +-pi/2 for right/left, 0 (look
// forward) otherwise. Past half deflection on the dominant axis.
double right_stick_look_target(double rx, double ry);
double apply_deadzone_unsigned(double raw, double inside, double outside);

enum class PadControl {
  kNone = 0,
  kButtonA,
  kButtonB,
  kButtonX,
  kButtonY,
  kLeftShoulder,
  kRightShoulder,
  kLeftTrigger,
  kRightTrigger,
  kBack,
  kDPadUp,
  kDPadDown,
  kDPadLeft,
  kDPadRight,
  kLeftStickClick,
  kRightStickClick,
  kCount,
};

struct ControllerLayout {
  int id = 0;
  std::string name;
  std::array<std::string, size_t(PadControl::kCount)> bindings{};

  PadControl control_for(std::string_view action) const;
  std::string_view action_for(PadControl ctrl) const;
};

constexpr int kControllerLayoutCount = 5;
// Returns the built-in ControllerLayouts preset (id clamped to 0..4).
ControllerLayout default_controller_layout(int layout_id);
// Returns a copy of `layout` with Handbrake and Clutch bindings swapped when `swap` is true.
ControllerLayout apply_handbrake_clutch_swap(const ControllerLayout& layout, bool swap);

struct KeyBinding {
  std::string primary;
  std::string secondary;
};

struct KeyBindings {
  std::array<KeyBinding, size_t(ControlAction::kCount)> map{};

  static KeyBindings defaults();
  void set_primary(ControlAction a, std::string key);
  bool matches(ControlAction a, std::string_view key) const;
  // Compact "action=key,..." string of primary bindings (no spaces).
  std::string serialize() const;
  void deserialize(std::string_view text);
};

// Driving assist toggles matching retail FH1 Medium defaults
// (SPEC-CAR-PHYSICS, Assists and input shaping).
// The defaults are a new profile's (FEEL-12, SPEC_QUESTIONS Q71):
// ForzaProfile.sch's career assists (MetaDiff 1 Medium, Braking 1,
// Steering 1, TractionControl 0, StabilityControl 1, Shifting 0,
// SuggestedLine 0, Rewind 0), read with the game's own labels, where
// TractionControl, StabilityControl and Rewind 0 are On and SuggestedLine
// 0 is Full.
struct AssistSettings {
  int braking = 1;                 // 0 Assisted (auto-brake), 1 ABS On, 2 ABS Off
  int steering = 1;                // 0 Assisted, 1 Normal, 2 Simulation
  bool traction_control = true;    // new profile: TCS On
  bool stability_control = false;  // new profile: STM Off
  int shifting = 0;                // 0 Automatic, 1 Manual, 2 Manual + Clutch
  int driving_line = 2;            // ours: 0 Off, 1 Braking Only, 2 Full (the profile's is reversed)
  bool rewind = true;              // Rewind ON/OFF (REWIND.md: OFF adds +20% bonus)
  // Engine extra, not an original assist: the steering helpers (lane
  // centring, countersteer bias, front peak-slip pull). Off by default.
  bool engine_steer_assist = false;

  static constexpr AssistSettings retail_defaults() {
    return AssistSettings{1, 1, true, false, 0, 2, true};
  }
};

struct ControlSettings {
  int layout_id = 0;
  bool switch_handbrake_clutch = false;
  ControlDeadzones deadzones;
  bool rumble_enabled = true;
  double rumble_scale = 1.0;
  bool camera_effects = true;
  KeyBindings keys = KeyBindings::defaults();
  AssistSettings assists;
};

}  // namespace drive
