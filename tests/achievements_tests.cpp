// Unit tests for Slice 10: Xbox 360 XDBF / spa.bin parser, live achievement
// evaluation, wristband tier math, persistence round-trip, and F7 screen HUD;
// plus the per-id unlock rules against the SPA's unlock text (PROG-16, V4-19).
// With --corpus (SHIFT_STATE_ROOT, or SHIFT_SPA_ROOT naming a state folder),
// the title's real 70-entry table: unset exits 77, set but missing fails.

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <cstdio>
#include <cstdlib>
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

int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

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
  CHECK(ok != 0 && out.size() > 16);
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
  CHECK(!fh1::parse_spa_xdbf(garbage, bad, fh1::spa_language_id("EN"), &err));
  CHECK(!err.empty());

  // 2. Valid EN parse + Win-1252 ellipsis cleanup + PNG icon extraction
  const auto base_bytes = make_synthetic_spa(false);
  fh1::SpaDatabase db;
  CHECK(fh1::parse_spa_xdbf(base_bytes, db, fh1::spa_language_id("EN"), &err));
  CHECK(db.title_id == 0x4D5309C9u);
  CHECK(db.achievements.size() == 3);
  CHECK(db.images.size() == 3);
  const auto* a155 = db.find(155);
  CHECK(a155 != nullptr);
  CHECK(a155->label == "Welcome to Horizon");
  CHECK(a155->description == "Start your Horizon journey...");
  CHECK(a155->gamerscore == 10);
  CHECK(a155->image_id == 5001);
  CHECK(db.find_image(5001) != nullptr);

  // 3. Language selection (FR with EN fallback)
  fh1::SpaDatabase fr_db;
  CHECK(fh1::parse_spa_xdbf(base_bytes, fr_db, fh1::spa_language_id("FR"), &err));
  CHECK(fr_db.find(155)->label == "Bienvenue a Horizon");
  CHECK(fr_db.find(195)->label == "Vendetta");  // fell back to EN default

  // 4. Merge DLC SPA onto base SPA
  const auto dlc_bytes = make_synthetic_spa(true);
  fh1::SpaDatabase dlc_db;
  CHECK(fh1::parse_spa_xdbf(dlc_bytes, dlc_db, fh1::spa_language_id("EN"), &err));
  CHECK(dlc_db.achievements.size() == 5);
  db.merge(dlc_db);
  CHECK(db.achievements.size() == 5);
  CHECK(db.find(206) != nullptr);
  CHECK(db.find(216) != nullptr);
  CHECK(db.images.size() == 5);
}

void test_wristband_tiers() {
  CHECK(carview::achievement_completion_tier(0, 1000) == -1);
  CHECK(carview::achievement_completion_tier(49, 1000) == -1);
  CHECK(carview::achievement_completion_tier(50, 1000) == 0);    // Yellow
  CHECK(carview::achievement_completion_tier(150, 1000) == 1);   // Green
  CHECK(carview::achievement_completion_tier(300, 1000) == 2);   // Blue
  CHECK(carview::achievement_completion_tier(450, 1000) == 3);   // Pink
  CHECK(carview::achievement_completion_tier(600, 1000) == 4);   // Orange
  CHECK(carview::achievement_completion_tier(750, 1000) == 5);   // Purple
  CHECK(carview::achievement_completion_tier(900, 1000) == 6);   // Gold
  CHECK(carview::achievement_completion_tier(1000, 1000) == 7);  // Platinum
  CHECK(std::strcmp(carview::wristband_tier_info(7).name, "PLATINUM") == 0);
}

