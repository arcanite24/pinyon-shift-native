// Universal controller support: SDL3 HIDAPI hints, SDL_GameControllerDB loading
// (bundled file + embedded 8BitDo/common fallbacks), racing-wheel vs gamepad
// classification, calibrated raw SDL_Joystick fallback, and keyboard-safe
// driving input merging.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "drive/controls.h"
#include "drive/vehicle.h"
#include "drive/wheel_ffb.h"

namespace carview {

// Sets SDL3 HIDAPI & background-event hints before SDL_Init so 8BitDo, Xbox,
// PlayStation, Switch, and generic USB/BT controllers enumerate reliably on
// Windows, macOS, and Linux, and prevents fullscreen minimize stalls on alt-tab.
void apply_sdl_gamepad_hints();

// Embedded SDL_GameControllerDB fallback mappings (8BitDo Ultimate/Pro 2/SN30/
// M30/Lite 2/Arcade Stick + Xbox/PS/Nintendo/Generic USB pads).
const std::vector<std::string_view>& embedded_gamepad_mappings();

struct GamepadDbStats {
  int embedded_added = 0;
  int file_added = 0;
  std::filesystem::path loaded_file;
};

// Loads embedded fallback mappings and any gamecontrollerdb.txt found beside
// `exe_dir`, in `config/`, or in `../Resources/` (macOS Shift Engine.app).
GamepadDbStats load_gamepad_mappings(const std::filesystem::path& exe_dir = {});

// True only for genuine steering-wheel / pedal peripherals (Logitech,
// Thrustmaster, Fanatec, Moza, Simagic, Simucube, Asetek, Cammus, or explicit
// wheel product names) — NEVER true for standard gamepads like 8BitDo or
// generic USB pads.
bool is_racing_wheel_device(uint16_t vendor_id, uint16_t product_id, std::string_view name);

// Calibrated snapshot of a raw SDL_Joystick's axes, buttons, and hat.
struct RawJoystickSnapshot {
  std::vector<int16_t> axes;
  std::vector<int16_t> rest_axes;  // captured at open time (or zeroed)
  std::vector<uint8_t> buttons;
  uint8_t hat = 0;
  int16_t axis_or(size_t i, int16_t fallback = 0) const { return i < axes.size() ? axes[i] : fallback; }
  int16_t rest_or(size_t i, int16_t fallback = 0) const { return i < rest_axes.size() ? rest_axes[i] : fallback; }
  bool button(size_t i) const { return i < buttons.size() && buttons[i] != 0; }
};

struct RawJoystickDriveOutput {
  drive::Input input;
  double look_x = 0.0;
  double look_y = 0.0;
  bool any_active = false;
};

// Maps an unmapped (non-SDL_Gamepad, non-wheel) USB/BT controller cleanly using
// calibrated resting axis baselines so idle triggers at -32768, 0, or +32767
// NEVER produce phantom brake or throttle.
RawJoystickDriveOutput sample_raw_joystick_pad(const RawJoystickSnapshot& snap,
                                               const drive::ControlDeadzones& dz);

// Maps a genuine racing wheel's raw joystick axes using calibrated resting
// pedal baselines. Untouched pedals at 0, -32768, or +32767 always output 0.0
// until moved away from rest!
RawJoystickDriveOutput sample_wheel_joystick(const RawJoystickSnapshot& snap,
                                             const drive::WheelShaping& shaping);

// Merges keyboard driving input with gamepad / raw-joystick / wheel inputs
// without allowing an idle or slightly drifting peripheral to cancel keyboard
// W/Up acceleration or inject phantom brakes.
drive::Input merge_driving_inputs(const drive::Input& keyboard,
                                  const drive::Input& pad,
                                  bool pad_active,
                                  const drive::Input& raw_joy,
                                  bool raw_joy_active,
                                  const drive::Input& wheel,
                                  bool wheel_active);

// Runtime holder for open raw SDL_Joystick devices (fallback for controllers
// not recognized by SDL_IsGamepad()).
struct RawJoystickEntry {
  SDL_Joystick* joy = nullptr;
  SDL_JoystickID id = 0;
  std::vector<int16_t> rest_axes;
  std::vector<uint8_t> prev_buttons;
  uint8_t prev_hat = 0;
  bool is_wheel = false;
};

// Captures a live RawJoystickSnapshot from an open RawJoystickEntry.
RawJoystickSnapshot snapshot_raw_joystick(const RawJoystickEntry& entry);

}  // namespace carview
