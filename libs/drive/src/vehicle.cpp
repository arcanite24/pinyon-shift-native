#include "drive/vehicle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace drive {
namespace {

constexpr double kGravity = 9.81;
constexpr double kPi = 3.14159265358979323846;

double length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 normalized(const Vec3& a) {
  const double l = length(a);
  return l > 1e-12 ? a * (1.0 / l) : Vec3{};
}
double clamp1(double x) { return std::clamp(x, -1.0, 1.0); }
double approach(double from, double to, double rate, double dt) {
  const double d = to - from, step = rate * dt;
  return std::fabs(d) <= step ? to : from + (d > 0 ? step : -step);
}

}  // namespace

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
    // A solid box of the car's size (placeholder).
    const double w = p_.size.x, h = p_.size.y, l = p_.size.z, m = p_.mass / 12.0;
    inertia_ = {m * (h * h + l * l), m * (w * w + l * l), m * (w * w + h * h)};
  }
  // Static load per wheel from the centre of mass between the axles, so
  // the preloaded springs hold the car exactly at rest length.
  const double z_front = (p_.wheel_centres[0].z + p_.wheel_centres[1].z) / 2;
  const double z_rear = (p_.wheel_centres[2].z + p_.wheel_centres[3].z) / 2;
  const double span = z_front - z_rear;
  const double front_share = std::fabs(span) > 1e-6 ? (com_local_.z - z_rear) / span : 0.5;
  for (int i = 0; i < kWheels; ++i) {
    const double share = i < 2 ? front_share : 1 - front_share;
    // Left and right by the centre of mass's x between the two wheels.
    const double xl = p_.wheel_centres[i < 2 ? 0 : 2].x, xr = p_.wheel_centres[i < 2 ? 1 : 3].x;
    const double side = std::fabs(xr - xl) > 1e-6 ? (i % 2 == 1 ? (com_local_.x - xl) / (xr - xl)
                                                                 : (xr - com_local_.x) / (xr - xl))
                                                   : 0.5;
    preload_[i] = p_.mass * kGravity * share * side;
    const double corner_mass = preload_[i] / kGravity;
    const double omega = 2 * kPi * p_.frequency;
    spring_rate_[i] = corner_mass * omega * omega;
    damping_[i] = 2 * p_.damping_ratio * std::sqrt(spring_rate_[i] * corner_mass);
  }
  reset({0, 0, 0});
}

void Vehicle::place(const Vec3& position, double heading) {
  const double t = time_;
  reset(position, heading);
  time_ = t;
}

void Vehicle::reset(const Vec3& position, double heading) {
  rot_ = {0, std::sin(heading / 2), 0, std::cos(heading / 2)};
  pos_ = position + rotate(rot_, com_local_);  // pos_ holds the centre of mass
  vel_ = ang_ = {};
  gear_ = 1;
  shift_timer_ = 0;
  rpm_ = p_.idle_rpm;
  time_ = 0;
  raw_ = shaped_ = {};
  for (int i = 0; i < kWheels; ++i) {
    w_[i] = {};
    w_[i].centre = p_.wheel_centres[i];
  }
}

Vec3 Vehicle::position() const { return pos_ - rotate(rot_, com_local_); }

double Vehicle::speed() const { return dot(vel_, rotate(rot_, {0, 0, 1})); }

void Vehicle::shape_input(const Input& raw, double dt) {
  // Our own shaping (CAR_PHYSICS.md will give the game's): rate-limited
  // steering, faster back to centre; throttle and brake ramps.
  const bool centring = std::fabs(raw.steer) < std::fabs(shaped_.steer) && raw.steer * shaped_.steer >= 0;
  shaped_.steer = approach(shaped_.steer, std::clamp(raw.steer, -1.0, 1.0), centring ? 5.0 : 3.0, dt);
  shaped_.throttle = approach(shaped_.throttle, std::clamp(raw.throttle, 0.0, 1.0), 8.0, dt);
  shaped_.brake = approach(shaped_.brake, std::clamp(raw.brake, 0.0, 1.0), 8.0, dt);
  shaped_.handbrake = raw.handbrake;
}

double Vehicle::engine_torque(double rpm) const {
  // Placeholder curve: rising from idle, flat 1,500-6,000 rpm, falling to
  // the redline.
  if (rpm >= p_.redline_rpm) return 0;
  if (rpm < 1500) return p_.peak_torque * (0.6 + 0.4 * std::max(0.0, rpm - p_.idle_rpm) / (1500 - p_.idle_rpm));
  if (rpm > 6000) return p_.peak_torque * (1 - 0.3 * (rpm - 6000) / (p_.redline_rpm - 6000));
  return p_.peak_torque;
}

