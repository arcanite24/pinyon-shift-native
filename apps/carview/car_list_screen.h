// The original's car list, 108_C_BUY_CARSELECT (v4 replaces the disc's):
// the title tape, a strip of car tiles with the cursor on its fourth slot,
// the selected car's details (name, year and make, class and PI, drivetrain,
// SPEED .. BRAKING bars) and the player bar. The title shows it for the
// Autoshow's CAR SELECT, an event's HORIZON RECOMMENDS (19_autoshow_orig.png),
// the garage (MY GARAGE), its BARN FINDS and the trade-in list; the engine
// draws all of them on it, parameterised by title, tiles and help bar.
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

inline constexpr std::string_view kCarListScene = "108_C_BUY_CARSELECT";

namespace car_list_text {
// Titles.
inline constexpr StringRef kCarSelect{"ScreenTitles", "IDS_BuyNewCar", 0, "CAR SELECT"};
inline constexpr StringRef kHorizonRecommends{"ScreenTitles", "IDS_CareerEventBuyCarSelect", 0, "HORIZON RECOMMENDS"};
inline constexpr StringRef kManufacturerSelect{"ScreenTitles", "IDS_Manufacturer_Select", 0, "MANUFACTURER SELECT"};
inline constexpr StringRef kMyGarage{"carflow", "IDS_Garage_View_Title", 0, "MY GARAGE"};
inline constexpr StringRef kBarnFinds{"ScreenTitles", "IDS_SelectBarnFindCar", 0, "BARN FINDS"};
inline constexpr StringRef kTradeIn{"ScreenTitles", "IDS_TradeInCars", 0, "TRADE-IN CARS"};
// The details' bars.
inline constexpr StringRef kSpeed{"carflow", "IDS_Stat_Speed", 0, "SPEED"};
inline constexpr StringRef kHandling{"carflow", "IDS_Stat_Handling", 0, "HANDLING"};
inline constexpr StringRef kAcceleration{"carflow", "IDS_Stat_Accel", 0, "ACCELERATION"};
inline constexpr StringRef kLaunch{"carflow", "IDS_Stat_Launch", 0, "LAUNCH"};
inline constexpr StringRef kBraking{"carflow", "IDS_Stat_Braking", 0, "BRAKING"};
// Help bar (19_autoshow_orig.png: A SELECT, B BACK, Y TOGGLE, SEARCH).
inline constexpr StringRef kSelect{"HelpButtons", "IDS_Select", 0, "SELECT"};
inline constexpr StringRef kBack{"HelpButtons", "IDS_Back", 0, "BACK"};
inline constexpr StringRef kToggle{"HelpButtons", "IDS_Toggle", 0, "TOGGLE"};
inline constexpr StringRef kSearch{"HelpButtons", "IDS_Search", 0, "SEARCH"};
inline constexpr StringRef kSellCar{"carflow", "IDS_Sell_Car_Action", 0, "SELL CAR"};
inline constexpr StringRef kCredits{"UnitStrings", "IDS_Units_Credits", 0, "{0} Cr"};
// Barn Finds (Barnfind.str): the tiles' states, the popups, the map link.
inline constexpr StringRef kBarnLocked{"Barnfind", "IDS_CarState_Locked", 0, "NOT DISCOVERED"};
inline constexpr StringRef kBarnWrecked{"Barnfind", "IDS_CarState_Wrecked", 0, "NOT READY"};
inline constexpr StringRef kBarnReady{"Barnfind", "IDS_CarState_Ready", 0, "READY TO COLLECT"};
inline constexpr StringRef kBarnCollected{"Barnfind", "IDS_CarState_Collected", 0, "COLLECTED"};
inline constexpr StringRef kCollectTitle{"Barnfind", "IDS_Collect_Popup_Title", 0, "COLLECT CAR"};
inline constexpr StringRef kCollectMessage{"Barnfind", "IDS_Collect_Popup_Message", 0,
                                           "This Barn Find will now be added to your garage."};
inline constexpr StringRef kWreckedTitle{"Barnfind", "IDS_Wrecked_Popup_Title", 0, "NOT READY"};
inline constexpr StringRef kWreckedMessage{"Barnfind", "IDS_Wrecked_Popup_Message", 0,
                                           "Please come back later. This Barn Find is still being restored"};
inline constexpr StringRef kLockedTitle{"Barnfind", "IDS_Locked_Popup_Title", 0, "NOT DISCOVERED"};
inline constexpr StringRef kLockedMessage{"Barnfind", "IDS_Locked_Popup_Message", 0,
                                          "This Barn Find has not been discovered yet!"};
inline constexpr StringRef kCantSellTitle{"Barnfind", "IDS_Trade_BarnFind_Title", 0, "CAN'T SELL BARN FIND"};
inline constexpr StringRef kCantSellMessage{"Barnfind", "IDS_Trade_BarnFind_Message", 0, "You cannot sell Barn finds."};
inline constexpr StringRef kViewOnMap{"Barnfind", "IDS_ViewMapAction", 0, "VIEW ON MAP"};
inline constexpr StringRef kOk{"Workshop", "IDS_BarnFindCollect", 0, "OK"};
// The manufacturer select (115_C_BUY_MFRSELECT).
inline constexpr StringRef kAvailable{"carflow", "IDS_Num_Available", 0, "{0} AVAILABLE"};
inline constexpr StringRef kAffordableCars{"HelpButtons", "IDS_AffordableCars", 0, "ALL AFFORDABLE CARS"};
inline constexpr StringRef kNoneAffordableTitle{"carflow", "IDS_No_Affordable_Title", 0, "NONE AFFORDABLE"};
inline constexpr StringRef kNoneAffordableMessage{
    "carflow", "IDS_No_Affordable_Msg", 0,
    "There are currently no affordable cars. Continue racing to earn more credits."};
}  // namespace car_list_text

