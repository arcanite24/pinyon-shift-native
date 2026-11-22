#include "garage/tuning.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace garage {
namespace {

double lerp(double a, double b, double u) { return a + (b - a) * std::clamp(u, 0.0, 1.0); }
double inv_lerp(double a, double b, double v) {
  return std::fabs(b - a) > 1e-9 ? std::clamp((v - a) / (b - a), 0.0, 1.0) : 0.5;
}

}  // namespace

const char* tune_category_name(TuneCategory cat) {
  switch (cat) {
    case TuneCategory::kTires: return "Tires";
    case TuneCategory::kGearing: return "Gearing";
    case TuneCategory::kAlignment: return "Alignment";
    case TuneCategory::kAntiRollBars: return "Anti-Roll Bars";
    case TuneCategory::kSprings: return "Springs";
    case TuneCategory::kDamping: return "Damping";
    case TuneCategory::kAero: return "Aero";
    case TuneCategory::kBrake: return "Brake";
    case TuneCategory::kDifferential: return "Differential";
    default: return "Tuning";
  }
}

std::vector<TuneSlider> build_tune_sliders(const cars::CarPhysics& car, const drive::CarTune& tune) {
  std::vector<TuneSlider> out;
  const double mass = std::max(500.0, car.get("List_UpgradeCarBodyWeight.Mass", 1400.0));
  const int trans_lvl = int(std::llround(car.get("List_UpgradeDrivetrainTransmission.Level", 0)));
  const int susp_lvl = int(std::llround(car.get("List_UpgradeSpringDamper.Level", 0)));
  const int arb_f_lvl = int(std::llround(car.get("List_UpgradeAntiSwayFront.Level", 0)));
  const int arb_r_lvl = int(std::llround(car.get("List_UpgradeAntiSwayRear.Level", 0)));
  const int brake_lvl = int(std::llround(car.get("List_UpgradeBrakes.Level", 0)));
  const int diff_lvl = int(std::llround(car.get("List_UpgradeDrivetrainDifferential.Level", 0)));
  const int drivetype = int(std::llround(car.get("Data_Drivetrain.DrivetypeID", 2)));  // 1 FWD, 2 RWD, 3 AWD

  const bool gears_unlocked = trans_lvl >= 2;
  const bool align_unlocked = susp_lvl >= 2;
  const bool springs_unlocked = susp_lvl >= 2;
  const bool damping_unlocked = susp_lvl >= 2;
  const bool arb_f_unlocked = arb_f_lvl >= 2;
  const bool arb_r_unlocked = arb_r_lvl >= 2;
  const bool brake_unlocked = brake_lvl >= 2;
  const bool diff_unlocked = diff_lvl >= 2;
  const bool front_aero_unlocked =
      car.has("FrontBumperAero.Downforce1") &&
      car.get("FrontBumperAero.Downforce1", 0) > car.get("FrontBumperAero.Downforce0", 0) + 1e-3;
  const bool rear_aero_unlocked =
      car.has("RearWingAero.Downforce1") &&
      car.get("RearWingAero.Downforce1", 0) > car.get("RearWingAero.Downforce0", 0) + 1e-3;

  auto add = [&](const char* key, const char* label, const char* unit, TuneCategory cat, double lo, double hi,
                 double def, double step, bool unlocked, double cur, bool tuned) {
    TuneSlider s;
    s.key = key;
    s.label = label;
    s.unit = unit;
    s.category = cat;
    s.min_val = lo;
    s.max_val = hi;
    s.def_val = std::clamp(def, lo, hi);
    s.step = step;
    s.unlocked = unlocked;
    s.tuned = tuned;
    s.value = tuned ? std::clamp(cur, lo, hi) : s.def_val;
    out.push_back(std::move(s));
  };

  // 1. Tires (always unlocked).
  add("Tuning_frontTirePressure", "Front Tire Pressure", "PSI", TuneCategory::kTires, 15.0, 55.0, 30.0, 0.5, true,
      tune.front_tire_pressure_psi, tune.front_tire_pressure_psi >= 0);
  add("Tuning_rearTirePressure", "Rear Tire Pressure", "PSI", TuneCategory::kTires, 15.0, 55.0, 30.0, 0.5, true,
      tune.rear_tire_pressure_psi, tune.rear_tire_pressure_psi >= 0);

  // 2. Gearing.
  const double def_fd = car.get("List_UpgradeDrivetrainTransmission.FinalDriveRatio", 3.7);
  add("Tuning_finalDriveRatio", "Final Drive", "", TuneCategory::kGearing, 2.20, 6.10, def_fd, 0.05, gears_unlocked,
      tune.final_drive, tune.final_drive > 0);
  static const char* kGearKeys[8] = {"Tuning_firstGear",  "Tuning_secondGear", "Tuning_thirdGear",  "Tuning_fourthGear",
                                     "Tuning_fifthGear",  "Tuning_sixthGear",  "Tuning_seventhGear", "Tuning_eighthGear"};
  static const char* kGearLabels[8] = {"1st Gear", "2nd Gear", "3rd Gear", "4th Gear",
                                       "5th Gear", "6th Gear", "7th Gear", "8th Gear"};
  const int num_gears = std::clamp(int(std::llround(car.get("List_UpgradeDrivetrainTransmission.NumGears", 7))) - 1, 1, 8);
  for (int g = 0; g < num_gears; ++g) {
    const double gr = car.get("List_UpgradeDrivetrainTransmission.GearRatio" + std::to_string(g + 1), 1.5);
    add(kGearKeys[g], kGearLabels[g], "", TuneCategory::kGearing, 0.48, 6.00, gr, 0.05, gears_unlocked,
        tune.gears[size_t(g)], tune.gears[size_t(g)] > 0);
  }

  // 3. Alignment.
  const double def_camber_f = car.get("FrontSpringDamper.StaticCamber", -0.5);
  const double def_camber_r = car.get("RearSpringDamper.StaticCamber", -0.5);
  const double def_toe_f = car.get("FrontSpringDamper.StaticToe", 0.0);
  const double def_toe_r = car.get("RearSpringDamper.StaticToe", 0.0);
  const double def_caster = car.get("FrontSpringDamper.Caster", 5.0);
  add("Tuning_frontCamber", "Front Camber", "deg", TuneCategory::kAlignment, -5.0, 5.0, def_camber_f, 0.1,
      align_unlocked, tune.front_camber_deg, tune.front_camber_deg > -90);
  add("Tuning_rearCamber", "Rear Camber", "deg", TuneCategory::kAlignment, -5.0, 5.0, def_camber_r, 0.1,
      align_unlocked, tune.rear_camber_deg, tune.rear_camber_deg > -90);
  add("Tuning_frontToe", "Front Toe", "deg", TuneCategory::kAlignment, -5.0, 5.0, def_toe_f, 0.1, align_unlocked,
      tune.front_toe_deg, tune.front_toe_deg > -90);
  add("Tuning_rearToe", "Rear Toe", "deg", TuneCategory::kAlignment, -5.0, 5.0, def_toe_r, 0.1, align_unlocked,
      tune.rear_toe_deg, tune.rear_toe_deg > -90);
  add("Tuning_frontCaster", "Front Caster", "deg", TuneCategory::kAlignment, 1.0, 7.0, def_caster, 0.1, align_unlocked,
      tune.front_caster_deg, tune.front_caster_deg > -90);

  // 4. Anti-Roll Bars (displayed in 1..40 Forza units, stored normalized [0..1]).
  auto norm_slider = [&](const char* key, const char* label, const char* unit, TuneCategory cat, double lo_p,
                         double hi_p, double def_p, double step, bool unlocked, double norm_v) {
    const bool tuned = norm_v >= 0.0;
    const double cur = tuned ? lerp(lo_p, hi_p, norm_v) : def_p;
    add(key, label, unit, cat, lo_p, hi_p, def_p, step, unlocked, cur, tuned);
  };
  {
    const double def_f = car.get("FrontAntiSway.DefSwaybarStiffness", 10.0);
    const double lo_f = car.get("FrontAntiSway.MinSwaybarStiffness", def_f * 0.4);
    const double hi_f = car.get("FrontAntiSway.MaxSwaybarStiffness", def_f * 2.2);
    norm_slider("Tuning_frontSwaybar", "Front Anti-Roll Bar", "N/mm", TuneCategory::kAntiRollBars, lo_f, hi_f, def_f,
                std::max(0.2, (hi_f - lo_f) / 40.0), arb_f_unlocked, tune.front_arb);
    const double def_r = car.get("RearAntiSway.DefSwaybarStiffness", 8.0);
    const double lo_r = car.get("RearAntiSway.MinSwaybarStiffness", def_r * 0.4);
    const double hi_r = car.get("RearAntiSway.MaxSwaybarStiffness", def_r * 2.2);
    norm_slider("Tuning_rearSwaybar", "Rear Anti-Roll Bar", "N/mm", TuneCategory::kAntiRollBars, lo_r, hi_r, def_r,
                std::max(0.2, (hi_r - lo_r) / 40.0), arb_r_unlocked, tune.rear_arb);
  }

  // 5. Springs & Ride Height.
  {
    const double k_f = car.get("FrontSpringDamper.DefSpringRate", 45.0) * mass / 1000.0;  // N/mm
    const double lo_f = car.get("FrontSpringDamper.MinSpringRate", 25.0) * mass / 1000.0;
    const double hi_f = car.get("FrontSpringDamper.MaxSpringRate", 90.0) * mass / 1000.0;
    norm_slider("Tuning_frontSpring", "Front Springs", "N/mm", TuneCategory::kSprings, lo_f, hi_f, k_f,
                std::max(1.0, (hi_f - lo_f) / 40.0), springs_unlocked, tune.front_spring);
    const double k_r = car.get("RearSpringDamper.DefSpringRate", 40.0) * mass / 1000.0;
    const double lo_r = car.get("RearSpringDamper.MinSpringRate", 20.0) * mass / 1000.0;
    const double hi_r = car.get("RearSpringDamper.MaxSpringRate", 85.0) * mass / 1000.0;
    norm_slider("Tuning_rearSpring", "Rear Springs", "N/mm", TuneCategory::kSprings, lo_r, hi_r, k_r,
                std::max(1.0, (hi_r - lo_r) / 40.0), springs_unlocked, tune.rear_spring);

    const double rh_f = car.get("FrontSpringDamper.DefRideHeight", 0.16) * 100.0;  // cm
    const double rh_flo = car.get("FrontSpringDamper.MinRideHeight", 0.11) * 100.0;
    const double rh_fhi = car.get("FrontSpringDamper.MaxRideHeight", 0.21) * 100.0;
    norm_slider("Tuning_frontRideHeight", "Front Ride Height", "cm", TuneCategory::kSprings, rh_flo, rh_fhi, rh_f, 0.2,
                springs_unlocked, tune.front_ride_height);
    const double rh_r = car.get("RearSpringDamper.DefRideHeight", 0.17) * 100.0;
    const double rh_rlo = car.get("RearSpringDamper.MinRideHeight", 0.12) * 100.0;
    const double rh_rhi = car.get("RearSpringDamper.MaxRideHeight", 0.22) * 100.0;
    norm_slider("Tuning_rearRideHeight", "Rear Ride Height", "cm", TuneCategory::kSprings, rh_rlo, rh_rhi, rh_r, 0.2,
                springs_unlocked, tune.rear_ride_height);
  }

  // 6. Damping (Rebound & Bump).
  {
    const double rf = car.get("FrontSpringDamper.DefDampenReboundRate", 4.0);
    const double rf_lo = car.get("FrontSpringDamper.MinDampenReboundRate", 1.0);
    const double rf_hi = car.get("FrontSpringDamper.MaxDampenReboundRate", 12.0);
    norm_slider("Tuning_frontDampingStiffness", "Front Rebound Stiffness", "", TuneCategory::kDamping, rf_lo, rf_hi,
                rf, std::max(0.1, (rf_hi - rf_lo) / 30.0), damping_unlocked, tune.front_rebound);
    const double rr = car.get("RearSpringDamper.DefDampenReboundRate", 4.0);
    const double rr_lo = car.get("RearSpringDamper.MinDampenReboundRate", 1.0);
    const double rr_hi = car.get("RearSpringDamper.MaxDampenReboundRate", 12.0);
    norm_slider("Tuning_rearDampingStiffness", "Rear Rebound Stiffness", "", TuneCategory::kDamping, rr_lo, rr_hi, rr,
                std::max(0.1, (rr_hi - rr_lo) / 30.0), damping_unlocked, tune.rear_rebound);
    const double bf = car.get("FrontSpringDamper.DefDampenBumpRate", 2.5);
    const double bf_lo = car.get("FrontSpringDamper.MinDampenBumpRate", 1.0);
    const double bf_hi = car.get("FrontSpringDamper.MaxDampenBumpRate", 10.0);
    norm_slider("Tuning_frontBumpRatio", "Front Bump Stiffness", "", TuneCategory::kDamping, bf_lo, bf_hi, bf,
                std::max(0.1, (bf_hi - bf_lo) / 30.0), damping_unlocked, tune.front_bump);
    const double br = car.get("RearSpringDamper.DefDampenBumpRate", 2.5);
    const double br_lo = car.get("RearSpringDamper.MinDampenBumpRate", 1.0);
    const double br_hi = car.get("RearSpringDamper.MaxDampenBumpRate", 10.0);
    norm_slider("Tuning_rearBumpRatio", "Rear Bump Stiffness", "", TuneCategory::kDamping, br_lo, br_hi, br,
                std::max(0.1, (br_hi - br_lo) / 30.0), damping_unlocked, tune.rear_bump);
  }

  // 7. Aero Downforce (kgf at 150 mph).
  {
    const double f0 = car.get("FrontBumperAero.Downforce0", 0.0);
    const double f1 = std::max(f0 + 1.0, car.get("FrontBumperAero.Downforce1", f0));
    const double fu = car.get("FrontBumperAero.DefaultTuneSlider", 0.5);
    norm_slider("Tuning_frontDownforce", "Front Downforce", "kgf", TuneCategory::kAero, f0, f1, lerp(f0, f1, fu),
                std::max(1.0, (f1 - f0) / 25.0), front_aero_unlocked, tune.front_downforce);
    const double r0 = car.get("RearWingAero.Downforce0", 0.0);
    const double r1 = std::max(r0 + 1.0, car.get("RearWingAero.Downforce1", r0));
    const double ru = car.get("RearWingAero.DefaultTuneSlider", 0.5);
    norm_slider("Tuning_rearDownforce", "Rear Downforce", "kgf", TuneCategory::kAero, r0, r1, lerp(r0, r1, ru),
                std::max(1.0, (r1 - r0) / 25.0), rear_aero_unlocked, tune.rear_downforce);
  }

  // 8. Brakes.
  const double def_bias = car.get("List_UpgradeBrakes.BrakeBiasSlider", 0.5) * 100.0;
  add("Tuning_brakeBalance", "Braking Balance (Front)", "%", TuneCategory::kBrake, 0.0, 100.0, def_bias, 1.0,
      brake_unlocked, tune.brake_balance * 100.0, tune.brake_balance >= 0);
  add("Tuning_brakePressure", "Braking Pressure", "%", TuneCategory::kBrake, 0.0, 200.0, 100.0, 5.0, brake_unlocked,
      tune.brake_pressure * 100.0, tune.brake_pressure >= 0);

  // 9. Differential.
  const bool has_front_diff = drivetype == 1 || drivetype == 3;
  const bool has_rear_diff = drivetype == 2 || drivetype == 3;
  const bool has_center_diff = drivetype == 3;
  if (has_front_diff) {
    add("Tuning_frontAccel", "Front Diff Acceleration", "%", TuneCategory::kDifferential, 0.0, 100.0, 50.0, 5.0,
        diff_unlocked, tune.front_diff_accel * 100.0, tune.front_diff_accel >= 0);
    add("Tuning_frontDecel", "Front Diff Deceleration", "%", TuneCategory::kDifferential, 0.0, 100.0, 25.0, 5.0,
        diff_unlocked, tune.front_diff_decel * 100.0, tune.front_diff_decel >= 0);
  }
  if (has_rear_diff) {
    add("Tuning_rearAccel", "Rear Diff Acceleration", "%", TuneCategory::kDifferential, 0.0, 100.0, 65.0, 5.0,
        diff_unlocked, tune.rear_diff_accel * 100.0, tune.rear_diff_accel >= 0);
    add("Tuning_rearDecel", "Rear Diff Deceleration", "%", TuneCategory::kDifferential, 0.0, 100.0, 35.0, 5.0,
        diff_unlocked, tune.rear_diff_decel * 100.0, tune.rear_diff_decel >= 0);
  }
  if (has_center_diff) {
    const double def_split = car.get("List_UpgradeDrivetrainDifferential.RearToqueSplit", 0.6) * 100.0;
    add("Tuning_centerTorque", "Center Diff Balance (Rear)", "%", TuneCategory::kDifferential, 0.0, 100.0, def_split,
        5.0, diff_unlocked, tune.center_diff_split * 100.0, tune.center_diff_split >= 0);
  }

  return out;
}

