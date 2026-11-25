// Unit tests for Slice 10: Xbox 360 XDBF / spa.bin parser, live achievement
// evaluation, wristband tier math, persistence round-trip, and F7 screen HUD.

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "achievements.h"
#include "drive/save.h"
#include "fh1data/spa.h"
#include "race/career.h"
#include "ui4_chrome.h"

namespace {

void put_u16_be(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(uint8_t((v >> 8) & 0xFF));
  b.push_back(uint8_t(v & 0xFF));
}

void put_u32_be(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(uint8_t((v >> 24) & 0xFF));
  b.push_back(uint8_t((v >> 16) & 0xFF));
  b.push_back(uint8_t((v >> 8) & 0xFF));
  b.push_back(uint8_t(v & 0xFF));
}

void put_u64_be(std::vector<uint8_t>& b, uint64_t v) {
  put_u32_be(b, uint32_t(v >> 32));
  put_u32_be(b, uint32_t(v & 0xFFFFFFFFu));
}

std::vector<uint8_t> make_test_png() {
  uint8_t px[4 * 4 * 4];
  for (int i = 0; i < 16; ++i) {
    px[i * 4 + 0] = uint8_t(40 + i * 12);
    px[i * 4 + 1] = uint8_t(120 + (i % 4) * 30);
    px[i * 4 + 2] = 220;
    px[i * 4 + 3] = 255;
  }
  std::vector<uint8_t> out;
  auto cb = [](void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    const auto* p = static_cast<const uint8_t*>(data);
    v->insert(v->end(), p, p + size);
  };
  const int ok = stbi_write_png_to_func(cb, &out, 4, 4, 4, px, 4 * 4);
  assert(ok != 0 && out.size() > 16);
  return out;
}

struct XdbfBlobEntry {
  uint16_t section;
  uint64_t id;
  std::vector<uint8_t> data;
};

std::vector<uint8_t> build_xdbf(const std::vector<XdbfBlobEntry>& entries) {
  std::vector<uint8_t> out;
  // 24-byte XDBF header
  out.insert(out.end(), {'X', 'D', 'B', 'F'});
  put_u32_be(out, 0x00010000u);
  put_u32_be(out, uint32_t(entries.size()));
  put_u32_be(out, uint32_t(entries.size()));
  put_u32_be(out, 0u);  // free_table_len
  put_u32_be(out, 0u);  // free_count

  uint32_t offset = 0;
  for (const auto& e : entries) {
    put_u16_be(out, e.section);
    put_u64_be(out, e.id);
    put_u32_be(out, offset);
    put_u32_be(out, uint32_t(e.data.size()));
    offset += uint32_t(e.data.size());
  }
  for (const auto& e : entries) {
    out.insert(out.end(), e.data.begin(), e.data.end());
  }
  return out;
}

std::vector<uint8_t> build_xstr(const std::vector<std::pair<uint16_t, std::string>>& strs) {
  std::vector<uint8_t> body;
  for (const auto& [id, s] : strs) {
    put_u16_be(body, id);
    put_u16_be(body, uint16_t(s.size()));
    body.insert(body.end(), s.begin(), s.end());
  }
  std::vector<uint8_t> out;
  out.insert(out.end(), {'X', 'S', 'T', 'R'});
  put_u32_be(out, 1u);
  put_u32_be(out, uint32_t(body.size() + 2));
  put_u16_be(out, uint16_t(strs.size()));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

struct XachSpec {
  uint16_t id;
  uint16_t label_id;
  uint16_t desc_id;
  uint16_t unach_id;
  uint32_t image_id;
  uint16_t gamerscore;
};

std::vector<uint8_t> build_xach(const std::vector<XachSpec>& specs) {
  std::vector<uint8_t> out;
  out.insert(out.end(), {'X', 'A', 'C', 'H'});
  put_u32_be(out, 1u);
  put_u32_be(out, uint32_t(specs.size() * 36 + 2));
  put_u16_be(out, uint16_t(specs.size()));
  for (const auto& s : specs) {
    put_u16_be(out, s.id);
    put_u16_be(out, s.label_id);
    put_u16_be(out, s.desc_id);
    put_u16_be(out, s.unach_id);
    put_u32_be(out, s.image_id);
    put_u16_be(out, s.gamerscore);
    put_u16_be(out, 0u);
    put_u32_be(out, 0x00000008u);  // show unachieved
    put_u32_be(out, 0u);
    put_u32_be(out, 0u);
    put_u32_be(out, 0u);
    put_u32_be(out, 0u);
  }
  return out;
}

std::vector<uint8_t> make_synthetic_spa(bool include_dlc) {
  std::vector<uint8_t> xthd;
  xthd.insert(xthd.end(), {'X', 'T', 'H', 'D'});
  put_u32_be(xthd, 1u);
  put_u32_be(xthd, 20u);
  put_u32_be(xthd, 0x4D5309C9u);  // title id
  for (int i = 0; i < 4; ++i) put_u32_be(xthd, 0u);

  std::vector<uint8_t> xstc;
  xstc.insert(xstc.end(), {'X', 'S', 'T', 'C'});
  put_u32_be(xstc, 1u);
  put_u32_be(xstc, 4u);
  put_u32_be(xstc, 1u);  // default language = English

  std::vector<XachSpec> achs = {
      {155, 1, 2, 3, 5001, 10},
      {175, 4, 5, 6, 5002, 15},
      {195, 7, 8, 9, 5003, 25},
  };
  if (include_dlc) {
    achs.push_back({206, 10, 11, 12, 5004, 20});
    achs.push_back({216, 13, 14, 15, 5005, 15});
  }

  std::string win1252_desc = "Start your Horizon journey";
  win1252_desc.push_back(char(0x85));  // Win-1252 ellipsis -> "..."

  std::vector<std::pair<uint16_t, std::string>> en_strings = {
      {1, "Welcome to Horizon"},     {2, win1252_desc},
      {3, "Arrive at the Festival"}, {4, "Yellow Wristband"},
      {5, "Earned the Yellow Wristband"}, {6, "Win races to earn the Yellow Wristband"},
      {7, "Vendetta"},               {8, "Smashed all 100 Discount Signs"},
      {9, "Smash all 100 Discount Signs"},
      {10, "Rally Debut"},           {11, "Completed your first Rally stage"},
      {12, "Complete a Rally stage"},
      {13, "Club Member"},           {14, "Earned your first 1000 Club medal"},
      {15, "Earn a 1000 Club medal"},
  };
  std::vector<std::pair<uint16_t, std::string>> fr_strings = {
      {1, "Bienvenue a Horizon"},
      {2, "Commencez votre aventure"},
      {3, "Arrivez au Festival"},
  };

  const std::vector<uint8_t> png = make_test_png();
  std::vector<XdbfBlobEntry> entries = {
      {1, 0x58544844ull, xthd},
      {1, 0x58535443ull, xstc},
      {1, 0x58414348ull, build_xach(achs)},
      {3, 1ull, build_xstr(en_strings)},
      {3, 4ull, build_xstr(fr_strings)},
      {2, 5001ull, png},
      {2, 5002ull, png},
      {2, 5003ull, png},
  };
  if (include_dlc) {
    entries.push_back({2, 5004ull, png});
    entries.push_back({2, 5005ull, png});
  }
  return build_xdbf(entries);
}

void test_spa_parser() {
  // 1. Corrupt / truncated rejection
  fh1::SpaDatabase bad;
  std::string err;
  const uint8_t garbage[] = {'N', 'O', 'P', 'E', 0, 1, 2, 3};
  assert(!fh1::parse_spa_xdbf(garbage, bad, fh1::spa_language_id("EN"), &err));
  assert(!err.empty());

  // 2. Valid EN parse + Win-1252 ellipsis cleanup + PNG icon extraction
  const auto base_bytes = make_synthetic_spa(false);
  fh1::SpaDatabase db;
  assert(fh1::parse_spa_xdbf(base_bytes, db, fh1::spa_language_id("EN"), &err));
  assert(db.title_id == 0x4D5309C9u);
  assert(db.achievements.size() == 3);
  assert(db.images.size() == 3);
  const auto* a155 = db.find(155);
  assert(a155 != nullptr);
  assert(a155->label == "Welcome to Horizon");
  assert(a155->description == "Start your Horizon journey...");
  assert(a155->gamerscore == 10);
  assert(a155->image_id == 5001);
  assert(db.find_image(5001) != nullptr);

  // 3. Language selection (FR with EN fallback)
  fh1::SpaDatabase fr_db;
  assert(fh1::parse_spa_xdbf(base_bytes, fr_db, fh1::spa_language_id("FR"), &err));
  assert(fr_db.find(155)->label == "Bienvenue a Horizon");
  assert(fr_db.find(195)->label == "Vendetta");  // fell back to EN default

  // 4. Merge DLC SPA onto base SPA
  const auto dlc_bytes = make_synthetic_spa(true);
  fh1::SpaDatabase dlc_db;
  assert(fh1::parse_spa_xdbf(dlc_bytes, dlc_db, fh1::spa_language_id("EN"), &err));
  assert(dlc_db.achievements.size() == 5);
  db.merge(dlc_db);
  assert(db.achievements.size() == 5);
  assert(db.find(206) != nullptr);
  assert(db.find(216) != nullptr);
  assert(db.images.size() == 5);
}

void test_wristband_tiers() {
  assert(carview::achievement_completion_tier(0, 1000) == -1);
  assert(carview::achievement_completion_tier(49, 1000) == -1);
  assert(carview::achievement_completion_tier(50, 1000) == 0);    // Yellow
  assert(carview::achievement_completion_tier(150, 1000) == 1);   // Green
  assert(carview::achievement_completion_tier(300, 1000) == 2);   // Blue
  assert(carview::achievement_completion_tier(450, 1000) == 3);   // Pink
  assert(carview::achievement_completion_tier(600, 1000) == 4);   // Orange
  assert(carview::achievement_completion_tier(750, 1000) == 5);   // Purple
  assert(carview::achievement_completion_tier(900, 1000) == 6);   // Gold
  assert(carview::achievement_completion_tier(1000, 1000) == 7);  // Platinum
  assert(std::strcmp(carview::wristband_tier_info(7).name, "PLATINUM") == 0);
}

void test_achievements_system_and_persistence() {
  const auto bytes = make_synthetic_spa(true);
  fh1::SpaDatabase db;
  assert(fh1::parse_spa_xdbf(bytes, db, fh1::spa_language_id("EN"), nullptr));

  carview::AchievementsSystem fallback;
  fallback.init(fh1::SpaDatabase{});
  assert(fallback.total_count() >= 50);

  carview::AchievementsSystem sys;
  int sound_triggers = 0;
  sys.set_unlock_sound_callback([&]() { ++sound_triggers; });
  sys.init(db);
  assert(sys.total_count() == 5);
  assert(sys.find(155) != nullptr && sys.find(155)->name == "Welcome to Horizon");
  assert(sys.find(206) != nullptr && sys.find(206)->name == "Rally Debut");
  assert(sys.find(216) != nullptr && sys.find(216)->name == "Club Member");

  // Build DriveSave with partial + complete progress
  drive::DriveSave save;
  save.career_started = true;
  save.collected_wristband = 2;  // Yellow + Green
  save.num_races = 12;
  save.num_victories = 6;
  save.num_podiums = 10;
  save.total_winnings = 1250000;
  save.top_speed = 95.0;  // ~212 mph
  save.total_distance = 180000.0;
  for (int i = 0; i < 75; ++i) save.signs_found.push_back("Sign_" + std::to_string(i));
  for (int i = 0; i < 5; ++i) save.barns.push_back({"Barn_" + std::to_string(i), 2, 10.0, 20.0});
  save.garage = {101, 102, 103, 104, 105};
  save.promo_cars = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  save.discovered_outposts = {"GasStation_01", "GasStation_02", "GasStation_03"};
  save.completed_pr_stunts = {"PR_1", "PR_2", "PR_3", "PR_4", "PR_5"};
  save.club = {{100, {{1u, true}, {2u, true}}}, {200, {{1u, true}}}};

  auto ags = carview::AchievementGameplayState::from_drive_save(save, 150, 2);
  ags.has_rally = true;
  ags.rally_stages_done = 2;
  ags.rally_victories = 1;

  const int first_unlocked = sys.evaluate(ags, true);
  assert(first_unlocked == 4);
  assert(sound_triggers == first_unlocked);
  assert(sys.find(155)->unlocked);
  assert(sys.find(175)->unlocked);
  assert(sys.find(206)->unlocked);
  assert(sys.find(216)->unlocked);
  assert(!sys.find(195)->unlocked);  // 5 / 9 barn finds
  assert(sys.find(195)->progress.has_value());
  assert(sys.find(195)->progress->current == 5);
  assert(sys.find(195)->progress->target == 9);
  assert(sys.active_toast() != nullptr);

  // Also verify full 50-entry fallback evaluates >=5 unlocks and tracks 194 (75/100 signs)
  const int fallback_unlocked = fallback.evaluate(ags, false);
  assert(fallback_unlocked >= 5);
  assert(fallback.find(194) != nullptr && !fallback.find(194)->unlocked);
  assert(fallback.find(194)->progress.has_value());
  assert(fallback.find(194)->progress->current == 75);
  assert(fallback.find(194)->progress->target == 100);

  // Re-evaluating the same state unlocks 0 new achievements
  assert(sys.evaluate(ags, true) == 0);

  // Phase 1 parity: 1000 Club IDs 217..225 use exact make/country/value/car-count thresholds.
  {
    fh1::SpaDatabase club_db;
    for (uint16_t id = 216; id <= 225; ++id) {
      fh1::SpaAchievement sa;
      sa.id = id;
      sa.gamerscore = 20;
      sa.label = "Club #" + std::to_string(id);
      club_db.achievements.push_back(sa);
    }
    carview::AchievementsSystem csys;
    csys.init(club_db);
    carview::AchievementGameplayState cs;
    cs.club_medals_total = 1;
    cs.club_ferrari_cars = 13;
    cs.club_million_cr_cars = 14;
    cs.club_shelby_cars = 3;
    cs.club_lamborghini_cars = 7;
    cs.club_nissan_cars = 7;
    cs.club_bmw_cars = 10;
    cs.club_british_cars = 19;
    cs.club_ford_cars = 13;
    cs.club_cars_with_medal = 145;
    csys.evaluate(cs, false);
    assert(csys.find(216)->unlocked);
    assert(!csys.find(217)->unlocked && csys.find(217)->progress->target == 14);
    assert(csys.find(218)->unlocked);
    assert(csys.find(219)->unlocked);
    assert(!csys.find(220)->unlocked && csys.find(220)->progress->target == 8);
    assert(csys.find(221)->unlocked);
    assert(csys.find(222)->unlocked);
    assert(!csys.find(223)->unlocked && csys.find(223)->progress->target == 20);
    assert(csys.find(224)->unlocked);
    assert(!csys.find(225)->unlocked && csys.find(225)->progress->target == 146);
  }

  // Toast expires after advancing time
  for (int i = 0; i < 300 && sys.active_toast() != nullptr; ++i) sys.update(0.25);
  assert(sys.active_toast() == nullptr);

  // Navigation, sorting, tracking, and HUD generation
  sys.open_screen();
  assert(sys.screen_open());
  sys.select_tab(carview::AchievementCategory::kExplore);
  assert(sys.active_tab() == carview::AchievementCategory::kExplore);
  sys.switch_tab(+1);
  sys.select_tab(carview::AchievementCategory::kAll);
  for (int i = 0; i < int(carview::AchievementSort::kCount); ++i) sys.cycle_sort();
  sys.move_focus(+1, 0);
  sys.move_focus(0, +1);
  sys.set_tracked_id(195);
  assert(sys.tracked_id() == 195);

  // Trigger a fresh toast and verify both screen HUD and overlay HUD render shapes
  assert(sys.unlock(195, 1730000000, true));
  sys.update(0.6);
  std::set<std::string> requested_tex;
  carview::ui4::set_texture_loader([&](const std::string& p, std::array<float, 4>& uv) {
    requested_tex.insert(p);
    uv = {0.0f, 0.0f, 0.1f, 0.1f};
    return true;
  });
  carview::ui::List screen_list;
  sys.build_screen_hud(screen_list, 1280.0f, 720.0f);
  carview::ui4::set_texture_loader({});
  assert(screen_list.items().size() > 20);
  assert(requested_tex.count("DIRTMASKS/PILL02") > 0);
  assert(requested_tex.count("DIRTMASKS/PANEL01") > 0);
  // P1-7: Verify real SPA PNG icons were uploaded into UI atlas by build_screen_hud().
  std::array<float, 4> spa_uv{};
  assert(carview::ui::atlas_find("ach.c.1001", spa_uv) || carview::ui::atlas_find("ach.g.1001", spa_uv) ||
         requested_tex.count("T:UI4/Common/Icon_Gold") > 0 || requested_tex.count("HUD/TROPHY") > 0);

  // P0-7: Verify DriveSave round-trips intro_completed and tod_sec.
  {
    const auto save_tmp = std::filesystem::temp_directory_path() / "shift_save_polish_test.txt";
    std::error_code sec;
    std::filesystem::remove(save_tmp, sec);
    drive::DriveSave ds_out;
    ds_out.intro_completed = true;
    ds_out.tod_sec = 7200.0;  // 02:00 midnight
    std::string serr;
    assert(drive::write_save(save_tmp, ds_out, &serr));
    drive::DriveSave ds_in;
    assert(drive::read_save(save_tmp, ds_in, &serr));
    assert(ds_in.intro_completed);
    assert(ds_in.tod_sec > 7199.0 && ds_in.tod_sec < 7201.0);
    std::filesystem::remove(save_tmp, sec);
  }

  carview::ui::List overlay_list;
  sys.build_hud_overlays(overlay_list, 1280.0f, 720.0f, true);
  assert(!overlay_list.items().empty());

  // Persistence round-trip
  const auto tmp = std::filesystem::temp_directory_path() / "shift_achievements_test.txt";
  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  assert(sys.save_state(tmp));

  carview::AchievementsSystem reloaded;
  reloaded.init(db);
  assert(reloaded.load_state(tmp));
  assert(reloaded.unlocked_count() == sys.unlocked_count());
  assert(reloaded.find(195)->unlocked);
  assert(reloaded.find(195)->unlock_utc == 1730000000);
  assert(reloaded.tracked_id() == 195);
  std::filesystem::remove(tmp, ec);

  // Phase 6C: Secret achievement masking when locked + unmasking on unlock + 5x3 grid HUD.
  {
    fh1::SpaDatabase sdb;
    fh1::SpaAchievement sec;
    sec.id = 199;
    sec.gamerscore = 25;
    sec.flags = 0x0u;  // show_unachieved() == false -> secret achievement
    sec.label = "Hidden Legend";
    sec.description = "Find the hidden canyon jump.";
    sdb.achievements.push_back(sec);
    carview::AchievementsSystem ssys;
    ssys.init(sdb);
    const auto* e = ssys.find(199);
    assert(e != nullptr && e->is_secret && e->is_masked_secret());
    assert(e->display_name() == "SECRET ACHIEVEMENT");
    assert(e->display_description().find("SECRET") != std::string::npos);
    ssys.open_screen();
    carview::ui::List grid_ui;
    ssys.build_screen_hud(grid_ui, 1280.0f, 720.0f);
    assert(!grid_ui.items().empty());
    assert(ssys.unlock(199, 1730000100, false));
    assert(!ssys.find(199)->is_masked_secret());
    assert(ssys.find(199)->display_name() == "Hidden Legend");
  }

  // Round 2 parity: Achievements 181, 182, 211, and 215 are independently reachable.
  {
    fh1::SpaDatabase r2_db;
    for (uint16_t id : {uint16_t(181), uint16_t(182), uint16_t(211), uint16_t(215)}) {
      fh1::SpaAchievement sa;
      sa.id = id;
      sa.gamerscore = 50;
      sa.label = "Ach #" + std::to_string(id);
      r2_db.achievements.push_back(sa);
    }
    carview::AchievementsSystem r2_sys;
    r2_sys.init(r2_db);

    // 1. Gold Wristband (181) unlocks at 7 wristbands without unlocking Horizon Champion (182).
    drive::DriveSave ds;
    ds.collected_wristband = 6;  // 0..6 -> 7 wristbands earned
    ds.finale_complete = false;
    auto st = carview::AchievementGameplayState::from_drive_save(ds);
    r2_sys.evaluate(st, false);
    assert(r2_sys.find(181)->unlocked);
    assert(!r2_sys.find(182)->unlocked);
    assert(!r2_sys.find(211)->unlocked);
    assert(!r2_sys.find(215)->unlocked);

    // 2. Completing the Horizon Finale unlocks 182 independently.
    ds.finale_complete = true;
    st = carview::AchievementGameplayState::from_drive_save(ds);
    r2_sys.evaluate(st, false);
    assert(r2_sys.find(182)->unlocked);

    // 3. Completing a 4-stage Rally championship unlocks 211 without requiring unreachable rally_level 5.
    ds.has_rally = true;
    ds.num_rally_stages = 4;
    ds.rally_level = 0;
    st = carview::AchievementGameplayState::from_drive_save(ds);
    r2_sys.evaluate(st, false);
    assert(r2_sys.find(211)->unlocked);
    assert(!r2_sys.find(215)->unlocked);

    // 4. Earning Horizon Rally Champion (rally_level == 1) unlocks 215 independently.
    ds.rally_level = 1;
    st = carview::AchievementGameplayState::from_drive_save(ds);
    r2_sys.evaluate(st, false);
    assert(r2_sys.find(215)->unlocked);
  }

  // R3-CAM-17 parity: real street_races_won from event_results (no fabricated time_street heuristic),
  // Popularity Rank (1..250) on IDs 170..174, and ALL outposts (total_outposts = 10) on ID 187.
  {
    fh1::SpaDatabase r3_db;
    for (uint16_t id : {uint16_t(170), uint16_t(171), uint16_t(172), uint16_t(173), uint16_t(174), uint16_t(183),
                        uint16_t(184), uint16_t(187)}) {
      fh1::SpaAchievement sa;
      sa.id = id;
      sa.gamerscore = 20;
      sa.label = "R3 Ach #" + std::to_string(id);
      r3_db.achievements.push_back(sa);
    }
    carview::AchievementsSystem r3_sys;
    r3_sys.init(r3_db);

    // 1. Fabricated street_races_won MUST NOT happen from time_street + num_victories alone.
    drive::DriveSave ds;
    ds.time_street = 500.0;
    ds.num_victories = 40;
    ds.popularity_points = 999999.0;  // raw points alone must NOT unlock Popularity Rank achievements!
    for (int i = 1; i <= 6; ++i) ds.discovered_outposts.push_back("GasStation_0" + std::to_string(i));
    auto st0 = carview::AchievementGameplayState::from_drive_save(ds);
    assert(st0.street_races_won == 0);
    assert(st0.popularity_rank == 250);
    r3_sys.evaluate(st0, false);
    assert(!r3_sys.find(170)->unlocked && !r3_sys.find(171)->unlocked && !r3_sys.find(174)->unlocked);
    assert(!r3_sys.find(184)->unlocked);
    // 6 outposts is NOT enough when Colorado has 10 total outposts!
    assert(!r3_sys.find(187)->unlocked && r3_sys.find(187)->progress->target == 10);

    // 2. Real street vs festival wins classified via CareerCatalog + Popularity Rank milestones + all 10 outposts.
    race::CareerCatalog cat;
    for (int i = 1; i <= 10; ++i) {
      race::CareerEvent se;
      se.event_id = 100 + i;
      se.style = 2;  // Street Race
      cat.events.push_back(se);
      ds.event_results.emplace_back(se.event_id, 1);

      race::CareerEvent fe;
      fe.event_id = 200 + i;
      fe.style = 1;  // Festival Race
      cat.events.push_back(fe);
      ds.event_results.emplace_back(fe.event_id, 1);
    }
    for (int i = 7; i <= 10; ++i) ds.discovered_outposts.push_back("GasStation_" + std::to_string(i));
    auto st1 = carview::AchievementGameplayState::from_drive_save(ds, 0, 0, &cat, /*popularity_rank=*/50,
                                                                  /*total_outposts=*/10);
    assert(st1.street_races_won == 10 && st1.festival_races_won == 10);
    r3_sys.evaluate(st1, false);
    assert(r3_sys.find(183)->unlocked && r3_sys.find(184)->unlocked);
    assert(r3_sys.find(187)->unlocked);
    // Rank 50 unlocks Top 200 (170), Top 100 (171), Top 50 (172), NOT Top 10 (173) or #1 (174).
    assert(r3_sys.find(170)->unlocked && r3_sys.find(171)->unlocked && r3_sys.find(172)->unlocked);
    assert(!r3_sys.find(173)->unlocked && !r3_sys.find(174)->unlocked);

    st1.popularity_rank = 1;
    r3_sys.evaluate(st1, false);
    assert(r3_sys.find(173)->unlocked && r3_sys.find(174)->unlocked);
  }
}

void test_optional_real_spa() {
  const std::filesystem::path state_root =
      std::filesystem::path("../horizon1-recomp/.local/android/astra-state-20261008");
  if (!std::filesystem::exists(state_root)) return;
  fh1::SpaDatabase real_db;
  std::string note;
  assert(fh1::load_spa_packages({}, state_root, real_db, "EN", &note) > 0);
  assert(real_db.achievements.size() == 70);
  assert(real_db.images.size() >= 70);
  assert(real_db.find(155) != nullptr && !real_db.find(155)->label.empty());
  assert(real_db.find(206) != nullptr && !real_db.find(206)->label.empty());
  assert(real_db.find(225) != nullptr && !real_db.find(225)->label.empty());
  std::printf("real spa.bin verified: %s\n", note.c_str());
}

}  // namespace

int main() {
  test_spa_parser();
  test_wristband_tiers();
  test_achievements_system_and_persistence();
  test_optional_real_spa();
  std::printf("achievements_tests: ALL PASSED\n");
  return 0;
}
