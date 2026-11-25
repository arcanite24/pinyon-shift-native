#include "gamepad_db.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace carview {

namespace {

std::string to_lower_ascii(std::string_view sv) {
  std::string out(sv);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Normalizes a calibrated trigger/pedal axis relative to its captured resting
// value. Handles all three USB HID resting conventions cleanly:
//   - rest near -32768 (e.g. XInput/DirectInput triggers resting at -32768 -> pressed +32767)
//   - rest near +32767 (inverted racing-wheel pedals resting at +32767 -> pressed -32768)
//   - rest near 0      (uncalibrated/split-axis devices; only reacts past deadzone)
double calibrated_trigger_value(int16_t raw, int16_t rest, double dz_in, double dz_out) {
  const double d_raw = double(raw);
  const double d_rest = double(rest);
  double norm = 0.0;
  if (rest <= -16000) {
    // Resting near -32768, full travel toward +32767.
    const double span = std::max(1.0, 32767.0 - d_rest);
    norm = std::clamp((d_raw - d_rest) / span, 0.0, 1.0);
  } else if (rest >= 16000) {
    // Inverted pedal/trigger resting near +32767, full travel toward -32768.
    const double span = std::max(1.0, d_rest + 32768.0);
    norm = std::clamp((d_rest - d_raw) / span, 0.0, 1.0);
  } else {
    // Centered at ~0 (e.g. half-axis trigger or uninitialized axis before first touch).
    // Never emit non-zero output while sitting near rest!
    const double delta = d_raw - d_rest;
    if (std::fabs(delta) < 2500.0) return 0.0;
    norm = std::clamp(std::fabs(delta) / 32767.0, 0.0, 1.0);
  }
  return drive::apply_deadzone_unsigned(norm, dz_in, dz_out);
}

// Calibrated racing-wheel pedal axis: only activates when displaced from rest,
// auto-detecting normal (-32768 -> +32767) vs inverted (+32767 -> -32768).
double calibrated_wheel_pedal(int16_t raw, int16_t rest, const drive::WheelShaping& shaping) {
  const double d_raw = double(raw);
  const double d_rest = double(rest);
  double unit = 0.0;
  if (rest >= 16000) {
    const double span = std::max(1.0, d_rest + 32768.0);
    unit = std::clamp((d_rest - d_raw) / span, 0.0, 1.0);
  } else if (rest <= -16000) {
    const double span = std::max(1.0, 32767.0 - d_rest);
    unit = std::clamp((d_raw - d_rest) / span, 0.0, 1.0);
  } else {
    // Resting near 0 (unmoved pedal on macOS/Linux/uninitialized HID): require
    // real displacement before applying throttle or brake.
    const double delta = d_raw - d_rest;
    if (std::fabs(delta) < 3000.0) return 0.0;
    unit = std::clamp(std::fabs(delta) / 32767.0, 0.0, 1.0);
  }
  if (unit <= 0.01) return 0.0;
  return drive::shape_wheel_pedal(unit, shaping);
}

}  // namespace

void apply_sdl_gamepad_hints() {
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX_ONE, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
}

const std::vector<std::string_view>& embedded_gamepad_mappings() {
  static const std::vector<std::string_view> kMappings = {
      // 8BitDo Ultimate 2C Wired & Wireless (Windows / macOS / Linux)
      "03000000c82d00001d30000000000000,8BitDo Ultimate 2C Wired Controller,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a3,leftx:a0,lefty:a1,paddle1:b2,paddle2:b5,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a5,start:b11,x:b3,y:b4,platform:Windows,",
      "03000000c82d00001d30000001000000,8BitDo Ultimate 2C Wired Controller,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a3,leftx:a0,lefty:a1,paddle1:b2,paddle2:b5,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a5,start:b11,x:b3,y:b4,platform:Windows,",
      "03000000c82d00001d30000000000000,8BitDo Ultimate 2C Wired Controller,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Mac OS X,",
      "03000000c82d00001d30000001010000,8BitDo Ultimate 2C Wired Controller,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Mac OS X,",
      "03000000c82d00001d30000011010000,8BitDo Ultimate 2C Wired Controller,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Linux,",
      "03000000c82d00000a31000000000000,8BitDo Ultimate 2C Wireless Controller,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a5,start:b11,x:b3,y:b4,platform:Windows,",
      "03000000c82d00000a31000000000000,8BitDo Ultimate 2C Wireless Controller,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Mac OS X,",
      // 8BitDo Ultimate Wired / 2.4G / Bluetooth / C
      "03000000c82d00000631000000000000,8BitDo Ultimate Wireless 2.4G,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b10,leftshoulder:b4,leftstick:b8,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b9,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,platform:Windows,",
      "03000000c82d00000631000000000000,8BitDo Ultimate Wireless 2.4G,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Mac OS X,",
      "03000000c82d00000731000000000000,8BitDo Ultimate Wired Controller,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Mac OS X,",
      "03000000c82d00001030000000000000,8BitDo Ultimate Wired (D-Input),a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Mac OS X,",
      "03000000c82d00001530000000000000,8BitDo Ultimate Bluetooth,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Mac OS X,",
      "03000000c82d00001630000000000000,8BitDo Ultimate 2.4G (D-Input),a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Mac OS X,",
      // 8BitDo Pro 2 / SN30 Pro / Pro+ / M30 / Lite 2 / Arcade Stick
      "03000000c82d00000360000000000000,8BitDo Pro 2,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Windows,",
      "03000000c82d00000360000000000000,8BitDo Pro 2,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Mac OS X,",
      "03000000c82d00000660000000000000,8BitDo Pro 2 Wired,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Windows,",
      "03000000c82d00000660000000000000,8BitDo Pro 2 Wired,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Mac OS X,",
      "03000000c82d00000160000000000000,8BitDo SN30 Pro,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b6,leftstick:b13,lefttrigger:b8,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:b9,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Mac OS X,",
      "03000000c82d00000260000000000000,8BitDo SN30 Pro+,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:a4,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Mac OS X,",
      "03000000c82d00000650000000000000,8BitDo M30,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b8,lefttrigger:b9,leftx:a0,lefty:a1,rightshoulder:b6,righttrigger:b7,start:b11,x:b3,y:b4,platform:Mac OS X,",
      "03000000c82d00001251000000000000,8BitDo Lite 2,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:b8,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:b9,rightx:a2,righty:a3,start:b11,x:b3,y:b4,platform:Mac OS X,",
      "03000000c82d00001230000000000000,8BitDo Arcade Stick,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,lefttrigger:b8,leftx:a0,lefty:a1,rightshoulder:b7,righttrigger:b9,start:b11,x:b3,y:b4,platform:Mac OS X,",
      // Xbox / PS / Nintendo / Generic USB fallbacks
      "030000005e0400008e02000000000000,Xbox 360 Controller,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b10,leftshoulder:b4,leftstick:b8,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b9,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,platform:Mac OS X,",
      "030000004c050000e60c000000000000,PS5 DualSense Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,touchpad:b13,x:b0,y:b3,platform:Mac OS X,",
      "030000007e0500000920000000000000,Nintendo Switch Pro Controller,a:b0,b:b1,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2,righty:a3,start:b9,x:b2,y:b3,platform:Mac OS X,",
      "03000000790000000600000000000000,Generic USB Joystick,a:b2,b:b1,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2,righty:a4,start:b9,x:b3,y:b0,platform:Mac OS X,",
  };
  return kMappings;
}

GamepadDbStats load_gamepad_mappings(const std::filesystem::path& exe_dir) {
  GamepadDbStats stats;
  for (std::string_view m : embedded_gamepad_mappings()) {
    std::string line(m);
    const int r = SDL_AddGamepadMapping(line.c_str());
    if (r >= 0) ++stats.embedded_added;
  }

  std::vector<std::filesystem::path> candidates;
  if (!exe_dir.empty()) {
    candidates.push_back(exe_dir / "gamecontrollerdb.txt");
    candidates.push_back(exe_dir / "config" / "gamecontrollerdb.txt");
    candidates.push_back(exe_dir / ".." / "Resources" / "gamecontrollerdb.txt");
    candidates.push_back(exe_dir / ".." / "config" / "gamecontrollerdb.txt");
    candidates.push_back(exe_dir / ".." / ".." / "config" / "gamecontrollerdb.txt");
  }
  if (const char* base = SDL_GetBasePath()) {
    // SDL3 owns the returned SDL_GetBasePath() string; never SDL_free it.
    const std::filesystem::path bp(base);
    candidates.push_back(bp / "gamecontrollerdb.txt");
    candidates.push_back(bp / "config" / "gamecontrollerdb.txt");
    candidates.push_back(bp / ".." / "Resources" / "gamecontrollerdb.txt");
  }
  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec) {
    candidates.push_back(cwd / "config" / "gamecontrollerdb.txt");
    candidates.push_back(cwd / "gamecontrollerdb.txt");
  }

