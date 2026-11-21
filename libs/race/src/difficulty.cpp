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
}

void DriverAi::update(double gap, int places_behind) {
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
    else if (gap >= 0 && gap >= row_.start_rubberbanding && !row_.modes.empty()) {
      next = State::kRubberband;
      if (gap > row_.start_torque_cut) torque = torque_cut(row_, gap);
    }
  }
  if (next != state_) {
    // A threshold crossed: roll again; entering rubber-band mode draws a mode.
    roll();
    if (next == State::kRubberband) {
      double total = 0;
      for (const auto& m : row_.modes) total += m.frequency;
      double u = uniform() * total;
      mode_ = 0;
      for (const auto& m : row_.modes) {
        if (u < m.frequency) {
          mode_ = m.mode;
          break;
        }
        u -= m.frequency;
      }
    } else mode_ = 0;
    state_ = next;
  }
  if (state_ == State::kRubberband && gap > row_.start_torque_cut) mode_ = 8;
  braking_ = base_braking_, cornering_ = base_cornering_;
  if (max_) braking_ = skill_.braking_max, cornering_ = skill_.cornering_max;
  if (state_ == State::kCatchUp) {
    braking_ += (1.0 - braking_) * row_.catch_up_factor;
    cornering_ += (1.25 - cornering_) * row_.catch_up_factor;
  }
  // The drawn modes (under/over braking or cornering, lags): which id is
  // which and what Amount means are not in the spec; interim no effect.
  torque_ = skill_.nominal_torque_scale * torque;
}

}  // namespace race
