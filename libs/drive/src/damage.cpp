#include "drive/damage.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace drive {

const char* damage_mode_name(DamageMode m) {
  switch (m) {
    case DamageMode::kNone: return "NONE";
    case DamageMode::kCosmetic: return "COSMETIC";
    case DamageMode::kSimulation: return "SIMULATION";
    case DamageMode::kLimited: return "LIMITED";
    default: return "COSMETIC";
  }
}

const std::array<DamageModDef, kDamagePartCount>& default_damage_mod_defs() {
  // gamedb.slt List_DamageModDef as shipped: ModZeroDamage, ModFullDamage,
  // DamageMinImpact, DamageMaxImpact, DamageMin, DamageMax, RandomImpact,
  // ProbabilityMinImpact, ProbabilityMaxImpact, ProbabilityMin, ProbabilityMax.
  static const std::array<DamageModDef, kDamagePartCount> rows{{
      {1.0, 8.0, 25, 100, 0.2, 1.0, 0.1, 0, 75, 0.3, 1.0},    // GearBox
      {1.0, 8.0, 25, 100, 0.1, 1.0, 0.2, 0, 75, 0.3, 1.0},    // Driveline
      {0.0, 4.0, 25, 100, 0.25, 1.0, 0.1, 0, 75, 0.3, 1.0},   // Steering
      {1.0, 2.0, 5, 75, 0.05, 1.0, 0.2, 0, 40, 0.5, 1.0},     // BodyDrag
      {1.0, 0.1, 5, 50, 0.1, 1.0, 0.2, 0, 30, 0.5, 1.0},      // FrontDownforce
      {1.0, 0.1, 5, 50, 0.1, 1.0, 0.2, 0, 30, 0.5, 1.0},      // RearDownforce
      {1.0, 0.1, 10, 100, 0.1, 1.0, 0.05, 0, 75, 0.3, 1.0},   // Brakes
      {1.0, 1.0, 20, 150, 0.0, 0.0, 0.3, 0, 150, 0.3, 1.0},   // Tires
      {1.0, 0.2, 15, 100, 0.2, 1.0, 0.05, 0, 75, 0.3, 1.0},   // Suspension
      {1.0, 0.05, 15, 100, 0.2, 1.0, 0.05, 0, 75, 0.2, 1.0},  // Engine
      {1.0, 0.05, 15, 100, 0.2, 1.0, 0.05, 0, 75, 0.2, 1.0},  // Motor
  }};
  return rows;
}

int damage_part_by_name(const char* name) {
  static const char* names[kDamagePartCount] = {"GearBox", "Driveline",  "Steering",   "BodyDrag",
                                                "FrontDownforce", "RearDownforce", "Brakes", "Tires",
                                                "Suspension", "Engine", "Motor"};
  if (!name) return -1;
  for (int i = 0; i < kDamagePartCount; ++i)
    if (std::string_view(name) == names[i]) return i;
  return -1;
}

void CarDamageState::reset() {
  const DamageMode keep_mode = mode;
  const auto keep_defs = mod_defs;
  *this = CarDamageState{};
  mode = keep_mode;
  mod_defs = keep_defs;
}

