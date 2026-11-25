#include "drive/controls.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace drive {
namespace {

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

void clamp_pair(double& inside, double& outside) {
  inside = std::clamp(inside, 0.0, 0.90);
  outside = std::clamp(outside, inside + 0.05, 1.0);
}

constexpr const char* kActionNames[size_t(ControlAction::kCount)] = {
    "steer-left", "steer-right", "throttle",  "brake",      "handbrake",  "clutch",
    "shift-up",   "shift-down",  "camera",    "look-back",  "rewind",     "reset",
    "map",        "radio-prev",  "radio-next", "activate",  "look-left",  "look-right",
    "photo-mode", "telemetry",
};

}  // namespace

const char* control_action_name(ControlAction a) {
  const size_t i = size_t(a);
  return i < size_t(ControlAction::kCount) ? kActionNames[i] : "";
}

bool parse_control_action(std::string_view name, ControlAction& out) {
  for (size_t i = 0; i < size_t(ControlAction::kCount); ++i) {
    if (iequals(name, kActionNames[i])) {
      out = ControlAction(i);
      return true;
    }
  }
  return false;
}

void ControlDeadzones::clamp_valid() {
  clamp_pair(steering_inside, steering_outside);
  clamp_pair(throttle_inside, throttle_outside);
  clamp_pair(brake_inside, brake_outside);
  clamp_pair(clutch_inside, clutch_outside);
  clamp_pair(handbrake_inside, handbrake_outside);
}

ControlDeadzones ControlDeadzones::wheel_defaults() {
  ControlDeadzones d;
  d.steering_inside = 0.01, d.steering_outside = 0.95;
  d.throttle_inside = 0.15, d.throttle_outside = 1.00;
  d.brake_inside = 0.15, d.brake_outside = 0.95;
  d.clutch_inside = 0.15, d.clutch_outside = 0.90;
  d.handbrake_inside = 0.10, d.handbrake_outside = 1.00;
  return d;
}

double right_stick_look_target(double rx, double ry) {
  constexpr double kPi = 3.14159265358979323846;
  if (ry > 0.5 && ry > std::fabs(rx)) return kPi;
  if (std::fabs(rx) > 0.5 && std::fabs(rx) >= std::fabs(ry)) return rx > 0 ? 0.5 * kPi : -0.5 * kPi;
  return 0.0;
}

double apply_deadzone_signed(double raw, double inside, double outside) {
  const double a = std::fabs(raw);
  if (a <= inside) return 0.0;
  const double span = std::max(1e-6, outside - inside);
  const double mag = std::clamp((a - inside) / span, 0.0, 1.0);
  return raw < 0.0 ? -mag : mag;
}

double apply_deadzone_unsigned(double raw, double inside, double outside) {
  if (raw <= inside) return 0.0;
  const double span = std::max(1e-6, outside - inside);
  return std::clamp((raw - inside) / span, 0.0, 1.0);
}

PadControl ControllerLayout::control_for(std::string_view action) const {
  for (size_t i = 1; i < size_t(PadControl::kCount); ++i) {
    if (iequals(bindings[i], action)) return PadControl(i);
  }
  return PadControl::kNone;
}

std::string_view ControllerLayout::action_for(PadControl ctrl) const {
  const size_t i = size_t(ctrl);
  return i < size_t(PadControl::kCount) ? std::string_view(bindings[i]) : std::string_view{};
}

