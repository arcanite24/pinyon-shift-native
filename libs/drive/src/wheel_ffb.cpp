#include "drive/wheel_ffb.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if defined(__has_include)
#if __has_include(<dinput.h>)
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#define SHIFT_HAS_DINPUT 1
#endif
#endif
#endif

namespace drive {
namespace {

constexpr double kTwoPi = 6.28318530717958647692;

std::string_view trim_sv(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool starts_with_nocase(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

std::optional<int> parse_index(std::string_view s) {
  s = trim_sv(s);
  int val = -1;
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
  if (ec != std::errc() || end != s.data() + s.size() || val < 0 || val > 255) return std::nullopt;
  return val;
}

void split_device(std::string_view text, std::string& device, std::string_view& rest) {
  const size_t bar = text.rfind('|');
  if (bar == std::string_view::npos) {
    device.clear();
    rest = trim_sv(text);
    return;
  }
  device = std::string(trim_sv(text.substr(0, bar)));
  rest = trim_sv(text.substr(bar + 1));
}

const char* target_name(WheelTarget t, int8_t gear) {
  if (t == WheelTarget::kGear) {
    if (gear < 0) return "REVERSE";
    switch (gear) {
      case 1: return "GEAR1";
      case 2: return "GEAR2";
      case 3: return "GEAR3";
      case 4: return "GEAR4";
      case 5: return "GEAR5";
      case 6: return "GEAR6";
      case 7: return "GEAR7";
      default: return "GEAR1";
    }
  }
  switch (t) {
    case WheelTarget::kButtonA: return "A";
    case WheelTarget::kButtonB: return "B";
    case WheelTarget::kButtonX: return "X";
    case WheelTarget::kButtonY: return "Y";
    case WheelTarget::kLeftShoulder: return "LB";
    case WheelTarget::kRightShoulder: return "RB";
    case WheelTarget::kLeftTrigger: return "LT";
    case WheelTarget::kRightTrigger: return "RT";
    case WheelTarget::kLeftStick: return "LS";
    case WheelTarget::kRightStick: return "RS";
    case WheelTarget::kBack: return "BACK";
    case WheelTarget::kStart: return "START";
    case WheelTarget::kDpadUp: return "UP";
    case WheelTarget::kDpadDown: return "DOWN";
    case WheelTarget::kDpadLeft: return "LEFT";
    case WheelTarget::kDpadRight: return "RIGHT";
    case WheelTarget::kShiftUp: return "SHIFT_UP";
    case WheelTarget::kShiftDown: return "SHIFT_DOWN";
    case WheelTarget::kHandbrake: return "HANDBRAKE";
    case WheelTarget::kClutch: return "CLUTCH";
    case WheelTarget::kCamera: return "CAMERA";
    case WheelTarget::kRewind: return "REWIND";
    case WheelTarget::kLookBack: return "LOOK_BACK";
    default: return "";
  }
}

bool parse_target(std::string_view name, WheelTarget& out_target, int8_t& out_gear) {
  const std::string l = to_lower(trim_sv(name));
  out_gear = 0;
  if (l == "reverse" || l == "gearr" || l == "r") {
    out_target = WheelTarget::kGear;
    out_gear = -1;
    return true;
  }
  if (l.size() == 5 && l.rfind("gear", 0) == 0 && l[4] >= '1' && l[4] <= '7') {
    out_target = WheelTarget::kGear;
    out_gear = int8_t(l[4] - '0');
    return true;
  }
  if (l == "a") { out_target = WheelTarget::kButtonA; return true; }
  if (l == "b") { out_target = WheelTarget::kButtonB; return true; }
  if (l == "x") { out_target = WheelTarget::kButtonX; return true; }
  if (l == "y") { out_target = WheelTarget::kButtonY; return true; }
  if (l == "lb" || l == "l1") { out_target = WheelTarget::kLeftShoulder; return true; }
  if (l == "rb" || l == "r1") { out_target = WheelTarget::kRightShoulder; return true; }
  if (l == "lt" || l == "l2") { out_target = WheelTarget::kLeftTrigger; return true; }
  if (l == "rt" || l == "r2") { out_target = WheelTarget::kRightTrigger; return true; }
  if (l == "ls" || l == "l3") { out_target = WheelTarget::kLeftStick; return true; }
  if (l == "rs" || l == "r3") { out_target = WheelTarget::kRightStick; return true; }
  if (l == "back" || l == "select" || l == "view") { out_target = WheelTarget::kBack; return true; }
  if (l == "start" || l == "menu" || l == "options") { out_target = WheelTarget::kStart; return true; }
  if (l == "up" || l == "dpad_up") { out_target = WheelTarget::kDpadUp; return true; }
  if (l == "down" || l == "dpad_down") { out_target = WheelTarget::kDpadDown; return true; }
  if (l == "left" || l == "dpad_left") { out_target = WheelTarget::kDpadLeft; return true; }
  if (l == "right" || l == "dpad_right") { out_target = WheelTarget::kDpadRight; return true; }
  if (l == "shift_up" || l == "shiftup") { out_target = WheelTarget::kShiftUp; return true; }
  if (l == "shift_down" || l == "shiftdown") { out_target = WheelTarget::kShiftDown; return true; }
  if (l == "handbrake" || l == "ebrake") { out_target = WheelTarget::kHandbrake; return true; }
  if (l == "clutch") { out_target = WheelTarget::kClutch; return true; }
  if (l == "camera") { out_target = WheelTarget::kCamera; return true; }
  if (l == "rewind") { out_target = WheelTarget::kRewind; return true; }
  if (l == "look_back" || l == "lookback") { out_target = WheelTarget::kLookBack; return true; }
  return false;
}

const UsbDeviceSnapshot* find_device(const std::vector<UsbDeviceSnapshot>& devices,
                                     std::string_view match, std::string_view fallback_match) {
  const std::string_view effective = match.empty() ? fallback_match : match;
  for (const auto& d : devices) {
    if (wheel_device_matches(effective, d.name, d.vendor, d.product, d.path)) return &d;
  }
  return nullptr;
}

}  // namespace

// ---- Single-device SDL wheel shaping ----

int16_t shape_wheel_steering_i16(int16_t raw, const WheelShaping& shaping) {
  if (raw == 0) return 0;
  double travel = raw < 0 ? -double(raw) / 32768.0 : double(raw) / 32767.0;
  travel = std::min(1.0, travel * 100.0 / double(std::clamp(shaping.steering_saturation, 10, 100)));
  if (shaping.steering_curve != 100) {
    travel = std::pow(travel, double(std::clamp(shaping.steering_curve, 50, 300)) / 100.0);
  }
  const double skip = double(std::clamp(shaping.steering_deadzone, 0, 50)) / 100.0;
  if (skip > 0.0 && travel > 0.0) travel = skip + (1.0 - skip) * travel;
  const double out = raw < 0 ? -travel * 32768.0 : travel * 32767.0;
  return static_cast<int16_t>(std::clamp(std::lround(out), -32768L, 32767L));
}

uint8_t shape_wheel_pedal_u8(uint8_t raw, const WheelShaping& shaping) {
  const int rest = std::clamp(shaping.pedal_deadzone, 0, 50) * 255 / 100;
  if (rest == 0) return raw;
  if (raw <= rest) return 0;
  return static_cast<uint8_t>(std::clamp(std::lround(double(raw - rest) * 255.0 / double(255 - rest)), 0L, 255L));
}

double shape_wheel_steering(double raw_signed, const WheelShaping& shaping) {
  const double clamped = std::clamp(raw_signed, -1.0, 1.0);
  if (std::fabs(clamped) < 1e-9) return 0.0;
  double travel = std::min(1.0, std::fabs(clamped) * 100.0 / double(std::clamp(shaping.steering_saturation, 10, 100)));
  if (shaping.steering_curve != 100) {
    travel = std::pow(travel, double(std::clamp(shaping.steering_curve, 50, 300)) / 100.0);
  }
  const double skip = double(std::clamp(shaping.steering_deadzone, 0, 50)) / 100.0;
  if (skip > 0.0 && travel > 0.0) travel = skip + (1.0 - skip) * travel;
  return clamped < 0.0 ? -travel : travel;
}

double shape_wheel_pedal(double raw_unit, const WheelShaping& shaping) {
  const double clamped = std::clamp(raw_unit, 0.0, 1.0);
  const double rest = double(std::clamp(shaping.pedal_deadzone, 0, 50)) / 100.0;
  if (rest <= 0.0) return clamped;
  if (clamped <= rest) return 0.0;
  return std::clamp((clamped - rest) / (1.0 - rest), 0.0, 1.0);
}

// ---- Separate Devices parsing & evaluation ----

std::string WheelAxisBinding::format() const {
  if (axis < 0) return {};
  std::string s;
  if (!device.empty()) s += device + "|";
  if (direction == AxisDirection::kNormal) s += "+";
  else if (direction == AxisDirection::kInverted) s += "-";
  s += "axis" + std::to_string(axis);
  return s;
}

std::string WheelButtonBinding::format() const {
  std::string s = target_name(target, gear);
  if (s.empty()) return {};
  s += "=";
  if (!device.empty()) s += device + "|";
  if (axis >= 0) {
    if (axis_direction == AxisDirection::kNormal) s += "+";
    else if (axis_direction == AxisDirection::kInverted) s += "-";
    s += "axis" + std::to_string(axis);
  } else if (hat >= 0) {
    s += "h" + std::to_string(hat) + ".";
    if (hat_mask & 0x01) s += "up";
    else if (hat_mask & 0x02) s += "right";
    else if (hat_mask & 0x04) s += "down";
    else if (hat_mask & 0x08) s += "left";
  } else if (button >= 0) {
    s += std::to_string(button);
  }
  return s;
}

std::optional<WheelAxisBinding> parse_wheel_axis_binding(std::string_view text) {
  text = trim_sv(text);
  if (text.empty()) return std::nullopt;
  WheelAxisBinding b;
  std::string_view rest;
  split_device(text, b.device, rest);
  if (!rest.empty() && (rest.front() == '+' || rest.front() == '-')) {
    b.direction = rest.front() == '+' ? AxisDirection::kNormal : AxisDirection::kInverted;
    rest.remove_prefix(1);
  }
  if (starts_with_nocase(rest, "axis")) rest.remove_prefix(4);
  else if (starts_with_nocase(rest, "a")) rest.remove_prefix(1);
  else return std::nullopt;
  auto idx = parse_index(rest);
  if (!idx) return std::nullopt;
  b.axis = *idx;
  return b;
}

std::vector<WheelButtonBinding> parse_wheel_button_bindings(std::string_view text) {
  std::vector<WheelButtonBinding> out;
  bool have_target = false;
  WheelTarget cur_target = WheelTarget::kNone;
  int8_t cur_gear = 0;

  size_t start = 0;
  while (start <= text.size()) {
    size_t comma = text.find(',', start);
    if (comma == std::string_view::npos) comma = text.size();
    std::string_view part = trim_sv(text.substr(start, comma - start));
    start = comma + 1;
    if (part.empty()) {
      if (comma == text.size()) break;
      continue;
    }
    std::string_view input = part;
    const size_t eq = part.find('=');
    if (eq != std::string_view::npos) {
      have_target = parse_target(part.substr(0, eq), cur_target, cur_gear);
      input = part.substr(eq + 1);
    }
    if (!have_target) {
      if (comma == text.size()) break;
      continue;
    }
    WheelButtonBinding b;
    std::string_view rest;
    split_device(input, b.device, rest);
    b.target = cur_target;
    b.gear = cur_gear;

    std::string_view axis_text = rest;
    AxisDirection adir = AxisDirection::kAuto;
    if (!axis_text.empty() && (axis_text.front() == '+' || axis_text.front() == '-')) {
      adir = axis_text.front() == '+' ? AxisDirection::kNormal : AxisDirection::kInverted;
      axis_text.remove_prefix(1);
    }
    if (starts_with_nocase(axis_text, "axis")) {
      auto axis = parse_index(axis_text.substr(4));
      if (axis) {
        b.axis = *axis;
        b.axis_direction = adir;
        out.push_back(std::move(b));
      }
    } else if (starts_with_nocase(rest, "h")) {
      std::string_view h = rest.substr(1);
      const size_t dot = h.find('.');
      if (dot != std::string_view::npos) {
        auto hat = parse_index(h.substr(0, dot));
        const std::string dir = to_lower(trim_sv(h.substr(dot + 1)));
        uint8_t mask = 0;
        if (dir == "up" || dir == "1") mask = 0x01;
        else if (dir == "right" || dir == "2") mask = 0x02;
        else if (dir == "down" || dir == "4") mask = 0x04;
        else if (dir == "left" || dir == "8") mask = 0x08;
        if (hat && mask) {
          b.hat = *hat;
          b.hat_mask = mask;
          out.push_back(std::move(b));
        }
      }
    } else {
      if (starts_with_nocase(rest, "b")) rest.remove_prefix(1);
      auto btn = parse_index(rest);
      if (btn) {
        b.button = *btn;
        out.push_back(std::move(b));
      }
    }
    if (comma == text.size()) break;
  }
  return out;
}

std::string format_wheel_button_bindings(const std::vector<WheelButtonBinding>& bindings) {
  std::string out;
  for (const auto& b : bindings) {
    const std::string f = b.format();
    if (f.empty()) continue;
    if (!out.empty()) out += ',';
    out += f;
  }
  return out;
}

bool wheel_device_matches(std::string_view match, std::string_view name, uint16_t vendor,
                          uint16_t product, std::string_view path) {
  match = trim_sv(match);
  if (match.empty()) return true;
  if (starts_with_nocase(match, "path:")) {
    return !path.empty() && to_lower(trim_sv(match.substr(5))) == to_lower(path);
  }
  if (match.size() == 9 && match[4] == ':') {
    auto parse_hex = [](std::string_view d, uint16_t& val) {
      unsigned p = 0;
      auto [end, ec] = std::from_chars(d.data(), d.data() + d.size(), p, 16);
      if (ec != std::errc() || end != d.data() + d.size() || p > 0xFFFF) return false;
      val = static_cast<uint16_t>(p);
      return true;
    };
    uint16_t v = 0, p = 0;
    if (parse_hex(match.substr(0, 4), v) && parse_hex(match.substr(5, 4), p)) {
      return v == vendor && p == product;
    }
  }
  return to_lower(name).find(to_lower(match)) != std::string::npos;
}

bool is_known_wheel_vendor(uint16_t vendor) {
  switch (vendor) {
    case 0x046D:  // Logitech
    case 0x044F:  // Thrustmaster
    case 0x0EB7:  // Fanatec
    case 0x346E:  // Moza
    case 0x2433:  // Asetek SimSports
    case 0x3416:  // Cammus
    case 0x16D0:  // Simucube / Simagic
      return true;
    default:
      return false;
  }
}

AxisDirection resolve_axis_direction(AxisDirection dir, int16_t rest_value) {
  if (dir != AxisDirection::kAuto) return dir;
  return rest_value > 0 ? AxisDirection::kInverted : AxisDirection::kNormal;
}

int16_t steering_from_axis(int16_t raw, bool invert, int scale_percent, int deadzone_percent) {
  int32_t v = raw;
  if (invert) v = -v - 1;
  const int32_t dz = std::clamp(deadzone_percent, 0, 50) * 32767 / 100;
  const int32_t mag = std::abs(v);
  if (mag <= dz) return 0;
  int64_t scaled = int64_t(mag - dz) * 32767 / std::max(32767 - dz, 1);
  scaled = scaled * std::clamp(scale_percent, 10, 1000) / 100;
  scaled = std::min<int64_t>(scaled, 32767);
  return static_cast<int16_t>(v < 0 ? -scaled : scaled);
}

uint8_t pedal_from_axis(int16_t raw, AxisDirection dir, int bottom_deadzone_pct, int top_deadzone_pct) {
  int32_t travel = int32_t(raw) + 32768;  // 0..65535
  if (dir == AxisDirection::kInverted) travel = 65535 - travel;
  const int32_t bot = std::clamp(bottom_deadzone_pct, 0, 50) * 65535 / 100;
  const int32_t top = std::clamp(top_deadzone_pct, 0, 50) * 65535 / 100;
  const int32_t span = std::max(65535 - bot - top, 1);
  if (travel <= bot) return 0;
  if (travel >= 65535 - top) return 255;
  const int32_t out = (travel - bot) * 255 / span;
  return static_cast<uint8_t>(std::clamp(out, 0, 255));
}

SeparateWheelOutput evaluate_separate_wheel(const SeparateWheelConfig& cfg,
                                            const std::vector<UsbDeviceSnapshot>& devices) {
  SeparateWheelOutput out;
  if (!cfg.enabled || devices.empty()) return out;

  const std::string& base_dev = cfg.steering.device;
  auto read_pedal = [&](const WheelAxisBinding& bind, int dz_bot, int dz_top, double& dst) {
    if (!bind.valid()) return false;
    const UsbDeviceSnapshot* d = find_device(devices, bind.device, base_dev);
    if (!d || size_t(bind.axis) >= d->axes.size()) return false;
    const int16_t rest = size_t(bind.axis) < d->rest_axes.size() ? d->rest_axes[size_t(bind.axis)] : 0;
    const AxisDirection dir = resolve_axis_direction(bind.direction, rest);
    dst = double(pedal_from_axis(d->axes[size_t(bind.axis)], dir, dz_bot, dz_top)) / 255.0;
    return true;
  };

  if (cfg.steering.valid()) {
    if (const UsbDeviceSnapshot* d = find_device(devices, cfg.steering.device, {})) {
      if (size_t(cfg.steering.axis) < d->axes.size()) {
        const int16_t raw = d->axes[size_t(cfg.steering.axis)];
        const bool inv = cfg.invert_steering || cfg.steering.direction == AxisDirection::kInverted;
        // Scale 900-degree linear mapping if wheel_rotation_deg != 900.
        const int rot_scale = cfg.steering_scale_pct * 900 / std::clamp(cfg.wheel_rotation_deg, 180, 1440);
        const int16_t s = steering_from_axis(raw, inv, rot_scale, cfg.steering_deadzone_pct);
        out.steer = s < 0 ? double(s) / 32768.0 : double(s) / 32767.0;
        out.active = true;
        out.linear_900 = cfg.linear_900_deg;
      }
    }
  }

  if (read_pedal(cfg.throttle, cfg.throttle_dz_bottom, cfg.throttle_dz_top, out.throttle)) out.active = true;
  if (read_pedal(cfg.brake, cfg.brake_dz_bottom, cfg.brake_dz_top, out.brake)) out.active = true;
  if (read_pedal(cfg.clutch, cfg.clutch_dz_bottom, cfg.clutch_dz_top, out.clutch)) out.active = true;
  if (read_pedal(cfg.handbrake, cfg.handbrake_dz_bottom, cfg.handbrake_dz_top, out.handbrake)) out.active = true;

  for (const auto& b : cfg.buttons) {
    if (b.target == WheelTarget::kGear) out.has_h_shifter = true;
    const UsbDeviceSnapshot* d = find_device(devices, b.device, base_dev);
    if (!d) continue;
    bool pressed = false;
    if (b.button >= 0 && size_t(b.button) < d->buttons.size()) {
      pressed = d->buttons[size_t(b.button)] != 0;
    } else if (b.hat >= 0 && size_t(b.hat) < d->hats.size()) {
      pressed = (d->hats[size_t(b.hat)] & b.hat_mask) != 0;
    } else if (b.axis >= 0 && size_t(b.axis) < d->axes.size()) {
      const int16_t rest = size_t(b.axis) < d->rest_axes.size() ? d->rest_axes[size_t(b.axis)] : 0;
      const AxisDirection dir = resolve_axis_direction(b.axis_direction, rest);
      pressed = pedal_from_axis(d->axes[size_t(b.axis)], dir, 10, 0) >= 128;
    }
    if (!pressed) continue;
    out.active = true;
    switch (b.target) {
      case WheelTarget::kGear: out.h_shifter_gear = b.gear; break;
      case WheelTarget::kShiftUp:
      case WheelTarget::kRightShoulder: out.shift_up = true; break;
      case WheelTarget::kShiftDown:
      case WheelTarget::kLeftShoulder: out.shift_down = true; break;
      case WheelTarget::kHandbrake: out.handbrake = 1.0; break;
      case WheelTarget::kClutch: out.clutch = 1.0; break;
      case WheelTarget::kButtonA: out.button_a = true; break;
      case WheelTarget::kButtonB: out.button_b = true; break;
      case WheelTarget::kButtonX: out.button_x = true; break;
      case WheelTarget::kButtonY: out.button_y = true; break;
      case WheelTarget::kStart: out.start = true; break;
      case WheelTarget::kBack: out.back = true; break;
      case WheelTarget::kCamera: out.camera = true; break;
      case WheelTarget::kRewind: out.rewind = true; break;
      case WheelTarget::kLookBack: out.look_back = true; break;
      case WheelTarget::kDpadUp: out.dpad_up = true; break;
      case WheelTarget::kDpadDown: out.dpad_down = true; break;
      case WheelTarget::kDpadLeft: out.dpad_left = true; break;
      case WheelTarget::kDpadRight: out.dpad_right = true; break;
      default: break;
    }
  }
  return out;
}

int resolve_wheel_shift(const SeparateWheelOutput& out, int current_gear, bool prev_shift_up,
                        bool prev_shift_down) {
  if (out.has_h_shifter && out.h_shifter_gear != 0) {
    if (out.h_shifter_gear > current_gear) return +1;
    if (out.h_shifter_gear < current_gear) return -1;
    return 0;
  }
  if (out.shift_up && !prev_shift_up) return +1;
  if (out.shift_down && !prev_shift_down) return -1;
  return 0;
}

// ---- Force Feedback model ----

const char* wheel_ffb_mode_name(WheelFfbMode m) {
  switch (m) {
    case WheelFfbMode::kOff: return "off";
    case WheelFfbMode::kRumbleOnly: return "rumble";
    case WheelFfbMode::kFull: return "full";
  }
  return "off";
}

WheelFfbMode parse_wheel_ffb_mode(std::string_view text) {
  const std::string l = to_lower(trim_sv(text));
  if (l == "full" || l == "on" || l == "1") return WheelFfbMode::kFull;
  if (l == "rumble" || l == "rumble_only" || l == "rumble-only") return WheelFfbMode::kRumbleOnly;
  return WheelFfbMode::kOff;
}

WheelFfbTelemetry ffb_telemetry_from_vehicle(const Vehicle& v, bool driving, double lateral_g) {
  WheelFfbTelemetry t;
  t.driving = driving;
  t.speed_mps = v.speed();
  t.steer_angle_rad = v.steer_angle();
  t.steer_input = v.shaped_input().steer;
  t.throttle_input = v.shaped_input().throttle;
  t.brake_input = v.shaped_input().brake;
  const auto& w = v.wheels();
  t.front_slip_angle = 0.5 * (w[0].slip_angle + w[1].slip_angle);
  t.front_slip_ratio = 0.5 * (w[0].slip_ratio + w[1].slip_ratio);
  t.front_load_n = w[0].load + w[1].load;
  t.static_front_load_n = std::max(100.0, v.static_load(0) + v.static_load(1));
  t.lateral_g = lateral_g;
  t.rpm = v.rpm();
  t.redline_rpm = v.params().redline_rpm;
  t.surface_bump_rumble = v.surface_bump_rumble();
  t.body_contact_force = v.body_contact_force();
  return t;
}

void WheelFfbSolver::start_test_pull() {
  test_timer_ = 1.0;  // 1.0..0.6 left, 0.6..0.4 pause, 0.4..0.0 right
}

void WheelFfbSolver::reset() {
  rumble_phase_ = 0.0;
  smooth_align_ = 0.0;
  test_timer_ = 0.0;
}

WheelFfbForces WheelFfbSolver::step(const WheelFfbTelemetry& tel, const WheelFfbConfig& cfg, double dt_s) {
  WheelFfbForces out;
  const double dt = std::clamp(dt_s, 0.0, 0.1);
  const double master = double(std::clamp(cfg.strength_percent, 0, 100)) / 100.0;
  const double dir = cfg.invert ? -1.0 : 1.0;

  // Test pull runs even while paused/in settings so the player can verify wheel direction.
  if (test_timer_ > 0.0) {
    test_timer_ = std::max(0.0, test_timer_ - dt);
    double pull = 0.0;
    if (test_timer_ > 0.6) pull = -0.35;      // pull left first
    else if (test_timer_ < 0.4) pull = +0.35; // then pull right
    out.active = true;
    out.aligning_norm = pull;
    out.constant_force = static_cast<int32_t>(std::clamp(std::lround(pull * dir * std::max(master, 0.25) * 10000.0),
                                                         -10000L, 10000L));
    return out;
  }

  if (cfg.mode == WheelFfbMode::kOff || !tel.driving || master <= 0.0) {
    smooth_align_ = 0.0;
    return out;
  }

  out.active = true;
  const double speed = std::fabs(tel.speed_mps);
  const double speed_norm = std::clamp(speed / 55.0, 0.0, 1.0);  // ~123 mph full scale

  // Engine rumble grows smoothly towards redline (matching FH1 ControllerFFB).
  const double rpm_frac = tel.redline_rpm > 1.0 ? std::clamp(tel.rpm / tel.redline_rpm, 0.0, 1.0) : 0.0;
  const double engine_raw = std::pow(rpm_frac, 2.2) * (0.25 + 0.75 * tel.throttle_input);
  out.engine_rumble = std::clamp(engine_raw * (double(std::clamp(cfg.engine_rumble_percent, 0, 100)) / 100.0),
                                 0.0, 1.0);

  // Road rumble: surface bumpiness + body impacts, scaled by speed for surface bumps.
  const double impact = std::clamp(tel.body_contact_force / 25000.0, 0.0, 1.0);
  const double bump = std::clamp(tel.surface_bump_rumble * std::clamp(speed / 8.0, 0.0, 1.0), 0.0, 1.0);
  const double road_raw = std::clamp(std::max(bump, impact), 0.0, 1.0);
  out.road_rumble = std::clamp(road_raw * (double(std::clamp(cfg.road_rumble_percent, 0, 100)) / 100.0),
                               0.0, 1.0);

  // Rumble oscillation injected onto constant force for bases without separate motors.
  const double rumble_freq = 18.0 + 28.0 * rpm_frac;
  rumble_phase_ = std::fmod(rumble_phase_ + dt * rumble_freq, 1.0);
  const double osc = std::sin(kTwoPi * rumble_phase_);
  const double rumble_force = osc * (0.14 * out.engine_rumble + 0.30 * out.road_rumble);

  if (cfg.mode == WheelFfbMode::kRumbleOnly) {
    out.constant_force = static_cast<int32_t>(
        std::clamp(std::lround(rumble_force * dir * master * 10000.0), -10000L, 10000L));
    return out;
  }

  // Condition effects: inertia, friction, damper, speed-stiffening spring.
  const auto scale_cond = [&](double base, int pct) -> int32_t {
    const double s = double(std::clamp(pct, 0, 200)) / 100.0;
    return static_cast<int32_t>(std::clamp(std::lround(base * s * master * 10000.0), 0L, 10000L));
  };
  // Friction fades down as the car rolls (heavy static steering resistance when parked).
  const double static_fric = 0.28 * (1.0 - 0.70 * std::clamp(speed / 12.0, 0.0, 1.0));
  const double speed_spring = 0.15 + 0.65 * speed_norm;
  const double damper = 0.22 + 0.18 * speed_norm;
  const double inertia = 0.18;

  out.inertia_coeff = scale_cond(inertia, cfg.inertia_percent);
  out.friction_coeff = scale_cond(static_fric, cfg.friction_percent);
  out.damper_coeff = scale_cond(damper, cfg.damper_percent);
  out.spring_coeff = scale_cond(speed_spring, cfg.spring_percent);

  // Dynamic self-aligning torque from front tyre slip angle + front load + lateral G.
  // Past peak grip (|slip| > 1.0), aligning torque drops off naturally (understeer lightness).
  const double load_ratio = std::clamp(tel.front_load_n / std::max(100.0, tel.static_front_load_n), 0.0, 2.0);
  const double sa = tel.front_slip_angle;
  const double abs_sa = std::fabs(sa);
  const double pneumatic_trail = abs_sa <= 1.0 ? abs_sa : std::max(0.25, 1.0 - 0.35 * (abs_sa - 1.0));
  const double slip_align = (sa < 0.0 ? -1.0 : (sa > 0.0 ? 1.0 : 0.0)) * pneumatic_trail;
  // Lockup under heavy front braking lightens steering feel slightly.
  const double lock_fade = 1.0 - 0.35 * std::clamp(std::fabs(tel.front_slip_ratio) - 0.8, 0.0, 1.0);
  const double low_speed_gate = std::clamp(speed / 3.0, 0.0, 1.0);
  const double raw_align =
      -(0.72 * slip_align * load_ratio * lock_fade + 0.22 * std::clamp(tel.lateral_g / 1.5, -1.0, 1.0)) *
      low_speed_gate;

  const double alpha = dt > 0.0 ? (1.0 - std::exp(-dt * 35.0)) : 1.0;
  smooth_align_ += (raw_align - smooth_align_) * alpha;

  const double align_scale = double(std::clamp(cfg.align_percent, 0, 200)) / 100.0;
  double combined = std::clamp(smooth_align_ * align_scale + rumble_force, -1.0, 1.0);
  out.aligning_norm = std::clamp(smooth_align_ * align_scale, -1.0, 1.0);

  // Min-force lift so small aligning cues overcome geared/belted wheel static friction.
  const double min_f = double(std::clamp(cfg.min_force_percent, 0, 30)) / 100.0;
  if (min_f > 0.0 && std::fabs(combined) > 1e-4) {
    const double lifted = min_f + (1.0 - min_f) * std::fabs(combined);
    combined = combined < 0.0 ? -lifted : lifted;
  }

  out.constant_force =
      static_cast<int32_t>(std::clamp(std::lround(combined * dir * master * 10000.0), -10000L, 10000L));
  return out;
}

// ---- FFB Trace CSV recorder ----

FfbTraceRecorder::~FfbTraceRecorder() { stop(); }

bool FfbTraceRecorder::start(const std::filesystem::path& directory, std::string* created_path) {
  stop();
  std::error_code ec;
  if (!directory.empty()) std::filesystem::create_directories(directory, ec);
  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &tt);
#else
  localtime_r(&tt, &tm_buf);
#endif
  char stamp[32];
  std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1,
                tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
  const auto path = directory / (std::string("ffb-trace-") + stamp + ".csv");
  file_.open(path, std::ios::out | std::ios::trunc);
  if (!file_) return false;
  rows_ = 0;
  file_ << "time_s,mode,steer,throttle,brake,speed_mps,front_slip,front_load_n,"
           "constant_force,spring,damper,inertia,friction,engine_rumble,road_rumble\n";
  if (created_path) *created_path = path.string();
  return bool(file_);
}

void FfbTraceRecorder::stop() {
  if (file_.is_open()) {
    file_.flush();
    file_.close();
  }
}

void FfbTraceRecorder::record(double time_s, WheelFfbMode mode, const WheelFfbTelemetry& tel,
                              const WheelFfbForces& forces) {
  if (!file_.is_open()) return;
  file_ << std::fixed << std::setprecision(4) << time_s << ',' << wheel_ffb_mode_name(mode) << ','
        << tel.steer_input << ',' << tel.throttle_input << ',' << tel.brake_input << ',' << tel.speed_mps << ','
        << tel.front_slip_angle << ',' << tel.front_load_n << ',' << forces.constant_force << ','
        << forces.spring_coeff << ',' << forces.damper_coeff << ',' << forces.inertia_coeff << ','
        << forces.friction_coeff << ',' << forces.engine_rumble << ',' << forces.road_rumble << '\n';
  ++rows_;
}

// ---- Windows DirectInput8 Force Feedback backend ----

struct DirectInputFfbDevice::Impl {
#if defined(_WIN32) && defined(SHIFT_HAS_DINPUT)
  using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
  HMODULE dinput_dll = nullptr;
  IDirectInput8A* di = nullptr;
  IDirectInputDevice8A* dev = nullptr;
  IDirectInputEffect* fx_const = nullptr;
  IDirectInputEffect* fx_spring = nullptr;
  IDirectInputEffect* fx_damper = nullptr;
  IDirectInputEffect* fx_inertia = nullptr;
  IDirectInputEffect* fx_friction = nullptr;
  std::string name;
  bool started = false;

