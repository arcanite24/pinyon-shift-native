// Car-to-car contact (ours until a spec covers it; SPEC-TRAFFIC's traffic
// bodies use it): each car is its CarParams::size box on the ground plane
// at its height band; two boxes that overlap are pushed apart by inverse
// mass and given an impulse with restitution and friction at the deepest
// corners. The contact normal is chosen with the boxes as they stood one
// step before (centres moved back by their velocities), so a fast closing
// pair that has sunk deep in one step is pushed back the way it came, not
// through: no tunnelling while the closing travel in a step stays under the
// boxes' combined size (about 500 km/h closing at 120 Hz for cars).
#pragma once

#include <span>

#include "drive/vehicle.h"

namespace drive {

struct ContactOptions {
  double restitution = 0.2, friction = 0.3;
};

// Resolves one pair; `scale_a`/`scale_b` scale each car's angular response
// (traffic: TrafficTorqueScale). True when they touched; `impulse` gets the
// normal impulse (N s, 0 when only separated).
bool car_contact(Vehicle& a, double scale_a, Vehicle& b, double scale_b, const ContactOptions& o,
                 double* impulse = nullptr);

// A car in a contact pass.
struct ContactBody {
  Vehicle* v = nullptr;
  double angular_scale = 1;
  int group = 0;  // pairs inside one group > 0 are skipped (resolved elsewhere)
};
// Every pair of `bodies` once (a circle test first); the number in contact.
int resolve_contacts(std::span<const ContactBody> bodies, const ContactOptions& o, double* largest = nullptr);

}  // namespace drive
