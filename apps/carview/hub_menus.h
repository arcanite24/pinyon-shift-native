// The original's option menus on its generic menu scene, H_GENERIC
// (docs/UI_SCENE_SCREENS.md): the screen registry gives H_GENERIC to the
// Workshop, the Paintshop and Paint screens, the photo mode select, the
// upgrade-or-tune choice and Race Central, and the captures show MY PROFILE
// and OPTIONS as the same list (pairs 20, 21, 22 and 31: one column of
// tapes, the selected row pink, A SELECT / B BACK). The rows are the
// string tables' own; each opens the engine's screen for it.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "scene_screen.h"

namespace carview {

inline constexpr std::string_view kGenericMenuScene = "H_GENERIC";

// The menus drawn on H_GENERIC.
enum class HubMenu : uint8_t {
  kProfile,        // MY PROFILE (pause): OPTIONS / STATS / ACHIEVEMENTS / TEAM CREDITS
  kOptions,        // OPTIONS (pause SETTINGS, MY PROFILE > OPTIONS): CONTROLS / DIFFICULTY / HUD / AUDIO / VIDEO
  kPaintBooth,     // the paint shop: PAINT CAR / ADD DECALS / CREATE VINYL GROUP / MY VINYL GROUPS / FM4 VINYL GROUPS / CLEAR DESIGN
  kWorkshop,       // Dak's workshop: CHANGE CAR / UPGRADE CAR / PAINT CAR / BARN FINDS
  kUpgradeChoice,  // UPGRADE CAR: AUTO UPGRADE / CUSTOM UPGRADE
  kPhotoSelect,    // photo mode: TAKE PHOTO / VIEW PHOTO / PHOTO SHARE
};

// What a row opens.
enum class HubMenuAction : uint8_t {
  kOptions,
  kStats,
  kAchievements,
  kTeamCredits,
  kControls,
  kDifficulty,
  kHud,
  kAudio,
  kVideo,
  kPaintCar,
  kAddDecals,
  kCreateVinylGroup,
  kMyVinylGroups,
  kFm4VinylGroups,
  kClearDesign,
  kChangeCar,
  kUpgradeCar,
  kBarnFinds,
  kAutoUpgrade,
  kCustomUpgrade,
  kTakePhoto,
  kViewPhoto,
  kPhotoShare,
  // The engine's own (X in the help bar; no table has them).
  kEngineSettings,  // OPTIONS: the engine's settings (graphics, mods, ...)
};

namespace hub_menu_text {
// MY PROFILE (pairs 21, 26).
inline constexpr StringRef kOptions{"MyProfile", "IDS_Options", 0, "OPTIONS"};
inline constexpr StringRef kStats{"MyProfile", "IDS_Statistics", 0, "STATS"};
inline constexpr StringRef kAchievements{"MyProfile", "IDS_Achievements", 0, "ACHIEVEMENTS"};
inline constexpr StringRef kTeamCredits{"MyProfile", "IDS_Credits", 0, "TEAM CREDITS"};
// OPTIONS (pair 22).
inline constexpr StringRef kControls{"PauseMenu", "IDS_Controller", 0, "CONTROLS"};
inline constexpr StringRef kDifficulty{"MyProfile", "IDS_Assists_Difficulty", 0, "DIFFICULTY"};
inline constexpr StringRef kHud{"MyProfile", "IDS_HUD", 0, "HUD"};
inline constexpr StringRef kAudio{"MyProfile", "IDS_Audio", 0, "AUDIO"};
inline constexpr StringRef kVideo{"MyProfile", "IDS_Video", 0, "VIDEO"};
// The paint booth (pair 20).
inline constexpr StringRef kPaintCar{"Paint", "IDS_PaintCar", 0, "PAINT CAR"};
inline constexpr StringRef kAddDecals{"Paint", "IDS_AddDecals", 0, "ADD DECALS"};
inline constexpr StringRef kCreateVinylGroup{"Paint", "IDS_VinylGroups_NewVinylGroup", 0, "CREATE VINYL GROUP"};
inline constexpr StringRef kMyVinylGroups{"Paint", "IDS_VinylGroups_MyVinylGroup", 0, "MY VINYL GROUPS"};
inline constexpr StringRef kFm4VinylGroups{"Paint", "IDS_VinylGroups_FM4VinylGroup", 0, "FM4 VINYL GROUPS"};
inline constexpr StringRef kClearDesign{"Paint", "IDS_ClearDesign", 0, "CLEAR DESIGN"};
// The workshop (Workshop.str).
inline constexpr StringRef kChangeCar{"Workshop", "IDS_ChangeCar", 0, "CHANGE CAR"};
inline constexpr StringRef kUpgradeCar{"Workshop", "IDS_UpgradeTune", 0, "UPGRADE CAR"};
inline constexpr StringRef kWorkshopPaint{"Workshop", "IDS_Paint", 0, "PAINT CAR"};
inline constexpr StringRef kBarnFinds{"Workshop", "IDS_BarnFindCars", 0, "BARN FINDS"};
inline constexpr StringRef kAutoUpgrade{"Workshop", "IDS_AutoUpgrade", 0, "AUTO UPGRADE"};
inline constexpr StringRef kCustomUpgrade{"Workshop", "IDS_CustomUpgrade", 0, "CUSTOM UPGRADE"};
// Photo mode (pair 31).
inline constexpr StringRef kTakePhoto{"PauseMenu", "IDS_TakePhoto", 0, "TAKE PHOTO"};
inline constexpr StringRef kViewPhoto{"PauseMenu", "IDS_ViewPhoto", 0, "VIEW PHOTO"};
inline constexpr StringRef kPhotoShare{"PauseMenu", "IDS_SharePhoto", 0, "PHOTO SHARE"};
// Help bar.
inline constexpr StringRef kSelect{"HelpButtons", "IDS_Select", 0, "SELECT"};
inline constexpr StringRef kBack{"HelpButtons", "IDS_Back", 0, "BACK"};
inline constexpr StringRef kAdvanced{"HelpButtons", "IDS_Advanced", 0, "ADVANCED"};
// The engine's own.
inline constexpr StringRef kEngineSettings{{}, {}, 0, "ENGINE SETTINGS", true};
}  // namespace hub_menu_text

struct HubMenuRow {
  StringRef text;
  HubMenuAction action;
};

// A menu's rows in the original's order.
std::vector<HubMenuRow> hub_menu_rows(HubMenu m);
// The row X opens (the engine's extra), if the menu has one.
bool hub_menu_extra(HubMenu m, HubMenuRow& out);

// The open menus, innermost last (MY PROFILE > OPTIONS), each with its
// selection. B pops one; the stack empties back to the caller.
struct HubMenuStack {
  struct Level {
    HubMenu menu;
    int selected = 0;
  };
  std::vector<Level> levels;
  // The selection the HUD last drew and when it changed (the cross-fade).
  mutable int shown_selected = -1, shown_from = -1;
  mutable double shown_at = -1e9;

