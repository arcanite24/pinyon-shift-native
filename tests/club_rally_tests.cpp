// The title update's content rules and menus (docs/V4_CONTENT.md): the
// 1000 Club's achievement table and per-car medals, its menus' model
// (club_menu.h), the save's first-visit fields, and the pause rows of the
// packages. No game files needed; club_rally_corpus_tests checks the scenes
// and strings against them.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "club_menu.h"
#include "drive/save.h"
#include "freeroam/club.h"
#include "hud.h"

namespace {

int g_failures = 0;
int g_checks = 0;
void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
  }
}

namespace club = freeroam::club;
using carview::ClubMenu;
using carview::ClubMenuData;

void test_achievement_table() {
  const auto& defs = club::club_achievements();
  // The package's ten achievements (spa 216..225), their description ids
  // (IDS_Achievement_Desc_60.._69) and targets.
  const int targets[10] = {1, 14, 14, 3, 8, 7, 10, 20, 13, 146};
  for (size_t i = 0; i < defs.size(); ++i) {
    check(defs[i].id == 216 + i, "achievement " + std::to_string(i) + " is spa " + std::to_string(216 + i));
    check(defs[i].desc == int(60 + i), "achievement " + std::to_string(216 + i) + " desc id");
    check(defs[i].target == targets[i], "achievement " + std::to_string(216 + i) + " target");
    check(defs[i].per_car == (i != 0), "achievement " + std::to_string(216 + i) + " counts cars");
  }
  check(club::club_achievement(220) && club::club_achievement(220)->target == 8, "lookup 220");
  check(club::club_achievement(215) == nullptr && club::club_achievement(226) == nullptr, "lookup outside");

  club::CarMeta ferrari{1, "Ferrari", "Italy", 250000};
  club::CarMeta mclaren{2, "McLaren", "UK", 1200000};
  club::CarMeta mini{3, "MINI", "", 20000};
  club::CarMeta ford{4, "Ford", "USA", 30000};
  club::CarMeta shelby{5, "Shelby", "USA", 90000};
  check(club::counts_for(217, ferrari) && !club::counts_for(217, ford), "Ferrari counts for 217");
  check(club::counts_for(218, mclaren) && !club::counts_for(218, ferrari), "1 million Cr counts for 218");
  check(club::counts_for(223, mclaren) && club::counts_for(223, mini) && !club::counts_for(223, ford),
        "British cars count for 223");
  check(club::counts_for(224, ford) && !club::counts_for(224, shelby), "Ford counts for 224, not Shelby");
  check(club::counts_for(219, shelby), "Shelby counts for 219");
  check(club::counts_for(216, ford) && club::counts_for(225, ford), "every car counts for 216 and 225");

  // compute_medal_stats agrees with counts_for and achievement_progress.
  std::map<int64_t, club::CarMeta> meta = {{1, ferrari}, {2, mclaren}, {4, ford}};
  const auto st = club::compute_medal_stats({{1, 2}, {2, 1}, {4, 0}}, meta);
  check(st.total_medals == 3 && st.cars_with_medal == 2, "medal stats totals");
  check(club::achievement_progress(216, st) == 3, "216 progress: medals");
  check(club::achievement_progress(217, st) == 1, "217 progress: Ferraris");
  check(club::achievement_progress(218, st) == 1 && club::achievement_progress(223, st) == 1,
        "218 and 223 progress: the McLaren");
  check(club::achievement_progress(224, st) == 0, "224: a Ford without a medal does not count");
  check(club::achievement_progress(225, st) == 2, "225 progress: cars");
}

void test_car_medals() {
  club::ClubData data;
  data.cars[7] = std::vector<club::Challenge>(5);
  const auto none = club::car_medals(data, 7, nullptr);
  check(none.available == 5 && none.earned == 0 && !none.begun, "car medals before begin");
  const std::vector<bool> done = {true, false, true, false, false};
  const auto some = club::car_medals(data, 7, &done);
  check(some.available == 5 && some.earned == 2 && some.begun, "car medals: two earned");
  const std::vector<bool> too_many(8, true);
  check(club::car_medals(data, 7, &too_many).earned == 5, "car medals clamp to the car's challenges");
  check(club::car_medals(data, 99, nullptr).available == 0, "a car without challenges");
}