void Vehicle::update_gearbox(double dt) {
  if (shift_timer_ > 0) {
    shift_timer_ = std::max(0.0, shift_timer_ - dt);
    return;
  }
  const double v = speed();
  if (gear_ >= 1) {
    // Reverse when stopped and holding the brake alone.
    if (std::fabs(v) < 0.5 && shaped_.brake > 0.5 && shaped_.throttle < 0.05) {
      gear_ = -1;
      shift_timer_ = p_.shift_time;
      return;
    }
    // Shift on the engine speed the road speed gives through each gear, not
    // on the driven wheels' (wheelspin would shift up, then the shift's
    // pause would shift down again, over and over up a hill).
    double radius = 0;
    int driven = 0;
    for (int i = 0; i < kWheels; ++i)
      if ((i < 2) != (p_.layout == CarParams::Layout::kRear) || p_.layout == CarParams::Layout::kAll) {
        radius += p_.tyre_radius[size_t(i)];
        ++driven;
      }
    radius = driven ? radius / driven : 0.3;
    auto road_rpm = [&](int g) { return std::fabs(v) / radius * p_.gears[size_t(g - 1)] * p_.final_drive * 60 / (2 * kPi); };
    // Also up from the limiter when the next gear does not bog down.
    const bool limiter = rpm_ >= p_.redline_rpm * 0.98 && gear_ < int(p_.gears.size()) &&
                         road_rpm(gear_ + 1) > p_.shift_down_rpm + 500;
    if ((road_rpm(gear_) > p_.shift_up_rpm || limiter) && gear_ < int(p_.gears.size())) {
      ++gear_;
      shift_timer_ = p_.shift_time;
    } else if (gear_ > 1 && road_rpm(gear_) < p_.shift_down_rpm && road_rpm(gear_ - 1) < p_.shift_up_rpm * 0.9) {
      --gear_;
      shift_timer_ = p_.shift_time;
    }
  } else if (gear_ == -1) {
    if (v > -0.5 && shaped_.throttle > 0.5 && shaped_.brake < 0.05) {
      gear_ = 1;
      shift_timer_ = p_.shift_time;
    }
  }
}

void Vehicle::step(const Input& input, const Ground& ground) {
  raw_ = input;
  shape_input(input, kStep);
  update_gearbox(kStep);
  const double dt = kStep / kSubsteps;
  for (int s = 0; s < kSubsteps; ++s) substep(dt, ground);
  time_ += kStep;
}

