// One race's flow (SPEC-EVENTS, Race flow; Laps, places and the clock;
// Wrong way, resets and leaving the track): the countdown to the start
// signal, each car's start-line crossings (a circuit finishes on its
// NumLaps + 1th) or the sprint finish, interpolated within the step, race
// times and lap times, places, and the interim automatic resets. The
// session reads one sample per car a step and holds no car state of its
// own beyond the race's.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "drive/ai_driver.h"
#include "drive/vehicle.h"
#include "race/reset_rules.h"
#include "race/route.h"

namespace race {

// Grid slot of the player for PlayerGridPosition p (1..8 -> p - 1; 0, or
// past the grid: the last of `cars`), and each opponent's slot: the rows
// in order fill the other slots from pole back (interim).
int player_slot(int player_grid_position, int cars);
std::vector<int> opponent_slots(int player_slot, int opponents);
// SPEC-RACE-AI, AI car upgrades step 1: the faked PI's offset r from the
// car's Ordinal (one LCG step, k = bits 16..30 mod 5; +k odd, -k even:
// 0, +1, -2, +3, -4), +5 on a flagged participant.
int faked_pi_offset(int64_t ordinal, bool flagged = false);

// SPEC-RACE-AI, AI car upgrades step 2: six scales moved together by signed
// fraction t in [-1, 1] (up: mass 1-0.1t, torque 1+0.4t, friction 1+0.20t;
// down: mass 1+0.111|t|, torque 1-0.286|t|, friction 1-0.167|t|), solved via
// 10-iteration bisection (`AIAutoUpgrades\Iterations = 10`).
struct AiUpgradeScales {
  double t = 0.0;
  double mass_scale = 1.0;
  double torque_scale = 1.0;
  double friction_scale = 1.0;
  double rated_pi = 0.0;
};
AiUpgradeScales ai_upgrade_scales(double t);
void apply_ai_upgrade(drive::CarParams& params, double t, int tuning_level = 0);
AiUpgradeScales solve_ai_upgrade(const drive::CarParams& stock, double stock_pi, double target_pi,
                                 const std::function<double(const drive::CarParams&)>& rate_pi, int iterations = 10);

struct CarSample {
  double x0 = 0, z0 = 0, x1 = 0, z1 = 0;  // the body reference point before and after the step
  double speed = 0;                       // m/s along the car's forward
  double heading_cos = 1;                 // cos(car forward, the line's direction)
  double off_route = 0;                   // m beyond the road's edge (0 on the road)
  double progress = 0;                    // m along the shared line
  double up_y = 1.0;                      // body up-axis Y in world space (< 0.5 = rolled over)
};

class Session {
 public:
  struct Options {
    double countdown = 6.0;           // s from the cutscene's start to the start signal
    bool resets = true;
    double rollover_time = 2.0;       // s with up_y < 0.5 before triggering rollover recovery
    double off_route_near = 5.0;      // m off route for 6 s timer (widened to 12.0 m in Street/Nemesis)
    double off_route_far = 15.0;      // m off route for 3 s timer (widened to 25.0 m in Street/Nemesis)
    double start_launch_grace = 25.0; // s after start signal before resetting an unlaunched player
    size_t player_index = size_t(-1); // car index given start-line launch grace (size_t(-1): none)
    // The reset's timings, GameTunableSettings.ini [HandOfGodReset] (set
    // them from the file with apply_hand_of_god; these are its values):
    double reset_fade_out = 2.0;          // FadeOutTime: the fade leading into the reset
    double reset_fade_in = 1.0;           // FadeInTime: the fade after reset_done
    double post_reset_non_collide = 3.0;  // PostResetNonCollideTime: no car contact after reset_done
    double collisions_off = 2.0;          // CollisionsOffTime: no car contact leading into the reset
  };
  struct Car {
    int crossings = 0;
    double finish = -1;      // race time, or -1
    double last_cross = -1;  // race time of the last start-line crossing
    double best_lap = -1;
    std::vector<double> laps;
    int place = 0;
    int car_class = 0;       // multi-class bucket (CarClasses.Id; 0 = single-class default)
    int class_place = 0;     // 1-based standing within car_class
    double wrong = 0, slow = 0, off5 = 0, off15 = 0, rolled = 0;  // reset timers, s
    double no_collide_until = -1e9;
    const char* reason = "";  // the last reset's condition
    double distance = 0;      // m driven from the signal to the finish (horizontal)
    bool finish_held = false;  // a checkpoint race's earlier gate not passed yet
    bool lap_dirty = false;    // current lap invalidated (e.g. by Rewind)
    bool race_dirty = false;   // any lap in this race invalidated (e.g. by Rewind)
    bool has_launched = false;       // true once car has moved off the grid after signal
    double reset_fade_out = 0.0;     // [0..1] fade-out ramp before Hand-of-God trigger
    double reset_fade_in_timer = 0.0;// s remaining in post-reset fade-in
    bool collisions_off = false;     // within CollisionsOffTime of a pending reset
  };
  struct Snapshot {
    std::vector<Car> cars;
    std::vector<double> progress;
    std::vector<size_t> resets;
    double clock = 0.0;
  };

