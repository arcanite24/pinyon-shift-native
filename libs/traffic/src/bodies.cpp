#include "traffic/bodies.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace traffic {
namespace {

using drive::Vec3;

// A body's box on the ground plane: centre, unit forward and right, half
// length and width, and its height band.
struct Box {
  Vec3 c;
  double fx, fz, rx, rz, hl, hw, y0, y1;
};

Box box_of(const drive::Vehicle& v) {
  const auto& p = v.params();
  const Vec3 local{0, p.sill_height + 0.5 * p.size.y, p.centre_of_mass.z};
  Box b;
  b.c = v.position() + drive::rotate(v.orientation(), local);
  const Vec3 f = drive::rotate(v.orientation(), {0, 0, 1});
  const double l = std::max(1e-6, std::hypot(f.x, f.z));
  b.fx = f.x / l, b.fz = f.z / l;
  const auto r = right_of(b.fx, b.fz);
  b.rx = r[0], b.rz = r[1];
  b.hl = 0.5 * p.size.z, b.hw = 0.5 * p.size.x;
  b.y0 = b.c.y - 0.5 * p.size.y, b.y1 = b.c.y + 0.5 * p.size.y;
  return b;
}

// The corners of a box (x, z).
std::array<std::array<double, 2>, 4> corners(const Box& b) {
  std::array<std::array<double, 2>, 4> out;
  int k = 0;
  for (double sl : {-1.0, 1.0})
    for (double sw : {-1.0, 1.0})
      out[size_t(k++)] = {b.c.x + b.fx * b.hl * sl + b.rx * b.hw * sw, b.c.z + b.fz * b.hl * sl + b.rz * b.hw * sw};
  return out;
}

bool inside(const Box& b, double x, double z) {
  const double dx = x - b.c.x, dz = z - b.c.z;
  return std::fabs(dx * b.fx + dz * b.fz) <= b.hl && std::fabs(dx * b.rx + dz * b.rz) <= b.hw;
}

}  // namespace

Bodies::Bodies(Population& population, Params params, BodiesOptions options)
    : pop_(population), params_(std::move(params)), o_(options) {
  pop_.set_bodies(true);
}

const drive::Vehicle* Bodies::body(uint32_t car) const {
  const auto it = bodies_.find(car);
  return it == bodies_.end() ? nullptr : it->second.v.get();
}

drive::Input Bodies::drive_input(const Car& car, const drive::Vehicle& v) const {
  drive::Input in;
  const double speed = v.speed();
  const P3 aim = pop_.lane_point(car, std::max(8.0, speed));
  const Vec3 pos = v.position();
  const Vec3 local = drive::rotate_inverse(v.orientation(), Vec3{aim.x - pos.x, 0, aim.z - pos.z});
  const double d2 = local.x * local.x + local.z * local.z;
  const auto& p = v.params();
  const double wheelbase = std::fabs(p.wheel_centres[0].z - p.wheel_centres[2].z);
  // Pure pursuit: the road-wheel angle onto the arc through the aim point.
  const double delta = d2 > 1e-6 ? std::atan(2 * wheelbase * local.x / d2) : 0;
  in.steer = std::clamp(delta / std::max(0.05, p.max_steer), -1.0, 1.0);
  const double err = car.target - speed;
  if (err > 0) in.throttle = std::clamp(0.08 * err + 0.05, 0.0, o_.accel_max_input);
  else if (err < -0.3 || car.target < 0.5) in.brake = std::clamp(-0.2 * err + (car.target < 0.5 ? 0.1 : 0), 0.0, o_.brake_max_input);
  return in;
}

