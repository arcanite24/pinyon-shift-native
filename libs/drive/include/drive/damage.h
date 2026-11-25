// Car damage + dynamic body/tyre dirt & mud accumulation.
// The original's damage difficulties are Cosmetic, Limited and Simulation
// (EDamageDifficulty; GameOptionValues Cosmetic, Limited). The stored values
// keep the engine's: None (0, ours), Cosmetic (1), Simulation (2), and
// Limited is 3 (SPEC_QUESTIONS Q102).
#pragma once

#include <array>
#include <cstdint>

#include "drive/vehicle.h"

namespace drive {

enum class DamageMode : int {
  kNone = 0,
  kCosmetic = 1,
  kSimulation = 2,
  kLimited = 3,
};

// One List_DamageModDef row (gamedb.slt): an impact of at least
// DamageMinImpact (mph, ours: Q102) damages the part by DamageMin..DamageMax
// across DamageMinImpact..DamageMaxImpact, with a chance of
// ProbabilityMin..ProbabilityMax across ProbabilityMinImpact..MaxImpact,
// the impact first varied by +-RandomImpact of itself; the part's effect is
// ModZeroDamage..ModFullDamage across its damage 0..1.
struct DamageModDef {
  double mod_zero = 1, mod_full = 1;
  double damage_min_impact = 0, damage_max_impact = 100, damage_min = 0, damage_max = 0;
  double random_impact = 0;
  double probability_min_impact = 0, probability_max_impact = 100, probability_min = 0, probability_max = 1;
};
enum DamagePart : int {
  kDamageGearBox = 0,
  kDamageDriveline,
  kDamageSteering,
  kDamageBodyDrag,
  kDamageFrontDownforce,
  kDamageRearDownforce,
  kDamageBrakes,
  kDamageTires,
  kDamageSuspension,
  kDamageEngine,
  kDamageMotor,
  kDamagePartCount
};
// The shipped rows (DamageModDefID 1..11 in DamagePart order).
const std::array<DamageModDef, kDamagePartCount>& default_damage_mod_defs();
// The part named by a DamageModName ("GearBox" .. "Motor"); -1: none.
int damage_part_by_name(const char* name);

const char* damage_mode_name(DamageMode m);

struct DamageMultipliers {
  double engine_torque = 1.0;     // 0.50..1.0 (limp-home 0.15 when fuel == 0)
  double shift_time_add = 0.0;    // +s (clutch grind)
  double shift_time_scale = 1.0;  // GearBox: 1..8
  double brake_power = 1.0;       // 0.55..1.0 (aggregate mean across axles)
  double aero_drag = 1.0;         // 1.0..1.40
  double aero_downforce = 1.0;    // mean of the two below
  double aero_downforce_front = 1.0, aero_downforce_rear = 1.0;  // Front/RearDownforce 1..0.1
  std::array<double, 4> tyre_grip{1.0, 1.0, 1.0, 1.0};        // 0.62..1.0 (tyre wear)
  std::array<double, 4> suspension_scale{1.0, 1.0, 1.0, 1.0}; // 0.55..1.0
  double steer_pull_rad = 0.0;    // asymmetric front suspension pull
  std::array<double, 2> brake_power_axle{1.0, 1.0};  // 0.55..1.0 (0 front, 1 rear)
  double clutch_capacity = 1.0;   // 0.45..1.0 (Manual+Clutch grind wear)
};

struct CarDamageState {
  static constexpr float kLampBreakThreshold = 0.45f;

  DamageMode mode = DamageMode::kCosmetic;
  // The DamageModDef rows in use (the loaded ones, else the shipped ones)
  // and each part's damage (0..1) under Limited and Simulation.
  std::array<DamageModDef, kDamagePartCount> mod_defs = default_damage_mod_defs();
  std::array<float, kDamagePartCount> part_damage{};
  uint32_t rng = 0x9E3779B9u;  // the impact rolls' state
  // Whether impacts damage the car's parts, and whether tyres wear and
  // fuel burns (Simulation only; Lobby IDS_OptionDamageDifficultyDesc).
  bool mechanical() const { return mode == DamageMode::kSimulation || mode == DamageMode::kLimited; }

  // Cosmetic zones (0.0 = pristine .. 1.0 = heavily dented/scratched).
  float zone_front = 0.0f;
  float zone_rear = 0.0f;
  float zone_left = 0.0f;
  float zone_right = 0.0f;
  float zone_roof = 0.0f;
  // 4-corner impact accumulation (0.0 = intact .. 1.0 = crushed) for independent
  // FL/FR headlight and RL/RR taillight breakage.
  float corner_fl = 0.0f;
  float corner_fr = 0.0f;
  float corner_rl = 0.0f;
  float corner_rr = 0.0f;
  float glass_crack = 0.0f;      // windshield/window spiderweb cracking (0..1)
  float scratch_amount = 0.0f;   // aggregate scratch/dent surface blend (0..1)

  // Deterministic part detachment & hood-ajar states unlocked at high zone impact thresholds.
  bool front_bumper_detached = false;
  bool rear_bumper_detached = false;
  bool left_mirror_detached = false;
  bool right_mirror_detached = false;
  bool wing_detached = false;
  float hood_ajar = 0.0f;        // 0.0 = latched .. 1.0 = popped open

  // Dynamic environmental dirt & mud (0..1) — active in ALL damage modes including None.
  float body_dirt = 0.0f;
  float tyre_dirt = 0.0f;

  bool headlight_fl_broken() const { return corner_fl >= kLampBreakThreshold; }
  bool headlight_fr_broken() const { return corner_fr >= kLampBreakThreshold; }
  bool taillight_rl_broken() const { return corner_rl >= kLampBreakThreshold; }
  bool taillight_rr_broken() const { return corner_rr >= kLampBreakThreshold; }

  // Simulation mechanical wear & damage (0 = healthy .. 1 = severe).
  float engine_damage = 0.0f;
  float trans_damage = 0.0f;
  float clutch_damage = 0.0f;
  float brake_damage = 0.0f;
  std::array<float, 2> brake_damage_axle{0.0f, 0.0f};  // 0 front, 1 rear
  float aero_damage = 0.0f;
  std::array<float, 4> suspension_damage{0.0f, 0.0f, 0.0f, 0.0f};  // LF, RF, LR, RR
  std::array<float, 4> tyre_wear{0.0f, 0.0f, 0.0f, 0.0f};          // 0 = fresh .. 1 = bald
  float fuel_fraction = 1.0f;                                      // 1 = full .. 0 = empty
  float steer_pull_sign = 1.0f;  // the Steering part's pull side

  void reset();

  // Records a collision impact in car-local coordinates (+X right, +Y up, +Z forward)
  // with closing speed `impact_speed_mps` (m/s).
  void record_impact(float local_x, float local_y, float local_z, float impact_speed_mps);

  // Records a Manual-with-Clutch gear grind (accumulates clutch & transmission wear in Simulation mode).
  void record_clutch_grind(float amount = 0.06f);

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