void test_achievements_system_and_persistence() {
  const auto bytes = make_synthetic_spa(true);
  fh1::SpaDatabase db;
  CHECK(fh1::parse_spa_xdbf(bytes, db, fh1::spa_language_id("EN"), nullptr));

  carview::AchievementsSystem fallback;
  fallback.init(fh1::SpaDatabase{});
  CHECK(fallback.total_count() == 50);  // the base set; the DLC sets need their packages
  CHECK(fallback.total_gamerscore() == 1000);
  CHECK(fallback.find(163) == nullptr);
  CHECK(fallback.find(172) != nullptr && fallback.find(172)->name == "Almost Famous");
  CHECK(fallback.find(182) != nullptr && fallback.find(182)->name == "Darius Who?" &&
        fallback.find(182)->gamerscore == 100);
  for (uint16_t id = 155; id <= 205; ++id) {
    const auto* e = fallback.find(id);
    if (!e) continue;
    CHECK(e->name.find("ACHIEVEMENT") == std::string::npos);
  }

  carview::AchievementsSystem sys;
  int sound_triggers = 0;
  sys.set_unlock_sound_callback([&]() { ++sound_triggers; });
  sys.init(db);
  CHECK(sys.total_count() == 5);
  CHECK(sys.find(155) != nullptr && sys.find(155)->name == "Welcome to Horizon");
  CHECK(sys.find(206) != nullptr && sys.find(206)->name == "Rally Debut");
  CHECK(sys.find(216) != nullptr && sys.find(216)->name == "Club Member");

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
  CHECK(first_unlocked == 4);
  CHECK(sound_triggers == first_unlocked);
  CHECK(sys.find(155)->unlocked);
  CHECK(sys.find(175)->unlocked);
  CHECK(sys.find(206)->unlocked);
  CHECK(sys.find(216)->unlocked);
  CHECK(!sys.find(195)->unlocked);  // Road Trip: 150 / 216 roads
  CHECK(sys.find(195)->progress.has_value());
  CHECK(sys.find(195)->progress->current == 150);
  CHECK(sys.find(195)->progress->target == 216);
  CHECK(sys.active_toast() != nullptr);

  // Also verify full 50-entry fallback evaluates >=5 unlocks and tracks 194 (75/100 signs)
  const int fallback_unlocked = fallback.evaluate(ags, false);
  CHECK(fallback_unlocked >= 5);
  CHECK(fallback.find(194) != nullptr && !fallback.find(194)->unlocked);
  CHECK(fallback.find(194)->progress.has_value());
  CHECK(fallback.find(194)->progress->current == 75);
  CHECK(fallback.find(194)->progress->target == 100);

  // Re-evaluating the same state unlocks 0 new achievements
  CHECK(sys.evaluate(ags, true) == 0);

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
    CHECK(csys.find(216)->unlocked);
    CHECK(!csys.find(217)->unlocked && csys.find(217)->progress->target == 14);
    CHECK(csys.find(218)->unlocked);
    CHECK(csys.find(219)->unlocked);
    CHECK(!csys.find(220)->unlocked && csys.find(220)->progress->target == 8);
    CHECK(csys.find(221)->unlocked);
    CHECK(csys.find(222)->unlocked);
    CHECK(!csys.find(223)->unlocked && csys.find(223)->progress->target == 20);
    CHECK(csys.find(224)->unlocked);
    CHECK(!csys.find(225)->unlocked && csys.find(225)->progress->target == 146);
  }

  // Toast expires after advancing time
  for (int i = 0; i < 300 && sys.active_toast() != nullptr; ++i) sys.update(0.25);
  CHECK(sys.active_toast() == nullptr);

  // Navigation, sorting, tracking, and HUD generation
  sys.open_screen();
  CHECK(sys.screen_open());
  sys.select_tab(carview::AchievementCategory::kExplore);
  CHECK(sys.active_tab() == carview::AchievementCategory::kExplore);
  sys.switch_tab(+1);
  sys.select_tab(carview::AchievementCategory::kAll);
  for (int i = 0; i < int(carview::AchievementSort::kCount); ++i) sys.cycle_sort();
  sys.move_focus(+1, 0);
  sys.move_focus(0, +1);
  sys.set_tracked_id(195);
  CHECK(sys.tracked_id() == 195);

  // Trigger a fresh toast and verify both screen HUD and overlay HUD render shapes
  CHECK(sys.unlock(195, 1730000000, true));
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
  CHECK(screen_list.items().size() > 20);
  CHECK(requested_tex.count("DIRTMASKS/PILL02") > 0);
  CHECK(requested_tex.count("DIRTMASKS/PANEL01") > 0);
  // P1-7: Verify real SPA PNG icons were uploaded into UI atlas by build_screen_hud().
  std::array<float, 4> spa_uv{};
  CHECK(carview::ui::atlas_find("ach.c.5001", spa_uv) || carview::ui::atlas_find("ach.g.5001", spa_uv) ||
         requested_tex.count("T:UI4/Common/Icon_Gold") > 0 || requested_tex.count("HUD/TROPHY") > 0);

  // P0-7: Verify DriveSave round-trips intro_completed and tod_sec.
  {
    const auto save_tmp = std::filesystem::temp_directory_path() / "shift_save_polish_test.txt";
    std::error_code sec;
    std::filesystem::remove(save_tmp, sec);
    drive::DriveSave ds_out;
    ds_out.car = "VW_CorradoVR6_95";  // a save without a car is rejected
    ds_out.intro_completed = true;
    ds_out.tod_sec = 7200.0;  // 02:00 midnight
    std::string serr;
    CHECK(drive::write_save(save_tmp, ds_out, &serr));
    drive::DriveSave ds_in;
    CHECK(drive::read_save(save_tmp, ds_in, &serr));
    CHECK(ds_in.intro_completed);
    CHECK(ds_in.tod_sec > 7199.0 && ds_in.tod_sec < 7201.0);
    std::filesystem::remove(save_tmp, sec);
  }

  carview::ui::List overlay_list;
  sys.build_hud_overlays(overlay_list, 1280.0f, 720.0f, true);
  CHECK(!overlay_list.items().empty());

  // Persistence round-trip
  const auto tmp = std::filesystem::temp_directory_path() / "shift_achievements_test.txt";
  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  CHECK(sys.save_state(tmp));

  carview::AchievementsSystem reloaded;
  reloaded.init(db);
  CHECK(reloaded.load_state(tmp));
  CHECK(reloaded.unlocked_count() == sys.unlocked_count());
  CHECK(reloaded.find(195)->unlocked);
  CHECK(reloaded.find(195)->unlock_utc == 1730000000);
  CHECK(reloaded.tracked_id() == 0);  // unlocking the tracked achievement clears tracking
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
    CHECK(e != nullptr && e->is_secret && e->is_masked_secret());
    CHECK(e->display_name() == "SECRET ACHIEVEMENT");
    CHECK(e->display_description().find("SECRET") != std::string::npos);
    ssys.open_screen();
    carview::ui::List grid_ui;
    ssys.build_screen_hud(grid_ui, 1280.0f, 720.0f);
    CHECK(!grid_ui.items().empty());
    CHECK(ssys.unlock(199, 1730000100, false));
    CHECK(!ssys.find(199)->is_masked_secret());
    CHECK(ssys.find(199)->display_name() == "Hidden Legend");
  }
}

