#include "drive/vehicle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace drive {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kKgf = 9.80665;  // N per kilogram-force (the curves' load unit)
constexpr double kRpm = 60 / (2 * kPi);

double length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 normalized(const Vec3& a) {
  const double l = length(a);
  return l > 1e-12 ? a * (1.0 / l) : Vec3{};
}
double approach(double from, double to, double rate, double dt) {
  const double d = to - from, step = rate * dt;
  return std::fabs(d) <= step ? to : from + (d > 0 ? step : -step);
}
double lerp(double a, double b, double t) { return a + (b - a) * t; }
double ramp(double x, double x0, double x1) {  // 0 at x0, 1 at x1, clamped
  if (x1 == x0) return x >= x1 ? 1 : 0;
  return std::clamp((x - x0) / (x1 - x0), 0.0, 1.0);
}
double sign(double x) { return x > 0 ? 1 : x < 0 ? -1 : 0; }

}  // namespace

int substeps_for(double dt, double hz) {
  dt = std::min(dt, 0.1);
  // A hair under the product so that dt x hz landing on an integer by
  // rounding does not add a substep.
  return std::max(1, int(std::ceil(dt * hz - 1e-9)));
}

double FrictionCurve::peak_slip() const {
  if (values.size() < 2) return max_slip;
  size_t best = 0;
  for (size_t i = 1; i < values.size(); ++i)
    if (values[i] >= values[best]) best = i;
  return max_slip * double(best) / double(values.size() - 1);
}

double FrictionCurve::at(double slip, double stretch) const {
  if (values.empty()) return 0;
  if (values.size() == 1 || max_slip <= 0) return values.back();
  const double spacing = stretch * max_slip / double(values.size() - 1);
  const double x = std::fabs(slip) / spacing;
  const size_t last = values.size() - 1;
  if (x >= double(last)) return values[last];
  const size_t i = size_t(x);
  return lerp(values[i], values[i + 1], x - double(i));
}

double TyreCurves::blend(double load_kgf) const {
  return ramp(std::min(load_kgf, load_clamp_kgf), light_load_kgf, heavy_load_kgf);
}
double TyreCurves::peak_slip(double load_kgf, double stretch) const {
  return stretch * lerp(light.peak_slip(), heavy.peak_slip(), blend(load_kgf));
}
double TyreCurves::value(double slip, double load_kgf, double stretch, const std::array<double, 2>& scale) const {
  return lerp(light.at(slip, stretch) * scale[0], heavy.at(slip, stretch) * scale[1], blend(load_kgf));
}

FrictionCurve synthetic_curve(double max_slip, double peak, double tail) {
  FrictionCurve c;
  c.max_slip = max_slip;
  c.values.resize(100);
  for (size_t i = 0; i < c.values.size(); ++i) {
    const double x = max_slip * double(i) / 99.0;
    c.values[i] = x <= peak ? std::sin(0.5 * kPi * x / peak)
                            : lerp(1.0, tail, std::min(1.0, (x - peak) / (0.4 * max_slip)));
  }
  return c;
}

Vec3 rotate(const Quat& q, const Vec3& v) {
  const Vec3 u{q.x, q.y, q.z};
  const Vec3 t = cross(u, v) * 2.0;
  return v + t * q.w + cross(u, t);
}
Vec3 rotate_inverse(const Quat& q, const Vec3& v) { return rotate(Quat{-q.x, -q.y, -q.z, q.w}, v); }

bool FlatGround::raycast(const Vec3& origin, const Vec3& dir, double max_distance, Hit& hit) const {
  if (dir.y > -1e-9) return false;
  const double d = (height_ - origin.y) / dir.y;
  if (d < 0 || d > max_distance) return false;
  hit.distance = d;
  hit.normal = {0, 1, 0};
  hit.grip = 1;
  return true;
}

Vehicle::Vehicle(const CarParams& params) : p_(params) {
  com_local_ = p_.centre_of_mass;
  inertia_ = p_.inertia;
  if (inertia_.x <= 0 || inertia_.y <= 0 || inertia_.z <= 0) {
    // SPEC-CAR-PHYSICS: the mass as a solid box (X across, Y up, Z along).
    const double x = p_.inertia_box.x, y = p_.inertia_box.y, z = p_.inertia_box.z, m = p_.mass / 12.0;
    inertia_ = {m * (y * y + z * z), m * (x * x + z * z), m * (x * x + y * y)};
  }
  substeps_ = substeps_for(kStep, p_.physics_hz);
  // Static load per wheel from the centre of mass between the axles and
  // across each axle; the preloaded springs hold the car at the baked pose.
  const double z_front = (p_.wheel_centres[0].z + p_.wheel_centres[1].z) / 2;
  const double z_rear = (p_.wheel_centres[2].z + p_.wheel_centres[3].z) / 2;
  const double span = z_front - z_rear;
  const double front_share = std::fabs(span) > 1e-6 ? (com_local_.z - z_rear) / span : 0.5;
  for (int i = 0; i < kWheels; ++i) {
    const int a = axle_of(i);
    const double share = a == 0 ? front_share : 1 - front_share;
    const double xl = p_.wheel_centres[a == 0 ? 0 : 2].x, xr = p_.wheel_centres[a == 0 ? 1 : 3].x;
    const double side = std::fabs(xr - xl) > 1e-6 ? (i % 2 == 1 ? (com_local_.x - xl) / (xr - xl)
                                                                 : (xr - com_local_.x) / (xr - xl))
                                                   : 0.5;
    preload_[i] = p_.mass * p_.gravity * share * side;
    droop_[i] = std::min(p_.max_droop[a], preload_[i] / std::max(1.0, p_.spring_rate[a]));
  }
  for (int i = 0; i < kWheels; ++i) {
    const bool front = axle_of(i) == 0;
    switch (p_.layout) {
      case CarParams::Layout::kRear: weight_[i] = front ? 0 : 0.5; break;
      case CarParams::Layout::kFront: weight_[i] = front ? 0.5 : 0; break;
      case CarParams::Layout::kAll: weight_[i] = 0.5 * (front ? 1 - p_.rear_split : p_.rear_split); break;
    }
  }
  reset({0, 0, 0});
}

