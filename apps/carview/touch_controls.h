// Multi-touch driving HUD & input controller for Android (and touchscreens).
// Processes SDL_EVENT_FINGER_DOWN/MOTION/UP/CANCELED events in normalized [0..1]
// screen space, scales safely across phones (20:9), handhelds/Odin (16:9), and
// tablets (4:3/16:10), auto-hides when a physical gamepad is active or after
// inactivity, and renders an FH1-styled translucent overlay via carview::ui::List.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"

union SDL_Event;

namespace carview {

enum class TouchSteerMode : int {
  kButtons = 0,     // Left / Right directional pads on bottom-left
  kThumbstick = 1,  // Floating virtual steering thumbstick on left half
  kTilt = 2,        // Accelerometer / gyro roll steering
};

enum class TouchVisibility : int {
  kAuto = 0,      // Appears on first touch, hides on gamepad or after 20 s idle
  kAlwaysOn = 1,  // Always visible (e.g. demo/testing or accessibility)
  kOff = 2,       // Never shown or processed
};

struct TouchRect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // normalized [0..1]
  bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
  float cx() const { return 0.5f * (x0 + x1); }
  float cy() const { return 0.5f * (y0 + y1); }
  float w() const { return x1 - x0; }
  float h() const { return y1 - y0; }
};

struct TouchLayout {
  TouchRect steer_left;
  TouchRect steer_right;
  TouchRect steer_zone;   // floating thumbstick activation region
  TouchRect throttle;
  TouchRect brake;
  TouchRect handbrake;
  TouchRect shift_up;
  TouchRect shift_down;
  TouchRect btn_pause;
  TouchRect btn_map;
  TouchRect btn_camera;
  TouchRect btn_rewind;
  TouchRect btn_radio;
  TouchRect btn_mode;     // cycles Buttons / Thumbstick / Tilt
};

struct TouchDriveOutput {
  bool active = false;
  double steer = 0.0;      // -1..+1
  double throttle = 0.0;   // 0..1
  double brake = 0.0;      // 0..1
  bool handbrake = false;
};

struct TouchActions {
  bool pause = false;
  bool map = false;
  bool camera = false;
  bool rewind = false;
  bool radio_next = false;
  bool shift_up = false;
  bool shift_down = false;
};

class TouchControls {
 public:
  static constexpr double kAutoHideSeconds = 20.0;

  void set_visibility(TouchVisibility v) { visibility_ = v; }
  TouchVisibility visibility() const { return visibility_; }

  void set_steer_mode(TouchSteerMode m) { steer_mode_ = m; }
  TouchSteerMode steer_mode() const { return steer_mode_; }
  void cycle_steer_mode();

  // Computes aspect-ratio-aware layout (safe-area margins for phones, tablets, Odin).
  static TouchLayout compute_layout(float screen_w, float screen_h);

  // Processes an SDL finger event; returns true if consumed by touch controls.
  bool handle_sdl_event(const SDL_Event& e, float screen_w, float screen_h, double now_s);

  // Direct normalized [0..1] finger entry points (used by SDL handler & unit tests).
  bool on_finger_down(int64_t finger_id, float nx, float ny, float screen_w, float screen_h, double now_s);
  bool on_finger_motion(int64_t finger_id, float nx, float ny, float screen_w, float screen_h, double now_s);
  bool on_finger_up(int64_t finger_id, double now_s);

  // Updates accelerometer/gyro roll angle (radians; positive = right turn) for Tilt mode.
  void set_tilt_roll(float roll_rad, float deadzone_rad = 0.06f, float max_rad = 0.55f);

  // Call when physical gamepad connects or sends input (auto-hides touch overlay).
  void notify_gamepad_input(double now_s);
  void set_gamepad_connected(bool connected);

  // Advances inactivity timer and refreshes continuous output.
  void update(double now_s);

  bool visible(double now_s) const;
  TouchDriveOutput output() const;
  TouchActions consume_actions();

  // Renders translucent FH1-styled touch controls into `list`.
  // When `demo_active` is true, highlights steering/throttle as an active driving demo.
  void build_overlay(ui::List& list, float screen_w, float screen_h, double now_s, bool demo_active = false) const;

 private:
  enum class Role {
    kNone,
    kSteerButtons,
    kThumbstick,
    kThrottle,
    kBrake,
    kHandbrake,
    kShiftUp,
    kShiftDown,
    kPause,
    kMap,
    kCamera,
    kRewind,
    kRadio,
    kMode,
  };

  struct Finger {
    int64_t id = 0;
    Role role = Role::kNone;
    float anchor_x = 0, anchor_y = 0;
    float x = 0, y = 0;
  };

  void recompute_output(float screen_w, float screen_h);

  TouchVisibility visibility_ = TouchVisibility::kAuto;
  TouchSteerMode steer_mode_ = TouchSteerMode::kButtons;
  bool gamepad_connected_ = false;
  bool ever_touched_ = false;
  double last_touch_s_ = -1000.0;
  double last_gamepad_s_ = -1000.0;
  float tilt_steer_ = 0.0f;
  std::vector<Finger> fingers_;
  TouchDriveOutput out_;
  TouchActions pending_actions_;
};

const char* touch_steer_mode_name(TouchSteerMode m);
bool parse_touch_visibility(const std::string& s, TouchVisibility& out);
bool parse_touch_steer_mode(const std::string& s, TouchSteerMode& out);

}  // namespace carview
