// Traffic cars in full physics (SPEC-TRAFFIC, Physics and level of detail,
// Collision with the player): each car the population puts in physics
// (within the high-LOD distance of the player, or knocked off its line)
// gets a drive::Vehicle with its model's own rows, taken over on the move
// from its lane pose, driven along its lane with the AI's controls, and
// stepped on the ground; its pose goes back to the population. Bodies push
// each other and the player's car.
//
// Ours until the spec says: the lane is followed by pure pursuit at a
// look-ahead of max(8 m, 1 s), the speed by a proportional throttle and
// brake whose changes are limited to AI/Traffic/VehicleControls/
// AccelMaxInput and BrakeMaxInput per second (interim reading, Q41: read
// as caps on the inputs, four light models could not hold 30 mph; the gear
// caps and SteeringMultiplier are not used); the
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
  double accel_max_input = 0.2;  // AI/Traffic/VehicleControls/AccelMaxInput (throttle change per s)
  double brake_max_input = 0.2;  // AI/Traffic/VehicleControls/BrakeMaxInput (brake change per s)
  double restitution = 0.2, friction = 0.3;
};

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

  // The throttle and brake for a target speed one step on from `last`
  // (ours: proportional with a small hold; each input moves towards its
  // wanted value at most the options' rate per second).
  static void speed_input(double target, double speed, const BodiesOptions& o, const drive::Input& last,
                          drive::Input& in);

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
    drive::Input last;  // the inputs of the last step
  };
  drive::Input drive_input(const Car& car, const drive::Vehicle& v, const drive::Input& last) const;
  bool collide(drive::Vehicle& a, double scale_a, drive::Vehicle& b, double scale_b);

  Population& pop_;
  Params params_;
  BodiesOptions o_;
  std::map<uint32_t, Body> bodies_;
  int contacts_ = 0, player_contacts_ = 0;
  double largest_ = 0;
};

}  // namespace traffic
