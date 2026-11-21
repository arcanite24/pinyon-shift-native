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
  int64_t credits = 0;
  int wristband_points = 0;
  bool first_completion = false, won = false;
  int64_t prize_car = 0;  // 0: none
};

// One finished (or not-finished) race of an event with one race, applied
// to `record` in the spec's order. `style`: Events.CareerEventStyle (1:
// festival, the only style with wristband points); `level`: Events.Level.
// `prize_car`: the event's Rewards_EventPrizes car (0: none), given once
// for place 1.
RaceAward award_race(CareerRecord& record, int64_t event_id, int64_t race_id, int career_type, int style, int level,
                     double prize, int racers, int place, bool finished, double race_time, double top_speed,
                     int64_t prize_car, const WristbandTables& tables);

}  // namespace race
