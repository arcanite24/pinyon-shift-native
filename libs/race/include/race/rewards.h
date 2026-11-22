// SPEC-EVENT-REWARDS: what a finished career race gives the player (the
// credit share and its rounding, wristband points by improvement) and the
// career records it writes (best place per race and event, the counters).
// The records live in the engine's own save (drive/save.h), not in the
// game's profile.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cars {
class PartsDb;
}

namespace race {

enum class Winnings { kDefault, kWinnerTakesAll, kTopThree };

// The race mode's WinningsDistribution. Interim (SPEC_QUESTIONS Q52): from
// the career type, as the spec's table names the modes (headline 12:
// winner takes all; street 4 and nemesis 11: top three; else the default).
Winnings winnings_for_career_type(int career_type);

// Two significant figures, half up; under 10 is 0.
int64_t round_prize(double x);
// The player's share of `prize` for place p (1-based) of n racers; 0 for a
// racer that did not finish or a race of one.
int64_t race_credits(double prize, Winnings w, int racers, int place, bool finished);

// WristbandScoring and CareerWristbandLevels.
struct WristbandTables {
  std::map<int, std::map<int, int>> points;         // [level][place]
  std::vector<std::pair<int, std::string>> levels;  // (point threshold, name) ascending
  int points_for(int level, int place) const;       // level < 0 counts as 0; place 0: 0
  // The highest level whose threshold `total` has reached (its index; -1: none).
  int level_index(int total) const;
};
bool load_wristband_tables(const std::filesystem::path& game_root, WristbandTables& out, std::string* error);
bool load_wristband_tables(const std::filesystem::path& game_root, const std::filesystem::path& state_root,
                           const cars::PartsDb* db, WristbandTables& out, std::string* error);

// The career's records (Career_RaceResults, Career_EventResults, Main/*).
struct CareerRecord {
  int64_t credits = 0, total_winnings = 0;
  int wristband_points = 0;
  int num_races = 0, num_victories = 0, num_podiums = 0;
  double time_in_festival = 0, time_in_street = 0, time_in_exhibition = 0;
  double top_speed = 0;
  std::map<int64_t, int> race_places;   // RaceId -> best place
  std::map<int64_t, int> event_places;  // EventId -> best place
  std::vector<int64_t> prize_cars;      // Data_Car ids given by prize events
};

struct RaceAward {
  int64_t base_credits = 0;
  int64_t mileage_bonus = 0;
  int difficulty_pct = 0;
  int64_t difficulty_bonus = 0;
  int64_t credits = 0;
  int wristband_points = 0;
  bool first_completion = false, won = false;
  int64_t prize_car = 0;  // 0: none
};

// Per-assist settings for the difficulty credit bonus (SPEC-REWIND section 7,
// SPEC-EVENT-REWARDS Credits / Unknowns).
// Rewind OFF awards +20% (0.20) except in excluded session mode 3; Rewind ON
// awards +0% with no extra per-use penalty. Remaining assists are interim 0%
// until their exact percentages are confirmed.
struct DifficultyAssists {
  static constexpr int kExcludedSessionMode = 3;
  bool rewind_on = true;  // Main/Rewind: 0 = ON (default), 1 = OFF
  int session_mode = 0;   // mode 3 skips the Rewind difficulty bonus
  int braking = 1;        // 0 = Assisted, 1 = ABS On, 2 = ABS Off
  int steering = 1;       // 0 = Assisted, 1 = Normal, 2 = Simulation
  bool tcs_on = false;    // TractionControl
  bool stm_on = true;     // StabilityControl
  int shifting = 0;       // 0 = Automatic, 1 = Manual, 2 = Manual + Clutch
  int driving_line = 2;   // 0 = Off, 1 = Braking Only, 2 = Full
};

// Rewind difficulty bonus as an exact fraction: 0.20 when Rewind is OFF and
// session_mode != 3, 0.0 otherwise (SPEC-REWIND section 7).
double rewind_difficulty_bonus(bool rewind_on, int session_mode = 0);
// Sum of per-assist difficulty bonus fractions across all assists.
double assist_difficulty_bonus(const DifficultyAssists& assists);
// Sum of per-assist difficulty bonuses in integer percent (e.g. 20 for +20%).
int assist_difficulty_bonus_percent(const DifficultyAssists& assists);

// Difficulty bonus percentage (ours: 0 Easy, 15 Med, 30 Hard, 45 Pro) and
// mileage bonus from race distance driven (m).
int difficulty_bonus_percent(int difficulty_index);
// Combined AI difficulty + per-assist difficulty bonus percentage.
int total_difficulty_bonus_percent(int difficulty_index, const DifficultyAssists& assists);
int64_t mileage_bonus_credits(double distance_m);

// One finished (or not-finished) race of an event with one race, applied
// to `record` in the spec's order. `style`: Events.CareerEventStyle (1:
// festival, the only style with wristband points); `level`: Events.Level.
// `prize_car`: the event's Rewards_EventPrizes car (0: none), given once
// for place 1.
RaceAward award_race(CareerRecord& record, int64_t event_id, int64_t race_id, int career_type, int style, int level,
                     double prize, int racers, int place, bool finished, double race_time, double top_speed,
                     int64_t prize_car, const WristbandTables& tables, int difficulty_pct = 0, double distance_m = 0);

}  // namespace race
