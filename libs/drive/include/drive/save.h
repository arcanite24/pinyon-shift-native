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

#include <array>
#include <cstdint>
#include <utility>
#include <filesystem>
#include <string>
#include <vector>

namespace drive {

struct SavedBarn {
  std::string name;
  int status = 3;
  double rumour_at = 0, found_at = 0;
};

struct SavedClubCar {
  int64_t car_id = 0;
  std::vector<std::pair<uint32_t, bool>> challenges;  // key, completed
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
  std::vector<int> garage;
  bool owned_garage = false;
  // The career.
  int64_t credits = 0, total_winnings = 0;
  int wristband_points = 0;
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
  // Copies the carried fields (the ones the drive does not change) from `s`.
  void carry_from(const DriveSave& s) {
    level = s.level, event_unlocks = s.event_unlocks, hub_unlocks = s.hub_unlocks;
    has_rally = s.has_rally, num_rallies = s.num_rallies, num_rally_stages = s.num_rally_stages, club = s.club;
    num_rally_victories = s.num_rally_victories, rally_xp = s.rally_xp, rally_level = s.rally_level;
    rally_winnings = s.rally_winnings;
  }
};

std::string save_to_text(const DriveSave& save);
// False with `error` on a missing header, a newer version, or a missing
// car, position or heading.
bool save_from_text(const std::string& text, DriveSave& out, std::string* error);

// Reads `path`; false with an empty `error` when the file does not exist.
bool read_save(const std::filesystem::path& path, DriveSave& out, std::string* error);
// Writes through a temporary file renamed over `path`, so a crash leaves
// the old save or the new one; creates the folder.
bool write_save(const std::filesystem::path& path, const DriveSave& save, std::string* error);

}  // namespace drive