// An AchievementsSystem holding every FH1 id with its fallback name, so each
// rule can be checked in isolation.
carview::AchievementsSystem make_full_system() {
  fh1::SpaDatabase db;
  for (uint16_t id = 155; id <= 225; ++id) {
    if (!carview::achievement_fallback_name(id)) continue;
    fh1::SpaAchievement sa;
    sa.id = id;
    sa.gamerscore = carview::achievement_fallback_gamerscore(id);
    sa.label = carview::achievement_fallback_name(id);
    sa.flags = 0x8u;
    db.achievements.push_back(sa);
  }
  carview::AchievementsSystem sys;
  sys.init(db);
  return sys;
}

// True when `st` (and only `st`) unlocks `id` on a fresh system.
bool unlocks(uint16_t id, const carview::AchievementGameplayState& st) {
  auto sys = make_full_system();
  sys.evaluate(st, false);
  const auto* e = sys.find(id);
  return e && e->unlocked;
}

void test_rule_table() {
  int offline = 0, online = 0;
  uint32_t total_g = 0;
  for (uint16_t id = 150; id <= 230; ++id) {
    const auto rule = carview::achievement_rule(id);
    const bool fh1_id = id >= 155 && id <= 225 && id != 163;
    CHECK((rule != carview::AchievementRule::kNone) == fh1_id);
    CHECK((carview::achievement_fallback_name(id) != nullptr) == fh1_id);
    if (rule == carview::AchievementRule::kOffline) ++offline;
    if (rule == carview::AchievementRule::kOnlineOnly) ++online;
    total_g += carview::achievement_fallback_gamerscore(id);
  }
  // Online only: 168 (Design Storefront), 196-205, 213 (Rally online).
  CHECK(online == 12);
  CHECK(offline == 58);
  CHECK(total_g == 1500);
  for (uint16_t id : {uint16_t(168), uint16_t(196), uint16_t(205), uint16_t(213)})
    CHECK(carview::achievement_rule(id) == carview::AchievementRule::kOnlineOnly);
  // Vendetta (165) and Rally Rivals (212) run on local rival ghosts.
  CHECK(carview::achievement_rule(165) == carview::AchievementRule::kOffline);
  CHECK(carview::achievement_rule(212) == carview::AchievementRule::kOffline);
  CHECK(carview::achievement_fallback_gamerscore(209) == 75);
  CHECK(carview::achievement_fallback_gamerscore(225) == 80);
  CHECK(std::strcmp(carview::achievement_fallback_name(209), "Horizon Rally Champion") == 0);
  CHECK(std::strcmp(carview::achievement_fallback_name(225), "Forza Legend") == 0);

  // Online-only achievements never unlock offline, whatever the state says.
  carview::AchievementGameplayState all;
  all.num_races = all.num_victories = 1000;
  all.rally_stages_done = all.rally_victories = 1000;
  auto sys = make_full_system();
  sys.evaluate(all, false);
  for (uint16_t id : {uint16_t(168), uint16_t(196), uint16_t(201), uint16_t(205), uint16_t(213)})
    CHECK(!sys.find(id)->unlocked && !sys.find(id)->available());
}