bool set_tune_slider(drive::CarTune& tune, const cars::CarPhysics& car, const std::string& key, double value) {
  const auto sliders = build_tune_sliders(car, tune);
  const TuneSlider* def = nullptr;
  for (const auto& s : sliders)
    if (s.key == key) {
      def = &s;
      break;
    }
  if (!def) return false;
  const double v = std::clamp(value, def->min_val, def->max_val);
  const double u = inv_lerp(def->min_val, def->max_val, v);

  if (key == "Tuning_frontTirePressure") { tune.front_tire_pressure_psi = v; return true; }
  if (key == "Tuning_rearTirePressure") { tune.rear_tire_pressure_psi = v; return true; }
  if (key == "Tuning_finalDriveRatio") { tune.final_drive = v; return true; }
  static const char* kGearKeys[8] = {"Tuning_firstGear",  "Tuning_secondGear", "Tuning_thirdGear",  "Tuning_fourthGear",
                                     "Tuning_fifthGear",  "Tuning_sixthGear",  "Tuning_seventhGear", "Tuning_eighthGear"};
  for (size_t i = 0; i < 8; ++i)
    if (key == kGearKeys[i]) { tune.gears[i] = v; return true; }
  if (key == "Tuning_frontCamber") { tune.front_camber_deg = v; return true; }
  if (key == "Tuning_rearCamber") { tune.rear_camber_deg = v; return true; }
  if (key == "Tuning_frontToe") { tune.front_toe_deg = v; return true; }
  if (key == "Tuning_rearToe") { tune.rear_toe_deg = v; return true; }
  if (key == "Tuning_frontCaster") { tune.front_caster_deg = v; return true; }
  if (key == "Tuning_frontSwaybar") { tune.front_arb = u; return true; }
  if (key == "Tuning_rearSwaybar") { tune.rear_arb = u; return true; }
  if (key == "Tuning_frontSpring") { tune.front_spring = u; return true; }
  if (key == "Tuning_rearSpring") { tune.rear_spring = u; return true; }
  if (key == "Tuning_frontRideHeight") { tune.front_ride_height = u; return true; }
  if (key == "Tuning_rearRideHeight") { tune.rear_ride_height = u; return true; }
  if (key == "Tuning_frontDampingStiffness") { tune.front_rebound = u; return true; }
  if (key == "Tuning_rearDampingStiffness") { tune.rear_rebound = u; return true; }
  if (key == "Tuning_frontBumpRatio") { tune.front_bump = u; return true; }
  if (key == "Tuning_rearBumpRatio") { tune.rear_bump = u; return true; }
  if (key == "Tuning_frontDownforce") { tune.front_downforce = u; return true; }
  if (key == "Tuning_rearDownforce") { tune.rear_downforce = u; return true; }
  if (key == "Tuning_brakeBalance") { tune.brake_balance = v / 100.0; return true; }
  if (key == "Tuning_brakePressure") { tune.brake_pressure = v / 100.0; return true; }
  if (key == "Tuning_frontAccel") { tune.front_diff_accel = v / 100.0; return true; }
  if (key == "Tuning_frontDecel") { tune.front_diff_decel = v / 100.0; return true; }
  if (key == "Tuning_rearAccel") { tune.rear_diff_accel = v / 100.0; return true; }
  if (key == "Tuning_rearDecel") { tune.rear_diff_decel = v / 100.0; return true; }
  if (key == "Tuning_centerTorque") { tune.center_diff_split = v / 100.0; return true; }
  return false;
}

