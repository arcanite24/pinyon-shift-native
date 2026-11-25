// The 1000 Club's menus on the title update's own scenes
// (docs/UI_SCENE_SCREENS.md; car_challenge_menus.xml, _intro.xml and
// _tutorial.xml in v4's media.zip): the intro video on the first visit, the
// home screen of tickets (H_CAR_CHALLENGE_HUB), the achievements
// (H_CAR_CHALLENGE_ACHIEVEMENTS), the eligible garage cars of an achievement
// (H_CAR_CHALLENGE_BUY_CARSELECT) with its SELECT ACTION popup, the current
// car's challenges (H_CAR_CHALLENGE_DETAILS), the tutorial popups, and the
// driving HUD's challenge items (947_HUD's HUDCarChallengePrompt).
//
// ClubMenu is the screens' model, with no renderer: ui_tests and
// club_rally_tests check it. ClubMenuData is what the drive session fills
// from the save and the package's challenge files.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "freeroam/club.h"
#include "scene_screen.h"
#include "ui4_chrome.h"

namespace carview {

struct DriveHud;

inline constexpr std::string_view kClubHomeScene = "H_CAR_CHALLENGE_HUB";
inline constexpr std::string_view kClubAchievementsScene = "H_CAR_CHALLENGE_ACHIEVEMENTS";
inline constexpr std::string_view kClubCarListScene = "H_CAR_CHALLENGE_BUY_CARSELECT";
inline constexpr std::string_view kClubDetailsScene = "H_CAR_CHALLENGE_DETAILS";
inline constexpr std::string_view kClubPopupScene = "H_MESSAGE_POPUP";
// The intro video (car_challenge_intro.xml's IntroVideo) in the 1000 Club
// package's Media/UI/Videos/Splash_Intros.
inline constexpr std::string_view kClubIntroVideo = "car_challenges_intro";

// Every string the 1000 Club binds; the corpus test resolves each one.
namespace club_text {
inline constexpr StringRef kTitle{"CarChallenges", "IDS_PauseMenu_CarChallenges", 0, "1000 CLUB"};
inline constexpr StringRef kCurrentChallenges{"CarChallenges", "IDS_CurrentChallenges", 0, "CURRENT CAR CHALLENGES"};
inline constexpr StringRef kAchievements{"CarChallenges", "IDS_Achievements", 0, "1000 CLUB ACHIEVEMENTS"};
inline constexpr StringRef kAchievementProgress{"CarChallenges", "IDS_AchievementProgress", 0, "ACHIEVEMENT PROGRESS"};
inline constexpr StringRef kAllChallenges{"CarChallenges", "IDS_AllChallenges", 0, "VIEW ALL CHALLENGES"};
inline constexpr StringRef kChallenges{"CarChallenges", "IDS_Challenges", 0, "CHALLENGES"};
inline constexpr StringRef kMedals{"CarChallenges", "IDS_Progress", 0, "Medals"};
inline constexpr StringRef kMedalsRequired{"CarChallenges", "IDS_MedalsRequired", 0,
                                           "{0 - medal count} MEDALS REQUIRED"};
inline constexpr StringRef kMedalRequired{"CarChallenges", "IDS_MedalRequired", 0, "1 MEDAL REQUIRED"};
inline constexpr StringRef kCarsRequired{"CarChallenges", "IDS_CarsRequired", 0, "{0 - car count} NEW CARS REQUIRED"};
inline constexpr StringRef kCarRequired{"CarChallenges", "IDS_CarRequired", 0, "1 NEW CAR REQUIRED"};
inline constexpr StringRef kMedalsAvailable{"CarChallenges", "IDS_Recommendation_MedalsNeeded", 0,
                                            "{0} MEDALS AVAILABLE"};
inline constexpr StringRef kMedalAvailable{"CarChallenges", "IDS_Recommendation_MedalNeeded", 0, "{0} MEDAL AVAILABLE"};
inline constexpr StringRef kOwned{"CarChallenges", "IDS_CarSelect_Owned_Single", 0, "OWNED"};
inline constexpr StringRef kBegin{"CarChallenges", "IDS_Option_BeginChallenges", 0, "BEGIN CHALLENGES IN THIS CAR"};
inline constexpr StringRef kViewChallenges{"CarChallenges", "IDS_Option_ViewChallenges", 0, "VIEW CHALLENGES"};
inline constexpr StringRef kSelectAction{"CarChallenges", "IDS_Popup_Title_SelectAction", 0, "SELECT ACTION"};
inline constexpr StringRef kSelectCar{"CarChallenges", "IDS_HelpButton_SelectGarageCar", 0, "SELECT CAR"};
inline constexpr StringRef kTutorial{"CarChallenges", "IDS_Tutorial_PopupHeader", 0, "TUTORIAL"};
inline constexpr StringRef kTutorialHome{"CarChallenges", "IDS_Tutorial_CarChallengesHomeWelcome", 0,
                                         "Welcome to the 1000 CLUB Screen!"};
inline constexpr StringRef kTutorialCarList{"CarChallenges", "IDS_Tutorial_CarListWelcome", 0,
                                            "This is a list of cars in your garage that are eligible for this "
                                            "achievement."};
inline constexpr StringRef kTutorialChallengeList{"CarChallenges", "IDS_Tutorial_ChallengeListWelcome", 0,
                                                  "On this screen you can view a detailed description of how to "
                                                  "complete every challenge for your current car."};
inline constexpr StringRef kTutorialGoToDetails{"CarChallenges", "IDS_Tutorial_Prompt_GoToDetailsScreen", 0,
                                                "Enter the CURRENT CAR CHALLENGES screen to view all challenges for "
                                                "your car."};
inline constexpr StringRef kTutorialGoToHome{"CarChallenges", "IDS_Tutorial_Prompt_GoToHomeScreen", 0,
                                             "Visit the 1000 CLUB screen from the Pause Menu."};
inline constexpr StringRef kUnavailableTitle{"CarChallenges", "IDS_PauseMenu_Challenges_Unavailable_Title", 0,
                                             "1000 CLUB UNAVAILABLE"};
inline constexpr StringRef kUnavailableMsg{"CarChallenges", "IDS_PauseMenu_Challenges_Unavailable_Msg", 0,
                                           "You must earn a green wristband in Horizon Single Player before you can "
                                           "start competing in the 1000 CLUB."};
inline constexpr StringRef kClubCarTitle{"CarChallenges", "IDS_CannotUseClubCar_Title", 0, "FEATURE UNAVAILABLE"};
inline constexpr StringRef kClubCarMsg{"CarChallenges", "IDS_CannotUseClubCar_Body", 0,
                                       "Challenges cannot be completed using this car. To complete challenges, "
                                       "please use a car from your own garage."};
inline constexpr StringRef kNotEligibleTitle{"CarChallenges", "IDS_NotEligibleForChallengeTitle", 0, "NOT ELIGIBLE"};
inline constexpr StringRef kNotEligibleMsg{"CarChallenges", "IDS_NotEligibleForChallengeDesc", 0,
                                           "You haven't unlocked everything you need to complete this challenge yet"};
inline constexpr StringRef kGetInCarRestrictedTitle{"CarChallenges", "IDS_GetInCar_Restricted_Title", 0,
                                                    "CHANGE CAR NOT ALLOWED"};
inline constexpr StringRef kGetInCarRestrictedMsg{"CarChallenges", "IDS_GetInCar_Restricted_Msg", 0,
                                                  "Please return to single-player free roam to change your current "
                                                  "car."};
// The driving HUD.
inline constexpr StringRef kMedalEarned{"CarChallenges", "IDS_Hud_CompletedChallenge", 0, "MEDAL EARNED!"};
inline constexpr StringRef kFailed{"CarChallenges", "IDS_Hud_FailedChallenge", 0, "FAILED"};
inline constexpr StringRef kHudMoreInfo{"CarChallenges", "IDS_HudPrompt_MoreInfo", 0, "1000 Club Info"};
inline constexpr StringRef kDetailsHintL{"HelpButtons", "IDS_InputHint_CarChallengeDetails_L", 0, "PRESS"};
inline constexpr StringRef kDetailsHintR{"HelpButtons", "IDS_InputHint_CarChallengeDetails_R", 0,
                                         "TO VIEW CAR CHALLENGE DETAILS"};
// Help bars.
inline constexpr StringRef kSelect{"HelpButtons", "IDS_Select", 0, "SELECT"};
inline constexpr StringRef kBack{"HelpButtons", "IDS_Back", 0, "BACK"};
inline constexpr StringRef kOk{"HelpButtons", "IDS_OK", 0, "OK"};
inline constexpr StringRef kCancel{"HelpButtons", "IDS_Cancel", 0, "CANCEL"};
}  // namespace club_text

// The achievement description for `remaining` medals or cars left:
// IDS_Achievement_Desc_<desc>_single / _plural ("EARN A MEDAL IN ONE MORE
// FORD." / "EARN A MEDAL IN {0} DIFFERENT FORDS.").
StringRef club_achievement_desc_ref(int desc, bool single);
// Replaces the first "{0...}" slot (the tables write "{0 - medal count}").
std::string format_count(std::string_view fmt, int n);

// One row of the achievements screen.
struct ClubAchievementLine {
  uint16_t id = 0;
  int desc = 0;  // IDS_Achievement_Desc_<desc>
  int have = 0, target = 1;
  bool per_car = true;
  bool unlocked = false;
  std::string name;  // the spa name ("" when no spa)
  int gamerscore = 0;
};
// The ten rows (216..225) for `stats` with the package's targets.
std::vector<ClubAchievementLine> club_achievement_lines(const freeroam::club::ClubMedalStats& stats);
// "EARN A MEDAL IN 3 DIFFERENT FORDS." for the medals or cars still needed
// (the full target when none is earned yet).
std::string club_achievement_desc(const ClubAchievementLine& a);
// "2 MEDALS REQUIRED" / "5 NEW CARS REQUIRED"; empty once unlocked.
std::string club_required_text(const ClubAchievementLine& a);

// A garage car with challenges.
struct ClubCarEntry {
  int64_t car_id = 0;
  int64_t garage_id = 0;
  std::string name;       // MX-5 Miata
  std::string year_make;  // 1994 Mazda
  std::string class_letter;
  int pi = 0;
  freeroam::club::CarMedals medals;
  std::vector<std::string> challenge_titles;
  std::vector<bool> completed;
  // Achievements (216..225) the car counts toward, by index in
  // club_achievements().
  std::array<bool, 10> counts{};
  bool current = false;  // the driven car
};

// One challenge of the current car.
struct ClubChallengeLine {
  std::string title, description, hint, status;
  bool completed = false;
  double progress = 0.0;
};

struct ClubMenuData {
  freeroam::club::ClubMedalStats stats;
  std::vector<ClubAchievementLine> achievements;
  std::vector<ClubCarEntry> garage;
  int current = -1;                           // index in `garage` of the driven car; -1: not a club car
  std::vector<ClubChallengeLine> challenges;  // the current car's
  bool current_begun = false;
  // The pause row's gate (empty: open): the unavailable popup's title and
  // message.
  StringRef gate_title{}, gate_message{};
  bool gated() const { return !gate_message.fallback.empty(); }
  int tutorial = 0;  // drive::DriveSave::club_tutorial
};

// The screens, the popup over them, and the selections.
struct ClubMenu {
  enum class Screen : uint8_t { kClosed, kIntro, kHome, kAchievements, kCarList, kDetails };
  enum class Popup : uint8_t { kNone, kTutorial, kSelectAction, kUnavailable };
  enum class Ticket : uint8_t { kCurrentCar, kAchievement, kViewAll };
  enum class Button : uint8_t { kUp, kDown, kLeft, kRight, kAccept, kBack };
  // What the drive does after a press.
  enum class Action : uint8_t {
    kNone,
    kMove,          // a selection moved (Vscroll)
    kAccept,        // a screen opened (Accept)
    kBack,          // back a screen (Cancel)
    kClose,         // closed (UnPause)
    kDeny,          // nothing there (DenyAccess)
    kBegin,         // BEGIN CHALLENGES IN `action_car`
    kTutorialStep,  // tutorial advanced: save `ClubMenuData::tutorial` = `tutorial_to`
  };