void Vehicle::place(const Vec3& position, double heading) {
  const double t = time_;
  reset(position, heading);
  time_ = t;
}

void Vehicle::launch(const Vec3& position, double heading, double speed, double pitch) {
  place(position, heading);
  const Quat yaw = rot_;
  // Nose up: a rotation about the body's +x by -pitch (z forward, y up).
  const Quat tilt{std::sin(-pitch / 2), 0, 0, std::cos(-pitch / 2)};
  rot_ = {yaw.w * tilt.x + yaw.x * tilt.w + yaw.y * tilt.z - yaw.z * tilt.y,
          yaw.w * tilt.y - yaw.x * tilt.z + yaw.y * tilt.w + yaw.z * tilt.x,
          yaw.w * tilt.z + yaw.x * tilt.y - yaw.y * tilt.x + yaw.z * tilt.w,
          yaw.w * tilt.w - yaw.x * tilt.x - yaw.y * tilt.y - yaw.z * tilt.z};
  pos_ = position + rotate(rot_, com_local_);
  vel_ = rotate(rot_, Vec3{0, 0, speed});
  for (int i = 0; i < kWheels; ++i) w_[i].omega = p_.tyre_radius[i] > 0 ? speed / p_.tyre_radius[i] : 0;
  // The highest gear that keeps the engine above the floor and below the
  // up-shift point (first gear when none does).
  int gear = 1;
  const double floor = std::max(p_.idle_rpm * 1.5, 0.35 * p_.redline_rpm);
  for (int g = 1; g <= int(p_.gears.size()); ++g) {
    const double rpm = speed / std::max(1e-3, p_.tyre_radius[2]) * ratio(g) * kRpm;
    if (rpm < p_.upshift_fraction * p_.redline_rpm && rpm >= floor) gear = g;
  }
  gear_ = target_gear_ = gear_before_ = gear;
  clutch_ = 1;
  locked_ = true;
  engine_omega_ = std::max(p_.idle_rpm / kRpm, speed / std::max(1e-3, p_.tyre_radius[2]) * ratio(gear));
  prev_rpm_ = engine_omega_ * kRpm;
}

void Vehicle::apply_impulse(const Vec3& point, const Vec3& impulse, double angular_scale) {
  vel_ = vel_ + impulse * (1 / p_.mass);
  const Vec3 tw = cross(point - pos_, impulse) * angular_scale;
  const Vec3 tl = rotate_inverse(rot_, tw);
  ang_ = ang_ + rotate(rot_, Vec3{tl.x / inertia_.x, tl.y / inertia_.y, tl.z / inertia_.z});
}

Vec3 Vehicle::point_velocity(const Vec3& point) const { return vel_ + cross(ang_, point - pos_); }

double Vehicle::inverse_mass_along(const Vec3& point, const Vec3& n, double angular_scale) const {
  const Vec3 rn = rotate_inverse(rot_, cross(point - pos_, n));
  const Vec3 w = rotate(rot_, Vec3{rn.x / inertia_.x, rn.y / inertia_.y, rn.z / inertia_.z});
  return 1 / p_.mass + angular_scale * dot(cross(w, point - pos_), n);
}

void Vehicle::reset(const Vec3& position, double heading) {
  rot_ = {0, std::sin(heading / 2), 0, std::cos(heading / 2)};
  pos_ = position + rotate(rot_, com_local_);  // pos_ holds the centre of mass
  vel_ = ang_ = {};
  gear_ = target_gear_ = gear_before_ = 1;
  state_ = GearState::kOpen;
  state_time_ = 0;
  clutch_ = 0;
  locked_ = rev_matching_ = false;
  launch_target_ = 0;
  engine_omega_ = p_.idle_rpm / kRpm;
  prev_rpm_ = p_.idle_rpm;
  spool_ = engine_throttle_ = driver_throttle_ = 0;
  tc_prev_ = 0;
  slip_pos_ = 0;
  time_ = 0;
  steer_angle_ = 0;
  abs_flag_ = false;
  raw_ = shaped_ = {};
  for (int i = 0; i < kWheels; ++i) {
    w_[i] = {};
    w_[i].centre = p_.wheel_centres[i];
    w_[i].contact = true;
    prev_compression_[i] = 0;
    abs_timer_[i] = 0;
    for (double& h : slip_hist_[i]) h = 0;
  }
}

Vec3 Vehicle::position() const { return pos_ - rotate(rot_, com_local_); }

double Vehicle::speed() const { return dot(vel_, rotate(rot_, {0, 0, 1})); }

double Vehicle::rpm() const { return engine_omega_ * kRpm; }

double Vehicle::ratio(int gear) const {
  if (gear >= 1 && size_t(gear) <= p_.gears.size()) return p_.gears[size_t(gear - 1)] * p_.final_drive;
  if (gear == -1) return -p_.reverse_gear * p_.final_drive;
  return 0;
}

double Vehicle::clutch_output_rpm() const { return ratio(gear_) * input_omega() * kRpm; }

double Vehicle::input_omega() const {
  double o = 0;
  for (int i = 0; i < kWheels; ++i) o += weight_[i] * w_[i].omega;
  return o;
}

void Vehicle::shape_input(const Input& raw, double dt) {
  // SPEC-CAR-PHYSICS, Throttle, brake and steering from the pad: the
  // pedals move towards the input at their ControllerDelays rates; the
  // road-wheel angle moves towards stick x lock at SteerMaxAngVelTurning.
  if (ai_) {
    // The AI's controls skip the shaping: the road wheels at steer x
    // SteerMaxAngle at once.
    shaped_.throttle = std::clamp(raw.throttle, 0.0, 1.0);
    shaped_.brake = std::clamp(raw.brake, 0.0, 1.0);
    shaped_.steer = std::clamp(raw.steer, -1.0, 1.0);
    shaped_.handbrake = raw.handbrake;
    steer_angle_ = shaped_.steer * p_.max_steer;
    return;
  }
  shaped_.throttle = approach(shaped_.throttle, std::clamp(raw.throttle, 0.0, 1.0), p_.throttle_rate, dt);
  shaped_.brake = approach(shaped_.brake, std::clamp(raw.brake, 0.0, 1.0), p_.brake_rate, dt);
  shaped_.steer = std::clamp(raw.steer, -1.0, 1.0);  // the traced steer control steps to the stick
  shaped_.handbrake = raw.handbrake;
  // The lock narrows with speed to hold steer_max_gees of kinematic lateral
  // acceleration (ours, after SteerSpeedSensitiveMaxGees), never below
  // steer_min_lock. At walking pace it is the full SteerMaxAngle.
  const double lock = steer_lock();
  const double target = shaped_.steer * lock;
  const bool centring = std::fabs(target) < std::fabs(steer_angle_) && target * steer_angle_ >= 0;
  // Turning slows with the narrowed lock (placeholder); centring does not
  // (140 to 190 deg/s measured at speed).
  const double share = p_.max_steer > 0 ? std::clamp(lock / p_.max_steer, p_.steer_rate_min_share, 1.0) : 1.0;
  steer_angle_ = approach(steer_angle_, target, centring ? p_.straighten_rate : p_.steer_rate * share, dt);
}