  for (const auto& p : candidates) {
    std::error_code fec;
    if (!p.empty() && std::filesystem::is_regular_file(p, fec)) {
      const int added = SDL_AddGamepadMappingsFromFile(p.string().c_str());
      if (added >= 0) {
        stats.file_added = added;
        stats.loaded_file = p;
        break;
      }
    }
  }
  return stats;
}

bool is_racing_wheel_device(uint16_t vendor_id, uint16_t product_id, std::string_view name) {
  (void)product_id;
  const std::string low = to_lower_ascii(name);
  // Explicitly reject known gamepad brands even if they share a generic chip.
  if (low.find("8bitdo") != std::string::npos || low.find("gamepad") != std::string::npos ||
      low.find("dualshock") != std::string::npos || low.find("dualsense") != std::string::npos ||
      low.find("xbox") != std::string::npos || low.find("joy-con") != std::string::npos ||
      low.find("pro controller") != std::string::npos) {
    return false;
  }
  // Logitech makes both gamepads (F310/F510/F710/Dual Action) and wheels (G25/G27/G29/G920/G923/DFGT/PRO).
  if (vendor_id == 0x046D) {
    if (low.find("f310") != std::string::npos || low.find("f510") != std::string::npos ||
        low.find("f710") != std::string::npos || low.find("dual action") != std::string::npos ||
        low.find("rumblepad") != std::string::npos) {
      return false;
    }
    if (low.find("wheel") != std::string::npos || low.find("driving force") != std::string::npos ||
        low.find("g25") != std::string::npos || low.find("g27") != std::string::npos ||
        low.find("g29") != std::string::npos || low.find("g920") != std::string::npos ||
        low.find("g923") != std::string::npos || low.find("pro racing") != std::string::npos) {
      return true;
    }
    return false;
  }
  // Dedicated sim-racing hardware vendors.
  if (drive::is_known_wheel_vendor(vendor_id)) return true;
  // Fallback keyword match for unlisted wheel vendors.
  if (low.find("racing wheel") != std::string::npos || low.find("steering wheel") != std::string::npos ||
      low.find("driving force") != std::string::npos || low.find("direct drive") != std::string::npos ||
      low.find("pedals") != std::string::npos) {
    return true;
  }
  return false;
}