  Screen screen = Screen::kClosed;
  Popup popup = Popup::kNone;
  StringRef tutorial_text{};
  int home = 0, achievement = 0, car = 0, detail = 0;
  uint16_t filter = 216;  // the car list's achievement
  int64_t action_car = 0;
  int tutorial_to = 0;
  double opened_at = 0;  // the intro's start (drive time)

  bool open() const { return screen != Screen::kClosed; }
  // Opens from the pause row: the unavailable popup when gated, the intro
  // video on the first visit, else the home screen (with the tutorial's
  // welcome until a car is picked).
  void show(const ClubMenuData& d);
  // The intro video ended (or was skipped).
  void intro_done(const ClubMenuData& d);
  void close() { *this = ClubMenu{}; }
  Action press(Button b, ClubMenuData& d);

  // The home screen's tickets: until the first medal (the tutorial), only
  // the first achievement; then the current car's challenges when begun in
  // it, the next achievement to earn, and VIEW ALL CHALLENGES.
  static std::vector<Ticket> tickets(const ClubMenuData& d);
  // The achievement the home ticket shows: the first not unlocked (216 first).
  static int next_achievement(const ClubMenuData& d);
  // The car list of achievement `filter`: garage cars that count toward it.
  static std::vector<int> cars_for(const ClubMenuData& d, uint16_t achievement);
};

// Draws the open screen and popup on their scenes; false when the screen's
// scene is missing (the caller keeps the hand-drawn club screen).
// `hud`: the PlayerBar's wristband, car and credits (null: hidden).
bool draw_club_menu(ui::List& l, const ClubMenu& m, const ClubMenuData& d, float w, float h,
                    const DriveHud* hud = nullptr);

// The driving HUD's challenge items (947_HUD's HUDCarChallengePrompt): up to
// six of the current car's running challenges, each with its medal number,
// its description and its result once passed or failed.
struct ClubHudItem {
  int number = 1;  // the challenge's number in the car's list
  std::string text;
  double progress = 0.0;
  enum class Result : uint8_t { kRunning, kPassed, kFailed } result = Result::kRunning;
  double result_age = 0.0;  // s since passed or failed
};
struct ClubHud {
  std::vector<ClubHudItem> items;
  bool details_hint = false;  // PRESS <dpad> TO VIEW CAR CHALLENGE DETAILS
  std::string instruction;    // the tutorial's prompt (top left)
};
// How long a passed or failed item stays (the bgf's COMPLETE / FAIL
// animations; interim, SPEC_QUESTIONS Q144).
inline constexpr double kClubResultHold = 3.0;
bool draw_club_hud(ui::List& l, const ClubHud& hud, float w, float h);

// A message on H_MESSAGE_POPUP (title on its pill, the message wrapped to
// the panel, the world dimmed) with `buttons` in the help bar; false when
// the scene is missing. The club's tutorials and the Rally entry use it.
bool draw_scene_message(ui::List& l, const std::string& title, const std::string& message,
                        const std::vector<ui4::Prompt>& buttons, float w, float h);

// Horizon Rally's entry from the pause row (game_start_rally.xml's gate and
// confirmation, rally.str).
namespace rally_text {
inline constexpr StringRef kGateTitle{"rally", "IDS_FirstTimeCareerError_Title", 0, "HORIZON RALLY"};
inline constexpr StringRef kGateMsg{"rally", "IDS_FirstTimeCareerError_Msg", 0,
                                    "You must earn a yellow wristband in Horizon Single Player before you can "
                                    "enter Horizon Rally."};
inline constexpr StringRef kConfirmTitle{"rally", "IDS_QuitHorizonToRally_Title", 0, "HORIZON RALLY"};
inline constexpr StringRef kConfirmMsg{"rally", "IDS_QuitHorizonToRally_Msg", 0,
                                       "Are you sure you want to quit Horizon Single Player and enter Horizon "
                                       "Rally?"};
}  // namespace rally_text
// The Rally entry's popups: the yellow-wristband gate (A OK) and the
// confirmation (A OK, B CANCEL; rally_confirm.png).
enum class RallyEntryPopup : uint8_t { kNone, kGate, kConfirm };
bool draw_rally_entry_popup(ui::List& l, RallyEntryPopup p, float w, float h);

// The nodes the club screens bind (checked by the corpus test).
const std::vector<std::pair<std::string_view, std::vector<const char*>>>& club_screen_nodes();
// Every StringRef the club screens bind (checked by the corpus test).
std::vector<StringRef> club_text_refs();
// The Rally entry's strings (checked by the corpus test).
std::vector<StringRef> rally_entry_text_refs();

}  // namespace carview
