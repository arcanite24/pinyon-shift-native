// Vehicle exterior lamp state machine, emissive technique classification,
// ShaderSettings.xml overrides, lamp-point evaluation, and cockpit gauge/wheel
// animation logic.
//
// Spec: CAR_LAMPS.md (horizon1-recomp docs/specs/CAR_LAMPS.md).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fh1data/carbin.h"

namespace drive {

// Per-frame runtime shader control vectors (CAR_LAMPS.md §2.6).
struct LampShaderVectors {
  float dynamic_lights_amount[4] = {0, 0, 0, 0};  // brake, reverse, ind_left, ind_right
  float dynamic_lights_2[4] = {0, 0, 1, 0};       // head, fullbeam, drl (default 1), fog
  float dynamic_lights_3[4] = {0, 0, 0, 0};       // left_ind_active, right_ind_active, side_lights, 0
};

// Inputs to CarLampState::update (one simulation tick).
struct LampInputs {
  float brake_cmd = 0.0f;       // processed brake command (>0)
  float brake_pedal = 0.0f;     // driver service brake pedal (>0; handbrake does NOT activate brake lamps)
  int gear = 1;                 // 0 = Reverse, >= 1 forward
  double tod_sec = 43200.0;     // seconds since midnight [0, 86400)
  float dt = 1.0f / 60.0f;      // frame delta seconds
  int indicator_mode = 0;       // 0 off, 1 left, 2 right, 3 hazard
  int light_override = 0;       // 0 auto, 1 forced low beam, 2 full beam, 3 fog lights
  float popup_open = 1.0f;      // <1.0 suppresses headlamps while pop-up pods open
  bool is_ai = false;           // enables 15-tick brake hold & 5% per-tick headlamp stagger
  bool snap_headlamps = false;  // bypasses stagger on spawn / headless render
};

// Per-vehicle lamp state machine (CAR_LAMPS.md §2).
struct CarLampState {
  // Constants confirmed in CAR_LAMPS.md §2.3 & §2.4.
  static constexpr double kMorningCutoffSec = 29400.0;  // 08:10:00
  static constexpr double kEveningStartSec = 67800.0;   // 18:50:00
  static constexpr float kIndicatorOnSec = 0.36f;
  static constexpr float kIndicatorOffSec = 0.33f;
  static constexpr int kAiBrakeHoldTicks = 15;

  bool brake_on = false;
  int ai_brake_hold = 0;
  bool reverse_on = false;
  bool headlamps_on = false;
  bool fullbeam_on = false;
  bool fog_on = false;
  bool drl_on = true;
  bool left_ind_active = false;
  bool right_ind_active = false;
  float ind_left = 0.0f;
  float ind_right = 0.0f;
  float ind_timer = 0.0f;
  uint32_t rng_state = 0x7A31C95Du;

