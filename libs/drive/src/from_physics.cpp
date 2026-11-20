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
  p.rear_fix_scale = get("Data_Car.FixListingRearFricScale", 1);
  p.fix_slip0 = get("Data_Car.FixListingNormSlip0", 0.5);
  p.fix_slip1 = get("Data_Car.FixListingNormSlip1", 1);
  p.fix_steer0 = get("Data_Car.FixListingSteerAngle0", 0.5);
  p.fix_steer1 = get("Data_Car.FixListingSteerAngle1", 1);

  // Engine. The torque chain's k is 1 (SPEC-CAR-PHYSICS: the stock boost
  // multiplier x GameTorqueScale is not near 1 on the R34, so k = 1).
  if (const auto* t = car.curve("torque"); t && t->size() >= 3) {
    p.torque_curve.assign(t->begin(), t->end());
    p.torque_curve_slots = double(t->size() - 1);
    if (p.torque_curve.back() < 0) p.torque_curve.pop_back();  // the negative sentinel
  } else if (notes) {
    notes->push_back("no torque curve: synthetic");
  }
  p.torque_curve_max_rpm = get("List_UpgradeEngineCamshaft.TorqueCurveMaxRPM", p.torque_curve_max_rpm);
  p.torque_scale = get("List_TorqueCurve.TorqueScale", p.torque_scale);
  p.engine_braking = get("List_TorqueCurve.ZeroThrottleTorqueScale", p.engine_braking);
  p.idle_rpm = get("List_UpgradeEngineCamshaft.StallRPM", p.idle_rpm);
  p.redline_rpm = get("List_UpgradeEngineCamshaft.RedlineRPM", p.redline_rpm);
  p.launch_rpm = get("List_UpgradeEngineCamshaft.StartRPM", p.launch_rpm);
  p.engine_inertia = get("Data_Engine.MomentInertia", 0.1) + get("List_UpgradeEngineFlywheel.MomentInertia", 0.1);

  // Drivetrain.
  const int type = int(std::lround(get("Data_Drivetrain.DrivetypeID", 2)));
  p.layout = type == 1 ? CarParams::Layout::kFront : type == 3 ? CarParams::Layout::kAll : CarParams::Layout::kRear;
  p.rear_split = get("List_UpgradeDrivetrainDifferential.RearToqueSplit", 0.5);
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

  // Brakes: the front torque is the spec's measured full-press figure; the
  // rear follows the bias (our reading: the rear share is BrakeBiasSlider
  // less BrakeBiasOffsetRear).
  p.front_brake_torque = 3500;
  const double rear_share =
      std::clamp(get("List_UpgradeBrakes.BrakeBiasSlider", 0.5) - s.get("BrakeBiasOffsetRear", 0.03), 0.05, 0.95);
  p.rear_brake_torque = p.front_brake_torque * rear_share / (1 - rear_share);
  p.abs_front_no_steer = get("List_UpgradeBrakes.ReleasePointABSFrontNoSteer", p.abs_front_no_steer);
  p.abs_front_full_steer = get("List_UpgradeBrakes.ReleasePointABSFrontFullSteer", p.abs_front_full_steer);
  p.abs_rear_no_steer = get("List_UpgradeBrakes.ReleasePointABSRearNoSteer", p.abs_rear_no_steer);
  p.abs_rear_full_steer = get("List_UpgradeBrakes.ReleasePointABSRearFullSteer", p.abs_rear_full_steer);
  p.abs_duration = get("List_UpgradeBrakes.DurationABS", p.abs_duration);

  // Input shaping and steering.
  p.throttle_rate = s.get("ControllerDelays\\AccelDelaySpeed", p.throttle_rate);
  p.brake_rate = s.get("ControllerDelays\\BrakeDelaySpeed", p.brake_rate);
  p.handbrake_rate = s.get("ControllerDelays\\HandBrakeDelaySpeed", p.handbrake_rate);
  p.max_steer = get("Data_Car.SteerMaxAngle", 35) * kDeg;
  p.steer_rate = get("Data_Car.SteerMaxAngVelTurning", 105) * kDeg;
  p.straighten_rate = get("Data_Car.SteerMaxAngVelStraighten", 105) * kDeg;
  p.steer_max_gees = get("Data_Car.SteerSpeedSensitiveMaxGees", 1);
  p.steer_min_lock = get("Data_Car.SteerSpeedSensitiveMinMaxAngle", 5) * kDeg;

  p.physics_hz = s.get("CarPhysicsUpdateHz", 359);
}

CarParams params_for_car_text(const cars::bake::CarInfo& car, const std::string& text, std::vector<std::string>* notes);

CarParams params_for_baked_car(const cars::bake::CarInfo& car, const std::filesystem::path& bake_dir,
                               std::vector<std::string>* notes) {
  std::string name = car.media_name;
  for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
  const auto path = bake_dir / "cars" / (name + ".physics.txt");
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (notes) notes->push_back("no " + path.string() + ": the synthetic car's physics (carbake --physics-only writes it)");
    return params_from_bake(car);
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return params_for_car_text(car, ss.str(), notes);
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
