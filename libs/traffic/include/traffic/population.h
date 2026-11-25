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
#include <limits>
#include <random>
#include <span>
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
  int lane_index = 0;       // 0: right/normal lane (+2.2 m), 1: fast/left lane (-1.8 m on multi-lane freeways)
  double lateral = 2.2;     // m right of the centre line, in its direction of travel
  double target_lateral = 2.2;
  double avoid_offset = 0;  // lateral swerve away from an encroaching player (m)
  double speed = 0, target = 0;  // m/s
  double base_speed = 0;    // the road's speed with this car's variance
  double accel = 0;         // longitudinal acceleration (m/s^2) for brake light threshold
  int gear = 1;             // simulated gear (1..5) from GearMaxSpeed1st..4th or physics body
  double rpm = 900.0;       // simulated engine RPM from GearMaxSpeed1st..4th or physics body
  int last_node = -1;       // the node it last passed (no turning back there)
  // The pose on the lane: the body frame's origin (ground height when the
  // host gives one) and the heading (atan2 of forward x, z).
  P3 position;
  double heading = 0, pitch = 0;
  double steer_angle = 0;   // front road-wheel steer angle (rad)
  double yaw_rate = 0;      // body yaw rate (rad/s)
  bool pose_initialized = false;
  bool physics = false;     // in full physics (within the high-LOD distance, or knocked off its line)
  bool off_line = false;    // knocked off its lane (stays in physics until back on it)
  bool body = false;        // the host has a body for it (bodies.h): it is not placed on the lane
  double age = 0;           // s since spawning
  float alpha = 1.0f;       // spawn/despawn fade-in/out opacity [0, 1] (TRAFFIC.md:269)
  // Multi-lane & collision-avoidance state
  double blocked_time = 0;
  double since_lane_change = 10.0;
  // Lamp / indicator / flash state (hooked into Slice 3 CarLampState)
  bool turn_left = false;
  bool turn_right = false;
  bool hazard_on = false;
  bool flash_headlamps = false;
  double indicator_hold = 0;
  double flash_timer = 0;
  double flash_cooldown = 0;
  // DeadBehavior recovery / reanimation
  double dead_timer = 0;
  double stopped_dead_timer = 0;
  double up_y = 1.0;
};

struct ExternalVehicle {
  P3 position{};
  P3 velocity{};
  bool race_ai = true;  // promotes nearby traffic to physics bodies so Race AI >30 m from player never ghosts through
};

// Simulates traffic gear (1..5) and engine RPM from speed and GearMaxSpeed1st..4th.
void estimate_traffic_gear_rpm(double speed, const std::array<double, 4>& gear_max_speed, int& gear_out,
                               double& rpm_out);

struct PopulationStats {
  int spawned = 0, despawned = 0, initial = 0, overspawn_culled = 0;
  std::vector<double> spawn_distances, despawn_distances;
  int spawned_away = 0;
  int spawned_behind = 0;
  int convoys_spawned = 0;
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
  // first call places the initial cars. Optional `player_heading` (rad;
  // NaN derives heading from velocity when moving) gates behind-player
  // spawns via BehindChance / BehindMaxSpeed. Optional `obstacles` (e.g.
  // Race AI opponents in Street/Nemesis races) cause traffic to brake,
  // yield at junctions, and promote to full physics near them.
  void step(double dt, const P3& player, const P3& player_velocity,
            double player_heading = std::numeric_limits<double>::quiet_NaN(),
            std::span<const ExternalVehicle> obstacles = {});

  // Adds a car on a road (its id given here), placed on its lane.
  void add(Car car, int road, double s, int dir);
  const std::vector<Car>& cars() const { return cars_; }
  std::vector<Car>& cars() { return cars_; }
  // Moves a car in full physics to its body's pose: finds its place on its
  // road (or the road it has reached) and whether it is back on its line.
  void follow(size_t index, const P3& body, double heading, double speed, double up_y = 1.0);
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
  // Current traffic car id holding right-of-way at junction `node` (0 if unreserved).
  uint32_t junction_holder(uint32_t node) const;

  struct JunctionReservation {
    uint32_t holder_id = 0;
    int holder_road = -1;
    int holder_dir = 1;
    P3 junction_pos{};
    double reserved_at = 0.0;
    double expires_at = 0.0;
  };

  struct Snapshot {
    std::vector<Car> cars;
    uint32_t next_id = 1;
    bool started = false;
    double time = 0.0;
    double smoothed = 0.0, target = 0.0;
    double next_test = 0.0, next_spawn = 0.0, next_rotation = 0.0, next_overspawn = 0.0;
    std::map<uint32_t, JunctionReservation> junction_reservations;
  };
  Snapshot snapshot() const {
    return {cars_, next_id_, started_, time_, smoothed_, target_, next_test_, next_spawn_, next_rotation_,
            next_overspawn_, junction_reservations_};
  }
  void restore(const Snapshot& s) {
    cars_ = s.cars;
    next_id_ = s.next_id;
    started_ = s.started;
    time_ = s.time;
    smoothed_ = s.smoothed;
    target_ = s.target;
    next_test_ = s.next_test;
    next_spawn_ = s.next_spawn;
    next_rotation_ = s.next_rotation;
    next_overspawn_ = s.next_overspawn;
    junction_reservations_ = s.junction_reservations;
  }

 private:
  const DensityEntry* entry(const Road& r) const { return r.traffic ? config_.density(r.density) : nullptr; }
  bool is_multilane(const Road& r) const;
  double lane_center(const Road& r, int lane_index) const;
  double road_speed(const Road& r) const;
  void measure(const P3& player);
  void rotate_models();
  void check_overspawn(const P3& player);
  double spawn_interval(double player_speed);
  bool spawn(const P3& player, double player_speed, bool initial, bool has_heading, double fwd_x, double fwd_z);
  bool place(Car& car, int road, double s, int dir, int lane_index = 0);
  struct JunctionOption {
    int road = -1, index = 0, dir = 1;
  };
  bool pick_junction_option(const Car& car, uint32_t node, const std::array<double, 2>& in_tangent,
                            JunctionOption& out) const;
  bool next_junction(const Car& car, double max_dist, uint32_t& node_out, double& dist_out, P3& pos_out,
                     double& turn_angle_out) const;
  void drive(Car& car, double dt, const P3& player, const P3& player_velocity,
             std::span<const ExternalVehicle> obstacles);
  void update_junction_reservations();
  void sync_cursor(Car& car, double max_advance) const;
  void steer_kinematic(Car& car, const P3& aim, double dt) const;
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
  double next_test_ = 0, next_spawn_ = 0, next_rotation_ = 0, next_overspawn_ = 0;
  // The traffic road lengths within the spawn-out distance (by density id),
  // measured again once the player has moved 10 m.
  std::map<int, double> within_;
  P3 within_at_{1e30, 0, 1e30};
  std::map<uint32_t, JunctionReservation> junction_reservations_;
  PopulationStats stats_;
};

}  // namespace traffic