  static bool tod_requests_headlamps(double tod_sec);
  void update(const LampInputs& in);
  LampShaderVectors vectors() const;
  // Evaluates whether a .carbin lamp point's channel_mask is active (0..1).
  float point_active(const fh1::CarbinLampPoint& pt) const;
};

// Emissive car shader technique classes (CAR_LAMPS.md §3.1 & §3.2).
// Passive covers (detail_glass_*, reflector, head_light, etc.) ALWAYS map to kNone.
enum class LampChannel : uint8_t {
  kNone = 0,
  kBrake = 1,             // tail_light*, lights_gls_tail_light, white_brake
  kTail = 2,              // taillightst, lights_gls_taillightst
  kTailBrake2S = 3,       // taillight2S, lights_gls_taillight2S
  kReverse = 4,           // reverse_light*, lights_gls_reverse_light
  kBrakeReverse = 5,      // brkreverse
  kIndicatorLeft = 6,     // indicator_left, lights_gls_indicator_left, linred
  kIndicatorRight = 7,    // indicator_right, lights_gls_indicator_right, rinred
  kBrakeIndLeft = 8,      // brinl
  kBrakeIndRight = 9,     // brinr
  kTailBrakeIndLeft = 10, // lindtailside
  kTailBrakeIndRight = 11,// rindtailside
  kHeadOld = 12,          // oldhead, lights_gls_oldhead
  kHeadXenon = 13,        // xenonhead
  kHeadHid = 14,          // hidhead, lights_gls_hidhead
  kFullBeam = 15,         // fullbeam
  kFogWhite = 16,         // fogwhite
  kFogRed = 17,           // fogred
  kDrlWhite = 18,         // drlwhite
  kDrlRed = 19,           // drlred
  kDrlOrange = 20,        // drlorange
  kDrlWhiteSwitch = 21,   // drlwhiteswitch (1 - head)
  kDrlBrakeRed = 22,      // drlbrakered
  kDrlBrakeOrange = 23,   // drlbrakeorange
  kSideLightOrange = 24,  // slorange
  kSideIndLeft = 25,      // slinlorange
  kSideIndRight = 26,     // slinrorange
  kNumPlate = 27,         // numplate
  kGaugeBacklight = 28,   // gauge_emissive*, gauge_needle*, cockpit_gauge_*
  kDigitalGauge = 29,     // digital_gauge, cockpit_digital_gauge
};

// Maps a carbin technique name (case-insensitive) to its emissive channel.
// Passive cover/reflector techniques strictly return LampChannel::kNone.
LampChannel classify_lamp_technique(std::string_view technique);

// Per-car ShaderSettings.xml overrides (technique -> param -> rgb).
struct LampColorTable {
  std::array<float, 3> brake{2.0f, 0.0f, 0.0f};
  std::array<float, 3> tail{0.6f, 0.0f, 0.0f};
  std::array<float, 3> reverse{1.1f, 1.1f, 1.1f};
  std::array<float, 3> indicator_amber{1.6f, 0.45f, 0.0f};
  std::array<float, 3> indicator_red{2.8f, 0.0f, 0.0f};
  std::array<float, 3> head_old{1.8f, 1.25f, 0.85f};
  std::array<float, 3> head_xenon{1.5f, 1.65f, 2.1f};
  std::array<float, 3> head_hid{1.7f, 1.55f, 1.55f};
  std::array<float, 3> fullbeam{2.4f, 2.2f, 2.0f};
  std::array<float, 3> fog_white{2.5f, 2.2f, 1.6f};
  std::array<float, 3> fog_red{2.2f, 0.25f, 0.0f};
  std::array<float, 3> drl_white{1.3f, 1.3f, 1.35f};
  std::array<float, 3> drl_red{1.3f, 0.0f, 0.0f};
  std::array<float, 3> drl_orange{2.2f, 0.45f, 0.0f};
  std::array<float, 3> side_orange{1.4f, 0.7f, 0.0f};
  std::array<float, 3> numplate{1.0f, 1.0f, 1.0f};
  float gauge_emissive = 2.5f;

  // Parses optional per-car ShaderSettings.xml overrides.
  void load_shader_settings_xml(std::string_view xml_text);
};

// Evaluates linear HDR emissive RGB for a technique channel under current lamp vectors.
std::array<float, 3> evaluate_lamp_emission(LampChannel ch, const LampShaderVectors& v,
                                            const LampColorTable& colors);

// Resolves the projected road headlight beam origin (midpoint of left/right 0x01
// points in body frame, +Z forward) and tint RGB by highest-priority lamp_kind
// (CAR_LAMPS.md §5).
struct HeadlightBeamSpec {
  bool valid = false;
  std::array<float, 3> origin_body{0.0f, 0.65f, 1.95f};  // body frame (+Z forward)
  std::array<float, 3> color{1.0f, 1.0f, 0.80f};
  float range_m = 55.0f;
};
HeadlightBeamSpec resolve_headlight_beam(std::span<const fh1::CarbinLampPoint> points, bool fullbeam);

// Resolves preset colour, radius (m), texture index (1..7), and cone angles (deg)
// for a lamp point whose fields are 0 / 255 (CAR_LAMPS.md §4 & §8).
struct ResolvedLampGlow {
  std::array<float, 3> color{1.0f, 0.98f, 0.92f};
  float radius_m = 0.35f;
  int texture_index = 1;  // 1..7
  float inner_cone_deg = 30.0f;
  float outer_cone_deg = 65.0f;
};
ResolvedLampGlow resolve_lamp_glow(const fh1::CarbinLampPoint& pt);

struct CarParams;

// Piecewise-linear analog gauge calibration (<TickMark> points, CAR_LAMPS.md §7.2).
struct GaugeConfig {
  float min_val = 0.0f;
  float max_val = 1.0f;
  // Pairs of (normalized_needle_pos, physical_value) sorted by physical_value ascending.
  std::vector<std::pair<float, float>> cal_points;

