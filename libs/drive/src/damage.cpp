#include "drive/damage.h"

#include <algorithm>
#include <cmath>

namespace drive {

const char* damage_mode_name(DamageMode m) {
  switch (m) {
    case DamageMode::kNone: return "NONE";
    case DamageMode::kCosmetic: return "COSMETIC";
    case DamageMode::kSimulation: return "SIMULATION";
    default: return "COSMETIC";
  }
}

void CarDamageState::reset() {
  const DamageMode keep_mode = mode;
  *this = CarDamageState{};
  mode = keep_mode;
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

  if (mode != DamageMode::kSimulation) return;

  // Simulation mechanical degradation mapped to impact location.
  if (front_hit) {
    engine_damage = std::min(1.0f, engine_damage + sev * 0.65f);
    aero_damage = std::min(1.0f, aero_damage + sev * 0.70f);
    brake_damage = std::min(1.0f, brake_damage + sev * 0.30f);
    if (local_x <= 0.15f) suspension_damage[0] = std::min(1.0f, suspension_damage[0] + sev * 0.55f);
    if (local_x >= -0.15f) suspension_damage[1] = std::min(1.0f, suspension_damage[1] + sev * 0.55f);
  }
  if (rear_hit) {
    aero_damage = std::min(1.0f, aero_damage + sev * 0.65f);
    trans_damage = std::min(1.0f, trans_damage + sev * 0.45f);
    if (local_x <= 0.15f) suspension_damage[2] = std::min(1.0f, suspension_damage[2] + sev * 0.50f);
    if (local_x >= -0.15f) suspension_damage[3] = std::min(1.0f, suspension_damage[3] + sev * 0.50f);
  }
  if (left_hit) {
    suspension_damage[0] = std::min(1.0f, suspension_damage[0] + sev * 0.50f);
    suspension_damage[2] = std::min(1.0f, suspension_damage[2] + sev * 0.40f);
    brake_damage = std::min(1.0f, brake_damage + sev * 0.25f);
  }
  if (right_hit) {
    suspension_damage[1] = std::min(1.0f, suspension_damage[1] + sev * 0.50f);
    suspension_damage[3] = std::min(1.0f, suspension_damage[3] + sev * 0.40f);
    brake_damage = std::min(1.0f, brake_damage + sev * 0.25f);
  }
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
  if (mode != DamageMode::kSimulation) return m;
  m.engine_torque = 1.0 - 0.45 * double(engine_damage);
  if (fuel_fraction <= 1e-4f) m.engine_torque = std::min(m.engine_torque, 0.15);
  m.shift_time_add = 0.60 * double(trans_damage);
  m.brake_power = 1.0 - 0.40 * double(brake_damage);
  m.aero_drag = 1.0 + 0.35 * double(aero_damage);
  m.aero_downforce = 1.0 - 0.50 * double(aero_damage);
  for (size_t i = 0; i < 4; ++i) {
    m.tyre_grip[i] = 1.0 - 0.35 * double(tyre_wear[i]);
    m.suspension_scale[i] = 1.0 - 0.42 * double(suspension_damage[i]);
  }
  // Asymmetric front suspension damage induces subtle steering pull.
  m.steer_pull_rad = 0.045 * double(suspension_damage[1] - suspension_damage[0]);
  return m;
}

void CarDamageState::apply_to_params(CarParams& p) const {
  if (mode != DamageMode::kSimulation) return;
  const DamageMultipliers m = multipliers();
  p.torque_scale *= m.engine_torque;
  p.gear_shift_time += m.shift_time_add;
  p.brake_friction_scale *= m.brake_power;
  p.aero_drag_long *= m.aero_drag;
  p.drag_quadratic *= m.aero_drag;
  p.aero_df_front *= m.aero_downforce;
  p.aero_df_rear *= m.aero_downforce;
  p.aero_part_df_front *= m.aero_downforce;
  p.aero_part_df_rear *= m.aero_downforce;
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