  bool open() const { return !levels.empty(); }
  void show(HubMenu m) { levels.assign(1, Level{m, 0}), shown_selected = -1; }
  void push(HubMenu m) { levels.push_back(Level{m, 0}), shown_selected = -1; }
  // False when the last menu closed.
  bool back() {
    if (!levels.empty()) levels.pop_back();
    shown_selected = -1;
    return !levels.empty();
  }
  void close() { levels.clear(); }
  HubMenu top() const { return levels.back().menu; }
  int selected() const { return levels.empty() ? 0 : levels.back().selected; }
  void move(int by);
  void select(int row);
  // The selected row's action (false: no menu open).
  bool action(HubMenuAction& out) const;
};

// One frame of a generic menu.
struct HubMenuHud {
  std::vector<std::string> rows;
  int selected = 0;
  std::vector<std::pair<std::string, std::string>> prompts;  // (button, label)
  std::string notice;  // the engine's notice line (empty: none)
};

namespace hub_menu_layout {
// The list's extra drop on H_GENERIC (canvas px, over race_layout::kMenuDy),
// measured on pairs 31, 21, 22 and 20 (3..6 rows): +4 px at four rows, 16.7
// px up per row more.
inline constexpr float list_dy(int rows) { return 4.0f - 16.7f * float(rows - 4); }
}  // namespace hub_menu_layout

// Fills the frame from the stack (texts resolved now).
void fill_hub_menu(const HubMenuStack& stack, HubMenuHud& out);
// Draws H_GENERIC's rows and the help bar; false when the scene is missing.
bool draw_hub_menu(ui::List& l, const HubMenuHud& m, float w, float h);

}  // namespace carview
