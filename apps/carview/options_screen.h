// The original's option pages: the title tape, the spinner rows (label,
// value between arrows, the pink bar on the selected row), the DESCRIPTION
// panel and the help bar.
// - 300_I_OPT_ASSISTS_DIFFICULTY: 24_options_difficulty_orig.png (DIFFICULTY:
//   GAME DIFFICULTY .. OPPONENT DIFFICULTY and TOTAL RACE BONUS) and
//   25_options_hud_orig.png (HUD: CAMERA VIEW .. POPULARITY AND SKILLS).
// - 837_I_AUDIO_OPTIONS: AUDIO, two slider rows (the volumes) over the
//   spinners.
// - 831_C_OPT_CONTROLLER: CONTROLS (23_options_controls_orig.png), four
//   spinners over the controller's diagram, its buttons labelled with the
//   layout's actions.
// The rows are the engine's settings rows (settings_page.h) by their own
// names; the engine's other rows stay on its own pages.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "scene_screen.h"

namespace carview {

inline constexpr std::string_view kOptionsListScene = "300_I_OPT_ASSISTS_DIFFICULTY";
inline constexpr std::string_view kAudioOptionsScene = "837_I_AUDIO_OPTIONS";
inline constexpr std::string_view kControlsScene = "831_C_OPT_CONTROLLER";

enum class OptionsPage : uint8_t { kNone, kDifficulty, kHud, kAudio, kControls };

// The page's scene; empty for kNone.
std::string_view options_scene(OptionsPage p);

struct OptionsRowSpec {
  const char* id;  // the settings row's own name ("braking"); empty: no engine row
  StringRef label;
  StringRef description;
  bool slider = false;  // a slider row (837's volumes): steps stop at 0% and 100%
};

namespace options_text {
inline constexpr StringRef kDifficulty{"ScreenTitles", "IDS_Assists", 0, "DIFFICULTY"};
inline constexpr StringRef kHud{"ScreenTitles", "IDS_HUDOptions", 0, "HUD"};
inline constexpr StringRef kDescription{"GameOptions2", "IDS_Description", 0, "DESCRIPTION"};
inline constexpr StringRef kTotalRaceBonus{"Assists", "IDS_Total_Earnings", 0, "TOTAL RACE BONUS"};
inline constexpr StringRef kSave{"HelpButtons", "IDS_Save", 0, "SAVE"};
inline constexpr StringRef kCancel{"HelpButtons", "IDS_Cancel", 0, "CANCEL"};
inline constexpr StringRef kResetDefault{"HelpButtons", "IDS_ResetDefault", 0, "RESET TO DEFAULT"};
inline constexpr StringRef kAdvanced{"HelpButtons", "IDS_Advanced", 0, "ADVANCED"};
inline constexpr StringRef kAudio{"MyProfile", "IDS_Audio", 0, "AUDIO"};
inline constexpr StringRef kControls{"PauseMenu", "IDS_Controller", 0, "CONTROLS"};
}  // namespace options_text

// The page's rows in the original's order; empty for kNone.
const std::vector<OptionsRowSpec>& options_rows(OptionsPage p);
// The page's title (DIFFICULTY, HUD, AUDIO, CONTROLS).
std::string options_title(OptionsPage p);
// The value as the original words it ("abs on" -> ABS ON, "insane" ->
// EXPERT, "layout 2" -> DEFAULT LAYOUT 2); an engine-only value is the
// engine's, upper-cased.
std::string options_value_text(std::string_view id, std::string_view engine_value);
// A slider row's value (the settings row's choice labels, "75%") one step
// up or down, stopping at the ends; the value itself at an end or when it
// is not a percentage.
std::string options_slider_step(const std::vector<std::string>& choices, std::string_view value, int by);
// RADIO STATION's value for the station playing (the radio's "Radio1" ->
// HORIZON BASS ARENA, MyProfile.str's names; a user station by its name).
std::string options_station_text(std::string_view station);
// A percentage's fraction ("75%" -> 0.75); -1 when it is not one.
float options_percent(std::string_view value);

// The CONTROLS diagram's labels, by 831's Group_TEXT names (LT, LB, BACK,
// START, LTS, LTSB, DPAD, LEFT, RIGHT, DOWN, RT, RB, Y, B, X, A, RTS, RTSB):
// the engine's layout (drive::default_controller_layout), the handbrake and
// clutch swapped or not. shifting: 0 automatic, 1 manual, 2 manual with
// clutch; the buttons a shifting mode leaves free read NOT USED.
std::vector<std::pair<std::string, std::string>> controls_pad_labels(int layout_id, bool swap_handbrake_clutch,
                                                                      int shifting);

struct OptionsHud {
  OptionsPage page = OptionsPage::kDifficulty;
  std::string title;
  std::vector<std::pair<std::string, std::string>> rows;  // (label, value)
  int selected = 0;
  std::string description_title, description;
  std::string bonus;  // "+40%" under the rows (DIFFICULTY); empty: none
  std::vector<std::pair<std::string, std::string>> pad;  // CONTROLS: controls_pad_labels
  std::vector<std::pair<std::string, std::string>> prompts;
  std::string notice;
};

const std::vector<const char*>& options_list_nodes();
// 837's and 831's nodes the pages bind.
const std::vector<const char*>& options_audio_nodes();
const std::vector<const char*>& options_controls_nodes();
bool draw_options_list(ui::List& l, const OptionsHud& o, float w, float h);

}  // namespace carview
