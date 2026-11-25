// The engine's own save for the drive (ours, M4 groundwork): where the car
// stands and which car it is, so a restart puts the player back. A small
// versioned text file of our own format, kept in a user-data folder of
// ours; it is not the game's profile and reads nothing of it.
//
//   shift-drive-save 1
//   car NIS_SkylineGTRVSpecII_02
//   position -1790.46 34.16 1863.44
//   heading -1.4358
//   spawn 2002.96 143.84 960.00 2.2008
//   distance 51234.5
//   signs flyer_001 flyer_017
//   barn barnfind_01 2 50000.0 51200.3
//   garage 1219
//   owned_garage 1
//   credits 7500 7500
//   wristband 120
//   stats 1 0 1 160.93 0 0 61.2
//   race_result 143 3
//   event_result 22 3
//   prize_cars 1031
//   level 12
//   unlocks event 22 143
//   unlocks hub 1 2
//   rally 1 1
//   club 1219 3735928559:1 12345:0
//
// Lines are "key values"; unknown keys are skipped, so later versions can
// add lines. `spawn` (optional) is the last spawn point, where "back to the
// start" goes when no road is found. The free-roam progress
// (SPEC-FREEROAM-COLLECTIBLES; freeroam::Progress) is optional: `distance`
// is the total driven (m), `signs` the found discount signs, one `barn`
// line per barn find off its starting state (name, status, rumour and
// discovery distances), `garage` the Data_Car ids barn finds gave
// (with `owned_garage 1`, the save came from the player's profile and
// `garage` is every owned car: the car picker offers only those). The
// career (SPEC-EVENT-REWARDS; race::CareerRecord) is optional: credits and
// total winnings, wristband points, the counters (races, victories,
// podiums, festival, street and exhibition race time in s, top speed in
// m/s), the best place per race and per event, and the prize cars.
// From an imported profile (FORMAT-PROFILE) the save also carries the
// driver level, the event and hub unlocks, the Rally counters (`rally`
// races and stages, present only on a v4 profile) and the 1000 Club
// record per car (each challenge's key and completed flag, in file order).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "drive/controls.h"

