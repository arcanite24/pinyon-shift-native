// Horizon Rally full championship loop (SPEC-V4-CONTENT, Horizon Rally):
// - RALLY_HEATS intro + all 7 x 4-stage championships (MultiStageSeriesEvents /
//   MultiStageSeriesRaces) in hub order
// - RallyScoring (places 1..50), RallyWristbandLevels (Yellow / Green Champion),
//   Rewards_EventUnlock chain, championship & stage credit payouts
// - Cumulative 4-stage total-time standings across the 50-driver field
// - DriveSave persistence matching profile-import Rally counters
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "drive/save.h"
#include "race/rally.h"

namespace cars {
class PartsDb;
}

namespace fh1 {
class StringTables;
}

namespace race {

struct RallyStageInfo {
  int64_t event_id = 0;
  int stage_index = 0;       // 1..4
  int64_t track_id = 0;
  int route_id = 0;
  std::string horizon_id;    // e.g. "RALLY_5A"
  std::string name;          // localized track/stage name
  StageTrack track;          // length, start/finish lines, 4 splits, 5 best split times
};

struct RallyChampionshipDef {
  int series_id = 0;         // MultiStageSeriesEvents.Id (1..7, hub order)
  int64_t event_id = 0;      // Events.Id (271..277)
  std::string horizon_id;    // MS_RALLY_01..MS_RALLY_07
  std::string name;          // e.g. "Beaumont Rally"
  int target_class = 4;      // CarClasses Id (4=B, 5=A, 6=S, 7=R3)
  int64_t cash_prize = 0;    // Events.CashPrize (10,000..50,000)
  int unlock_points_req = 0; // Events.UnlockPointsReq (310 for MS_RALLY_02)
  int64_t unlocks_event_id = 0;  // Rewards_EventUnlock target (0 for final)
  double trigger_x = 0.0;    // rally_event_activations.xml
  double trigger_z = 0.0;
  std::vector<RallyStageInfo> stages;  // 4 stages in StageIndex order
};

// RALLY_HEATS: the first-run stage, 15 drivers (SPEC-V4-CONTENT).
inline constexpr size_t kHeatsDrivers = 15;

struct RallyHeatsDef {
  int64_t event_id = 279;
  int route_id = 24;
  int64_t player_car_id = 1626;  // Rally WRX twin
  int64_t cash_prize = 45000;
  std::string name = "Horizon Rally Heats";
  StageTrack track;
};

struct RallyWristbandDef {
  int level = 0;
  int xp_required = 0;
  std::string name;       // e.g. "Yellow Rally", "Horizon Rally Champion"
  std::string thumbnail;  // "Yellow", "Green"
};

struct RallyProgress {
  int championships_won_count = 0;
  int podiums = 0;
  int stages_completed = 0;
  bool champion = false;

  int championships_won() const { return championships_won_count; }
  int podiums_count() const { return podiums; }
  int stages_completed_total() const { return stages_completed; }
  bool champion_unlocked() const { return champion; }
};

struct RallyCatalog {
  RallyHeatsDef heats;
  std::vector<RallyChampionshipDef> championships;  // hub order (series_id 1..7)
  std::vector<int> scoring;                         // 1-based place -> Rally XP (index 0 unused)
  std::vector<RallyWristbandDef> wristbands;        // ascending by level
  RallyField field;                                 // 49 AI drivers + difficulty bounds
  std::map<int, std::string> ai_names;              // AIPlayers.Id -> display name
  std::filesystem::path package_dir;                // installed Rally DLC package folder (if found)
  bool loaded = false;

  const RallyChampionshipDef* find_championship(int64_t event_id) const;
  const RallyChampionshipDef* find_by_horizon_id(const std::string& horizon_id) const;

  // RallyScoring points for final championship place (1..50); 0 out of range.
  int points_for_place(int place) const;
  // Highest RallyWristbandLevels row reached at `xp`.
  int wristband_for_xp(int xp) const;
  std::string wristband_name(int level) const;
  int next_wristband_xp(int level) const;

  // True when `event_id` is unlocked given Rally Heats completion, Rally XP,
  // unlocked event IDs, and completed championship results (podium required for chain).
  bool is_championship_unlocked(int64_t event_id, int rally_xp, bool heats_done,
                                const std::vector<int64_t>& event_unlocks,
                                const std::vector<std::pair<int64_t, int>>& event_results) const;

  // Reachable progression signals across all 7 championships / 28 stages (for achievements 211 & 215).
  int championships_won(const drive::DriveSave& save) const;
  int podiums_count(const drive::DriveSave& save) const;
  int stages_completed_total(const drive::DriveSave& save) const;
  bool champion_unlocked(const drive::DriveSave& save) const;
  RallyProgress summarize_progress(const drive::DriveSave& save) const;