RawJoystickDriveOutput sample_raw_joystick_pad(const RawJoystickSnapshot& snap,
                                               const drive::ControlDeadzones& dz) {
  RawJoystickDriveOutput out;
  const size_t n_axes = snap.axes.size();
  // Steering: Axis 0 (Left Stick X) relative to resting baseline.
  if (n_axes >= 1) {
    const double raw_s = std::clamp(double(snap.axis_or(0) - snap.rest_or(0)) / 32767.0, -1.0, 1.0);
    out.input.steer = drive::apply_deadzone_signed(raw_s, dz.steering_inside, dz.steering_outside);
  }

  // Triggers / Throttle & Brake:
  //   - 6-axis HID pads (8BitDo D-Input/macOS, PS/Xbox raw): axes 4/5 or 3/4 are LT/RT.
  //     We probe axes 4 & 5 first (standard Linux/macOS HID), falling back to digital
  //     shoulder/trigger buttons (b8/b9 or b6/b7) if no trigger axis moved.
  double brake = 0.0;
  double throttle = 0.0;
  if (n_axes >= 6) {
    // Detect which pair holds triggers by checking if axes 4/5 rest at extremes (-32768/+32767)
    // vs axes 3/4 (Windows DirectInput 8BitDo layout where a3=LT, a4=RT, a2=RX, a5=RY).
    const bool a34_triggers = (std::abs(int(snap.rest_or(3))) >= 16000 && std::abs(int(snap.rest_or(4))) >= 16000 &&
                               std::abs(int(snap.rest_or(5))) < 16000);
    const size_t lt_idx = a34_triggers ? 3u : 4u;
    const size_t rt_idx = a34_triggers ? 4u : 5u;
    const size_t rx_idx = 2u;
    const size_t ry_idx = a34_triggers ? 5u : 3u;
    // Note: on some macOS HID reports, a4=RT and a5=LT (8BitDo D-Input). Using LT=a5/RT=a4 if
    // both rest at extremes is consistent; default standard a4=LT, a5=RT otherwise.
    brake = calibrated_trigger_value(snap.axis_or(lt_idx), snap.rest_or(lt_idx), dz.brake_inside, dz.brake_outside);
    throttle = calibrated_trigger_value(snap.axis_or(rt_idx), snap.rest_or(rt_idx), dz.throttle_inside, dz.throttle_outside);
    out.look_x = drive::apply_deadzone_signed(
        std::clamp(double(snap.axis_or(rx_idx) - snap.rest_or(rx_idx)) / 32767.0, -1.0, 1.0), 0.20, 0.95);
    out.look_y = drive::apply_deadzone_signed(
        std::clamp(double(snap.axis_or(ry_idx) - snap.rest_or(ry_idx)) / 32767.0, -1.0, 1.0), 0.20, 0.95);
  } else if (n_axes >= 4) {
    out.look_x = drive::apply_deadzone_signed(
        std::clamp(double(snap.axis_or(2) - snap.rest_or(2)) / 32767.0, -1.0, 1.0), 0.20, 0.95);
    out.look_y = drive::apply_deadzone_signed(
        std::clamp(double(snap.axis_or(3) - snap.rest_or(3)) / 32767.0, -1.0, 1.0), 0.20, 0.95);
  }

  // Digital trigger button fallbacks (e.g. SN30 Pro / Switch / M30 / Generic USB pads where
  // L2/R2 are buttons b8/b9 or b6/b7).
  if (snap.buttons.size() >= 10) {
    if (snap.button(9)) throttle = std::max(throttle, 1.0);
    if (snap.button(8)) brake = std::max(brake, 1.0);
  } else if (snap.buttons.size() >= 8) {
    if (snap.button(7)) throttle = std::max(throttle, 1.0);
    if (snap.button(6)) brake = std::max(brake, 1.0);
  }

  // Standard face buttons: South (b0) = Handbrake when driving (matches default FH1 layout).
  const bool handbrake = snap.button(0);

  out.input.throttle = throttle;
  out.input.brake = brake;
  out.input.handbrake = handbrake;
  out.any_active = (std::fabs(out.input.steer) > 1e-3 || out.input.throttle > 1e-3 || out.input.brake > 1e-3 ||
                    out.input.handbrake || std::fabs(out.look_x) > 1e-3 || std::fabs(out.look_y) > 1e-3);
  return out;
}