void CarDamageState::record_impact(float local_x, float local_y, float local_z, float impact_speed_mps) {
  if (mode == DamageMode::kNone || impact_speed_mps < 2.2f) return;

  // Severity scales smoothly from 2.2 m/s (~5 mph scuff) to 30 m/s (~67 mph hard crash).
  const float sev = std::clamp((impact_speed_mps - 2.2f) / 26.0f, 0.03f, 0.85f);

  const float ax = std::fabs(local_x);
  const float az = std::fabs(local_z);
  const bool roof_hit = local_y > 1.15f;
  const bool front_hit = local_z > 0.35f && az >= ax * 0.65f;
  const bool rear_hit = local_z < -0.35f && az >= ax * 0.65f;
  const bool left_hit = local_x < -0.25f && ax >= az * 0.55f;
  const bool right_hit = local_x > 0.25f && ax >= az * 0.55f;

  if (front_hit) zone_front = std::min(1.0f, zone_front + sev);
  if (rear_hit) zone_rear = std::min(1.0f, zone_rear + sev);
  if (left_hit) zone_left = std::min(1.0f, zone_left + sev);
  if (right_hit) zone_right = std::min(1.0f, zone_right + sev);
  if (roof_hit) zone_roof = std::min(1.0f, zone_roof + sev);

  // Fallback when impact point is near center: apply to nearest primary face.
  if (!front_hit && !rear_hit && !left_hit && !right_hit && !roof_hit) {
    if (local_z >= 0.0f) zone_front = std::min(1.0f, zone_front + sev);
    else zone_rear = std::min(1.0f, zone_rear + sev);
  }

  // 4-corner damage accumulation (car-local +X right, +Z forward):
  // - Off-center front/rear impacts concentrate full severity on the struck corner
  //   with zero bleed to the opposite corner past |x| >= 0.32 m, so a front-left
  //   hit breaks only the FL headlight.
  // - Center impacts distribute 0.55x severity to each corner, requiring a heavier
  //   center hit to break both lamps simultaneously.
  // - Side impacts near the front or rear quarter contribute proportionally to
  //   the adjacent corner while mid-door scrapes leave corner lamps intact.
  auto corner_lateral_weights = [](float lx, float& left_w, float& right_w) {
    const float cx = std::clamp(lx / 0.45f, -1.0f, 1.0f);
    if (cx < 0.0f) {
      const float mag = -cx;
      left_w = 0.55f + 0.45f * mag;
      right_w = 0.55f * std::max(0.0f, 1.0f - mag * 1.4f);
    } else if (cx > 0.0f) {
      left_w = 0.55f * std::max(0.0f, 1.0f - cx * 1.4f);
      right_w = 0.55f + 0.45f * cx;
    } else {
      left_w = 0.55f;
      right_w = 0.55f;
    }
  };
  if (front_hit || (!front_hit && !rear_hit && !left_hit && !right_hit && !roof_hit && local_z >= 0.0f)) {
    float lw = 0.0f, rw = 0.0f;
    corner_lateral_weights(local_x, lw, rw);
    corner_fl = std::min(1.0f, corner_fl + sev * lw);
    corner_fr = std::min(1.0f, corner_fr + sev * rw);
  }
  if (rear_hit || (!front_hit && !rear_hit && !left_hit && !right_hit && !roof_hit && local_z < 0.0f)) {
    float lw = 0.0f, rw = 0.0f;
    corner_lateral_weights(local_x, lw, rw);
    corner_rl = std::min(1.0f, corner_rl + sev * lw);
    corner_rr = std::min(1.0f, corner_rr + sev * rw);
  }
  if (left_hit && !front_hit && !rear_hit) {
    if (local_z > 0.45f) {
      const float z_w = std::clamp((local_z - 0.35f) / 1.0f, 0.0f, 0.75f);
      corner_fl = std::min(1.0f, corner_fl + sev * z_w);
    } else if (local_z < -0.45f) {
      const float z_w = std::clamp((-local_z - 0.35f) / 1.0f, 0.0f, 0.75f);
      corner_rl = std::min(1.0f, corner_rl + sev * z_w);
    }
  }
  if (right_hit && !front_hit && !rear_hit) {
    if (local_z > 0.45f) {
      const float z_w = std::clamp((local_z - 0.35f) / 1.0f, 0.0f, 0.75f);
      corner_fr = std::min(1.0f, corner_fr + sev * z_w);
    } else if (local_z < -0.45f) {
      const float z_w = std::clamp((-local_z - 0.35f) / 1.0f, 0.0f, 0.75f);
      corner_rr = std::min(1.0f, corner_rr + sev * z_w);
    }
  }

  // Deterministic part detachment & hood-ajar progression at high zone impact thresholds.
  if (zone_front >= 0.85f) front_bumper_detached = true;
  if (zone_rear >= 0.85f) rear_bumper_detached = true;
  if (zone_rear >= 0.78f || zone_roof >= 0.70f) wing_detached = true;
  if (zone_left >= 0.65f || corner_fl >= 0.85f) left_mirror_detached = true;
  if (zone_right >= 0.65f || corner_fr >= 0.85f) right_mirror_detached = true;
  if (zone_front > 0.55f) {
    const float ajar = std::clamp((zone_front - 0.55f) / 0.35f, 0.0f, 1.0f);
    hood_ajar = std::max(hood_ajar, ajar);
  }

  // Windshield/glass cracks on moderate-to-hard front or roof hits.
  if ((front_hit || roof_hit) && impact_speed_mps > 8.5f) {
    glass_crack = std::min(1.0f, glass_crack + sev * 0.85f);
  }
  scratch_amount = std::min(1.0f, std::max({zone_front, zone_rear, zone_left, zone_right, zone_roof}) * 0.92f +
                                      0.25f * sev);

  if (!mechanical()) return;

  // The parts (Limited, Simulation): each List_DamageModDef row on the
  // impact in mph (Q102: the unit, the roll and Limited's half are ours).
  auto next = [this]() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return double(rng) / 4294967296.0;
  };
  auto ramp = [](double x, double x0, double x1) {
    return x1 > x0 ? std::clamp((x - x0) / (x1 - x0), 0.0, 1.0) : (x >= x1 ? 1.0 : 0.0);
  };
  const double mph = double(impact_speed_mps) / 0.44704;
  const double share = mode == DamageMode::kLimited ? 0.5 : 1.0;
  for (int i = 0; i < kDamagePartCount; ++i) {
    const DamageModDef& d = mod_defs[size_t(i)];
    const double impact = mph * (1.0 + d.random_impact * (2.0 * next() - 1.0));
    if (impact < d.damage_min_impact || !(d.damage_max > 0)) continue;
    const double chance = d.probability_min + (d.probability_max - d.probability_min) *
                                                  ramp(impact, d.probability_min_impact, d.probability_max_impact);
    if (next() >= chance) continue;
    const double add =
        d.damage_min + (d.damage_max - d.damage_min) * ramp(impact, d.damage_min_impact, d.damage_max_impact);
    part_damage[size_t(i)] = float(std::clamp(double(part_damage[size_t(i)]) + add * share, 0.0, 1.0));
  }
  // The engine's damage fields from the parts (read by the sputter rumble,
  // the audio's damage sounds, the HUD and replays).
  engine_damage = std::max(engine_damage, std::max(part_damage[kDamageEngine], part_damage[kDamageMotor]));
  trans_damage = std::max(trans_damage, part_damage[kDamageGearBox]);
  brake_damage = std::max(brake_damage, part_damage[kDamageBrakes]);
  brake_damage_axle = {brake_damage, brake_damage};
  aero_damage = std::max({aero_damage, part_damage[kDamageBodyDrag], part_damage[kDamageFrontDownforce],
                          part_damage[kDamageRearDownforce]});
  for (auto& sd : suspension_damage) sd = std::max(sd, part_damage[kDamageSuspension]);
  steer_pull_sign = local_x < 0 ? -1.0f : 1.0f;
}

