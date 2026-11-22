// Cosmetic and Simulation car damage + dynamic body/tyre dirt & mud accumulation.
// Modes match FH1 SettingsPage: None (0), Cosmetic (1), Simulation (2).
#pragma once

#include <array>
#include <cstdint>

#include "drive/vehicle.h"

namespace drive {

enum class DamageMode : int {
  kNone = 0,
  kCosmetic = 1,
  kSimulation = 2,
};

const char* damage_mode_name(DamageMode m);

struct DamageMultipliers {
  double engine_torque = 1.0;     // 0.50..1.0 (limp-home 0.15 when fuel == 0)
  double shift_time_add = 0.0;    // +0.0..+0.65 s
  double brake_power = 1.0;       // 0.55..1.0
  double aero_drag = 1.0;         // 1.0..1.40
  double aero_downforce = 1.0;    // 0.45..1.0
  std::array<double, 4> tyre_grip{1.0, 1.0, 1.0, 1.0};        // 0.62..1.0 (tyre wear)
  std::array<double, 4> suspension_scale{1.0, 1.0, 1.0, 1.0}; // 0.55..1.0
  double steer_pull_rad = 0.0;    // asymmetric front suspension pull
};

struct CarDamageState {
  DamageMode mode = DamageMode::kCosmetic;

  // Cosmetic zones (0.0 = pristine .. 1.0 = heavily dented/scratched).
  float zone_front = 0.0f;
  float zone_rear = 0.0f;
  float zone_left = 0.0f;
  float zone_right = 0.0f;
  float zone_roof = 0.0f;
  float glass_crack = 0.0f;      // windshield/window spiderweb cracking (0..1)
  float scratch_amount = 0.0f;   // aggregate scratch/dent surface blend (0..1)

  // Dynamic environmental dirt & mud (0..1) — active in Cosmetic & Simulation.
  float body_dirt = 0.0f;
  float tyre_dirt = 0.0f;

  // Simulation mechanical wear & damage (0 = healthy .. 1 = severe).
  float engine_damage = 0.0f;
  float trans_damage = 0.0f;
  float brake_damage = 0.0f;
  float aero_damage = 0.0f;
  std::array<float, 4> suspension_damage{0.0f, 0.0f, 0.0f, 0.0f};  // LF, RF, LR, RR
  std::array<float, 4> tyre_wear{0.0f, 0.0f, 0.0f, 0.0f};          // 0 = fresh .. 1 = bald
  float fuel_fraction = 1.0f;                                      // 1 = full .. 0 = empty

  void reset();

  // Records a collision impact in car-local coordinates (+X right, +Y up, +Z forward)
  // with closing speed `impact_speed_mps` (m/s).
  void record_impact(float local_x, float local_y, float local_z, float impact_speed_mps);

  // Advances dirt/mud accumulation (on gravel/dirt/mud off-road surfaces) and, in
  // Simulation mode, tyre wear & fuel consumption.
  void step(double dt, double speed_mps, bool offroad, double throttle, double rpm_norm,
            const std::array<double, 4>& wheel_slip);

  // Returns runtime physics multipliers (all 1.0 unless mode == kSimulation).
  DamageMultipliers multipliers() const;

  // Applies Simulation-mode degradation onto a copy of `CarParams`.
  void apply_to_params(CarParams& p) const;
};

}  // namespace drive
