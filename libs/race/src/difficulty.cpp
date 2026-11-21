#include "race/difficulty.h"

#include <algorithm>

namespace race {

DriverSetup assign_driver(int event_skill, int event_temperament, int event_rubberband, int skill_modifier,
                          int aggro_modifier, int rubberband_modifier, const EventData& rows) {
  DriverSetup s;
  s.skill = std::clamp(event_skill + skill_modifier, 1, 80);
  s.temperament = std::clamp(event_temperament + aggro_modifier, 1, 10);
  if (event_rubberband >= 1 && event_rubberband <= 10) s.rubberband_id = event_rubberband + rubberband_modifier;
  else if (const SkillRow* r = rows.skill(s.skill)) s.rubberband_id = r->rubberband_id;
  else s.rubberband_id = 0;
  return s;
}

double torque_cut(const RubberbandRow& row, double gap) {
  const double span = row.max_torque_cut - row.start_torque_cut;
  const double r = span > 0 ? std::clamp((gap - row.start_torque_cut) / span, 0.0, 1.0) : (gap >= row.start_torque_cut ? 1.0 : 0.0);
  return 1 - r * row.torque_cut_factor;
}

double start_boost(int slot, int cars, double s) {
  if (cars < 1) return 0;
  const double top = cars > 1 ? 0.4 - 0.3 * double(slot) / double(cars - 1) : 0.4;
  if (s <= 300) return top;
  if (s >= 1000) return 0;
  return top * (1 - (s - 300) / 700);
}

double DriverAi::uniform() {
  // splitmix64
  uint64_t z = (rng_ += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return double(z >> 11) * (1.0 / 9007199254740992.0);
}

void DriverAi::roll() {
  base_braking_ = skill_.braking_min + uniform() * (skill_.braking_max - skill_.braking_min);
  base_cornering_ = skill_.cornering_min + uniform() * (skill_.cornering_max - skill_.cornering_min);
  ++rolls_;
}

void DriverAi::init(const SkillRow& skill, const RubberbandRow* row, uint64_t seed, bool enabled) {
  skill_ = skill;
  has_row_ = row != nullptr;
  row_ = row ? *row : RubberbandRow{};
  enabled_ = enabled;
  rng_ = seed;
  rolls_ = 0;
  roll();
  braking_ = base_braking_, cornering_ = base_cornering_;
  torque_ = skill_.nominal_torque_scale;
  state_ = State::kNormal, mode_ = 0, catch_up_ = false, max_ = false;
  amount_ = 0, mistake_left_ = 0, have_last_ = false;
}

void DriverAi::draw_mode() {
  // SPEC-RACE-AI, The draw: the rows in Id order, a running total of their
  // RelativeFrequency, the first row whose total reaches u (u uniform in
  // [0, 1), not normalised); the last row if none. An id with no row: the
  // three built-in choices.
  static const RubberbandModeChoice kBuiltIn[] = {{2, 0.67, 0.45}, {1, 0.29, 0.35}, {3, 0.04, 1.0}};
  const RubberbandModeChoice* rows = row_.modes.empty() ? kBuiltIn : row_.modes.data();
  const size_t n = row_.modes.empty() ? 3 : row_.modes.size();
  const double u = uniform();
  double total = 0;
  size_t pick = n - 1;
  for (size_t i = 0; i < n; ++i) {
    total += rows[i].frequency;
    if (total >= u) {
      pick = i;
      break;
    }
  }
  mode_ = rows[pick].mode;
  amount_ = rows[pick].amount;
  mistake_left_ = 1.0;  // interim: the raised scalar for 1 s (measured 0.5 to 2 s)
}

void DriverAi::update(double gap, int places_behind, double dt) {
  State next = State::kNormal;
  double torque = 1;
  if (enabled_) {
    const double behind = -gap;
    const double per = row_.catch_up_gap_per_position * std::max(0, places_behind);
    // Behind the player: catch-up (with its hysteresis) over the far-behind
    // maxima; ahead: rubber-band mode.
    if (has_row_ && gap < 0) {
      if (!catch_up_ && behind > row_.start_catch_up + per) catch_up_ = true;
      else if (catch_up_ && behind < row_.stop_catch_up + per) catch_up_ = false;
    } else catch_up_ = false;
    max_ = behind > skill_.dist_behind_for_max;
    if (catch_up_) next = State::kCatchUp;
    else if (max_) next = State::kMaxPerformance;
    else if (gap >= 0 && gap >= row_.start_rubberbanding) {
      next = State::kRubberband;
      if (gap > row_.start_torque_cut) torque = torque_cut(row_, gap);
    }
  }
  // When a mode is drawn: on entering rubber-band mode, and while in it
  // whenever the place changes or the gap crosses StartRubberbandingDistance,
  // StartTorqueCutDistance or 0. (-DistBehindForTorqueBoost is not read.)
  const auto crossed = [&](double at) { return (last_gap_ < at) != (gap < at); };
  const bool threshold = have_last_ && (crossed(row_.start_rubberbanding) || crossed(row_.start_torque_cut) ||
                                        crossed(0) || places_behind != last_places_);
  if (next != state_) {
    roll();  // a threshold crossed: roll again
    state_ = next;
    if (state_ == State::kRubberband) draw_mode();
    else mode_ = 0, amount_ = 0;
  } else if (state_ == State::kRubberband && threshold) {
    draw_mode();
  }
  last_gap_ = gap, last_places_ = places_behind, have_last_ = true;
  braking_ = base_braking_, cornering_ = base_cornering_;
  if (max_) braking_ = skill_.braking_max, cornering_ = skill_.cornering_max;
  if (state_ == State::kCatchUp) {
    braking_ += (1.0 - braking_) * row_.catch_up_factor;
    cornering_ += (1.25 - cornering_) * row_.catch_up_factor;
  }
  if (state_ == State::kRubberband) {
    mistake_left_ -= dt;
    if (gap > row_.start_torque_cut) {
      // Mode 8, the torque cut: braking and cornering at the row's minima.
      braking_ = skill_.braking_min, cornering_ = skill_.cornering_min;
    } else {
      // The mode table: under- modes take Amount off the row's minimum
      // (floor 0.25), over- modes put the scalar at 1 + Amount for the
      // mistake's hold, then the rolled value; the lags (5 to 7, unused on
      // the disc) have no effect.
      switch (mode_) {
        case 1: braking_ = std::max(skill_.braking_min - amount_, 0.25); break;
        case 2: cornering_ = std::max(skill_.cornering_min - amount_, 0.25); break;
        case 3:
          if (mistake_left_ > 0) braking_ = 1 + amount_;
          break;
        case 4:
          if (mistake_left_ > 0) cornering_ = 1 + amount_;
          break;
        default: break;
      }
    }
  }
  torque_ = skill_.nominal_torque_scale * torque;
}

}  // namespace race
