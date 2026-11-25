// Unit tests for drive::ControlSettings, deadzones, controller layout presets,
// rebindable keyboard keys and DriveSave persistence.
#include <cmath>
#include <cstdio>
#include <string>

#include "drive/controls.h"
#include "drive/save.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

}  // namespace

int main() {
  // Deadzone rescaling (SPEC-CAR-PHYSICS: rescaled from inside to outside to 0..1 and clamped).
  CHECK(near(drive::apply_deadzone_signed(0.20, 0.24, 0.95), 0.0));
  CHECK(near(drive::apply_deadzone_signed(-0.24, 0.24, 0.95), 0.0));
  CHECK(near(drive::apply_deadzone_signed(0.95, 0.24, 0.95), 1.0));
  CHECK(near(drive::apply_deadzone_signed(-1.00, 0.24, 0.95), -1.0));
  CHECK(near(drive::apply_deadzone_signed(0.595, 0.24, 0.95), 0.5));
  CHECK(near(drive::apply_deadzone_signed(-0.595, 0.24, 0.95), -0.5));

  // FEEL-26: the right stick snaps to the original's look targets only
  // (back pi, left/right pi/2), never an analogue orbit angle.
  {
    const double pi = 3.14159265358979323846;
    CHECK(near(drive::right_stick_look_target(0.0, 0.0), 0.0));
    CHECK(near(drive::right_stick_look_target(0.0, 1.0), pi));
    CHECK(near(drive::right_stick_look_target(0.9, 0.2), 0.5 * pi));
    CHECK(near(drive::right_stick_look_target(-0.9, -0.3), -0.5 * pi));
    CHECK(near(drive::right_stick_look_target(0.0, -1.0), 0.0));  // pushed up: look forward
    CHECK(near(drive::right_stick_look_target(0.4, 0.4), 0.0));   // under half deflection
    CHECK(near(drive::right_stick_look_target(0.7, 0.7), 0.5 * pi));  // diagonal: left/right wins the tie
    for (double a = 0; a < 2 * pi; a += 0.1) {
      const double t = drive::right_stick_look_target(std::sin(a), std::cos(a));
      CHECK(near(t, 0.0) || near(t, pi) || near(std::fabs(t), 0.5 * pi));
    }
  }

  CHECK(near(drive::apply_deadzone_unsigned(-0.5, 0.15, 0.90), 0.0));
  CHECK(near(drive::apply_deadzone_unsigned(0.15, 0.15, 0.90), 0.0));
  CHECK(near(drive::apply_deadzone_unsigned(0.525, 0.15, 0.90), 0.5));
  CHECK(near(drive::apply_deadzone_unsigned(0.90, 0.15, 0.90), 1.0));
  CHECK(near(drive::apply_deadzone_unsigned(1.00, 0.15, 0.90), 1.0));

  // ControllerAdvancedOptions defaults.
  {
    drive::ControlDeadzones d;
    CHECK(near(d.steering_inside, 0.24) && near(d.steering_outside, 0.95));
    CHECK(near(d.throttle_inside, 0.15) && near(d.throttle_outside, 0.90));
    CHECK(near(d.brake_inside, 0.15) && near(d.brake_outside, 0.95));
    d.steering_inside = 0.99;
    d.steering_outside = 0.50;
    d.clamp_valid();
    CHECK(d.steering_inside < d.steering_outside && d.steering_outside <= 1.0);
    // WheelAdvancedOptions defaults (FEEL-18).
    const auto w = drive::ControlDeadzones::wheel_defaults();
    CHECK(near(w.steering_inside, 0.01) && near(w.steering_outside, 0.95));
    CHECK(near(w.throttle_inside, 0.15) && near(w.throttle_outside, 1.00));
    CHECK(near(w.brake_inside, 0.15) && near(w.brake_outside, 0.95));
    CHECK(near(w.clutch_inside, 0.15) && near(w.clutch_outside, 0.90));
    CHECK(near(w.handbrake_inside, 0.10) && near(w.handbrake_outside, 1.00));
  }

  // Controller layout presets 0..4 (ControllerLayouts).
  {
    auto l0 = drive::default_controller_layout(0);
    CHECK(l0.control_for("SwitchCamera") == drive::PadControl::kRightShoulder);
    CHECK(l0.control_for("Handbrake") == drive::PadControl::kButtonA);
    CHECK(l0.control_for("Gas") == drive::PadControl::kRightTrigger);
    CHECK(l0.control_for("Brake") == drive::PadControl::kLeftTrigger);

    auto l2 = drive::default_controller_layout(2);
    CHECK(l2.control_for("SwitchCamera") == drive::PadControl::kButtonY);
    CHECK(l2.control_for("ShiftUp") == drive::PadControl::kRightShoulder);
    CHECK(l2.control_for("ShiftDown") == drive::PadControl::kLeftShoulder);

    auto l3 = drive::default_controller_layout(3);
    CHECK(l3.control_for("Gas") == drive::PadControl::kButtonA);
    CHECK(l3.control_for("Brake") == drive::PadControl::kButtonX);

    auto l4 = drive::default_controller_layout(4);
    CHECK(l4.control_for("SwitchCamera") == drive::PadControl::kLeftShoulder);
    CHECK(l4.control_for("Gas") == drive::PadControl::kLeftTrigger);
    CHECK(l4.control_for("Brake") == drive::PadControl::kRightTrigger);

    // R3-UI-02: D-pad Up -> PhotoMode, D-pad Down -> Telemetry, D-pad Left/Right -> Radio across all 5 layouts.
    for (int lid = 0; lid < 5; ++lid) {
      const auto lay = drive::default_controller_layout(lid);
      CHECK(lay.action_for(drive::PadControl::kDPadUp) == "PhotoMode");
      CHECK(lay.action_for(drive::PadControl::kDPadDown) == "Telemetry");
      CHECK(lay.action_for(drive::PadControl::kDPadLeft) == "RadioLeft");
      CHECK(lay.action_for(drive::PadControl::kDPadRight) == "RadioRight");
      CHECK(lay.action_for(drive::PadControl::kBack) == "Map");
    }
  }

  // Keyboard rebinding and serialization (all 20 ControlActions).
  {
    auto kb = drive::KeyBindings::defaults();
    CHECK(int(drive::ControlAction::kCount) == 20);
    for (int i = 0; i < int(drive::ControlAction::kCount); ++i) {
      const auto act = static_cast<drive::ControlAction>(i);
      const char* nm = drive::control_action_name(act);
      drive::ControlAction parsed{};
      CHECK(nm != nullptr && nm[0] != '\0');
      CHECK(drive::parse_control_action(nm, parsed) && parsed == act);
    }
    CHECK(kb.matches(drive::ControlAction::kPhotoMode, "P"));
    CHECK(kb.matches(drive::ControlAction::kTelemetry, "F3"));
    CHECK(kb.matches(drive::ControlAction::kThrottle, "W"));
    CHECK(kb.matches(drive::ControlAction::kThrottle, "up"));
    CHECK(kb.matches(drive::ControlAction::kSwitchCamera, "v"));
    // Default LookLeft / LookRight bindings.
    CHECK(kb.matches(drive::ControlAction::kLookLeft, "J"));
    CHECK(kb.matches(drive::ControlAction::kLookLeft, "Keypad 4"));
    CHECK(kb.matches(drive::ControlAction::kLookRight, "K"));
    CHECK(kb.matches(drive::ControlAction::kLookRight, "Keypad 6"));
    drive::ControlAction parsed_act;
    CHECK(drive::parse_control_action("look-left", parsed_act) && parsed_act == drive::ControlAction::kLookLeft);
    CHECK(drive::parse_control_action("look-right", parsed_act) && parsed_act == drive::ControlAction::kLookRight);

    kb.set_primary(drive::ControlAction::kThrottle, "I");
    kb.set_primary(drive::ControlAction::kSwitchCamera, "F1");
    kb.set_primary(drive::ControlAction::kLookLeft, "U");
    kb.set_primary(drive::ControlAction::kLookRight, "O");
    CHECK(kb.matches(drive::ControlAction::kThrottle, "I"));
    CHECK(!kb.matches(drive::ControlAction::kThrottle, "W"));

    const std::string s = kb.serialize();
    auto kb2 = drive::KeyBindings::defaults();
    kb2.deserialize(s);
    CHECK(kb2.matches(drive::ControlAction::kThrottle, "I"));
    CHECK(kb2.matches(drive::ControlAction::kSwitchCamera, "F1"));
    CHECK(kb2.matches(drive::ControlAction::kLookLeft, "U"));
    CHECK(kb2.matches(drive::ControlAction::kLookRight, "O"));
  }

  // Persistence roundtrip through DriveSave.
  {
    drive::DriveSave save;
    save.car = "NIS_SkylineGTRVSpecII_02";
    save.position = {10, 2, 30};
    save.heading = 0.5;
    save.has_controls = true;
    save.controls.layout_id = 2;
    save.controls.deadzones.steering_inside = 0.12;
    save.controls.deadzones.steering_outside = 0.88;
    save.controls.deadzones.throttle_inside = 0.05;
    save.controls.deadzones.throttle_outside = 0.95;
    save.controls.rumble_enabled = false;
    save.controls.rumble_scale = 0.5;
    save.controls.camera_effects = false;
    save.controls.keys.set_primary(drive::ControlAction::kHandbrake, "B");
    save.controls.assists.braking = 2;
    save.controls.assists.steering = 2;
    save.controls.assists.traction_control = true;
    save.controls.assists.stability_control = false;
    save.controls.assists.shifting = 2;
    save.controls.assists.driving_line = 1;

    const std::string txt = drive::save_to_text(save);
    drive::DriveSave loaded;
    std::string err;
    CHECK(drive::save_from_text(txt, loaded, &err));
    CHECK(loaded.has_controls);
    CHECK(loaded.controls.layout_id == 2);
    CHECK(near(loaded.controls.deadzones.steering_inside, 0.12));
    CHECK(near(loaded.controls.deadzones.steering_outside, 0.88));
    CHECK(near(loaded.controls.deadzones.throttle_inside, 0.05));
    CHECK(near(loaded.controls.deadzones.throttle_outside, 0.95));
    CHECK(!loaded.controls.rumble_enabled);
    CHECK(near(loaded.controls.rumble_scale, 0.5));
    CHECK(!loaded.controls.camera_effects);
    CHECK(loaded.controls.keys.matches(drive::ControlAction::kHandbrake, "B"));
    CHECK(loaded.controls.assists.braking == 2);
    CHECK(loaded.controls.assists.steering == 2);
    CHECK(loaded.controls.assists.traction_control);
    CHECK(!loaded.controls.assists.stability_control);
    CHECK(loaded.controls.assists.shifting == 2);
    CHECK(loaded.controls.assists.driving_line == 1);
  }

  std::printf("controls_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}