void CarDamageState::record_clutch_grind(float amount) {
  if (!mechanical() || amount <= 0.0f) return;
  clutch_damage = std::min(1.0f, clutch_damage + amount);
  trans_damage = std::min(1.0f, trans_damage + amount * 0.75f);
}

void CarDamageState::step(double dt, double speed_mps, bool offroad, double throttle, double rpm_norm,
                          const std::array<double, 4>& wheel_slip) {
  if (dt <= 0.0) return;
  const double v = std::fabs(speed_mps);

  // Dynamic body & tyre dirt/mud accumulation off-road; gradual dust shedding on tarmac.
  // Active even when DamageMode == kNone (dirt is environmental, not collision damage).
  if (offroad && v > 1.5) {
    const double rate = std::min(1.0, v / 25.0);
    tyre_dirt = float(std::min(1.0, double(tyre_dirt) + dt * 0.14 * rate));
    body_dirt = float(std::min(1.0, double(body_dirt) + dt * 0.055 * rate));
  } else if (!offroad && v > 8.0) {
    // Tyres scrub mud off faster on asphalt than the lower bodywork does.
    tyre_dirt = float(std::max(0.0, double(tyre_dirt) - dt * 0.035 * std::min(1.0, v / 30.0)));
    body_dirt = float(std::max(0.0, double(body_dirt) - dt * 0.004 * std::min(1.0, v / 40.0)));
  }

  if (mode != DamageMode::kSimulation) return;

  // Tyre wear proportional to speed and slip ratio (~30 min hard driving to full wear).
  for (size_t i = 0; i < 4; ++i) {
    const double slip = std::clamp(std::fabs(wheel_slip[i]), 0.0, 2.0);
    const double wear_rate = (0.00018 + 0.0022 * slip * slip) * std::min(1.5, v / 30.0);
    tyre_wear[i] = float(std::min(1.0, double(tyre_wear[i]) + dt * wear_rate));
  }

  // Fuel consumption proportional to throttle & RPM (~25 min WOT tank).
  const double burn = (0.00004 + 0.00062 * std::clamp(throttle, 0.0, 1.0) * std::clamp(rpm_norm, 0.15, 1.0)) * dt;
  fuel_fraction = float(std::max(0.0, double(fuel_fraction) - burn));
}

