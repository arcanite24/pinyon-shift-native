#include "traffic/bodies.h"

#include "drive/contact.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace traffic {
namespace {

using drive::Vec3;

}  // namespace

BodiesOptions bodies_options(const TrafficConfig& c) {
  BodiesOptions o;
  o.accel_max_input = c.accel_max_input;
  o.brake_max_input = c.brake_max_input;
  o.gains_min = c.gains_min;
  o.gains_max = c.gains_max;
  o.gains_length_min = c.gains_length_min;
  o.gains_length_max = c.gains_length_max;
  o.accel_max = c.accel_max;
  o.decel_max = c.decel_max;
  return o;
}

Bodies::Bodies(Population& population, Params params, BodiesOptions options)
    : pop_(population), params_(std::move(params)), o_(options) {
  pop_.set_bodies(true);
}

const drive::Vehicle* Bodies::body(uint32_t car) const {
  const auto it = bodies_.find(car);
  return it == bodies_.end() ? nullptr : it->second.v.get();
}

drive::Vehicle* Bodies::body(uint32_t car) {
  const auto it = bodies_.find(car);
  return it == bodies_.end() ? nullptr : it->second.v.get();
}

drive::Input Bodies::drive_input(const Car& car, const drive::Vehicle& v) const {
  drive::Input in;
  const double speed = v.speed();
  if (car.off_line) {
    in.brake = 1;  // knocked off its line: full brake until stopped
    return in;
  }
  const P3 aim = pop_.lane_point(car, std::max(8.0, speed));
  const Vec3 pos = v.position();
  const Vec3 local = drive::rotate_inverse(v.orientation(), Vec3{aim.x - pos.x, 0, aim.z - pos.z});
  const double d2 = local.x * local.x + local.z * local.z;
  const auto& p = v.params();
  const double wheelbase = std::fabs(p.wheel_centres[0].z - p.wheel_centres[2].z);
  // Pure pursuit: the road-wheel angle onto the arc through the aim point.
  const double delta = d2 > 1e-6 ? std::atan(2 * wheelbase * local.x / d2) : 0;
  in.steer = std::clamp(delta / std::max(0.05, p.max_steer), -1.0, 1.0);
  speed_input(car.target, speed, o_, in);
  return in;
}

void Bodies::speed_input(double target, double speed, const BodiesOptions& o, drive::Input& in) {
  const double err = target - speed;
  in.throttle = in.brake = 0;
  if (target < 0.5) in.brake = o.brake_max_input;  // stop and hold
  else if (err < -0.6) in.brake = std::clamp(-0.3 * err, 0.0, o.brake_max_input);
  else in.throttle = o.accel_max_input;
}

void Bodies::hold_lane(drive::Vehicle& v, double target, const std::array<double, 2>& lane, double lateral,
                       double heading_error, const BodiesOptions& o) {
  const double f = std::clamp((v.params().size.z - o.gains_length_min) /
                                  std::max(1e-6, o.gains_length_max - o.gains_length_min),
                              0.0, 1.0);
  auto k = [f](double a, double b) { return a + (b - a) * f; };
  // The share of an error removed in one step, and the rate that removes it.
  auto share = [](double gain) { return 1 - std::pow(1 - std::clamp(gain, 0.0, 1.0), drive::Vehicle::kStep * 60); };
  const double sp = share(k(o.gains_min.position_k, o.gains_max.position_k));
  const double sl = share(k(o.gains_min.lane_offset_k, o.gains_max.lane_offset_k));
  const double sh = share(k(o.gains_min.heading_k, o.gains_max.heading_k));
  const auto right = right_of(lane[0], lane[1]);
  Vec3 vel = v.velocity();
  const double along = vel.x * lane[0] + vel.z * lane[1];
  const double along2 = along + (target - along) * sp;
  const double across2 = std::clamp(-lateral * sl / drive::Vehicle::kStep, -o.hold_max_across, o.hold_max_across);
  vel.x = lane[0] * along2 + right[0] * across2;
  vel.z = lane[1] * along2 + right[1] * across2;
  Vec3 ang = v.angular_velocity();
  ang.y = std::clamp(heading_error * sh / drive::Vehicle::kStep, -o.hold_max_yaw, o.hold_max_yaw);
  v.set_velocity(vel, ang);
}