double Vehicle::steer_lock() const {
  const double wheelbase = std::fabs((p_.wheel_centres[0].z + p_.wheel_centres[1].z) / 2 -
                                     (p_.wheel_centres[2].z + p_.wheel_centres[3].z) / 2);
  const double v = std::fabs(speed());
  if (ai_ || v <= 0.1) return p_.max_steer;
  return std::clamp(std::atan(p_.steer_gees_fit * p_.steer_max_gees * p_.gravity * wheelbase / (v * v)),
                    std::min(p_.steer_min_lock, p_.max_steer), p_.max_steer);
}

double Vehicle::engine_torque(double rpm, double throttle) const {
  // Below idle the engine opens its own throttle to hold idle (ours).
  throttle = std::max(throttle, ramp(p_.idle_rpm - rpm, 0, 200));
  double full;
  if (p_.torque_curve.empty()) {
    // The synthetic car's curve: rising from idle, flat 2,000-5,500 rpm,
    // falling to the redline.
    const double r = std::max(rpm, 0.0);
    full = r < 2000 ? 0.6 + 0.4 * r / 2000 : r > 5500 ? 1 - 0.3 * ramp(r, 5500, p_.redline_rpm) : 1.0;
  } else {
    // Sample i at i x TorqueCurveMaxRPM / (NumTorqueValues - 1).
    const double slots = p_.torque_curve_slots > 0 ? p_.torque_curve_slots : double(p_.torque_curve.size() - 1);
    const double x = std::max(rpm, 0.0) / (p_.torque_curve_max_rpm / slots);
    const size_t last = p_.torque_curve.size() - 1;
    if (x >= double(last)) full = p_.torque_curve[last];
    else {
      const size_t i = size_t(x);
      full = lerp(p_.torque_curve[i], p_.torque_curve[i + 1], x - double(i));
    }
  }
  full *= p_.torque_scale * torque_factor_;
  const double closed = -p_.engine_braking * ramp(rpm, p_.idle_rpm, p_.redline_rpm);
  double t = throttle * full + (1 - throttle) * closed;
  if (rpm >= p_.redline_rpm) t = std::min(t, closed);  // the limiter cuts the fuel
  return t;
}

