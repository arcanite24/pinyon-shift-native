// Free-roam Festival AI Racers (AIOpenWorld.xml cartype="festival") and
// instant Head-to-Head point-to-point races on the Colorado nav road graph.
//
// Roaming Festival AI cars cruise Colorado's roads using the `festival`
// DensityList (2..7 active cars, curated FreeRoamDrivers roster matched to the
// player's class). Pulling behind one inside the retail activation box (5-20 m
// ahead, +/-2.5 m lateral, same heading, <=15 mph speed diff, target >=25 mph)
// enables the instant Head-to-Head challenge prompt; accepting builds a
// 2.5-5.0 km (optimal 3.5 km) one-way-respecting nav-graph point-to-point
// route, races the opponent to the 10 m radius finish gate, and awards
// PI-differential credits + popularity (without deducting credits on loss).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "traffic/config.h"
#include "traffic/nav.h"
#include "traffic/population.h"
#include "traffic/roads.h"

namespace freeroam {

struct FestivalRacerSpec {
  int64_t car_id = 0;
  std::string car_media;
  std::string driver_name;
  std::string car_class;
  int pi = 450;
  double mass_kg = 1480.0;
};

struct FestivalAiCar {
  uint32_t id = 0;
  FestivalRacerSpec spec;
  int road = -1;
  int dir = 1;
  double s = 0.0;
  double lateral = 2.2;
  double target_lateral = 2.2;
  double speed = 0.0;
  double base_speed = 24.0;
  traffic::P3 position{};
  double vx = 0.0, vy = 0.0, vz = 0.0;
  double heading = 0.0, pitch = 0.0;
  double steer_angle = 0.0;
  double yaw_rate = 0.0;
  bool pose_initialized = false;
  double rpm = 2200.0;
  bool braking = false;
  bool turn_left = false;
  bool turn_right = false;
  bool reversing = false;
  double knockback_vx = 0.0, knockback_vz = 0.0;
  double roll = 0.0;                 // suspension body roll reaction (rad)
  double pitch_impulse = 0.0;        // transient suspension impact pitch reaction (rad)
  double spin_yaw_rate = 0.0;        // collision-imparted angular spin velocity (rad/s)
  double off_road_recovery = 0.0;    // smooth road-recovery steering timer after bump/impact (s)
  double stuck_timer = 0.0;          // accumulated wedged/stalled time (s)
  double ghost_timer = 0.0;          // 3.0 s post-crash / stuck ghosting timer (s)
  bool in_h2h = false;
  double h2h_progress = 0.0;
  bool ghosted() const { return ghost_timer > 0.0; }
};

struct FestivalCollisionImpulse {
  bool hit = false;
  uint32_t car_id = 0;
  std::array<double, 3> contact_point{};
  std::array<double, 3> player_impulse{};  // N*s applied to player body
  double impulse_magnitude = 0.0;
};

struct HeadToHeadResult {
  bool won = false;
  std::string opponent_name;
  int64_t wager_credits = 0;
  int64_t payout_credits = 0;
  int popularity_points = 0;
  int driver_xp = 0;
  double elapsed_s = 0.0;
};

struct HeadToHeadHud {
  // Pre-challenge proximity prompt (when behind a Festival AI racer < 35 m)
  bool prompt_visible = false;
  uint32_t prompt_car_id = 0;
  std::string prompt_driver;
  std::string prompt_car_class;
  int prompt_pi = 0;
  int64_t prompt_wager = 0;
  double prompt_distance_m = 0.0;
  // Active Head-to-Head race
  bool active = false;
  std::string opponent_name;
  std::string opponent_class;
  int opponent_pi = 0;
  int64_t wager_credits = 0;
  int place = 1;                 // 1 or 2
  double route_length_m = 0.0;
  double player_remaining_m = 0.0;
  double gap_m = 0.0;            // + when player is ahead
  double elapsed_s = 0.0;
  double progress = 0.0;         // [0..1]
  // Recent finish banner
  bool finish_banner = false;
  bool finish_won = false;
  int64_t finish_credits = 0;
  std::string finish_opponent;
  double finish_at = -1e9;
};

// Computes credit reward / wager for a Head-to-Head challenge using retail PI-differential
// and difficulty scaling:
// RewardBaseLevel = 5000, RewardPlayerLowerMultiplier = 20 (player PI < opponent PI),
// RewardPlayerHigherMultiplier = 10 (player PI > opponent PI), scaled by difficulty_scale.
int64_t h2h_reward_credits(int player_pi, int opponent_pi, double difficulty_scale = 1.0);

// Exact 1:1 alias for the offered H2H wager displayed to the player (matches payout 1:1).
int64_t h2h_wager_credits(int player_pi, int opponent_pi, double difficulty_scale = 1.0);

class FestivalManager {
 public:
  using GroundHeight = std::function<bool(double x, double z, double y_hint, double& y)>;