void test_base_rules() {
  using S = carview::AchievementGameplayState;
  // 156 "Buy a car from the Festival Autoshow." (owning cars is not buying one)
  {
    S s;
    s.garage_cars = 5;
    CHECK(!unlocks(156, s));
    s.autoshow_purchases = 1;
    CHECK(unlocks(156, s));
  }
  // 157 "Finish your first race" / 158 "Win a race for the first time"
  {
    S s;
    s.num_races = 1;
    CHECK(unlocks(157, s) && !unlocks(158, s));
    s.num_victories = 1;
    CHECK(unlocks(158, s));
  }
  // 159 "Win a Mixed-Surface or Dirt Race." (any win is not enough)
  {
    S s;
    s.num_victories = 50;
    CHECK(!unlocks(159, s));
    s.dirt_races_won = 1;
    CHECK(unlocks(159, s));
  }
  // 160 "Win 10 Street Races." / 161 "Win 10 Festival Races."
  {
    S s;
    s.num_podiums = 50;
    s.num_victories = 50;
    s.street_races_won = 9;
    s.festival_races_won = 9;
    CHECK(!unlocks(160, s) && !unlocks(161, s));
    s.street_races_won = 10;
    CHECK(unlocks(160, s) && !unlocks(161, s));
    s.festival_races_won = 10;
    CHECK(unlocks(161, s));
  }
  // 162 "Beat 10 Festival Racers after challenging them to a race." (street wins do not count)
  {
    S s;
    s.street_races_won = 10;
    s.race_encounters_won = 9;
    CHECK(!unlocks(162, s));
    s.race_encounters_won = 10;
    CHECK(unlocks(162, s));
  }
  // 164 "Set a speed on 5 speed traps and 5 average speed zones."
  {
    S s;
    s.roads_discovered = 216;
    s.speed_traps_set = 10;
    s.speed_zones_set = 4;
    CHECK(!unlocks(164, s));
    s.speed_zones_set = 5;
    CHECK(unlocks(164, s));
  }
  // 165 "Beat Rivals in 10 events."
  {
    S s;
    s.rivals_beaten = 9;
    CHECK(!unlocks(165, s));
    s.rivals_beaten = 10;
    CHECK(unlocks(165, s));
  }
  // 166 "Visit the Paint Shop, Autoshow, Car Club, Marketplace, Race Central, and Workshop"
  {
    S s;
    s.garage_cars = 10;
    s.festival_venues = drive::AchievementSignals::kAllVenues & ~drive::AchievementSignals::kMarketplace;
    CHECK(!unlocks(166, s));
    s.festival_venues = drive::AchievementSignals::kAllVenues;
    CHECK(unlocks(166, s));
  }
  // 167 "Create and save a Paint Job." / 169 "Receive your Forza loyalty cars."
  {
    S s;
    s.promo_cars_spotted = 5;
    s.total_winnings = 2000000;
    CHECK(!unlocks(167, s) && !unlocks(169, s));
    s.paint_jobs_saved = 1;
    s.loyalty_cars_received = true;
    CHECK(unlocks(167, s) && unlocks(169, s));
  }
  // 170-174 popularity chart position 200 / 100 / 50 / 10 / 1.
  {
    const struct {
      uint16_t id;
      int rank;
    } tiers[] = {{170, 200}, {171, 100}, {172, 50}, {173, 10}, {174, 1}};
    for (const auto& t : tiers) {
      S s;
      s.popularity_rank = t.rank + 1;
      CHECK(!unlocks(t.id, s));
      s.popularity_rank = t.rank;
      CHECK(unlocks(t.id, s));
    }
  }
  // 183 "Win a race on the Hard difficulty setting." (festival wins do not count)
  {
    S s;
    s.festival_races_won = 10;
    CHECK(!unlocks(183, s));
    s.hard_races_won = 1;
    CHECK(unlocks(183, s));
  }
  // 184-188 "Win all / every ...": unknown totals never unlock; all of them do.
  {
    S s;
    s.festival_races_won = 69;
    s.street_races_won = 30;
    CHECK(!unlocks(184, s) && !unlocks(185, s) && !unlocks(188, s));  // totals unknown
    s.festival_races_total = 69;
    s.street_races_total = 30;
    s.showcases_total = 10;
    s.star_showdowns_total = 7;
    CHECK(unlocks(184, s) && unlocks(185, s));
    s.festival_races_won = 68;
    CHECK(!unlocks(184, s));
    s.festival_races_won = 69;
    s.showcases_won = 9;
    s.star_showdowns_won = 7;
    CHECK(!unlocks(186, s) && unlocks(187, s) && !unlocks(188, s));
    s.showcases_won = 10;
    CHECK(unlocks(186, s) && unlocks(188, s));
  }
  // 189 "Complete every Horizon Outpost PR Stunt."
  {
    S s;
    s.pr_stunts_completed = 29;
    CHECK(!unlocks(189, s));
    s.pr_stunts_completed = 30;
    CHECK(unlocks(189, s));
  }
  // 190 "Complete 5 Sponsorship Challenges." / 191 "... 20 ..." (PR stunts do not count)
  {
    S s;
    s.pr_stunts_completed = 30;
    s.sponsor_challenges_completed = 4;
    CHECK(!unlocks(190, s));
    s.sponsor_challenges_completed = 5;
    CHECK(unlocks(190, s) && !unlocks(191, s));
    s.sponsor_challenges_completed = 20;
    CHECK(unlocks(191, s));
  }
  // 192 / 193 / 194 barn find, 10 signs, all signs; 195 "Drive along all of the roads"
  {
    S s;
    s.barns_found = 9;
    s.discount_signs_found = 99;
    s.roads_discovered = 215;
    CHECK(unlocks(192, s) && unlocks(193, s) && !unlocks(194, s) && !unlocks(195, s));
    s.discount_signs_found = 100;
    s.roads_discovered = 216;
    CHECK(unlocks(194, s) && unlocks(195, s));
  }

  // from_drive_save: distinct wins by catalog style, rally ids excluded, the
  // speed records, sponsors and achievement signals carried through.
  {
    race::CareerCatalog cat;
    drive::DriveSave ds;
    int64_t next_id = 1;
    auto add = [&](int style, int n, int won) {
      for (int i = 0; i < n; ++i) {
        race::CareerEvent ev;
        ev.event_id = next_id++;
        ev.style = style;
        cat.events.push_back(ev);
        ds.event_results.emplace_back(ev.event_id, i < won ? 1 : 2);
      }
    };
    add(1, 4, 3);  // Festival
    add(2, 3, 3);  // Street
    add(3, 2, 1);  // Showcase
    add(5, 2, 2);  // Star Showdown
    add(6, 1, 1);  // Headline: none of the four
    ds.event_results.emplace_back(9001, 1);  // a Rally stage: outside the catalog
    ds.speed_traps = {{"T1", 120.0}, {"T2", 0.0}};
    ds.speed_zones = {{"Z1", 90.0}};
    ds.sponsors = {{"Drift", 10, 2}, {"Air", 4, 1}, {"Fame", 0, 3}};
    ds.fame_rewards_claimed = 3;
    ds.rivals_beaten = 4;
    ds.ach.autoshow_purchases = 2;
    ds.ach.rally_cars_purchased = {11, 12};
    ds.ach.rally_air_m = 200.5;
    const auto st = carview::AchievementGameplayState::from_drive_save(ds, 0, 0, &cat, 120);
    CHECK(st.festival_races_won == 3 && st.festival_races_total == 4);
    CHECK(st.street_races_won == 3 && st.street_races_total == 3);
    CHECK(st.showcases_won == 1 && st.showcases_total == 2);
    CHECK(st.star_showdowns_won == 2 && st.star_showdowns_total == 2);
    CHECK(st.speed_traps_set == 1 && st.speed_zones_set == 1);
    CHECK(st.sponsor_challenges_completed == 6);  // 2 + 1 skill tiers + 3 Fame tiers
    CHECK(st.rivals_beaten == 4 && st.autoshow_purchases == 2);
    CHECK(st.rally_cars_purchased == 2 && st.rally_air_m > 200.0);
    CHECK(st.popularity_rank == 120);
    // Without a catalog nothing is classified (and nothing counts as a festival win).
    const auto st0 = carview::AchievementGameplayState::from_drive_save(ds);
    CHECK(st0.festival_races_won == 0 && st0.street_races_won == 0 && st0.festival_races_total == 0);
  }
}

