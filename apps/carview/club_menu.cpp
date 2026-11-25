#include "club_menu.h"

#include <algorithm>

namespace carview {

namespace {

// IDS_Achievement_Desc_60.._69, _single and _plural (carchallenges).
constexpr std::array<StringRef, 10> kDescSingle = {{
    {"CarChallenges", "IDS_Achievement_Desc_60_single", 0, "EARN YOUR FIRST MEDAL."},
    {"CarChallenges", "IDS_Achievement_Desc_61_single", 0, "EARN A MEDAL IN ONE MORE FERRARI."},
    {"CarChallenges", "IDS_Achievement_Desc_62_single", 0, "EARN A MEDAL IN ONE MORE CAR WORTH AT LEAST 1 MILLION Cr."},
    {"CarChallenges", "IDS_Achievement_Desc_63_single", 0, "EARN A MEDAL IN ONE MORE SHELBY."},
    {"CarChallenges", "IDS_Achievement_Desc_64_single", 0, "EARN A MEDAL IN ONE MORE LAMBORGHINI."},
    {"CarChallenges", "IDS_Achievement_Desc_65_single", 0, "EARN A MEDAL IN ONE MORE NISSAN."},
    {"CarChallenges", "IDS_Achievement_Desc_66_single", 0, "EARN A MEDAL IN ONE MORE BMW."},
    {"CarChallenges", "IDS_Achievement_Desc_67_single", 0, "EARN A MEDAL IN ONE MORE BRITISH CAR."},
    {"CarChallenges", "IDS_Achievement_Desc_68_single", 0, "EARN A MEDAL IN ONE MORE FORD."},
    {"CarChallenges", "IDS_Achievement_Desc_69_single", 0, "EARN A MEDAL IN ONE MORE CAR!"},
}};
// IDS_Achievement_Desc_60_plural does not exist (the first medal is one).
constexpr std::array<StringRef, 10> kDescPlural = {{
    {"CarChallenges", "IDS_Achievement_Desc_60_single", 0, "EARN YOUR FIRST MEDAL."},
    {"CarChallenges", "IDS_Achievement_Desc_61_plural", 0, "EARN A MEDAL IN {0} DIFFERENT FERRARIS."},
    {"CarChallenges", "IDS_Achievement_Desc_62_plural", 0,
     "EARN A MEDAL IN {0} DIFFERENT CARS WORTH AT LEAST 1 MILLION Cr."},
    {"CarChallenges", "IDS_Achievement_Desc_63_plural", 0, "EARN A MEDAL IN {0} DIFFERENT SHELBYS."},
    {"CarChallenges", "IDS_Achievement_Desc_64_plural", 0, "EARN A MEDAL IN {0} DIFFERENT LAMBORGHINIS."},
    {"CarChallenges", "IDS_Achievement_Desc_65_plural", 0, "EARN A MEDAL IN {0} DIFFERENT NISSANS."},
    {"CarChallenges", "IDS_Achievement_Desc_66_plural", 0, "EARN A MEDAL IN {0} DIFFERENT BMWS."},
    {"CarChallenges", "IDS_Achievement_Desc_67_plural", 0, "EARN A MEDAL IN {0} DIFFERENT BRITISH CARS."},
    {"CarChallenges", "IDS_Achievement_Desc_68_plural", 0, "EARN A MEDAL IN {0} DIFFERENT FORDS."},
    {"CarChallenges", "IDS_Achievement_Desc_69_plural", 0, "EARN A MEDAL IN {0} DIFFERENT CARS!"},
}};

int wrap(int v, int by, int n) {
  return n <= 0 ? 0 : ((v + by) % n + n) % n;
}

}  // namespace

StringRef club_achievement_desc_ref(int desc, bool single) {
  const int i = std::clamp(desc - 60, 0, 9);
  return single ? kDescSingle[size_t(i)] : kDescPlural[size_t(i)];
}

std::string format_count(std::string_view fmt, int n) {
  std::string s(fmt);
  const size_t a = s.find("{0");
  // IDS_MedalsRequired and IDS_CarsRequired leave the count to the code
  // (" MEDALS REQUIRED"): it goes in front.
  if (a == std::string::npos) return !s.empty() && s[0] == ' ' ? std::to_string(n) + s : s;
  const size_t b = s.find('}', a);
  if (b == std::string::npos) return s;
  s.replace(a, b - a + 1, std::to_string(n));
  return s;
}

std::vector<ClubAchievementLine> club_achievement_lines(const freeroam::club::ClubMedalStats& stats) {
  std::vector<ClubAchievementLine> out;
  for (const auto& def : freeroam::club::club_achievements()) {
    ClubAchievementLine a;
    a.id = def.id;
    a.desc = def.desc;
    a.target = def.target;
    a.per_car = def.per_car;
    a.have = std::min(freeroam::club::achievement_progress(def.id, stats), def.target);
    a.unlocked = a.have >= a.target;
    out.push_back(a);
  }
  return out;
}

std::string club_achievement_desc(const ClubAchievementLine& a) {
  const int left = std::max(1, a.target - a.have);
  return format_count(text_of(club_achievement_desc_ref(a.desc, left == 1)), left);
}

std::string club_required_text(const ClubAchievementLine& a) {
  if (a.unlocked) return {};
  const int left = std::max(1, a.target - a.have);
  if (a.per_car) return format_count(text_of(left == 1 ? club_text::kCarRequired : club_text::kCarsRequired), left);
  return format_count(text_of(left == 1 ? club_text::kMedalRequired : club_text::kMedalsRequired), left);
}

// --- the model ---------------------------------------------------------------

std::vector<ClubMenu::Ticket> ClubMenu::tickets(const ClubMenuData& d) {
  std::vector<Ticket> t;
  if (d.current_begun) t.push_back(Ticket::kCurrentCar);
  t.push_back(Ticket::kAchievement);
  if (d.stats.total_medals > 0 || d.tutorial >= 4) t.push_back(Ticket::kViewAll);
  return t;
}

int ClubMenu::next_achievement(const ClubMenuData& d) {
  for (size_t i = 0; i < d.achievements.size(); ++i)
    if (!d.achievements[i].unlocked) return int(i);
  return 0;
}

std::vector<int> ClubMenu::cars_for(const ClubMenuData& d, uint16_t achievement) {
  std::vector<int> out;
  int ai = -1;
  const auto& defs = freeroam::club::club_achievements();
  for (size_t i = 0; i < defs.size(); ++i)
    if (defs[i].id == achievement) ai = int(i);
  for (size_t i = 0; i < d.garage.size(); ++i)
    if (ai < 0 || d.garage[i].counts[size_t(ai)]) out.push_back(int(i));
  // The driven car first (the capture's tutorial list starts on it).
  std::stable_partition(out.begin(), out.end(), [&](int i) { return d.garage[size_t(i)].current; });
  return out;
}

void ClubMenu::show(const ClubMenuData& d) {
  *this = ClubMenu{};
  if (d.gated()) {
    screen = Screen::kHome;
    popup = Popup::kUnavailable;
    return;
  }
  if (d.tutorial <= 0) {
    screen = Screen::kIntro;
    return;
  }
  screen = Screen::kHome;
  if (d.tutorial == 1) {
    popup = Popup::kTutorial;
    tutorial_text = club_text::kTutorialHome;
  }
}

void ClubMenu::intro_done(const ClubMenuData& d) {
  screen = Screen::kHome;
  home = 0;
  if (d.tutorial <= 1) {
    popup = Popup::kTutorial;
    tutorial_text = club_text::kTutorialHome;
  }
}

ClubMenu::Action ClubMenu::press(Button b, ClubMenuData& d) {
  const bool accept = b == Button::kAccept, back = b == Button::kBack;
  switch (popup) {
    case Popup::kUnavailable:
      if (accept || back) {
        close();
        return Action::kClose;
      }
      return Action::kNone;
    case Popup::kTutorial:
      if (accept) {
        popup = Popup::kNone;
        return Action::kAccept;
      }
      return Action::kNone;
    case Popup::kSelectAction:
      if (accept) {
        popup = Popup::kNone;
        const int64_t car = action_car;
        close();
        action_car = car;
        if (d.tutorial < 2) tutorial_to = 2;
        return Action::kBegin;
      }
      if (back) {
        popup = Popup::kNone;
        return Action::kBack;
      }
      return Action::kNone;
    case Popup::kNone:
      break;
  }
  switch (screen) {
    case Screen::kClosed:
      return Action::kNone;
    case Screen::kIntro:
      if (accept || back) {
        intro_done(d);
        if (d.tutorial < 1) {
          tutorial_to = 1;
          return Action::kTutorialStep;
        }
        return Action::kAccept;
      }
      return Action::kNone;
    case Screen::kHome: {
      const auto t = tickets(d);
      if (b == Button::kLeft || b == Button::kRight) {
        if (t.size() < 2) return Action::kNone;
        home = wrap(home, b == Button::kLeft ? -1 : 1, int(t.size()));
        return Action::kMove;
      }
      if (back) {
        close();
        return Action::kClose;
      }
      if (!accept) return Action::kNone;
      switch (t[size_t(std::clamp(home, 0, int(t.size()) - 1))]) {
        case Ticket::kCurrentCar:
          screen = Screen::kDetails;
          detail = 0;
          if (d.tutorial == 2) {
            popup = Popup::kTutorial;
            tutorial_text = club_text::kTutorialChallengeList;
            tutorial_to = 3;
            return Action::kTutorialStep;
          }
          return Action::kAccept;
        case Ticket::kAchievement:
          // The tutorial goes straight to the first medal's cars.
          if (d.tutorial <= 1 && d.stats.total_medals == 0) {
            filter = 216;
            screen = Screen::kCarList;
            car = 0;
            popup = Popup::kTutorial;
            tutorial_text = club_text::kTutorialCarList;
            return Action::kAccept;
          }
          screen = Screen::kAchievements;
          achievement = next_achievement(d);
          return Action::kAccept;
        case Ticket::kViewAll:
          // interim (SPEC_QUESTIONS Q143): the original's ALL CHALLENGES
          // goes through every manufacturer and model (to buy one); the
          // engine lists the garage's club cars.
          filter = 0;
          screen = Screen::kCarList;
          car = 0;
          return Action::kAccept;
      }
      return Action::kNone;
    }
    case Screen::kAchievements: {
      const int n = int(d.achievements.size());
      if (b == Button::kUp || b == Button::kDown) {
        if (n < 2) return Action::kNone;
        achievement = wrap(achievement, b == Button::kUp ? -1 : 1, n);
        return Action::kMove;
      }
      if (back) {
        screen = Screen::kHome;
        return Action::kBack;
      }
      if (accept && n > 0) {
        filter = d.achievements[size_t(std::clamp(achievement, 0, n - 1))].id;
        screen = Screen::kCarList;
        car = 0;
        return Action::kAccept;
      }
      return Action::kNone;
    }
    case Screen::kCarList: {
      const auto cars = cars_for(d, filter);
      const int n = int(cars.size());
      if (b == Button::kLeft || b == Button::kRight) {
        if (n < 2) return Action::kNone;
        car = wrap(car, b == Button::kLeft ? -1 : 1, n);
        return Action::kMove;
      }
      if (back) {
        // The tutorial's list came from the home ticket.
        screen =
            (filter == 0 || (d.tutorial <= 1 && d.stats.total_medals == 0)) ? Screen::kHome : Screen::kAchievements;
        return Action::kBack;
      }
      if (accept) {
        if (n == 0) return Action::kDeny;
        action_car = d.garage[size_t(cars[size_t(std::clamp(car, 0, n - 1))])].car_id;
        popup = Popup::kSelectAction;
        return Action::kAccept;
      }
      return Action::kNone;
    }
    case Screen::kDetails: {
      const int n = int(d.challenges.size());
      if (b == Button::kUp || b == Button::kDown) {
        if (n < 2) return Action::kNone;
        detail = wrap(detail, b == Button::kUp ? -1 : 1, n);
        return Action::kMove;
      }
      if (back) {
        screen = Screen::kHome;
        return Action::kBack;
      }
      return Action::kNone;
    }
  }
  return Action::kNone;
}

std::vector<StringRef> rally_entry_text_refs() {
  return {rally_text::kGateTitle, rally_text::kGateMsg, rally_text::kConfirmTitle, rally_text::kConfirmMsg};
}

std::vector<StringRef> club_text_refs() {
  using namespace club_text;
  std::vector<StringRef> v = {kTitle,
                              kCurrentChallenges,
                              kAchievements,
                              kAchievementProgress,
                              kAllChallenges,
                              kChallenges,
                              kMedals,
                              kMedalsRequired,
                              kMedalRequired,
                              kCarsRequired,
                              kCarRequired,
                              kMedalsAvailable,
                              kMedalAvailable,
                              kOwned,
                              kBegin,
                              kViewChallenges,
                              kSelectAction,
                              kSelectCar,
                              kTutorial,
                              kTutorialHome,
                              kTutorialCarList,
                              kTutorialChallengeList,
                              kTutorialGoToDetails,
                              kTutorialGoToHome,
                              kUnavailableTitle,
                              kUnavailableMsg,
                              kClubCarTitle,
                              kClubCarMsg,
                              kNotEligibleTitle,
                              kNotEligibleMsg,
                              kGetInCarRestrictedTitle,
                              kGetInCarRestrictedMsg,
                              kMedalEarned,
                              kFailed,
                              kHudMoreInfo,
                              kDetailsHintL,
                              kDetailsHintR,
                              kSelect,
                              kBack,
                              kOk,
                              kCancel};
  for (int i = 60; i <= 69; ++i) {
    v.push_back(club_achievement_desc_ref(i, true));
    if (i > 60) v.push_back(club_achievement_desc_ref(i, false));
  }
  return v;
}

}  // namespace carview