void test_texts() {
  check(carview::format_count("{0 - medal count} MEDALS REQUIRED", 4) == "4 MEDALS REQUIRED", "format_count slot");
  check(carview::format_count("NO SLOT", 4) == "NO SLOT", "format_count without slot");
  check(carview::format_count(" NEW CARS REQUIRED", 3) == "3 NEW CARS REQUIRED",
        "format_count: the table's count-less form");
  club::ClubMedalStats st;
  auto lines = carview::club_achievement_lines(st);
  check(lines.size() == 10, "ten achievement lines");
  // No tables loaded: the fallbacks.
  check(carview::club_achievement_desc(lines[0]) == "EARN YOUR FIRST MEDAL.", "first medal text");
  check(carview::club_achievement_desc(lines[8]) == "EARN A MEDAL IN 13 DIFFERENT FORDS.", "Ford text, none yet");
  check(carview::club_required_text(lines[0]) == "1 MEDAL REQUIRED", "first medal: 1 medal required");
  check(carview::club_required_text(lines[8]) == "13 NEW CARS REQUIRED", "Ford: 13 cars required");
  st.ford_cars = 12;
  lines = carview::club_achievement_lines(st);
  check(carview::club_achievement_desc(lines[8]) == "EARN A MEDAL IN ONE MORE FORD.", "Ford text, one left");
  check(carview::club_required_text(lines[8]) == "1 NEW CAR REQUIRED", "Ford: 1 car required");
  st.ford_cars = 20;
  lines = carview::club_achievement_lines(st);
  check(lines[8].unlocked && lines[8].have == 13 && carview::club_required_text(lines[8]).empty(),
        "Ford unlocked, progress clamped");
  // Every ref the screens bind has a table and an id (the corpus test
  // resolves them).
  for (const auto& r : carview::club_text_refs())
    check(!r.table.empty() && !r.id.empty() && !r.fallback.empty(), "ref " + std::string(r.id));
}

ClubMenuData sample_data(int tutorial) {
  ClubMenuData d;
  d.tutorial = tutorial;
  d.achievements = carview::club_achievement_lines(d.stats);
  for (int i = 0; i < 3; ++i) {
    carview::ClubCarEntry e;
    e.car_id = 100 + i;
    e.name = "car " + std::to_string(i);
    e.medals.available = 5;
    e.counts.fill(true);
    e.counts[1] = i == 2;  // only the third is a Ferrari
    e.current = i == 1;
    d.garage.push_back(e);
  }
  d.current = 1;
  return d;
}

void test_menu_first_visit() {
  ClubMenuData d = sample_data(0);
  ClubMenu m;
  // The first visit: the intro video, then the home with its tutorial.
  m.show(d);
  check(m.screen == ClubMenu::Screen::kIntro, "first visit opens the intro");
  check(m.press(ClubMenu::Button::kAccept, d) == ClubMenu::Action::kTutorialStep && m.tutorial_to == 1,
        "intro done: tutorial step 1");
  d.tutorial = 1;
  check(m.screen == ClubMenu::Screen::kHome && m.popup == ClubMenu::Popup::kTutorial, "home with the tutorial");
  check(ClubMenu::tickets(d).size() == 1, "one ticket before any medal");
  check(m.press(ClubMenu::Button::kBack, d) == ClubMenu::Action::kNone, "B does not dismiss the tutorial");
  m.press(ClubMenu::Button::kAccept, d);
  check(m.popup == ClubMenu::Popup::kNone, "A dismisses the tutorial");
  // The first medal's ticket goes straight to its cars, the driven car first.
  m.press(ClubMenu::Button::kAccept, d);
  check(m.screen == ClubMenu::Screen::kCarList && m.filter == 216 && m.popup == ClubMenu::Popup::kTutorial,
        "first-medal ticket: car list with its tutorial");
  const auto cars = ClubMenu::cars_for(d, m.filter);
  check(cars.size() == 3 && cars[0] == 1, "car list: the driven car first");
  m.press(ClubMenu::Button::kAccept, d);
  m.press(ClubMenu::Button::kRight, d);
  check(m.car == 1, "car list moves right");
  m.press(ClubMenu::Button::kLeft, d);
  m.press(ClubMenu::Button::kAccept, d);
  check(m.popup == ClubMenu::Popup::kSelectAction && m.action_car == 101, "SELECT ACTION on the driven car");
  check(m.press(ClubMenu::Button::kBack, d) == ClubMenu::Action::kBack && m.popup == ClubMenu::Popup::kNone,
        "B leaves SELECT ACTION");
  m.press(ClubMenu::Button::kAccept, d);
  check(m.press(ClubMenu::Button::kAccept, d) == ClubMenu::Action::kBegin, "BEGIN CHALLENGES IN THIS CAR");
  check(!m.open() && m.action_car == 101 && m.tutorial_to == 2, "begun: closed, tutorial step 2");
}