void test_rally_rules() {
  using S = carview::AchievementGameplayState;
  // 206 "Complete your first Horizon Rally stage." / 207 "Win a Horizon Rally."
  {
    S s;
    s.rally_stages_done = 1;
    CHECK(unlocks(206, s) && !unlocks(207, s));
    s.rally_victories = 1;
    CHECK(unlocks(207, s));
  }
  // None of 208-215 follow from stage or championship counts alone.
  {
    S s;
    s.has_rally = true;
    s.rally_stages_done = 28;
    s.rally_victories = 7;
    s.rally_level = 1;
    for (uint16_t id = 208; id <= 215; ++id) CHECK(!unlocks(id, s));
  }
  // 208 "Obtain your first Horizon Rally car."
  {
    S s;
    s.rally_cars_obtained = 1;
    CHECK(unlocks(208, s) && !unlocks(215, s));
  }
  // 209 "Become the Horizon Rally Champion."
  {
    S s;
    s.rally_champion = true;
    CHECK(unlocks(209, s));
  }
  // 210 "Win a Rally stage on Hard opponent difficulty without ... automatic shifting or the Driving Line."
  {
    S s;
    s.rally_hard_manual_stage_wins = 1;
    CHECK(unlocks(210, s));
  }
  // 211 "Get a total of 1609 metres of air in Horizon Rally events."
  {
    S s;
    s.rally_air_m = 1608.9;
    CHECK(!unlocks(211, s));
    s.rally_air_m = 1609.0;
    CHECK(unlocks(211, s));
  }
  // 212 "Complete 5 Horizon Rally events in Rivals mode."
  {
    S s;
    s.rally_rivals_events = 4;
    CHECK(!unlocks(212, s));
    s.rally_rivals_events = 5;
    CHECK(unlocks(212, s));
  }
  // 214 "Win a Horizon Rally stage without crashing or using rewind."
  {
    S s;
    s.rally_flawless_stage_wins = 1;
    CHECK(unlocks(214, s));
  }
  // 215 "Purchase all five Horizon Rally cars."
  {
    S s;
    s.rally_cars_purchased = 4;
    CHECK(!unlocks(215, s));
    s.rally_cars_purchased = 5;
    CHECK(unlocks(215, s));
  }
}

