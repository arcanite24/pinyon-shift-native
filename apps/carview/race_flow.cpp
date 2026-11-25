#include "race_flow.h"

#include <algorithm>

namespace carview::race_flow {

std::string_view screen_name(Screen s) {
  switch (s) {
    case Screen::kNone: return "none";
    case Screen::kEventSummary: return "event_summary";
    case Screen::kCarSelect: return "car_select";
    case Screen::kEntryFade: return "entry_fade";
    case Screen::kTitleCard: return "title_card";
    case Screen::kGridMenu: return "grid_menu";
    case Screen::kRivalIntro: return "rival_intro";
    case Screen::kCountdown: return "countdown";
    case Screen::kRacing: return "racing";
    case Screen::kFinishBanner: return "finish_banner";
    case Screen::kCashReward: return "cash_reward";
    case Screen::kPoints: return "points";
    case Screen::kRewardCar: return "reward_car";
    case Screen::kTombola: return "tombola";
    case Screen::kRivalOffer: return "rival_offer";
    case Screen::kStayInCar: return "stay_in_car";
    case Screen::kBlackOut: return "black_out";
    case Screen::kDone: return "done";
  }
  return "?";
}

Kind kind_of_style(int style) {
  switch (style) {
    case 2: return Kind::kStreet;
    case 3: return Kind::kShowcase;
    case 5: return Kind::kNemesis;
    case 6: return Kind::kHeadline;
    default: return Kind::kFestival;
  }
}

std::vector<Screen> pre_steps(const PreFacts& f) {
  std::vector<Screen> s;
  // PreRace_restart: no intro, no title, no callout; the menu over the grid.
  // Rivals: the menu over prerace_looping_01, no callout. Street and nemesis
  // flows run their intro without a title screen.
  const bool title = !f.restart && f.kind != Kind::kRivals && f.kind != Kind::kStreet &&
                     f.kind != Kind::kNemesis && f.kind != Kind::kHeadToHead;
  if (title) s.push_back(Screen::kTitleCard);
  if (f.kind != Kind::kHeadToHead) s.push_back(Screen::kGridMenu);
  // prerace_nemesis: only the festival-type flows call the rival out, and
  // only on a first start.
  const bool callout = f.nemesis_in_event && !f.restart && f.kind != Kind::kRivals &&
                       f.kind != Kind::kStreet && f.kind != Kind::kNemesis && f.kind != Kind::kHeadToHead;
  if (callout) s.push_back(Screen::kRivalIntro);
  s.push_back(Screen::kCountdown);
  return s;
}

std::vector<Screen> post_steps(const PostFacts& f) {
  std::vector<Screen> s;
  if (f.quit) return {Screen::kBlackOut, Screen::kDone};
  // A lost head-to-head shows nothing; a won one the banner and its cash.
  if (f.kind == Kind::kHeadToHead) {
    if (f.won && f.crossed_line) {
      s.push_back(Screen::kFinishBanner);
      if (f.credits > 0) s.push_back(Screen::kCashReward);
    }
    s.push_back(Screen::kDone);
    return s;
  }
  s.push_back(Screen::kFinishBanner);
  if (f.crossed_line && f.kind != Kind::kRivals) {
    // CareerRewardsFlow.
    if (f.credits > 0) s.push_back(Screen::kCashReward);
    const bool points = f.career_xp_event && f.kind != Kind::kShowcase && f.kind != Kind::kHeadline;
    if (points) s.push_back(Screen::kPoints);
    if (f.reward_car)
      s.push_back(Screen::kRewardCar);
    else if (f.tombola)
      s.push_back(Screen::kTombola);
    // postrace_rivals: the offer needs a live session (CanShowRivalOffer);
    // a given reward car ends the flow before the event-car check.
    if (f.kind != Kind::kRally) {
      if (!f.first_time_career && f.online) s.push_back(Screen::kRivalOffer);
      if (!f.reward_car && f.event_car) s.push_back(Screen::kStayInCar);
    }
  }
  s.push_back(Screen::kBlackOut);
  s.push_back(Screen::kDone);
  return s;
}

std::vector<Screen> entry_steps(bool car_eligible) {
  if (car_eligible) return {Screen::kEventSummary, Screen::kEntryFade};
  return {Screen::kEventSummary, Screen::kCarSelect, Screen::kEntryFade};
}

bool hides_opponents(Kind k) { return k != Kind::kNemesis && k != Kind::kHeadToHead; }

double auto_advance(Screen s) {
  switch (s) {
    case Screen::kEntryFade: return timing::kEntryFade;
    case Screen::kTitleCard: return timing::kMenuDelay;
    case Screen::kRivalIntro: return timing::kRivalIntro;
    case Screen::kCashReward: return timing::kBannerOut + timing::kCashAuto;
    case Screen::kBlackOut: return timing::kBlackFade + timing::kBlackHold;
    default: return -1;
  }
}

void Flow::start(std::vector<Screen> steps) {
  steps_ = std::move(steps);
  at_ = 0;
  t_ = 0;
}

void Flow::stop() {
  steps_.clear();
  at_ = 0;
  t_ = 0;
}

bool Flow::ready() const {
  switch (screen()) {
    case Screen::kGridMenu: return t_ >= timing::kMenuGrow;
    case Screen::kCashReward: return t_ >= timing::kBannerOut + timing::kCashLines;
    case Screen::kEntryFade:
    case Screen::kBlackOut:
    case Screen::kCountdown:
    case Screen::kDone:
    case Screen::kNone: return false;
    default: return true;
  }
}

bool Flow::update(double dt) {
  if (!active()) return false;
  t_ += dt;
  const double a = auto_advance(screen());
  if (a >= 0 && t_ >= a) return next();
  return false;
}

bool Flow::next() {
  if (!active() || screen() == Screen::kDone) return false;
  ++at_;
  t_ = 0;
  return true;
}

bool Flow::skip_to(Screen s) {
  for (size_t i = at_ + 1; i < steps_.size(); ++i) {
    if (steps_[i] == s) {
      at_ = i;
      t_ = 0;
      return true;
    }
  }
  return false;
}

void Flow::insert_next(Screen s) {
  if (!active()) return;
  if (at_ + 1 < steps_.size() && steps_[at_ + 1] == s) return;
  steps_.insert(steps_.begin() + std::ptrdiff_t(at_ + 1), s);
}

}  // namespace carview::race_flow
