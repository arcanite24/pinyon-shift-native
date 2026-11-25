// The front end and the loading screen drawn from the game's own scenes
// (docs/UI_SCENE_SCREENS.md): Press Start and the SINGLE PLAYER /
// MULTIPLAYER menu on 120_P_PRE_SPLASH, and the tip card with PLEASE WAIT
// and its spinner on 2_C_INI_LOADING. Each draw returns false when its scene
// is missing (the caller falls back to the hand-drawn screen).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "scene_screen.h"
#include "title_menu.h"

namespace carview {

struct TitleScreenHud;

inline constexpr std::string_view kPreSplashScene = "120_P_PRE_SPLASH";
inline constexpr std::string_view kLoadingScene = "2_C_INI_LOADING";

// The strings the front end binds; the corpus test checks every table one.
namespace title_text {
inline constexpr StringRef kPressStart{"Main", "IDS_PressStart", 0, "PRESS START"};
inline constexpr StringRef kSinglePlayer{"Main", "IDS_Horizon", 0, "SINGLE PLAYER"};
inline constexpr StringRef kMultiplayer{"Main", "IDS_Multiplayer", 0, "MULTIPLAYER"};
inline constexpr StringRef kSelect{"HelpButtons", "IDS_Select", 0, "SELECT"};
inline constexpr StringRef kBack{"HelpButtons", "IDS_Back", 0, "BACK"};
inline constexpr StringRef kContinue{"HelpButtons", {}, 0xD2A8, "CONTINUE"};
inline constexpr StringRef kYes{"HelpButtons", {}, 0xEA08, "YES"};
inline constexpr StringRef kNo{"HelpButtons", {}, 0xD4A2, "NO"};
inline constexpr StringRef kPleaseWait{"Main", {}, 0xD8D2, "PLEASE WAIT"};
// MULTIPLAYER offline: the original's popup for a feature that needs Xbox
// LIVE (the same title it shows for MESSAGE CENTER offline).
inline constexpr StringRef kNoNetworkTitle{"LiveStrings", "IDS_Not_Connected_To_Live_PopUp_Title", 0,
                                           "NO NETWORK ACCESS"};
inline constexpr StringRef kNoNetworkBody{"LiveStrings", "IDS_Not_Connected_To_Live_PopUp_Body", 0,
                                          "This option is not available because you are not signed into Xbox LIVE."};
// A save that does not read at the profile step.
inline constexpr StringRef kProfileCorruptTitle{"Main", "IDS_ProfileCorrupt_Title", 0, "INVALID PROFILE"};
inline constexpr StringRef kProfileCorruptBody{"Main", "IDS_ProfileCorrupt_Desc", 0,
                                               "The Forza Horizon save that you have selected is damaged and "
                                               "cannot be loaded."};
// No table has these: the engine's own entries behind EXTRAS.
inline constexpr StringRef kExtras{{}, {}, 0, "EXTRAS", true};
inline constexpr StringRef kNewCareer{{}, {}, 0, "NEW CAREER", true};
inline constexpr StringRef kFreeRoam{{}, {}, 0, "FREE ROAM", true};
inline constexpr StringRef kSettings{{}, {}, 0, "SETTINGS", true};
inline constexpr StringRef kQuit{{}, {}, 0, "QUIT", true};
inline constexpr StringRef kNewCareerTitle{{}, {}, 0, "START NEW CAREER?", true};
inline constexpr StringRef kNewCareerBody{{}, {}, 0, "This will overwrite your saved career and start again.", true};
}  // namespace title_text

// A title row's text.
StringRef title_row_text(TitleMenu::Row r);

// SPLASH_MENU's row `slot` (0..4): its button and the group holding its
// TEXT_BUTTON and Tape. Title update v4 (the scene the game mounts) put
// SPLASH_MENU_BUTTON_SPECIAL in the disc's SPLASH_MENU_BUTTON1's place: its
// NormalMode is the plain row, its SpecialMode a medal-badged one (Q138).
// -1 where missing.
struct TitleRowNodes {
  int button = -1;
  int group = -1;
  int special = -1;  // v4's SpecialMode, hidden
};
TitleRowNodes title_row_nodes(const SceneDraw& sd, int slot);

// Measured placements (1280x720 canvas) of the original's captures. The
// scene's slides do not move its Content group (the logo and PRESS START),
// which rests off screen right (Q137).
namespace title_layout {
// 01_title_pressstart_orig.png: the logo's pink plate at 922..1180 x 533..594
// and PRESS START at 988..1168 x 611..628 (both matched to the pixel).
inline constexpr float kContentDx = -405.0f, kContentDy = 248.0f;
inline constexpr float kPressStartScale = 1.0f;  // TitleTape's size step (its rest scale is 0.5)
// 02_title_menu_orig.png: SINGLE PLAYER (selected) at 865..1138 x 564..588 on
// its tape at 858..1154 x 559..594; MULTIPLAYER (unselected) at 883..1129 x
// 602..625, moved in 28 px, a light grey (195). The rows' colours and the
// selected row's size step (its Focused slide's pulse, 0.7 to 0.722 at
// 1.04..1.27 s of 2.5) come from their slides; the offsets do not (Q138).
// kRowUnselected is the look without a timeline (the scene's Blurred: 200).
inline constexpr float kRowSelectedDx = 8.0f;
inline constexpr float kRowUnselectedDx = 28.0f;
inline constexpr uint32_t kRowUnselected = ui::rgba(195, 195, 195);
}  // namespace title_layout

// Draws the front end (no PressStart movie: the caller draws it under).
bool draw_title_screen(ui::List& l, const TitleScreenHud& ts, float w, float h);

// One frame of the loading screen.
struct LoadingSceneHud {
  std::string tip_title;    // POPULARITY, ALI HOWARD
  std::string tip_body;     // the tip's message
  std::string event_title;  // the event's name on event loads (engine request); empty: none
  std::string backdrop;     // LOADING/BGLOADINGIMAGES/<image>
  double t = 0.0;           // seconds on screen (the spinner)
};

namespace loading_layout {
// 03_loading_tip_orig.png: PLEASE WAIT's right edge at x 1150 and the
// spinner's middle at 1172,655; the scene's Info group rests at 758,361.
inline constexpr float kInfoDx = 414.0f, kInfoDy = 294.0f;
// The tip's texts against their tapes and pills (the scene's text boxes
// anchor otherwise than the engine draws them): the title's left edge at
// x 118 over its tape, each line inside its pill.
inline constexpr float kTipTitleDx = -21.0f, kTipTitleDy = 35.0f;
inline constexpr float kTipLineDy = 17.0f;
// The event's name (Stat_Text, 54 px): set by eye in the middle of its
// tapes (no original frame shows it, Q140).
inline constexpr float kStatDx = kTipTitleDx, kStatDy = 38.0f;
// The spinner's turn (radians a second): one turn a second, set by eye (Q140).
inline constexpr float kSpinnerRate = 6.2831853f;
}  // namespace loading_layout

bool draw_loading_scene(ui::List& l, const LoadingSceneHud& hud, float w, float h);

// The LoadingDefs.xml group of a load into an event (CareerEvent style and
// level): a Festival race its level's festival_N (N = level + 1; that
// wristband's star rival's tips: 11b_loading_event_orig.png, a Green event,
// shows Ali's), a qualifier festival_heats, a street race streetrace, a
// showcase showcase, the rest festivalevent. Loads into free roam use
// freeroam.
std::string loading_group_for_event(int style, int level);

// The scenes and node paths the two screens bind (the corpus test checks
// each).
const std::vector<std::pair<std::string_view, std::vector<const char*>>>& frontend_scene_nodes();

}  // namespace carview