void test_signals_round_trip() {
  drive::DriveSave out;
  out.car = "VW_CorradoVR6_95";
  out.ach.autoshow_purchases = 3;
  out.ach.dirt_race_wins = 2;
  out.ach.festival_venues = drive::AchievementSignals::kPaintShop | drive::AchievementSignals::kWorkshop;
  out.ach.paint_jobs_saved = 1;
  out.ach.loyalty_cars_received = true;
  out.ach.hard_race_wins = 4;
  out.ach.rally_cars_obtained = {101, 102};
  out.ach.rally_champion = true;
  out.ach.rally_hard_manual_stage_wins = 1;
  out.ach.rally_air_m = 812.25;
  out.ach.rally_rivals_events = 5;
  out.ach.rally_flawless_stage_wins = 2;
  out.ach.rally_cars_purchased = {101, 102, 103};
  const std::string text = drive::save_to_text(out);
  drive::DriveSave in;
  std::string err;
  CHECK(drive::save_from_text(text, in, &err));
  const auto& a = in.ach;
  CHECK(a.autoshow_purchases == 3 && a.dirt_race_wins == 2 && a.paint_jobs_saved == 1);
  CHECK(a.festival_venues == (drive::AchievementSignals::kPaintShop | drive::AchievementSignals::kWorkshop));
  CHECK(a.loyalty_cars_received && a.rally_champion);
  CHECK(a.hard_race_wins == 4 && a.rally_hard_manual_stage_wins == 1 && a.rally_flawless_stage_wins == 2);
  CHECK(a.rally_air_m > 812.2 && a.rally_air_m < 812.3);
  CHECK(a.rally_rivals_events == 5);
  CHECK(a.rally_cars_obtained.size() == 2 && a.rally_cars_purchased.size() == 3);

  // An old save (no `ach` lines) loads with every signal clear.
  drive::DriveSave plain;
  plain.car = "VW_CorradoVR6_95";
  const std::string old_text = drive::save_to_text(plain);
  CHECK(old_text.find("\nach ") == std::string::npos);
  drive::DriveSave old_in;
  CHECK(drive::save_from_text(old_text, old_in, &err));
  CHECK(old_in.ach.autoshow_purchases == 0 && !old_in.ach.rally_champion && old_in.ach.rally_cars_purchased.empty());
}