bool Bodies::collide(drive::Vehicle& a, double scale_a, drive::Vehicle& b, double scale_b, double* impulse) {
  double j = 0;
  if (!drive::car_contact(a, scale_a, b, scale_b, {o_.restitution, o_.friction}, &j)) return false;
  largest_ = std::max(largest_, j);
  if (impulse) *impulse = j;
  return true;
}

void Bodies::step(const drive::Ground& ground, drive::Vehicle* player) {
  auto& cars = pop_.cars();
  // Drop the bodies of cars gone or back to kinematic.
  std::set<uint32_t> wanted;
  for (const auto& c : cars)
    if (c.physics) wanted.insert(c.id);
  for (auto it = bodies_.begin(); it != bodies_.end();)
    it = wanted.count(it->first) ? std::next(it) : bodies_.erase(it);
  // Make the new ones on the move, from the lane pose.
  for (const auto& c : cars) {
    if (!c.physics || bodies_.count(c.id)) continue;
    const drive::CarParams* p = params_(c.model);
    if (!p) continue;
    Body b;
    b.v = std::make_unique<drive::Vehicle>(*p);
    b.v->set_ai_controls(true);
    b.v->launch({c.position.x, c.position.y, c.position.z}, c.heading, c.speed, c.pitch);
    bodies_.emplace(c.id, std::move(b));
  }
  for (auto& c : cars) c.body = bodies_.count(c.id) > 0;
  // Drive and step.
  for (const auto& c : cars) {
    const auto it = bodies_.find(c.id);
    if (it == bodies_.end()) continue;
    Body& b = it->second;
    b.since_contact += drive::Vehicle::kStep;
    if (!c.off_line && b.since_contact >= o_.hold_after_contact) {
      // The lane line at the body: its direction of travel, the body's
      // offset right of it and the heading error.
      const P3 p0 = pop_.lane_point(c, 0), p1 = pop_.lane_point(c, 1);
      const double dx = p1.x - p0.x, dz = p1.z - p0.z, l = std::hypot(dx, dz);
      if (l > 1e-6) {
        const std::array<double, 2> lane{dx / l, dz / l};
        const auto rt = right_of(lane[0], lane[1]);
        const Vec3 at = b.v->position();
        const Vec3 fw = drive::rotate(b.v->orientation(), {0, 0, 1});
        const double lateral = (at.x - p0.x) * rt[0] + (at.z - p0.z) * rt[1];
        const double herr = std::remainder(std::atan2(lane[0], lane[1]) - std::atan2(fw.x, fw.z), 2 * 3.14159265358979323846);
        const double sp = b.v->speed(), dt = drive::Vehicle::kStep;
        const double speed = sp + std::clamp(c.target - sp, -o_.decel_max * dt, o_.accel_max * dt);
        hold_lane(*b.v, speed, lane, lateral, herr, o_);
      }
    }
    b.v->step(drive_input(c, *b.v), ground);
  }
  // Contacts: each body with the player's car and with the others.
  std::vector<Body*> list;
  for (auto& [id, b] : bodies_) list.push_back(&b);
  step_player_ = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    double j = 0;
    if (player && collide(*player, 1.0, *list[i]->v, o_.torque_scale, &j)) {
      ++contacts_, ++player_contacts_;
      step_player_ = std::max(step_player_, j);
      list[i]->since_contact = 0;
    }
    for (size_t k = i + 1; k < list.size(); ++k)
      if (collide(*list[i]->v, o_.torque_scale, *list[k]->v, o_.torque_scale)) {
        ++contacts_;
        list[i]->since_contact = list[k]->since_contact = 0;
      }
  }
  // The poses back to the population.
  for (size_t i = 0; i < cars.size(); ++i) {
    const auto it = bodies_.find(cars[i].id);
    if (it == bodies_.end()) continue;
    const drive::Vehicle& v = *it->second.v;
    const Vec3 at = v.position();
    const Vec3 f = drive::rotate(v.orientation(), {0, 0, 1});
    const Vec3 u = drive::rotate(v.orientation(), {0, 1, 0});
    pop_.follow(i, {at.x, at.y, at.z}, std::atan2(f.x, f.z), v.speed(), u.y);
    cars[i].pitch = std::asin(std::clamp(f.y, -1.0, 1.0));
  }
}

}  // namespace traffic