  // Retail Activity/RaceEncounter trigger-box & routing constants.
  static constexpr double kMinActivationDistance = 5.0;       // m
  static constexpr double kMaxActivationDistance = 20.0;      // m
  static constexpr double kActivationBoxHalfWidth = 2.5;      // m (ActivationBoxWidth = 5 m)
  static constexpr double kMinHeadingDot = 0.85;              // same driving direction
  static constexpr double kMaxSpeedDifference = 6.71;         // m/s (15 mph)
  static constexpr double kMinTargetCarSpeed = 11.18;         // m/s (25 mph)
  static constexpr double kH2hMinLength = 2500.0;             // m
  static constexpr double kH2hOptimalLength = 3500.0;         // m
  static constexpr double kH2hMaxLength = 5000.0;             // m
  static constexpr double kH2hFinishTriggerRadius = 10.0;     // m (EndPointTriggerDiameter = 20 m)
  static constexpr double kH2hTurnDelaySeconds = 20.0;        // s
  static constexpr double kH2hOneWayMultiplier = 999.0;       // wrong-way penalty multiplier
  static constexpr double kCollisionGhostDuration = 3.0;      // s post-crash / stuck ghosting
  static constexpr double kNearPhysicsDistance = 80.0;        // m nearby collision realism zone

  FestivalManager(const traffic::NavMap& nav, const traffic::Roads& roads, const traffic::TrafficConfig& config,
                  uint64_t seed = 42);
  ~FestivalManager();
  void set_ground(GroundHeight g) { ground_ = std::move(g); }
  void set_roster(std::vector<FestivalRacerSpec> roster);
  // Populates fallback driver names from gamedb.slt AIPlayers before/after set_roster.
  void set_driver_names(std::vector<std::string> names);
  void set_hour(double hour) { hour_ = hour; }
  void set_wristband_level(int level) { wristband_level_ = level; }
  void set_player_pi(int pi) { player_pi_ = std::clamp(pi, 100, 999); }
  int player_pi() const { return player_pi_; }
  void set_difficulty_scale(double scale) { difficulty_scale_ = std::clamp(scale, 0.5, 2.5); }
  double difficulty_scale() const { return difficulty_scale_; }

  // Advances roaming Festival AI cars and any active Head-to-Head race with
  // corner curvature braking, traffic/player overtaking avoidance, and velocity/RPM telemetry.
  // Returns a completed HeadToHeadResult on the exact step the race finishes.
  bool step(double dt, double time, const traffic::P3& player_pos, double player_heading, double player_speed,
            HeadToHeadResult* finished_out = nullptr, const std::vector<traffic::Car>* traffic_cars = nullptr);

  // Resolves OBB/capsule car-to-car collision between the player vehicle and nearby
  // Festival AI racers, updating AI knockback velocity and returning impulse on player.
  FestivalCollisionImpulse resolve_player_collision(const traffic::P3& player_pos,
                                                    const std::array<double, 3>& player_vel,
                                                    double player_mass_kg, double dt);

  // Forces a roaming Festival AI car near the player on the road ahead (for demos/tests).
  uint32_t spawn_ahead(const traffic::P3& player_pos, double player_heading, double distance_m = 22.0);

  // Starts an instant Head-to-Head race against the nearest promptable Festival AI
  // (or `car_id` if non-zero). Builds a 2.0-3.5 km nav-graph route ahead.
  bool start_head_to_head(const traffic::P3& player_pos, double player_heading, double time, uint32_t car_id = 0);
  void cancel_head_to_head();

  bool h2h_active() const { return h2h_active_; }
  const std::vector<FestivalAiCar>& cars() const { return cars_; }
  const std::vector<std::array<double, 2>>& h2h_route() const { return h2h_route_2d_; }
  std::array<double, 3> h2h_finish_pos() const { return h2h_finish_; }
  HeadToHeadHud hud(double time) const;

