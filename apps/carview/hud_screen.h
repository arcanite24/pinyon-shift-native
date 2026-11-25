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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hud_notify.h"
#include "scene_screen.h"
#include "ui4_chrome.h"

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
// No string table has these: the engine's own.
inline constexpr StringRef kSplit{{}, {}, 0, "SPLIT", true};
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

// A race clock as the HUD shows it: "00:16.851".
std::string hud_clock(double seconds);

// The race blocks for a w x h screen at HUD scale `s`: laps or progress
// (EndConditionBlock), place (PlaceBlock), the timers (HUDTimes, with the
// engine's lap split line under them) and the standings
// (PositionIndicators: the leader and the rows round the player).
bool draw_hud_race(ui::List& l, const RaceHud& r, float w, float h, float s, bool timers = true);

// The skill chain's multiplier as the HUD shows it after its "X" ("1.1").
// The value itself is the chain's (SkillTracker); only its text is here.
std::string skill_multiplier_text(const SkillChainHud& sk);

// The skill chain (DrivingSkill: the score, the multiplier, the last skill
// and its points) and, when a chain banks or the rank changes, the
// popularity disc (PopularityBar).
bool draw_hud_skills(ui::List& l, const SkillChainHud& sk, float w, float h, float s, double time);

// An event's sponsor line ("HORIZON presents", Events/IDS_Sponsor_<id>), or
// empty; and its name with that sponsor cut from the front.
std::string event_sponsor(int64_t event_id);
std::string event_name_without_sponsor(std::string name, std::string_view sponsor);

// The event prompt (ActivationPrompt) while the car is in an activation
// zone: the sponsor line, the event's name with its type icon, and the call
// to action (SIGN ME UP! for a sponsored event, else ENTER NOW; the locked
// bar with the reason when the car is not eligible).
bool draw_hud_prompt(ui::List& l, const WorldMarkerHud& wm, float w, float h, float s);
bool hud_prompt_available();

// --- The help bar (BUTTON_BAR_SCENE) ---------------------------------------

// Draws ui4::button_bar()'s prompts on the scene's HelpButton0..6: each
// button's glyph (the active glyph set's, where BUTTON_LOGO sits), its
// label and its pill fitted to it, packed left to right from
// `left_x_canvas` (the first pill's left edge) as the original packs them;
// false when the scene is missing. help_bar_screen.cpp.
bool draw_help_bar(ui::List& l, const std::vector<ui4::Prompt>& items, float w, float h, float left_x_canvas);
// Makes ui4::button_bar() draw through draw_help_bar().
void install_scene_help_bar();

// --- Notifications (CombinedNotification, PopularityUnlock) ---------------

// A kind's strings: the tag over the panel (RankIcon), the title (TEXT2,
// the larger white line) and the message (TEXT1, the yellow line under it);
// an empty table is no line.
struct NotifyStrings {
  NotifyKind kind;
  StringRef tag, title, message;
  NotifyIcon icon = NotifyIcon::kMisc;
};
const std::vector<NotifyStrings>& notify_strings();
const NotifyStrings* notify_strings(NotifyKind kind);

// A notification's lines, formatted ("{0}", "{1}" from the arguments; a
// line whose format needs an argument not given is dropped).
struct NotifyText {
  std::string tag, title, message;
};
NotifyText notify_text(NotifyKind kind, const std::vector<std::string>& tag_args,
                       const std::vector<std::string>& title_args, const std::vector<std::string>& message_args);
// "{0}".."{9}" replaced from `args`; nullopt when one is missing.
std::optional<std::string> format_args(std::string_view fmt, const std::vector<std::string>& args);

// One notification as drawn: its kind and icon, its lines and the sponsor
// logo texture (SPONSORLOGOTILE/<NAME>), `age` seconds into `life`.
struct NotifyView {
  NotifyKind kind = NotifyKind::kEngine;
  NotifyIcon icon = NotifyIcon::kMisc;
  std::string tag, title, message, logo;
  double age = 0, life = 4.5;
  // A showcase's Show me! is drawn only when the engine acts on its button
  // (it does not yet, so the button would do nothing).
  bool action = false;
};
// Draws it (CombinedNotification, or PopularityUnlock for a showcase),
// lifted `lift` canvas px; returns the canvas height it takes (0: none up,
// or the scene is missing).
float draw_hud_notification(ui::List& l, const NotifyView& n, float w, float h, float s, float lift);
bool hud_notification_available();

}  // namespace carview
