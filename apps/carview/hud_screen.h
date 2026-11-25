// The driving HUD drawn from the game's own scene, 947_HUD (and the help bar
// from BUTTON_BAR_SCENE), on the scene-screen path of docs/UI_SCENE_SCREENS.md:
// the dial (GAUGE_ANALOG), the race blocks (EndConditionBlock, PlaceBlock,
// HUDTimes, PositionIndicators), the skill chain and popularity meter
// (DrivingSkill, PopularityBar), the event prompt (ActivationPrompt), the
// notifications (CombinedNotification), the countdown (I_321_Go), the wrong
// way sign and the minimap frame. Each draw returns false when the scene or
// its node is missing, and the caller falls back to the drawn HUD.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "scene_screen.h"

namespace carview {

struct DriveHud;
struct RaceHud;
struct SkillChainHud;
struct WorldMarkerHud;

inline constexpr std::string_view kHudScene = "947_HUD";
inline constexpr std::string_view kButtonBarScene = "BUTTON_BAR_SCENE";

// The strings the HUD binds: every one is checked against the tables by the
// corpus test.
namespace hud_text {
inline constexpr StringRef kKmh{"UnitStrings", "IDS_Units_KmPerHour", 0, "{0} km/h"};
inline constexpr StringRef kMph{"UnitStrings", "IDS_Units_Mph", 0, "{0} mph"};
inline constexpr StringRef kEnterNow{"Activities", "IDS_EnterNow", 0, "ENTER NOW"};
inline constexpr StringRef kSignMeUp{"Activities", "IDS_SignMeUp", 0, "SIGN ME UP!"};
inline constexpr StringRef kGo{"InGame", "IDS_3_2_1_GO", 0, "GO!"};
inline constexpr StringRef kProgress{"InGame", "IDS_Progress", 0, "PROGRESS"};
inline constexpr StringRef kPlace{"InGame", "IDS_Place", 0, "PLACE"};
inline constexpr StringRef kLaps{"InGame", "IDS_Laps", 0, "LAPS"};
inline constexpr StringRef kRace{"InGame", "IDS_Race", 0, "RACE"};
inline constexpr StringRef kLap{"InGame", "IDS_CurrentLapTimeLabel", 0, "LAP"};
inline constexpr StringRef kLast{"InGame", "IDS_Last", 0, "LAST"};
inline constexpr StringRef kBest{"InGame", "IDS_Best", 0, "BEST"};
}  // namespace hud_text

// The dial's scale, in thousands of rpm: the sweep spans 0..top, with a
// large hatch every `step`, a mid hatch at its half and small hatches at its
// quarters, and a numeral on each large hatch up to `last`. Read off the
// original's dial for two cars (a 7,000 rpm redline: 0..9 in ones, numerals
// to 8; an 8,000 rpm redline: 0..10 in twos, numerals to 10): two above the
// redline in thousands, in ones up to 9 and in twos above (Q73).
struct DialScale {
  int top = 9;
  int step = 1;
  int last = 8;
};
DialScale dial_scale(double redline_rpm);

// The 947_HUD frames: the scene's 1280x720 canvas scaled by the HUD scale and
// pinned to the screen edge its block sits on.
enum class HudAnchor : uint8_t { kTopLeft, kTop, kTopRight, kBottomLeft, kBottom, kBottomRight, kCentre };
SceneFrame hud_frame(HudAnchor a, float w, float h, float s);

// The dial (GAUGE_ANALOG) in frame `f`.
bool draw_hud_dial(ui::List& l, const DriveHud& st, const SceneFrame& f);

}  // namespace carview
