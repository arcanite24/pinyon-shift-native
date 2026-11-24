// The UI layer's CPU rasterizer: text measures and draws inside its box,
// and SHIFT_UI_SPECIMEN=<file.png> writes a specimen of the font and the
// shapes to look at.
#include <cmath>
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
#include "hud.h"
#include "hud_scene.h"
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

  // Free roam items & wrap-around.
  pm.show(false);
  const auto fr_items = pm.items();
  CHECK(fr_items.size() == 16);
  CHECK(fr_items.front() == carview::PauseMenu::kResume);
  CHECK(fr_items.back() == carview::PauseMenu::kQuitToTitle);
  {
    bool found_club = false;
    bool found_car_club = false;
    bool found_achievements = false;
    bool found_rally = false;
    for (size_t i = 0; i + 1 < fr_items.size(); ++i) {
      if (fr_items[i] == carview::PauseMenu::kCareer && fr_items[i + 1] == carview::PauseMenu::kClub) found_club = true;
      if (fr_items[i] == carview::PauseMenu::kClub && fr_items[i + 1] == carview::PauseMenu::kCarClub)
        found_car_club = true;
      if (fr_items[i] == carview::PauseMenu::kCarClub && fr_items[i + 1] == carview::PauseMenu::kAchievements)
        found_achievements = true;
      if (fr_items[i] == carview::PauseMenu::kRally) found_rally = true;
    }
    CHECK(found_club);
    CHECK(found_car_club);
    CHECK(found_achievements);
    CHECK(found_rally);
    CHECK(pm.label(carview::PauseMenu::kClub) == "1000 CLUB");
    CHECK(pm.label(carview::PauseMenu::kCarClub) == "CAR CLUB");
    CHECK(pm.label(carview::PauseMenu::kRally) == "HORIZON RALLY");
    CHECK(pm.label(carview::PauseMenu::kAchievements) == "ACHIEVEMENTS");
    CHECK(pm.label(carview::PauseMenu::kPhotoMode) == "PHOTO MODE");
    CHECK(pm.label(carview::PauseMenu::kReplay) == "REPLAYS");
  }
  CHECK(pm.selected_action() == carview::PauseMenu::kResume);
  pm.move(-1);
  CHECK(pm.selected_action() == carview::PauseMenu::kQuitToTitle);
  pm.move(+1);
  CHECK(pm.selected_action() == carview::PauseMenu::kResume);
  CHECK(pm.label(carview::PauseMenu::kEvents) == "COLLECT GREEN WRISTBAND!");
  CHECK(pm.label(carview::PauseMenu::kDifficulty) == "< DIFFICULTY: HARD (+30%) >");

  // In-race items & HudState fill.
  pm.show(true);
  const auto race_items = pm.items();
  CHECK(race_items.size() == 8);
  CHECK(race_items[1] == carview::PauseMenu::kRestartRace);
  CHECK(race_items.back() == carview::PauseMenu::kQuitRace);
  pm.move(4);
  carview::HudState st;
  pm.fill(st);
  CHECK(st.menu_title == "paused");
  CHECK(st.menu.size() == 8 && st.menu_selected == 4);
  CHECK(st.menu[4] == "MAP");
}

void test_hud_overlays() {
  // TitleScreenHud confirm_new_career
  {
    carview::TitleScreenHud ts;
    ts.active = true, ts.menu_phase = true, ts.has_save = true;
    ts.confirm_new_career = true, ts.confirm_selected = 0;
    ts.menu = {"CONTINUE CAREER", "NEW CAREER", "FREE ROAM", "SETTINGS", "QUIT"};
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

  {
    List l;
    carview::DriveHud d;
    d.abs = false, d.tcs = false;
    carview::build_drive_hud(l, d, 1280, 720);
    CHECK(find_colour(l, 0.1f) == off_col);
    CHECK(find_colour(l, 0.3f) == off_col);
  }
  {
    List l;
    carview::DriveHud d;
    d.abs = true, d.tcs = false;
    carview::build_drive_hud(l, d, 1280, 720);
    CHECK(find_colour(l, 0.1f) == on_col);
    CHECK(find_colour(l, 0.3f) == off_col);
  }
  {
    List l;
    carview::DriveHud d;
    d.abs = false, d.tcs = true;
    carview::build_drive_hud(l, d, 1280, 720);
    CHECK(find_colour(l, 0.1f) == off_col);
    CHECK(find_colour(l, 0.3f) == on_col);
  }
}

}  // namespace

int main() {
  // Every letter and digit has strokes.
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
    ts.active = true, ts.menu_phase = false;
    carview::DriveHud d;
    d.title_screen = &ts, d.time = 1.0;
    check_screen(d);
    ts.menu_phase = true, ts.has_save = true;
    ts.menu = {"CONTINUE CAREER", "NEW CAREER", "FREE ROAM", "SETTINGS", "QUIT"};
    ts.save_band_name = "YELLOW WRISTBAND", ts.save_car_name = "1995 Volkswagen Corrado VR6";
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

  // 4. Phase R2-6 checks: BGF property targets + UiScene::geometry_of non-quad mesh + CONTROLLER/* glyph routing
  //    + per-page Post-Race tally timing + BAR/PSI boost & lap split delta formatting.
  {
    CHECK(fh1::classify_bgf_property(fh1::bgf_attr::kPosX) == fh1::UiBgfPropTarget::kPosX);
    CHECK(fh1::classify_bgf_property(fh1::bgf_attr::kScaleY) == fh1::UiBgfPropTarget::kScaleY);
    CHECK(fh1::classify_bgf_property(fh1::bgf_attr::kRotation) == fh1::UiBgfPropTarget::kRotation);
    CHECK(fh1::classify_bgf_property(fh1::bgf_attr::kAlpha) == fh1::UiBgfPropTarget::kAlpha);
    CHECK(fh1::classify_bgf_property(0xDEADBEEFu, 0u) == fh1::UiBgfPropTarget::kUnknown);

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