void Vehicle::update_gearbox(double dt) {
  // SPEC-CAR-PHYSICS, Clutch and gearbox states. clutch_ is the engagement
  // (the spec's c is 1 - clutch_). In reverse the pedals swap: the brake
  // pedal is the throttle and the throttle pedal the brake.
  const double v = speed();
  const bool reverse = gear_ == -1;
  const double drive_pedal = reverse ? shaped_.brake : shaped_.throttle;
  const double brake_pedal = reverse ? shaped_.throttle : shaped_.brake;
  // The effective throttle builds like boost (ours, see CarParams::boost_lag).
  const double trigger = std::clamp(reverse ? raw_.brake : raw_.throttle, 0.0, 1.0);
  if (p_.boost_lag > 0 && trigger > spool_) spool_ += (trigger - spool_) * (1 - std::exp(-dt / p_.boost_lag));
  else spool_ = trigger;
  const double driver = std::min(drive_pedal, spool_);
  driver_throttle_ = driver;
  const double rpm_now = rpm();
  const double rpm_rate = (rpm_now - prev_rpm_) / dt;
  prev_rpm_ = rpm_now;
  const double idle = p_.idle_rpm, start = p_.launch_rpm, red = p_.redline_rpm;
  auto out_rpm = [&] { return ratio(gear_) * input_omega() * kRpm; };  // the clutch output, signed
  state_time_ += dt;
  auto enter = [&](GearState s) {
    state_ = s;
    state_time_ = 0;
  };
  auto open = [&] {
    enter(GearState::kOpen);
    clutch_ = 0;
  };
  auto start_shift = [&](int target) {
    gear_before_ = gear_;
    target_gear_ = target;
    enter(GearState::kClutchIn);
  };
  // The gearbox's own throttle in a rev-match: towards the clutch output's
  // speed in the new gear (ours: proportional over 200 rpm).
  auto rev_request = [&] { return std::clamp((std::fabs(out_rpm()) - rpm_now) / 200, 0.0, 1.0); };
  const bool in_shift = state_ == GearState::kClutchIn || state_ == GearState::kGearChange ||
                        state_ == GearState::kRevMatch || state_ == GearState::kClutchOut;

  // Gear selection outside a shift: reverse with the brake held below
  // MaxForwardVelShiftReverseMPH; forward again from reverse the same way.
  if (!in_shift) {
    if (gear_ >= 1 && shaped_.brake > 0.5 && shaped_.throttle < 0.1 && v < p_.reverse_select_speed) {
      gear_before_ = gear_;
      gear_ = target_gear_ = -1;
      open();
    } else if (gear_ == -1 && shaped_.throttle > 0.5 && shaped_.brake < 0.1 && v > -p_.reverse_select_speed) {
      gear_before_ = gear_;
      gear_ = target_gear_ = 1;
      open();
    } else if (gear_ == 0) {
      gear_ = target_gear_ = 1;
      open();
    }
  }

  switch (state_) {
    case GearState::kOpen: {
      clutch_ = 0;
      engine_throttle_ = driver;
      if (drive_pedal > 0.1 && brake_pedal < 0.1) {
        const double s0 = idle + 0.2 * (start - idle);
        if (out_rpm() > p_.launch_max_output_rpm) {
          gear_before_ = target_gear_ = gear_;  // rolling: match the engine, then close
          rev_matching_ = true;
          enter(GearState::kRevMatch);
        } else if (rpm_now > lerp(s0, start, drive_pedal)) {
          enter(GearState::kLaunch);
        }
      }
      break;
    }
    case GearState::kLaunch: {
      engine_throttle_ = driver;
      if (brake_pedal > 0.1) {
        open();
        break;
      }
      const double tau = std::min(1.0, state_time_ / p_.launch_fall_time);
      const double high = lerp(start + 0.2 * (red - start), idle + 0.3 * (start - idle), tau);
      const double low = lerp(idle + 0.2 * (start - idle), idle + 0.01 * (start - idle), tau);
      const double target = lerp(low, high, drive_pedal);
      launch_target_ = target;
      // dc/dt = gain x (target - (rpm + lookahead x rpm rate)) / redline.
      const double c_rate = p_.launch_gain * (target - (rpm_now + p_.launch_lookahead * rpm_rate)) / red;
      clutch_ = std::clamp(clutch_ - c_rate * dt, 0.0, 1.0);
      double slip = 0, n = 0;
      for (int i = 0; i < kWheels; ++i)
        if (weight_[i] > 0) slip += std::fabs(w_[i].slip_ratio), n += 1;
      if (n > 0) slip /= n;
      if (out_rpm() > target && slip < p_.launch_exit_slip) {
        rev_matching_ = false;
        enter(GearState::kClutchOut);
      }
      break;
    }
    case GearState::kEngaged: {
      engine_throttle_ = driver;
      clutch_ = approach(clutch_, 1, 1 / p_.clutch_out_time, dt);
      const double road = std::fabs(out_rpm());
      if (road < 0.9 * idle) {
        open();  // the engine would be dragged under idle (ours)
        break;
      }
      // Automatic shifts (SPEC-CAR-PHYSICS, Gears): up at upshift_fraction x
      // the redline; down under braking, or when the engine lugs (ours).
      // Up at part throttle (ours: the spec traced full throttle only): the
      // point falls with the pedal to half the redline, if the next gear
      // keeps the engine over 0.4 x the redline (the lug rule's 0.35 below).
      const int top = int(p_.gears.size());
      const double up_at = lerp(0.5, p_.upshift_fraction, std::clamp(drive_pedal, 0.0, 1.0)) * red;
      const double next_rpm = gear_ >= 1 && gear_ < top ? road * ratio(gear_ + 1) / ratio(gear_) : 0;
      // The part-throttle rule is the pad's: the AI's lap shows no early
      // shift on a lift (7,100 rpm in third at gas 0.3).
      const bool part = !ai_ && rpm_now >= up_at && next_rpm >= 0.4 * red;
      if (gear_ >= 1 && gear_ < top && locked_ && (rpm_now >= p_.upshift_fraction * red || part)) {
        start_shift(gear_ + 1);
      } else if (gear_ > 1) {
        const double lower = road * ratio(gear_ - 1) / ratio(gear_);
        if ((brake_pedal > 0.1 && lower < p_.downshift_brake_fraction * red) ||
            (drive_pedal > 0.1 && road < 0.35 * red && lower < 0.75 * red))
          start_shift(gear_ - 1);
      }
      break;
    }
    case GearState::kClutchIn:
      engine_throttle_ = 0;  // the power cut
      clutch_ = std::max(0.0, clutch_ - dt / p_.clutch_in_time);
      if (clutch_ <= 0) enter(GearState::kGearChange);
      break;
    case GearState::kGearChange:
      engine_throttle_ = 0;
      clutch_ = 0;
      if (state_time_ >= p_.gear_shift_time - 1e-9) {
        gear_ = target_gear_;  // the gear number changes at the end
        rev_matching_ = true;
        enter(GearState::kRevMatch);
      }
      break;
    case GearState::kRevMatch:
      clutch_ = 0;
      engine_throttle_ = rev_request();
      if (state_time_ >= p_.rev_match_time - 1e-9) enter(GearState::kClutchOut);
      break;
    case GearState::kClutchOut: {
      clutch_ = std::min(1.0, clutch_ + dt / p_.clutch_out_time);
      // Power comes back as the clutch closes: lerp(driver, the box's
      // request, c). Out of a launch the box asks for nothing of its own.
      const double request = rev_matching_ ? rev_request() : driver;
      engine_throttle_ = lerp(driver, request, 1 - clutch_);
      if (clutch_ >= 1) enter(GearState::kEngaged);
      break;
    }
  }
}

void Vehicle::step(const Input& input, const Ground& ground) {
  raw_ = input;
  shape_input(input, kStep);
  update_gearbox(kStep);
  abs_flag_ = false;
  strongest_ = {};
  const double dt = kStep / substeps_;
  for (int s = 0; s < substeps_; ++s) substep(dt, ground);
  time_ += kStep;
}