void reset_tune(drive::CarTune& tune) { tune = drive::CarTune{}; }

void reset_tune_category(drive::CarTune& tune, TuneCategory category) {
  switch (category) {
    case TuneCategory::kTires:
      tune.front_tire_pressure_psi = tune.rear_tire_pressure_psi = -1.0;
      break;
    case TuneCategory::kGearing:
      tune.final_drive = -1.0;
      tune.gears.fill(-1.0);
      break;
    case TuneCategory::kAlignment:
      tune.front_camber_deg = tune.rear_camber_deg = tune.front_toe_deg = tune.rear_toe_deg = tune.front_caster_deg =
          -100.0;
      break;
    case TuneCategory::kAntiRollBars:
      tune.front_arb = tune.rear_arb = -1.0;
      break;
    case TuneCategory::kSprings:
      tune.front_spring = tune.rear_spring = tune.front_ride_height = tune.rear_ride_height = -1.0;
      break;
    case TuneCategory::kDamping:
      tune.front_rebound = tune.rear_rebound = tune.front_bump = tune.rear_bump = -1.0;
      break;
    case TuneCategory::kAero:
      tune.front_downforce = tune.rear_downforce = -1.0;
      break;
    case TuneCategory::kBrake:
      tune.brake_balance = tune.brake_pressure = -1.0;
      break;
    case TuneCategory::kDifferential:
      tune.front_diff_accel = tune.front_diff_decel = tune.rear_diff_accel = tune.rear_diff_decel =
          tune.center_diff_split = -1.0;
      break;
    default: break;
  }
}

