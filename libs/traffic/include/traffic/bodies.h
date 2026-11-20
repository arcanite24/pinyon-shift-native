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
  double restitution = 0.2, friction = 0.3;
  TrafficConfig::LaneGains gains_min, gains_max{0.9, 0.93, 0.93};
  double gains_length_min = 4.5, gains_length_max = 10;
  double hold_after_contact = 1.0;  // s without the lane hold after a contact (ours)
  // AI/Traffic/Speed/AccelerationMax, DeccelerationMax (m/s^2): the lane
  // speed the hold aims at moves towards the target at these, as a
  // kinematic car's does.
  double accel_max = 4, decel_max = 6;
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
  // share. The vertical motion and pitch and roll are the body's own.
  static void hold_lane(drive::Vehicle& v, double target, const std::array<double, 2>& lane, double lateral,
                        double heading_error, const BodiesOptions& o);

  const drive::Vehicle* body(uint32_t car) const;
  size_t count() const { return bodies_.size(); }
  int contacts() const { return contacts_; }  // since the start
  // Contacts with the player's car since the start, and the largest
  // impulse one of them gave (N s).
  int player_contacts() const { return player_contacts_; }
  double largest_impulse() const { return largest_; }

 private:
  struct Body {
    std::unique_ptr<drive::Vehicle> v;
    double since_contact = 1e9;  // s
  };
  drive::Input drive_input(const Car& car, const drive::Vehicle& v) const;
  bool collide(drive::Vehicle& a, double scale_a, drive::Vehicle& b, double scale_b);

  Population& pop_;
  Params params_;
  BodiesOptions o_;
  std::map<uint32_t, Body> bodies_;
  int contacts_ = 0, player_contacts_ = 0;
  double largest_ = 0;
};

}  // namespace traffic