void Vehicle::substep(double dt, const Ground& ground) {
  const Vec3 up = rotate(rot_, {0, 1, 0});
  const Vec3 down = up * -1.0;
  const CarParams& p = p_;
  Vec3 force{};
  Vec3 torque{};

  // 1. Suspension and contact.
  Vec3 contact[kWheels]{}, normal[kWheels]{};
  double grip[kWheels]{}, rate[kWheels]{};
  SurfaceTyre surf[kWheels]{};
  int off = 0;
  for (int i = 0; i < kWheels; ++i) {
    WheelState& w = w_[i];
    const int a = axle_of(i);
    const double radius = p.tyre_radius[i];
    const Vec3 hard_local{p.wheel_centres[i].x, p.wheel_centres[i].y + p.rest_length, p.wheel_centres[i].z};
    const Vec3 hard = pos_ + rotate(rot_, hard_local - com_local_);
    // Ackermann (ours: the spec has no steering geometry): each front wheel
    // points at one turn centre on the rear axle's line, steer_angle_ being
    // the angle of a wheel on the centre line.
    w.steer = 0;
    if (a == 0 && std::fabs(steer_angle_) > 1e-9) {
      const double z_rear = 0.5 * (p.wheel_centres[2].z + p.wheel_centres[3].z);
      const double base = p.wheel_centres[i].z - z_rear;
      const double centre_x = base / std::tan(steer_angle_);
      w.steer = std::atan(base / (centre_x - p.wheel_centres[i].x));
      if (w.steer * steer_angle_ < 0) w.steer += steer_angle_ > 0 ? kPi : -kPi;  // past 90 degrees
    }
    Hit hit;
    const double reach = p.probe_height + p.rest_length + droop_[i] + radius;
    const Vec3 probe = hard - down * p.probe_height;
    double compression = -droop_[i];
    w.contact = up.y > 0.2 && ground.raycast(probe, down, reach, hit);
    w.surface = w.contact ? hit.surface : -1;
    if (w.contact) {
      compression = p.rest_length - (hit.distance - p.probe_height - radius);
      if (compression < -droop_[i]) w.contact = false, compression = -droop_[i];
    }
    if (w.contact) {
      normal[i] = hit.normal;
      grip[i] = hit.grip;
      surf[i] = hit.tyre;
      contact[i] = probe + down * hit.distance;
    }
    // The suspension's speed from the travel itself, so a car running down
    // a grade does not read the fall as rebound.
    rate[i] = (compression - prev_compression_[i]) / dt;
    prev_compression_[i] = compression;
    w.compression = compression;
    off += !w.contact;
    double load = 0;
    if (w.contact) {
      double damper = rate[i] * (rate[i] > 0 ? p.bump_rate[a] : p.rebound_rate[a]);
      damper = std::clamp(damper, -p.damper_clamp[a], p.damper_clamp[a]);
      load = preload_[i] + p.spring_rate[a] * compression + damper;
      if (compression > p.bump_travel[a])
        load += p.bumpstop_rate[a] * (compression - p.bump_travel[a]) + p.bumpstop_damping[a] * std::max(0.0, rate[i]);
    }
    w.load = load;
    w.centre = {p.wheel_centres[i].x, p.wheel_centres[i].y + std::min(compression, p.bump_travel[a] + 0.05),
                p.wheel_centres[i].z};
  }
  // Anti-roll bars: equal and opposite on an axle's two wheels.
  for (int a = 0; a < 2; ++a) {
    const int l = 2 * a, r = 2 * a + 1;
    if (!w_[l].contact || !w_[r].contact) continue;
    const double f = p.antiroll_rate[a] * (w_[l].compression - w_[r].compression) +
                     p.antiroll_damping[a] * (rate[l] - rate[r]);
    w_[l].load += f;
    w_[r].load -= f;
  }
  for (auto& w : w_) w.load = std::max(0.0, w.load);

  const double g_scale = off == kWheels ? p.airborne_gravity_scale : std::pow(p.per_tyre_off_gravity_scale, off);
  force.y -= p.mass * p.gravity * g_scale;

  // 2. Brakes with the ABS (SPEC-CAR-PHYSICS, ABS). In reverse the pedals
  // swap: the throttle pedal brakes. The engine's throttle is the gearbox's
  // (update_gearbox).
  const double drive_in = engine_throttle_;
  const double brake_in = gear_ == -1 ? shaped_.throttle : shaped_.brake;
  // BrakeInputScale, normalised so a full press is the measured torque.
  static constexpr double kBrakeIn[4] = {0, 0.001, 0.90, 1}, kBrakeOut[4] = {0, 0.12, 0.70, 0.81};
  double brake_frac = 0;
  for (int k = 0; k < 3; ++k)
    if (brake_in <= kBrakeIn[k + 1] || k == 2) {
      brake_frac = lerp(kBrakeOut[k], kBrakeOut[k + 1], ramp(brake_in, kBrakeIn[k], kBrakeIn[k + 1])) / kBrakeOut[3];
      break;
    }
  if (brake_in <= 0) brake_frac = 0;
  // The AI's brake is linear, ai_brake_scale x the full press per unit.
  if (ai_) brake_frac = brake_in * p.ai_brake_scale;
  const double stick = std::fabs(shaped_.steer);
  // A wheel whose smoothed normalised slip ratio (the mean of its last
  // abs_smoothing substeps) passes the release point, blended by the
  // steering input, gets no brake for DurationABS, then the full brake.
  const double G = ratio(gear_);
  double brake[kWheels]{};
  bool slowing[kWheels]{};
  const int hist_n = std::clamp(p.abs_smoothing, 1, 8);
  for (int i = 0; i < kWheels; ++i) {
    WheelState& w = w_[i];
    const int a = axle_of(i);
    double frac = brake_frac;
    if (p.abs && frac > 0) {
      if (abs_timer_[i] <= 0 && w.contact) {
        double mean = 0;
        for (int k = 0; k < hist_n; ++k) mean += slip_hist_[i][k];
        mean /= hist_n;
        const double release = ai_ ? p.abs_ai_release
                               : a == 0 ? lerp(p.abs_front_no_steer, p.abs_front_full_steer, stick)
                                        : lerp(p.abs_rear_no_steer, p.abs_rear_full_steer, stick);
        if ((ai_ ? w.slip_ratio : mean) < -release) abs_timer_[i] = ai_ ? kStep : p.abs_duration;
      }
      if (abs_timer_[i] > 0) {
        frac = 0;
        abs_flag_ = true;
      }
    }
    if (abs_timer_[i] > 0) abs_timer_[i] -= dt;
    w.brake = frac;
    brake[i] = frac * (a == 0 ? p.front_brake_torque : p.rear_brake_torque);
    if (a == 1 && shaped_.handbrake) brake[i] += p.handbrake_torque;
    // The wheel's applied torque slows it: the brake pedal (held through
    // an ABS release: the oracle's released front keeps F_x / load 1.76,
    // SPEC-CAR-PHYSICS scenario B onset table), the handbrake, or the
    // driveline's torque (last substep's) against its spin.
    slowing[i] = brake_frac > 0 || brake[i] > 0 || tc_prev_ * G * weight_[i] * w.omega < 0;
  }

  // 3. Tyres: forces at the current wheel speeds, with the longitudinal
  // force's slope against wheel speed for a linearised-implicit spin update.

  Vec3 fwd[kWheels]{}, lat[kWheels]{};
  double vx[kWheels]{}, vy[kWheels]{}, fx0[kWheels]{}, fy[kWheels]{}, slope[kWheels]{};
  auto tyre = [&](int i, double omega, double* fx_out, double* fy_out, double* sx_out, double* sy_out) {
    const int a = axle_of(i);
    const double n = w_[i].load, kgf = n / kKgf, stretch = p.sidewall_stretch[a];
    const double sig_pk = std::max(1e-4, p.longitudinal.peak_slip(kgf, stretch));
    const double alpha_pk = std::clamp(p.lateral.peak_slip(kgf, stretch), p.lateral_peak_min_deg, p.lateral_peak_max_deg);
    // An off-road surface moves the normalising peak towards its own; the
    // curve is still read at the road's (SPEC-CAR-PHYSICS, Off-road surfaces).
    const double alpha_norm = alpha_pk + surf[i].off_roadness * (surf[i].off_road_peak_sa - alpha_pk);
    const double denom = std::max(std::fabs(vx[i]), p.slip_speed_floor);
    const double sigma = (omega * p.tyre_radius[i] - vx[i]) / denom;
    // Below the floor speed the angle becomes the linear term v_y / v_min.
    const double alpha = std::atan2(vy[i], denom) * 180 / kPi;
    const double sx = sigma / sig_pk, sy = alpha / std::max(alpha_norm, 1e-3);
    const double s = std::max(std::sqrt(sx * sx + sy * sy), 1e-4);
    // Each curve times its row's FrictionScale, blended by load, times the
    // width scale.
    const double mu_x = p.longitudinal.value(s * sig_pk, kgf, stretch, p.long_curve_scale) * p.width_scale[a];
    const double mu_y = p.lateral.value(s * alpha_pk, kgf, stretch, p.lat_curve_scale) * p.width_scale[a];
    const double rear = a == 1 ? surf[i].rear_grip : 1.0;
    const double ai = ai_grip_[a];
    // Braking friction on a slowed wheel, else the speed-blended accel
    // scale (SPEC-CAR-PHYSICS, Brakes: Braking friction).
    double kx = grip[i] * rear * ai * p.long_scale[a] * p.long_misc_scale;
    kx *= slowing[i] ? p.brake_friction_scale
                     : lerp(p.accel_scale0, p.accel_scale1, ramp(std::fabs(vx[i]), p.accel_speed0, p.accel_speed1));
    double ky = grip[i] * rear * ai * p.lat_scale[a] * p.lat_compound_scale * p.long_misc_scale;
    // The rear-grip fix: g + d (1 - g), g the scale faded out by the
    // normalised slip angle, d by the road wheels' steer angle in degrees.
    if (a == 1) {
      const double g = lerp(p.rear_fix_scale, 1.0, ramp(std::fabs(sy), p.fix_slip0, p.fix_slip1));
      const double d = ramp(std::fabs(steer_angle_) * 180 / kPi, p.fix_steer0, p.fix_steer1);
      ky *= g + d * (1 - g);
    }
    if (fx_out) *fx_out = mu_x * (sx / s) * n * kx;
    if (fy_out) *fy_out = -mu_y * (sy / s) * n * ky;
    if (sx_out) *sx_out = sx;
    if (sy_out) *sy_out = sy;
  };
  for (int i = 0; i < kWheels; ++i) {
    WheelState& w = w_[i];
    if (!w.contact || w.load <= 0) {
      w.slip_ratio = w.slip_angle = 0;
      continue;
    }
    const Vec3 heading_local{std::sin(w.steer), 0, std::cos(w.steer)};
    Vec3 f = rotate(rot_, heading_local);
    fwd[i] = normalized(f - normal[i] * dot(f, normal[i]));
    lat[i] = normalized(cross(normal[i], fwd[i]));  // +x on flat ground with fwd +z
    const Vec3 v_contact = vel_ + cross(ang_, contact[i] - pos_);
    vx[i] = dot(v_contact, fwd[i]);
    vy[i] = dot(v_contact, lat[i]);
    tyre(i, w.omega, &fx0[i], &fy[i], &w.slip_ratio, &w.slip_angle);
    const double d_omega = 1e-3 * std::max(1.0, std::fabs(w.omega));
    double fx1 = 0;
    tyre(i, w.omega + d_omega, &fx1, nullptr, nullptr, nullptr);
    slope[i] = std::max(0.0, (fx1 - fx0[i]) / d_omega);
  }
  for (int i = 0; i < kWheels; ++i) slip_hist_[i][slip_pos_] = w_[i].slip_ratio;
  slip_pos_ = (slip_pos_ + 1) % hist_n;

  // 4. Drivetrain: the engine, the clutch and the wheels. A locked clutch's
  // torque is solved so the engine and the diff-weighted wheel speed stay
  // together; past its capacity it slips at the capacity.
  const double cap = G != 0 ? p.clutch_max_torque * p.clutch_torque_factor * clutch_ : 0;
  const double je = p.engine_inertia;
  const double te = engine_torque(rpm(), drive_in);
  const double reflected = G != 0 ? p.gearbox_inertia * G * G + p.driveline_inertia * p.final_drive * p.final_drive : 0;
  double j_eff[kWheels]{}, resist[kWheels]{};
  double num = te / je, den = 1 / je;
  for (int i = 0; i < kWheels; ++i) {
    // The tyre's torque back on the wheel is F_x x r (SPEC-CAR-PHYSICS).
    j_eff[i] = p.wheel_inertia + reflected * weight_[i] + dt * p.tyre_radius[i] * slope[i];
    resist[i] = p.tyre_radius[i] * fx0[i] + (std::fabs(w_[i].omega) > 0.1 ? sign(w_[i].omega) * brake[i] : 0);
  }
  // A limited-slip centre (AWD): it moves torque from the faster axle to
  // the slower, up to its locking torque (drive side while the clutch
  // passes engine torque forward, else the decel side), and no more than
  // would bring the two axles' speeds together this substep (ours: a
  // clutch-type lock; the columns' RelVelClamp is not decoded).
  double lsd[kWheels]{};
  if (p.layout == CarParams::Layout::kAll && G != 0 && clutch_ > 0) {
    const double lock = (tc_prev_ >= 0 ? p.centre_lsd_accel : p.centre_lsd_decel) * p.final_drive;
    if (lock > 0) {
      const double d_omega = 0.5 * (w_[2].omega + w_[3].omega) - 0.5 * (w_[0].omega + w_[1].omega);
      const double jf = j_eff[0] + j_eff[1], jr = j_eff[2] + j_eff[3];
      const double t = std::clamp(d_omega / (dt * (1 / jf + 1 / jr)), -lock, lock);  // to the front axle
      lsd[0] = lsd[1] = 0.5 * t;
      lsd[2] = lsd[3] = -0.5 * t;
    }
  }
  for (int i = 0; i < kWheels; ++i) {
    resist[i] -= lsd[i];
    num += G * weight_[i] * resist[i] / j_eff[i];
    den += G * G * weight_[i] * weight_[i] / j_eff[i];
  }
  const double t_lock = num / den;
  const double slip = engine_omega_ - G * input_omega();
  double tc = 0;
  bool lock_now = false;
  if (cap > 0) {
    if ((locked_ || std::fabs(slip) < 0.5) && std::fabs(t_lock) <= cap) {
      tc = t_lock;
      lock_now = true;
    } else {
      tc = (slip != 0 ? sign(slip) : sign(t_lock)) * cap;
    }
  }
  double omega_e = engine_omega_ + dt * (te - tc) / je;
  double omega_new[kWheels];
  for (int i = 0; i < kWheels; ++i) {
    WheelState& w = w_[i];
    double o = w.omega + dt * (tc * G * weight_[i] + lsd[i] - p.tyre_radius[i] * fx0[i]) / j_eff[i];
    const double b = brake[i] * dt / j_eff[i];
    o = std::fabs(o) <= b ? 0 : o - sign(o) * b;
    if (!w.contact) o *= 1 - 0.5 * dt;  // bearing drag in the air
    omega_new[i] = o;
  }
  double in_new = 0;
  for (int i = 0; i < kWheels; ++i) in_new += weight_[i] * omega_new[i];
  if (cap > 0 && !lock_now && sign(omega_e - G * in_new) != sign(slip)) lock_now = true;  // synchronised
  if (lock_now) omega_e = G * in_new;
  if (lock_now && omega_e < 0.3 * p.idle_rpm / kRpm) {
    lock_now = false;  // the engine would stall: the clutch lets go of it (ours)
    omega_e = std::max(omega_e, engine_omega_ + dt * te / je);
  }
  locked_ = lock_now;
  tc_prev_ = tc;
  engine_omega_ = std::max(0.0, omega_e);

  // 5. Forces on the body.
  for (int i = 0; i < kWheels; ++i) {
    WheelState& w = w_[i];
    if (w.contact && w.load > 0) {
      // The tyre at the new wheel speed: the linearisation only steadies
      // the spin update; past the curve's linear part it would overshoot.
      double fx = 0, fy_new = 0;
      tyre(i, omega_new[i], &fx, &fy_new, nullptr, nullptr);
      const Vec3 f = normal[i] * w.load + fwd[i] * fx + lat[i] * fy_new;
      w.fx = fx;
      force = force + f;
      torque = torque + cross(contact[i] - pos_, f);
      // The longitudinal force pitches the body through the measured arm
      // instead of the contact patch's height below the centre of mass
      // (SPEC-CAR-PHYSICS scenario B, load transfer): the same force at a
      // point raised along the body's up axis.
      // The lateral force rolls it through its own arm likewise.
      const double below = dot(pos_ - contact[i], up);
      if (p.longitudinal_pitch_arm > 0 && below > p.longitudinal_pitch_arm)
        torque = torque + cross(up * (below - p.longitudinal_pitch_arm), fwd[i] * fx);
      if (p.lateral_roll_arm > 0 && below > p.lateral_roll_arm)
        torque = torque + cross(up * (below - p.lateral_roll_arm), lat[i] * fy_new);
    }
    if (!w.contact || w.load <= 0) w.fx = 0;
    w.omega = omega_new[i];
    w.spin = std::fmod(w.spin + w.omega * dt, 2 * kPi);
  }

  // Body contacts: 26 points on the box (corners, edge and face middles),
  // each probed by a ray from the box's centre. A point past the first
  // surface along its ray is pushed out along that surface's normal, so
  // floors, walls, barriers and ceilings all act on the body. The force per
  // point is scaled so that the eight corners carry what they did when they
  // were the only contacts.
  body_force_ = 0;
  constexpr int kPoints = 26;
  constexpr double kShare = 8.0 / kPoints;
  const Vec3 centre_local{0, p.sill_height + 0.5 * p.size.y, p.centre_of_mass.z};
  const Vec3 centre = pos_ + rotate(rot_, centre_local - com_local_);
  for (int k = 0; k < 27; ++k) {
    const int ix = k % 3 - 1, iy = k / 3 % 3 - 1, iz = k / 9 - 1;
    if (ix == 0 && iy == 0 && iz == 0) continue;
    const Vec3 point_local = centre_local + Vec3{0.5 * ix * p.size.x, 0.5 * iy * p.size.y, 0.5 * iz * p.size.z};
    const Vec3 point = pos_ + rotate(rot_, point_local - com_local_);
    const Vec3 ray = point - centre;
    const double reach = length(ray);
    const Vec3 dir = ray * (1 / reach);
    Hit hit;
    if (!ground.raycast(centre, dir, reach, hit)) continue;
    // The normal faces against the ray: out of the surface, towards the box.
    const Vec3 n = dot(hit.normal, dir) > 0 ? hit.normal * -1.0 : hit.normal;
    const double depth = (reach - hit.distance) * -dot(dir, n);
    const Vec3 v = vel_ + cross(ang_, point - pos_);
    const double vn = dot(v, n);
    const double fn = kShare * std::max(0.0, p.mass * (p.body_stiffness * depth - p.body_damping * vn));
    if (fn <= 0) continue;
    Vec3 f = n * fn;
    const Vec3 vt = v - n * vn;
    const double vt_len = length(vt);
    if (vt_len > 1e-6) {
      const double ft = std::min(p.body_friction * fn, p.mass / kPoints * vt_len / dt);
      f = f - vt * (ft / vt_len);
    }
    force = force + f;
    torque = torque + cross(point - pos_, f);
    body_force_ += length(f);
    if (fn > strongest_.force) strongest_ = {centre + dir * hit.distance, n, fn, hit.surface, hit.triangle};
  }

  // Resistance (SPEC-CAR-PHYSICS, Aerodynamics: the measured fit, rolling
  // resistance included).
  const double v_abs = length(vel_);
  if (v_abs > 1e-6) {
    const double r = p.drag_quadratic * v_abs * v_abs + p.drag_constant * std::min(1.0, v_abs);
    force = force - vel_ * (r / v_abs);
  }

  // Integrate (semi-implicit Euler).
  vel_ = vel_ + force * (dt / p.mass);
  const Vec3 tl = rotate_inverse(rot_, torque);
  Vec3 wl = rotate_inverse(rot_, ang_);
  const Vec3 iw{inertia_.x * wl.x, inertia_.y * wl.y, inertia_.z * wl.z};
  const Vec3 gyro = cross(wl, iw);
  wl = wl + Vec3{(tl.x - gyro.x) / inertia_.x, (tl.y - gyro.y) / inertia_.y, (tl.z - gyro.z) / inertia_.z} * dt;
  ang_ = rotate(rot_, wl);
  pos_ = pos_ + vel_ * dt;
  const Quat dq{ang_.x, ang_.y, ang_.z, 0};
  Quat q{rot_.x + 0.5 * dt * (dq.w * rot_.x + dq.x * rot_.w + dq.y * rot_.z - dq.z * rot_.y),
         rot_.y + 0.5 * dt * (dq.w * rot_.y - dq.x * rot_.z + dq.y * rot_.w + dq.z * rot_.x),
         rot_.z + 0.5 * dt * (dq.w * rot_.z + dq.x * rot_.y - dq.y * rot_.x + dq.z * rot_.w),
         rot_.w + 0.5 * dt * (dq.w * rot_.w - dq.x * rot_.x - dq.y * rot_.y - dq.z * rot_.z)};
  const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  rot_ = {q.x / n, q.y / n, q.z / n, q.w / n};
}

