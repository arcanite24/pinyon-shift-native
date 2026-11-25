// Traffic cars in full physics (SPEC-TRAFFIC, Physics and level of detail,
// Collision with the player): each car the population puts in physics
// (within the high-LOD distance of the player, or knocked off its line)
// gets a drive::Vehicle with its model's own rows, taken over on the move
// from its lane pose, driven along its lane with the AI's controls, and
// stepped on the ground; its pose goes back to the population. Bodies push
// each other and the player's car.
//
// On its line a body drives at the throttle AI/Traffic/VehicleControls/
// AccelMaxInput (0.2, a cap the oracle's cars sat on), braking at most
// BrakeMaxInput, steered by pure pursuit at a look-ahead of max(8 m, 1 s);
// the lane hold keeps it at its lane speed, offset and heading with the
// VehiclePosition/Interpolation gains (hold_lane). Knocked off its line it
// brakes at 1.0 with no hold. Ours until the spec says (Q41): the gains'
// reading (hold_lane), the hold left off for 1 s after a contact, no first
// gear forced off the line, the gear caps and SteeringMultiplier unused; the
// contacts are between the bodies' boxes (CarParams::size) on the ground
// plane, an impulse with restitution 0.2 and friction 0.3 at the deepest
// corners, the angular part on a traffic car scaled by TrafficTorqueScale,
// and CollisionBiasTraffic's even share read as the plain mass-weighted
// split.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>

#include "drive/vehicle.h"
#include "traffic/population.h"

namespace traffic {

struct BodiesOptions {
  double torque_scale = 0.25;   // PhysicsSettings Collision\TrafficTorqueScale
  double accel_max_input = 0.2;  // AI/Traffic/VehicleControls/AccelMaxInput (the throttle on the line)
  double brake_max_input = 0.2;  // AI/Traffic/VehicleControls/BrakeMaxInput (the brake cap on the line)
  double steering_multiplier = 1.0;
  double restitution = 0.05, friction = 0.45;
  TrafficConfig::LaneGains gains_min, gains_max{0.9, 0.93, 0.93};
  double gains_length_min = 4.5, gains_length_max = 10;
  double hold_after_contact = 1.0;  // s without the lane hold after a contact (ours)
  // The most the hold commands across the lane (m/s) and in yaw (rad/s),
  // so a body nudged off its line goes back to it rather than snapping
  // (ours).
  double hold_max_across = 1.5, hold_max_yaw = 1.0;
  // AI/Traffic/Speed/AccelerationMax, DeccelerationMax (m/s^2): the lane
  // speed the hold aims at moves towards the target at these, as a
  // kinematic car's does.
  double accel_max = 4, decel_max = 6;
  std::array<double, 4> gear_max_speed{12.0, 22.0, 34.0, 48.0};  // GearMaxSpeed1st..4th (m/s)
};

// The options from the traffic configuration (registry values).
BodiesOptions bodies_options(const TrafficConfig& config);

class Bodies {
 public:
  // The parameters of a model's car (null: that model stays kinematic).
  using Params = std::function<const drive::CarParams*(int64_t model)>;
  Bodies(Population& population, Params params, BodiesOptions options = {});

  // One fixed step (drive::Vehicle::kStep), after Population::step: bodies
  // made and dropped as the population says, driven and stepped, the
  // contacts (with the player's car when given) resolved, and the poses
  // handed back.
  void step(const drive::Ground& ground, drive::Vehicle* player);

  // The throttle and brake on the line for a target speed: the throttle at
  // AccelMaxInput unless the car is 0.6 m/s over its target, then the brake
  // (proportional, capped at BrakeMaxInput).
  static void speed_input(double target, double speed, const BodiesOptions& o, drive::Input& in);
  // The lane hold, one step (drive::Vehicle::kStep), before the body's
  // step: `lane` the unit direction of travel along the lane line,
  // `lateral` the body's offset right of the line (m), `heading_error` the
  // line's heading less the body's (rad). Our reading of the gains (Q41):
  // each K is the share of the error removed per 1/60 s tick, the gains
  // interpolated by the body's length between the Min and Max cars; the
  // speed along the lane goes to `target`, the speed across it and the yaw
  // rate to those that remove the offset and the heading error at that
  // share, capped at hold_max_across and hold_max_yaw. The vertical motion
  // and pitch and roll are the body's own.
  static void hold_lane(drive::Vehicle& v, double target, const std::array<double, 2>& lane, double lateral,
                        double heading_error, const BodiesOptions& o, double blend = 1.0);

  const drive::Vehicle* body(uint32_t car) const;
  drive::Vehicle* body(uint32_t car);
  size_t count() const { return bodies_.size(); }
  double torque_scale() const { return o_.torque_scale; }
  // Every body, for a contact pass with other cars.
  template <class F>
  void for_each(F f) {
    for (auto& [id, b] : bodies_) f(*b.v);
  }
  int contacts() const { return contacts_; }  // since the start
  // Contacts with the player's car since the start, and the largest
  // impulse one of them gave (N s).
  int player_contacts() const { return player_contacts_; }
  double largest_impulse() const { return largest_; }
  // The largest impulse a body gave the player's car in the last step (N s).
  double player_impulse() const { return step_player_; }

  struct BodySnapshot {
    drive::Vehicle vehicle;
    double since_contact = 1e9;
    double prev_steer = 0.0;
    BodySnapshot(const drive::Vehicle& v, double sc, double ps = 0.0)
        : vehicle(v), since_contact(sc), prev_steer(ps) {}
  };
  struct Snapshot {
    std::map<uint32_t, BodySnapshot> bodies;
  };
  Snapshot snapshot() const {
    Snapshot s;
    for (const auto& [id, b] : bodies_) {
      if (b.v) s.bodies.emplace(id, BodySnapshot(*b.v, b.since_contact, b.prev_steer));
    }
    return s;
  }
  void restore(const Snapshot& s) {
    bodies_.clear();
    for (const auto& [id, bs] : s.bodies) {
      Body b;
      b.v = std::make_unique<drive::Vehicle>(bs.vehicle);
      b.since_contact = bs.since_contact;
      b.prev_steer = bs.prev_steer;
      bodies_.emplace(id, std::move(b));
    }
    step_player_ = 0.0;
  }

 private:
  struct Body {
    std::unique_ptr<drive::Vehicle> v;
    double since_contact = 1e9;  // s
    double prev_steer = 0.0;
  };
  drive::Input drive_input(const Car& car, const drive::Vehicle& v, double& prev_steer) const;
  bool collide(drive::Vehicle& a, double scale_a, drive::Vehicle& b, double scale_b, double* impulse = nullptr);

  Population& pop_;
  Params params_;
  BodiesOptions o_;
  std::map<uint32_t, Body> bodies_;
  int contacts_ = 0, player_contacts_ = 0;
  double largest_ = 0, step_player_ = 0;
};

}  // namespace traffic
