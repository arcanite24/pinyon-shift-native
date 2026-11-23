// Multi-touch driving HUD & input controller for Android (and touchscreens).
// Processes SDL_EVENT_FINGER_DOWN/MOTION/UP/CANCELED events in normalized [0..1]
// screen space, scales safely across phones (20:9), handhelds/Odin (16:9), and
// tablets (4:3/16:10), auto-hides when a physical gamepad is active or after
// inactivity, and renders an FH1-styled translucent overlay via carview::ui::List.
#pragma once

#include <algorithm>
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
  static inline TouchLayout compute_layout(float screen_w, float screen_h) {
    TouchLayout lay;
    const float aspect = (screen_h > 1.0f) ? (screen_w / screen_h) : (16.0f / 9.0f);
    const float mx = aspect >= 2.0f ? 0.045f : (aspect <= 1.5f ? 0.025f : 0.030f);
    const float my = aspect <= 1.5f ? 0.036f : 0.040f;
    const float pad_w = aspect >= 2.0f ? 0.086f : (aspect <= 1.5f ? 0.110f : 0.095f);
    const float pad_h = aspect <= 1.5f ? 0.215f : 0.240f;

    const float sy1 = 1.0f - my;
    const float sy0 = sy1 - pad_h * 0.72f;
    lay.steer_left = {mx, sy0, mx + pad_w * 1.05f, sy1};
    lay.steer_right = {mx + pad_w * 1.15f, sy0, mx + pad_w * 2.20f, sy1};
    lay.steer_zone = {0.0f, 0.35f, 0.45f, 1.0f};

    const float tx1 = 1.0f - mx;
    const float tx0 = tx1 - pad_w;
    lay.throttle = {tx0, 1.0f - my - pad_h, tx1, 1.0f - my};

    const float bx1 = tx0 - 0.014f;
    const float bx0 = bx1 - pad_w * 0.92f;
    lay.brake = {bx0, 1.0f - my - pad_h * 0.84f, bx1, 1.0f - my};

    const float hbh = 0.082f;
    lay.handbrake = {bx0, lay.brake.y0 - 0.016f - hbh, bx1, lay.brake.y0 - 0.016f};

    const float sh_w = pad_w * 0.56f;
    const float sh_h = 0.082f;
    const float sh_x1 = bx0 - 0.014f;
    const float sh_x0 = sh_x1 - sh_w;
    lay.shift_up = {sh_x0, lay.brake.y0 - 0.012f, sh_x1, lay.brake.y0 - 0.012f + sh_h};
    lay.shift_down = {sh_x0, lay.shift_up.y1 + 0.014f, sh_x1, lay.shift_up.y1 + 0.014f + sh_h};

    const float top_y0 = my;
    const float top_y1 = my + 0.064f;
    const float cw = aspect >= 2.0f ? 0.064f : 0.072f;
    const float cg = 0.010f;
    const float group_w = 6.0f * cw + 5.0f * cg;
    const float gx0 = 0.5f - group_w * 0.5f;
    lay.btn_mode   = {gx0 + 0 * (cw + cg), top_y0, gx0 + 0 * (cw + cg) + cw, top_y1};
    lay.btn_rewind = {gx0 + 1 * (cw + cg), top_y0, gx0 + 1 * (cw + cg) + cw, top_y1};
    lay.btn_camera = {gx0 + 2 * (cw + cg), top_y0, gx0 + 2 * (cw + cg) + cw, top_y1};
    lay.btn_radio  = {gx0 + 3 * (cw + cg), top_y0, gx0 + 3 * (cw + cg) + cw, top_y1};
    lay.btn_map    = {gx0 + 4 * (cw + cg), top_y0, gx0 + 4 * (cw + cg) + cw, top_y1};
    lay.btn_pause  = {gx0 + 5 * (cw + cg), top_y0, gx0 + 5 * (cw + cg) + cw, top_y1};
    return lay;
  }

  // Non-overlapping normalized [0..1] safe regions for the driving minimap and
  // tachometer when touch controls are visible (guaranteed clear of all pads).
  static inline bool rects_overlap(const TouchRect& a, const TouchRect& b) {
    return a.x0 < b.x1 && a.x1 > b.x0 && a.y0 < b.y1 && a.y1 > b.y0;
  }

  static inline void hud_safe_rects(float screen_w, float screen_h, TouchRect& minimap_out, TouchRect& tach_out) {
    const TouchLayout lay = compute_layout(screen_w, screen_h);
    const float aspect = (screen_h > 1.0f) ? (screen_w / screen_h) : (16.0f / 9.0f);
    const float mx = lay.steer_left.x0;
    const float gap = 0.028f;

    const float mm_y1 = lay.steer_left.y0 - gap;
    const float mm_h = std::min(0.24f, std::max(0.14f, mm_y1 - (lay.btn_mode.y1 + 0.04f)));
    const float mm_w = mm_h / std::max(0.5f, aspect);
    minimap_out = {mx, mm_y1 - mm_h, mx + mm_w, mm_y1};

    const float right_top = std::min({lay.handbrake.y0, lay.throttle.y0, lay.shift_up.y0});
    const float tach_y1 = right_top - gap;
    const float tach_h = std::min(0.25f, std::max(0.16f, tach_y1 - (lay.btn_pause.y1 + 0.04f)));
    const float tach_w = (tach_h / std::max(0.5f, aspect)) * 1.12f;
    const float tx1 = lay.throttle.x1;
    tach_out = {tx1 - tach_w, tach_y1 - tach_h, tx1, tach_y1};
  }

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

// Android / mobile SDL3 app lifecycle state machine (P0-8): cleanly pauses audio
// & GPU submission on WILL/DID_ENTER_BACKGROUND, flushes saves, trims caches on
// LOW_MEMORY, and schedules swapchain recreation + audio resume on foreground.
struct AppLifecycleActions {
  bool pause_audio = false;
  bool resume_audio = false;
  bool flush_save = false;
  bool wait_gpu_idle = false;
  bool recreate_swapchain = false;
  bool reset_frame_clock = false;
  bool trim_memory = false;
  bool handled = false;
};

struct AppLifecycleState {
  bool backgrounded = false;
  bool audio_paused_by_bg = false;
  uint32_t background_count = 0;
  uint32_t foreground_count = 0;
  uint32_t low_memory_count = 0;

  AppLifecycleActions handle_event(uint32_t sdl_event_type, bool audio_was_running);
};

// Maps normalized SDL3 finger coordinates [0..1] to live window/pixel coordinates.
struct TouchPixelPoint {
  float x = 0.0f;
  float y = 0.0f;
};
inline TouchPixelPoint touch_norm_to_coords(float nx, float ny, int live_w, int live_h) {
  return {std::clamp(nx, 0.0f, 1.0f) * float(std::max(1, live_w)),
          std::clamp(ny, 0.0f, 1.0f) * float(std::max(1, live_h))};
}

}  // namespace carview