RawJoystickDriveOutput sample_wheel_joystick(const RawJoystickSnapshot& snap,
                                             const drive::WheelShaping& shaping) {
  RawJoystickDriveOutput out;
  const size_t n_axes = snap.axes.size();
  if (n_axes == 0) return out;
  // WheelAdvancedOptions' dead zones (FEEL-18), then the engine's shaping.
  static const drive::ControlDeadzones kWheelDz = drive::ControlDeadzones::wheel_defaults();
  const double raw_s = std::clamp(double(snap.axis_or(0) - snap.rest_or(0)) / 32767.0, -1.0, 1.0);
  const double shaped_s = drive::shape_wheel_steering(
      drive::apply_deadzone_signed(raw_s, kWheelDz.steering_inside, kWheelDz.steering_outside), shaping);
  double shaped_t = 0.0;
  double shaped_b = 0.0;
  if (n_axes >= 2) {
    shaped_t = drive::apply_deadzone_unsigned(calibrated_wheel_pedal(snap.axis_or(1), snap.rest_or(1), shaping),
                                              kWheelDz.throttle_inside, kWheelDz.throttle_outside);
  }
  if (n_axes >= 3) {
    shaped_b = drive::apply_deadzone_unsigned(calibrated_wheel_pedal(snap.axis_or(2), snap.rest_or(2), shaping),
                                              kWheelDz.brake_inside, kWheelDz.brake_outside);
  }
  out.input.steer = shaped_s;
  out.input.throttle = shaped_t;
  out.input.brake = shaped_b;
  out.any_active = (std::fabs(shaped_s) > 1e-3 || shaped_t > 1e-3 || shaped_b > 1e-3);
  return out;
}

