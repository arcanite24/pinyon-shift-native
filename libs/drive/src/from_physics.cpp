#include "cars/mods.h"
#include "drive/from_physics.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

#include "drive/from_bake.h"

namespace drive {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180;

double ramp(double x, double x0, double x1) {
  if (x1 == x0) return x >= x1 ? 1 : 0;
  return std::clamp((x - x0) / (x1 - x0), 0.0, 1.0);
}

}  // namespace

double sidewall_stretch(double width_mm, double aspect, const cars::PhysicsSettings& s) {
  const double h = width_mm * aspect / 100 / 1000;  // m
  const double h0 = s.get("SidewallScalePeakSA\\TireSidewall0", 0.0781);
  const double h1 = s.get("SidewallScalePeakSA\\TireSidewall1", 0.1143);
  const double f0 = s.get("SidewallScalePeakSA\\TireSidewallScalePeakSASR0", 1);
  const double f1 = s.get("SidewallScalePeakSA\\TireSidewallScalePeakSASR1", 1.5);
  return f0 + (f1 - f0) * ramp(h, h0, h1);
}

void geometry_from_physics(const cars::CarPhysics& car, CarParams& p) {
  const double wheelbase = car.get("Data_CarBody.ModelWheelbase", 2.6);
  for (int i = 0; i < kWheels; ++i) {
    const bool front = axle_of(i) == 0;
    const std::string axle = front ? "Front" : "Rear";
    const double width = car.get("Data_Car." + axle + "TireWidthMM", 225) / 1000;
    const double aspect = car.get("Data_Car." + axle + "TireAspect", 45);
    const double rim = car.get("Data_Car." + axle + "WheelDiameterIN", 17) * 0.0254;
    const double radius = rim / 2 + width * aspect / 100;
    const double track = car.get(std::string("Data_CarBody.Model") + axle + "TrackOuter", 1.75) - width;
    p.tyre_radius[size_t(i)] = radius;
    p.wheel_centres[size_t(i)] = {(i % 2 == 0 ? -0.5 : 0.5) * track, radius, (front ? 0.5 : -0.5) * wheelbase};
  }
}

