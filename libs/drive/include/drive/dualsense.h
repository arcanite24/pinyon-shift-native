// Sony DualSense / DualSense Edge adaptive triggers (SDL3 PS5 HID effect
// payload), RPM/Player lightbar, player LEDs, Gyro Tilt Steering (DualSense,
// DualShock 4, Switch Pro) and crash/pause/focus-loss safety release.
// Pure C++ (no SDL dependency) so libs/drive and unit tests stay headless.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "drive/vehicle.h"

namespace drive::dualsense {

inline constexpr uint16_t kSonyVendorId = 0x054C;
inline constexpr uint16_t kDualSenseProductId = 0x0CE6;
inline constexpr uint16_t kDualSenseEdgeProductId = 0x0DF2;
inline constexpr uint16_t kDualShock4V1ProductId = 0x05C4;
inline constexpr uint16_t kDualShock4V2ProductId = 0x09CC;
inline constexpr uint16_t kNintendoVendorId = 0x057E;
inline constexpr uint16_t kSwitchProProductId = 0x2009;

// True for Sony DualSense and DualSense Edge (the pads that accept trigger effects).
bool is_dualsense(uint16_t vendor_id, uint16_t product_id);
// True for DualSense, DualSense Edge, or DualShock 4 (PlayStation button glyphs).
bool is_playstation_pad(uint16_t vendor_id, uint16_t product_id);
// True for pads supporting gyro tilt steering (DualSense, DS4, Switch Pro).
bool supports_gyro_tilt(uint16_t vendor_id, uint16_t product_id);

enum class ConnectionStatus : uint8_t {
  kDisconnected = 0,
  kConnectedUsb = 1,
  kConnectedBluetooth = 2,
  kSteamInputWrapped = 3,
};

const char* connection_status_label(ConnectionStatus s);

// 47-byte SDL_SendGamepadEffect payload matching SDL3's DS5EffectsState_t.
// Only trigger enable bits (0x04 right / 0x08 left) are set in byte 0 so SDL's
// rumble and LED reports never clobber trigger effects and vice versa.
inline constexpr size_t kTriggerEffectSize = 11;
inline constexpr size_t kEffectPayloadSize = 47;
inline constexpr size_t kRightTriggerOffset = 10;
inline constexpr size_t kLeftTriggerOffset = 21;
inline constexpr uint8_t kEnableRightTrigger = 0x04;
inline constexpr uint8_t kEnableLeftTrigger = 0x08;

using TriggerEffect = std::array<uint8_t, kTriggerEffectSize>;
using EffectPayload = std::array<uint8_t, kEffectPayloadSize>;

// Trigger block builders (positions 0..9, strengths/amplitudes 1..8).
// Out-of-range arguments safely return off().
TriggerEffect off();
TriggerEffect feedback(int position, int strength);
TriggerEffect multi_feedback(const std::array<int, 10>& strengths);
TriggerEffect slope_feedback(int start, int end, int start_strength, int end_strength);
TriggerEffect weapon(int start, int end, int strength);
TriggerEffect vibration(int position, int amplitude, int frequency_hz);
TriggerEffect machine(int start, int end, int amplitude_a, int amplitude_b, int frequency_hz, int period_tenths);
TriggerEffect bow(int start, int end, int strength, int snap);

// Trigger effect family reported in DualSense input report byte 48:
// 0=off, 1=feedback, 2=weapon, 3=vibration, 4=bow, 6=machine.
int reported_family(const TriggerEffect& effect);

struct TriggerEffects {
  TriggerEffect left = off();   // L2 (brake)
  TriggerEffect right = off();  // R2 (throttle)
  bool operator==(const TriggerEffects& o) const { return left == o.left && right == o.right; }
  bool operator!=(const TriggerEffects& o) const { return !(*this == o); }
};

// Builds the 47-byte SDL_SendGamepadEffect payload setting both triggers and nothing else.
EffectPayload build_effect_payload(const TriggerEffects& effects);

// Direct HID emergency release reports (used on quit/crash to bypass SDL's rumble queue):
// USB report ID 0x02 (48 bytes) and Bluetooth report ID 0x31 with CRC32 seeded 0xA2 (78 bytes).
std::array<uint8_t, 48> build_usb_release_report();
std::array<uint8_t, 78> build_bt_release_report();
uint32_t dualsense_bt_crc32(const uint8_t* data, size_t len);

// Rate-limited, change-only output channel (max 60 Hz default so BT stacks never stall).
class EffectChannel {
 public:
  static constexpr uint64_t kDefaultMinIntervalNs = 16666667ULL;

  EffectChannel() = default;
  explicit EffectChannel(uint64_t min_interval_ns) : min_interval_ns_(min_interval_ns) {}