namespace drive {

struct SavedBarn {
  std::string name;
  int status = 3;
  double rumour_at = 0, found_at = 0;
};

struct PendingBarnRestoration {
  int barn_id = 0;
  int car_id = 0;
  float remaining_sec = 0.0f;
};

struct SavedClubCar {
  int64_t car_id = 0;
  std::vector<std::pair<uint32_t, bool>> challenges;  // key, completed
  // Optional per-challenge save_progress node counts (parallel to `challenges`).
  std::vector<std::vector<int>> progress;
};

struct SavedCarHistory {
  int64_t garage_id = 0;
  int64_t car_id = 0;
  double distance_driven = 0.0;  // DistanceDriven (m)
  double time_driven = 0.0;      // TimeDriven (s)
  int64_t total_winnings = 0;
  int64_t total_repairs = 0;
  int64_t num_races = 0;
  int64_t num_victories = 0;
  int64_t num_podiums = 0;
};

struct DriveSave {
  static constexpr int kVersion = 1;
  std::string car;
  std::array<double, 3> position{};
  double heading = 0;  // rad, 0 faces +z, positive turns towards +x
  bool has_spawn = false;
  std::array<double, 3> spawn{};
  double spawn_heading = 0;
  double total_distance = 0;
  std::vector<std::string> signs_found;
  std::vector<SavedBarn> barns;
  std::vector<PendingBarnRestoration> pending_barn_restorations;
  std::vector<int> garage;
  bool owned_garage = false;
  // The career.
  bool career_started = false;
  bool intro_completed = true;  // false on brand-new career until Viper->Corrado opening finishes
  // Campaign progression state (Minute-0 -> Horizon Festival Finale & Credits).
  std::string campaign_stage;  // empty: legacy/inferred; else canonical stage key
  std::vector<std::string> completed_cutscenes;
  std::vector<std::string> heard_dialogs;
  std::vector<std::string> unlocked_bosses;
  std::vector<std::string> unlocked_showcases;
  bool finale_complete = false;
  int64_t credits = 0, total_winnings = 0;
  int wristband_points = 0;
  int collected_wristband = -1;  // -1: legacy/imported (inferred from wristband_points); 0..6: collected at Race Central
  double popularity_points = 0;  // SPEC-SKILLS-POPULARITY: Fame ladder points
  int64_t max_skills_score = 0, max_skills_car = 0;
  int fame_rewards_claimed = 0;
  int difficulty = 1;  // 0 Easy, 1 Med, 2 Hard, 3 Pro, 4 Hard+
  int ai_skill = 10;   // Main/AISkill dynamic difficulty (1..20, lower = harder)
  int ai_mode = 0;     // 0 Stock, 1 Fair (no rubber-band), 2 Hard (+1 tier)
  // Radio & Sat-Nav state (SPEC-RADIO: station, DJ, volume, SatNav).
  std::string radio_station;  // empty: default station; else e.g. Radio1..Radio4_Silent
  bool radio_dj = true;
  double radio_volume = 1.0;
  bool satnav_enabled = true;
  bool treasure_map = false;
  bool fast_travel_anywhere = false;  // Main/FastTravelAnywhere (v4 token-store unlock)
  bool codriver_enabled = true;       // Main/CoDriver (v4 Rally co-driver calls)
  double tod_sec = -1.0;  // <0: unset/default; [0, 86400): persisted Colorado time-of-day seconds
  // Roads Discovered (CRoadDiscovery bitset: node_count + ceil(node_count/32) words).
  uint32_t road_discovery_nodes = 0;
  std::vector<uint32_t> road_discovery_words;
  // Discovered Festival Outposts (GasStation_01..10) & completed PR Stunts.
  std::vector<std::string> discovered_outposts;
  std::vector<std::string> completed_pr_stunts;
  // Speed Traps & Speed Zones personal bests (id -> best mph).
  std::vector<std::pair<std::string, double>> speed_traps;
  std::vector<std::pair<std::string, double>> speed_zones;
  // Sponsorship Challenges progress (challenge_id -> {count, tiers_claimed}).
  struct SavedSponsor {
    std::string id;
    int count = 0;
    int claimed = 0;
  };
  std::vector<SavedSponsor> sponsors;
  // Head-to-Head & Rivals counters.
  int h2h_races = 0, h2h_wins = 0;
  int rivals_beaten = 0, rally_rivals_beaten = 0;
  // Driver XP (PlayerLevelDef) & Manufacturer Affinity XP (AffinityLevelDef: make_id -> xp).
  int driver_xp = 0;
  std::vector<std::pair<int64_t, int>> make_affinity;
  // Horizon Promo spotted car IDs (Data_Car.Id).
  std::vector<int> promo_cars;
  int num_races = 0, num_victories = 0, num_podiums = 0;
  double time_festival = 0, time_street = 0, time_exhibition = 0, top_speed = 0;
  std::vector<std::pair<int64_t, int>> race_results, event_results;
  std::vector<int64_t> prize_cars;
  // Carried from an imported profile.
  int level = 0;
  std::vector<int64_t> event_unlocks, hub_unlocks;
  bool has_rally = false;
  int num_rallies = 0, num_rally_stages = 0, num_rally_victories = 0, rally_xp = 0, rally_level = 0;
  int64_t rally_winnings = 0;
  std::vector<SavedClubCar> club;
  std::vector<SavedCarHistory> car_history;
  // Offline Car Club (451_CLUB_SETTINGS: name, 4-char [TAG], shared Club Garage car IDs).
  std::string car_club_name;
  std::string car_club_tag;
  std::vector<int> car_club_shared;
  bool has_controls = false;
  ControlSettings controls;
  // Copies the carried fields (the ones the drive does not change) from `s`.
  void carry_from(const DriveSave& s) {
    level = s.level, event_unlocks = s.event_unlocks, hub_unlocks = s.hub_unlocks;
    has_rally = s.has_rally, num_rallies = s.num_rallies, num_rally_stages = s.num_rally_stages, club = s.club;
    num_rally_victories = s.num_rally_victories, rally_xp = s.rally_xp, rally_level = s.rally_level;
    rally_winnings = s.rally_winnings;
    rivals_beaten = std::max(rivals_beaten, s.rivals_beaten);
    rally_rivals_beaten = std::max(rally_rivals_beaten, s.rally_rivals_beaten);
    if (car_history.empty()) car_history = s.car_history;
    if (car_club_name.empty()) car_club_name = s.car_club_name;
    if (car_club_tag.empty()) car_club_tag = s.car_club_tag;
    if (car_club_shared.empty()) car_club_shared = s.car_club_shared;
  }
};

std::string save_to_text(const DriveSave& save);
// False with `error` on a missing header, a newer version, or a missing
// car, position or heading.
bool save_from_text(const std::string& text, DriveSave& out, std::string* error);

// Atomically replaces `dst_path` with an already-written sibling temp file
// `tmp_path` after flushing to storage (MoveFileExW with
// MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH on Windows; fsync +
// rename + parent-dir fsync on POSIX). Removes `tmp_path` on failure.
bool atomic_replace_file(const std::filesystem::path& tmp_path, const std::filesystem::path& dst_path,
                         std::string* error);

// Atomically writes `data` (`size` bytes) to `path` via `<path>.tmp`,
// flushing to storage before atomic replace. Creates parent directories
// and removes `<path>.tmp` on failure. Reusable for save.txt, garage.txt,
// and profile files.
bool atomic_write_file(const std::filesystem::path& path, const void* data, size_t size, std::string* error);
bool atomic_write_file(const std::filesystem::path& path, std::string_view text, std::string* error);

// Reads `path`; false with an empty `error` when the file does not exist.
bool read_save(const std::filesystem::path& path, DriveSave& out, std::string* error);
// Writes through a temporary file flushed and atomically renamed over `path`,
// so a crash leaves either the old save or the new one; creates the folder.
bool write_save(const std::filesystem::path& path, const DriveSave& save, std::string* error);

// Loads `save_path` into `out_save`. If the primary file is missing, empty, or
// corrupt/unparseable, scans backup snapshots in `backup_dir` newest-first
// (and any `<save_path>.bak` sibling), preserves any corrupt primary file as
// `<save_path>.corrupt`, atomically restores the newest valid backup over
// `save_path`, and reports its path in `recovered_from`.
bool load_save_with_recovery(const std::filesystem::path& save_path, const std::filesystem::path& backup_dir,
                             DriveSave& out_save, std::filesystem::path* recovered_from = nullptr,
                             std::string* error = nullptr);

// Convenience loader that tries `save_path` and automatically falls back to
// `backup_dir` (or `<save_dir>/../backups/saves` if omitted) and `<save_path>.bak`.
bool load_save_file(const std::filesystem::path& save_path, DriveSave& out_save, std::string* error = nullptr,
                    const std::filesystem::path& backup_dir = {}, std::filesystem::path* recovered_from = nullptr);

// Updates (or inserts) a per-car driving & race history entry in `history`.
void update_car_history(std::vector<SavedCarHistory>& history, int64_t garage_id, int64_t car_id,
                        double delta_distance_m, double delta_time_s, int64_t delta_winnings = 0,
                        int64_t delta_races = 0, int64_t delta_victories = 0, int64_t delta_podiums = 0);

// Returns true when `campaign_stage` denotes a pre-Free-Roam onboarding stage
// (opening cutscene, Viper highway drive, Corrado qualification, or registration).
bool campaign_stage_is_onboarding(std::string_view campaign_stage);

// Reconciles `save.intro_completed` and `save.campaign_stage` on Continue Career
// reload and returns the canonical car media name that should be loaded.
std::string resolve_campaign_reload_car(DriveSave& save, const std::string& selected_garage_car,
                                        const std::string& viper_media = "VIP_Viper_13",
                                        const std::string& starter_media = "VW_CorradoVR6_95");

}  // namespace drive
