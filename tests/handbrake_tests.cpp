// The handbrake on the R34 V-Spec II (car 445), built from the player's
// own physics rows: at 60 km/h it locks the rear wheels and the car turns
// more than on the same steer without it, and it holds the car parked on a
// 10 % grade. The torque is an interim (CarParams::handbrake_torque; the
// spec's BiasHandbrake is only "probable"). Skipped (exit 77) without
// SHIFT_GAME_ROOT.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

#include "cars/physics.h"
#include "cars/sources.h"
#include "drive/from_physics.h"
#include "drive/vehicle.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

// The plane y = -grade z: downhill towards +z.
class SlopeGround : public drive::Ground {
 public:
  explicit SlopeGround(double grade) : g_(grade) {}
  bool raycast(const drive::Vec3& o, const drive::Vec3& d, double max_distance, drive::Hit& hit) const override {
    const double den = d.y + g_ * d.z;
    if (std::fabs(den) < 1e-12) return false;
    const double t = -(o.y + g_ * o.z) / den;
    if (t < 0 || t > max_distance) return false;
    const double l = std::sqrt(1 + g_ * g_);
    hit.distance = t;
    hit.normal = {0, 1 / l, g_ / l};
    return true;
  }

 private:
  double g_;
};

double yaw_of(const drive::Vehicle& v) {
  const drive::Vec3 f = drive::rotate(v.orientation(), {0, 0, 1});
  return std::atan2(f.x, f.z);
}

// From 60 km/h on flat ground with a little steer: the yaw after 1 s, and
// the rear wheels' slip (wheel speed against road speed, -1 locked).
void run(const drive::CarParams& p, bool handbrake, double& yaw, double& rear_slip) {
  const drive::FlatGround flat;
  drive::Vehicle v(p);
  v.launch({0, 0, 0}, 0, 60 / 3.6);
  for (int i = 0; i < 120; ++i) v.step({0.15, 0, 0, handbrake}, flat);
  yaw = std::fabs(yaw_of(v));
  rear_slip = 0;
  for (int w = 2; w < 4; ++w) {
    const auto& s = v.wheels()[size_t(w)];
    rear_slip += (s.omega * p.tyre_radius[size_t(w)] - v.speed()) / std::max(1.0, std::fabs(v.speed())) / 2;
  }
  std::printf("  %s: speed %.1f km/h, yaw %.1f deg, rear slip %.2f\n", handbrake ? "handbrake" : "no handbrake",
              v.speed() * 3.6, yaw * 57.2958, rear_slip);
}

}  // namespace

int main() {
  const char* root = std::getenv("SHIFT_GAME_ROOT");
  if (!root) {
    std::printf("SHIFT_GAME_ROOT not set: skipped\n");
    return 77;
  }
  const char* state = std::getenv("SHIFT_STATE_ROOT");
  cars::Install install;
  std::string error;
  cars::PhysicsSettings settings;
  std::vector<cars::CarPhysics> rows;
  if (!cars::discover_install(root, state ? state : "", "", install, &error) ||
      !cars::load_physics_settings(install.media / "physics.zip", settings, &error) ||
      !cars::load_car_physics(install, {445}, rows, &error)) {
    std::printf("FAIL %s\n", error.c_str());
    return 1;
  }
  drive::CarParams p;
  drive::geometry_from_physics(rows[0], p);
  drive::apply_physics(rows[0], settings, p, nullptr);
  std::printf("R34: handbrake %.0f N m per rear wheel (interim), rate %.2f\n", p.handbrake_torque, p.handbrake_rate);

  double yaw_free = 0, slip_free = 0, yaw_hand = 0, slip_hand = 0;
  run(p, false, yaw_free, slip_free);
  run(p, true, yaw_hand, slip_hand);
  CHECK(slip_hand < -0.9);           // the rears locked
  CHECK(yaw_hand > yaw_free * 1.5);  // and the car rotates

  // Parked on a 10 % grade, nose downhill: settle 2 s, then no more than
  // 5 cm of creep over 5 s.
  for (double heading : {0.0, 3.14159265358979323846}) {
    const SlopeGround slope(0.1);
    drive::Vehicle v(p);
    v.reset({0, 0, 0}, heading);
    for (int i = 0; i < 240; ++i) v.step({0, 0, 0, true}, slope);
    const drive::Vec3 a = v.position();
    for (int i = 0; i < 600; ++i) v.step({0, 0, 0, true}, slope);
    const drive::Vec3 b = v.position();
    const double creep = std::hypot(b.x - a.x, b.z - a.z);
    std::printf("  10 %% grade, %s: creep %.3f m over 5 s\n", heading == 0 ? "nose down" : "nose up", creep);
    CHECK(creep < 0.05);
  }
  std::printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
  return failures ? 1 : 0;
}
