// SPEC-RACE-AI, Difficulty and rubber-banding: each opponent's skill,
// temperament and rubber-band row, its rolled braking and cornering, the
// torque cut while it leads the player, catch-up while it trails, and the
// race-start torque boost by grid place.
#pragma once

#include <cstdint>

#include "race/event.h"

namespace race {

struct DriverSetup {
  int skill = 50, temperament = 5, rubberband_id = 0;
};
// Steps 1-3 of the spec: the event's values plus the driver's modifiers,
// skill clamped to 1-80 and temperament to 1-10; the rubber-band id is the
// event's plus the driver's modifier when it is 1 to 10, otherwise the
// skill row's Rubberbands_id.
DriverSetup assign_driver(int event_skill, int event_temperament, int event_rubberband, int skill_modifier,
                          int aggro_modifier, int rubberband_modifier, const EventData& rows);

// The torque factor in torque-cut mode: 1 - r x TorqueCutFactor, r linear
// from 0 at StartTorqueCutDistance to 1 at MaxTorqueCutDistance (clamped).
double torque_cut(const RubberbandRow& row, double gap);
// The race-start boost (probable): 0.4 on pole down to 0.1 on the last
// slot, held for the first 300 m from the slot, gone over the next 700 m;
// the torque is x (1 + boost).
double start_boost(int slot, int cars, double distance_from_slot);

// The AI's acceleration limits from its rolled values (the spec's fits):
// corner a_c = cornering x 1.7 g, braking = braking x 2.0 g.
constexpr double kGravity = 9.81;
inline double corner_accel(double cornering) { return cornering * 1.7 * kGravity; }
inline double brake_decel(double braking) { return braking * 2.0 * kGravity; }

// One opponent's rubber-band state, updated every step.
class DriverAi {
 public:
  enum class State { kNormal, kMaxPerformance, kRubberband, kCatchUp };
  // `row` null: the built-in values (rubber band from 100 m with the
  // built-in mode choices, no torque cut, no catch-up). `enabled` false: the "no rubber-banding" option.
  void init(const SkillRow& skill, const RubberbandRow* row, uint64_t seed, bool enabled = true);
  // gap: metres along the route, positive while this car is ahead of the
  // player; places_behind: this car's place minus the player's.
  // dt: the time since the last update (the over- modes' hold).
  void update(double gap, int places_behind, double dt = 1.0 / 120.0);

  double braking() const { return braking_; }
  double cornering() const { return cornering_; }
  double torque() const { return torque_; }  // engine torque factor (cut x nominal)
  State state() const { return state_; }
  int mode() const { return state_ == State::kRubberband && torque_ < skill_.nominal_torque_scale ? 8 : mode_; }
  double amount() const { return amount_; }
  int rolls() const { return rolls_; }

 private:
  double uniform();
  void roll();
  void draw_mode();
  SkillRow skill_;
  RubberbandRow row_;
  bool has_row_ = false, enabled_ = true;
  uint64_t rng_ = 0;
  double base_braking_ = 1, base_cornering_ = 1;
  double braking_ = 1, cornering_ = 1, torque_ = 1;
  State state_ = State::kNormal;
  int mode_ = 0, rolls_ = 0;
  bool catch_up_ = false, max_ = false;
  double amount_ = 0, mistake_left_ = 0;
  double last_gap_ = 0;
  int last_places_ = 0;
  bool have_last_ = false;
};

}  // namespace race
