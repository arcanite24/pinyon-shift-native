// Free-roam traffic's population (SPEC-TRAFFIC, Rule): the local density
// and the target around the player, spawning at the spawn-in distance and
// despawning past the spawn-out one, the loaded models and their rotation,
// and each car driving its lane on the road map. Cars are placed
// kinematically on their lane; the host puts the ones `physics()` names
// in full physics and hands their poses back with `follow` (bodies.h).
//
// Interim rules where the spec leaves things open (its Unknowns table):
// density per 100 m of traffic road within the spawn-out distance; one
// lane each way 2.2 m right of the centre line, one-way ways the same; a
// uniform pick among the allowed ways at a junction; uniform model picks
// among the loaded set; braking to keep the gap, no swerve; despawn at the
// spawn-out distance + 20 m (390 m on the disc). Ours: the spawn interval
// (TrafficMed +- TrafficVariance, which fits the 5 to 9 s measured), the
// away chance's rise with the player's speed (SpawnAwayChanceMin at rest
// to the road's value at SpawnAwayChanceLerpSpeed), one loaded model
// replaced per rotation, and a rotated-out model's cars driving on.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include "traffic/config.h"
#include "traffic/roads.h"

namespace traffic {

struct Car {
  uint32_t id = 0;          // unique per population
  int64_t model = 0;        // Data_Car.Id
  int road = -1;
  int dir = 1;              // +1: node order, -1: against
  double s = 0;             // arc length along the road's centre line
  double lateral = 2.2;     // m right of the centre line, in its direction of travel
  double speed = 0, target = 0;  // m/s
  double base_speed = 0;    // the road's speed with this car's variance
  int last_node = -1;       // the node it last passed (no turning back there)
  // The pose on the lane: the body frame's origin (ground height when the
  // host gives one) and the heading (atan2 of forward x, z).
  P3 position;
  double heading = 0, pitch = 0;
  bool physics = false;     // in full physics (within the high-LOD distance, or knocked off its line)
  bool off_line = false;    // knocked off its lane (stays in physics until back on it)
  double age = 0;           // s since spawning
};

struct PopulationStats {
  int spawned = 0, despawned = 0, initial = 0;
  std::vector<double> spawn_distances, despawn_distances;
  int spawned_away = 0;
};

class Population {
 public:
  // Heights for the kinematic placement: the ground at (x, z) near
  // `y_hint`, false where none is loaded (the road's own height is used).
  using GroundHeight = std::function<bool(double x, double z, double y_hint, double& y)>;

  Population(const Roads& roads, const TrafficConfig& config, uint64_t seed = 1);
  void set_ground(GroundHeight ground) { ground_ = std::move(ground); }
  // Whether the host moves the cars `physics()` names (bodies.h); without
  // one, every car is placed on its lane (the flag is still set).
  void set_bodies(bool on) { bodies_ = on; }
  // The hour of the world clock (0 to 24).
  void set_hour(double hour) { hour_ = hour; }
  double hour() const { return hour_; }
  // The models the host can build (empty: all of the car list).
  void set_available(std::vector<int64_t> models);

  // One tick: the player's position and velocity (world), dt in s. The
  // first call places the initial cars.
  void step(double dt, const P3& player, const P3& player_velocity);

  const std::vector<Car>& cars() const { return cars_; }
  std::vector<Car>& cars() { return cars_; }
  // Moves a car in full physics to its body's pose: finds its place on its
  // road (or the road it has reached) and whether it is back on its line.
  void follow(size_t index, const P3& body, double heading, double speed);
  // A full-physics car's wanted heading change and speed (the host turns
  // them into inputs): the point `ahead` m along its lane.
  P3 lane_point(const Car& car, double ahead) const;

  const std::vector<int64_t>& loaded() const { return loaded_; }
  double local_density() const { return smoothed_; }
  double target() const { return target_; }
  double factor() const { return time_factor(hour_, config_); }
  const PopulationStats& stats() const { return stats_; }
  double despawn_distance(const Car& car) const;
  double time() const { return time_; }

 private:
  const DensityEntry* entry(const Road& r) const { return r.traffic ? config_.density(r.density) : nullptr; }
  double road_speed(const Road& r) const;
  void measure(const P3& player);
  void rotate_models();
  bool spawn(const P3& player, double player_speed, bool initial);
  bool place(Car& car, int road, double s, int dir);
  void drive(Car& car, double dt, const P3& player, const P3& player_velocity);
  void advance(Car& car, double distance) const;
  void pose(Car& car) const;
  int64_t pick_model(const DensityEntry& e);
  double gap_needed(double player_speed) const;
  double uniform(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng_); }

  const Roads& roads_;
  TrafficConfig config_;
  std::mt19937_64 rng_;
  GroundHeight ground_;
  double hour_ = 12;
  std::vector<int64_t> available_;
  std::vector<int64_t> loaded_;
  std::vector<Car> cars_;
  uint32_t next_id_ = 1;
  bool started_ = false;
  bool bodies_ = false;
  double time_ = 0;
  double smoothed_ = 0, target_ = 0;
  double next_test_ = 0, next_spawn_ = 0, next_rotation_ = 0;
  // The traffic road lengths within the spawn-out distance (by density id),
  // measured again once the player has moved 10 m.
  std::map<int, double> within_;
  P3 within_at_{1e30, 0, 1e30};
  PopulationStats stats_;
};

}  // namespace traffic
