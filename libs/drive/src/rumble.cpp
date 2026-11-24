#include "drive/rumble.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace drive {
namespace {

// Extracts name="value" attributes from an XML tag's body.
std::map<std::string, std::string> parse_attributes(std::string_view tag) {
  std::map<std::string, std::string> out;
  size_t i = 0;
  while (i < tag.size()) {
    const size_t eq = tag.find("=\"", i);
    if (eq == std::string_view::npos) break;
    size_t start = eq;
    while (start > i && tag[start - 1] != ' ' && tag[start - 1] != '\t' && tag[start - 1] != '\n' &&
           tag[start - 1] != '\r') {
      --start;
    }
    const size_t end = tag.find('"', eq + 2);
    if (end == std::string_view::npos) break;
    out[std::string(tag.substr(start, eq - start))] = std::string(tag.substr(eq + 2, end - eq - 2));
    i = end + 1;
  }
  return out;
}

// Parses a floating-point attribute value, stopping at a second decimal point
// (matching the retail v4 ControllerRumble.xml quirk e.g. "0.0.083335" -> 0.0).
double parse_number(const std::map<std::string, std::string>& attrs, const char* key, double fallback = 0.0) {
  auto it = attrs.find(key);
  if (it == attrs.end() || it->second.empty()) return fallback;
  return std::strtod(it->second.c_str(), nullptr);
}

bool find_section(std::string_view xml, std::string_view tag, std::string_view& body) {
  const std::string open_tok = "<" + std::string(tag) + ">";
  const std::string close_tok = "</" + std::string(tag) + ">";
  const size_t open = xml.find(open_tok);
  if (open == std::string_view::npos) return false;
  const size_t start = open + open_tok.size();
  const size_t close = xml.find(close_tok, start);
  if (close == std::string_view::npos) return false;
  body = xml.substr(start, close - start);
  return true;
}

bool find_tag_attrs(std::string_view body, std::string_view tag, std::map<std::string, std::string>& out) {
  const std::string prefix = "<" + std::string(tag) + " ";
  const size_t at = body.find(prefix);
  if (at == std::string_view::npos) return false;
  const size_t end = body.find('>', at + prefix.size());
  if (end == std::string_view::npos) return false;
  out = parse_attributes(body.substr(at + prefix.size(), end - at - prefix.size()));
  return true;
}

void parse_curve(const std::map<std::string, std::string>& a, RumbleCurve& c) {
  c.x[0] = parse_number(a, "x0");
  c.y[0] = parse_number(a, "y0");
  c.x[1] = parse_number(a, "x1");
  c.y[1] = parse_number(a, "y1");
  c.x[2] = parse_number(a, "x2");
  c.y[2] = parse_number(a, "y2");
}

void parse_motor_pair(std::string_view section, std::string_view channel, RumbleMotorPair& out) {
  std::string_view body;
  if (!find_section(section, channel, body)) return;
  std::map<std::string, std::string> a;
  if (find_tag_attrs(body, "LeftMotor", a)) parse_curve(a, out.left);
  if (find_tag_attrs(body, "RightMotor", a)) parse_curve(a, out.right);
}

void parse_event_pair(std::string_view section, std::string_view channel, RumbleEventPair& out) {
  std::string_view body;
  if (!find_section(section, channel, body)) return;
  std::map<std::string, std::string> a;
  if (find_tag_attrs(body, "LeftMotor", a)) out.left = parse_number(a, "rumble");
  if (find_tag_attrs(body, "RightMotor", a)) out.right = parse_number(a, "rumble");
}

}  // namespace

double RumbleCurve::evaluate(double input) const {
  if (y[0] == 0.0 && y[1] == 0.0 && y[2] == 0.0) return 0.0;
  if (input <= x[0]) return y[0];
  if (input >= x[2]) return y[2];
  if (input <= x[1]) {
    const double span = x[1] - x[0];
    if (span <= 1e-12) return y[1];
    return y[0] + (input - x[0]) / span * (y[1] - y[0]);
  }
  const double span = x[2] - x[1];
  if (span <= 1e-12) return y[2];
  return y[1] + (input - x[1]) / span * (y[2] - y[1]);
}