// A tile's state: the scene's own icons (Ready_Icon, Not_Ready_Icon,
// Undiscovered_Icon, Car_Lock_LG) over the car.
enum class CarTileState : uint8_t { kNormal, kLocked, kUndiscovered, kNotReady, kReady };

struct CarTile {
  int64_t car_id = 0;   // the thumbnail (Thumbnails/Thumbnail_<id>); 0: none
  std::string cursor;   // the cursor's line under the selected tile (price, state)
  CarTileState state = CarTileState::kNormal;
};

// The selected car's panel (CAR_STATS).
struct CarListDetails {
  bool valid = false;
  int64_t car_id = 0;
  std::string model;      // TEXT_CAR_NAME: XKR-S
  std::string year_make;  // TEXT_MANUFACTURER: 2012 Jaguar
  std::string flag;       // the make's country flag (List_Country.IconPath); empty: none
  std::string car_class;  // the class letter (CLASS_A's texture); empty: none
  int pi = 0;
  int drive_type = 0;     // 1 FWD, 2 RWD, 3 AWD; 0: unknown
  int engine_placement = 1;  // Data_Car.EnginePlacementID: 1 front, 2 mid, 3 rear
  std::array<double, 5> stats{};  // SPEED, HANDLING, ACCELERATION, LAUNCH, BRAKING (0..10)
};

// A message popup over the list (H_MESSAGE_POPUP): Barn Finds' COLLECT CAR,
// NOT READY, NOT DISCOVERED and the garage's CAN'T SELL BARN FIND.
struct CarListPopup {
  bool open = false;
  std::string title, message;
  std::vector<std::pair<std::string, std::string>> prompts;  // (button, label)
};

struct CarListHud {
  CarListPopup popup;
  std::string title;
  std::vector<CarTile> tiles;
  int selected = 0;
  CarListDetails details;
  std::vector<std::pair<std::string, std::string>> prompts;  // (button, label)
  std::string notice;
};

// The cursor's slot: tile `selected + k - kCarListCursorSlot` sits in slot k.
inline constexpr int kCarListCursorSlot = 3;
inline constexpr int kCarListSlots = 7;

// The nodes the screen binds (checked by the corpus test).
const std::vector<const char*>& car_list_nodes();
// "{0} Cr".
std::string car_list_credits(int64_t cr);
// The drivetrain icon (UI4/Common/Drivetrain_<FWD|RWD|AWD>_<Front|Mid|Rear>); empty: none.
std::string drivetrain_texture(int drive_type, int engine_placement);
// A game texture path (GAME:\Media\UI\Textures\...\X.tga) as the scenes'
// texture key ("T:UI4/..." or a Horizon.zip member); empty in, empty out.
std::string ui_texture_key(const std::string& game_path);

// Draws the list; false when the scene is missing.
bool draw_car_list(ui::List& l, const DriveHud& st, const CarListHud& c, float w, float h);

// The Autoshow's first step, 115_C_BUY_MFRSELECT: the title tape, a strip
// of make tiles (List_CarMake.IconPath's logo on MANU_IMAGE) with the cursor
// on its fourth slot, the make's count of cars (Available), its profile in
// the info panel (List_CarMake.Profile) and the player bar.
inline constexpr std::string_view kMakeListScene = "115_C_BUY_MFRSELECT";

struct MakeTile {
  int id = 0;          // the tile's hit index (the caller's make)
  std::string logo;    // the logo's texture key; empty: `label` instead
  std::string label;
};

struct MakeListHud {
  std::string title;
  std::vector<MakeTile> tiles;
  int selected = 0;
  std::string cursor;      // under the cursor: the make's name
  std::string available;   // "{0} AVAILABLE"
  std::string info_title;  // the info panel's title and text
  std::string info_text;
  std::vector<std::pair<std::string, std::string>> prompts;  // (button, label)
  std::string notice;
  CarListPopup popup;  // NONE AFFORDABLE
};

const std::vector<const char*>& make_list_nodes();
bool draw_make_list(ui::List& l, const DriveHud& st, const MakeListHud& m, float w, float h);

}  // namespace carview