std::string tune_to_tokens(const drive::CarTune& t) {
  std::ostringstream o;
  o.precision(6);
  auto emit = [&](const char* k, double v, bool active) {
    if (!active) return;
    if (o.tellp() > 0) o << " ";
    o << k << "=" << v;
  };
  emit("Tuning_frontTirePressure", t.front_tire_pressure_psi, t.front_tire_pressure_psi >= 0);
  emit("Tuning_rearTirePressure", t.rear_tire_pressure_psi, t.rear_tire_pressure_psi >= 0);
  emit("Tuning_finalDriveRatio", t.final_drive, t.final_drive > 0);
  static const char* kGearKeys[8] = {"Tuning_firstGear",  "Tuning_secondGear", "Tuning_thirdGear",  "Tuning_fourthGear",
                                     "Tuning_fifthGear",  "Tuning_sixthGear",  "Tuning_seventhGear", "Tuning_eighthGear"};
  for (size_t i = 0; i < 8; ++i) emit(kGearKeys[i], t.gears[i], t.gears[i] > 0);
  emit("Tuning_frontCamber", t.front_camber_deg, t.front_camber_deg > -90);
  emit("Tuning_rearCamber", t.rear_camber_deg, t.rear_camber_deg > -90);
  emit("Tuning_frontToe", t.front_toe_deg, t.front_toe_deg > -90);
  emit("Tuning_rearToe", t.rear_toe_deg, t.rear_toe_deg > -90);
  emit("Tuning_frontCaster", t.front_caster_deg, t.front_caster_deg > -90);
  emit("Tuning_frontSwaybar", t.front_arb, t.front_arb >= 0);
  emit("Tuning_rearSwaybar", t.rear_arb, t.rear_arb >= 0);
  emit("Tuning_frontSpring", t.front_spring, t.front_spring >= 0);
  emit("Tuning_rearSpring", t.rear_spring, t.rear_spring >= 0);
  emit("Tuning_frontRideHeight", t.front_ride_height, t.front_ride_height >= 0);
  emit("Tuning_rearRideHeight", t.rear_ride_height, t.rear_ride_height >= 0);
  emit("Tuning_frontDampingStiffness", t.front_rebound, t.front_rebound >= 0);
  emit("Tuning_rearDampingStiffness", t.rear_rebound, t.rear_rebound >= 0);
  emit("Tuning_frontBumpRatio", t.front_bump, t.front_bump >= 0);
  emit("Tuning_rearBumpRatio", t.rear_bump, t.rear_bump >= 0);
  emit("Tuning_frontDownforce", t.front_downforce, t.front_downforce >= 0);
  emit("Tuning_rearDownforce", t.rear_downforce, t.rear_downforce >= 0);
  emit("Tuning_brakeBalance", t.brake_balance, t.brake_balance >= 0);
  emit("Tuning_brakePressure", t.brake_pressure, t.brake_pressure >= 0);
  emit("Tuning_frontAccel", t.front_diff_accel, t.front_diff_accel >= 0);
  emit("Tuning_frontDecel", t.front_diff_decel, t.front_diff_decel >= 0);
  emit("Tuning_rearAccel", t.rear_diff_accel, t.rear_diff_accel >= 0);
  emit("Tuning_rearDecel", t.rear_diff_decel, t.rear_diff_decel >= 0);
  emit("Tuning_centerTorque", t.center_diff_split, t.center_diff_split >= 0);
  return o.str();
}

