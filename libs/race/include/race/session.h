// One race's flow (SPEC-EVENTS, Race flow; Laps, places and the clock;
// Wrong way, resets and leaving the track): the countdown to the start
// signal, each car's start-line crossings (a circuit finishes on its
// NumLaps + 1th) or the sprint finish, interpolated within the step, race
// times and lap times, places, and the interim automatic resets. The
// session reads one sample per car a step and holds no car state of its
// own beyond the race's.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "drive/ai_driver.h"
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

struct CarSample {
  double x0 = 0, z0 = 0, x1 = 0, z1 = 0;  // the body reference point before and after the step
  double speed = 0;                       // m/s along the car's forward
  double heading_cos = 1;                 // cos(car forward, the line's direction)
  double off_route = 0;                   // m beyond the road's edge (0 on the road)
  double progress = 0;                    // m along the shared line
};

class Session {
 public:
  struct Options {
    double countdown = 6.0;  // s from the cutscene's start to the start signal
    bool resets = true;
  };
  struct Car {
    int crossings = 0;
    double finish = -1;      // race time, or -1
    double last_cross = -1;  // race time of the last start-line crossing
    double best_lap = -1;
    std::vector<double> laps;
    int place = 0;
    double wrong = 0, slow = 0, off5 = 0, off15 = 0;  // reset timers, s
    double no_collide_until = -1e9;
    const char* reason = "";  // the last reset's condition
    double distance = 0;      // m driven from the signal to the finish (horizontal)
    bool finish_held = false;  // a checkpoint race's earlier gate not passed yet
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
  bool collides(size_t i) const { return clock_ >= cars_[i].no_collide_until; }
  // Car indices from first to last place.
  std::vector<size_t> order() const;

 private:
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

// SPEC-RACE-AI, Traffic between cars (interim): each AI car keeps its line
// while the car ahead is further than its trailing distance; closer and
// faster, it moves to the side with more room by up to half the road's
// half width plus the car clearance (never past the half width minus the
// edge clearance), and returns once past. `lane` (m right of the line) is
// moved at most `rate` m/s. Cars [0, ai) are steered; every car counts as
// one to pass.
struct LaneParams {
  float trailing = 7, car_clearance = 0.3f, edge_clearance = 0.4f;
};
// `cap` (when not empty): each steered car's speed cap, m/s (ours): while
// a slower car ahead within the trailing distance is still in its way, no
// faster than that car plus half the gap beyond a car's length; else 1e9.
void plan_lanes(const drive::RacingLine& line, std::span<const double> progress, std::span<const float> cross,
                std::span<const float> speed, std::span<const LaneParams> params, size_t ai, std::span<float> lane,
                double dt, double rate = 1.5, std::span<float> cap = {});

}  // namespace race