  void release_effects() {
    auto rel = [](IDirectInputEffect*& e) {
      if (e) {
        e->Stop();
        e->Release();
        e = nullptr;
      }
    };
    rel(fx_const);
    rel(fx_spring);
    rel(fx_damper);
    rel(fx_inertia);
    rel(fx_friction);
    started = false;
  }

  IDirectInputEffect* create_cond(REFGUID guid) {
    if (!dev) return nullptr;
    DWORD axis = DIJOFS_X;
    LONG dir = 0;
    DICONDITION cond{};
    cond.lOffset = 0;
    cond.lPositiveCoefficient = 0;
    cond.lNegativeCoefficient = 0;
    cond.dwPositiveSaturation = 10000;
    cond.dwNegativeSaturation = 10000;
    cond.lDeadBand = 0;
    DIEFFECT eff{};
    eff.dwSize = sizeof(eff);
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration = INFINITE;
    eff.dwGain = DI_FFNOMINALMAX;
    eff.dwTriggerButton = DIEB_NOTRIGGER;
    eff.cAxes = 1;
    eff.rgdwAxes = &axis;
    eff.rglDirection = &dir;
    eff.cbTypeSpecificParams = sizeof(cond);
    eff.lpvTypeSpecificParams = &cond;
    IDirectInputEffect* out = nullptr;
    if (FAILED(dev->CreateEffect(guid, &eff, &out, nullptr))) return nullptr;
    return out;
  }