void apply_physics(const cars::CarPhysics& car, const cars::PhysicsSettings& s, CarParams& p,
                   std::vector<std::string>* notes) {
  auto get = [&](const std::string& key, double fallback) {
    if (!car.has(key)) {
      if (notes) notes->push_back("no " + key + ": " + std::to_string(fallback));
      return fallback;
    }
    return car.get(key);
  };

  // Mass, centre of mass and inertia.
  const double m = get("List_UpgradeCarBodyWeight.Mass", p.mass);
  p.mass = m;
  const double z_front = (p.wheel_centres[0].z + p.wheel_centres[1].z) / 2;
  const double z_rear = (p.wheel_centres[2].z + p.wheel_centres[3].z) / 2;
  const double track = std::fabs(p.wheel_centres[1].x - p.wheel_centres[0].x);
  p.centre_of_mass.z = z_rear + get("List_UpgradeCarBodyWeight.CMBackFront", 0.5) * (z_front - z_rear);
  p.centre_of_mass.y = get("List_UpgradeCarBodyWeight.CMHeight", p.centre_of_mass.y);
  // CMLeftRight 0.5 is the centre; the side it measures from is our guess.
  p.centre_of_mass.x = (get("List_UpgradeCarBodyWeight.CMLeftRight", 0.5) - 0.5) * track;
  // SPEC-CAR-PHYSICS scenario B, load transfer: the tyres' longitudinal
  // forces pitch the body through a 0.27 m arm (measured fit, one car).
  p.longitudinal_pitch_arm = 0.27;
  p.lateral_roll_arm = 0.28;  // SPEC-CAR-PHYSICS, Check: the first corner (0.278 m)
  p.inertia = {};
  p.inertia_box = {get("List_UpgradeCarBodyWeight.BlockDimX", p.inertia_box.x),
                   get("List_UpgradeCarBodyWeight.BlockDimY", p.inertia_box.y),
                   get("List_UpgradeCarBodyWeight.BlockDimZ", p.inertia_box.z)};
  p.gravity = 9.8 * s.get("CarGravityScale", 1);
  p.airborne_gravity_scale = s.get("AirbornGravityScale", 1.15);
  p.per_tyre_off_gravity_scale = s.get("PerTireOffGroundGravityScale", 1.05);

  // Suspension: rates per unit of the car's mass.
  for (int a = 0; a < 2; ++a) {
    const std::string sd = a == 0 ? "FrontSpringDamper." : "RearSpringDamper.";
    const std::string ar = a == 0 ? "FrontAntiSway." : "RearAntiSway.";
    p.spring_rate[a] = get(sd + "DefSpringRate", p.spring_rate[a] / m) * m;
    p.bump_rate[a] = get(sd + "DefDampenBumpRate", p.bump_rate[a] / m) * m;
    p.rebound_rate[a] = get(sd + "DefDampenReboundRate", p.rebound_rate[a] / m) * m;
    p.damper_clamp[a] = get(sd + "DampenBumpClamp", p.damper_clamp[a] / m) * m;
    const double travel = get(sd + "DefRideHeight", 0.2) - get(sd + "MaxCompressHeight", 0.12);
    p.bump_travel[a] = travel > 0.02 ? travel : 0.08;
    p.max_droop[a] = get(sd + "MaxStretchDeltaFromRideHeight", 0.5);
    p.bumpstop_rate[a] = get(sd + "BumpstopStiffness", 200) * m;
    p.bumpstop_damping[a] = get(sd + "BumpstopDamping", 2) * m;
    p.antiroll_rate[a] = get(ar + "DefSwaybarStiffness", p.antiroll_rate[a] / m) * m;
    p.antiroll_damping[a] = get(ar + "SwaybarDamping", 2) * m;
  }

  // Tyres.
  auto curves = [&](const char* group, const char* name, TyreCurves& out) {
    const std::string g = group;
    const double max_slip = get(g + ".MaxSlip", out.light.max_slip);
    for (int c = 0; c < 2; ++c) {
      FrictionCurve& f = c == 0 ? out.light : out.heavy;
      if (const auto* v = car.curve(name + std::to_string(c)); v && v->size() >= 2) {
        f.values = *v;
        f.max_slip = max_slip;
      } else if (notes) {
        notes->push_back(std::string("no curve ") + name + std::to_string(c) + ": synthetic");
      }
    }
    out.light_load_kgf = get(g + ".MinLoadCurve", out.light_load_kgf);
    out.heavy_load_kgf = get(g + ".MaxLoadCurve", out.heavy_load_kgf);
    out.load_clamp_kgf = get(g + ".LoadClamp", out.load_clamp_kgf);
  };
  for (int c = 0; c < 2; ++c) {
    p.long_curve_scale[size_t(c)] = get("LongMultiCurve.FrictionScale" + std::to_string(c), 1);
    p.lat_curve_scale[size_t(c)] = get("LatMultiCurve.FrictionScale" + std::to_string(c), 1);
  }
  p.lat_compound_scale = get("List_TireCompound.TorqueFreeLatFrictionScale", 1);
  curves("LongMultiCurve", "long", p.longitudinal);
  curves("LatMultiCurve", "lat", p.lateral);
  for (int a = 0; a < 2; ++a) {
    const std::string axle = a == 0 ? "Front" : "Rear";
    p.sidewall_stretch[a] =
        sidewall_stretch(get("Data_Car." + axle + "TireWidthMM", 225), get("Data_Car." + axle + "TireAspect", 45), s);
    p.long_scale[a] = get("List_UpgradeCarBodyChassisStiffness." + axle + "LongFrictionScale", 1);
    p.lat_scale[a] = get("List_UpgradeCarBodyChassisStiffness." + axle + "LatFrictionScale", 1);
  }
  p.lateral_peak_min_deg = s.get("SidewallScalePeakSA\\TireClampHeavyPeakSALow", 4);
  p.lateral_peak_max_deg = s.get("SidewallScalePeakSA\\TireClampHeavyPeakSAHigh", 18);
  p.accel_scale0 = get("List_TireCompound.TorqueFreeLongFrictionScaleAccel0", 1);
  p.accel_scale1 = get("List_TireCompound.TorqueFreeLongFrictionScaleAccel1", 1);
  p.accel_speed0 = get("List_TireCompound.TorqueFreeLongFrictionScaleAccelSpeed0", 5);
  p.accel_speed1 = get("List_TireCompound.TorqueFreeLongFrictionScaleAccelSpeed1", 15);
  p.wheel_inertia = get("List_TireCompound.MomentInertia", p.wheel_inertia);
  // The tyre-width scale, linear in the width between the compound's two
  // points and clamped (probable).
  for (int a = 0; a < 2; ++a) {
    const double w0 = get("List_TireCompound.TireFricWidth0", 0), w1 = get("List_TireCompound.TireFricWidth1", 0);
    const double s0 = get("List_TireCompound.TireFricScale0", 1), s1 = get("List_TireCompound.TireFricScale1", 1);
    const double width = get(std::string("Data_Car.") + (a == 0 ? "Front" : "Rear") + "TireWidthMM", 225);
    p.width_scale[size_t(a)] = w1 > w0 ? s0 + (s1 - s0) * std::clamp((width - w0) / (w1 - w0), 0.0, 1.0) : 1;
  }
  p.rear_fix_scale = get("Data_Car.FixListingRearFricScale", 1);
  p.fix_slip0 = get("Data_Car.FixListingNormSlip0", 0.5);
  p.fix_slip1 = get("Data_Car.FixListingNormSlip1", 1);
  p.fix_steer0 = get("Data_Car.FixListingSteerAngle0", 0.5);
  p.fix_steer1 = get("Data_Car.FixListingSteerAngle1", 1);

  // Engine (SPEC-CAR-PHYSICS, Engine torque, forced induction and
  // GameTorqueScale).
  if (const auto* t = car.curve("torque"); t && t->size() >= 3) {
    p.torque_curve.assign(t->begin(), t->end());
    p.torque_curve_slots = double(t->size() - 1);
    if (p.torque_curve.back() < 0) p.torque_curve.pop_back();  // the negative sentinel
  } else if (notes) {
    notes->push_back("no torque curve: synthetic");
  }
  p.torque_curve_max_rpm = get("List_UpgradeEngineCamshaft.TorqueCurveMaxRPM", p.torque_curve_max_rpm);
  double upg_sum = 1.0;
  for (const char* t : {"List_UpgradeEngineValves", "List_UpgradeEngineDisplacement",
                        "List_UpgradeEnginePistonsCompression", "List_UpgradeEngineFuelSystem",
                        "List_UpgradeEngineIgnition", "List_UpgradeEngineExhaust",
                        "List_UpgradeEngineIntake", "List_UpgradeEngineManifold",
                        "List_UpgradeEngineOilCooling", "List_UpgradeEngineRestrictorPlate"}) {
    const std::string k = std::string(t) + ".TorqueScale";
    if (car.has(k)) upg_sum += car.get(k, 1.0) - 1.0;
  }
  std::string turbo_table;
  for (const char* t :
       {"List_UpgradeEngineTurboSingle", "List_UpgradeEngineTurboTwin", "List_UpgradeEngineTurboQuad"}) {
    if (car.has(std::string(t) + ".MaxScale")) {
      turbo_table = t;
      break;
    }
  }
  std::string sc_table;
  for (const char* t : {"List_UpgradeEngineDSC", "List_UpgradeEngineCSC"}) {
    if (car.has(std::string(t) + ".RedlineRPMScale")) {
      sc_table = t;
      break;
    }
  }
  // database.cpp folds upg_sum into List_TorqueCurve.TorqueScale; recover the
  // base curve scale and apply S_upg (upgrades + RobScale) so MinScale_eff /
  // MaxScale_eff divide by S_upg identically on baked and fresh CarPhysics.
  const double raw_torque_scale =
      get("List_TorqueCurve.TorqueScale", p.torque_scale) / (upg_sum > 1e-6 ? upg_sum : 1.0);
  double s_upg = upg_sum;
  if (!turbo_table.empty()) s_upg += car.get(turbo_table + ".RobScale", 1.0) - 1.0;
  if (!sc_table.empty()) s_upg += car.get(sc_table + ".RobScale", 1.0) - 1.0;
  s_upg = std::max(1e-3, s_upg);
  p.torque_scale = raw_torque_scale * s_upg;
  p.engine_braking = get("List_TorqueCurve.ZeroThrottleTorqueScale", p.engine_braking);
  p.idle_rpm = get("List_UpgradeEngineCamshaft.StallRPM", p.idle_rpm);
  p.redline_rpm = get("List_UpgradeEngineCamshaft.RedlineRPM", p.redline_rpm);
  p.launch_rpm = get("List_UpgradeEngineCamshaft.StartRPM", p.launch_rpm);
  p.engine_inertia = get("Data_Engine.MomentInertia", 0.1) + get("List_UpgradeEngineFlywheel.MomentInertia", 0.1);
  p.game_torque_scale = std::clamp(get("Data_Car.GameTorqueScale", 1.0), 0.5, 1.5);

  const double ic_scale = car.get("List_UpgradeEngineIntercooler.MaxScaleScale", 1.0);
  p.has_turbo = !turbo_table.empty();
  if (p.has_turbo) {
    const double min_s = car.get(turbo_table + ".MinScale", 1.0);
    const double max_s = car.get(turbo_table + ".MaxScale", 1.0);
    p.turbo_tau = std::max(1e-3, 0.01 * car.get(turbo_table + ".MomentInertia", 19.0));
    p.turbo_power_min = 745.7 * car.get(turbo_table + ".PowerMinScale", 10.0);
    p.turbo_power_max = 745.7 * car.get(turbo_table + ".PowerMaxScale", 122.0);
    p.boost_min_psi = 3.675;
    p.boost_max_psi = 14.7 * (2.0 * (max_s - 1.0) + 1.0);
    p.boost_scale_min = 1.0 + (min_s - 1.0) / s_upg;
    p.boost_scale_max = 1.0 + ((max_s - 1.0) + (ic_scale - 1.0)) / s_upg;
    p.turbo_dropoff_rpm0 = car.get(turbo_table + ".TorqueDropOffRPM0", 0.0);
    p.turbo_dropoff_rpm1 = car.get(turbo_table + ".TorqueDropOffRPM1", 0.0);
    p.turbo_dropoff_scale0 = car.get(turbo_table + ".TorqueDropOffScale0", 1.0);
    p.turbo_dropoff_scale1 = car.get(turbo_table + ".TorqueDropOffScale1", 1.0);
  }
  p.has_supercharger = !sc_table.empty();
  if (p.has_supercharger) {
    const double zero_s = car.get(sc_table + ".ZeroRPMScale", 1.0);
    const double red_s = car.get(sc_table + ".RedlineRPMScale", 1.0);
    p.sc_zero_psi = 14.7 * (2.0 * (zero_s - 1.0) + 1.0);
    p.sc_redline_psi = 14.7 * (2.0 * (red_s - 1.0) + 1.0);
    p.sc_scale_zero = 1.0 + (zero_s - 1.0) / s_upg;
    p.sc_scale_redline = 1.0 + ((red_s - 1.0) + (ic_scale - 1.0)) / s_upg;
    p.sc_dropoff_rpm0 = car.get(sc_table + ".TorqueDropOffRPM0", 0.0);
    p.sc_dropoff_rpm1 = car.get(sc_table + ".TorqueDropOffRPM1", 0.0);
    p.sc_dropoff_scale0 = car.get(sc_table + ".TorqueDropOffScale0", 1.0);
    p.sc_dropoff_scale1 = car.get(sc_table + ".TorqueDropOffScale1", 1.0);
  }
  p.boost_lag = 0.0;

  // Drivetrain.
  const int type = int(std::lround(get("Data_Drivetrain.DrivetypeID", 2)));
  p.layout = type == 1 ? CarParams::Layout::kFront : type == 3 ? CarParams::Layout::kAll : CarParams::Layout::kRear;
  p.rear_split = get("List_UpgradeDrivetrainDifferential.RearToqueSplit", 0.5);
  auto read_lsd = [&](const std::string& prefix) {
    CarParams::LsdParams d;
    const std::string base = "List_UpgradeDrivetrainDifferential." + prefix + "LimitedSlip";
    d.accel = get(base + "TorqueAccel", 0);
    d.decel = get(base + "TorqueDecel", 0);
    d.rel_vel_clamp = get(base + "RelVelClamp", 50);
    d.accel_def_in = get(base + "AccelDefInputTorque", 50);
    return d;
  };
  p.front_lsd = read_lsd("Front");
  p.rear_lsd = read_lsd("Rear");
  p.center_lsd = read_lsd("Center");
  const std::string tr = "List_UpgradeDrivetrainTransmission.";
  const int count = int(std::lround(get(tr + "NumGears", 7)));
  std::vector<double> gears;
  for (int g = 1; g < count && g <= 8; ++g) {
    const double r = get(tr + "GearRatio" + std::to_string(g), -1);
    if (r > 0) gears.push_back(r);
  }
  if (!gears.empty()) p.gears = gears;
  p.reverse_gear = std::fabs(get(tr + "GearRatio0", -p.reverse_gear));
  p.final_drive = get(tr + "FinalDriveRatio", p.final_drive);
  p.gearbox_inertia = get(tr + "MomentInertia", p.gearbox_inertia);
  p.gear_shift_time = get(tr + "GearShiftTime", p.gear_shift_time);
  p.driveline_inertia = get("List_UpgradeDrivetrainDriveline.MomentInertia", p.driveline_inertia);
  p.clutch_max_torque = get("List_UpgradeDrivetrainClutch.ClutchMaxTorque", p.clutch_max_torque);
  p.clutch_in_time = get("List_UpgradeDrivetrainClutch.ClutchInTime", p.clutch_in_time);
  p.clutch_out_time = get("List_UpgradeDrivetrainClutch.ClutchOutTime", p.clutch_out_time);

  // Direct clutch capacity (SPEC-CAR-PHYSICS, Clutch and gearbox states:
  // with direct J = engine + flywheel and unscaled T_engine, no 0.72 factor).
  p.clutch_torque_factor = 1.0;
  p.reverse_select_speed = s.get("MaxForwardVelShiftReverseMPH", 15) * 0.44704;
  p.reverse_auto_enter_speed = s.get("MaxForwardSpeedMphToAutoEnterReverse",
                                     s.get("MaxForwardVelShiftReverseMPH", 15)) * 0.44704;
  p.shift_reverse_pause = s.get("ShiftReversePause", 0.0);

  // Brakes (SPEC-CAR-PHYSICS, Brakes): per-car brake constants and 10-point
  // decel table computed from BrakeTorqueSlider, BrakeBiasSlider,
  // BrakeBiasOffsetRear, BiasHandbrake, tyres, mass, geometry, and aero.
  p.use_per_car_brakes = true;
  p.brake_torque_slider = get("List_UpgradeBrakes.BrakeTorqueSlider", 0.5);
  p.brake_bias_slider = get("List_UpgradeBrakes.BrakeBiasSlider", 0.5);
  p.brake_bias_offset_rear = s.get("BrakeBiasOffsetRear", 0.03);
  p.brake_pressure_min_share = s.get("BrakePressureMaxPercentFrontOrRear", 0.25);
  p.bias_handbrake = get("List_UpgradeBrakes.BiasHandbrake", 1.5);
  for (int k = 0; k < 4; ++k) {
    p.brake_input_in[k] = s.get("BrakeInputScale\\BrakeInputScaleInput" + std::to_string(k), p.brake_input_in[k]);
    p.brake_input_out[k] = s.get("BrakeInputScale\\BrakeInputScaleOutput" + std::to_string(k), p.brake_input_out[k]);
  }
  p.abs_ai_release = get("List_UpgradeBrakes.ReleasePointABS", p.abs_ai_release);
  // The tyre's longitudinal scale on a wheel whose applied torque slows it.
  p.brake_friction_scale = get("List_TireCompound.TorqueFreeLongFrictionScaleBrake", 1) *
                           get("List_UpgradeBrakes.GameFrictionScaleBraking", 1);
  p.abs_front_no_steer = get("List_UpgradeBrakes.ReleasePointABSFrontNoSteer", p.abs_front_no_steer);
  p.abs_front_full_steer = get("List_UpgradeBrakes.ReleasePointABSFrontFullSteer", p.abs_front_full_steer);
  p.abs_rear_no_steer = get("List_UpgradeBrakes.ReleasePointABSRearNoSteer", p.abs_rear_no_steer);
  p.abs_rear_full_steer = get("List_UpgradeBrakes.ReleasePointABSRearFullSteer", p.abs_rear_full_steer);
  p.abs_duration = get("List_UpgradeBrakes.DurationABS", p.abs_duration);
  p.no_abs_brake_travel = s.get("NoABSBrakeTravel", p.no_abs_brake_travel);
  p.no_abs_disable_time = s.get("NoABSDisableTime", p.no_abs_disable_time);
  p.no_abs_front_no_steer = s.get("NoABSSlipDefFrontNoSteer", p.no_abs_front_no_steer);
  p.no_abs_front_full_steer = s.get("NoABSSlipDefFrontFullSteer", p.no_abs_front_full_steer);
  p.no_abs_rear_no_steer = s.get("NoABSSlipDefRearNoSteer", p.no_abs_rear_no_steer);
  p.no_abs_rear_full_steer = s.get("NoABSSlipDefRearFullSteer", p.no_abs_rear_full_steer);
  p.abs_off_braking_friction_scale = s.get("ABSOffBrakingFrictionScale", p.abs_off_braking_friction_scale);

  // TCS, STM, and Manual + Clutch settings (PhysicsSettings.ini).
  p.tcs_speed_mph = s.get("TractionControlSpeed", p.tcs_speed_mph);
  p.tcs_steer_scale_rwd = s.get("TCSFullSteerSlipScaleRWD", p.tcs_steer_scale_rwd);
  p.tcs_steer_scale_fwd = s.get("TCSFullSteerSlipScaleFWD", p.tcs_steer_scale_fwd);
  p.tcs_steer_scale_awd = s.get("TCSFullSteerSlipScaleAWD", p.tcs_steer_scale_awd);
  p.tcs_fric_diff = s.get("TCSFullEffectFricDiff", p.tcs_fric_diff);
  p.tcs_yaw_min_deg = s.get("YawMinAngleForTCS", p.tcs_yaw_min_deg);
  p.tcs_yaw_max_deg = s.get("YawMaxAngleForTCS", p.tcs_yaw_max_deg);
  p.stm_speed_mph = s.get("StabilityManageSpeed", p.stm_speed_mph);
  p.clutch_pedal_dead = s.get("ClutchPedalDistBeforeTorqueStarts", p.clutch_pedal_dead);
  p.clutch_torque_exp = s.get("ClutchTorqueCurveExponent", p.clutch_torque_exp);
  p.clutch_max_vel = s.get("ClutchMaxVel", p.clutch_max_vel);
  p.clutch_vel_no_clutch = s.get("ClutchVelShiftWithoutClutch", p.clutch_vel_no_clutch);
  p.clutch_stall_timeout = s.get("ClutchStallTimeout", p.clutch_stall_timeout);
  p.shift_without_clutch_damage = s.get("ShiftWithoutClutchDamageAmount", p.shift_without_clutch_damage);

  // Input shaping and steering.
  p.throttle_rate = s.get("ControllerDelays\\AccelDelaySpeed", p.throttle_rate);
  p.brake_rate = s.get("ControllerDelays\\BrakeDelaySpeed", p.brake_rate);
  p.handbrake_rate = s.get("ControllerDelays\\HandBrakeDelaySpeed", p.handbrake_rate);
  p.max_steer = get("Data_Car.SteerMaxAngle", 35) * kDeg;
  p.max_steer_filtered = get("Data_Car.SteerMaxAngleFiltered", get("Data_Car.SteerMaxAngle", 35)) * kDeg;
  p.steer_rate = get("Data_Car.SteerMaxAngVelTurning", 105) * kDeg;
  p.straighten_rate = get("Data_Car.SteerMaxAngVelStraighten", 105) * kDeg;
  p.steer_countersteer_rate =
      get("Data_Car.SteerAngVelCountersteer", s.get("SteerDefCountersteerRate", 105)) * kDeg;
  p.steer_max_gees = get("Data_Car.SteerSpeedSensitiveMaxGees", 1);
  p.steer_min_lock = get("Data_Car.SteerSpeedSensitiveMinMaxAngle", 5) * kDeg;

  // Aerodynamics (SPEC-CAR-PHYSICS, Aerodynamics): DB BodyAero* and
  // List_AeroPhysics columns are in kgf at 150 mph (67.056 m/s).
  constexpr double kAero = 9.80665 / (67.056 * 67.056);
  double part_drag_scales = 1.0;
  for (const char* t : {"List_UpgradeCarBodyFrontBumper", "List_UpgradeCarBodyHood",
                        "List_UpgradeCarBodyRearBumper", "List_UpgradeCarBodySideSkirt",
                        "List_UpgradeCarBodyTireWidthFront", "List_UpgradeCarBodyTireWidthRear",
                        "List_UpgradeRearWing", "List_UpgradeRimSizeFront", "List_UpgradeRimSizeRear",
                        "List_UpgradeSpringDamper", "List_UpgradeTireCompound", "List_UpgradeEngine",
                        "List_UpgradeEngineCSC", "List_UpgradeEngineDSC", "List_UpgradeEngineExhaust",
                        "List_UpgradeEngineFuelSystem", "List_UpgradeEngineIgnition",
                        "List_UpgradeEngineIntake", "List_UpgradeEngineIntercooler"}) {
    const std::string k = std::string(t) + ".DragScale";
    if (car.has(k)) part_drag_scales *= car.get(k, 1.0);
  }
  p.aero_drag_long =
      get("Data_Car.BodyAeroLongitudinalDrag", 0) * kAero * get("Data_Car.GameDragScale", 1) * part_drag_scales;
  p.aero_drag_lat_front = get("Data_Car.BodyAeroLateralDragFront", 0) * kAero;
  p.aero_drag_lat_rear = get("Data_Car.BodyAeroLateralDragRear", 0) * kAero;
  p.aero_drag_vert = get("Data_Car.BodyAeroVerticalDrag", 0) * kAero;
  p.aero_df_front = (get("Data_Car.BodyAeroForwardDownforceFront", 0) +
                     car.get("List_UpgradeCarBodyRearBumper.BodyAeroForwardDownforceFront", 0)) *
                    kAero;
  p.aero_df_rear = (get("Data_Car.BodyAeroForwardDownforceRear", 0) +
                    car.get("List_UpgradeCarBodyRearBumper.BodyAeroForwardDownforceRear", 0)) *
                   kAero;
  p.aero_zero_df_rad = get("Data_Car.BodyAeroAngleZeroDownforce", 45) * kDeg;
  auto read_aero_part = [&](const std::string& group, double& drag_out, double& df_out) {
    if (!car.has(group + ".Drag0")) {
      drag_out = df_out = 0;
      return;
    }
    const double u = std::clamp(car.get(group + ".DefaultTuneSlider", 0.5), 0.0, 1.0);
    const double d0 = car.get(group + ".Drag0", 0), d1 = car.get(group + ".Drag1", 0);
    const double f0 = car.get(group + ".Downforce0", 0), f1 = car.get(group + ".Downforce1", 0);
    drag_out = (d0 + (d1 - d0) * u) * kAero;
    df_out = (f0 + (f1 - f0) * u) * kAero;
  };
  read_aero_part("FrontBumperAero", p.aero_part_drag_front, p.aero_part_df_front);
  read_aero_part("RearWingAero", p.aero_part_drag_rear, p.aero_part_df_rear);
  p.aero_part_lat_drag =
      (car.get("FrontBumperAero.LateralDrag", 0) + car.get("RearWingAero.LateralDrag", 0)) * kAero;
  p.high_speed_drag_v0 = s.get("HighSpeedDragSpeed0MetersSec", 119.3);
  p.high_speed_drag_v1 = s.get("HighSpeedDragSpeed1MetersSec", 122.9);
  p.high_speed_drag_s0 = s.get("HighSpeedDragScale0", 1.0);
  p.high_speed_drag_s1 = s.get("HighSpeedDragScale1", 1.5);
  if (p.aero_drag_long > 0) {
    p.drag_quadratic = p.aero_drag_long + p.aero_part_drag_front + p.aero_part_drag_rear;
    p.drag_constant = 0;
  }

  p.physics_hz = s.get("CarPhysicsUpdateHz", 359);
}