  bool update(const TriggerEffects& wanted, uint64_t now_ns, EffectPayload* out_payload);
  // Forces an immediate release report (bypasses rate limit, for pause/focus-loss/exit).
  EffectPayload force_release(uint64_t now_ns);
  void invalidate() { valid_ = false; }
  void mark_sent(const TriggerEffects& effects, uint64_t now_ns);
  const TriggerEffects& sent() const { return sent_; }
  uint64_t reports_sent() const { return reports_; }

 private:
  uint64_t min_interval_ns_ = kDefaultMinIntervalNs;
  TriggerEffects sent_{};
  bool valid_ = false;
  bool ever_sent_ = false;
  uint64_t last_send_ns_ = 0;
  uint64_t reports_ = 0;
};

enum class TriggerMode : uint8_t { kOff = 0, kBrake = 1, kFull = 2 };
const char* trigger_mode_name(TriggerMode m);
TriggerMode parse_trigger_mode(std::string_view text);

enum class LightbarMode : uint8_t { kOff = 0, kPlayer = 1, kRpm = 2 };
const char* lightbar_mode_name(LightbarMode m);
LightbarMode parse_lightbar_mode(std::string_view text);

struct CarTelemetry {
  bool valid = false;
  float rpm = 0.0f;
  float redline_rpm = 0.0f;
  int gear = 0;  // -1 reverse, 0 neutral, 1..n
  float speed_mps = 0.0f;
  // Longitudinal slip normalised to peak grip (1 = peak, <-1 locking, >+1 wheelspin).
  float front_slip = 0.0f;
  float rear_slip = 0.0f;
  bool abs_active = false;
  bool driven_front = false;
  bool driven_rear = true;
};

CarTelemetry telemetry_from_vehicle(const Vehicle& v);

struct TriggerInputs {
  float brake = 0.0f;       // 0..1
  float throttle = 0.0f;    // 0..1
  float motor_low = 0.0f;   // 0..1 fallback rumble
  float motor_high = 0.0f;  // 0..1 fallback rumble
  bool driving = true;      // false in menus, pause, F6/F10, focus loss, or shutdown
  const CarTelemetry* telemetry = nullptr;
};

struct Settings {
  TriggerMode trigger_mode = TriggerMode::kOff;
  int trigger_strength_pct = 100;  // 25..100
  bool gear_click = false;
  LightbarMode lightbar_mode = LightbarMode::kPlayer;
  bool player_leds = true;
  bool gyro_steering = false;
  int gyro_sensitivity_pct = 100;  // 25..400 (100% = full lock at 45 deg)
};

class TriggerModel {
 public:
  TriggerEffects step(const TriggerInputs& inputs, const Settings& settings, double dt_s);
  void reset();

 private:
  bool spin_buzz(float slip, double dt_s);

  float brake_buzz_ = 0.0f;
  float throttle_buzz_ = 0.0f;
  bool brake_buzzing_ = false;
  bool throttle_buzzing_ = false;
  float slip_ = 0.0f;
  float lock_ = 0.0f;
  int last_gear_ = 0;
  double click_left_s_ = 0.0;
  double abs_hold_s_ = 0.0;
  bool spin_buzzing_ = false;
  double spin_hold_s_ = 0.0;
};

struct Rgb {
  uint8_t r = 0, g = 0, b = 0;
  bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
  bool operator!=(const Rgb& o) const { return !(*this == o); }
};

// Green below 50% redline -> yellow -> red at 95% -> flashing red >= 97%.
Rgb rpm_colour(float rpm, float redline_rpm, bool blink_on);
// Resolves lightbar colour for the given mode (returns player index colour for kPlayer).
Rgb lightbar_colour(LightbarMode mode, int player_index, float rpm, float redline_rpm, bool driving, bool blink_on);

// ---- Gyro Tilt Steering (DualSense, DualShock 4, Switch Pro) ----

class GyroTiltSteering {
 public:
  void reset();
  // Accelerometer sample in SDL gamepad axes (+X right, +Y up out of face, +Z toward player).
  void on_accel(float ax, float ay, float az);
  // Gyroscope sample (rad/s) with monotonic sensor timestamp in nanoseconds.
  void on_gyro(float gx, float gy, float gz, uint64_t timestamp_ns);
  // Touchpad / Capture button press sets current tilt as neutral centre.
  void recenter();
  bool valid() const { return valid_; }
  float tilt_degrees() const;
  // Returns steering contribution in [-1, +1] (100% sensitivity => full lock at 45 degrees).
  float steering(int sensitivity_pct) const;

 private:
  bool valid_ = false;
  float tilt_rad_ = 0.0f;
  float centre_rad_ = 0.0f;
  uint64_t last_gyro_ns_ = 0;
};

}  // namespace drive::dualsense