ControllerLayout default_controller_layout(int layout_id) {
  const int id = std::clamp(layout_id, 0, kControllerLayoutCount - 1);
  ControllerLayout l;
  l.id = id;
  l.name = "Layout " + std::to_string(id + 1);
  auto set = [&](PadControl c, const char* act) { l.bindings[size_t(c)] = act; };
  // Common across all 5 ControllerLayouts rows.
  set(PadControl::kRightStickClick, "LookStraightBack");
  set(PadControl::kDPadUp, "PhotoMode");
  set(PadControl::kDPadDown, "Telemetry");
  set(PadControl::kDPadLeft, "RadioLeft");
  set(PadControl::kDPadRight, "RadioRight");
  set(PadControl::kBack, "Map");
  switch (id) {
    case 0:
      set(PadControl::kButtonA, "Handbrake");
      set(PadControl::kButtonB, "ShiftUp");
      set(PadControl::kButtonX, "ShiftDown");
      set(PadControl::kButtonY, "Rewind");
      set(PadControl::kLeftShoulder, "Clutch");
      set(PadControl::kRightShoulder, "SwitchCamera");
      set(PadControl::kLeftTrigger, "Brake");
      set(PadControl::kRightTrigger, "Gas");
      break;
    case 1:
      set(PadControl::kButtonA, "ShiftDown");
      set(PadControl::kButtonB, "Handbrake");
      set(PadControl::kButtonX, "ShiftUp");
      set(PadControl::kButtonY, "Rewind");
      set(PadControl::kLeftShoulder, "Clutch");
      set(PadControl::kRightShoulder, "SwitchCamera");
      set(PadControl::kLeftTrigger, "Brake");
      set(PadControl::kRightTrigger, "Gas");
      break;
    case 2:
      set(PadControl::kButtonA, "Clutch");
      set(PadControl::kButtonB, "Handbrake");
      set(PadControl::kButtonX, "Rewind");
      set(PadControl::kButtonY, "SwitchCamera");
      set(PadControl::kLeftShoulder, "ShiftDown");
      set(PadControl::kRightShoulder, "ShiftUp");
      set(PadControl::kLeftTrigger, "Brake");
      set(PadControl::kRightTrigger, "Gas");
      break;
    case 3:
      set(PadControl::kButtonA, "Gas");
      set(PadControl::kButtonB, "Handbrake");
      set(PadControl::kButtonX, "Brake");
      set(PadControl::kButtonY, "Rewind");
      set(PadControl::kLeftShoulder, "Clutch");
      set(PadControl::kRightShoulder, "SwitchCamera");
      set(PadControl::kLeftTrigger, "ShiftDown");
      set(PadControl::kRightTrigger, "ShiftUp");
      break;
    case 4:
      set(PadControl::kButtonA, "Handbrake");
      set(PadControl::kButtonB, "ShiftUp");
      set(PadControl::kButtonX, "ShiftDown");
      set(PadControl::kButtonY, "Rewind");
      set(PadControl::kLeftShoulder, "SwitchCamera");
      set(PadControl::kRightShoulder, "Clutch");
      set(PadControl::kLeftTrigger, "Gas");
      set(PadControl::kRightTrigger, "Brake");
      break;
  }
  return l;
}

ControllerLayout apply_handbrake_clutch_swap(const ControllerLayout& layout, bool swap) {
  if (!swap) return layout;
  ControllerLayout out = layout;
  for (size_t i = 1; i < size_t(PadControl::kCount); ++i) {
    if (iequals(out.bindings[i], "Handbrake")) out.bindings[i] = "Clutch";
    else if (iequals(out.bindings[i], "Clutch")) out.bindings[i] = "Handbrake";
  }
  return out;
}

KeyBindings KeyBindings::defaults() {
  KeyBindings kb;
  auto set = [&](ControlAction a, const char* p, const char* s = "") { kb.map[size_t(a)] = {p, s}; };
  set(ControlAction::kSteerLeft, "A", "Left");
  set(ControlAction::kSteerRight, "D", "Right");
  set(ControlAction::kThrottle, "W", "Up");
  set(ControlAction::kBrake, "S", "Down");
  set(ControlAction::kHandbrake, "Space");
  set(ControlAction::kClutch, "LeftShift");
  set(ControlAction::kShiftUp, "X");
  set(ControlAction::kShiftDown, "Z");
  set(ControlAction::kSwitchCamera, "V");
  set(ControlAction::kLookBack, "C");
  set(ControlAction::kRewind, "Y");
  set(ControlAction::kReset, "R");
  set(ControlAction::kMap, "M");
  set(ControlAction::kRadioPrev, "Comma");
  set(ControlAction::kRadioNext, "Period");
  set(ControlAction::kActivate, "E");
  set(ControlAction::kLookLeft, "J", "Keypad 4");
  set(ControlAction::kLookRight, "K", "Keypad 6");
  set(ControlAction::kPhotoMode, "P");
  set(ControlAction::kTelemetry, "F3");
  return kb;
}

void KeyBindings::set_primary(ControlAction a, std::string key) {
  const size_t i = size_t(a);
  if (i < map.size() && !key.empty()) map[i].primary = std::move(key);
}

bool KeyBindings::matches(ControlAction a, std::string_view key) const {
  const size_t i = size_t(a);
  if (i >= map.size() || key.empty()) return false;
  if (!map[i].primary.empty() && iequals(map[i].primary, key)) return true;
  if (!map[i].secondary.empty() && iequals(map[i].secondary, key)) return true;
  return false;
}

std::string KeyBindings::serialize() const {
  std::string out;
  for (size_t i = 0; i < map.size(); ++i) {
    if (map[i].primary.empty()) continue;
    if (!out.empty()) out += ',';
    out += control_action_name(ControlAction(i));
    out += '=';
    out += map[i].primary;
  }
  return out;
}

void KeyBindings::deserialize(std::string_view text) {
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t comma = text.find(',', pos);
    const std::string_view item = comma == std::string_view::npos ? text.substr(pos) : text.substr(pos, comma - pos);
    const size_t eq = item.find('=');
    if (eq != std::string_view::npos && eq + 1 < item.size()) {
      ControlAction act;
      if (parse_control_action(item.substr(0, eq), act)) {
        set_primary(act, std::string(item.substr(eq + 1)));
      }
    }
    if (comma == std::string_view::npos) break;
    pos = comma + 1;
  }
}

}  // namespace drive
