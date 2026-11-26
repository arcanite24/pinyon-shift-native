// The UI layer's CPU rasterizer: text measures and draws inside its box,
// and SHIFT_UI_SPECIMEN=<file.png> writes a specimen of the font and the
// shapes to look at.
#include <algorithm>
#include <cmath>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "events_page.h"
#include "fh1data/ui_scene.h"
#include "fh1data/vfont.h"
#include "game_hud.h"
#include "title_menu.h"
#include "hud.h"
#include "hud_scene.h"
#include "hud_screen.h"
#include "map_anim.h"
#include "audio/xml.h"
#include "pause_screen.h"
#include "ui.h"
#include "ui4_chrome.h"

static int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

using namespace carview::ui;

namespace {

void check_hud_screen(const carview::DriveHud& d) {
  List l;
  carview::build_drive_hud(l, d, 1280, 720);
  CHECK(!l.items().empty());
  std::vector<uint8_t> out_px;
  rasterize(l.items(), out_px, 1280, 720);
  size_t lit = 0;
  for (size_t i = 3; i < out_px.size(); i += 4) lit += out_px[i] > 0;
  CHECK(lit > 5000);
}

race::CareerCatalog make_test_catalog() {
  race::CareerCatalog cat;
  cat.wristbands = {
      {0, 0, "", "Yellow", 0, 0},
      {1, 220, "", "Green", 0, 424},
      {2, 750, "", "Blue", 0, 0},
      {3, 1500, "", "Gold", 0, 0},
  };
  cat.hubs = {{1, "", 300, {57}, true, {200, 0, 200}, 24.0}};
  cat.sites = {{race::FestivalSite::kRaceCentral, "RACE CENTRAL", "racecentral", {0, 0, 0}, 24.0}};

  race::CarSpec corrado;
  corrado.car_id = 309, corrado.media_name = "VW_Corrado_95", corrado.stock_class = 2;
  corrado.props.drive_type = shift::data::kDriveFwd;
  race::CarSpec skyline;
  skyline.car_id = 1219, skyline.media_name = "NIS_Skyline", skyline.stock_class = 5;
  skyline.props.drive_type = shift::data::kDriveAwd;
  race::CarSpec mustang;
  mustang.car_id = 404, mustang.media_name = "FRD_MustangBoss_70", mustang.stock_class = 3;
  race::CarSpec rx7;
  rx7.car_id = 424, rx7.media_name = "MAZ_RX7_97", rx7.stock_class = 3;
  cat.cars[309] = corrado;
  cat.cars[1219] = skyline;
  cat.cars[404] = mustang;
  cat.cars[424] = rx7;

  // Completed festival event (level 0)
  race::CareerEvent fr02;
  fr02.event_id = 26, fr02.horizon_id = "FR02", fr02.level = 0, fr02.style = 1, fr02.target_class = 3;
  fr02.cash_prize = 4000, fr02.opponents = 7, fr02.has_trigger = true, fr02.trigger_pos = {50, 0, 0};
  fr02.trigger_radius = 25.0;

  // Uncompleted festival event (level 0)
  race::CareerEvent fr05;
  fr05.event_id = 27, fr05.horizon_id = "FR05", fr05.level = 0, fr05.style = 1, fr05.target_class = 2;
  fr05.cash_prize = 5000, fr05.opponents = 7, fr05.has_trigger = true, fr05.trigger_pos = {80, 0, 0};
  fr05.trigger_radius = 25.0;
  shift::data::RestrictionSet fwd_only;
  fwd_only.drive_type = shift::data::kDriveFwd;
  fr05.restrictions.buckets[0] = fwd_only;

  // Locked Green wristband event
  race::CareerEvent fr10;
  fr10.event_id = 30, fr10.horizon_id = "FR10", fr10.level = 1, fr10.style = 1, fr10.unlock_points_req = 220;
  fr10.target_class = 3;

  // Street event in Hub 1
  race::CareerEvent st01;
  st01.event_id = 57, st01.horizon_id = "ST01", st01.style = 2, st01.hub_id = 1, st01.unlock_points_req = 300;
  st01.level = 1;

  // Showcase with loaner car
  race::CareerEvent sh01;
  sh01.event_id = 101, sh01.horizon_id = "EXHIBITION_1", sh01.style = 3, sh01.popularity_rank_req = 225;
  sh01.player_car_id = 404, sh01.prize_car_id = 424, sh01.cash_prize = 10000;

  // Nemesis showdown with prereq (fr10, event_id 30 — uncompleted in test catalog)
  race::CareerEvent nm01;
  nm01.event_id = 169, nm01.horizon_id = "NEM_1", nm01.style = 5, nm01.prereq_event_id = 30;

  // Headline finale (style 6)
  race::CareerEvent sd01;
  sd01.event_id = 200, sd01.horizon_id = "FINALE", sd01.style = 6, sd01.unlock_points_req = 1500, sd01.level = 3;

  cat.events = {fr02, fr05, fr10, st01, sh01, nm01, sd01};
  // Extra locked events to verify locked-row cap (8) and 7-row scroll window.
  for (int i = 0; i < 6; ++i) {
    race::CareerEvent extra;
    extra.event_id = 300 + i;
    extra.horizon_id = "LOCKED_EXTRA_" + std::to_string(i);
    extra.style = 1;
    extra.level = 2;
    extra.unlock_points_req = 750;
    cat.events.push_back(extra);
  }
  return cat;
}

void test_events_page() {
  const auto cat = make_test_catalog();
  const std::vector<garage::ClassBand> bands = {
      {0.2, 100, "F"}, {0.3, 200, "E"}, {0.4, 300, "D"}, {0.5, 400, "C"}, {0.6, 500, "B"}, {0.7, 600, "A"}};
  race::CareerRecord rec;
  rec.wristband_points = 220;  // Green pending while collected_wristband == 0
  rec.event_places[26] = 1;    // FR02 completed in 1st

  carview::EventsPage page;
  page.open(cat, rec, /*collected_wristband=*/0, /*popularity_rank=*/250, /*car=*/309, /*class=*/2, bands, 0);
  CHECK(page.is_open());
  CHECK(page.hub_filter() == 0);

  std::string title, dtitle;
  std::vector<std::string> lines, dlines;
  int sel = 0;
  page.fill(title, lines, sel, dtitle, dlines);
  CHECK(title.find("ALL EVENTS") != std::string::npos);
  CHECK(lines.size() == 7);  // capped to 7 visible rows
  CHECK(lines[0] == "* COLLECT GREEN WRISTBAND!");
  CHECK(dtitle == "RACE CENTRAL WRISTBAND CEREMONY");
  bool has_bonus = false;
  for (const auto& dl : dlines)
    if (dl.find("BONUS CAR:") != std::string::npos) has_bonus = true;
  CHECK(has_bonus);

  // Sort order after pending wristband: [NEW] (FR05) -> [1ST] (FR02) -> [LOCKED] (capped at 8)
  CHECK(lines[1] == "[NEW] FR05");
  CHECK(lines[2] == "[1ST] FR02");
  CHECK(lines[3].rfind("[LOCKED] ", 0) == 0);

  // Wrap-around on move(-1) lands on last entry, move(+1) returns to 0.
  page.move(-1);
  const auto* last_e = page.selected_entry();
  CHECK(last_e != nullptr && !last_e->unlocked);
  page.move(+1);
  CHECK(page.selected_entry() && page.selected_entry()->is_collect_wristband);

  // Check locked reason variants across tabs.
  page.switch_tab(+1);  // kFestival
  page.fill(title, lines, sel, dtitle, dlines);
  CHECK(title.find("FESTIVAL") != std::string::npos);
  // Move down to a locked Green wristband event.
  page.move(3);
  CHECK(page.selected_entry() && page.selected_entry()->reason.find("COLLECT GREEN WRISTBAND") != std::string::npos);

  page.switch_tab(+2);  // kShowcase (index 0 is pending wristband, index 1 is EXHIBITION_1)
  page.move(1);
  page.fill(title, lines, sel, dtitle, dlines);
  CHECK(title.find("SHOWCASES") != std::string::npos);
  CHECK(page.selected_entry() && page.selected_entry()->reason == "REACH POPULARITY RANK 225");

  page.switch_tab(+1);  // kShowdown (index 0 is pending wristband, index 1 is NEM_1, index 2 is FINALE)
  page.move(1);
  page.fill(title, lines, sel, dtitle, dlines);
  CHECK(title.find("SHOWDOWNS") != std::string::npos);
  CHECK(page.selected_entry() && page.selected_entry()->reason == "WIN PREREQUISITE EVENT FIRST");
  page.move(1);
  CHECK(page.selected_entry() && page.selected_entry()->reason.find("GOLD WRISTBAND & 1,500 PTS") != std::string::npos);

  // Check eligibility failure (Skyline AWD in D-class FWD FR05) & Showcase loaner when rank 225 & Green collected.
  page.open(cat, rec, /*collected_wristband=*/1, /*popularity_rank=*/225, /*car=*/1219, /*class=*/5, bands, 0);
  page.fill(title, lines, sel, dtitle, dlines);
  CHECK(lines[0] == "[NEW] FR05");  // pending wristband gone once collected
  CHECK(page.selected_entry() && !page.selected_entry()->eligible);
  CHECK(page.selected_entry()->reason.find("REQUIRES CLASS") != std::string::npos);
  page.switch_tab(+3);  // kShowcase -> unlocked + loaner
  page.fill(title, lines, sel, dtitle, dlines);
  CHECK(page.selected_entry() && page.selected_entry()->unlocked && page.selected_entry()->uses_loaner);
  bool has_loaner = false, has_ready = false;
  for (const auto& dl : dlines) {
    if (dl.find("SHOWCASE CAR PROVIDED:") != std::string::npos) has_loaner = true;
    if (dl.find("READY TO RACE") != std::string::npos) has_ready = true;
  }
  CHECK(has_loaner && has_ready);

  // Rasterize EventsPage detail card via build_drive_hud.
  carview::DriveHud hud;
  hud.driving = false;
  hud.menu_title = title;
  hud.menu = lines;
  hud.menu_selected = sel;
  hud.detail_title = dtitle;
  hud.detail_lines = dlines;
  check_hud_screen(hud);

  // hub_filter > 0 locks tab to kStreet, omits pending wristband, and switch_tab is a no-op.
  page.open(cat, rec, /*collected_wristband=*/0, /*popularity_rank=*/250, /*car=*/309, /*class=*/2, bands, 1);
  page.switch_tab(+1);
  page.fill(title, lines, sel, dtitle, dlines);
  CHECK(page.hub_filter() == 1);
  CHECK(title.find("STREET RACE HUB 1") != std::string::npos);
  CHECK(!lines.empty() && lines[0].find("COLLECT GREEN WRISTBAND") == std::string::npos);
}

void test_pause_menu() {
  carview::PauseMenu pm;
  pm.events_label = "COLLECT GREEN WRISTBAND!";
  pm.difficulty_label = "< DIFFICULTY: HARD (+30%) >";

  // Closed fill() leaves HudState menu empty.
  carview::HudState closed_st;
  pm.fill(closed_st);
  CHECK(closed_st.menu.empty() && closed_st.menu_title.empty());

  using PM = carview::PauseMenu;
  // Free roam: the retail rows (925_PAUSE_MENU), no RESUME row (B resumes).
  pm.show(false);
  CHECK((pm.items() == std::vector<PM::Action>{PM::kMap, PM::kSettings, PM::kPhotoMode, PM::kMessageCentre,
                                                PM::kSponsorChallenges, PM::kCareer, PM::kQuitToTitle}));
  {
    // No string tables loaded: the English fallbacks.
    carview::HudState st;
    pm.fill(st);
    CHECK(st.menu_title == "paused");
    CHECK((st.menu == std::vector<std::string>{"MAP", "SETTINGS", "PHOTO MODE", "MESSAGE CENTER",
                                               "SPONSOR CHALLENGES", "MY PROFILE", "QUIT"}));
  }
  CHECK(pm.selected_action() == PM::kMap);
  pm.move(-1);
  CHECK(pm.selected_action() == PM::kQuitToTitle);
  pm.move(+1);
  CHECK(pm.selected_action() == PM::kMap);

  // The engine's screens sit behind EXTRAS, which keeps the row to return to.
  pm.move(2);
  pm.open_extras();
  CHECK(pm.screen == PM::Screen::kExtras && pm.selected == 0);
  {
    const auto ex = pm.extras();
    for (PM::Action a : {PM::kEvents, PM::kGarage, PM::kUpgrades, PM::kPaint, PM::kAutoshow, PM::kCarClub,
                         PM::kAchievements, PM::kReplay})
      CHECK(std::find(ex.begin(), ex.end(), a) != ex.end());
    // Without the packages neither row shows (V4-22).
    for (PM::Action a : {PM::kRally, PM::kClub}) CHECK(std::find(ex.begin(), ex.end(), a) == ex.end());
    CHECK(pm.rows() == ex);
    CHECK(pm.selected_action() == PM::kEvents);
  }
  CHECK(pm.label(PM::kEvents) == "COLLECT GREEN WRISTBAND!");
  CHECK(pm.label(PM::kClub) == "1000 CLUB");
  CHECK(pm.label(PM::kCarClub) == "CAR CLUB");
  CHECK(pm.label(PM::kRally) == "HORIZON RALLY");
  CHECK(pm.label(PM::kReplay) == "REPLAYS");
  CHECK(pm.back());  // B: back to the rows, on the row left
  CHECK(pm.open && pm.screen == PM::Screen::kList && pm.selected_action() == PM::kPhotoMode);
  pm.open_screen(PM::Screen::kSponsorChallenges);
  CHECK(pm.screen == PM::Screen::kSponsorChallenges && pm.sponsor_selected == 0);
  CHECK(pm.back() && pm.screen == PM::Screen::kList);
  CHECK(!pm.back() && !pm.open);  // B on the rows resumes

  // Racing: RESTART, MAP, PHOTO MODE, SPONSOR CHALLENGES, MY PROFILE, QUIT RACE.
  pm.show(true);
  CHECK((pm.items() == std::vector<PM::Action>{PM::kRestartRace, PM::kMap, PM::kPhotoMode, PM::kSponsorChallenges,
                                                PM::kCareer, PM::kQuitRace}));
  pm.move(4);
  carview::HudState st;
  pm.fill(st);
  CHECK(st.menu.size() == 6 && st.menu_selected == 4);
  CHECK(st.menu[0] == "RESTART" && st.menu[5] == "QUIT RACE");
  pm.open_extras();
  CHECK(pm.selected_action() == PM::kReplay);
  CHECK(pm.label(PM::kDifficulty) == "< DIFFICULTY: HARD (+30%) >");
  CHECK(pm.label(PM::kReplay) == "VIEW REPLAY");

  // The selection cross-fade starts when the drawn selection changes.
  pm.show(false);
  pm.note_shown(10.0);
  CHECK(pm.shown_selected == 0 && pm.shown_from == -1);
  pm.move(1);
  pm.note_shown(11.0);
  CHECK(pm.shown_selected == 1 && pm.shown_from == 0 && pm.shown_at == 11.0);
  carview::PauseScreenHud ps;
  carview::fill_pause_screen(pm, 11.05, ps);
  CHECK(ps.selected == 1 && ps.from == 0 && ps.fade > 0.4 && ps.fade < 0.6);
  carview::fill_pause_screen(pm, 12.0, ps);
  CHECK(ps.fade == 1.0 && ps.from == -1);
}

void test_scene_screen_strings_and_cues() {
  using carview::StringRef;
  // Keys: table hash << 16 | id hash, or the raw id.
  CHECK(carview::string_key({"PauseMenu", "IDS_Map", 0, "MAP"}) == (0x3739u << 16 | 0xE909u));
  CHECK(carview::string_key({"PauseMenu", {}, 0xDD6B, "SETTINGS"}) == (0x3739u << 16 | 0xDD6Bu));
  CHECK(carview::string_key(carview::pause_text::kSelect) == (0xB59Du << 16 | 0x6FA4u));
  // Every retail row is bound to the PauseMenu table; the extras are the engine's.
  using PM = carview::PauseMenu;
  for (PM::Action a : {PM::kMap, PM::kPhotoMode, PM::kMessageCentre, PM::kSponsorChallenges, PM::kCareer,
                       PM::kQuitToTitle, PM::kRestartRace, PM::kQuitRace}) {
    const StringRef r = carview::pause_row_text(a, false);
    CHECK(r.table == "PauseMenu" && !r.id.empty() && !r.engine_label);
  }
  CHECK(carview::pause_row_text(PM::kSettings, false).engine_label);
  CHECK(carview::pause_row_text(PM::kGarage, false).table.empty());

  // Text binding through game_text: the table's text, else the fallback;
  // an engine label always shows its own.
  carview::set_game_text([](const std::string& v) -> std::string {
    if (v == "_&" + std::to_string(0x3739u << 16 | 0xE909u)) return "KARTE";
    if (v == "_&" + std::to_string(0x3739u << 16 | 0xDD6Bu)) return "MULTIJUGADOR";
    if (v == "_&" + std::to_string(carview::string_key(carview::pause_text::kCredits)))
      return "{colour='255 0 0 255'}{0}{colour_pop} Cr{x}";
    return v;
  });
  // Format slots survive the markup strip (other unknown {...} do not).
  CHECK(carview::format1(carview::text_of(carview::pause_text::kCredits), "1,250") ==
        "{colour='255 0 0 255'}1,250{colour_pop} Cr");
  CHECK(carview::text_of(carview::pause_row_text(PM::kMap, false)) == "KARTE");
  CHECK(carview::text_of(carview::pause_row_text(PM::kSettings, false)) == "SETTINGS");
  CHECK(carview::lookup(carview::pause_row_text(PM::kSettings, false)) == std::optional<std::string>("MULTIJUGADOR"));
  CHECK(carview::text_of(carview::pause_row_text(PM::kPhotoMode, false)) == "PHOTO MODE");
  CHECK(!carview::lookup(carview::pause_row_text(PM::kPhotoMode, false)));
  carview::set_game_text(nullptr);
  CHECK(carview::format1("COLLECT {0} WRISTBAND", "BLUE") == "COLLECT BLUE WRISTBAND");

  // Navigation cues: ui4audio.xml play names.
  CHECK(carview::ui_play_name(carview::UiEvent::kOpen) == "Pause");
  CHECK(carview::ui_play_name(carview::UiEvent::kClose) == "UnPause");
  CHECK(carview::ui_play_name(carview::UiEvent::kMove) == "Vscroll");
  CHECK(carview::ui_play_name(carview::UiEvent::kAccept) == "Accept");
  CHECK(carview::ui_play_name(carview::UiEvent::kBack) == "Cancel");

  // Rows wrap; a window keeps the selection in view.
  CHECK(carview::wrap_row(0, -1, 7) == 6 && carview::wrap_row(6, 1, 7) == 0 && carview::wrap_row(3, 0, 0) == 0);
  CHECK(carview::window_first(0, 10, 7) == 0 && carview::window_first(9, 10, 7) == 3 &&
        carview::window_first(5, 6, 7) == 0);

  // No scene loaded: the pause screen does not draw (the caller falls back).
  carview::set_ui_scenes(nullptr);
  carview::ui::List l;
  carview::DriveHud d;
  carview::PauseScreenHud ps;
  ps.rows = {"MAP"};
  CHECK(!carview::draw_pause_screen(l, d, ps, 1280, 720));
}

// A "rotation" group turned by NodeOverride::turn carries its children
// round its own position (the dial's needle and hatches), and the dial's
// scale follows the redline.
void test_scene_turn_and_dial_scale() {
  fh1::UiScene sc;
  fh1::UiObject grp;
  grp.kind = fh1::UiObjectKind::kGroup, grp.id = 1, grp.name = "rotation";
  sc.objects.push_back(grp);
  fh1::UiObject mod;
  mod.kind = fh1::UiObjectKind::kModel, mod.id = 2, mod.name = "hatch";
  sc.objects.push_back(mod);
  fh1::UiObject mat;
  mat.kind = fh1::UiObjectKind::kMaterial, mat.id = 3, mat.colour = {1, 1, 1, 1};
  sc.objects.push_back(mat);
  // The pivot at canvas (100, 100); the hatch 50 px above it, 10 px square.
  fh1::UiNode pivot;
  pivot.parent = -1, pivot.object = 1, pivot.object_index = 0, pivot.x = 100 - 640, pivot.y = 360 - 100;
  sc.nodes.push_back(pivot);
  fh1::UiNode hatch;
  hatch.parent = 0, hatch.object = 2, hatch.object_index = 1, hatch.x = 100 - 640, hatch.y = 360 - 50;
  hatch.bounds_min = {-5, -5, 0}, hatch.bounds_max = {5, 5, 0};
  sc.nodes.push_back(hatch);
  carview::SceneDraw sd(&sc);
  auto centre = [&](float turn, float dy) {
    carview::SceneBindings b;
    b.index(0).turn = turn;
    b.index(1).dy = dy;
    List l;
    sd.draw_subtree(l, {0, 0, 1}, 0, b);
    CHECK(l.items().size() == 1);
    const auto& r = l.items()[0].rect;
    return std::array<float, 2>{0.5f * (r[0] + r[2]), 0.5f * (r[1] + r[3])};
  };
  auto near = [](std::array<float, 2> a, float x, float y) { return std::fabs(a[0] - x) < 0.5f && std::fabs(a[1] - y) < 0.5f; };
  CHECK(near(centre(0, 0), 100, 50));
  // A quarter turn counter-clockwise (scene y up) takes the top to the left.
  CHECK(near(centre(1.5707963f, 0), 50, 100));
  CHECK(near(centre(-1.5707963f, 0), 150, 100));
  // A child's own offset turns with it.
  CHECK(near(centre(1.5707963f, -10), 40, 100));

  // The dial's scale: a 7,000 rpm redline reads 0..9 in ones with numerals
  // to 8, an 8,000 one 0..10 in twos; the MX-5 with its first camshaft
  // (7,300 rpm) still reads 0..9 in ones (first30b idle-cam0 capture).
  const auto mx5 = carview::dial_scale(7000), r34 = carview::dial_scale(8000), low = carview::dial_scale(5500);
  CHECK(mx5.top == 9 && mx5.step == 1 && mx5.last == 8);
  CHECK(carview::dial_scale(7300).top == 9 && carview::dial_scale(7300).step == 1);
  CHECK(r34.top == 10 && r34.step == 2 && r34.last == 10);
  CHECK(low.top == 7 && low.step == 1 && low.last == 6);
  CHECK(carview::dial_scale(9500).top == 12 && carview::dial_scale(9500).step == 2);
  // A sponsored event's name loses its sponsor line.
  CHECK(carview::event_name_without_sponsor("HORIZON presents The Gauntlet", "HORIZON presents") == "The Gauntlet");
  CHECK(carview::event_name_without_sponsor("Red Rock Rampage", "HORIZON presents") == "Red Rock Rampage");
  CHECK(carview::event_name_without_sponsor("Red Rock", "") == "Red Rock");
  // The race clock: two-digit minutes.
  CHECK(carview::hud_clock(16.851) == "00:16.851" && carview::hud_clock(164.698) == "02:44.698");
  CHECK(carview::hud_clock(-1) == "00:00.000");
}

// The front end's model (title_menu.h): Press Start, the profile step, the
// original's two rows and the engine's EXTRAS.
void test_title_menu_model() {
  using M = carview::TitleMenu;
  M m;
  CHECK(m.phase == M::Phase::kPressStart && m.rows().empty() && !m.menu_visible());
  CHECK(m.move(1).cue == M::Cue::kNone);
  // Press Start: the profile step, then the menu.
  auto st = m.confirm();
  CHECK(st.cue == M::Cue::kAccept && st.action == M::Action::kNone && m.phase == M::Phase::kProfile);
  CHECK(m.confirm().cue == M::Cue::kNone && m.back().cue == M::Cue::kNone);
  m.update(M::kProfileSeconds * 0.5);
  CHECK(m.phase == M::Phase::kProfile);
  m.update(M::kProfileSeconds);
  CHECK(m.phase == M::Phase::kMenu && m.popup == M::Popup::kNone);
  // SINGLE PLAYER and MULTIPLAYER, wrapping.
  CHECK(m.rows().size() == 2 && m.rows()[0] == M::Row::kSinglePlayer && m.rows()[1] == M::Row::kMultiplayer);
  CHECK(m.move(1).cue == M::Cue::kMove && m.selected_row() == M::Row::kMultiplayer);
  m.move(1);
  CHECK(m.selected == 0);
  m.move(-1);
  // MULTIPLAYER offline: NO NETWORK ACCESS; A closes it and leaves the row.
  st = m.confirm();
  CHECK(st.cue == M::Cue::kDeny && st.action == M::Action::kNone && m.popup == M::Popup::kNoNetwork);
  CHECK(m.move(1).cue == M::Cue::kNone && m.selected == 1);
  CHECK(m.confirm().action == M::Action::kNone && m.popup == M::Popup::kNone && m.selected == 1);
  // SINGLE PLAYER: the first-time career without a played save, else continue.
  m.move(1);
  CHECK(m.confirm().action == M::Action::kStartCareer);
  m.has_save = true;
  CHECK(m.confirm().action == M::Action::kContinueCareer);
  // EXTRAS (X): the engine's rows; B back keeps the menu row.
  m.move(1);
  CHECK(m.open_extras().cue == M::Cue::kAccept && m.phase == M::Phase::kExtras && m.selected == 0);
  CHECK(m.rows().size() == 4 && m.rows()[0] == M::Row::kNewCareer && m.rows()[3] == M::Row::kQuit);
  CHECK(m.open_extras().cue == M::Cue::kNone);
  // NEW CAREER over a save asks first; B NO closes, A YES starts it.
  CHECK(m.confirm().action == M::Action::kNone && m.popup == M::Popup::kConfirmNewCareer);
  CHECK(m.back().cue == M::Cue::kBack && m.popup == M::Popup::kNone && m.phase == M::Phase::kExtras);
  m.confirm();
  CHECK(m.confirm().action == M::Action::kNewCareer && m.popup == M::Popup::kNone);
  m.move(1);
  CHECK(m.confirm().action == M::Action::kFreeRoam);
  m.move(1);
  CHECK(m.confirm().action == M::Action::kSettings);
  m.move(1);
  CHECK(m.confirm().action == M::Action::kQuit);
  CHECK(m.hover(1).cue == M::Cue::kMove && m.selected == 1 && m.hover(1).cue == M::Cue::kNone);
  CHECK(m.hover(9).cue == M::Cue::kNone);
  CHECK(m.back().cue == M::Cue::kBack && m.phase == M::Phase::kMenu && m.selected == 1);
  // B on the menu goes back to Press Start; B there quits (PC).
  CHECK(m.back().cue == M::Cue::kBack && m.phase == M::Phase::kPressStart);
  CHECK(m.back().action == M::Action::kQuit);
  // No played save: NEW CAREER starts without asking.
  M n;
  n.show_menu();
  n.open_extras();
  CHECK(n.confirm().action == M::Action::kNewCareer && n.popup == M::Popup::kNone);
  // A new profile: Press Start, the profile step, then VIDEO (BRIGHTNESS 60,
  // SUBTITLES off); left/right change the row, Y resets, B does nothing, A
  // saves and goes on to the menu, and VIDEO does not come again.
  M v;
  v.first_profile = true;
  v.confirm();
  v.update(M::kProfileSeconds + 0.01);
  CHECK(v.phase == M::Phase::kVideo && !v.menu_visible() && v.video_row == 0);
  CHECK(v.brightness == M::kDefaultBrightness && !v.subtitles);
  CHECK(v.adjust(1).cue == M::Cue::kMove && v.brightness == M::kDefaultBrightness + M::kBrightnessStep);
  CHECK(v.move(1).cue == M::Cue::kMove && v.video_row == 1);
  CHECK(v.adjust(-1).cue == M::Cue::kMove && v.subtitles);
  CHECK(v.reset_video().cue == M::Cue::kAccept && v.brightness == M::kDefaultBrightness && !v.subtitles);
  CHECK(v.back().cue == M::Cue::kNone && v.phase == M::Phase::kVideo);
  v.move(1);
  for (int i = 0; i < 200; ++i) v.adjust(1);
  CHECK(v.brightness == 100 && v.adjust(1).cue == M::Cue::kNone);
  v.brightness = 70;
  st = v.confirm();
  CHECK(st.action == M::Action::kSaveVideo && v.phase == M::Phase::kMenu && v.brightness == 70 && !v.first_profile);
  v.back();
  v.confirm();
  v.update(M::kProfileSeconds + 0.01);
  CHECK(v.phase == M::Phase::kMenu);
  CHECK(v.adjust(1).cue == M::Cue::kNone && v.reset_video().cue == M::Cue::kNone);
  // A save that did not read: INVALID PROFILE once the menu shows.
  M p;
  p.profile_error = true;
  p.confirm();
  p.update(M::kProfileSeconds + 0.01);
  CHECK(p.phase == M::Phase::kMenu && p.popup == M::Popup::kProfileCorrupt);
  CHECK(p.confirm().action == M::Action::kNone && p.popup == M::Popup::kNone);
}

void test_hud_overlays() {
  // TitleScreenHud confirm_new_career
  {
    carview::TitleScreenHud ts;
    ts.active = true;
    ts.menu.has_save = true;
    ts.menu.show_menu();
    ts.menu.open_extras();
    ts.menu.confirm();  // EXTRAS > NEW CAREER over a save: the confirm popup
    CHECK(ts.menu.popup == carview::TitleMenu::Popup::kConfirmNewCareer);
    carview::DriveHud d;
    d.title_screen = &ts, d.time = 1.0;
    check_hud_screen(d);
  }
  // WorldMarkerHud out-of-range + ineligible + festival + empty prompt fallback
  {
    carview::WorldMarkerHud wm;
    wm.visible = true, wm.in_range = false, wm.is_festival = true, wm.eligible = false;
    wm.screen_x = 640, wm.screen_y = 260, wm.distance_m = 85.0;
    wm.title = "RACE CENTRAL", wm.subtitle = "HORIZON FESTIVAL";
    carview::DriveHud d;
    d.world_marker = &wm, d.time = 1.0;
    check_hud_screen(d);

    wm.in_range = true, wm.is_festival = false, wm.prompt.clear();
    check_hud_screen(d);
  }
  // SkillChainHud banked / lost / rank-up states
  {
    carview::SkillChainHud sk;
    sk.active = false, sk.banked_total = 3200, sk.banked_at = 1.0, sk.popularity_rank = 240, sk.popularity_progress = 0.6;
    carview::DriveHud d;
    d.skills = &sk, d.time = 2.0;
    check_hud_screen(d);

    sk.banked_at = -1e9, sk.lost_at = 1.0;
    d.time = 1.5;
    check_hud_screen(d);

    sk.lost_at = -1e9, sk.rank_up_at = 1.0, sk.popularity_rank = 225;
    check_hud_screen(d);
  }
  // MapOverlayHud (hovered marker + fallback)
  {
    carview::MapOverlayHud mo;
    mo.active = true, mo.title = "RED ROCK RAMPAGE", mo.subtitle = "FESTIVAL RACE", mo.action_hint = "ENTER EVENT";
    mo.distance_m = 1450.0;
    carview::DriveHud d;
    d.map_overlay = &mo;
    check_hud_screen(d);

    mo.title.clear(), mo.subtitle.clear(), mo.action_hint.clear(), mo.distance_m = -1;
    check_hud_screen(d);
  }
  // NameTagHud during RaceHud::kRacing (on-screen leader + normal + off-screen clipped)
  {
    carview::RaceHud r;
    r.phase = carview::RaceHud::kRacing, r.event = "RED ROCK RAMPAGE", r.place = 2, r.cars = 8, r.clock = 42.5;
    carview::DriveHud d;
    d.race = &r, d.kmh = 160, d.rpm = 5500, d.gear = 4;
    d.name_tags = {{640.0f, 220.0f, "DARIUS FLYNT", true},
                   {480.0f, 250.0f, "ALI HOWARD", false},
                   {-100.0f, 250.0f, "CLIPPED", false}};
    check_hud_screen(d);
  }
  // RaceHud::kFinished results_page = 1 (mileage & difficulty bonus) and results_page = 3 (won_car_name)
  {
    carview::RaceHud r;
    r.phase = carview::RaceHud::kFinished, r.event = "RED ROCK RAMPAGE", r.place = 1, r.cars = 8;
    r.finish_time = 123.45, r.base_credits = 4000, r.mileage_bonus = 200, r.difficulty_pct = 30, r.difficulty_bonus = 1260;
    r.credits = 5460, r.balance = 25460, r.won_car_name = "1997 Mazda RX-7";
    r.results_page = 1;
    carview::DriveHud d;
    d.race = &r, d.time = 5.0;
    check_hud_screen(d);

    r.results_page = 3;
    check_hud_screen(d);
  }
}

void test_world_trigger() {
  const auto cat = make_test_catalog();
  race::CareerRecord rec;
  // 140 m cutoff: at (141, 0) no site or event is within 140 m of RaceCentral(0,0), FR02(50,0), FR05(80,0)?
  // Check (-141, 0): 141 m from RaceCentral(0,0), > 190 m from FR02/FR05.
  CHECK(carview::select_world_trigger(cat, rec, 0, 250, -141.0, 0.0).kind == carview::WorldTrigger::kNone);
  const auto far_site = carview::select_world_trigger(cat, rec, 0, 250, -139.0, 0.0);
  CHECK(far_site.kind == carview::WorldTrigger::kFestivalSite && !far_site.in_range);

  // in_range priority: place test site/event where player is closer to out-of-range A than in-range B.
  race::CareerCatalog prio;
  race::FestivalSite site_a{race::FestivalSite::kAutoshow, "AUTOSHOW", "autoshow", {0, 0, 0}, 10.0};
  race::FestivalSite site_b{race::FestivalSite::kWorkshop, "WORKSHOP", "workshop", {45, 0, 0}, 30.0};
  prio.sites = {site_a, site_b};
  // At x = 18: dist to A = 18 m (not in_range, radius 10), dist to B = 27 m (in_range, radius 30) -> B wins!
  const auto chosen = carview::select_world_trigger(prio, rec, 0, 250, 18.0, 0.0);
  CHECK(chosen.kind == carview::WorldTrigger::kFestivalSite && chosen.site &&
        chosen.site->kind == race::FestivalSite::kWorkshop && chosen.in_range);

  // Locked Street Hub at (200, 0, 200) is excluded while points < 300, selected once unlocked.
  CHECK(carview::select_world_trigger(cat, rec, 0, 250, 200.0, 200.0).kind == carview::WorldTrigger::kNone);
  rec.wristband_points = 300;
  const auto unlocked_hub = carview::select_world_trigger(cat, rec, 1, 250, 200.0, 200.0);
  CHECK(unlocked_hub.kind == carview::WorldTrigger::kStreetHub && unlocked_hub.hub && unlocked_hub.hub->id == 1 &&
        unlocked_hub.in_range);
}

void test_hud_abs_tcs_lamps() {
  static fh1::UiScene scene = [] {
    fh1::UiScene s;
    auto add_obj = [&](fh1::UiObjectKind k, uint32_t id, const char* name) {
      fh1::UiObject o;
      o.kind = k, o.id = id, o.name = name;
      if (k == fh1::UiObjectKind::kText) o.font = "b", o.text_size = 14, o.text_width = 40;
      s.objects.push_back(std::move(o));
      return int(s.objects.size()) - 1;
    };
    auto add_node = [&](int parent, uint32_t oid, float alpha = 1.0f) {
      fh1::UiNode n;
      n.parent = parent, n.object = oid, n.opacity = alpha, n.alpha = alpha;
      n.sx = 1, n.sy = 1, n.x = 500, n.y = -250;
      n.bounds_min = {-10, -10, 0}, n.bounds_max = {10, 10, 0};
      s.nodes.push_back(n);
      return int(s.nodes.size()) - 1;
    };
    auto add_mat_tex = [&](uint32_t mid, uint32_t tid, const char* path) {
      fh1::UiObject m;
      m.kind = fh1::UiObjectKind::kMaterial, m.id = mid, m.name = "Mat";
      m.colour = {1, 1, 1, 1};
      s.objects.push_back(m);
      fh1::UiObject t;
      t.kind = fh1::UiObjectKind::kTexture, t.id = tid, t.name = "Tex", t.owner = mid, t.path = path;
      t.matrix[0] = 1.0f, t.matrix[5] = 1.0f;
      s.objects.push_back(t);
    };
    add_obj(fh1::UiObjectKind::kGroup, 1, "GAUGE_ANALOG");
    add_obj(fh1::UiObjectKind::kModel, 2, "MainBackRing");
    add_obj(fh1::UiObjectKind::kMaterial, 3, "RingMat");
    add_obj(fh1::UiObjectKind::kGroup, 4, "_P_Tach_Needle");
    add_obj(fh1::UiObjectKind::kModel, 5, "Rectangle");
    add_obj(fh1::UiObjectKind::kMaterial, 6, "NeedleMat");
    add_obj(fh1::UiObjectKind::kGroup, 7, "ASSIST_ICONS");
    add_obj(fh1::UiObjectKind::kGroup, 8, "HUD_ABS");
    add_obj(fh1::UiObjectKind::kModel, 9, "Rectangle_ON");
    add_mat_tex(10, 11, "GAME:\\MEDIA\\UI\\TEXTURES\\HORIZON\\HUD\\DIALS\\ABSON.TGA");
    add_obj(fh1::UiObjectKind::kGroup, 12, "HUD_TCS");
    add_obj(fh1::UiObjectKind::kModel, 13, "Rectangle_ON");
    add_mat_tex(14, 15, "GAME:\\MEDIA\\UI\\TEXTURES\\HORIZON\\HUD\\DIALS\\TCS ON.TGA");
    add_obj(fh1::UiObjectKind::kGroup, 16, "Speedo");
    add_obj(fh1::UiObjectKind::kText, 17, "digit0");
    add_obj(fh1::UiObjectKind::kText, 18, "digit1");
    add_obj(fh1::UiObjectKind::kText, 19, "digit2");
    add_obj(fh1::UiObjectKind::kText, 20, "TEXT_SPEED_UNIT_ANALOG");
    add_obj(fh1::UiObjectKind::kText, 21, "TEXT_GEAR_ANALOG");

    const int g = add_node(-1, 1);
    add_node(g, 2);
    const int tn = add_node(g, 4);
    add_node(tn, 5);
    const int ai = add_node(g, 7);
    const int abs_g = add_node(ai, 8);
    add_node(abs_g, 9, 0.5f);
    const int tcs_g = add_node(ai, 12);
    add_node(tcs_g, 13, 0.5f);
    const int sp = add_node(g, 16);
    add_node(sp, 17);
    add_node(sp, 18);
    add_node(sp, 19);
    add_node(g, 20);
    add_node(g, 21);

    for (auto& n : s.nodes)
      for (size_t i = 0; i < s.objects.size(); ++i)
        if (s.objects[i].id == n.object) n.object_index = int(i);
    return s;
  }();

  carview::set_ui_scenes([&](const std::string& name) -> const fh1::UiScene* {
    return name == "947_HUD" ? &scene : nullptr;
  });
  carview::set_ui_textures([](const std::string& path, std::array<float, 4>& uv) {
    if (path == "HUD/DIALS/ABSON") {
      uv = {0.1f, 0.1f, 0.2f, 0.2f};
      return true;
    }
    if (path == "HUD/DIALS/TCS ON") {
      uv = {0.3f, 0.3f, 0.4f, 0.4f};
      return true;
    }
    uv = {0.5f, 0.5f, 0.6f, 0.6f};
    return path.rfind("HUD/DIALS/", 0) == 0;
  });

  auto find_colour = [](const List& l, float u0) -> uint32_t {
    for (const auto& it : l.items()) {
      if (it.kind == kImage && std::fabs(it.q[0] - u0) < 1e-4f) return it.color;
    }
    return 0;
  };
  const uint32_t off_col = fade(rgba(110, 114, 122), 0.36f);
  const uint32_t on_col = fade(rgba(232, 236, 244), 0.92f);
  const uint32_t blink_on_col = rgba(255, 168, 28, 255);

  {
    List l;
    carview::DriveHud d;
    d.abs_enabled = false, d.tcs_enabled = false;
    d.abs = false, d.tcs = false;
    carview::build_drive_hud(l, d, 1280, 720);
    CHECK(find_colour(l, 0.1f) == off_col);
    CHECK(find_colour(l, 0.3f) == off_col);
  }
  {
    List l;
    carview::DriveHud d;
    d.abs_enabled = true, d.tcs_enabled = false;
    d.abs = false, d.tcs = false;
    carview::build_drive_hud(l, d, 1280, 720);
    CHECK(find_colour(l, 0.1f) == on_col);
    CHECK(find_colour(l, 0.3f) == off_col);
  }
  {
    List l;
    carview::DriveHud d;
    d.abs_enabled = false, d.tcs_enabled = true;
    d.abs = false, d.tcs = false;
    carview::build_drive_hud(l, d, 1280, 720);
    CHECK(find_colour(l, 0.1f) == off_col);
    CHECK(find_colour(l, 0.3f) == on_col);
  }
  {
    List l;
    carview::DriveHud d;
    d.abs_enabled = true, d.tcs_enabled = true;
    d.abs = true, d.abs_flash_timer = 0.25f;
    d.time = 0.035;  // sin(2*pi*7*0.035) > 0 -> amber blink ON
    carview::build_drive_hud(l, d, 1280, 720);
    CHECK(find_colour(l, 0.1f) == blink_on_col);
    CHECK(find_colour(l, 0.3f) == on_col);
  }
}

// The map profile animations: frames, keys, eases, loops and triggers as
// the title reads them (MapProfileFullscreen.xml).
void check_map_animations() {
  namespace ma = carview::mapanim;
  const char* xml =
      "<animations>"
      "<animation name=\"ghost\"><frame advance=\"0\"><key type=\"alpha\" value=\"1\" ease=\"none\"/></frame>"
      "<frame advance=\"0.3\"><key type=\"alpha\" value=\"0.3\" ease=\"none\"/></frame></animation>"
      "<animation name=\"radar\" loop=\"loop\"><frame advance=\"1.2\"><key type=\"alpha\" value=\"0.5\"/>"
      "<key type=\"scaleX\" value=\"0\"/><trigger type=\"audio\" value=\"Map_Radar\"/></frame>"
      "<frame advance=\"0.6\"><key type=\"alpha\" value=\"0\"/><key type=\"scaleX\" value=\"8\" ease=\"in\"/></frame>"
      "<frame advance=\"1.2\"/></animation>"
      "<animation name=\"pulse\" loop=\"pingpong\" speed=\"0.5\"><frame><key type=\"scaleY\" value=\"1\"/></frame>"
      "<frame advance=\"1\"><key type=\"scaleY\" value=\"2\"/></frame></animation>"
      "<animation name=\"pulse\"><frame><key type=\"scaleY\" value=\"9\"/></frame></animation>"
      "<animation name=\"smooth\"><frame><key type=\"angle\" value=\"0\" ease=\"smooth\"/></frame>"
      "<frame advance=\"1\"><key type=\"angle\" value=\"1\" ease=\"smooth\"/></frame>"
      "<frame advance=\"1\"><key type=\"angle\" value=\"2\" ease=\"smooth\"/></frame>"
      "<frame advance=\"1\"><key type=\"angle\" value=\"3\" ease=\"smooth\"/></frame></animation>"
      "<animation name=\"barn_find_region1_unlock\"><frame advance=\"0.6\"><key type=\"alpha\" value=\"0\"/>"
      "<key type=\"scaleY\" value=\"0.2\"/><key type=\"posX\" value=\"1310\"/><key type=\"posY\" value=\"-4921\"/></frame>"
      "<frame advance=\"1.5\"><key type=\"alpha\" value=\"1\"/><trigger type=\"set_cursor\" value=\"set\"/></frame>"
      "</animation></animations>";
  audio::XmlNode root;
  CHECK(audio::parse_xml(xml, root, nullptr));
  ma::Library lib;
  ma::parse_animations(root, lib);
  CHECK(lib.by_name.size() == 5);

  // Linear keys ("none"): straight between them, held after the last.
  const ma::Animation* ghost = lib.find("ghost");
  CHECK(ghost && ghost->loop == ma::kStop && std::fabs(ghost->duration - 0.3f) < 1e-5f);
  if (ghost) {
    CHECK(std::fabs(ghost->sample(ma::kAlpha, 0.15f, 1) - 0.65f) < 1e-4f);
    CHECK(ghost->sample(ma::kAlpha, 5, 1) == 0.3f);
    CHECK(!ghost->done(0.2f) && ghost->done(0.3f));
    ma::IconLook look;
    ma::apply(*ghost, 1, look);
    CHECK(look.alpha == 0.3f && look.scale_x == 1 && look.scale_y == 1);
  }

  // The default ease is flat at both keys: the midpoint is halfway and the
  // value barely moves just after the start.
  const ma::Animation* radar = lib.find("radar");
  CHECK(radar && radar->loop == ma::kLoop && std::fabs(radar->duration - 3.0f) < 1e-5f);
  if (radar) {
    CHECK(radar->sample(ma::kAlpha, 0.5f, 1) == 0.5f);  // before the first key
    CHECK(std::fabs(radar->sample(ma::kAlpha, 1.5f, 1) - 0.25f) < 1e-4f);
    CHECK(radar->sample(ma::kAlpha, 1.26f, 1) > 0.48f);
    CHECK(std::fabs(radar->wrap(4.5f) - 1.5f) < 1e-4f);
    // The audio trigger fires once a pass.
    CHECK(radar->crossed(0, 1.3f).size() == 1);
    CHECK(radar->crossed(1.3f, 4.0f).empty());
    CHECK(radar->crossed(1.3f, 4.3f).size() == 1);
    CHECK(radar->crossed(0, 7.3f).size() == 3);
    CHECK(radar->triggers.size() == 1 && radar->triggers[0].value == "Map_Radar");
  }

  // The speed scales the key times; ping-pong runs back. The second
  // "pulse" does not replace the first.
  const ma::Animation* pulse = lib.find("pulse");
  CHECK(pulse && pulse->loop == ma::kPingPong && std::fabs(pulse->duration - 0.5f) < 1e-5f);
  if (pulse) {
    CHECK(std::fabs(pulse->wrap(0.75f) - 0.25f) < 1e-5f);
    CHECK(std::fabs(pulse->sample(ma::kScaleY, pulse->wrap(0.75f), 1) - 1.5f) < 1e-4f);
    CHECK(pulse->sample(ma::kScaleY, 0.5f, 1) == 2);
  }

  // "smooth" runs through the neighbours (a straight line here).
  const ma::Animation* smooth = lib.find("smooth");
  CHECK(smooth != nullptr);
  if (smooth) CHECK(std::fabs(smooth->sample(ma::kAngle, 1.5f, 0) - 1.5f) < 1e-4f);

  // The barn rumour camera move: blend 0 -> 1 between 0.6 s and 2.1 s, then
  // the cursor trigger.
  const ma::Animation* barn = lib.find("barn_find_region1_unlock");
  CHECK(barn && std::fabs(barn->duration - 2.1f) < 1e-5f);
  if (barn) {
    ma::CameraMove m = ma::camera_at(*barn, 0.3f);
    CHECK(m.has_target && m.has_zoom && m.x == 1310 && m.z == -4921 && m.weight == 0);
    m = ma::camera_at(*barn, 1.35f);
    CHECK(std::fabs(m.weight - 0.5) < 1e-4 && std::fabs(m.zoom - 0.2) < 1e-6);
    CHECK(ma::camera_at(*barn, 3).weight == 1);
    const auto tr = barn->crossed(2.0f, 2.2f);
    CHECK(tr.size() == 1 && tr[0]->type == "set_cursor");
    CHECK(std::fabs(ma::map_distance(0.2) - (200 + 7452 * 0.2)) < 1e-6);
  }

  CHECK(ma::ease_of("", false) == ma::kBoth && ma::ease_of("bogus", true) == ma::kSmooth);
  ma::Channel c;
  CHECK(ma::channel_of("posX2", c) && c == ma::kScaleX && ma::channel_of("zoom", c) && c == ma::kScaleY);
  CHECK(!ma::channel_of("width", c));

  // A symbolizer's event bindings.
  audio::XmlNode sym;
  CHECK(audio::parse_xml("<Symbolizer><animation event=\"hover\" animation=\"bulge\"/>"
                         "<animation event=\"ghost\" animation=\"ghost\"/></Symbolizer>",
                         sym, nullptr));
  const ma::Bindings b = ma::parse_bindings(sym);
  CHECK(b.size() == 2 && b.at("hover") == "bulge");
}

// SpeedCamera's slides from the trap and zone state (Q242).
void check_speed_camera_phase() {
  using carview::speed_camera_phase;
  freeroam::SpeedActivityHud h;
  CHECK(speed_camera_phase(h).slide.empty());
  // A trap: its speed (SHOW) for 1 s, then the result for 2 s.
  h.banner_active = true;
  h.banner_title = "speed_camera_03";
  h.banner_speed_mph = 140.0;
  h.banner_best_mph = 150.0;
  h.banner_age_s = 0.5;
  auto p = speed_camera_phase(h);
  CHECK(p.slide == "SHOW" && std::fabs(p.ms - 500.0) < 1e-6 && !p.zone && p.title == "speed_camera_03");
  h.banner_age_s = 1.5;
  p = speed_camera_phase(h);
  CHECK(p.slide == "OLDPB" && std::fabs(p.ms - 499.0) < 1e-6 && p.best_mph == 150.0);
  h.banner_new_pb = true;
  CHECK(speed_camera_phase(h).slide == "NEWPB");
  h.banner_age_s = 3.1;
  CHECK(speed_camera_phase(h).slide.empty());
  // A zone: SHOW as it starts, ON while it runs with the live average.
  h = {};
  h.zone_active = true;
  h.active_zone_id = "average_speed_01";
  h.live_avg_mph = 90.0;
  h.zone_elapsed_s = 0.2;
  CHECK(speed_camera_phase(h).slide == "SHOW");
  h.zone_elapsed_s = 5.0;
  p = speed_camera_phase(h);
  CHECK(p.slide == "ON" && p.zone && p.speed_mph == 90.0);
  // Its result skips SHOW; a cancelled run plays ABORT for 0.2 s.
  h = {};
  h.banner_active = h.banner_is_zone = true;
  h.banner_age_s = 0.3;
  CHECK(speed_camera_phase(h).slide == "OLDPB");
  h = {};
  h.abort_active = true;
  h.abort_title = "average_speed_01";
  h.abort_speed_mph = 70.0;
  h.abort_age_s = 0.1;
  p = speed_camera_phase(h);
  CHECK(p.slide == "ABORT" && p.speed_mph == 70.0 && std::fabs(p.ms - 100.0) < 1e-6);
  h.abort_age_s = 0.25;
  CHECK(speed_camera_phase(h).slide.empty());
}

}  // namespace