void test_menu_after_begin() {
  ClubMenuData d = sample_data(2);
  d.current_begun = true;
  d.challenges.resize(5);
  ClubMenu m;
  m.show(d);
  check(m.screen == ClubMenu::Screen::kHome && m.popup == ClubMenu::Popup::kNone, "home, no tutorial");
  check(ClubMenu::tickets(d).front() == ClubMenu::Ticket::kCurrentCar, "the current car's ticket first");
  // CURRENT CAR CHALLENGES: the challenge list tutorial once.
  check(m.press(ClubMenu::Button::kAccept, d) == ClubMenu::Action::kTutorialStep && m.tutorial_to == 3,
        "details: tutorial step 3");
  check(m.screen == ClubMenu::Screen::kDetails && m.popup == ClubMenu::Popup::kTutorial, "details with its tutorial");
  m.press(ClubMenu::Button::kAccept, d);
  m.press(ClubMenu::Button::kUp, d);
  check(m.detail == 4, "details wrap up");
  m.press(ClubMenu::Button::kBack, d);
  check(m.screen == ClubMenu::Screen::kHome, "details back to home");
  // With a medal: the achievements list and its filtered cars.
  d.stats.total_medals = 1;
  d.stats.cars_with_medal = 1;
  d.achievements = carview::club_achievement_lines(d.stats);
  d.tutorial = 4;
  const auto t = ClubMenu::tickets(d);
  check(t.size() == 3 && t.back() == ClubMenu::Ticket::kViewAll, "three tickets after the first medal");
  check(ClubMenu::next_achievement(d) == 1, "next achievement after the first medal");
  m.home = 1;
  m.press(ClubMenu::Button::kAccept, d);
  check(m.screen == ClubMenu::Screen::kAchievements && m.achievement == 1, "achievements on the next one");
  m.press(ClubMenu::Button::kAccept, d);
  check(m.screen == ClubMenu::Screen::kCarList && m.filter == 217, "Ferrari list");
  check(ClubMenu::cars_for(d, 217) == std::vector<int>{2}, "only the Ferrari");
  m.press(ClubMenu::Button::kBack, d);
  check(m.screen == ClubMenu::Screen::kAchievements, "car list back to achievements");
  m.press(ClubMenu::Button::kBack, d);
  check(m.press(ClubMenu::Button::kBack, d) == ClubMenu::Action::kClose && !m.open(), "home B closes");
}

void test_menu_gated() {
  ClubMenuData d = sample_data(1);
  d.gate_title = carview::club_text::kUnavailableTitle;
  d.gate_message = carview::club_text::kUnavailableMsg;
  ClubMenu m;
  m.show(d);
  check(m.popup == ClubMenu::Popup::kUnavailable, "no green wristband: the unavailable popup");
  check(m.press(ClubMenu::Button::kAccept, d) == ClubMenu::Action::kClose && !m.open(), "OK closes");
  // An empty list denies.
  ClubMenuData e = sample_data(4);
  e.garage.clear();
  e.current = -1;
  ClubMenu n;
  n.show(e);
  n.screen = ClubMenu::Screen::kCarList;
  n.filter = 0;
  check(n.press(ClubMenu::Button::kAccept, e) == ClubMenu::Action::kDeny, "no cars: deny");
}

void test_save_fields() {
  drive::DriveSave s;
  s.car = "MAZ_MX5_94";
  s.club_tutorial = 3;
  s.rally_flow = 2;
  const std::string text = drive::save_to_text(s);
  drive::DriveSave back;
  std::string err;
  const bool read_ok = drive::save_from_text(text, back, &err);
  check(read_ok, "save round trip: " + err);
  check(back.club_tutorial == 3 && back.rally_flow == 2, "club_tutorial and rally_flow round trip");
  drive::DriveSave none;
  none.car = "MAZ_MX5_94";
  const std::string t0 = drive::save_to_text(none);
  check(t0.find("club_tutorial") == std::string::npos && t0.find("rally_flow") == std::string::npos,
        "zero fields are not written");
  drive::DriveSave clamp;
  check(drive::save_from_text(t0 + "club_tutorial 9\nrally_flow -3\n", clamp, &err), "clamped load");
  check(clamp.club_tutorial == 4 && clamp.rally_flow == 0, "fields clamp on load");
}