  // Championship credits payout for `place` (1..50), scaled by difficulty + assists %.
  static int64_t championship_credits(int64_t cash_prize, int place, int diff_pct = 0, int assists_pct = 0);
};

// Locates the installed Horizon Rally DLC package folder (offer 1600) under
// `state_root/user/0000000000000000/4D5309C9/00000002/`. Empty path if absent.
std::filesystem::path find_rally_package(const std::filesystem::path& state_root);

// Loads the complete Rally catalog from `db` (or v4 `media.zip:db/gamedb.slt`),
// `rallytracktimings.xml`, `rallyaidifficulty.xml`, and `rally_event_activations.xml`.
bool load_rally_catalog(const std::filesystem::path& state_root, const cars::PartsDb* db,
                        const fh1::StringTables* strings, RallyCatalog& out, std::string* error = nullptr);

// Class letter string for Rally TargetClass (4=B, 5=A, 6=S, 7=R3).
const char* rally_class_name(int target_class);

// Cumulative standing entry across a 4-stage championship (or Heats).
struct RallyStandingRow {
  int driver_id = 0;  // 0 for player, AIPlayers.Id for AI
  std::string name;
  bool is_player = false;
  std::vector<double> stage_times;
  double last_stage_time = 0.0;
  double total_time = 0.0;
  int stage_place = 0;
  int overall_place = 0;
  double gap_to_leader = 0.0;
};

struct RallyStageFinish {
  int stage_index = 0;  // 0-based index of stage just finished
  int total_stages = 4;
  double player_stage_time = 0.0;
  double player_total_time = 0.0;
  int stage_place = 1;
  int overall_place = 1;
  double stage_gap_to_leader = 0.0;
  double overall_gap_to_leader = 0.0;
  bool championship_complete = false;
  std::vector<RallyStandingRow> stage_standings;
  std::vector<RallyStandingRow> overall_standings;
};

struct RallyChampionshipReward {
  bool is_heats = false;
  int64_t event_id = 0;
  int final_place = 1;
  double total_time = 0.0;
  int rally_xp_earned = 0;
  int old_rally_xp = 0;
  int new_rally_xp = 0;
  int old_rally_level = 0;
  int new_rally_level = 0;
  bool wristband_up = false;
  bool champion_unlocked = false;
  int64_t base_credits = 0;
  int64_t bonus_credits = 0;
  int64_t total_credits = 0;
  std::vector<int64_t> unlocked_events;
};

struct RallyCarValidation {
  bool valid = true;
  std::string reason;
};

// Runs a full 4-stage championship (or single-stage RALLY_HEATS) with
// cumulative time standings, service-area state, and final reward commit.
class RallyChampionshipSession {
 public:
  RallyChampionshipSession() = default;

  bool start_championship(const RallyCatalog& catalog, int64_t championship_event_id, int difficulty,
                          uint64_t seed = 1, const std::string& player_name = "YOU");
  bool start_heats(const RallyCatalog& catalog, int difficulty, uint64_t seed = 1,
                   const std::string& player_name = "YOU");

  bool active() const { return active_; }
  bool is_heats() const { return is_heats_; }
  bool finished() const { return finished_; }
  bool in_service_area() const { return in_service_area_; }

  int64_t event_id() const { return event_id_; }
  const std::string& title() const { return title_; }
  int target_class() const { return target_class_; }
  int64_t cash_prize() const { return cash_prize_; }
  int difficulty() const { return difficulty_; }

  int current_stage_index() const { return current_stage_; }  // 0..total_stages-1
  int stages_completed() const { return drivers_.empty() ? 0 : int(drivers_[0].stage_times.size()); }
  int total_stages() const { return int(stages_.size()); }
  const RallyStageInfo* current_stage() const;
  const std::vector<RallyStageInfo>& stages() const { return stages_; }

  // Records completion of the current stage using `player_stage_time` and
  // deterministic `ai_stage_times` (if empty, generated from `catalog.field`).
  // Guarded against double-scoring if the current stage was already recorded.
  RallyStageFinish record_stage_finish(const RallyCatalog& catalog, double player_stage_time,
                                       const std::vector<double>& ai_stage_times = {});

  // Revalidates that the player's current car still meets this championship's
  // class limit (car_class_id <= target_class) and required rally upgrade kit
  // before leaving the Service Area. (RALLY_HEATS uses a loaner and always passes.)
  RallyCarValidation validate_car(int car_class_id, bool has_rally_kit) const;

  // Leaves the between-stage Service Area and advances to the next stage.
  bool advance_from_service_area();
  // Revalidates car class & rally kit before leaving Service Area.
  bool advance_from_service_area(int car_class_id, bool has_rally_kit, std::string* reason = nullptr);

  // Retires early from the championship.
  void retire();

  // Commits final championship rewards, XP, wristband, statistics, results, and
  // unlocks into `save` (once per completed session).
  RallyChampionshipReward finalize(const RallyCatalog& catalog, drive::DriveSave& save, int diff_pct = 0,
                                   int assists_pct = 0);

  const std::vector<RallyStandingRow>& overall_standings() const { return overall_; }
  const RallyStageFinish& last_stage_finish() const { return last_finish_; }
  int player_overall_place() const;
  double player_total_time() const;

 private:
  // `max_ai`: the field's AI drivers kept, in field order (the heats: 14).
  void init_drivers(const RallyCatalog& catalog, const std::string& player_name, size_t max_ai = SIZE_MAX);

  bool active_ = false;
  bool is_heats_ = false;
  bool finished_ = false;
  bool finalized_ = false;
  bool in_service_area_ = false;

  int64_t event_id_ = 0;
  int64_t unlocks_event_id_ = 0;
  std::string title_;
  int target_class_ = 4;
  int64_t cash_prize_ = 0;
  int difficulty_ = 1;
  uint64_t seed_ = 1;

  std::vector<RallyStageInfo> stages_;
  int current_stage_ = 0;

  std::vector<RallyStandingRow> drivers_;  // index 0 = player, 1..49 = AI
  std::vector<RallyStandingRow> overall_;  // sorted ascending by total_time
  RallyStageFinish last_finish_;
};

// Convenience: marks RALLY_HEATS done in `save`? True if event_id 279 has a result
// or any Rally unlock/championship already exists in `save`.
bool is_rally_heats_completed(const drive::DriveSave& save);

}  // namespace race