// The title's real achievement table (the content packages' spa.bin under the
// state root). Returns 77 when no root is configured.
int run_corpus() {
  const char* spa_env = std::getenv("SHIFT_SPA_ROOT");
  const char* state_env = std::getenv("SHIFT_STATE_ROOT");
  const char* root = (spa_env && *spa_env) ? spa_env : state_env;
  if (!root || !*root) {
    std::printf("achievements corpus: skipped (set SHIFT_STATE_ROOT or SHIFT_SPA_ROOT)\n");
    return 77;
  }
  const std::filesystem::path state_root(root);
  fh1::SpaDatabase db;
  std::string note;
  const size_t packages = fh1::load_spa_packages({}, state_root, db, "EN", &note);
  std::printf("achievements corpus: %s: %s\n", state_root.string().c_str(), note.c_str());
  if (packages == 0) {
    std::printf("achievements corpus: FAILED: no spa.bin under %s\n", state_root.string().c_str());
    return 1;
  }
  CHECK(db.achievements.size() == 70);
  uint32_t total = 0;
  for (const auto& a : db.achievements) total += a.gamerscore;
  CHECK(total == 1500);
  CHECK(db.find(163) == nullptr);

  carview::AchievementsSystem sys;
  sys.init(db);
  CHECK(sys.total_count() == 70);
  CHECK(sys.total_gamerscore() == 1500);
  const struct {
    uint16_t id;
    const char* name;
    uint16_t g;
  } samples[] = {
      {155, "Welcome to Horizon", 10}, {165, "Vendetta", 20},          {172, "Almost Famous", 20},
      {182, "Darius Who?", 100},       {195, "Road Trip", 25},         {206, "Now that's a Special Stage", 20},
      {209, "Horizon Rally Champion", 75}, {215, "High Five!", 20},    {225, "Forza Legend", 80},
  };
  for (const auto& sm : samples) {
    const auto* e = sys.find(sm.id);
    CHECK(e != nullptr);
    if (!e) continue;
    if (e->name != sm.name || e->gamerscore != sm.g)
      std::printf("  id %u: \"%s\" %uG, expected \"%s\" %uG\n", unsigned(sm.id), e->name.c_str(),
                  unsigned(e->gamerscore), sm.name, unsigned(sm.g));
    CHECK(e->name == sm.name);
    CHECK(e->gamerscore == sm.g);
  }
  for (const auto& a : db.achievements) {
    const auto* e = sys.find(a.id);
    CHECK(e != nullptr);
    if (!e) continue;
    // No placeholder names; descriptions are the SPA's.
    CHECK(!e->name.empty() && e->name.find("ACHIEVEMENT") == std::string::npos);
    CHECK(!e->description.empty() && e->description.find("spa.bin") == std::string::npos);
    CHECK(!e->unachieved_description.empty());
    // Every SPA id has a rule, and the fallback table matches the SPA.
    CHECK(carview::achievement_rule(a.id) != carview::AchievementRule::kNone);
    const char* fb = carview::achievement_fallback_name(a.id);
    CHECK(fb != nullptr && a.label == fb);
    CHECK(carview::achievement_fallback_gamerscore(a.id) == a.gamerscore);
    if (fb && a.label != fb) std::printf("  id %u: fallback \"%s\" != SPA \"%s\"\n", unsigned(a.id), fb, a.label.c_str());
  }
  for (uint16_t id = 155; id <= 225; ++id) {
    if (id == 163) continue;
    CHECK(db.find(id) != nullptr);
    CHECK(carview::achievement_rule(id) != carview::AchievementRule::kNone);
  }

  // With the disc too: the "win all / every" totals from the real catalog
  // (69 Festival Races, 30 Street Races, 10 Showcases, 7 Star Showdowns).
  if (const char* game = std::getenv("SHIFT_GAME_ROOT"); game && *game) {
    race::CareerCatalog cat;
    std::string err;
    const bool ok = race::load_career_catalog(game, cat, &err);
    CHECK(ok);
    if (!ok) std::printf("  career catalog: %s\n", err.c_str());
    const auto st = carview::AchievementGameplayState::from_drive_save(drive::DriveSave{}, 0, 0, &cat);
    std::printf("achievements corpus: catalog totals festival %d, street %d, showcase %d, star showdown %d\n",
                st.festival_races_total, st.street_races_total, st.showcases_total, st.star_showdowns_total);
    CHECK(st.festival_races_total == 69);
    CHECK(st.street_races_total == 30);
    CHECK(st.showcases_total == 10);
    CHECK(st.star_showdowns_total == 7);
  }
  return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--corpus") == 0) {
    const int rc = run_corpus();
    if (rc == 0) std::printf("achievements_tests --corpus: ALL PASSED\n");
    else if (rc == 1) std::printf("achievements_tests --corpus: %d failure(s)\n", g_failures);
    return rc;
  }
  test_spa_parser();
  test_wristband_tiers();
  test_achievements_system_and_persistence();
  test_rule_table();
  test_base_rules();
  test_rally_rules();
  test_signals_round_trip();
  if (g_failures) {
    std::printf("achievements_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("achievements_tests: ALL PASSED\n");
  return 0;
}