bool Bodies::collide(drive::Vehicle& a, double scale_a, drive::Vehicle& b, double scale_b) {
  const Box A = box_of(a), B = box_of(b);
  if (A.y1 < B.y0 || B.y1 < A.y0) return false;
  if (std::hypot(A.c.x - B.c.x, A.c.z - B.c.z) > A.hl + A.hw + B.hl + B.hw) return false;
  // Separating axes: both boxes' forward and right.
  const double axes[4][2] = {{A.fx, A.fz}, {A.rx, A.rz}, {B.fx, B.fz}, {B.rx, B.rz}};
  double depth = 1e30;
  double nx = 0, nz = 0;
  for (const auto& ax : axes) {
    auto extent = [&](const Box& q) {
      return q.hl * std::fabs(q.fx * ax[0] + q.fz * ax[1]) + q.hw * std::fabs(q.rx * ax[0] + q.rz * ax[1]);
    };
    const double dist = (B.c.x - A.c.x) * ax[0] + (B.c.z - A.c.z) * ax[1];
    const double overlap = extent(A) + extent(B) - std::fabs(dist);
    if (overlap <= 0) return false;
    if (overlap < depth) {
      depth = overlap;
      const double s = dist < 0 ? -1 : 1;
      nx = ax[0] * s, nz = ax[1] * s;  // from A towards B
    }
  }
  // The contact point: the corners of each box inside the other.
  double px = 0, pz = 0;
  int n = 0;
  for (const auto& c : corners(B))
    if (inside(A, c[0], c[1])) px += c[0], pz += c[1], ++n;
  for (const auto& c : corners(A))
    if (inside(B, c[0], c[1])) px += c[0], pz += c[1], ++n;
  if (n == 0) px = 0.5 * (A.c.x + B.c.x), pz = 0.5 * (A.c.z + B.c.z), n = 1;
  const Vec3 point{px / n, 0.5 * (std::max(A.y0, B.y0) + std::min(A.y1, B.y1)), pz / n};
  const Vec3 normal{nx, 0, nz};
  const double ia = a.inverse_mass_along(point, normal, scale_a), ib = b.inverse_mass_along(point, normal, scale_b);
  // Separation, shared by inverse mass.
  const double ma = 1 / a.params().mass, mb = 1 / b.params().mass;
  const double push = std::max(0.0, depth - 0.01);
  a.translate(normal * (-push * ma / (ma + mb)));
  b.translate(normal * (push * mb / (ma + mb)));
  const Vec3 rel = b.point_velocity(point) - a.point_velocity(point);
  const double vn = drive::dot(rel, normal);
  if (vn >= 0) return true;
  const double j = -(1 + o_.restitution) * vn / (ia + ib);
  a.apply_impulse(point, normal * -j, scale_a);
  b.apply_impulse(point, normal * j, scale_b);
  // Friction along the contact, at most friction x j.
  Vec3 t = rel - normal * vn;
  t.y = 0;
  const double tl = std::sqrt(drive::dot(t, t));
  if (tl > 1e-6) {
    t = t * (1 / tl);
    const double it = a.inverse_mass_along(point, t, scale_a) + b.inverse_mass_along(point, t, scale_b);
    const double jt = std::min(o_.friction * j, tl / it);
    a.apply_impulse(point, t * jt, scale_a);
    b.apply_impulse(point, t * -jt, scale_b);
  }
  largest_ = std::max(largest_, j);
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
    it->second.v->step(drive_input(c, *it->second.v), ground);
  }
  // Contacts: each body with the player's car and with the others.
  std::vector<drive::Vehicle*> list;
  for (auto& [id, b] : bodies_) list.push_back(b.v.get());
  for (size_t i = 0; i < list.size(); ++i) {
    if (player && collide(*player, 1.0, *list[i], o_.torque_scale)) ++contacts_, ++player_contacts_;
    for (size_t k = i + 1; k < list.size(); ++k)
      if (collide(*list[i], o_.torque_scale, *list[k], o_.torque_scale)) ++contacts_;
  }
  // The poses back to the population.
  for (size_t i = 0; i < cars.size(); ++i) {
    const auto it = bodies_.find(cars[i].id);
    if (it == bodies_.end()) continue;
    const drive::Vehicle& v = *it->second.v;
    const Vec3 at = v.position();
    const Vec3 f = drive::rotate(v.orientation(), {0, 0, 1});
    pop_.follow(i, {at.x, at.y, at.z}, std::atan2(f.x, f.z), v.speed());
    cars[i].pitch = std::asin(std::clamp(f.y, -1.0, 1.0));
  }
}

}  // namespace traffic
