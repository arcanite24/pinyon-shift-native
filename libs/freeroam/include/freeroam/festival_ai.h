// Free-roam Festival AI Racers (AIOpenWorld.xml cartype="festival") and
// instant Head-to-Head point-to-point races on the Colorado nav road graph.
//
// Roaming Festival AI cars cruise Colorado's roads using the `festival`
// DensityList (2..7 active cars, sports/performance roster matched to the
// player's class). Pulling behind one within 35 m enables the instant
// Head-to-Head challenge prompt; accepting builds a 2.0-3.5 km nav-graph
// point-to-point route with a credit wager scaled by car class & wristband,
// races the opponent to the finish gate, and awards credits + popularity.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "traffic/config.h"
#include "traffic/nav.h"
#include "traffic/roads.h"

namespace freeroam {

struct FestivalRacerSpec {
  int64_t car_id = 0;
  std::string car_media;
  std::string driver_name;
  std::string car_class;
  int pi = 450;
};

struct FestivalAiCar {
  uint32_t id = 0;
  FestivalRacerSpec spec;
  int road = -1;
  int dir = 1;
  double s = 0.0;
  double lateral = 2.2;
  double speed = 0.0;
  double base_speed = 24.0;
  traffic::P3 position{};
  double heading = 0.0, pitch = 0.0;
  bool in_h2h = false;
  double h2h_progress = 0.0;
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

// Computes credit wager for a Head-to-Head challenge by car PI and wristband tier (0..6).
int64_t h2h_wager_credits(int pi, int wristband_level);

class FestivalManager {
 public:
  using GroundHeight = std::function<bool(double x, double z, double y_hint, double& y)>;

  FestivalManager(const traffic::NavMap& nav, const traffic::Roads& roads, const traffic::TrafficConfig& config,
                  uint64_t seed = 42);
  void set_ground(GroundHeight g) { ground_ = std::move(g); }
  void set_roster(std::vector<FestivalRacerSpec> roster);
  void set_hour(double hour) { hour_ = hour; }
  void set_wristband_level(int level) { wristband_level_ = level; }

  // Advances roaming Festival AI cars and any active Head-to-Head race.
  // Returns a completed HeadToHeadResult on the exact step the race finishes.
  bool step(double dt, double time, const traffic::P3& player_pos, double player_heading, double player_speed,
            HeadToHeadResult* finished_out = nullptr);

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

 private:
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
  std::vector<FestivalRacerSpec> roster_;
  std::vector<FestivalAiCar> cars_;
  uint32_t next_id_ = 1;
  double hour_ = 12.0;
  int wristband_level_ = 0;
  bool started_ = false;
  double next_spawn_ = 0.0;

  // Proximity prompt state
  uint32_t prompt_id_ = 0;
  double prompt_dist_ = 1e9;

  // Active Head-to-Head state
  bool h2h_active_ = false;
  uint32_t h2h_car_id_ = 0;
  int64_t h2h_wager_ = 2000;
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
