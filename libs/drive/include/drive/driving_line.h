// 3D Driving / Braking Line: modes Off / Braking Only / Full, 10-point
// speed-vs-deceleration table from the car's actual tyres/mass/brakes/aero,
// curvature corner speed, backward 1/75 s braking integration, +-5 m/s
// Yellow/Red/Green colour band, and 3D road ribbon mesh builder.
//
// Spec: SPEC-CAR-PHYSICS (docs/specs/CAR_PHYSICS.md, "3D Driving / Braking Line").
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "drive/ai_driver.h"
#include "drive/vehicle.h"

namespace drive {

// Profile Main/SuggestedLine: 0 = Off, 1 = Braking Only, 2 = Full.
enum class DrivingLineMode : int {
  kOff = 0,
  kBrakingOnly = 1,
  kFull = 2,
};

struct DrivingLineColor {
  uint8_t r = 192;
  uint8_t g = 164;
  uint8_t b = 52;
  uint8_t a = 255;
};

// Exact RGBs from SPEC-CAR-PHYSICS "3D Driving / Braking Line".
inline constexpr DrivingLineColor kDrivingLineYellow{192, 164, 52, 255};
inline constexpr DrivingLineColor kDrivingLineRed{192, 39, 52, 255};
inline constexpr DrivingLineColor kDrivingLineGreen{44, 147, 44, 255};

// Evaluates the +-5.0 m/s colour band around v_target (t = clamp(|dv| * 0.2, 0, 1)).
DrivingLineColor driving_line_color(double v_target, double v_car, DrivingLineMode mode);

// 10-point speed-vs-deceleration table built at car init from CarParams using
// drive::tyre_peak_mu, mass, brakes, and aero drag.
struct BrakingTable {
  static constexpr int kPoints = 10;
  static constexpr double kSpeedStepMps = 10.0;  // 0, 10, 20, ..., 90 m/s
  std::array<double, kPoints> decel_mps2{};      // positive braking decel (m/s^2)
  double lateral_mu = 1.0;                       // effective lateral grip (mu_lat)

  static BrakingTable build(const CarParams& p);
  double decel_at(double speed_mps) const;
};

struct DrivingLineOptions {
  static constexpr double kGravity = 9.81;
  static constexpr double kBrakeStepS = 1.0 / 75.0;  // 0.0133333 s fixed step

  double behind_m = 0.0;
  double ahead_m = 175.0;
  double brake_lookahead_m = 320.0;
  double sample_step_m = 1.5;
  double half_width_m = 0.38;
  double lift_m = 0.08;
  double max_speed_mps = 110.0;
  double safety_factor = 0.9;         // cornering safety factor (spec: 0.8..1.01, 0.9)
  double exit_accel_mps2 = 6.0;       // corner-exit forward acceleration envelope for smooth colour unwind
  double color_smooth_sigma_m = 9.0;  // spatial Gaussian smoothing sigma (m) on display target speed
  double near_fade_start_m = 3.2;     // ahead of car center where ribbon fades in (clears front bumper)
  double near_fade_end_m = 8.0;       // ahead of car center where ribbon reaches full opacity
  double far_fade_m = 36.0;           // smooth fade-out distance at far horizon end
  bool fade_ends = true;
};

// Frame-rate-independent asymmetric temporal low-pass filter on effective car speed
// for driving-line colour evaluation: responsive when accelerating toward a braking
// zone, calm on brief throttle lifts or wheel-speed jitters so the line never flashes.
struct DrivingLineSmoother {
  double tau_up_s = 0.14;
  double tau_down_s = 0.24;
  double snap_delta_mps = 25.0;
  double speed_mps = 0.0;
  bool initialized = false;

  void reset() {
    speed_mps = 0.0;
    initialized = false;
  }

  double update(double raw_speed_mps, double dt_s);
};

struct DrivingLineSample {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  float right_x = 1.0f, right_z = 0.0f;
  float dist = 0.0f;
  float v_corner = 0.0f;
  float v_target = 0.0f;
  DrivingLineColor color{};
};

// Evaluates target speeds along the car's lookahead spline window via
// curvature corner speed + backward 1/75 s braking integration, and assigns
// per-sample colours for `mode`.
void build_driving_line(const RacingLine& line, const BrakingTable& table, double car_dist,
                        double car_speed, DrivingLineMode mode,
                        std::vector<DrivingLineSample>& out,
                        const DrivingLineOptions& opt = {});

struct DrivingLineVertex {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  uint8_t r = 255, g = 255, b = 255, a = 255;
};

// Converts ribbon samples into world-space triangles (3 vertices per triangle)
// lifted `lift_m` above the road surface.
void build_driving_line_mesh(const std::vector<DrivingLineSample>& samples, double half_width_m,
                             double lift_m, std::vector<DrivingLineVertex>& out_triangles);

}  // namespace drive