std::string trace_header() {
  std::string h = "t,x,y,z,vx,vy,vz,speed,yaw,pitch,roll,rpm,gear,steer,throttle,brake,body,clutch,abs";
  for (int i = 0; i < kWheels; ++i) {
    const std::string n = std::to_string(i);
    h += ",comp" + n + ",omega" + n + ",load" + n + ",sx" + n + ",sy" + n + ",brk" + n + ",fx" + n;
  }
  return h;
}

std::string trace_row(const Vehicle& v) {
  char buf[128];
  std::string row;
  auto add = [&](double x) {
    std::snprintf(buf, sizeof buf, "%s%.9g", row.empty() ? "" : ",", x);
    row += buf;
  };
  const Vec3 p = v.position(), vel = v.velocity();
  const Vec3 fwd = rotate(v.orientation(), {0, 0, 1}), up = rotate(v.orientation(), {0, 1, 0});
  const Vec3 right = rotate(v.orientation(), {1, 0, 0});
  add(v.time());
  for (double x : {p.x, p.y, p.z, vel.x, vel.y, vel.z, v.speed()}) add(x);
  add(std::atan2(fwd.x, fwd.z));
  add(std::asin(std::clamp(fwd.y, -1.0, 1.0)));
  add(std::asin(std::clamp(right.y, -1.0, 1.0)) * (up.y < 0 ? -1 : 1));
  add(v.rpm());
  add(v.gear());
  add(v.shaped_input().steer);
  add(v.shaped_input().throttle);
  add(v.shaped_input().brake);
  add(v.body_contact_force());
  add(v.clutch());
  add(v.abs_active());
  for (const auto& w : v.wheels()) {
    add(w.compression);
    add(w.omega);
    add(w.load);
    add(w.slip_ratio);
    add(w.slip_angle);
    add(w.brake);
    add(w.fx);
  }
  return row;
}