void Vehicle::substep(double dt, const Ground& ground) {
  const Vec3 up = rotate(rot_, {0, 1, 0});
  const Vec3 down = up * -1.0;
  Vec3 force{0, -p_.mass * kGravity, 0};
  Vec3 torque{};

  // Drive and brake inputs (reverse swaps the pedals).
  const double drive_in = gear_ == -1 ? shaped_.brake : shaped_.throttle;
  const double brake_in = gear_ == -1 ? shaped_.throttle : shaped_.brake;
  const double ratio = gear_ == -1 ? -p_.reverse_gear : gear_ >= 1 ? p_.gears[size_t(gear_ - 1)] : 0;
  const bool driven[kWheels] = {p_.layout != CarParams::Layout::kRear, p_.layout != CarParams::Layout::kRear,
                                p_.layout != CarParams::Layout::kFront, p_.layout != CarParams::Layout::kFront};
  int driven_count = 0;
  double driven_omega = 0;
  for (int i = 0; i < kWheels; ++i)
    if (driven[i]) {
      ++driven_count;
      driven_omega += w_[i].omega;
    }
  driven_omega /= std::max(1, driven_count);
  const double wheel_rpm = driven_omega * 60 / (2 * kPi);
  rpm_ = std::clamp(std::fabs(wheel_rpm * ratio * p_.final_drive), p_.idle_rpm, p_.redline_rpm);
  const double engine = shift_timer_ > 0 || ratio == 0 ? 0 : engine_torque(rpm_) * drive_in;
  const double axle_torque = engine * ratio * p_.final_drive * p_.drivetrain_efficiency;

  const double v_abs = length(vel_);
  const double lock = p_.max_steer / (1 + std::fabs(speed()) / p_.steer_speed_falloff);

  for (int i = 0; i < kWheels; ++i) {
    WheelState& w = w_[i];
    const CarParams& p = p_;
    const double radius = p.tyre_radius[i];
    const Vec3 hard_local{p.wheel_centres[i].x, p.wheel_centres[i].y + p.rest_length, p.wheel_centres[i].z};
    const Vec3 hard = pos_ + rotate(rot_, hard_local - com_local_);
    w.steer = i < 2 ? shaped_.steer * lock : 0;

    Hit hit;
    const double reach = p.rest_length + p.droop_travel + radius + p.probe_height;
    const Vec3 probe = hard - down * p.probe_height;
    double len = p.rest_length + p.droop_travel;
    double load = 0;
    Vec3 contact{}, normal{0, 1, 0};
    w.contact = up.y > 0.2 && ground.raycast(probe, down, reach, hit);
    if (w.contact) {
      len = hit.distance - p.probe_height - radius;
      normal = hit.normal;
      contact = probe + down * hit.distance;
      const Vec3 v_hard = vel_ + cross(ang_, hard - pos_);
      const double len_rate = dot(v_hard, up);  // the hardpoint rising lengthens the spring
      double f = preload_[i] + spring_rate_[i] * (p.rest_length - len) - damping_[i] * len_rate;
      const double bump_limit = p.rest_length - p.bump_travel;
      if (len < bump_limit) f += 20 * spring_rate_[i] * (bump_limit - len);  // bump stop
      load = std::max(0.0, f);
    }
    len = std::clamp(len, p.rest_length - p.bump_travel - 0.05, p.rest_length + p.droop_travel);
    w.compression = p.rest_length - len;
    w.load = load;
    w.centre = {p.wheel_centres[i].x, p.wheel_centres[i].y + w.compression, p.wheel_centres[i].z};

    // Tyre forces in the ground plane.
    double fx = 0;
    if (w.contact && load > 0) {
      const Vec3 heading_local{std::sin(w.steer), 0, std::cos(w.steer)};
      Vec3 fwd = rotate(rot_, heading_local);
      fwd = normalized(fwd - normal * dot(fwd, normal));
      const Vec3 lat = normalized(cross(normal, fwd));  // +x on flat ground with fwd +z
      const Vec3 v_contact = vel_ + cross(ang_, contact - pos_);
      const double v_long = dot(v_contact, fwd), v_lat = dot(v_contact, lat);
      const double denom = std::max(std::fabs(v_long), 3.0);
      double sx = ((w.omega * radius - v_long) / denom) / p.peak_slip_ratio;
      double sy = std::atan2(v_lat, denom) / p.peak_slip_angle;
      const double s = std::sqrt(sx * sx + sy * sy);
      if (s > 1) {
        sx /= s;
        sy /= s;
      }
      const double grip = p.friction * hit.grip * load;
      fx = grip * sx;
      const double fy = -grip * sy;
      const double roll = -p.rolling_resistance * load * clamp1(v_long / 0.5);
      const Vec3 f = normal * load + fwd * (fx + roll) + lat * fy;
      force = force + f;
      torque = torque + cross(contact - pos_, f);
    }

    // Wheel spin: drive, tyre reaction, then brakes (which cannot reverse it).
    const double drive = driven[i] ? axle_torque / driven_count : 0;
    double omega = w.omega + (drive - fx * radius) / p.wheel_inertia * dt;
    double brake = brake_in * p.brake_torque * (i < 2 ? 1.0 : p.brake_rear_share);
    if (i >= 2 && shaped_.handbrake) brake += p.handbrake_torque;
    const double brake_step = brake / p.wheel_inertia * dt;
    omega = std::fabs(omega) <= brake_step ? 0 : omega - std::copysign(brake_step, omega);
    if (!w.contact) omega *= 1 - 0.5 * dt;  // bearing drag in the air
    w.omega = omega;
    w.spin = std::fmod(w.spin + omega * dt, 2 * kPi);
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
  const Vec3 centre_local{0, p_.sill_height + 0.5 * p_.size.y, p_.centre_of_mass.z};
  const Vec3 centre = pos_ + rotate(rot_, centre_local - com_local_);
  for (int k = 0; k < 27; ++k) {
    const int ix = k % 3 - 1, iy = k / 3 % 3 - 1, iz = k / 9 - 1;
    if (ix == 0 && iy == 0 && iz == 0) continue;
    const Vec3 point_local = centre_local + Vec3{0.5 * ix * p_.size.x, 0.5 * iy * p_.size.y, 0.5 * iz * p_.size.z};
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
    const double fn = kShare * std::max(0.0, p_.mass * (p_.body_stiffness * depth - p_.body_damping * vn));
    if (fn <= 0) continue;
    Vec3 f = n * fn;
    const Vec3 vt = v - n * vn;
    const double vt_len = length(vt);
    if (vt_len > 1e-6) {
      const double ft = std::min(p_.body_friction * fn, p_.mass / kPoints * vt_len / dt);
      f = f - vt * (ft / vt_len);
    }
    force = force + f;
    torque = torque + cross(point - pos_, f);
    body_force_ += length(f);
  }

  // Aerodynamic drag.
  force = force - vel_ * (0.5 * p_.air_density * p_.drag_area * v_abs);

  // Integrate (semi-implicit Euler).
  vel_ = vel_ + force * (dt / p_.mass);
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
  std::string h = "t,x,y,z,vx,vy,vz,speed,yaw,pitch,roll,rpm,gear,steer,throttle,brake,body";
  for (int i = 0; i < kWheels; ++i) {
    const std::string n = std::to_string(i);
    h += ",comp" + n + ",omega" + n + ",load" + n;
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
  for (const auto& w : v.wheels()) {
    add(w.compression);
    add(w.omega);
    add(w.load);
  }
  return row;
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