bool parse_controller_rumble(std::string_view xml, RumbleConfig& out, std::string* error, bool wheel) {
  out = {};
  std::string_view section;
  const char* root_tag = wheel ? "Wheel" : "Controller";
  if (!find_section(xml, root_tag, section)) {
    if (error) *error = std::string("ControllerRumble.xml has no <") + root_tag + "> section";
    return false;
  }
  parse_motor_pair(section, "EngineNormRPM", out.engine_norm_rpm);
  parse_motor_pair(section, "TransmissionRPM", out.transmission_rpm);
  parse_motor_pair(section, "CarToCarCollision", out.car_to_car_collision);
  parse_motor_pair(section, "CarToCarCollisionTime", out.car_to_car_collision_time);
  parse_motor_pair(section, "CarToWorldCollision", out.car_to_world_collision);
  parse_motor_pair(section, "CarToWorldCollisionTime", out.car_to_world_collision_time);
  parse_motor_pair(section, "CarToObjectCollision", out.car_to_object_collision);
  parse_motor_pair(section, "CarToObjectCollisionTime", out.car_to_object_collision_time);
  parse_motor_pair(section, "TireTraction", out.tire_traction);
  parse_motor_pair(section, "TireTractionFront", out.tire_traction_front);
  parse_motor_pair(section, "TireTractionRear", out.tire_traction_rear);
  parse_motor_pair(section, "AirFriction", out.air_friction);
  parse_motor_pair(section, "SurfaceBumpiness", out.surface_bumpiness);
  parse_event_pair(section, "OnShift", out.on_shift);
  parse_event_pair(section, "OnEngineSputter", out.on_engine_sputter);
  parse_event_pair(section, "OnBackfire", out.on_backfire);
  out.loaded = true;
  return true;
}

uint16_t RumbleOutput::left_u16() const {
  const double v = std::clamp(left, 0.0, 1.0);
  return static_cast<uint16_t>(std::lround(v * 65535.0));
}

uint16_t RumbleOutput::right_u16() const {
  const double v = std::clamp(right, 0.0, 1.0);
  return static_cast<uint16_t>(std::lround(v * 65535.0));
}

void GamepadRumble::reset() {
  car_car_ = {};
  car_world_ = {};
  car_object_ = {};
  shift_rem_ = 0;
  sputter_rem_ = 0;
  backfire_rem_ = 0;
  smooth_left_ = 0;
  smooth_right_ = 0;
}

void GamepadRumble::trigger_pulse(Pulse& p, double impulse, const RumbleMotorPair& amp, const RumbleMotorPair& time) {
  if (impulse <= 0.0) return;
  const double al = amp.left.evaluate(impulse);
  const double ar = amp.right.evaluate(impulse);
  const double tl = time.left.evaluate(impulse);
  const double tr = time.right.evaluate(impulse);
  if (al > 0.0 && tl > 0.0 && al >= p.amp_left * (p.dur_left > 0 ? p.rem_left / p.dur_left : 0.0)) {
    p.amp_left = al;
    p.dur_left = tl;
    p.rem_left = tl;
  }
  if (ar > 0.0 && tr > 0.0 && ar >= p.amp_right * (p.dur_right > 0 ? p.rem_right / p.dur_right : 0.0)) {
    p.amp_right = ar;
    p.dur_right = tr;
    p.rem_right = tr;
  }
}

void GamepadRumble::step_pulse(Pulse& p, double dt, double& out_left, double& out_right) {
  if (p.rem_left > 0.0 && p.dur_left > 0.0) {
    out_left += p.amp_left * (p.rem_left / p.dur_left);
    p.rem_left = std::max(0.0, p.rem_left - dt);
  }
  if (p.rem_right > 0.0 && p.dur_right > 0.0) {
    out_right += p.amp_right * (p.rem_right / p.dur_right);
    p.rem_right = std::max(0.0, p.rem_right - dt);
  }
}

