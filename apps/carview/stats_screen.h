// MY PROFILE > STATS on H_STATISTICS (the title update's scene replaces the
// disc's): the STATS tape, the tab between LB and RB, fourteen stat rows
// (label, value, GAME PROGRESS's bar) and the help bar.
// 26_my_profile_stats_orig.png is the GENERAL tab. The tabs are the three
// MyProfile.str names the title's code lists (GENERAL, RECORDS, TIME); which
// rows RECORDS and TIME hold is the engine's reading (SPEC_QUESTIONS Q134).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "scene_screen.h"

namespace carview {

struct DriveHud;

inline constexpr std::string_view kStatsScene = "H_STATISTICS";

enum class StatsTab : uint8_t { kGeneral, kRecords, kTime };
inline constexpr int kStatsTabCount = 3;

namespace stats_text {
inline constexpr StringRef kTitle{"MyProfile", "IDS_Statistics", 0, "STATS"};
inline constexpr StringRef kGeneral{"MyProfile", "IDS_GeneralStats", 0, "GENERAL"};
inline constexpr StringRef kRecords{"MyProfile", "IDS_RecordsStats", 0, "RECORDS"};
inline constexpr StringRef kTime{"MyProfile", "IDS_TimeStats", 0, "TIME"};
inline constexpr StringRef kBack{"HelpButtons", "IDS_Back", 0, "BACK"};
}  // namespace stats_text

// What the rows show. The engine does not count the ultimate skills or the
// time in menus and shops yet: those stay zero.
struct StatsData {
  double game_progress = 0;  // 0..1
  double time_driven_s = 0;
  int64_t victories = 0, podiums = 0, races = 0, cars_in_garage = 0;
  int64_t total_winnings = 0, garage_value = 0, parts_value = 0;
  int popularity_rank = 0;  // 1 the top; 0: none
  int64_t max_skills_score = 0;
  std::array<int64_t, 13> ultimate_skills{};  // the order of stats_rows(kRecords)
  double time_festival_s = 0, time_street_s = 0, time_showcase_s = 0;
};

struct StatRow {
  std::string label, value;
  double progress = -1;  // 0..1: the row's bar; < 0: none
};

std::string stats_tab_name(StatsTab t);
std::vector<StatRow> stats_rows(StatsTab t, const StatsData& d);
// "16 h 45:17" (the original's TIME DRIVEN).
std::string stats_time_text(double seconds);
// "166th".
std::string stats_ordinal(int n);

struct StatsHud {
  StatsTab tab = StatsTab::kGeneral;
  std::vector<StatRow> rows;
  std::vector<std::pair<std::string, std::string>> prompts;
};

const std::vector<const char*>& stats_nodes();
// Hits: the LB and RB pills (ui::kHitTab 0, 1) and the help bar.
bool draw_stats_screen(ui::List& l, const DriveHud& st, const StatsHud& s, float w, float h);

}  // namespace carview
