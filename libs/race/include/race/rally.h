// Horizon Rally stages (SPEC-V4-CONTENT, Horizon Rally): a stage's track
// row and splits from v4's database, the best split times, and the
// 49-driver timed field the player is ranked against.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace cars {
class PartsDb;
}

namespace race {

struct StageTrack {
  int64_t track_id = 0;
  int route_id = 0;
  double length = 0;                  // Tracks.Length, m
  double start_line = 0, finish_line = 1;  // Tracks.StartLine and FinishLine (fractions of the length)
  std::vector<double> splits;         // SplitPoints.Split, ascending (4 on a stage, none on the heats)
  std::vector<double> best;           // rallytracktimings.xml: the best time of each of the five splits, s
  double best_total() const;
};

struct RallyField {
  double skill_variance = 0;
  std::vector<std::pair<int, double>> drivers;  // AIPlayers id, skill 0..1
  std::array<std::pair<double, double>, 4> global{};  // (Min, Max) per difficulty, Easy to Pro
  std::vector<std::pair<int64_t, std::array<std::pair<double, double>, 4>>> overrides;  // by track
  std::pair<double, double> range(int64_t track_id, int difficulty) const;
};

// The route's track (environment 111) from `db` when open, else
// <state_root>/title-update-v4/media.zip: db/gamedb.slt, and
// gametunablesettings/rallytracktimings.xml.
bool load_stage_track(const std::filesystem::path& state_root, int route_id, StageTrack& out, std::string* error,
                      const cars::PartsDb* db = nullptr);
// gametunablesettings/rallyaidifficulty.xml from the same archive.
bool load_rally_field(const std::filesystem::path& state_root, RallyField& out, std::string* error);
bool parse_rally_field(std::string_view xml, RallyField& out, std::string* error);

// The field's stage times (SPEC-V4-CONTENT, The AI field and times; the
// interim rule): q = clamp(skill + u x variance, 0, 1) with u uniform in
// [-1, 1] from `seed`, f = Min + (Max - Min) q, time = best total / f.
// Difficulty 0 Easy to 3 Pro. One time per driver, in the field's order.
std::vector<double> field_times(const RallyField& field, const StageTrack& track, int difficulty, uint64_t seed);

// The player's place among the field (1-based; ties go to the player).
int rally_place(const std::vector<double>& field, double player_time);

}  // namespace race
