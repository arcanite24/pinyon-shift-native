// The screens around a race drawn from the game's own scenes
// (docs/UI_SCENE_SCREENS.md), in the order race_flow.h runs them: the event
// summary at the marker, the title card and the grid menu, the rival
// callout, the finish banner, the cash breakdown, the points, the won car
// and the stay-in-car popup. Each draw returns false when its scene is
// missing (the caller falls back to the hand-drawn pages).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "car_list_screen.h"
#include "race_flow.h"
#include "scene_screen.h"
#include "ui4_chrome.h"

namespace carview {

struct DriveHud;
struct RaceHud;

inline constexpr std::string_view kEventSummaryScene = "H_EVENT_SUMMARY";
inline constexpr std::string_view kRaceTitleScene = "PRE_AND_POST_RACE";
inline constexpr std::string_view kGridMenuScene = "H_GENERIC_TITLE";
inline constexpr std::string_view kFinishedScene = "H_POST_RACE_FINISHED";
inline constexpr std::string_view kCashScene = "H_POST_RACE_CREDITS_BREAKDOWN";
inline constexpr std::string_view kPointsScene = "H_POST_RACE_CAREER_POINTS";
inline constexpr std::string_view kWonCarScene = "H_POST_RACE_WON_CAR";
inline constexpr std::string_view kPopupScene = "H_MESSAGE_POPUP";
inline constexpr std::string_view kEncounterScene = "H_RACE_ENCOUNTER";
// game_rally_race_flow's final screens. The title update's executable
// registers H_POST_RACE_RALLY_POINTS and H_HORIZON_CHAMPION, and shows the
// Rally's banner on the base H_POST_RACE_FINISHED; its
// h_post_race_rally_finished and h_horizon_rally_champion files are named
// by no code (Q212), so they are not used.
inline constexpr std::string_view kRallyPointsScene = "H_POST_RACE_RALLY_POINTS";
inline constexpr std::string_view kChampionScene = "H_HORIZON_CHAMPION";

// The strings the race screens bind; every one is checked against the
// tables by the corpus test (scene_screen_tests).
namespace race_text {
// Help bars.
inline constexpr StringRef kEnterEvent{"HelpButtons", {}, 0x494B, "ENTER EVENT"};
inline constexpr StringRef kBack{"HelpButtons", "IDS_Back", 0, "BACK"};
inline constexpr StringRef kRestrictions{"HelpButtons", {}, 0xAC80, "RESTRICTIONS"};
inline constexpr StringRef kSelect{"HelpButtons", "IDS_Select", 0, "SELECT"};
inline constexpr StringRef kContinue{"HelpButtons", {}, 0xD2A8, "CONTINUE"};
inline constexpr StringRef kRestart{"HelpButtons", {}, 0x788D, "RESTART"};
inline constexpr StringRef kRaceResults{"InGame", {}, 0x29C5, "RACE RESULTS"};
inline constexpr StringRef kViewReplay{"InGame", {}, 0x7B0F, "VIEW REPLAY"};
inline constexpr StringRef kYes{"HelpButtons", {}, 0xEA08, "YES"};
inline constexpr StringRef kNo{"HelpButtons", {}, 0xD4A2, "NO"};
// The grid menu (PreRace): START RACE / DIFFICULTY / OPTIONS / QUIT RACE.
inline constexpr StringRef kStartRace{"InGame", {}, 0x7E5A, "START RACE"};
inline constexpr StringRef kDifficulty{"PauseMenu", {}, 0xB7DD, "DIFFICULTY"};
inline constexpr StringRef kOptions{"PauseMenu", {}, 0xFEC9, "OPTIONS"};
inline constexpr StringRef kQuitRace{"InGame", {}, 0xE8EA, "QUIT RACE"};
// The event summary (CareerEventUI, Calendar).
inline constexpr StringRef kFestivalRace{"CareerEventUI", {}, 0x03B8, "FESTIVAL RACE"};
inline constexpr StringRef kStreetRace{"CareerEventUI", {}, 0x035C, "STREET RACE"};
inline constexpr StringRef kShowcaseRace{"CareerEventUI", {}, 0x20CA, "SHOWCASE RACE"};
inline constexpr StringRef kHeadlineRace{"CareerEventUI", {}, 0x3EA6, "FESTIVAL HEADLINE RACE"};
inline constexpr StringRef kStarShowdown{"CareerEventUI", {}, 0x7CA9, "STAR SHOWDOWN"};
inline constexpr StringRef kCircuit{"CareerEventUI", {}, 0x608E, "CIRCUIT RACE"};
inline constexpr StringRef kPointToPoint{"CareerEventUI", {}, 0x6A02, "POINT-TO-POINT"};
inline constexpr StringRef kAsphalt{"CareerEventUI", {}, 0x9D26, "ASPHALT"};
inline constexpr StringRef kMixedSurface{"CareerEventUI", {}, 0xB7E1, "MIXED SURFACE"};
inline constexpr StringRef kOffRoad{"CareerEventUI", {}, 0x2048, "OFF ROAD"};
inline constexpr StringRef kTwoWords{"CareerEventUI", {}, 0xC25F, "{0} {1}"};
inline constexpr StringRef kPointsAvailable{"CareerEventUI", {}, 0x87C6, "POINTS AVAILABLE"};
inline constexpr StringRef kHorizonHeatsInfo{"CareerEventUI", "IDS_HorizonHeatsInfo", 0, "FINISH 3RD OR ABOVE TO WIN"};
inline constexpr StringRef kHorizonHeatsReward1{"CareerEventUI", "IDS_HorizonHeatsReward_Line1", 0, "YELLOW"};
inline constexpr StringRef kHorizonHeatsReward2{"CareerEventUI", "IDS_HorizonHeatsReward_Line2", 0, "WRISTBAND"};
inline constexpr StringRef kShowcaseInstructions{"CareerEventUI", "IDS_ShowcaseInstructions", 0, "WIN THE RACE TO KEEP THE CAR"};
inline constexpr StringRef kWinnerTakes{"CareerEventUI", {}, 0x4293, "WINNER TAKES"};
inline constexpr StringRef kEntryRestrictions{"CareerEventUI", {}, 0x4953, "ENTRY RESTRICTIONS:"};
inline constexpr StringRef kClassOrBelow{"CareerEventUI", {}, 0x7013, "Car Class must be {0} or below."};
inline constexpr StringRef kCarProvided{"CareerEventUI", {}, 0xA554, "CAR PROVIDED"};
inline constexpr StringRef kBestFinish{"CareerEventUI", {}, 0xE587, "BEST FINISH: {0}"};
inline constexpr StringRef kBeat{"CareerEventUI", {}, 0xB576, "Beat {0}"};
inline constexpr StringRef kReward{"CareerEventUI", {}, 0x609F, "REWARD"};
inline constexpr StringRef kCurrentCarEligible{"Calendar", {}, 0x8FCA, "CURRENT CAR ELIGIBLE"};
inline constexpr StringRef kGarageCarsEligible{"Calendar", {}, 0x86D5, "GARAGE CARS ELIGIBLE"};
inline constexpr StringRef kNoCarsEligible{"Calendar", {}, 0x436F, "NO CARS ELIGIBLE"};
inline constexpr StringRef kPts{"Calendar", {}, 0xF111, "PTS"};
inline constexpr StringRef kNoRestrictions{"InGame", {}, 0x8C7F, "There are no restrictions for this event."};
inline constexpr StringRef kCarSelectTitle{"ScreenTitles", {}, 0x0DE7, "CAR SELECT"};
inline constexpr StringRef kCredits{"UnitStrings", "IDS_Units_Credits", 0, "{0} Cr"};
// The finish banner and the cash breakdown.
inline constexpr StringRef kFinished{"InGame", {}, 0x7709, "FINISHED"};
inline constexpr StringRef kCashReward{"ScreenTitles", {}, 0x6AF5, "CASH REWARD"};
inline constexpr StringRef kFinishedPlace{"OnlineRewards", {}, 0x8D0D, "FINISHED {0}"};
inline constexpr StringRef kMileageBonus{"OnlineRewards", {}, 0x3FA3, "MILEAGE BONUS"};
inline constexpr StringRef kBeatTheStar{"OnlineRewards", "IDS_Nemesis_Bonus_Heading", 0, "BEAT THE STAR"};
inline constexpr StringRef kDifficultyBonus{"OnlineRewards", {}, 0x7EFF, "DIFFICULTY & ASSISTS BONUS"};
inline constexpr StringRef kTotal{"InGame", {}, 0xE27D, "TOTAL"};
inline constexpr StringRef kBalance{"OnlineRewards", {}, 0x7130, "BALANCE:"};
inline constexpr StringRef kPlace{"PostRaceFlow", {}, 0x5A12, "{0} PLACE"};
// The points.
inline constexpr StringRef kPointsEarned{"PostRaceFlow", {}, 0x8F13, "POINTS EARNED"};
inline constexpr StringRef kPointsN{"PostRaceFlow", {}, 0x1525, "{0} POINTS"};
inline constexpr StringRef kCollectIt{"PostRaceFlow", {}, 0x2D22, "COLLECT IT FROM RACE CENTRAL"};
inline constexpr StringRef kWristbandUnlocked{"PostRaceFlow", {}, 0x8EBF, "WRISTBAND UNLOCKED!"};
inline constexpr StringRef kPointsToNext{"PostRaceFlow", {}, 0x77B3, "{0} POINTS TO NEXT WRISTBAND"};
inline constexpr StringRef kPointToNext{"PostRaceFlow", {}, 0xFD53, "{0} POINT TO NEXT WRISTBAND"};
inline constexpr StringRef kPleaseWait{"Main", {}, 0xD8D2, "PLEASE WAIT"};
// The Rally's points and champion screens.
inline constexpr StringRef kPointsToRallyFinal{"PostRaceFlow", {}, 0xC2FD, "{0} POINTS TO RALLY FINAL"};
inline constexpr StringRef kPointToRallyFinal{"PostRaceFlow", {}, 0x481D, "{0} POINT TO RALLY FINAL"};
inline constexpr StringRef kWinToBeChampion{"PostRaceFlow", {}, 0xC923, "WIN TO BE CHAMPION!"};
inline constexpr StringRef kRallyChampion{"CareerEventUI", {}, 0x2DD1, "HORIZON RALLY CHAMPION 2012"};
// The won car and the stay-in-car popup.
inline constexpr StringRef kYouWonACar{"InGame", {}, 0x921F, "YOU WON A CAR!"};
inline constexpr StringRef kStayInCar{"carflow", {}, 0x7E32, "STAY IN CAR"};
inline constexpr StringRef kStayInCarQ{"carflow", {}, 0xAD9E, "Do you want to stay in the {0} for free roam?"};
}  // namespace race_text

// The nodes each screen binds (checked by the corpus test).
const std::vector<std::pair<std::string_view, std::vector<const char*>>>& race_screen_nodes();

// The grid menu's rows (the PreRace screen's options).
enum class GridRow : uint8_t { kStartRace, kDifficulty, kOptions, kQuitRace };
inline constexpr GridRow kGridRows[] = {GridRow::kStartRace, GridRow::kDifficulty, GridRow::kOptions,
                                        GridRow::kQuitRace};
StringRef grid_row_text(GridRow r);

// Binds a menu scene's Option_List rows (H_GENERIC_TITLE's Menu and
// H_GENERIC share it): each row's text with its tapes cut to it, the
// selected row pink with its clip-on and one size step larger, rows past the
// list hidden, and a ui::kHitRow hit zone per row. The list sits at the
// offset measured on 13_prerace_menu_orig.png (race_layout::kMenuDx/Dy).
void bind_menu_rows(SceneScreen& sc, ui::List& l, const SceneFrame& f, const std::vector<std::string>& rows,
                    int selected, float extra_dy = 0.0f);

// The event summary at a marker (H_EVENT_SUMMARY).
struct EventSummaryHud {
  int style = 1;           // CareerEvent::style
  std::string type;        // FESTIVAL RACE ...
  std::string ticket;      // EVENTTICKETS/FESTIVALRACES/043 (empty: the scene's)
  std::string track;       // GLADSTONE CANYON RUN
  std::string track_info;  // ASPHALT POINT-TO-POINT
  int points = 0;          // the winner's wristband points
  int64_t credits = 0;     // the winner's cash
  std::string prize_heading;    // overrides POINTS AVAILABLE (e.g. Heats / Showcase / Street)
  std::string prize_wristband;  // wristband reward in place of points (e.g. "YELLOW\nWRISTBAND")
  std::string car_class;   // the class letter (A); empty: none
  std::string restriction;  // OPEN / the event's restriction line
  std::string restriction_detail;  // the restrictions view's text
  std::string eligibility;  // CURRENT CAR ELIGIBLE ...
  bool eligible = true;
  std::string provided_car;  // a loaner (CAR PROVIDED)
  std::string best_finish;   // 1ST ... (empty: never raced)
  bool restrictions_view = false;
  double t = 0;  // seconds since it opened
  std::string name;  // the event's display name (car select's title)
  race_flow::Screen screen = race_flow::Screen::kEventSummary;  // or kCarSelect / kEntryFade
  std::vector<std::string> cars;  // car select's rows
  int car_selected = 0;
  CarListHud car_list;  // car select on 108_C_BUY_CARSELECT
  double fade = 0;  // the entry fade's black (0..1)
  // The first-time career's sign-up: the ticket with PLEASE WAIT in place of
  // the help bar while the race loads (first30e r9 h00078-h00090).
  bool please_wait = false;
};

// The spinner and PLEASE WAIT at the bottom left (the career's map after A
// on the heats, its sign-up ticket; first30e r7 n0, r9 h00081).
void draw_please_wait_corner(ui::List& l, float w, float h, double t);

// The ticket texture of an event (EventTickets/<style folder>/<ticket>).
std::string event_ticket_texture(int style, std::string_view ticket);
// The surface + race type line ("ASPHALT POINT-TO-POINT").
std::string event_track_info(int surface, bool circuit);
// The event-type tape's text.
std::string event_type_text(int style);

bool draw_event_summary(ui::List& l, const DriveHud& st, const EventSummaryHud& es, float w, float h);
// The summary's ticket alone (no help bar): over the map for X DETAILS,
// where the map's own player bar shows (`player_bar` false hides the
// scene's).
bool draw_event_ticket(ui::List& l, const DriveHud& st, const EventSummaryHud& es, float w, float h, bool player_bar);
// The race screen `r.screen` (title card .. stay in car); false: not one of
// these, or its scene is missing.
bool draw_race_screen(ui::List& l, const DriveHud& st, const RaceHud& r, float w, float h);
// H_MESSAGE_POPUP over the screen: a title, a message wrapped at the
// original's pitch, and the help prompts (STAY IN CAR, the front end's
// notices). False when the scene is missing.
bool draw_message_popup(ui::List& l, const std::string& title, const std::string& message,
                        const std::vector<ui4::Prompt>& prompts, float w, float h);


// The Rally final's screens (game_rally_race_flow, after the last stage):
// the banner on H_POST_RACE_FINISHED, the earnings on
// H_POST_RACE_CREDITS_BREAKDOWN, the Rally XP on H_POST_RACE_RALLY_POINTS, a
// prize car on H_POST_RACE_WON_CAR and, the first time, H_HORIZON_CHAMPION.
struct RallyPostHud {
  int xp_before = 0, xp_after = 0;  // Rally XP
  int band_lo = 0, band_hi = 0;     // the Rally level's span
  int final_req = 0;                // the final's UnlockPointsReq
  bool champion = false;            // the final already won
  std::string band, band_next;      // the levels' WRISTBANDS thumbnails
};
// The points screen's line: N POINTS TO RALLY FINAL, WIN TO BE CHAMPION!
// once the final is open, empty once champion.
std::string rally_points_to_final(const RallyPostHud& r);
bool draw_rally_finished(ui::List& l, int place, float w, float h);
// The earnings: FINISHED <place> with the championship's prize, the
// difficulty and assists bonus and the balance, `t` seconds in (the banner
// goes out first, as after a race).
struct RallyEarningsHud {
  int place = 1;
  int64_t base = 0, total = 0, balance = -1;
  int difficulty_pct = 0;
  double t = 0.0;
};
bool draw_rally_earnings(ui::List& l, const RallyEarningsHud& e, float w, float h);
bool draw_rally_points(ui::List& l, const RallyPostHud& r, float w, float h);
bool draw_rally_prize_car(ui::List& l, const std::string& car_name, float w, float h);
// The champion's screen (CHorizonChampion in Rally mode): CareerEventUI's
// HORIZON RALLY CHAMPION 2012 as the title, no help bar (it closes with the
// cutscene).
bool draw_rally_champion(ui::List& l, float w, float h);
// The strings the champion's screen binds (checked by the corpus test).
std::vector<StringRef> rally_champion_text_refs();

// Measured placements (1280x720 canvas) of the original's captures.
namespace race_layout {
// 13_prerace_menu_orig.png: START RACE's text box from x 185, y 186 (the
// scene's Option_List rests at 226,235 before its stacker runs).
inline constexpr float kMenuDx = -44.0f, kMenuDy = -16.0f;
inline constexpr float kMenuFocusScale = 1.21f;  // as the pause list
inline constexpr float kMenuFocusDy = -5.0f;
}  // namespace race_layout

}  // namespace carview
