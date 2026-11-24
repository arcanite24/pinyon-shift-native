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
  std::vector<int64_t> nemesis_bonuses;             // CareerWristbandLevels.NemesisBonus per level
  int points_for(int level, int place) const;       // level < 0 counts as 0; place 0: 0
  // The highest level whose threshold `total` has reached (its index; -1: none).
  int level_index(int total) const;
  int64_t nemesis_bonus_for(int level) const;
};
bool load_wristband_tables(const std::filesystem::path& game_root, WristbandTables& out, std::string* error);
bool load_wristband_tables(const std::filesystem::path& game_root, const std::filesystem::path& state_root,
                           const cars::PartsDb* db, WristbandTables& out, std::string* error);

// Driver Levels (PlayerLevelDef 1,000 levels + Rewards_DriverLevelv3 gift cars)
// and Manufacturer Affinity (AffinityLevelDef 51 levels: upgrade discount + bonus CR).
struct DriverLevelRow {
  int level = 0;          // 0..999 (displayed 1..1000)
  int xp = 0;             // cumulative XP required
  int max_gift_cars = 0;
};

struct AffinityLevelRow {
  int level = 0;          // 0..50
  int xp = 0;             // cumulative XP required
  double discount = 0.0;  // upgrade cost discount [0.0 .. 1.0]
  int64_t bonus_credits = 0;
};

struct ProgressionTables {
  std::vector<DriverLevelRow> driver_levels;                // ascending by level
  std::map<int, std::vector<int64_t>> driver_level_cars;    // DriverLevel -> RewardCarIds
  std::vector<AffinityLevelRow> affinity_levels;            // ascending by level

  int driver_level_for_xp(int xp) const;
  int next_driver_level_xp(int xp) const;
  int64_t driver_level_gift_car(int level) const;
  int affinity_level_for_xp(int xp) const;
  double affinity_discount_for_xp(int xp) const;
  int64_t affinity_bonus_for_level(int level) const;
};

bool load_progression_tables(const std::filesystem::path& game_root, ProgressionTables& out, std::string* error);
bool load_progression_tables(const std::filesystem::path& game_root, const std::filesystem::path& state_root,
                             const cars::PartsDb* db, ProgressionTables& out, std::string* error);

struct ProgressionUpdate {
  int old_driver_level = 1;
  int new_driver_level = 1;
  std::vector<int64_t> gift_cars;
  int old_affinity_level = 0;
  int new_affinity_level = 0;
  int64_t affinity_bonus_credits = 0;
  double affinity_discount = 0.0;
};

// The career's records (Career_RaceResults, Career_EventResults, Main/*).
struct CareerRecord {
  int64_t credits = 0, total_winnings = 0;
  int wristband_points = 0;
  int driver_xp = 0, driver_level = 1;
  std::map<int64_t, int> make_affinity_xp;  // MakeID -> cumulative XP
  int num_races = 0, num_victories = 0, num_podiums = 0;
  double time_in_festival = 0, time_in_street = 0, time_in_exhibition = 0;
  double top_speed = 0;
  std::map<int64_t, int> race_places;   // RaceId -> best place
  std::map<int64_t, int> event_places;  // EventId -> best place
  std::vector<int64_t> prize_cars;      // Data_Car ids given by prize events
};

// Applies driver XP and manufacturer affinity XP to `record` and returns any
// level-up gift cars and affinity credit bonuses.
ProgressionUpdate apply_progression_xp(CareerRecord& record, int driver_xp_delta, int64_t make_id,
                                       int affinity_xp_delta, const ProgressionTables& tables);

struct RaceAward {
  int64_t base_credits = 0;
  int64_t mileage_bonus = 0;
  int difficulty_pct = 0;
  int64_t difficulty_bonus = 0;
  int assists_pct = 0;
  int64_t assists_bonus = 0;
  int64_t bounty_bonus = 0;
  int64_t skills_bonus = 0;
  int64_t nemesis_bonus = 0;
  int64_t credits = 0;
  int wristband_points = 0;
  int driver_xp = 0;
  int affinity_xp = 0;
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

// Optional extra race-breakdown inputs (defaults preserve backward compatibility).
struct RaceBonusInputs {
  int assists_pct = 0;          // per-assist difficulty bonus % (e.g. +20% rewind off)
  int64_t skills_points = 0;    // skill points banked during this race
  int64_t bounty_bonus = 0;     // rival / bounty bonus credits
  int wristband_level = -1;     // current wristband level (-1: infer from record.wristband_points)
  double reward_multiplier = 1.0;  // Festival Pacing multiplier (0.5x..5.0x)
  int prize_car_max_place = 1;     // Rewards_EventPrizes finishing place cutoff (1 or 2)
};

// One finished (or not-finished) race of an event with one race, applied
// to `record` in the spec's order. `style`: Events.CareerEventStyle (1:
// festival, the only style with wristband points; 6: Horizon Finale 50k CR
// first-win override); `level`: Events.Level.
// `prize_car`: the event's Rewards_EventPrizes car (0: none), given once
// for place 1.
RaceAward award_race(CareerRecord& record, int64_t event_id, int64_t race_id, int career_type, int style, int level,
                     double prize, int racers, int place, bool finished, double race_time, double top_speed,
                     int64_t prize_car, const WristbandTables& tables, int difficulty_pct = 0, double distance_m = 0,
                     const RaceBonusInputs& extras = {});

}  // namespace race