  struct RewindSnapshot {
    std::vector<FestivalAiCar> cars;
    uint32_t next_id = 1;
    bool started = false;
    double next_spawn = 0.0;
    uint32_t prompt_id = 0;
    double prompt_dist = 1e9;
    bool h2h_active = false;
    uint32_t h2h_car_id = 0;
    int64_t h2h_wager = 5000;
    double h2h_elapsed = 0.0;
    double h2h_player_s = 0.0;
    double h2h_length = 0.0;
    std::vector<traffic::P3> h2h_pts;
    std::vector<double> h2h_s;
    std::vector<std::array<double, 2>> h2h_route_2d;
    std::array<double, 3> h2h_finish{};
  };
  RewindSnapshot capture_rewind() const {
    return {cars_, next_id_, started_, next_spawn_, prompt_id_, prompt_dist_, h2h_active_, h2h_car_id_, h2h_wager_,
            h2h_elapsed_, h2h_player_s_, h2h_length_, h2h_pts_, h2h_s_, h2h_route_2d_, h2h_finish_};
  }
  void restore_rewind(const RewindSnapshot& s) {
    cars_ = s.cars;
    next_id_ = s.next_id;
    started_ = s.started;
    next_spawn_ = s.next_spawn;
    prompt_id_ = s.prompt_id;
    prompt_dist_ = s.prompt_dist;
    h2h_active_ = s.h2h_active;
    h2h_car_id_ = s.h2h_car_id;
    h2h_wager_ = s.h2h_wager;
    h2h_elapsed_ = s.h2h_elapsed;
    h2h_player_s_ = s.h2h_player_s;
    h2h_length_ = s.h2h_length;
    h2h_pts_ = s.h2h_pts;
    h2h_s_ = s.h2h_s;
    h2h_route_2d_ = s.h2h_route_2d;
    h2h_finish_ = s.h2h_finish;
  }

 private:
  struct BranchOption {
    int road = -1;
    int idx = 0;
    int dir = 1;
  };
  bool pick_roaming_branch(uint32_t car_id, int cur_road, int cur_dir, uint32_t node,
                           const std::array<double, 2>& in_tangent, BranchOption& out) const;
  bool sample_roaming_ahead(const FestivalAiCar& car, double dist, double lateral, traffic::P3& pos_out,
                            std::array<double, 2>& tangent_out) const;
  void sync_roaming_cursor(FestivalAiCar& car, double max_advance) const;
  void steer_towards(FestivalAiCar& car, const traffic::P3& aim, double dt, double max_lat_g) const;
  bool spawn_roaming(const traffic::P3& player, bool initial);
  void advance_roaming(FestivalAiCar& car, double dist) const;
  void pose_roaming(FestivalAiCar& car) const;
  bool build_h2h_route(const traffic::P3& start, double heading, double min_len, double max_len);
  double project_on_h2h(double x, double z, double hint_s) const;
  void pose_on_h2h(double s, double lateral, traffic::P3& pos, double& heading, double& pitch) const;

  const traffic::NavMap& nav_;
  const traffic::Roads& roads_;
  traffic::TrafficConfig config_;
  std::mt19937_64 rng_;
  GroundHeight ground_;
  std::vector<std::string> driver_names_;
  std::vector<FestivalRacerSpec> roster_;
  std::vector<FestivalAiCar> cars_;
  uint32_t next_id_ = 1;
  double hour_ = 12.0;
  int wristband_level_ = 0;
  int player_pi_ = 500;
  double difficulty_scale_ = 1.0;
  bool started_ = false;
  double next_spawn_ = 0.0;

  // Proximity prompt state
  uint32_t prompt_id_ = 0;
  double prompt_dist_ = 1e9;

  // Active Head-to-Head state
  bool h2h_active_ = false;
  uint32_t h2h_car_id_ = 0;
  int64_t h2h_wager_ = 5000;
  double h2h_elapsed_ = 0.0;
  double h2h_player_s_ = 0.0;
  double h2h_length_ = 0.0;
  std::vector<traffic::P3> h2h_pts_;
  std::vector<double> h2h_s_;
  std::vector<std::array<double, 2>> h2h_route_2d_;
  std::array<double, 3> h2h_finish_{};

  // Recent finish banner
  bool last_won_ = false;
  int64_t last_payout_ = 0;
  std::string last_opp_;
  double finish_banner_at_ = -1e9;
};

}  // namespace freeroam
