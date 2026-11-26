// The pause map's screen drawn from the game's own scenes
// (docs/UI_SCENE_SCREENS.md, "The map"): H_MAP over the map raster (the
// cursor, the icon's DETAIL popup, the player bar, the help bar), the FILTER
// popup on H_FIXED_SETTING_LIST and the fast travel dialogs on
// H_MESSAGE_POPUP.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hud.h"
#include "map_filter.h"
#include "scene_screen.h"

namespace carview {

struct DriveHud;
struct EventSummaryHud;

inline constexpr std::string_view kMapScene = "H_MAP";
inline constexpr std::string_view kMapFilterScene = "H_FIXED_SETTING_LIST";
inline constexpr std::string_view kMapDialogScene = "H_MESSAGE_POPUP";

// The strings the map binds; every one is checked against the tables by the
// corpus test.
namespace map_text {
inline constexpr StringRef kSetRoute{"HelpButtons", "IDS_SetDestination", 0, "SET ROUTE"};
inline constexpr StringRef kClearRoute{"HelpButtons", "IDS_ClearDestination", 0, "CLEAR ROUTE"};
inline constexpr StringRef kExitMap{"HelpButtons", "IDS_ExitMap", 0, "EXIT MAP"};
inline constexpr StringRef kFastTravelButton{"HelpButtons", "IDS_FastTravel", 0, "FAST TRAVEL ({0})"};
inline constexpr StringRef kDetails{"HelpButtons", "IDS_Details", 0, "DETAILS"};
inline constexpr StringRef kLeaderboard{"HelpButtons", "IDS_ShowLeaderboard", 0, "LEADERBOARD"};
inline constexpr StringRef kFilter{"HelpButtons", "IDS_Filter_Map", 0, "FILTER"};
inline constexpr StringRef kToggle{"HelpButtons", "IDS_Toggle", 0, "TOGGLE"};
inline constexpr StringRef kBack{"HelpButtons", "IDS_Back", 0, "BACK"};
inline constexpr StringRef kRestrictions{"HelpButtons", {}, 0xAC80, "RESTRICTIONS"};  // as race_text
inline constexpr StringRef kOk{"HelpButtons", "IDS_OK", 0, "OK"};
inline constexpr StringRef kCancel{"HelpButtons", "IDS_Cancel", 0, "CANCEL"};
inline constexpr StringRef kFilterTitle{"InGame", "IDS_FilterMap_Title", 0, "FILTER"};
inline constexpr StringRef kFastTravelTitle{"PauseMenu", "IDS_FastTravel", 0, "FAST TRAVEL"};
inline constexpr StringRef kFastTravelAsk{"PauseMenu", "IDS_FastTravelDesc", 0,
                                          "Are you sure you want to Fast Travel here for {0} credits?"};
inline constexpr StringRef kFastTravelAskFree{"PauseMenu", "IDS_FastTravelDesc_Free", 0,
                                              "Are you sure you want to Fast Travel here?"};
inline constexpr StringRef kFastTravelNoCredits{"PauseMenu", "IDS_FastTravelDesc_Failed", 0,
                                                "You do not have enough credits to fast travel here."};
inline constexpr StringRef kCredits{"UnitStrings", "IDS_Units_Credits", 0, "{0} Cr"};
// The post-mission card (InGame / GasStation).
inline constexpr StringRef kCardFastTravel{"InGame", "IDS_FastTravel", 0, "FAST TRAVEL"};
inline constexpr StringRef kCardDiscount{"InGame", "IDS_Discount", 0, "{0}% OFF"};
inline constexpr StringRef kCardAvailableAt{"InGame", "IDS_AvailableAtHub", 0, "AVAILABLE AT {0}"};
inline constexpr StringRef kCardEarned{"GasStation", "IDS_MissionCompletedDiscount", 0, "FAST TRAVEL DISCOUNT EARNED"};
}  // namespace map_text

// The filter row's label.
inline StringRef map_filter_text(MapFilterRow r) {
  return StringRef{"InGame", map_filter_string_id(r), 0, map_filter_fallback(r)};
}

// The DETAIL group the selected icon shows (H_MAP's MAP_OVERLAY/DETAIL).
enum class MapPopupKind : uint8_t {
  kNone = 0,
  kCareerRace,  // name; class, points and prize chips
  kExhibition,  // name; prize
  kStreetRace,  // STREET RACE HUB; the hub's name; a medal per race
  kGeneric,     // a heading and a second line (facilities, speed traps)
  kSimple,      // one line (the player's name)
  kNemesis,     // name; class and a line
  kOutpost,     // HORIZON OUTPOST; the name; a pip per mission and the discount
};

// A medal pip (StreetRaceHUB*/<state>).
enum class MapPip : int8_t { kUnavailable = -1, kAvailable = 0, kGold, kSilver, kBronze };

struct MapPopupHud {
  MapPopupKind kind = MapPopupKind::kNone;
  float x = 640.0f, y = 360.0f;  // the icon on the 1280x720 canvas
  std::string title;              // TEXT_NAME
  std::string line;               // the second row (TEXT_COMPLETE)
  std::string class_name;         // CareerRace / Nemesis class chip ("A"); empty: none
  std::string points;             // "110 PTS"
  std::string money;              // "5,000 Cr"
  std::vector<MapPip> pips;       // StreetRace (up to 10) / Outpost (3)
  std::string discount_label;     // Outpost "Discount:"
  std::string discount_value;     // Outpost "0%"
};

enum class MapDialog : uint8_t { kNone = 0, kConfirm, kNoCredits };

struct MapScreenHud {
  bool active = false;
  double age = 0.0;                    // seconds since the map opened
  std::array<float, 2> cursor{640.0f, 360.0f};  // canvas
  bool locked = false;                 // the cursor is on an icon (the lock-on ring, no cross-hair)
  MapPopupHud popup;
  std::vector<std::pair<std::string, std::string>> prompts;  // (button, label)
  bool filter_open = false;
  int filter_row = 0;
  int filter_first = -1;  // the first row shown; -1: as scrolled down to filter_row
  MapFilterSet filter;
  MapDialog dialog = MapDialog::kNone;
  std::string dialog_body;
  std::string band_texture;            // the player bar's wristband (empty: from the HUD)
  std::string tip;                     // the top-left (i) tip (the career's locked map)
  // The help bar replaced by a spinner and PLEASE WAIT (the career's map
  // after A on the heats, before it closes by itself; first30e r7 n0).
  bool please_wait = false;
  // X DETAILS on an event: its ticket over the map (no cursor or popup), with
  // A SET ROUTE, B BACK and Y RESTRICTIONS (map_popup_event_details_orig.png).
  const EventSummaryHud* details = nullptr;
};

// The canvas -> screen fit of the map's 1280x720 layout.
SceneFrame map_screen_frame(float w, float h);

// Draws the map's screens over the map raster; false when H_MAP is missing.
bool draw_map_screen(ui::List& l, const DriveHud& st, const MapScreenHud& ms, float w, float h);

// The popup rows' canvas offsets from the icon, measured on the original
// (map_popup_event_orig.png, map_popup_outpost_orig.png, map_open_orig.png):
// the stack ends about 60 px above the icon, 25 px a row.
inline constexpr float kPopupLastRowDy = -60.0f;
inline constexpr float kPopupRowPitch = 25.0f;

}  // namespace carview