bool parse_tune_token(const std::string& kv, drive::CarTune& t) {
  const size_t eq = kv.find('=');
  if (eq == std::string::npos) return false;
  const std::string key = kv.substr(0, eq);
  if (key.rfind("Tuning_", 0) != 0) return false;
  const double v = std::strtod(kv.c_str() + eq + 1, nullptr);
  if (key == "Tuning_frontTirePressure") { t.front_tire_pressure_psi = v; return true; }
  if (key == "Tuning_rearTirePressure") { t.rear_tire_pressure_psi = v; return true; }
  if (key == "Tuning_finalDriveRatio") { t.final_drive = v; return true; }
  static const char* kGearKeys[8] = {"Tuning_firstGear",  "Tuning_secondGear", "Tuning_thirdGear",  "Tuning_fourthGear",
                                     "Tuning_fifthGear",  "Tuning_sixthGear",  "Tuning_seventhGear", "Tuning_eighthGear"};
  for (size_t i = 0; i < 8; ++i)
    if (key == kGearKeys[i]) { t.gears[i] = v; return true; }
  if (key == "Tuning_frontCamber") { t.front_camber_deg = v; return true; }
  if (key == "Tuning_rearCamber") { t.rear_camber_deg = v; return true; }
  if (key == "Tuning_frontToe") { t.front_toe_deg = v; return true; }
  if (key == "Tuning_rearToe") { t.rear_toe_deg = v; return true; }
  if (key == "Tuning_frontCaster") { t.front_caster_deg = v; return true; }
  if (key == "Tuning_frontSwaybar") { t.front_arb = v; return true; }
  if (key == "Tuning_rearSwaybar") { t.rear_arb = v; return true; }
  if (key == "Tuning_frontSpring") { t.front_spring = v; return true; }
  if (key == "Tuning_rearSpring") { t.rear_spring = v; return true; }
  if (key == "Tuning_frontRideHeight") { t.front_ride_height = v; return true; }
  if (key == "Tuning_rearRideHeight") { t.rear_ride_height = v; return true; }
  if (key == "Tuning_frontDampingStiffness") { t.front_rebound = v; return true; }
  if (key == "Tuning_rearDampingStiffness") { t.rear_rebound = v; return true; }
  if (key == "Tuning_frontBumpRatio") { t.front_bump = v; return true; }
  if (key == "Tuning_rearBumpRatio") { t.rear_bump = v; return true; }
  if (key == "Tuning_frontDownforce") { t.front_downforce = v; return true; }
  if (key == "Tuning_rearDownforce") { t.rear_downforce = v; return true; }
  if (key == "Tuning_brakeBalance") { t.brake_balance = v; return true; }
  if (key == "Tuning_brakePressure") { t.brake_pressure = v; return true; }
  if (key == "Tuning_frontAccel") { t.front_diff_accel = v; return true; }
  if (key == "Tuning_frontDecel") { t.front_diff_decel = v; return true; }
  if (key == "Tuning_rearAccel") { t.rear_diff_accel = v; return true; }
  if (key == "Tuning_rearDecel") { t.rear_diff_decel = v; return true; }
  if (key == "Tuning_centerTorque") { t.center_diff_split = v; return true; }
  return false;
}

}  // namespace garage
