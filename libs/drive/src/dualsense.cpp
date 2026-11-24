#include "drive/dualsense.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace drive::dualsense {
namespace {

constexpr int kZones = 10;
constexpr float kPiF = 3.14159265358979323846f;

TriggerEffect block(uint8_t mode) {
  TriggerEffect e{};
  e[0] = mode;
  return e;
}

void put_zones(TriggerEffect& e, uint32_t zones) {
  e[1] = static_cast<uint8_t>(zones & 0xFFu);
  e[2] = static_cast<uint8_t>((zones >> 8) & 0xFFu);
}

void put_le32(TriggerEffect& e, size_t at, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) e[at + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFu);
}

float smooth_60hz(float current, float target, double dt_s) {
  const double keep = std::pow(0.5, std::clamp(dt_s, 0.0, 0.5) * 60.0);
  return static_cast<float>(double(target) + double(current - target) * keep);
}

int level_1_to_8(float value, float scale) {
  if (!(value > 0.0f)) return 0;
  return std::clamp(int(std::lround(1.0f + 7.0f * std::min(value, 1.0f) * scale)), 1, 8);
}

std::string lower_copy(std::string_view sv) {
  std::string s(sv);
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

bool is_dualsense(uint16_t vendor_id, uint16_t product_id) {
  return vendor_id == kSonyVendorId &&
         (product_id == kDualSenseProductId || product_id == kDualSenseEdgeProductId);
}

bool is_playstation_pad(uint16_t vendor_id, uint16_t product_id) {
  return vendor_id == kSonyVendorId &&
         (product_id == kDualSenseProductId || product_id == kDualSenseEdgeProductId ||
          product_id == kDualShock4V1ProductId || product_id == kDualShock4V2ProductId);
}

bool supports_gyro_tilt(uint16_t vendor_id, uint16_t product_id) {
  return is_playstation_pad(vendor_id, product_id) ||
         (vendor_id == kNintendoVendorId && product_id == kSwitchProProductId);
}

const char* connection_status_label(ConnectionStatus s) {
  switch (s) {
    case ConnectionStatus::kDisconnected: return "NO DUALSENSE CONNECTED";
    case ConnectionStatus::kConnectedUsb: return "DUALSENSE CONNECTED (USB)";
    case ConnectionStatus::kConnectedBluetooth: return "DUALSENSE CONNECTED (BLUETOOTH)";
    case ConnectionStatus::kSteamInputWrapped: return "STEAM INPUT HAS THE CONTROLLER";
  }
  return "NO DUALSENSE CONNECTED";
}

TriggerEffect off() { return block(0x05); }

TriggerEffect feedback(int position, int strength) {
  if (position < 0 || position > 9 || strength < 1 || strength > 8) return off();
  std::array<int, 10> strengths{};
  for (int i = position; i < kZones; ++i) strengths[size_t(i)] = strength;
  return multi_feedback(strengths);
}

TriggerEffect multi_feedback(const std::array<int, 10>& strengths) {
  uint32_t active = 0;
  uint32_t forces = 0;
  for (int i = 0; i < kZones; ++i) {
    const int s = strengths[size_t(i)];
    if (s < 0 || s > 8) return off();
    if (s == 0) continue;
    forces |= uint32_t((s - 1) & 7) << (3 * i);
    active |= 1u << i;
  }
  if (!active) return off();
  TriggerEffect e = block(0x21);
  put_zones(e, active);
  put_le32(e, 3, forces);
  return e;
}

TriggerEffect slope_feedback(int start, int end, int start_strength, int end_strength) {
  if (start < 0 || start > 8 || end <= start || end > 9 || start_strength < 1 ||
      start_strength > 8 || end_strength < 1 || end_strength > 8) {
    return off();
  }
  std::array<int, 10> strengths{};
  for (int i = start; i < kZones; ++i) {
    if (i <= end) {
      const float t = float(i - start) / float(end - start);
      strengths[size_t(i)] =
          int(std::lround(float(start_strength) + t * float(end_strength - start_strength)));
    } else {
      strengths[size_t(i)] = end_strength;
    }
  }
  return multi_feedback(strengths);
}

TriggerEffect weapon(int start, int end, int strength) {
  if (start < 2 || start > 7 || end <= start || end > 8 || strength < 1 || strength > 8) return off();
  TriggerEffect e = block(0x25);
  put_zones(e, (1u << start) | (1u << end));
  e[3] = static_cast<uint8_t>((strength - 1) & 7);
  return e;
}

TriggerEffect vibration(int position, int amplitude, int frequency_hz) {
  if (position < 0 || position > 9 || amplitude < 1 || amplitude > 8 || frequency_hz < 1 || frequency_hz > 255) {
    return off();
  }
  uint32_t active = 0;
  uint32_t amps = 0;
  for (int i = position; i < kZones; ++i) {
    amps |= uint32_t((amplitude - 1) & 7) << (3 * i);
    active |= 1u << i;
  }
  TriggerEffect e = block(0x26);
  put_zones(e, active);
  put_le32(e, 3, amps);
  e[9] = static_cast<uint8_t>(frequency_hz);
  return e;
}

TriggerEffect machine(int start, int end, int amplitude_a, int amplitude_b, int frequency_hz, int period_tenths) {
  if (start < 1 || start > 8 || end <= start || end > 9 || amplitude_a < 0 || amplitude_a > 7 ||
      amplitude_b < 0 || amplitude_b > 7 || frequency_hz < 1 || frequency_hz > 255 ||
      period_tenths < 0 || period_tenths > 255) {
    return off();
  }
  TriggerEffect e = block(0x27);
  put_zones(e, (1u << start) | (1u << end));
  e[3] = static_cast<uint8_t>((amplitude_a & 7) | ((amplitude_b & 7) << 3));
  e[4] = static_cast<uint8_t>(frequency_hz);
  e[5] = static_cast<uint8_t>(period_tenths);
  return e;
}

TriggerEffect bow(int start, int end, int strength, int snap) {
  if (start < 0 || start > 8 || end <= start || end > 8 || strength < 1 || strength > 8 || snap < 1 || snap > 8) {
    return off();
  }
  TriggerEffect e = block(0x22);
  put_zones(e, (1u << start) | (1u << end));
  const uint32_t pair = uint32_t((strength - 1) & 7) | (uint32_t((snap - 1) & 7) << 3);
  e[3] = static_cast<uint8_t>(pair & 0xFFu);
  e[4] = static_cast<uint8_t>(pair >> 8);
  return e;
}

int reported_family(const TriggerEffect& effect) {
  switch (effect[0]) {
    case 0x21:
    case 0x01: return 1;
    case 0x25:
    case 0x02: return 2;
    case 0x26:
    case 0x06: return 3;
    case 0x22: return 4;
    case 0x27: return 6;
    default: return 0;
  }
}

EffectPayload build_effect_payload(const TriggerEffects& effects) {
  EffectPayload payload{};
  payload[0] = kEnableRightTrigger | kEnableLeftTrigger;
  std::copy(effects.right.begin(), effects.right.end(), payload.begin() + kRightTriggerOffset);
  std::copy(effects.left.begin(), effects.left.end(), payload.begin() + kLeftTriggerOffset);
  return payload;
}

uint32_t dualsense_bt_crc32(const uint8_t* data, size_t len) {
  // IEEE 802.3 CRC-32 with 0xA2 BT output seed byte prepended.
  auto step = [](uint32_t crc, uint8_t byte) {
    crc ^= byte;
    for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    return crc;
  };
  uint32_t crc = 0xFFFFFFFFu;
  crc = step(crc, 0xA2);
  for (size_t i = 0; i < len; ++i) crc = step(crc, data[i]);
  return ~crc;
}

std::array<uint8_t, 48> build_usb_release_report() {
  std::array<uint8_t, 48> report{};
  report[0] = 0x02;  // USB output report ID
  const EffectPayload slack = build_effect_payload(TriggerEffects{});
  // Also stop motors (0x01 | 0x02) with zero motor values in crash/exit release.
  report[1] = static_cast<uint8_t>(slack[0] | 0x01 | 0x02);
  std::copy(slack.begin() + 1, slack.end(), report.begin() + 2);
  return report;
}

std::array<uint8_t, 78> build_bt_release_report() {
  std::array<uint8_t, 78> report{};
  report[0] = 0x31;  // Bluetooth enhanced output report ID
  report[1] = 0x02;  // tag / sequence
  const EffectPayload slack = build_effect_payload(TriggerEffects{});
  report[2] = static_cast<uint8_t>(slack[0] | 0x01 | 0x02);
  std::copy(slack.begin() + 1, slack.end(), report.begin() + 3);
  const uint32_t crc = dualsense_bt_crc32(report.data(), 74);
  report[74] = static_cast<uint8_t>(crc & 0xFFu);
  report[75] = static_cast<uint8_t>((crc >> 8) & 0xFFu);
  report[76] = static_cast<uint8_t>((crc >> 16) & 0xFFu);
  report[77] = static_cast<uint8_t>((crc >> 24) & 0xFFu);
  return report;
}

bool EffectChannel::update(const TriggerEffects& wanted, uint64_t now_ns, EffectPayload* out_payload) {
  if (valid_ && wanted == sent_) return false;
  if (ever_sent_ && now_ns >= last_send_ns_ && (now_ns - last_send_ns_) < min_interval_ns_) return false;
  if (out_payload) *out_payload = build_effect_payload(wanted);
  mark_sent(wanted, now_ns);
  return true;
}

EffectPayload EffectChannel::force_release(uint64_t now_ns) {
  const TriggerEffects slack{};
  const EffectPayload payload = build_effect_payload(slack);
  mark_sent(slack, now_ns);
  return payload;
}

void EffectChannel::mark_sent(const TriggerEffects& effects, uint64_t now_ns) {
  sent_ = effects;
  valid_ = true;
  ever_sent_ = true;
  last_send_ns_ = now_ns;
  ++reports_;
}

const char* trigger_mode_name(TriggerMode m) {
  switch (m) {
    case TriggerMode::kOff: return "off";
    case TriggerMode::kBrake: return "brake";
    case TriggerMode::kFull: return "full";
  }
  return "off";
}

TriggerMode parse_trigger_mode(std::string_view text) {
  const std::string l = lower_copy(text);
  if (l == "brake") return TriggerMode::kBrake;
  if (l == "full") return TriggerMode::kFull;
  return TriggerMode::kOff;
}

const char* lightbar_mode_name(LightbarMode m) {
  switch (m) {
    case LightbarMode::kOff: return "off";
    case LightbarMode::kPlayer: return "player";
    case LightbarMode::kRpm: return "rpm";
  }
  return "player";
}

LightbarMode parse_lightbar_mode(std::string_view text) {
  const std::string l = lower_copy(text);
  if (l == "off") return LightbarMode::kOff;
  if (l == "rpm") return LightbarMode::kRpm;
  return LightbarMode::kPlayer;
}

CarTelemetry telemetry_from_vehicle(const Vehicle& v) {
  CarTelemetry t;
  t.valid = true;
  t.rpm = static_cast<float>(v.rpm());
  t.redline_rpm = static_cast<float>(v.params().redline_rpm);
  t.gear = v.gear();
  t.speed_mps = static_cast<float>(v.speed());
  const auto& w = v.wheels();
  const auto max_mag = [](double a, double b) {
    return static_cast<float>(std::fabs(a) >= std::fabs(b) ? a : b);
  };
  const float sign_flip = v.gear() < 0 ? -1.0f : 1.0f;
  const auto wheel_slip = [&](size_t idx) -> double {
    if (!w[idx].contact) return 0.0;
    const double sr = w[idx].slip_ratio * double(sign_flip);
    const double sa = std::fabs(w[idx].slip_angle);
    if (sr < -0.15) {
      // Braking / lockup: preserve negative lock sign and include lateral scrub.
      return -std::hypot(sr, sa * 0.5);
    }
    // Driving / drifting: combined longitudinal + lateral friction-circle slip.
    return std::hypot(std::max(0.0, sr), sa);
  };
  t.front_slip = max_mag(wheel_slip(0), wheel_slip(1));
  t.rear_slip = max_mag(wheel_slip(2), wheel_slip(3));
  t.abs_active = v.abs_active();
  t.driven_front = v.params().layout != CarParams::Layout::kRear;
  t.driven_rear = v.params().layout != CarParams::Layout::kFront;
  return t;
}

void TriggerModel::reset() { *this = TriggerModel{}; }

constexpr float kPastPeakSlip = 1.0f;

bool TriggerModel::spin_buzz(float slip, double dt_s) {
  if (!spin_buzzing_ && slip > kPastPeakSlip) {
    spin_buzzing_ = true;
    spin_hold_s_ = 0.25;
  } else if (spin_buzzing_) {
    spin_hold_s_ = std::max(0.0, spin_hold_s_ - dt_s);
    if (spin_hold_s_ <= 0.0 && slip < 0.7f * kPastPeakSlip) spin_buzzing_ = false;
  }
  return spin_buzzing_;
}

TriggerEffects TriggerModel::step(const TriggerInputs& inputs, const Settings& settings, double dt_s) {
  TriggerEffects out;
  if (settings.trigger_mode == TriggerMode::kOff || !inputs.driving) {
    reset();
    return out;
  }
  const float scale = float(std::clamp(settings.trigger_strength_pct, 25, 100)) / 100.0f;
  const float raw_brake = std::clamp(inputs.brake, 0.0f, 1.0f);
  const float raw_throttle = std::clamp(inputs.throttle, 0.0f, 1.0f);
  const CarTelemetry* car = inputs.telemetry && inputs.telemetry->valid ? inputs.telemetry : nullptr;
  // In reverse (gear < 0), automatic pedal swap makes L2 drive backward and R2 brake.
  const bool rev_swap = (car && car->gear < 0);
  const float brake = rev_swap ? raw_throttle : raw_brake;
  const float throttle = rev_swap ? raw_brake : raw_throttle;

  TriggerEffect brake_fx = off();
  const TriggerEffect slope = slope_feedback(2, 8, level_1_to_8(0.15f, scale), level_1_to_8(1.0f, scale));
  if (car) {
    const float lock = std::max(0.0f, -std::min(car->front_slip, car->rear_slip));
    lock_ = smooth_60hz(lock_, lock, dt_s);
    abs_hold_s_ = car->abs_active ? 0.2 : std::max(0.0, abs_hold_s_ - dt_s);
    if (brake > 0.05f && abs_hold_s_ > 0.0) {
      brake_fx = machine(2, 8, 1, std::clamp(int(std::lround(3.0f * scale)), 1, 3), 24, 1);
    } else if (brake > 0.05f && lock_ > kPastPeakSlip) {
      brake_fx = vibration(2, level_1_to_8(1.0f, scale), 25);
    } else {
      brake_fx = slope;
    }
  } else {
    const float buzz = brake > 0.1f ? inputs.motor_low : 0.0f;
    brake_buzz_ = smooth_60hz(brake_buzz_, buzz, dt_s);
    brake_buzzing_ = brake_buzz_ > (brake_buzzing_ ? 0.15f : 0.25f);
    brake_fx = brake_buzzing_ ? vibration(2, level_1_to_8(brake_buzz_, scale), 25) : slope;
  }

  TriggerEffect throttle_fx = off();
  if (settings.trigger_mode == TriggerMode::kFull) {
    const TriggerEffect spring = feedback(1, level_1_to_8(0.1f, scale));
    if (car) {
      float spin = 0.0f;
      if (car->driven_front) spin = std::max(spin, car->front_slip);
      if (car->driven_rear) spin = std::max(spin, car->rear_slip);
      slip_ = smooth_60hz(slip_, spin, dt_s);
      const bool shifted = car->gear != last_gear_ && car->gear > 0 && last_gear_ > 0 && settings.gear_click;
      last_gear_ = car->gear;
      if (shifted) click_left_s_ = 0.1;
      if (click_left_s_ > 0.0) {
        click_left_s_ -= dt_s;
        throttle_fx = bow(4, 6, level_1_to_8(1.0f, scale), level_1_to_8(1.0f, scale));
      } else if (spin_buzz(throttle > 0.1f ? slip_ : 0.0f, dt_s)) {
        const float t = std::clamp((slip_ - kPastPeakSlip) / 3.0f, 0.0f, 1.0f);
        throttle_fx = vibration(3, level_1_to_8(0.05f + 0.15f * t, scale), 40);
      } else {
        throttle_fx = spring;
      }
    } else {
      const float buzz = throttle > 0.1f ? inputs.motor_high : 0.0f;
      throttle_buzz_ = smooth_60hz(throttle_buzz_, buzz, dt_s);
      throttle_buzzing_ = throttle_buzz_ > (throttle_buzzing_ ? 0.3f : 0.4f);
      throttle_fx = throttle_buzzing_
                        ? vibration(3, level_1_to_8(0.05f + 0.15f * (throttle_buzz_ - 0.3f) / 0.7f, scale), 40)
                        : spring;
    }
  }

  if (rev_swap) {
    out.right = brake_fx;
    out.left = throttle_fx;
  } else {
    out.left = brake_fx;
    out.right = throttle_fx;
  }
  return out;
}

Rgb rpm_colour(float rpm, float redline_rpm, bool blink_on) {
  if (!(redline_rpm > 0.0f)) return {0, 64, 0};
  const float t = std::clamp(rpm / redline_rpm, 0.0f, 1.0f);
  if (t >= 0.97f) return blink_on ? Rgb{255, 0, 0} : Rgb{0, 0, 0};
  const float warm = std::clamp((t - 0.5f) / 0.45f, 0.0f, 1.0f);
  const float red = std::min(1.0f, 2.0f * warm);
  const float green = std::min(1.0f, 2.0f * (1.0f - warm));
  const auto step = [](float v) { return static_cast<uint8_t>(std::lround(v * 8.0f) * 255 / 8); };
  return {step(red), step(green), 0};
}

Rgb lightbar_colour(LightbarMode mode, int player_index, float rpm, float redline_rpm, bool driving, bool blink_on) {
  if (mode == LightbarMode::kOff) return {0, 0, 0};
  if (mode == LightbarMode::kRpm && driving) return rpm_colour(rpm, redline_rpm, blink_on);
  static constexpr Rgb kPlayerColours[] = {
      {0, 64, 255},   // P1 blue
      {255, 32, 32},  // P2 red
      {32, 220, 64},  // P3 green
      {245, 4, 92},   // P4 pink
  };
  return kPlayerColours[size_t(std::clamp(player_index, 0, 3))];
}

// ---- Gyro Tilt Steering ----

void GyroTiltSteering::reset() {
  valid_ = false;
  tilt_rad_ = 0.0f;
  centre_rad_ = 0.0f;
  last_gyro_ns_ = 0;
}

void GyroTiltSteering::on_accel(float ax, float ay, float /*az*/) {
  if (std::fabs(ax) + std::fabs(ay) < 2.0f) return;
  const float accel_roll = std::atan2(ax, ay);
  if (!valid_) {
    tilt_rad_ = accel_roll;
    centre_rad_ = 0.0f;
    valid_ = true;
  } else {
    tilt_rad_ += 0.02f * (accel_roll - tilt_rad_);
  }
}

void GyroTiltSteering::on_gyro(float /*gx*/, float /*gy*/, float gz, uint64_t timestamp_ns) {
  if (!valid_) return;
  if (last_gyro_ns_ != 0 && timestamp_ns > last_gyro_ns_) {
    const float dt = std::min(0.05f, float(timestamp_ns - last_gyro_ns_) * 1e-9f);
    tilt_rad_ += gz * dt;
  }
  last_gyro_ns_ = timestamp_ns;
}

void GyroTiltSteering::recenter() {
  if (valid_) centre_rad_ = tilt_rad_;
}

float GyroTiltSteering::tilt_degrees() const {
  return valid_ ? (centre_rad_ - tilt_rad_) * (180.0f / kPiF) : 0.0f;
}

float GyroTiltSteering::steering(int sensitivity_pct) const {
  if (!valid_) return 0.0f;
  const float sens = float(std::clamp(sensitivity_pct, 25, 400)) / 100.0f;
  const float full_lock_rad = (45.0f * kPiF / 180.0f) / sens;  // 100% => full lock at 45 deg
  constexpr float kDeadBand = 2.0f * kPiF / 180.0f;
  float tilt = centre_rad_ - tilt_rad_;
  tilt = std::fabs(tilt) < kDeadBand ? 0.0f : tilt - std::copysign(kDeadBand, tilt);
  return std::clamp(tilt / std::max(full_lock_rad, 0.05f), -1.0f, 1.0f);
}

}  // namespace drive::dualsense
