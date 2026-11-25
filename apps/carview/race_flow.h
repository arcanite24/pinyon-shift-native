// The screens around a race in the original's order (SPEC-RACE-FLOW, the
// game's festival_event_entry_flow / prerace / postrace_internal flows):
// which screen shows, what it waits on and where it goes next. No drawing
// here: race_screens.h draws the screens, the drive session feeds the facts
// and acts on the screen changes.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace carview::race_flow {

enum class Screen : uint8_t {
  kNone,
  // Free roam, at the event marker (festival_event_entry_flow).
  kEventSummary,  // H_EVENT_SUMMARY: the event, its restrictions; A enters
  kCarSelect,     // 108_C_BUY_CARSELECT: an eligible car for the event
  kEntryFade,     // the 0.5 s fade out before the load (prepare_to_load_from_ticket)
  // On the grid (prerace_intro / prerace_menu / prerace_nemesis).
  kTitleCard,   // PRE_AND_POST_RACE's event title over the intro
  kGridMenu,    // H_GENERIC_TITLE: START RACE / DIFFICULTY / OPTIONS / QUIT RACE
  kRivalIntro,  // PRE_AND_POST_RACE's rival group after START (nemesis_show)
  kCountdown,   // the gantry count (the race's own)
  kRacing,
  // After the line (postrace_internal / CareerRewardsFlow / postrace_rivals).
  kFinishBanner,  // H_POST_RACE_FINISHED; X results, B replay, A continues
  kCashReward,    // H_POST_RACE_CREDITS_BREAKDOWN (auto-advances)
  kPoints,        // H_POST_RACE_CAREER_POINTS
  kRewardCar,     // H_POST_RACE_WON_CAR
  kTombola,       // H_CAR_TOMBOLA
  kRivalOffer,    // H_RIVAL_OFFER_CAREER_EVENT (online only in the original)
  kStayInCar,     // H_MESSAGE_POPUP: STAY IN CAR? (the event's car)
  kBlackOut,      // the fade to black before the load back to free roam
  kDone,          // the race leaves; free roam loads
};

std::string_view screen_name(Screen s);

// The flow file a race runs (the event's style, or the mode it came from).
enum class Kind : uint8_t {
  kFestival,    // festival races (style 1)
  kStreet,      // street races (style 2): no title screen
  kShowcase,    // showcases (style 3): earnings, then the reward car
  kNemesis,     // star showdowns (style 5): no title screen, the AI stays
  kHeadline,    // headline races (style 6): earnings, then the champion outro
  kHeadToHead,  // free-roam head-to-heads: nothing on a loss
  kRivals,      // Rivals runs: straight to the menu, no callout
  kRally,       // rally stages: banner, earnings, rally points
};

// Kind of a career event's style (CareerEvent::style).
Kind kind_of_style(int style);

struct PreFacts {
  Kind kind = Kind::kFestival;
  bool restart = false;          // PreRace_restart: the menu only (RACE-27)
  bool nemesis_in_event = false;  // IsNemesisInEventIgnoreIfLowerWristband
};

struct PostFacts {
  Kind kind = Kind::kFestival;
  bool quit = false;            // QUIT RACE from the pause menu: straight out
  bool crossed_line = true;     // false: DNF (no rewards)
  bool won = false;             // 1st (head-to-heads use it)
  int64_t credits = 0;          // the race's cash total
  bool career_xp_event = true;  // career points (wristband) awarded
  bool reward_car = false;      // a prize car was given (RewardCarSequence)
  bool tombola = false;         // the car tombola instead (H_CAR_TOMBOLA)
  bool online = false;          // CanShowRivalOffer needs a live session
  bool first_time_career = false;
  bool event_car = false;  // the race ran in a car other than the player's
};

// The original's step lists. Pre: the screens from the grid to the countdown
// (kCountdown last). Post: the screens from the line to the load (kDone
// last). Entry: the marker's screens up to the fade (kEntryFade last).
std::vector<Screen> pre_steps(const PreFacts& f);
std::vector<Screen> post_steps(const PostFacts& f);
std::vector<Screen> entry_steps(bool car_eligible);

// Whether the rewards flow hides the AI cars and parks the player at
// post_race_location_00 (PrepareForRewardsFlow; the nemesis flow keeps them).
bool hides_opponents(Kind k);

// Measured timings (docs/specs/UI_LAYOUTS.md b1/c/States, 60 fps captures).
namespace timing {
inline constexpr double kEntryFade = 0.5;        // prepare_to_load_from_ticket
inline constexpr double kMenuDelay = 1.8;        // the menu 100-110 frames after the card
inline constexpr double kMenuGrow = 10.0 / 60;   // the menu grows over ~10 frames
inline constexpr double kCardOut = 4.0 / 60;     // card and menu gone within 4 frames of START
inline constexpr double kBannerOut = 1.0;        // cash starts ~60 frames after A on the banner
inline constexpr double kCashAuto = 170.0 / 60;  // cash auto-advances after 160-180 frames
inline constexpr double kCashLines = 30.0 / 60;  // the remaining cash lines within 30 frames
inline constexpr double kBlackFade = 12.0 / 60;  // the darken after A on the points
inline constexpr double kBlackHold = 30.0 / 60;  // 30 frames of black before the load
// interim: the rival callout's length is the nemesis_show cutscene's, which
// the engine does not play; this holds the rival card for about as long as
// the callout capture shows it (SPEC_QUESTIONS Q95).
inline constexpr double kRivalIntro = 3.0;
}  // namespace timing

// One run of steps: the current screen, how long it has shown, and the
// timed advances. Input moves it with accept() / back().
class Flow {
 public:
  void start(std::vector<Screen> steps);
  void stop();
  bool active() const { return at_ < steps_.size(); }
  Screen screen() const { return active() ? steps_[at_] : Screen::kNone; }
  double elapsed() const { return t_; }
  const std::vector<Screen>& steps() const { return steps_; }
  // Whether the screen accepts A yet (the grid menu waits for its delay).
  bool ready() const;
  // Advances the clock; timed screens move on. True when the screen changed.
  bool update(double dt);
  // The next screen (A, or a screen's own end). True when it changed.
  bool next();
  // Jumps to `s` if it is a later step (true), e.g. QUIT RACE to the end.
  bool skip_to(Screen s);
  // Inserts `s` after the current step (car select from the summary).
  void insert_next(Screen s);

 private:
  std::vector<Screen> steps_;
  size_t at_ = 0;
  double t_ = 0;
};

// How long a timed screen shows before it moves on by itself (<0: waits).
double auto_advance(Screen s);

}  // namespace carview::race_flow