void test_pause_rows() {
  using PM = carview::PauseMenu;
  PM pm;
  pm.show(false);
  check(pm.items() == std::vector<PM::Action>{PM::kMap, PM::kSettings, PM::kPhotoMode, PM::kMessageCentre,
                                              PM::kSponsorChallenges, PM::kCareer, PM::kQuitToTitle},
        "no packages: the retail rows");
  pm.has_club = pm.has_rally = true;
  check(
      pm.items() == std::vector<PM::Action>{PM::kMap, PM::kClub, PM::kRally, PM::kSettings, PM::kPhotoMode,
                                            PM::kMessageCentre, PM::kSponsorChallenges, PM::kCareer, PM::kQuitToTitle},
      "both packages: MAP, 1000 CLUB, HORIZON RALLY, ...");
  const auto ex = pm.extras();
  check(
      std::find(ex.begin(), ex.end(), PM::kClub) == ex.end() && std::find(ex.begin(), ex.end(), PM::kRally) == ex.end(),
      "the packages' rows leave the extras");
  pm.has_club = false;
  check(pm.items()[1] == PM::kRally, "rally only: HORIZON RALLY after MAP");
  pm.in_rally_world = true;
  check(pm.items()[1] == PM::kSettings, "the rally world has no package rows");
  {
    pm.has_rally = true;
    const auto rx = pm.extras();
    check(std::find(rx.begin(), rx.end(), PM::kRally) != rx.end(), "the rally world keeps its hub in the extras");
    pm.has_rally = false;
    pm.in_rally_world = false;
    const auto nx = pm.extras();
    check(std::find(nx.begin(), nx.end(), PM::kRally) == nx.end() &&
              std::find(nx.begin(), nx.end(), PM::kClub) == nx.end(),
          "no packages: no Rally or 1000 Club row anywhere");
    pm.in_rally_world = true;
  }
  pm.show(true);
  check(pm.items().front() == PM::kRestartRace, "racing rows unchanged");
}

}  // namespace

// V4-19: the Rally achievements' writers (208, 211, 214, 215).
void test_rally_achievement_writers() {
  drive::AchievementSignals a;
  drive::note_rally_car(a, 1504, false);  // given
  drive::note_rally_car(a, 1504, true);   // the same car bought later
  drive::note_rally_car(a, 1505, true);
  drive::note_rally_car(a, 0, true);
  check(a.rally_cars_obtained == std::vector<int64_t>{1504, 1505}, "208: each Rally car once");
  check(a.rally_cars_purchased == std::vector<int64_t>{1504, 1505}, "215: bought cars once");

  drive::RallyStageSignals s;
  check(s.step(0, 30.0, 1.0) == 0.0, "no air counted off a stage");
  s.start();
  check(s.step(4, 30.0, 1.0) == 0.0, "wheels down: no air");
  check(s.step(0, 30.0, 0.5) == 15.0 && s.step(0, 10.0, 0.5) == 5.0, "air: horizontal travel while airborne");
  check(s.air_m == 20.0, "air summed over the stage");
  check(s.finish(1), "214: a clean win is flawless");
  check(!s.finish(1), "one count per stage");
  s.start();
  s.crashed = true;
  check(!s.finish(1), "214: a crash spoils it");
  s.start();
  s.rewound = true;
  check(!s.finish(1), "214: a rewind spoils it");
  s.start();
  check(!s.finish(2), "214: only a win");

  drive::DriveSave sv;
  sv.car = "MER_300SL_54";
  sv.ach = a;
  sv.ach.rally_air_m = 1609.5;
  sv.ach.rally_flawless_stage_wins = 2;
  drive::DriveSave back;
  std::string err;
  check(drive::save_from_text(drive::save_to_text(sv), back, &err), "save round trip: " + err);
  check(back.ach.rally_cars_obtained == a.rally_cars_obtained && back.ach.rally_cars_purchased == a.rally_cars_purchased,
        "208/215 saved");
  check(back.ach.rally_air_m > 1609.0 && back.ach.rally_flawless_stage_wins == 2, "211/214 saved");
}

int main() {
  test_rally_achievement_writers();
  test_achievement_table();
  test_car_medals();
  test_texts();
  test_menu_first_visit();
  test_menu_after_begin();
  test_menu_gated();
  test_save_fields();
  test_pause_rows();
  std::printf("club_rally_tests: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