  float evaluate(float value) const;
};

struct CockpitGaugeCalibration {
  GaugeConfig speed;
  GaugeConfig tach;
  GaugeConfig fuel;
  GaugeConfig boost;
  GaugeConfig power;
  bool has_speed = false;
  bool has_tach = false;
  bool has_fuel = false;
  bool has_boost = false;
  bool has_power = false;
};

// Parses <TickMark pos="..." val="..."/> (or position/value attributes) from a gauge XML block.
bool parse_gauge_tick_marks_xml(std::string_view xml, GaugeConfig& out);

// Cockpit steering wheel and analog gauge needle math (CAR_LAMPS.md §7).
struct CockpitAnimState {
  static constexpr float kSteeringWheelAngleScale = 0.75f;  // GlobalCarAttribs.xml
  static constexpr float kCockpitLockRad = 4.712389f;       // 270 deg half-lock
  static constexpr float kSteerSpeedMinMps = 0.0f;          // CAR_LAMPS.md §7.1 & §8
  static constexpr float kSteerSpeedMaxMps = 50.0f;
  static constexpr float kSteerMultLowSpeed = 12.0f;
  static constexpr float kSteerMultHighSpeed = 6.0f;
  static constexpr float kPowerGaugeScale = 0.1341022f;     // CAR_LAMPS.md §7.2
  float wheel_rad = 0.0f;                                   // current cockpit steering wheel angle (rad)
  float speed_norm = 0.0f;                                  // [0..1]
  float tach_norm = 0.0f;                                   // [0..1]
  float fuel_norm = 0.85f;                                  // [0..1]
  float boost_norm = 0.0f;                                  // [0..1]
  float power_norm = 0.0f;                                  // [0..1]
  CockpitGaugeCalibration calibration;                      // optional per-car <TickMark> tables

  // Speed-dependent rate-limited steering wheel rotation (CAR_LAMPS.md §7.1)
  // plus CameraPhysics.xml SteeringWheel kickback (rad).
  void update_steering(float road_wheel_angle_rad, float speed_mps, float dt, float kick_rad = 0.0f);
  // Estimates per-car speedometer dial top (m/s) from top-gear redline gearing.
  static float estimate_dial_max_speed_mps(const CarParams& vp);
  // Converts absolute boost pressure (psi) to gauge pressure (psi relative to 1 atm).
  static float gauge_boost_psi(double abs_boost_psi, bool has_forced_induction);
  // Computes per-car boost gauge dial maximum (gauge psi).
  static float gauge_max_boost_psi(const CarParams& vp);
  // Piecewise-linear gauge evaluation (CAR_LAMPS.md §7.2).
  static float eval_gauge_norm(float value, float min_val, float max_val,
                               std::span<const std::pair<float, float>> cal_points = {});
  void update_gauges(float speed_mps, float max_speed_mps, float rpm, float max_rpm,
                     float boost_psi = 0.0f, float max_boost_psi = 20.0f, float fuel_frac = 0.85f,
                     float power_w = 0.0f, float max_power_w = 0.0f, float min_power_w = 0.0f);
};

}  // namespace drive
