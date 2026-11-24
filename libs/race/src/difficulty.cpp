#include "race/difficulty.h"

#include <algorithm>
#include <cmath>

#include "drive/ai_driver.h"

namespace race {

DriverSetup assign_driver(int event_skill, int event_temperament, int event_rubberband, int skill_modifier,
                          int aggro_modifier, int rubberband_modifier, const EventData& rows) {
  DriverSetup s;
  s.skill = std::clamp(event_skill + skill_modifier, 1, 80);
  s.temperament = std::clamp(event_temperament + aggro_modifier, 1, 10);
  if (event_rubberband >= 1) {
    s.rubberband_id = (event_rubberband <= 10) ? (event_rubberband + rubberband_modifier) : event_rubberband;
  } else if (const SkillRow* r = rows.skill(s.skill)) {
    s.rubberband_id = r->rubberband_id;
  } else {
    s.rubberband_id = 0;
  }
  return s;
}

DriverSetup assign_driver_for_mode(int event_skill, int event_temperament, int event_rubberband, int skill_modifier,
                                   int aggro_modifier, int rubberband_modifier, Difficulty base_difficulty,
                                   AiMode ai_mode, const EventData& rows) {
  DriverSetup s = assign_driver(event_skill, event_temperament, event_rubberband, skill_modifier, aggro_modifier,
                                rubberband_modifier, rows);
  if (is_pinyon_hard(base_difficulty, ai_mode)) {
    // Pinyon Shift HARD (+1 tier): max aggression (AITemperaments row 1),
    // and at Pro / HardPlus base tier forces strongest AISkills row 1.
    s.temperament = 1;
    if (base_difficulty == Difficulty::kPro || base_difficulty == Difficulty::kHardPlus) s.skill = 1;
    s.rubberband_id = 0;
  }
  return s;
}

NemesisBoostResult nemesis_rival_boost(bool is_nemesis, bool start_boost_done, int other_ai_count, int other_ai_ahead,
                                       double gap_behind_player, double dist_to_finish, double braking,
                                       double cornering) {
  NemesisBoostResult r{braking, cornering, 0.0, false};
  if (!is_nemesis || !start_boost_done) return r;
  if (other_ai_count > 0) {
    if (other_ai_ahead > 0) {
      r.braking = drive::ai_boost(braking, 1.5, 1.0);
      r.cornering = drive::ai_boost(cornering, 1.5, 1.25);
      r.torque_add = 0.3;
      r.active = true;
    }
    return r;
  }
  // 1v1 nemesis rival (e.g. NR02): skill scalars untouched; torque-only ramp
  // from 50 m to 100 m behind the player, shut off within 500 m of the finish.
  if (dist_to_finish > 500.0 && gap_behind_player > 50.0) {
    const double f = std::clamp((gap_behind_player - 50.0) / 50.0, 0.0, 1.0);
    r.torque_add = 0.15 * f;
    r.active = r.torque_add > 0.0;
  }
  return r;
}

int lookup_skill_adjustment(int time_ratio_pct, const std::vector<std::pair<int, int>>& table) {
  static const std::pair<int, int> kBuiltIn[] = {
      {90, -9}, {91, -9}, {92, -9}, {93, -9}, {94, -7}, {95, -3}, {96, -2}, {97, -1}, {98, -1}, {99, 1}, {100, 2},
      {101, 4}, {102, 6}, {103, 8}, {104, 10}, {105, 11}, {106, 12}, {107, 13}, {108, 14}, {109, 15}, {110, 15}};
  const int r = std::clamp(time_ratio_pct, 90, 110);
  if (!table.empty()) {
    int best = table.front().second;
    int best_dist = std::abs(table.front().first - r);
    for (const auto& [ratio, adj] : table) {
      const int dist = std::abs(ratio - r);
      if (dist < best_dist) best_dist = dist, best = adj;
    }
    return best;
  }
  for (const auto& [ratio, adj] : kBuiltIn)
    if (ratio == r) return adj;
  return 0;
}

int update_dynamic_ai_skill(int current_skill, double player_time, double ref_ai_time, int num_races_completed,
                            AiMode mode, const std::vector<std::pair<int, int>>& table,
                            const DynamicDifficultyConfig& cfg) {
  const int clamped = std::clamp(current_skill, 1, std::max(1, cfg.easy_upper_bound));
  if (mode != AiMode::kStock || !(player_time > 0.0) || !(ref_ai_time > 0.0)) return clamped;
  const int ratio = std::clamp(int(std::lround(100.0 * player_time / ref_ai_time)), 90, 110);
  const int adj = lookup_skill_adjustment(ratio, table);
  const int div = std::clamp(num_races_completed, 1, std::max(1, cfg.max_num_races));
  const int delta = int(std::lround(double(adj) / double(div)));
  return std::clamp(clamped + delta, 1, std::max(1, cfg.easy_upper_bound));
}

Difficulty difficulty_from_dynamic_skill(int ai_skill, const DynamicDifficultyConfig& cfg) {
  if (ai_skill <= cfg.pro_upper_bound) return Difficulty::kPro;
  if (ai_skill <= cfg.hard_upper_bound) return Difficulty::kHard;
  if (ai_skill <= cfg.medium_upper_bound) return Difficulty::kMed;
  return Difficulty::kEasy;
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
  mistake_left_ = 0.5 + 1.5 * uniform();  // SPEC-RACE-AI: randomized 0.5 to 2.0 s
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