CarParams params_for_car_text(const cars::bake::CarInfo& car, const std::string& text, std::vector<std::string>* notes);

CarParams params_for_baked_car(const cars::bake::CarInfo& car, const std::filesystem::path& bake_dir,
                               std::vector<std::string>* notes) {
  std::string name = car.media_name;
  for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
  const auto path = cars::mods::resolve(bake_dir / "cars" / (name + ".physics.txt"));
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (notes) notes->push_back("no " + path.string() + ": the synthetic car's physics (carbake --physics-only writes it)");
    return params_from_bake(car);
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return params_for_car_text(car, ss.str(), notes);
}

CarParams params_for_fitted_car(const cars::bake::CarInfo& car, const std::filesystem::path& bake_dir,
                                const cars::CarPhysics& rows, std::vector<std::string>* notes) {
  std::string name = car.media_name;
  for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
  std::ifstream in(cars::mods::resolve(bake_dir / "cars" / (name + ".physics.txt")), std::ios::binary);
  CarParams p = params_from_bake(car);
  std::stringstream ss;
  ss << in.rdbuf();
  cars::CarPhysics stock;
  cars::PhysicsSettings settings;
  std::string error;
  if (!in || !cars::physics_from_text(ss.str(), stock, settings, &error)) {
    if (notes) notes->push_back("no physics settings for " + car.media_name + ": the synthetic car's physics");
    return p;
  }
  apply_physics(rows, settings, p, notes);
  return p;
}

CarParams params_for_car_text(const cars::bake::CarInfo& car, const std::string& text, std::vector<std::string>* notes) {
  CarParams p = params_from_bake(car);
  cars::CarPhysics rows;
  cars::PhysicsSettings settings;
  std::string error;
  if (!cars::physics_from_text(text, rows, settings, &error)) {
    if (notes) notes->push_back(error + ": the synthetic car's physics");
    return p;
  }
  apply_physics(rows, settings, p, notes);
  return p;
}

}  // namespace drive
