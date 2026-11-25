// The original's option pages on 300_I_OPT_ASSISTS_DIFFICULTY: the title
// tape, up to ten spinner rows (label, value between arrows, the pink bar on
// the selected row), the DESCRIPTION panel and the help bar.
// 24_options_difficulty_orig.png (DIFFICULTY: GAME DIFFICULTY .. OPPONENT
// DIFFICULTY and TOTAL RACE BONUS) and 25_options_hud_orig.png (HUD: CAMERA
// VIEW .. POPULARITY AND SKILLS) are the same scene. The rows are the
// engine's settings rows (settings_page.h) by their own names; the engine's
// other rows stay on its own pages.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "scene_screen.h"

namespace carview {

inline constexpr std::string_view kOptionsListScene = "300_I_OPT_ASSISTS_DIFFICULTY";

enum class OptionsPage : uint8_t { kNone, kDifficulty, kHud };

struct OptionsRowSpec {
  const char* id;  // the settings row's own name ("braking")
  StringRef label;
  StringRef description;
};

namespace options_text {
inline constexpr StringRef kDifficulty{"ScreenTitles", "IDS_Assists", 0, "DIFFICULTY"};
inline constexpr StringRef kHud{"ScreenTitles", "IDS_HUDOptions", 0, "HUD"};
inline constexpr StringRef kDescription{"GameOptions2", "IDS_Description", 0, "DESCRIPTION"};
inline constexpr StringRef kTotalRaceBonus{"Assists", "IDS_Total_Earnings", 0, "TOTAL RACE BONUS"};
inline constexpr StringRef kSave{"HelpButtons", "IDS_Save", 0, "SAVE"};
inline constexpr StringRef kCancel{"HelpButtons", "IDS_Cancel", 0, "CANCEL"};
inline constexpr StringRef kResetDefault{"HelpButtons", "IDS_ResetDefault", 0, "RESET TO DEFAULT"};
}  // namespace options_text

// The page's rows in the original's order; empty for kNone.
const std::vector<OptionsRowSpec>& options_rows(OptionsPage p);
// The value as the original words it ("abs on" -> ABS ON, "insane" ->
// EXPERT); an engine-only value is the engine's, upper-cased.
std::string options_value_text(std::string_view id, std::string_view engine_value);

struct OptionsHud {
  std::string title;
  std::vector<std::pair<std::string, std::string>> rows;  // (label, value)
  int selected = 0;
  std::string description_title, description;
  std::string bonus;  // "+40%" under the rows (DIFFICULTY); empty: none
  std::vector<std::pair<std::string, std::string>> prompts;
  std::string notice;
};

const std::vector<const char*>& options_list_nodes();
bool draw_options_list(ui::List& l, const OptionsHud& o, float w, float h);

}  // namespace carview
