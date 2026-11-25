// The pause menu drawn from the game's own scene, 925_PAUSE_MENU (the pilot
// of docs/UI_SCENE_SCREENS.md), with its two screens: Sponsor Challenges
// (H_SPONSOR_CHALLENGE_STATUS) and Message Centre (802_MESSAGE_CENTER).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hud.h"
#include "scene_screen.h"

namespace carview {

struct DriveHud;
struct SponsorChallengeRow;

inline constexpr std::string_view kPauseScene = "925_PAUSE_MENU";
inline constexpr std::string_view kSponsorScene = "H_SPONSOR_CHALLENGE_STATUS";
inline constexpr std::string_view kMessageScene = "802_MESSAGE_CENTER";

// A pause row's (or extra's) text: the PauseMenu table for the retail rows,
// an engine label for the extras.
StringRef pause_row_text(PauseMenu::Action a, bool in_race);

// The strings the three screens bind (titles, help prompts, headers): every
// one is checked against the tables by the corpus test.
namespace pause_text {
inline constexpr StringRef kSelect{"HelpButtons", "IDS_Select", 0, "SELECT"};
inline constexpr StringRef kResume{"HelpButtons", "IDS_Resume", 0, "RESUME"};
inline constexpr StringRef kBack{"HelpButtons", "IDS_Back", 0, "BACK"};
inline constexpr StringRef kCollectWristband{"InGame", "IDS_CollectWristband", 0, "COLLECT {0} WRISTBAND"};
inline constexpr StringRef kCredits{"UnitStrings", "IDS_Units_Credits", 0, "{0} Cr"};
inline constexpr StringRef kSponsorTitle{"SponsorChallengeScreen", "IDS_ScreenTitle", 0, "SPONSOR CHALLENGES"};
inline constexpr StringRef kPopularityChart{"SponsorChallengeScreen", {}, 0x17D6, "POPULARITY CHART POSITION"};
inline constexpr StringRef kRank{"SponsorChallengeScreen", "IDS_Rank", 0, "RANK"};
inline constexpr StringRef kReward{"SponsorChallengeScreen", {}, 0x36B1, "REWARD"};
inline constexpr StringRef kDetails{"SponsorChallengeScreen", {}, 0xFF40, "DETAILS"};
inline constexpr StringRef kChallengeCompleted{"SponsorChallengeScreen", "IDS_ChallengeCompleted", 0,
                                               "CHALLENGE COMPLETED"};
inline constexpr StringRef kMessageTitle{"ScreenTitles", {}, 0x9864, "MESSAGE CENTER"};
inline constexpr StringRef kNoMessages{"MessageCenter", {}, 0xD6BA, "NO MESSAGES"};
inline constexpr StringRef kYouHaveNoMessages{"MessageCenter", {}, 0xEC26, "YOU HAVE NO MESSAGES"};
inline constexpr StringRef kCheckBackLater{"MessageCenter", {}, 0x27F8, "Please check back later."};
// No string table has these: the engine's own.
inline constexpr StringRef kExtras{{}, {}, 0, "EXTRAS", true};
}  // namespace pause_text

// Everything the pause screens draw for one frame.
struct PauseScreenHud {
  PauseMenu::Screen screen = PauseMenu::Screen::kList;
  bool in_race = false;
  std::vector<std::string> rows;  // the current list's texts (rows or extras)
  int selected = 0;
  int from = -1;          // the row the selection left (cross-fading out)
  double fade = 1.0;      // 0..1 of the selection cross-fade
  std::vector<std::string> extras_rows;  // (the extras list is drawn over the rows)
  const std::vector<SponsorChallengeRow>* sponsors = nullptr;
  int sponsor_selected = 0;
  std::string band_texture;  // WRISTBANDS/<COLOUR>; empty: the scene's
};

// Binds a scene's PlayerBar (the pause menu's, shared by H_EVENT_SUMMARY):
// the wristband objective, the car and its class, the credits.
void bind_player_bar(SceneScreen& sc, const DriveHud& st, const std::string& band_texture = {});

// Fills `out` from the menu (the rows' texts resolved now).
void fill_pause_screen(const PauseMenu& menu, double now, PauseScreenHud& out);

// The selection cross-fade (measured on the original: about three frames)
// and the focused row's size step (MAP's text 116x38 unfocused in
// 29_message_center_offline_orig.png, 140x46 focused in 07_pause_menu_orig.png).
inline constexpr double kPauseFadeSeconds = 0.10;
inline constexpr float kPauseFocusScale = 1.21f;

// Binds a scene's PlayerBar (the pause menu's and H_MAP's share its node
// names): the wristband objective, the car and the credits. `band_texture`:
// WRISTBANDS/<COLOUR>; empty: from the HUD.
void bind_pause_player_bar(SceneScreen& sc, const DriveHud& st, const std::string& band_texture);

// Draws the pause screens over the game (null scene: nothing, false).
bool draw_pause_screen(ui::List& l, const DriveHud& st, const PauseScreenHud& ps, float w, float h);

}  // namespace carview