std::string physics_trace_event(const Vehicle& v, int64_t car_id, double clock_hz, int mode) {
  // gear: 0 reverse, 1..8 forward, 9 neutral.
  const int gear = v.gear() == -1 ? 0 : v.gear() == 0 ? 9 : v.gear();
  char buf[320];
  std::snprintf(buf, sizeof buf,
                "{\"event\":\"fh1.physics_trace\",\"frame\":%lld,\"t_ms\":%.3f,\"mode\":%d,"
                "\"cars\":\"0:%lld:0:%.4f:%.1f:%d:%.4f:%.4f:%.4f\"}",
                static_cast<long long>(std::llround(v.time() * clock_hz)), v.time() * 1000, mode,
                static_cast<long long>(car_id), std::fabs(v.speed()), v.rpm(), gear, v.shaped_input().throttle,
                v.shaped_input().brake, v.shaped_input().steer);
  return buf;
}

bool parse_script(const std::string& text, std::vector<ScriptLine>& out, std::string* error) {
  out.clear();
  std::istringstream in(text);
  in.imbue(std::locale::classic());
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::istringstream l(line);
    l.imbue(std::locale::classic());
    ScriptLine s;
    if (!(l >> s.time)) continue;  // blank
    int hand = 0;
    if (!(l >> s.input.steer >> s.input.throttle >> s.input.brake)) {
      if (error) *error = "script line " + std::to_string(number) + ": need time steer throttle brake";
      return false;
    }
    if (l >> hand) s.input.handbrake = hand != 0;
    if (!out.empty() && s.time < out.back().time) {
      if (error) *error = "script line " + std::to_string(number) + ": times must not go back";
      return false;
    }
    out.push_back(s);
  }
  return true;
}

Input script_input(const std::vector<ScriptLine>& script, double time) {
  Input in;
  for (const auto& s : script) {
    if (s.time > time + 1e-9) break;
    in = s.input;
  }
  return in;
}

}  // namespace drive