  void update_cond(IDirectInputEffect* e, int32_t coeff) {
    if (!e) return;
    DICONDITION cond{};
    cond.lOffset = 0;
    cond.lPositiveCoefficient = std::clamp<LONG>(coeff, 0, 10000);
    cond.lNegativeCoefficient = std::clamp<LONG>(coeff, 0, 10000);
    cond.dwPositiveSaturation = 10000;
    cond.dwNegativeSaturation = 10000;
    cond.lDeadBand = 0;
    DIEFFECT eff{};
    eff.dwSize = sizeof(eff);
    eff.cbTypeSpecificParams = sizeof(cond);
    eff.lpvTypeSpecificParams = &cond;
    e->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS | DIEP_NORESTART);
  }
#endif
};

DirectInputFfbDevice::DirectInputFfbDevice() : impl_(std::make_unique<Impl>()) {}
DirectInputFfbDevice::~DirectInputFfbDevice() { close(); }

bool DirectInputFfbDevice::open(void* native_hwnd, std::string_view device_match) {
  close();
#if defined(_WIN32) && defined(SHIFT_HAS_DINPUT)
  impl_->dinput_dll = LoadLibraryA("dinput8.dll");
  if (!impl_->dinput_dll) return false;
  auto create_fn = reinterpret_cast<Impl::DirectInput8CreateFn>(
      GetProcAddress(impl_->dinput_dll, "DirectInput8Create"));
  if (!create_fn) {
    close();
    return false;
  }
  if (FAILED(create_fn(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
                       reinterpret_cast<void**>(&impl_->di), nullptr)) ||
      !impl_->di) {
    close();
    return false;
  }

  struct EnumCtx {
    std::string_view match;
    GUID chosen{};
    std::string name;
    bool found = false;
  } ctx{device_match, {}, {}, false};

  impl_->di->EnumDevices(
      DI8DEVCLASS_GAMECTRL,
      [](LPCDIDEVICEINSTANCEA inst, LPVOID ref) -> BOOL {
        auto* c = static_cast<EnumCtx*>(ref);
        const uint16_t vid = static_cast<uint16_t>(inst->guidProduct.Data1 & 0xFFFF);
        const uint16_t pid = static_cast<uint16_t>((inst->guidProduct.Data1 >> 16) & 0xFFFF);
        if (wheel_device_matches(c->match, inst->tszProductName, vid, pid)) {
          c->chosen = inst->guidInstance;
          c->name = inst->tszProductName;
          c->found = true;
          return DIENUM_STOP;
        }
        return DIENUM_CONTINUE;
      },
      &ctx, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);

  if (!ctx.found) {
    close();
    return false;
  }
  if (FAILED(impl_->di->CreateDevice(ctx.chosen, &impl_->dev, nullptr)) || !impl_->dev) {
    close();
    return false;
  }
  impl_->dev->SetDataFormat(&c_dfDIJoystick);
  const HWND hwnd = native_hwnd ? static_cast<HWND>(native_hwnd) : GetDesktopWindow();
  if (FAILED(impl_->dev->SetCooperativeLevel(hwnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND))) {
    impl_->dev->SetCooperativeLevel(hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
  }
  // Disable DirectInput auto-center spring so our speed-stiffening spring owns it.
  DIPROPDWORD autocenter{};
  autocenter.diph.dwSize = sizeof(DIPROPDWORD);
  autocenter.diph.dwHeaderSize = sizeof(DIPROPHEADER);
  autocenter.diph.dwObj = 0;
  autocenter.diph.dwHow = DIPH_DEVICE;
  autocenter.dwData = DIPROPAUTOCENTER_OFF;
  impl_->dev->SetProperty(DIPROP_AUTOCENTER, &autocenter.diph);
  impl_->dev->Acquire();

  // Create Constant Force + 4 Condition effects.
  {
    DWORD axis = DIJOFS_X;
    LONG dir = 0;
    DICONSTANTFORCE cf{0};
    DIEFFECT eff{};
    eff.dwSize = sizeof(eff);
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration = INFINITE;
    eff.dwGain = DI_FFNOMINALMAX;
    eff.dwTriggerButton = DIEB_NOTRIGGER;
    eff.cAxes = 1;
    eff.rgdwAxes = &axis;
    eff.rglDirection = &dir;
    eff.cbTypeSpecificParams = sizeof(cf);
    eff.lpvTypeSpecificParams = &cf;
    impl_->dev->CreateEffect(GUID_ConstantForce, &eff, &impl_->fx_const, nullptr);
  }
  impl_->fx_spring = impl_->create_cond(GUID_Spring);
  impl_->fx_damper = impl_->create_cond(GUID_Damper);
  impl_->fx_inertia = impl_->create_cond(GUID_Inertia);
  impl_->fx_friction = impl_->create_cond(GUID_Friction);
  impl_->name = ctx.name;
  return impl_->fx_const != nullptr;
#else
  (void)native_hwnd;
  (void)device_match;
  return false;
#endif
}

void DirectInputFfbDevice::close() {
#if defined(_WIN32) && defined(SHIFT_HAS_DINPUT)
  if (!impl_) return;
  stop_all();
  impl_->release_effects();
  if (impl_->dev) {
    impl_->dev->Unacquire();
    impl_->dev->Release();
    impl_->dev = nullptr;
  }
  if (impl_->di) {
    impl_->di->Release();
    impl_->di = nullptr;
  }
  if (impl_->dinput_dll) {
    FreeLibrary(impl_->dinput_dll);
    impl_->dinput_dll = nullptr;
  }
  impl_->name.clear();
#endif
}

bool DirectInputFfbDevice::is_open() const {
#if defined(_WIN32) && defined(SHIFT_HAS_DINPUT)
  return impl_ && impl_->dev != nullptr;
#else
  return false;
#endif
}

std::string DirectInputFfbDevice::device_name() const {
#if defined(_WIN32) && defined(SHIFT_HAS_DINPUT)
  return impl_ ? impl_->name : std::string{};
#else
  return {};
#endif
}

void DirectInputFfbDevice::apply(const WheelFfbForces& forces) {
#if defined(_WIN32) && defined(SHIFT_HAS_DINPUT)
  if (!impl_ || !impl_->dev) return;
  if (!forces.active) {
    stop_all();
    return;
  }
  impl_->dev->Acquire();
  if (impl_->fx_const) {
    DICONSTANTFORCE cf{std::clamp<LONG>(forces.constant_force, -10000, 10000)};
    DIEFFECT eff{};
    eff.dwSize = sizeof(eff);
    eff.cbTypeSpecificParams = sizeof(cf);
    eff.lpvTypeSpecificParams = &cf;
    impl_->fx_const->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS | DIEP_NORESTART);
  }
  impl_->update_cond(impl_->fx_spring, forces.spring_coeff);
  impl_->update_cond(impl_->fx_damper, forces.damper_coeff);
  impl_->update_cond(impl_->fx_inertia, forces.inertia_coeff);
  impl_->update_cond(impl_->fx_friction, forces.friction_coeff);
  if (!impl_->started) {
    auto start_one = [](IDirectInputEffect* e) {
      if (e) e->Start(1, 0);
    };
    start_one(impl_->fx_const);
    start_one(impl_->fx_spring);
    start_one(impl_->fx_damper);
    start_one(impl_->fx_inertia);
    start_one(impl_->fx_friction);
    impl_->started = true;
  }
#else
  (void)forces;
#endif
}

void DirectInputFfbDevice::stop_all() {
#if defined(_WIN32) && defined(SHIFT_HAS_DINPUT)
  if (!impl_ || !impl_->dev) return;
  impl_->dev->SendForceFeedbackCommand(DISFFC_STOPALL);
  impl_->started = false;
#endif
}

}  // namespace drive
