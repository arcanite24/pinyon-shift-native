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
};

// Rescales an axis from [inside, outside] to [0, 1] and clamps.
// Signed preserves sign (steering); unsigned clamps negative inputs to 0 (triggers).
double apply_deadzone_signed(double raw, double inside, double outside);
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

// Driving assist toggles matching ForzaProfile.sch defaults
// (SPEC-CAR-PHYSICS, Assists and input shaping).
struct AssistSettings {
  int braking = 1;            // 0 Assisted (auto-brake), 1 ABS On, 2 ABS Off
  int steering = 1;           // 0 Assisted, 1 Normal, 2 Simulation
  bool traction_control = false;
  bool stability_control = true;
  int shifting = 0;           // 0 Automatic, 1 Manual, 2 Manual + Clutch
  int driving_line = 0;       // 0 Off, 1 Braking Only, 2 Full
  bool rewind = true;         // Rewind ON/OFF (REWIND.md: OFF adds +20% bonus)
};

struct ControlSettings {
  int layout_id = 0;
  ControlDeadzones deadzones;
  bool rumble_enabled = true;
  double rumble_scale = 1.0;
  bool camera_effects = true;
  KeyBindings keys = KeyBindings::defaults();
  AssistSettings assists;
};

}  // namespace drive
