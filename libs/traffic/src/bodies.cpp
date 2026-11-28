#include "traffic/bodies.h"

#include "drive/contact.h"
#include "fh1data/worker_pool.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>
#include <vector>

namespace traffic {
namespace {

using drive::Vec3;

}  // namespace

BodiesOptions bodies_options(const TrafficConfig& c) {
  BodiesOptions o;
  o.accel_max_input = c.accel_max_input;
  o.brake_max_input = c.brake_max_input;
  o.steering_multiplier = c.steering_multiplier;
  o.gains_min = c.gains_min;
  o.gains_max = c.gains_max;
  o.gains_length_min = c.gains_length_min;
  o.gains_length_max = c.gains_length_max;
  o.accel_max = c.accel_max;
  o.decel_max = c.decel_max;
  o.gear_max_speed = c.gear_max_speed;
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

drive::Input Bodies::drive_input(const Car& car, const drive::Vehicle& v, double& prev_steer) const {
  drive::Input in;
  const double speed = v.speed();
  if (car.off_line) {
    in.brake = 1;  // knocked off its line: full brake until stopped
    prev_steer *= 0.95;
    in.steer = prev_steer;
    return in;
  }
  const double look_d = std::clamp(speed * 0.75, 7.5, 24.0);
  const P3 aim = pop_.lane_point(car, look_d);
  const Vec3 pos = v.position();
  const Vec3 local = drive::rotate_inverse(v.orientation(), Vec3{aim.x - pos.x, 0, aim.z - pos.z});
  const double d2 = local.x * local.x + local.z * local.z;
  const auto& p = v.params();
  const double wheelbase = std::max(1.5, std::fabs(p.wheel_centres[0].z - p.wheel_centres[2].z));
  // Pure pursuit with speed-safe lateral acceleration cap and smooth steer rate clamping.
  double delta = d2 > 1e-6 ? std::atan(2.0 * wheelbase * local.x / d2) : 0.0;
  const double v_eff = std::max(speed, 2.5);
  const double grip_lock = std::clamp(std::atan(wheelbase * 7.5 / (v_eff * v_eff)), 0.08, std::max(0.08, p.max_steer));
  delta = std::clamp(delta, -grip_lock, grip_lock);
  const double want_steer =
      std::clamp((delta * std::clamp(o_.steering_multiplier, 0.4, 2.0)) / std::max(0.05, p.max_steer), -1.0, 1.0);
  constexpr double kSteerRate = 2.6;  // full-lock units per second
  const double max_step = kSteerRate * drive::Vehicle::kStep;
  prev_steer += std::clamp(want_steer - prev_steer, -max_step, max_step);
  in.steer = prev_steer;
  double target = car.target;
  if (o_.gear_max_speed[3] > 0.0) target = std::min(target, o_.gear_max_speed[3]);
  speed_input(target, speed, o_, in);
  return in;
}

void Bodies::speed_input(double target, double speed, const BodiesOptions& o, drive::Input& in) {
  const double err = target - speed;
  in.throttle = in.brake = 0;
  if (target < 1.0) {
    // Firm stop for obstacles/player/give-way instead of creeping through on 0.2 brake.
    in.brake = speed > 0.3 ? std::clamp(std::max(o.brake_max_input, 0.80), 0.0, 1.0) : o.brake_max_input;
  } else if (err < -2.5) {
    in.brake = std::clamp(-0.22 * err, o.brake_max_input, 0.85);
  } else if (err < -0.6) {
    in.brake = std::clamp(-0.3 * err, 0.0, o.brake_max_input);
  } else {
    in.throttle = o.accel_max_input;
  }
}

void Bodies::hold_lane(drive::Vehicle& v, double target, const std::array<double, 2>& lane, double lateral,
                       double heading_error, const BodiesOptions& o, double blend) {
  blend = std::clamp(blend, 0.0, 1.0);
  if (blend <= 1e-4) return;
  const double f = std::clamp((v.params().size.z - o.gains_length_min) /
                                  std::max(1e-6, o.gains_length_max - o.gains_length_min),
                              0.0, 1.0);
  auto k = [f](double a, double b) { return a + (b - a) * f; };
  auto share = [](double gain) { return 1 - std::pow(1 - std::clamp(gain, 0.0, 1.0), drive::Vehicle::kStep * 60); };
  const double sp = share(k(o.gains_min.position_k, o.gains_max.position_k)) * blend;
  const auto right = right_of(lane[0], lane[1]);
  Vec3 vel = v.velocity();
  const double along = vel.x * lane[0] + vel.z * lane[1];
  const double cur_across = vel.x * right[0] + vel.z * right[1];
  const double along2 = along + (target - along) * sp;
  // Gentle physically-scaled lane centering & yaw damping (steers on wheels instead of harsh crabbing).
  const double speed_scale = std::clamp(std::fabs(along) / 5.0, 0.0, 1.0);
  const double want_across = std::clamp(-lateral * 1.4 * speed_scale, -o.hold_max_across, o.hold_max_across);
  const double across2 = cur_across + (want_across - cur_across) * (0.28 * blend);
  vel.x = lane[0] * along2 + right[0] * across2;
  vel.z = lane[1] * along2 + right[1] * across2;
  Vec3 ang = v.angular_velocity();
  const double want_yaw = std::clamp(heading_error * 3.5 * speed_scale, -o.hold_max_yaw, o.hold_max_yaw);
  ang.y = ang.y + (want_yaw - ang.y) * (0.45 * blend);
  v.set_velocity(vel, ang);
}

bool Bodies::collide(drive::Vehicle& a, double scale_a, drive::Vehicle& b, double scale_b, double* impulse) {
  const Vec3 pre_b_vel = b.velocity();
  double j = 0;
  if (!drive::car_contact(a, scale_a, b, scale_b, {o_.restitution, o_.friction}, &j)) return false;
  // Realistic crumple-zone impulse absorption on struck traffic cars:
  // cap single-hit horizontal delta-v, prevent vertical upward catapults, and
  // clamp angular tumble rates so high-speed crashes spin/skid traffic naturally
  // ~10-25 m instead of launching cars comically far down the road or airborne.
  Vec3 bv = b.velocity();
  const double dvx = bv.x - pre_b_vel.x;
  const double dvz = bv.z - pre_b_vel.z;
  const double dvh = std::hypot(dvx, dvz);
  constexpr double kMaxTrafficDeltaV = 15.0;  // m/s (~54 km/h single-impact impulse cap)
  if (dvh > kMaxTrafficDeltaV) {
    const double s = kMaxTrafficDeltaV / dvh;
    bv.x = pre_b_vel.x + dvx * s;
    bv.z = pre_b_vel.z + dvz * s;
  }
  const double bh = std::hypot(bv.x, bv.z);
  constexpr double kMaxTrafficCrashSpeed = 34.0;
  if (bh > kMaxTrafficCrashSpeed) {
    const double s = kMaxTrafficCrashSpeed / bh;
    bv.x *= s;
    bv.z *= s;
  }
  const double max_vy = std::min(std::max(pre_b_vel.y, 0.0) + 1.0, 1.8);
  if (bv.y > max_vy) bv.y = max_vy;
  Vec3 ba = b.angular_velocity();
  ba.x = std::clamp(ba.x, -2.0, 2.0);
  ba.y = std::clamp(ba.y, -3.2, 3.2);
  ba.z = std::clamp(ba.z, -2.0, 2.0);
  b.set_velocity(bv, ba);
  largest_ = std::max(largest_, j);
  if (impulse) *impulse = j;
  return true;
}

void Bodies::step(const drive::Ground& ground, drive::Vehicle* player) {
  auto& cars = pop_.cars();
  // Chain-reaction pileup promotion: promote any traffic car within 22 m of a
  // recently struck or off-line traffic car into full physics so high-speed
  // pileups transfer momentum realistically instead of clipping kinematic cars.
  {
    struck_positions_scratch_.clear();
    for (const auto& c : cars) {
      const auto it = bodies_.find(c.id);
      const bool recently_struck = c.off_line || (it != bodies_.end() && it->second.since_contact < 2.5);
      if (recently_struck) struck_positions_scratch_.push_back(c.position);
    }
    if (!struck_positions_scratch_.empty()) {
      constexpr double kChainPromoteDistSq = 22.0 * 22.0;
      for (auto& c : cars) {
        if (c.physics) continue;
        for (const auto& sp : struck_positions_scratch_) {
          const double dx = c.position.x - sp.x, dz = c.position.z - sp.z;
          if (dx * dx + dz * dz <= kChainPromoteDistSq) {
            c.physics = true;
            break;
          }
        }
      }
    }
  }
  // Drop the bodies of cars gone or back to kinematic.
  wanted_scratch_.clear();
  for (const auto& c : cars)
    if (c.physics) wanted_scratch_.push_back(c.id);
  std::sort(wanted_scratch_.begin(), wanted_scratch_.end());
  for (auto it = bodies_.begin(); it != bodies_.end();)
    it = std::binary_search(wanted_scratch_.begin(), wanted_scratch_.end(), it->first) ? std::next(it)
                                                                                       : bodies_.erase(it);
  // Make the new ones on the move, from the lane pose (verify ground exists first so
  // Race AI promotions near streamed collision edges never drop a car through a hole).
  for (const auto& c : cars) {
    if (!c.physics || bodies_.count(c.id)) continue;
    const drive::CarParams* p = params_(c.model);
    if (!p) continue;
    drive::Hit hit;
    if (!ground.raycast({c.position.x, c.position.y + 4.0, c.position.z}, {0, -1, 0}, 12.0, hit)) continue;
    Body b;
    b.v = std::make_unique<drive::Vehicle>(*p);
    b.v->set_ai_controls(true);
    b.v->launch({c.position.x, c.position.y, c.position.z}, c.heading, c.speed, c.pitch);
    bodies_.emplace(c.id, std::move(b));
  }
  for (auto& c : cars) c.body = bodies_.count(c.id) > 0;
  // Drive and step promoted traffic vehicles in parallel on the frame-critical job lane.
  active_bodies_scratch_.clear();
  active_bodies_scratch_.reserve(bodies_.size());
  for (const auto& c : cars) {
    const auto it = bodies_.find(c.id);
    if (it != bodies_.end()) active_bodies_scratch_.push_back({&c, &it->second});
  }
  auto step_traffic_body = [&](size_t idx) {
    const Car& c = *active_bodies_scratch_[idx].car;
    Body& b = *active_bodies_scratch_[idx].body;
    b.since_contact += drive::Vehicle::kStep;
    if (!c.off_line && b.since_contact >= o_.hold_after_contact) {
      // Smooth pursuit chord along the lane so junctions never produce a 90-deg rail snap
      // and hold_lane aligns with (rather than fights) pure-pursuit lane re-entry.
      const double chord_d = std::clamp(c.speed * 0.55, 6.0, 16.0);
      const P3 p0 = pop_.lane_point(c, 0), p1 = pop_.lane_point(c, chord_d);
      const double dx = p1.x - p0.x, dz = p1.z - p0.z, l = std::hypot(dx, dz);
      if (l > 1e-6) {
        const std::array<double, 2> lane{dx / l, dz / l};
        const auto rt = right_of(lane[0], lane[1]);
        const Vec3 at = b.v->position();
        const Vec3 fw = drive::rotate(b.v->orientation(), {0, 0, 1});
        const double lateral = (at.x - p0.x) * rt[0] + (at.z - p0.z) * rt[1];
        const double aim_dx = p1.x - at.x, aim_dz = p1.z - at.z;
        const double aim_h = std::hypot(aim_dx, aim_dz) > 0.5 ? std::atan2(aim_dx, aim_dz) : std::atan2(lane[0], lane[1]);
        const double herr = std::remainder(aim_h - std::atan2(fw.x, fw.z), 2 * 3.14159265358979323846);
        const double sp = b.v->speed(), dt = drive::Vehicle::kStep;
        const double decel_rate = c.target < 1.0 ? o_.decel_max * 1.45 : o_.decel_max;
        const double speed = sp + std::clamp(c.target - sp, -decel_rate * dt, o_.accel_max * dt);
        const double hold_blend = std::clamp((b.since_contact - o_.hold_after_contact) / 1.0, 0.0, 1.0);
        hold_lane(*b.v, speed, lane, lateral, herr, o_, hold_blend);
      }
    }
    b.v->step(drive_input(c, *b.v, b.prev_steer), ground);
    // Struck off-line traffic cars scrub speed through locked tyres / road drag
    // so they spin/skid to a grounded stop within ~10-25 m instead of sliding forever.
    if (c.off_line || b.since_contact < 1.5) {
      constexpr double dt = drive::Vehicle::kStep;
      Vec3 v = b.v->velocity();
      Vec3 w = b.v->angular_velocity();
      const double vh = std::hypot(v.x, v.z);
      const double decel = c.off_line ? 8.5 : 3.5;
      if (vh > 0.05) {
        const double nh = std::max(0.0, vh - decel * dt);
        const double sc = nh / vh;
        v.x *= sc;
        v.z *= sc;
      }
      if (v.y > 2.2) v.y = 2.2;
      const double wd = std::exp(-dt * (c.off_line ? 4.0 : 2.0));
      w.x *= wd;
      w.y *= wd;
      w.z *= wd;
      b.v->set_velocity(v, w);
    }
  };
  fh1::parallel_for_frame(active_bodies_scratch_.size(), step_traffic_body);
  // Contacts: resolve player-vs-traffic first so struck traffic immediately receives
  // impact momentum, then run an iterative pairwise traffic-vs-traffic solver (up to
  // 3 passes) so multi-car pileups propagate momentum cleanly through secondary impacts.
  body_list_scratch_.clear();
  body_list_scratch_.reserve(bodies_.size());
  for (auto& [id, b] : bodies_) body_list_scratch_.push_back(&b);
  step_player_ = 0;
  if (player) {
    for (Body* b : body_list_scratch_) {
      double j = 0;
      // SPEC-TRAFFIC / PhysicsSettings.ini [Collision]: PlayerTorqueScale = 0.0.
      if (collide(*player, 0.0, *b->v, o_.torque_scale, &j)) {
        ++contacts_, ++player_contacts_;
        step_player_ = std::max(step_player_, j);
        b->since_contact = 0;
      }
    }
  }
  constexpr int kMaxPileupPasses = 3;
  counted_pairs_scratch_.clear();
  for (int pass = 0; pass < kMaxPileupPasses; ++pass) {
    bool any_hit = false;
    for (size_t i = 0; i < body_list_scratch_.size(); ++i) {
      for (size_t k = i + 1; k < body_list_scratch_.size(); ++k) {
        if (collide(*body_list_scratch_[i]->v, o_.torque_scale, *body_list_scratch_[k]->v, o_.torque_scale)) {
          any_hit = true;
          const uint64_t pair_key = (uint64_t(i) << 32) | uint64_t(k);
          if (std::find(counted_pairs_scratch_.begin(), counted_pairs_scratch_.end(), pair_key) ==
              counted_pairs_scratch_.end()) {
            counted_pairs_scratch_.push_back(pair_key);
            ++contacts_;
          }
          body_list_scratch_[i]->since_contact = 0.0;
          body_list_scratch_[k]->since_contact = 0.0;
        }
      }
    }
    if (!any_hit) break;
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
    if (v.gear() > 0) cars[i].gear = v.gear();
    if (v.rpm() > 100.0) cars[i].rpm = v.rpm();
  }
}

}  // namespace traffic