int main() {
  // Every letter and digit has strokes.
  check_map_animations();
  check_speed_camera_phase();
  CHECK(!font_segments().empty());
  TextStyle s;
  s.size = 20;
  const float w = List::measure("ABC", s);
  CHECK(w > 30 && w < 60);
  CHECK(List::measure("", s) == 0);

  // Text lands where it is drawn: some coverage inside its box, none far
  // outside.
  List list;
  list.text(10, 10, "HELLO 123", s);
  std::vector<uint8_t> px;
  rasterize(list.items(), px, 200, 40);
  int inside = 0, outside = 0;
  for (uint32_t y = 0; y < 40; ++y)
    for (uint32_t x = 0; x < 200; ++x) {
      const bool box = x >= 7 && x <= 13 + uint32_t(List::measure("HELLO 123", s)) && y >= 6 && y <= 33;
      if (px[(y * 200 + x) * 4 + 3] > 0) ++(box ? inside : outside);
    }
  CHECK(inside > 200);
  CHECK(outside == 0);

  test_hud_abs_tcs_lamps();
  // Verify all new Career & Horizon HUD screens build and rasterize cleanly at 1280x720.
  auto check_screen = [&](const carview::DriveHud& d) {
    List l;
    carview::build_drive_hud(l, d, 1280, 720);
    CHECK(!l.items().empty());
    std::vector<uint8_t> out_px;
    rasterize(l.items(), out_px, 1280, 720);
    size_t lit = 0;
    for (size_t i = 3; i < out_px.size(); i += 4) lit += out_px[i] > 0;
    CHECK(lit > 5000);
  };
  {
    carview::TitleScreenHud ts;
    ts.active = true;
    carview::DriveHud d;
    d.title_screen = &ts, d.time = 1.0;
    check_screen(d);
    ts.menu.has_save = true;
    ts.menu.show_menu();
    check_screen(d);
    ts.menu.open_extras();
    check_screen(d);
  }
  {
    carview::WorldMarkerHud wm;
    wm.visible = true, wm.in_range = true, wm.screen_x = 600, wm.screen_y = 300;
    wm.title = "RED ROCK RAMPAGE", wm.subtitle = "FESTIVAL RACE", wm.prompt = "ENTER NOW", wm.badge = "NEW";
    carview::SkillChainHud sk;
    sk.active = true, sk.chain_points = 500, sk.multiplier = 2, sk.last_skill = "GREAT DRIFT", sk.last_points = 250;
    sk.popularity_rank = 240, sk.popularity_progress = 0.5;
    carview::DriveHud d;
    d.world_marker = &wm, d.skills = &sk, d.time = 2.0;
    check_screen(d);
  }
  {
    carview::RaceHud r;
    r.phase = carview::RaceHud::kGrid, r.event = "RED ROCK RAMPAGE", r.event_type = "FESTIVAL RACE";
    r.grid_menu_open = true, r.grid_menu = {"START RACE", "< DIFFICULTY: MED (+15%) >", "QUIT RACE"};
    carview::DriveHud d;
    d.race = &r, d.time = 1.0;
    check_screen(d);
  }
  {
    carview::CeremonyHud c;
    c.active = true, c.headline = "NEW WRISTBAND EARNED!", c.band_name = "GREEN WRISTBAND", c.band_colour = "GREEN";
    c.subtitle = "7 NEW FESTIVAL EVENTS UNLOCKED", c.bonus_car = "1997 Mazda RX-7";
    carview::DriveHud d;
    d.ceremony = &c;
    check_screen(d);
  }
  test_events_page();
  test_pause_menu();
  test_scene_screen_strings_and_cues();
  test_scene_turn_and_dial_scale();
  test_title_menu_model();
  test_hud_overlays();
  test_world_trigger();

  // 1. Multi-frame-safe glyph atlas: never emits full glyph-band zero-alpha wipes
  //    mid-frame and never evicts active glyphs when transient images churn.
  {
    auto vf = std::make_shared<fh1::VFont>();
    vf->space_width = 0.35f;
    for (uint32_t cp = 33; cp < 127; ++cp) {
      fh1::VFontGlyph g;
      g.code_point = cp;
      g.advance = 0.62f;
      g.min_x = 0.05f;
      g.max_x = 0.57f;
      g.min_y = 0.0f;
      g.max_y = 0.78f;
      g.vertices = {{0.08f, 0.02f, 0.0f, 1.0f, 1.0f},
                    {0.54f, 0.02f, 0.0f, 1.0f, 1.0f},
                    {0.54f, 0.74f, 0.0f, 1.0f, 1.0f},
                    {0.08f, 0.74f, 0.0f, 1.0f, 1.0f}};
      g.indices = {0, 1, 2, 0, 2, 3};
      vf->glyphs[cp] = std::move(g);
    }
    set_atlas_size(2048);
    set_gpu_atlas_mode(true);
    set_vector_fonts(vf, vf, vf, vf);
    atlas_take_dirty_patches();

    std::vector<uint8_t> tile(128 * 128 * 4, 200);
    uint64_t wipe_patches = 0;
    for (int frame = 0; frame < 100; ++frame) {
      List l;
      for (int row = 0; row < 14; ++row) {
        const float sz = 24.0f + float((row % 5) * 6);
        TextStyle ts{.size = sz, .color = rgba(255, 255, 255), .font = (row % 2 == 0) ? Font::kD : Font::kE};
        l.text(40.0f, 40.0f + float(row) * 38.0f,
               "GRAPHICS PRESET RESOLUTION SHADOWS MSAA VSYNC AUDIO RADIO " + std::to_string(row), ts);
      }
      std::array<float, 4> tuv{};
      atlas_image("EventTickets/Stress/" + std::to_string(frame), 128, 128, tile, tuv, true);
      CHECK(tuv[1] * float(atlas_size()) >= 1280.0f);

      const auto patches = atlas_take_dirty_patches();
      for (const auto& p : patches) {
        if (p.x == 0 && p.y == 0 && p.w == atlas_size() && p.h >= 64) ++wipe_patches;
      }
    }
    CHECK(wipe_patches == 0);
    CHECK(atlas_glyph_evictions() == 0);
    CHECK(atlas_stats().glyph_count > 30);

    // A fresh renderer's empty atlas (the Rally world switch): atlas_reset
    // forgets every glyph and image, and both are put again on next use.
    {
      std::array<float, 4> kuv{};
      CHECK(atlas_image("reset.probe", 16, 16, std::vector<uint8_t>(16 * 16 * 4, 255), kuv));
      CHECK(atlas_find("reset.probe", kuv));
      atlas_reset();
      CHECK(!atlas_find("reset.probe", kuv));
      CHECK(atlas_stats().glyph_count == 0 && atlas_stats().image_count == 0);
      CHECK(atlas_take_dirty_patches().empty());
      List l;
      l.text(40.0f, 40.0f, "AB", TextStyle{.size = 24.0f, .color = rgba(255, 255, 255), .font = Font::kD});
      CHECK(atlas_stats().glyph_count >= 2);
      CHECK(!atlas_take_dirty_patches().empty());
      CHECK(atlas_image("reset.probe", 16, 16, std::vector<uint8_t>(16 * 16 * 4, 255), kuv));
      CHECK(atlas_find("reset.probe", kuv));
    }

    set_vector_fonts(nullptr, nullptr, nullptr, nullptr);
    set_gpu_atlas_mode(false);
    set_atlas_size(kAtlasSize);
    atlas_take_dirty();
  }

  // 2. List row heights & Y positions stay strictly invariant under selection change.
  {
    auto item_box = [](const Instance& it, float& cx, float& cy, float& w, float& h) -> bool {
      if (it.kind == kRoundRect) {
        cx = 0.5f * (it.rect[0] + it.rect[2]);
        cy = 0.5f * (it.rect[1] + it.rect[3]);
        w = it.rect[2] - it.rect[0];
        h = it.rect[3] - it.rect[1];
        return true;
      }
      if (it.kind == kImage && it.p[1] > 0.0f) {
        cx = 0.5f * (it.rect[0] + it.rect[2]);
        cy = 0.5f * (it.rect[1] + it.rect[3]);
        w = 2.0f * it.p[1];
        h = 2.0f * it.p[2];
        return true;
      }
      return false;
    };
    auto pause_row_rects = [&](int sel) {
      carview::DriveHud d;
      d.driving = false;
      d.pause = true;
      d.menu_title = "paused";
      d.menu = {"MAP", "SETTINGS", "PHOTO MODE", "MESSAGE CENTER", "SPONSOR CHALLENGES", "MY PROFILE", "QUIT"};
      d.menu_selected = sel;
      List l;
      carview::build_drive_hud(l, d, 1280, 720);
      std::vector<std::array<float, 4>> rects;
      for (const auto& it : l.items()) {
        float cx = 0, cy = 0, w = 0, h = 0;
        if (!item_box(it, cx, cy, w, h)) continue;
        if (cx > 160.0f && cx < 750.0f && w > 120.0f && w < 1050.0f && h > 40.0f && h < 60.0f) {
          rects.push_back({cx, cy, w, h});
        }
      }
      return rects;
    };
    const auto r0 = pause_row_rects(0);
    const auto r1 = pause_row_rects(1);
    const auto r4 = pause_row_rects(4);
    CHECK(r0.size() == 7 && r1.size() == 7 && r4.size() == 7);
    for (size_t i = 0; i < std::min({r0.size(), r1.size(), r4.size()}); ++i) {
      CHECK(std::fabs(r0[i][1] - r1[i][1]) < 0.05f);
      CHECK(std::fabs(r0[i][3] - r1[i][3]) < 0.05f);
      CHECK(std::fabs(r0[i][1] - r4[i][1]) < 0.05f);
      CHECK(std::fabs(r0[i][3] - r4[i][3]) < 0.05f);
    }

    auto blade_row_rects = [&](int sel) {
      carview::ui4::TiltedBladeSpec spec;
      spec.title = "TEST BLADE";
      spec.list_header = "ITEMS";
      spec.selected_index = sel;
      for (int i = 0; i < 6; ++i) {
        carview::ui4::BladeRow r;
        r.label = "ROW " + std::to_string(i);
        r.value = "VAL " + std::to_string(i);
        r.selected = (i == sel);
        spec.rows.push_back(r);
      }
      List l;
      carview::ui4::tilted_blade_screen(l, 1280, 720, spec);
      std::vector<std::array<float, 4>> rects;
      for (const auto& it : l.items()) {
        float cx = 0, cy = 0, w = 0, h = 0;
        if (!item_box(it, cx, cy, w, h)) continue;
        // Row pills are 696x34; list header pill is 696x28.
        if (w > 680.0f && w < 710.0f && h > 32.0f && h < 36.0f) {
          rects.push_back({cx, cy, w, h});
        }
      }
      return rects;
    };
    const auto b0 = blade_row_rects(0);
    const auto b3 = blade_row_rects(3);
    CHECK(b0.size() == 6 && b3.size() == 6);
    for (size_t i = 0; i < std::min(b0.size(), b3.size()); ++i) {
      CHECK(std::fabs(b0[i][1] - b3[i][1]) < 0.05f);
      CHECK(std::fabs(b0[i][3] - b3[i][3]) < 0.05f);
    }
  }

  // 3. Speedometer 1:1 checks: glowing trailing arc sweep, multi-layer redline, and faint leading zeros.
  {
    carview::DriveHud d;
    d.driving = true;
    d.kmh = 0.0;
    d.rpm = 4800.0;
    d.redline_rpm = 8000.0;
    d.gear = 4;
    List l;
    carview::build_drive_hud(l, d, 1280, 720);
    int pink_arcs = 0;
    int faint_digits = 0;
    int bright_digits = 0;
    for (const auto& it : l.items()) {
      const uint8_t cr = uint8_t((it.color >> 16) & 0xFF);
      const uint8_t cg = uint8_t((it.color >> 8) & 0xFF);
      const uint8_t cb = uint8_t(it.color & 0xFF);
      const uint8_t ca = uint8_t((it.color >> 24) & 0xFF);
      if (it.kind == kArc && cr > 220 && cg < 140 && cb > 80) ++pink_arcs;
      if (it.kind == kGlyph && it.p[2] > 2.5f) {
        if (ca > 40 && ca <= 80) ++faint_digits;
        if (ca >= 245 && cr > 240) ++bright_digits;
      }
    }
    CHECK(pink_arcs >= 12);
    CHECK(faint_digits >= 2);
    CHECK(bright_digits >= 1);
  }

  // 4. Phase R2-6 checks: BGF attribute hashes + UiScene::geometry_of non-quad mesh + CONTROLLER/* glyph routing
  //    + per-page Post-Race tally timing + BAR/PSI boost & lap split delta formatting.
  {
    // The runtime's name hash, h = (h + c) * 65599; attributes keep 27 bits.
    CHECK(fh1::ui_name_hash("") == 0u);
    CHECK(fh1::ui_name_hash("a") == 97u * 65599u);
    CHECK(fh1::ui_attr_hash("opacity") == (fh1::ui_name_hash("opacity") & 0x07FFFFFFu));
    CHECK(fh1::ui_attr::kPositionX != fh1::ui_attr::kPositionY);

    // Non-quad UiGeometry mesh renders via SceneDraw::model() using UiScene::geometry_of().
    fh1::UiScene sc;
    fh1::UiObject mod;
    mod.kind = fh1::UiObjectKind::kModel, mod.id = 10, mod.name = "TriMesh", mod.geometry = 0;
    sc.objects.push_back(mod);
    fh1::UiObject mat;
    mat.kind = fh1::UiObjectKind::kMaterial, mat.id = 11, mat.colour = {1.0f, 0.2f, 0.5f, 1.0f};
    sc.objects.push_back(mat);
    fh1::UiGeometry tri;
    tri.index = 0, tri.is_unit_quad = false;
    tri.bounds_min = {-20, -20, 0}, tri.bounds_max = {20, 20, 0};
    tri.vertices = {{0.0f, 20.0f}, {20.0f, -20.0f}, {-20.0f, -20.0f}};
    sc.geometries.push_back(tri);
    fh1::UiNode nd;
    nd.parent = -1, nd.object = 10, nd.object_index = 0, nd.opacity = 1.0f, nd.alpha = 1.0f;
    sc.nodes.push_back(nd);
    carview::SceneDraw sd(&sc);
    List l_tri;
    CHECK(sd.draw_subtree(l_tri, {0, 0, 1}, 0) == 1);
    CHECK(l_tri.items().size() == 3);  // 3 line edges from non-quad UiGeometry

    // FBF model referencing CONTROLLER/A_BUTTON routes cleanly through PlayStation & Keyboard glyphs.
    fh1::UiObject tex;
    tex.kind = fh1::UiObjectKind::kTexture, tex.id = 12, tex.owner = 11;
    tex.path = "GAME:\\MEDIA\\UI\\TEXTURES\\HORIZON\\CONTROLLER\\A_BUTTON.TGA";
    tex.matrix[0] = 1.0f, tex.matrix[5] = 1.0f;
    sc.objects.push_back(tex);
    const auto prev_gs = carview::ui4::glyph_set();
    carview::ui4::set_glyph_set(carview::ui4::GlyphSet::kPlayStation);
    List l_ps;
    CHECK(sd.model(l_ps, {0, 0, 1}, 0));
    CHECK(l_ps.items().size() >= 3);  // disc + Cross lines
    carview::ui4::set_glyph_set(carview::ui4::GlyphSet::kKeyboard);
    List l_kb;
    CHECK(sd.model(l_kb, {0, 0, 1}, 0));
    CHECK(l_kb.items().size() >= 3);  // keycap rect + bezel + ENTER text
    carview::ui4::set_glyph_set(prev_gs);

    // Boost BAR/PSI + split delta + per-page post-race timer checks.
    CHECK(carview::format_boost_unit(14.5037738, true) == "1.00 BAR");
    CHECK(carview::format_boost_unit(14.5, false) == "14.5 PSI");
    CHECK(carview::format_split_delta(-0.42) == "-0.42s");
    CHECK(carview::format_split_delta(0.31) == "+0.31s");
    carview::reset_post_race_page_timer();
    carview::RaceHud rh;
    rh.phase = carview::RaceHud::kFinished, rh.finish_time = 50.0, rh.clock = 55.0, rh.results_page = 0;
    CHECK(std::fabs(carview::post_race_page_elapsed(rh)) < 1e-4);
    rh.clock = 58.0, rh.results_page = 1;
    CHECK(std::fabs(carview::post_race_page_elapsed(rh)) < 1e-4);
    rh.clock = 58.6;
    CHECK(std::fabs(carview::post_race_page_elapsed(rh) - 0.6) < 1e-4);
  }

  // 5. Phase PT-A regression tests (PT-01/PT-02/PT-03/PT-05):
  //    4K glyph packing across A/B/D/E + Settings/HUD/Toast strings + 1280x720 fmv.frame + 1000x300 logo
  //    never drops characters, never evicts glyphs, and never overwrites live same-frame transient artwork.
  {
    auto vf = std::make_shared<fh1::VFont>();
    vf->space_width = 0.35f;
    for (uint32_t cp = 33; cp < 127; ++cp) {
      fh1::VFontGlyph g;
      g.code_point = cp;
      g.advance = 0.60f;
      g.min_x = 0.05f;
      g.max_x = 0.55f;
      g.min_y = -0.15f;
      g.max_y = 0.80f;
      g.vertices = {{0.08f, 0.02f, 0.0f, 1.0f, 1.0f},
                    {0.52f, 0.02f, 0.0f, 1.0f, 1.0f},
                    {0.52f, 0.76f, 0.0f, 1.0f, 1.0f},
                    {0.08f, 0.76f, 0.0f, 1.0f, 1.0f}};
      g.indices = {0, 1, 2, 0, 2, 3};
      vf->glyphs[cp] = std::move(g);
    }
    set_atlas_size(kAtlasSize);  // 4096x4096 desktop atlas
    set_gpu_atlas_mode(true);
    set_vector_fonts(vf, vf, vf, vf);
    atlas_take_dirty_patches();

    std::vector<uint8_t> fmv_px(1280u * 720u * 4u, 220);
    std::vector<uint8_t> logo_px(1000u * 300u * 4u, 180);
    const std::string ascii_all = [] {
      std::string s;
      for (char c = 33; c < 127; ++c) s.push_back(c);
      return s;
    }();
    const std::string showcase_msg =
        "New Showcase Event unlocked: MUSTANG VS MUSTANG (1970 Ford Mustang Boss 429 vs P-51 MUSTANG)!";
    const float s4k = 2071.0f / 720.0f;

    auto count_non_space = [](std::string_view sv) {
      size_t n = 0;
      for (char c : sv)
        if (c != ' ') ++n;
      return n;
    };

    for (int frame = 0; frame < 30; ++frame) {
      std::array<float, 4> fmv_uv{}, logo_uv{};
      CHECK(atlas_update("fmv.frame", 1280, 720, fmv_px, fmv_uv));
      CHECK(atlas_image("E3/E3LARGELOGO", 1000, 300, logo_px, logo_uv));
      // fmv.frame routes to transient band (>= kTransientY0), logo stays in persistent band (< kTransientY0).
      CHECK(fmv_uv[1] * float(kAtlasSize) >= float(kTransientY0));
      CHECK(logo_uv[1] * float(kAtlasSize) >= float(kGlyphRows));
      CHECK(logo_uv[3] * float(kAtlasSize) <= float(kTransientY0));
      // Never overlap vertically or horizontally.
      CHECK(logo_uv[3] <= fmv_uv[1]);

      List l;
      size_t expected_glyphs = 0;
      const Font fonts[4] = {Font::kA, Font::kB, Font::kD, Font::kE};
      const float sizes[4] = {15.5f * s4k, 14.0f * s4k, 14.0f * s4k, 34.0f * s4k};
      for (int fi = 0; fi < 4; ++fi) {
        TextStyle ts{.size = sizes[fi], .color = rgba(255, 255, 255), .font = fonts[fi], .keep_case = true};
        l.text(40.0f, 40.0f + float(fi) * 80.0f, ascii_all, ts);
        expected_glyphs += count_non_space(ascii_all);
      }
      // Exact regression strings from PT-02 / PT-03 / PT-05.
      const char* reg_strs[] = {"QUIT RACE", "PLACE 8/8", "One more thing.", showcase_msg.c_str(),
                                "RADIO",     "SOUND",     "VOLUME",          "AUDIO OPTIONS"};
      for (int ri = 0; ri < 8; ++ri) {
        TextStyle ts{.size = (ri < 2 ? 32.0f : 14.5f) * s4k,
                     .color = rgba(255, 255, 255),
                     .font = (ri == 0) ? Font::kE : (ri >= 4 ? Font::kD : Font::kA),
                     .keep_case = (ri == 2 || ri == 3)};
        l.text(40.0f, 400.0f + float(ri) * 50.0f, reg_strs[ri], ts);
        expected_glyphs += count_non_space(reg_strs[ri]);
      }
      CHECK(l.items().size() == expected_glyphs);
      atlas_take_dirty_patches();
    }
    CHECK(atlas_glyph_evictions() == 0);

    // Same-frame transient guard even on 2048x2048 atlas: a second oversized transient image
    // in the SAME frame must NEVER wipe or overwrite an already-active fmv.frame shelf!
    set_atlas_size(2048);
    atlas_take_dirty_patches();
    std::array<float, 4> fmv2048_uv{}, big_uv{};
    CHECK(atlas_update("fmv.frame", 1280, 720, fmv_px, fmv2048_uv));
    const bool big_ok = atlas_image("Loading/SameFrameConflict", 1000, 300, logo_px, big_uv, true);
    CHECK(!big_ok);  // refuses mid-frame destructive clear_transient()
    std::array<float, 4> fmv2048_after{};
    CHECK(atlas_find("fmv.frame", fmv2048_after));
    CHECK(fmv2048_after == fmv2048_uv);

    set_vector_fonts(nullptr, nullptr, nullptr, nullptr);
    set_gpu_atlas_mode(false);
    set_atlas_size(kAtlasSize);
    atlas_take_dirty();
  }

  // 6. Phase PT-E HUD & World-Label parity tests (PT-37, PT-38, PT-40):
  //    Nameplate distance LOD + race cap + 2D de-overlapping vs world markers,
  //    Popularity badge ring interpolation + rank-up flip/pop/flash animation,
  //    and high-contrast ROAD DISCOVERED banner with zero stray yellow artifacts.
  {
    // PT-37a (Q361): 1280x720 Gauntlet fit 0.46 + 7.5 / (d + 8) (14.2 px at 15 m,
    // 11.1 px at 40 m, floored at 0.50 = 9 px); full alpha to 100 m, gone by 120 m.
    const auto lod_close = carview::compute_name_tag_lod(5.0);
    const auto lod_near = carview::compute_name_tag_lod(15.0);
    const auto lod_40 = carview::compute_name_tag_lod(40.0);
    const auto lod_100 = carview::compute_name_tag_lod(100.0);
    const auto lod_mid = carview::compute_name_tag_lod(110.0);
    const auto lod_far = carview::compute_name_tag_lod(125.0);
    CHECK(lod_close.visible && std::fabs(lod_close.scale - 1.0f) < 1e-3f);
    CHECK(lod_near.visible && std::fabs(lod_near.scale * 18.0f - 14.15f) < 0.5f && lod_near.alpha == 1.0f);
    CHECK(lod_40.visible && std::fabs(lod_40.scale * 18.0f - 11.10f) < 0.5f && lod_40.alpha == 1.0f);
    CHECK(lod_100.visible && lod_100.scale >= 0.50f && lod_100.scale <= 0.55f && lod_100.alpha == 1.0f);
    CHECK(lod_mid.visible && lod_mid.alpha > 0.20f && lod_mid.alpha < 0.80f);
    CHECK(!lod_far.visible && lod_far.alpha == 0.0f);

    // PT-37b: 7-car packed race straightaway never stacks into a 7-box vertical wall and has zero overlap.
    std::vector<carview::NameTagHud> pack;
    const char* drivers[7] = {"GUY GILBERT", "ADAM BARR", "SCOTT ROCKFELD", "ALEX DARROW",
                              "LAWRENCE LAM", "JED BROWN", "TERRY ADAMS"};
    const double dists[7] = {18.0, 24.0, 30.0, 36.0, 42.0, 52.0, 64.0};
    for (int i = 0; i < 7; ++i) {
      carview::NameTagHud nt;
      nt.screen_x = 640.0f + float((i % 3) - 1) * 8.0f;
      nt.screen_y = 310.0f - float(i) * 4.0f;
      nt.name = drivers[i];
      nt.leader = (i == 0);
      nt.distance_m = dists[i];
      nt.car_class = "D";
      nt.pi = 260 + i * 5;
      pack.push_back(nt);
    }
    std::vector<carview::ResolvedNameTag> resolved_pack;
    carview::layout_world_labels(pack, nullptr, /*in_race=*/true, 1280.0f, 720.0f, resolved_pack);
    CHECK(!resolved_pack.empty() && resolved_pack.size() <= 3);
    float min_y0 = 720.0f, max_y1 = 0.0f;
    for (size_t i = 0; i < resolved_pack.size(); ++i) {
      min_y0 = std::min(min_y0, resolved_pack[i].y0);
      max_y1 = std::max(max_y1, resolved_pack[i].y1);
      for (size_t j = i + 1; j < resolved_pack.size(); ++j) {
        const bool ov = (resolved_pack[i].x0 < resolved_pack[j].x1) && (resolved_pack[i].x1 > resolved_pack[j].x0) &&
                        (resolved_pack[i].y0 < resolved_pack[j].y1) && (resolved_pack[i].y1 > resolved_pack[j].y0);
        CHECK(!ov);
      }
    }
    CHECK((max_y1 - min_y0) < 90.0f);  // compact cluster, never a 7-box vertical wall into the sky

    // PT-37c: WorldMarker ("MONTANO PLAINS") vs Festival AI nameplate ("[C 395] JAMIE CAMPBELL") never collide.
    carview::WorldMarkerHud wm;
    wm.visible = true;
    wm.in_range = false;
    wm.is_festival = true;
    wm.screen_x = 560.0f;
    wm.screen_y = 285.0f;
    wm.distance_m = 141.0;
    wm.title = "MONTANO PLAINS";
    carview::NameTagHud jamie;
    jamie.screen_x = 640.0f;
    jamie.screen_y = 288.0f;
    jamie.name = "[C 395] JAMIE CAMPBELL";
    jamie.distance_m = 38.0;
    std::vector<carview::ResolvedNameTag> resolved_fr;
    carview::WorldMarkerLayout wm_box{};
    carview::layout_world_labels({jamie}, &wm, /*in_race=*/false, 1280.0f, 720.0f, resolved_fr, &wm_box);
    CHECK(wm_box.visible);
    CHECK(resolved_fr.size() == 1);
    if (!resolved_fr.empty()) {
      CHECK(resolved_fr[0].has_badge && resolved_fr[0].car_class == "C" && resolved_fr[0].pi_text == "395");
      const bool hit_wm = (resolved_fr[0].x0 < wm_box.x1) && (resolved_fr[0].x1 > wm_box.x0) &&
                          (resolved_fr[0].y0 < wm_box.y1) && (resolved_fr[0].y1 > wm_box.y0);
      CHECK(!hit_wm);
    }
    // Rasterize combined free-roam world marker + car nameplate cleanly.
    {
      carview::DriveHud d;
      d.driving = true;
      d.world_marker = &wm;
      d.name_tags = {jamie};
      check_hud_screen(d);
    }

    // Q282: the chain's total counts up to each new total over Count_Up_Duration (1.6 s).
    {
      carview::SkillTotalCountUp c;
      CHECK(c.update(true, 100, 10.0) == 0);    // a new chain's first total starts from 0
      CHECK(c.update(true, 100, 10.8) == 50);   // half way at 0.8 s
      CHECK(c.update(true, 100, 11.6) == 100);  // there at 1.6 s
      CHECK(c.update(true, 100, 12.0) == 100);  // and holds
      CHECK(c.update(true, 250, 12.0) == 100);  // a new award: from the shown value
      CHECK(c.update(true, 250, 12.8) == 175);
      CHECK(c.update(true, 400, 12.8) == 175);  // another one resets the rate
      CHECK(c.update(true, 400, 13.6) == 287);
      CHECK(c.update(true, 400, 14.4) == 400);
      CHECK(c.update(false, 0, 15.0) == 0);     // the chain ends
      CHECK(c.update(true, 50, 16.0) == 0);     // the next starts from 0 again
    }

    // PT-38: Popularity badge smooth radial ring interpolation, rank-up flip/pop/flash, and skill pulse.
    {
      carview::PopularityBadgeAnimator anim;
      carview::SkillChainHud sk;
      sk.active = true;
      sk.chain_points = 500;
      sk.multiplier = 2;
      sk.last_skill = "SPEED";
      sk.last_points = 100;
      sk.last_skill_at = 10.0;
      sk.popularity_rank = 250;
      sk.popularity_progress = 0.10;
      anim.update(sk, 10.0);
      anim.apply(sk);
      CHECK(std::fabs(sk.pop_anim_progress - 0.10) < 1e-3);

      // Jump progress to 0.85 -> ring interpolates smoothly instead of snapping instantaneously.
      sk.popularity_progress = 0.85;
      anim.update(sk, 10.05);
      anim.apply(sk);
      CHECK(sk.pop_anim_progress > 0.12 && sk.pop_anim_progress < 0.65);
      for (int step = 0; step < 30; ++step) {
        anim.update(sk, 10.10 + double(step) * 0.05);
      }
      anim.apply(sk);
      CHECK(sk.pop_anim_progress > 0.82 && sk.pop_anim_progress <= 0.851);

      // Rank-up 250 -> 249: elastic scale pop (1.0 -> 1.24 -> 1.0), vertical flip, and magenta/gold flash sweep.
      sk.active = false;
      sk.popularity_rank = 249;
      sk.popularity_progress = 0.15;
      sk.rank_up_at = 12.0;
      anim.update(sk, 12.0);
      anim.apply(sk);
      const auto f_start = carview::evaluate_popularity_badge_anim(sk, 12.0);
      CHECK(f_start.display_rank == 250);

      anim.update(sk, 12.15);
      anim.update(sk, 12.30);  // u ~ 0.22 (scale pop peak)
      anim.apply(sk);
      const auto f_peak = carview::evaluate_popularity_badge_anim(sk, 12.30);
      CHECK(f_peak.disc_scale >= 1.20f && f_peak.disc_scale <= 1.25f);
      CHECK(f_peak.ring_flash_alpha > 0.70f);

      anim.update(sk, 12.48);  // u ~ 0.36 (mid vertical card-flip)
      anim.apply(sk);
      const auto f_mid = carview::evaluate_popularity_badge_anim(sk, 12.48);
      CHECK(f_mid.text_scale_y < 0.75f);

      // Rasterize mid-celebration popularity badge cleanly.
      {
        carview::DriveHud d;
        d.driving = true;
        d.skills = &sk;
        d.time = 12.48;
        check_hud_screen(d);
      }

      anim.update(sk, 12.75);  // u ~ 0.56 (new rank rolled in)
      anim.apply(sk);
      const auto f_new = carview::evaluate_popularity_badge_anim(sk, 12.75);
      CHECK(f_new.display_rank == 249);

      for (int step = 0; step < 16; ++step) anim.update(sk, 12.80 + double(step) * 0.05);
      anim.apply(sk);
      const auto f_done = carview::evaluate_popularity_badge_anim(sk, 13.55);
      CHECK(std::fabs(f_done.disc_scale - 1.0f) < 0.02f);
      CHECK(f_done.display_rank == 249);
      CHECK(f_done.ring_flash_alpha < 0.02f);

      // Active skill chain / banking pulse glow.
      sk.active = true;
      sk.last_skill_at = 14.0;
      const auto f_pulse = carview::evaluate_popularity_badge_anim(sk, 14.08);
      CHECK(f_pulse.skill_pulse_alpha > 0.35f && f_pulse.disc_scale > 1.01f);
    }

    // PT-40: High-contrast ROAD DISCOVERED banner + zero stray yellow artifact even if H_BARN_FIND_HUD is loaded.
    {
      const auto p1 = carview::split_popup_title_counter("ROAD DISCOVERED  2 / 216");
      CHECK(p1.label == "ROAD DISCOVERED" && p1.counter == "2 / 216");
      const auto p2 = carview::split_popup_title_counter("DISCOUNT SIGN 12/100");
      CHECK(p2.label == "DISCOUNT SIGN" && p2.counter == "12/100");
      const auto p3 = carview::split_popup_title_counter("ROAD DISCOVERED");
      CHECK(p3.label == "ROAD DISCOVERED" && p3.counter.empty());

      // Install a mock H_BARN_FIND_HUD containing a zero-alpha bright yellow rect to ensure it cannot leak.
      static fh1::UiScene mock_barn = [] {
        fh1::UiScene sc;
        auto add_o = [&](fh1::UiObjectKind k, uint32_t id, const char* nm) {
          fh1::UiObject o;
          o.kind = k, o.id = id, o.name = nm;
          sc.objects.push_back(o);
        };
        add_o(fh1::UiObjectKind::kGroup, 1, "DoubleTape");
        add_o(fh1::UiObjectKind::kModel, 2, "Panel");
        add_o(fh1::UiObjectKind::kModel, 3, "Panel2");
        add_o(fh1::UiObjectKind::kText, 4, "TEXT2");
        add_o(fh1::UiObjectKind::kText, 5, "TEXT1");
        add_o(fh1::UiObjectKind::kModel, 6, "StrayYellowTag");
        fh1::UiObject ym;
        ym.kind = fh1::UiObjectKind::kMaterial, ym.id = 7, ym.name = "YellowMat";
        ym.colour = {0.95f, 0.98f, 0.05f, 1.0f};
        sc.objects.push_back(ym);
        auto add_n = [&](int parent, uint32_t oid, float x, float y, float alpha) {
          fh1::UiNode n;
          n.parent = parent, n.object = oid, n.x = x, n.y = y, n.opacity = alpha, n.alpha = alpha;
          n.bounds_min = {-40, -12, 0}, n.bounds_max = {40, 12, 0};
          sc.nodes.push_back(n);
          return int(sc.nodes.size()) - 1;
        };
        const int root = add_n(-1, 1, 640, -630, 1.0f);
        add_n(root, 2, 640, -630, 1.0f);
        add_n(root, 3, 640, -630, 1.0f);
        add_n(root, 4, 540, -620, 1.0f);
        add_n(root, 5, 540, -640, 1.0f);
        add_n(root, 6, 700, -592, 0.0f);
        for (auto& n : sc.nodes)
          for (size_t i = 0; i < sc.objects.size(); ++i)
            if (sc.objects[i].id == n.object) n.object_index = int(i);
        return sc;
      }();
      carview::set_ui_scenes([&](const std::string& nm) -> const fh1::UiScene* {
        return nm == "H_BARN_FIND_HUD" ? &mock_barn : nullptr;
      });

      carview::DriveHud d;
      d.driving = true;
      d.speedo_mode = 2;  // isolate popup banner rasterization
      d.time = 2.0;
      d.popup_title = "ROAD DISCOVERED  2 / 216";
      d.popup_text = "COLORADO ROAD #88";
      d.popup_at = 1.0;
      d.popup_progress = 2.0 / 216.0;
      d.popup_colour = rgba(74, 238, 97);

      List l_pop;
      carview::build_drive_hud(l_pop, d, 1280.0f, 720.0f);
      std::vector<uint8_t> pop_px;
      rasterize(l_pop.items(), pop_px, 1280, 720);

      size_t bright_white = 0;
      size_t magenta_accent = 0;
      size_t stray_yellow = 0;
      for (uint32_t y = 520; y < 680; ++y) {
        for (uint32_t x = 380; x < 900; ++x) {
          const size_t idx = (size_t(y) * 1280u + x) * 4u;
          const uint8_t r = pop_px[idx + 0];
          const uint8_t g = pop_px[idx + 1];
          const uint8_t b = pop_px[idx + 2];
          const uint8_t a = pop_px[idx + 3];
          if (a < 140) continue;
          if (y >= 595 && y <= 665) {
            if (r > 215 && g > 215 && b > 215) ++bright_white;
            if (r > 190 && g < 75 && b > 70) ++magenta_accent;
          }
          if (r > 200 && g > 200 && b < 60) ++stray_yellow;
        }
      }
      CHECK(bright_white > 80);    // crisp high-contrast white ROAD DISCOVERED + 2 / 216 text
      CHECK(magenta_accent > 80);  // bright Horizon magenta/pink accent stripe + counter chip
      CHECK(stray_yellow == 0);    // zero stray bright yellow rectangle above or on banner

      carview::set_ui_scenes({});
    }
  }

  // 5. TitleMenu VIDEO screen (830_GAMMA_ADJUST, Q363): step=5, snap, held repeat cadence, reset, save.
  {
    carview::TitleMenu tm;
    tm.first_profile = true;
    tm.default_subtitles = false;
    CHECK(carview::TitleMenu::snap_brightness(60) == 60);
    CHECK(carview::TitleMenu::snap_brightness(62) == 60);
    CHECK(carview::TitleMenu::snap_brightness(63) == 65);
    CHECK(carview::TitleMenu::snap_brightness(0) == 0);
    CHECK(carview::TitleMenu::snap_brightness(100) == 100);

    tm.confirm();
    tm.update(carview::TitleMenu::kProfileSeconds + 0.01);
    CHECK(tm.phase == carview::TitleMenu::Phase::kVideo);
    CHECK(tm.brightness == 60);
    CHECK(tm.subtitles == false);
    // Single press steps by 5.
    tm.adjust(1);
    CHECK(tm.brightness == 65);
    // Held right repeats after 0.40 s initial delay, then every 0.10 s.
    tm.set_video_hold(1);
    CHECK(tm.step_video_hold(0.35).cue == carview::TitleMenu::Cue::kNone);
    CHECK(tm.brightness == 65);
    CHECK(tm.step_video_hold(0.10).cue == carview::TitleMenu::Cue::kMove);  // 0.45 s -> 1 repeat step -> 70
    CHECK(tm.brightness == 70);
    CHECK(tm.step_video_hold(0.30).cue == carview::TitleMenu::Cue::kMove);  // +3 repeat steps -> 85
    CHECK(tm.brightness == 85);
    CHECK(tm.step_video_hold(0.50).cue == carview::TitleMenu::Cue::kMove);  // capped at 100
    CHECK(tm.brightness == 100);
    tm.set_video_hold(0);
    // Row 1 toggles subtitles, Y resets both to defaults.
    tm.move(1);
    CHECK(tm.video_row == 1);
    tm.adjust(1);
    CHECK(tm.subtitles == true);
    tm.reset_video();
    CHECK(tm.brightness == 60);
    CHECK(tm.subtitles == false);
    // A SAVE moves to menu and clears first_profile.
    const auto saved = tm.confirm();
    CHECK(saved.action == carview::TitleMenu::Action::kSaveVideo);
    CHECK(!tm.first_profile);
    CHECK(tm.phase == carview::TitleMenu::Phase::kMenu);
  }

  if (const char* out = std::getenv("SHIFT_UI_SPECIMEN")) {
    const uint32_t W = 1280, H = 720;
    std::vector<uint8_t> img(size_t(W) * H * 4);
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) {
        uint8_t* p = &img[(size_t(y) * W + x) * 4];
        p[0] = uint8_t(150 + 60 * x / W), p[1] = uint8_t(120 + 40 * y / H), p[2] = 90, p[3] = 255;
      }
    List l;
    l.shadow(40, 40, 700, 330, rgba(0, 0, 0, 140), 14, 18);
    l.rect(40, 40, 700, 330, rgba(18, 20, 26, 215), 14, rgba(10, 11, 15, 235));
    TextStyle t{.size = 34, .weight = 0.1f, .color = rgba(255, 255, 255)};
    l.text(70, 70, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", t);
    t.size = 22;
    l.text(70, 125, "0123456789 .,:-+/%()[]!?<>'_=*#&", t);
    t.size = 16, t.weight = 0.09f, t.color = rgba(200, 205, 215);
    l.text(70, 170, "2009 FERRARI CALIFORNIA   1500 CR   LEVEL 12", t);
    t.size = 64, t.weight = 0.12f, t.slant = 0.18f, t.color = rgba(255, 255, 255);
    l.text(70, 220, "247", t);
    t.size = 40, t.color = rgba(255, 196, 0);
    l.text(300, 240, "3RD", t);
    t = TextStyle{.size = 12, .weight = 0.1f, .tracking = 0.25f, .color = rgba(255, 196, 0)};
    l.text(470, 230, "POSITION", t);
    // A gauge.
    const float cx = 1000, cy = 460, R = 150, a0 = -2.35f, a1 = 2.35f;
    l.circle(cx, cy, R + 24, rgba(10, 12, 16, 200));
    l.arc(cx, cy, R, 10, a0, a1, rgba(255, 255, 255, 50));
    l.arc(cx, cy, R, 10, a0, a0 + 0.7f * (a1 - a0), rgba(255, 255, 255));
    l.arc(cx, cy, R, 10, a0 + 0.85f * (a1 - a0), a1, rgba(230, 40, 40));
    for (int i = 0; i <= 8; ++i) {
      const float a = a0 + (a1 - a0) * float(i) / 8;
      l.line(cx + (R - 22) * std::sin(a), cy - (R - 22) * std::cos(a), cx + (R - 12) * std::sin(a),
             cy - (R - 12) * std::cos(a), 3, rgba(255, 255, 255, 180));
    }
    TextStyle big{.size = 70, .weight = 0.12f, .slant = 0.12f, .color = rgba(255, 255, 255), .align = Align::kCenter};
    l.text(cx, cy - 40, "184", big);
    TextStyle small{.size = 14, .weight = 0.1f, .tracking = 0.3f, .color = rgba(180, 185, 195), .align = Align::kCenter};
    l.text(cx, cy + 45, "KM/H", small);
    l.circle(cx, cy + 105, 26, rgba(255, 196, 0));
    TextStyle gear{.size = 30, .weight = 0.13f, .color = rgba(15, 15, 15), .align = Align::kCenter};
    l.text(cx, cy + 90, "4", gear);
    // A menu.
    l.rect(40, 380, 520, 690, rgba(12, 14, 18, 220), 12);
    TextStyle item{.size = 22, .weight = 0.1f, .color = rgba(230, 232, 238)};
    const char* items[] = {"RESUME", "MAP", "RACE FR22", "CHANGE CAR", "UPGRADES", "SETTINGS"};
    for (int i = 0; i < 6; ++i) {
      const float y = 400 + 46.0f * i;
      if (i == 2) l.rect(52, y - 8, 508, y + 30, rgba(255, 196, 0), 6);
      item.color = i == 2 ? rgba(15, 15, 15) : rgba(230, 232, 238);
      l.text(72, y, items[i], item);
    }
    rasterize(l.items(), img, W, H);
    stbi_write_png(out, int(W), int(H), 4, img.data(), int(W * 4));
    std::printf("ui_tests: specimen %s\n", out);
  }
  std::printf("ui_tests: %d failures\n", failures);
  return failures ? 1 : 0;
}