  // A circuit counts crossings of `start` (laps + 1 finish); a sprint
  // finishes on `finish`.
  Session(size_t cars, bool circuit, int laps, const Gate& start, const std::optional<Gate>& finish, const Options& o);
  Session(size_t cars, bool circuit, int laps, const Gate& start, const std::optional<Gate>& finish)
      : Session(cars, circuit, laps, start, finish, Options{}) {}

  // Advances the clock by dt, then reads every car's move over the step.
  void step(double dt, std::span<const CarSample> cars);
  double clock() const { return clock_; }  // race time; negative before the signal
  bool started() const { return clock_ >= 0; }
  // The countdown's shown digit: 3, 2, 1 from 3, 2 and 1 s before the
  // signal, else 0; the chase camera and HUD from 3 s before it.
  int digit() const;
  bool circuit() const { return circuit_; }
  int laps() const { return laps_; }
  size_t size() const { return cars_.size(); }
  const Car& car(size_t i) const { return cars_[i]; }
  // The car's lap on the HUD (0 on the grid, at most laps).
  int lap(size_t i) const;
  bool finished(size_t i) const { return cars_[i].finish >= 0; }
  bool all_finished() const;
  // Cars whose reset condition held long enough this step; the caller
  // puts each back on the line and calls reset_done.
  const std::vector<size_t>& resets() const { return resets_; }
  void reset_done(size_t i);
  // A checkpoint race (SPEC-EVENTS): the finish does not count for car i
  // while held (an earlier checkpoint not passed).
  void hold_finish(size_t i, bool held) { cars_[i].finish_held = held; }
  // Car-to-car contact: off for PostResetNonCollideTime after reset_done
  // and for the CollisionsOffTime leading into a reset.
  bool collides(size_t i) const { return clock_ >= cars_[i].no_collide_until && !cars_[i].collisions_off; }
  // Visibility factor [0, 1] across Hand-of-God 2 s fade-out and 1 s post-reset fade-in.
  double reset_fade_alpha(size_t i) const;
  // Marks car i's current lap & race uncertified (SPEC-REWIND section 5).
  void mark_lap_dirty(size_t i);
  // Full state snapshot/restore for Rewind rollback.
  Snapshot snapshot() const;
  void restore(const Snapshot& s);
  // Car indices from first to last place.
  std::vector<size_t> order() const;
  // Multi-class standings helpers (for events with mixed car classes).
  void set_car_class(size_t i, int car_class);
  void set_car_classes(std::span<const int> classes);
  int car_class(size_t i) const;
  int class_place(size_t car_idx) const;
  std::vector<size_t> class_order(int car_class) const;
  bool is_multi_class() const;

 private:
  void update_class_places(const std::vector<size_t>& ord);
  std::vector<Car> cars_;
  std::vector<double> progress_;
  std::vector<size_t> resets_;
  bool circuit_;
  int laps_;
  Gate start_;
  std::optional<Gate> finish_;
  Options o_;
  double clock_;
};

// The [HandOfGodReset] block into the session's reset options.
void apply_hand_of_god(const HandOfGodReset& h, Session::Options& o);

// SPEC-RACE-AI, Traffic between cars (interim): each AI car keeps its line
// while the car ahead is further than its trailing distance; closer and
// faster, it moves to the side with more room by up to half the road's
// half width plus the car clearance (never past the half width minus the
// edge clearance), and returns once past. `lane` (m right of the line) is
// moved at most `rate` m/s. Cars [0, ai) are steered; every car counts as
// one to pass.
struct LaneParams {
  float trailing = 7, car_clearance = 0.3f, edge_clearance = 0.4f;
  float aggression = 0.5f;  // AITemperaments.Passing_AggressionLevel (0..1; higher = more willing to pass)
};
// A live street-traffic car projected onto the racing line for race AI
// overtaking and collision avoidance in Street & Nemesis races.
struct TrafficObstacle {
  double progress = 0;  // m along the shared racing line
  float cross = 0;      // m right of the line
  float speed = 0;      // m/s magnitude
  bool oncoming = false;
};
// `cap` (when not empty): each steered car's speed cap, m/s (ours): while
// a slower car ahead within the trailing distance is still in its way, no
// faster than that car plus half the gap beyond a car's length; else 1e9.
void plan_lanes(const drive::RacingLine& line, std::span<const double> progress, std::span<const float> cross,
                std::span<const float> speed, std::span<const LaneParams> params, size_t ai, std::span<float> lane,
                double dt, double rate = 1.5, std::span<float> cap = {},
                std::span<const TrafficObstacle> traffic = {});

}  // namespace race