DamageMultipliers CarDamageState::multipliers() const {
  DamageMultipliers m;
  if (!mechanical()) return m;
  // Each part's effect: ModZeroDamage..ModFullDamage across its damage.
  auto mod = [&](int part) {
    const DamageModDef& d = mod_defs[size_t(part)];
    return d.mod_zero + (d.mod_full - d.mod_zero) * std::clamp(double(part_damage[size_t(part)]), 0.0, 1.0);
  };
  m.engine_torque = std::min(mod(kDamageEngine), mod(kDamageMotor));
  if (fuel_fraction <= 1e-4f) m.engine_torque = std::min(m.engine_torque, 0.15);
  m.shift_time_scale = mod(kDamageGearBox);
  m.shift_time_add = 0.60 * double(clutch_damage);
  m.brake_power_axle[0] = m.brake_power_axle[1] = mod(kDamageBrakes);
  m.brake_power = mod(kDamageBrakes);
  m.clutch_capacity = 1.0 - 0.55 * std::clamp(double(clutch_damage), 0.0, 1.0);
  m.aero_drag = mod(kDamageBodyDrag);
  m.aero_downforce_front = mod(kDamageFrontDownforce);
  m.aero_downforce_rear = mod(kDamageRearDownforce);
  m.aero_downforce = 0.5 * (m.aero_downforce_front + m.aero_downforce_rear);
  for (size_t i = 0; i < 4; ++i) {
    m.tyre_grip[i] = mode == DamageMode::kSimulation ? 1.0 - 0.35 * double(tyre_wear[i]) : 1.0;
    m.suspension_scale[i] = mod(kDamageSuspension);
  }
  // Steering: ModFullDamage degrees of pull (ours: the side of the last
  // mechanical impact).
  m.steer_pull_rad = double(steer_pull_sign) * mod(kDamageSteering) * 3.14159265358979323846 / 180.0;
  return m;
}

void CarDamageState::apply_to_params(CarParams& p) const {
  if (!mechanical()) return;
  const DamageMultipliers m = multipliers();
  p.torque_scale *= m.engine_torque;
  p.gear_shift_time = p.gear_shift_time * m.shift_time_scale + m.shift_time_add;
  p.clutch_torque_factor *= m.clutch_capacity;
  p.brake_friction_scale *= m.brake_power;
  p.front_brake_torque *= m.brake_power_axle[0];
  p.rear_brake_torque *= m.brake_power_axle[1];
  p.aero_drag_long *= m.aero_drag;
  p.drag_quadratic *= m.aero_drag;
  p.aero_df_front *= m.aero_downforce_front;
  p.aero_df_rear *= m.aero_downforce_rear;
  p.aero_part_df_front *= m.aero_downforce_front;
  p.aero_part_df_rear *= m.aero_downforce_rear;
  const double front_grip = 0.5 * (m.tyre_grip[0] + m.tyre_grip[1]);
  const double rear_grip = 0.5 * (m.tyre_grip[2] + m.tyre_grip[3]);
  p.width_scale[0] *= front_grip;
  p.width_scale[1] *= rear_grip;
  const double front_susp = 0.5 * (m.suspension_scale[0] + m.suspension_scale[1]);
  const double rear_susp = 0.5 * (m.suspension_scale[2] + m.suspension_scale[3]);
  p.spring_rate[0] *= front_susp;
  p.spring_rate[1] *= rear_susp;
  p.bump_rate[0] *= front_susp;
  p.bump_rate[1] *= rear_susp;
  p.rebound_rate[0] *= front_susp;
  p.rebound_rate[1] *= rear_susp;
}

}  // namespace drive
