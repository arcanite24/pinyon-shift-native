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
#include <string_view>
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

// The driver level a save resumes at (one-based): the saved level, kept as
// saved (an imported Main/Level is not recomputed from XP, FORMAT-PROFILE),
// or the level `driver_xp` reaches when that is higher. When the saved level
// is above the XP's, `driver_xp` is raised to that level's PlayerLevelDef
// floor so later gains count from it. `tables` may be null or empty.
int resume_driver_level(const ProgressionTables* tables, int saved_level, int& driver_xp);

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
  int braking = 1;        // 0 = Assisted, 1 = ABS On, 2 = ABS Off (+10%)
  int steering = 1;       // 0 = Assisted, 1 = Normal, 2 = Simulation (+5%)
  bool tcs_on = false;    // TractionControl (On = -5% penalty vs off, clamped >= 0)
  bool stm_on = true;     // StabilityControl (Off = +5%)
  int shifting = 0;       // 0 = Automatic, 1 = Manual (+10%), 2 = Manual + Clutch (+15%)
  int driving_line = 2;   // 0 = Off (+10%), 1 = Braking Only (+5%), 2 = Full
  int damage_mode = 1;    // 0 = None, 1 = Cosmetic, 2 = Simulation (+10%)
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

// The mileage bonus rule of one race mode, from the game's
// gametunablesettings.zip PostRaceOptions/PostRaceOptions_<mode>.xml
// `<MileageCredits totalDistance=".." reward=".." increment=".."/>`: the
// distance driven (m, up to totalDistance) pays reward × d / totalDistance,
// floored to a multiple of increment (offline career modes: 5,000 Cr over
// 50 km in steps of 100; Rivals: 20,000 in steps of 400). An empty rule
// (data not loaded) pays nothing.
struct MileageCredits {
  double total_distance_m = 0;
  double reward = 0;
  int64_t increment = 0;
  bool valid() const { return total_distance_m > 0 && reward > 0 && increment > 0; }
};
int64_t mileage_bonus_credits(double distance_m, const MileageCredits& rule);

// The offline PostRaceOptions file each race picks.
enum class PostRaceMode : uint8_t {
  kFestivalRace,
  kStreetRace,
  kHeadline,
  kNemesisRace,
  kExhibition,
  kRaceEncounter,
  kRivals,
  kCount
};
// From Events.CareerTypeId (4 street, 11 nemesis, 12 headline, 7/8
// showcase, else festival); a Rivals run picks the Rivals file.
PostRaceMode post_race_mode(int career_type, bool rivals);
// "PostRaceOptions/PostRaceOptions_Offline_<Mode>.xml".
std::string post_race_options_member(PostRaceMode mode);
// The MileageCredits element of one PostRaceOptions file.
bool parse_mileage_credits(std::string_view xml, MileageCredits& out, std::string* error);

struct PostRaceOptions {
  MileageCredits mileage[size_t(PostRaceMode::kCount)];
  const MileageCredits& for_mode(PostRaceMode m) const { return mileage[size_t(m)]; }
};
// Reads every offline mode's file from <game_root>/media/gametunablesettings.zip.
bool load_post_race_options(const std::filesystem::path& game_root, PostRaceOptions& out, std::string* error);

// Optional extra race-breakdown inputs (defaults preserve backward compatibility).
struct RaceBonusInputs {
  int assists_pct = 0;          // per-assist difficulty bonus % (e.g. +20% rewind off)
  int64_t skills_points = 0;    // skill points banked during this race
  int64_t bounty_bonus = 0;     // rival / bounty bonus credits
  int wristband_level = -1;     // current wristband level (-1: infer from record.wristband_points)
  double reward_multiplier = 1.0;  // Festival Pacing multiplier (0.5x..5.0x)
  int prize_car_max_place = 1;     // Rewards_EventPrizes finishing place cutoff (1 or 2)
  MileageCredits mileage;          // the race mode's PostRaceOptions rule (empty: no mileage bonus)
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

// What commit_race_award writes besides the award itself.
struct RaceAwardCommit {
  int64_t event_id = 0, race_id = 0;
  int career_type = 0;
  int place = 0;
  bool finished = false;
  double race_time = 0, top_speed = 0;
};
// award_race in two halves: compute_race_award reads `record` (best places,
// prize cars, wristband points) and changes nothing; commit_race_award
// writes the records, counters, credits, wristband points and the prize car
// in the spec's order. award_race is compute then commit.
RaceAward compute_race_award(const CareerRecord& record, int64_t event_id, int career_type, int style, int level,
                             double prize, int racers, int place, bool finished, int64_t prize_car,
                             const WristbandTables& tables, int difficulty_pct = 0, double distance_m = 0,
                             const RaceBonusInputs& extras = {});
void commit_race_award(CareerRecord& record, const RaceAward& award, const RaceAwardCommit& c);

// The award of a finished race, held until the player leaves the finish
// banner (the original updates and saves the profile only on the banner's
// done exit): Continue commits it once; Restart and Quit drop it.
class PendingRaceAward {
 public:
  void hold(const RaceAward& award, const RaceAwardCommit& c) {
    award_ = award;
    commit_ = c;
    held_ = true;
    committed_ = false;
  }
  bool held() const { return held_; }
  // The held award (for the post-race screens), or null.
  const RaceAward* award() const { return held_ ? &award_ : nullptr; }
  // Applies the held award to `record` once and returns it; null when
  // nothing is held (already committed or dropped).
  const RaceAward* commit(CareerRecord& record) {
    if (!held_) return nullptr;
    held_ = false;
    committed_ = true;
    commit_race_award(record, award_, commit_);
    return &award_;
  }
  void drop() { held_ = false; }
  // Whether the held award was committed (a dropped one never is).
  bool committed() const { return committed_; }

 private:
  RaceAward award_;
  RaceAwardCommit commit_;
  bool held_ = false;
  bool committed_ = false;
};

}  // namespace race