RumbleOutput GamepadRumble::update(double dt, const RumbleInput& in) {
  dt = std::max(0.0, dt);
  if (!cfg_.loaded || in.paused || !in.enabled || in.scale <= 0.0) {
    reset();
    return {};
  }

  trigger_pulse(car_car_, in.car_to_car_impulse, cfg_.car_to_car_collision, cfg_.car_to_car_collision_time);
  trigger_pulse(car_world_, in.car_to_world_impulse, cfg_.car_to_world_collision, cfg_.car_to_world_collision_time);
  trigger_pulse(car_object_, in.car_to_object_impulse, cfg_.car_to_object_collision, cfg_.car_to_object_collision_time);

  if (in.on_shift) shift_rem_ = kEventHold;
  if (in.on_engine_sputter) sputter_rem_ = kEventHold;
  if (in.on_backfire) backfire_rem_ = kEventHold;

  // When speed_mps is provided (>= 0), smoothly gate road/slip/aero channels
  // to zero at standstill so a parked car never hums or vibrates constantly.
  const double motion_gate =
      (in.speed_mps < 0.0) ? 1.0 : std::clamp((std::fabs(in.speed_mps) - 0.35) / 1.0, 0.0, 1.0);

  double left = 0.0, right = 0.0;
  auto add = [&](const RumbleMotorPair& pair, double val, double gain = 1.0) {
    if (gain <= 0.0) return;
    left += pair.left.evaluate(val) * gain;
    right += pair.right.evaluate(val) * gain;
  };
  add(cfg_.engine_norm_rpm, in.engine_norm_rpm);
  add(cfg_.transmission_rpm, in.transmission_rpm, motion_gate);
  add(cfg_.tire_traction, in.tire_traction, motion_gate);
  add(cfg_.tire_traction_front, in.tire_traction_front, motion_gate);
  add(cfg_.tire_traction_rear, in.tire_traction_rear, motion_gate);
  add(cfg_.air_friction, in.air_friction, motion_gate);
  add(cfg_.surface_bumpiness, in.surface_bumpiness, motion_gate);

  step_pulse(car_car_, dt, left, right);
  step_pulse(car_world_, dt, left, right);
  step_pulse(car_object_, dt, left, right);

  if (shift_rem_ > 0.0) {
    const double env = in.smooth ? std::clamp(shift_rem_ / kEventHold, 0.0, 1.0) : 1.0;
    left += cfg_.on_shift.left * env;
    right += cfg_.on_shift.right * env;
    shift_rem_ = std::max(0.0, shift_rem_ - dt);
  }
  if (sputter_rem_ > 0.0) {
    const double env = in.smooth ? std::clamp(sputter_rem_ / kEventHold, 0.0, 1.0) : 1.0;
    left += cfg_.on_engine_sputter.left * env;
    right += cfg_.on_engine_sputter.right * env;
    sputter_rem_ = std::max(0.0, sputter_rem_ - dt);
  }
  if (backfire_rem_ > 0.0) {
    const double env = in.smooth ? std::clamp(backfire_rem_ / kEventHold, 0.0, 1.0) : 1.0;
    left += cfg_.on_backfire.left * env;
    right += cfg_.on_backfire.right * env;
    backfire_rem_ = std::max(0.0, backfire_rem_ - dt);
  }

  const double scale = std::clamp(in.scale, 0.0, 1.0);
  double target_l = std::clamp(left * scale, 0.0, 1.0);
  double target_r = std::clamp(right * scale, 0.0, 1.0);

  if (in.smooth && dt > 0.0) {
    // Asymmetric attack/release envelope: crisp attack (~30 ms) for impacts
    // and curbs, smooth exponential release (~100 ms) so rumble never chatters
    // or cuts abruptly, with a clean zero snap below 0.005.
    auto env_step = [&](double cur, double tgt) {
      const double tau = (tgt > cur) ? 0.030 : 0.100;
      const double alpha = 1.0 - std::exp(-dt / tau);
      double v = cur + (tgt - cur) * alpha;
      if (tgt == 0.0 && v < 0.005) v = 0.0;
      return std::clamp(v, 0.0, 1.0);
    };
    smooth_left_ = env_step(smooth_left_, target_l);
    smooth_right_ = env_step(smooth_right_, target_r);
    target_l = smooth_left_;
    target_r = smooth_right_;
  } else {
    smooth_left_ = target_l;
    smooth_right_ = target_r;
  }

  RumbleOutput out;
  out.left = target_l;
  out.right = target_r;
  return out;
}

}  // namespace drive