drive::Input merge_driving_inputs(const drive::Input& keyboard,
                                  const drive::Input& pad,
                                  bool pad_active,
                                  const drive::Input& raw_joy,
                                  bool raw_joy_active,
                                  const drive::Input& wheel,
                                  bool wheel_active) {
  drive::Input out = keyboard;
  const bool kb_steering = std::fabs(keyboard.steer) > 1e-3;
  auto apply_dev = [&](const drive::Input& dev, bool active) {
    if (!active) return;
    if (!kb_steering && std::fabs(dev.steer) > 1e-3) out.steer = dev.steer;
    out.throttle = std::max(out.throttle, std::clamp(dev.throttle, 0.0, 1.0));
    out.brake = std::max(out.brake, std::clamp(dev.brake, 0.0, 1.0));
    out.clutch = std::max(out.clutch, std::clamp(dev.clutch, 0.0, 1.0));
    out.handbrake = out.handbrake || dev.handbrake;
  };
  if (pad_active) {
    apply_dev(pad, true);
  } else {
    if (raw_joy_active) apply_dev(raw_joy, true);
    if (wheel_active) apply_dev(wheel, true);
  }
  return out;
}

RawJoystickSnapshot snapshot_raw_joystick(const RawJoystickEntry& entry) {
  RawJoystickSnapshot snap;
  if (!entry.joy) return snap;
  const int n_axes = std::max(0, SDL_GetNumJoystickAxes(entry.joy));
  const int n_buttons = std::max(0, SDL_GetNumJoystickButtons(entry.joy));
  snap.axes.resize(size_t(n_axes));
  for (int i = 0; i < n_axes; ++i) snap.axes[size_t(i)] = SDL_GetJoystickAxis(entry.joy, i);
  snap.rest_axes = entry.rest_axes;
  if (snap.rest_axes.size() < snap.axes.size()) snap.rest_axes.resize(snap.axes.size(), 0);
  snap.buttons.resize(size_t(n_buttons));
  for (int i = 0; i < n_buttons; ++i) snap.buttons[size_t(i)] = SDL_GetJoystickButton(entry.joy, i) ? 1u : 0u;
  if (SDL_GetNumJoystickHats(entry.joy) > 0) snap.hat = SDL_GetJoystickHat(entry.joy, 0);
  return snap;
}

}  // namespace carview
