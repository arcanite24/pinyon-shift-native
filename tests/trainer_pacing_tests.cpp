// Unit tests for Slice 12 Sub-Milestone 3 + headless 1280x720 verification PNG
// generator:
// - Save tree isolation (user vs user-modded leaving main save untouched)
// - F10 Trainer (Player, World, Vehicle, Debug, Any Car in Any Event, Fast
//   Travel Anywhere, Saved Map Road GPS & Teleport with 10s expiry)
// - Festival Pacing (0.25x-3x prices excl. DLC, 0.5x-5x rewards/wristband/XP)
// - Hidden Cars (4 qualified variants selectable/buyable; traffic excluded)
// - Headless PNG captures when SHIFT_S12_SCREENSHOT_DIR is set.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <sqlite3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "autoshow.h"
#include "camera_editor.h"
#include "cars/physics.h"
#include "cars/sources.h"
#include "drive/cameras.h"
#include "drive/save.h"
#include "drive/trainer.h"
#include "fh1data/archive.h"
#include "fh1data/ui_scene.h"
#include "fh1data/vfont.h"
#include "fh1data/xds.h"
#include "garage/garage.h"
#include "hub_scene.h"
#include "race/career.h"
#include "race/rewards.h"
#include "trainer_overlay.h"
#include "ui.h"
#include "ui4_chrome.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

namespace fs = std::filesystem;

std::string read_whole_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void test_save_isolation() {
  const fs::path root = fs::temp_directory_path() / "shift_s12_save_isolation_test";
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path main_save = root / "user" / "save.txt";
  drive::DriveSave orig;
  orig.car = "VW_Corrado_95";
  orig.position = {100.0, 12.0, 200.0};
  orig.heading = 0.5;
  orig.credits = 25000;
  orig.wristband_points = 10;
  orig.collected_wristband = 0;
  std::string err;
  check(drive::write_save(main_save, orig, &err), "write main save in user/");
  const std::string orig_bytes = read_whole_file(main_save);

  drive::TrainerConfig tc;
  drive::FestivalPacingConfig pc;
  check(!drive::requires_modded_save(tc, pc, false), "clean session uses main save");
  check(drive::resolve_isolated_save_path(main_save, false) == main_save, "unmodded resolves to user/save.txt");

  tc.cheats_enabled = true;
  check(drive::requires_modded_save(tc, pc, false), "cheats require user-modded");
  tc.cheats_enabled = false;
  pc.enabled = true;
  pc.price_multiplier = 0.5;
  check(drive::requires_modded_save(tc, pc, false), "non-1x Festival Pacing requires user-modded");
  pc.enabled = false;
  check(drive::requires_modded_save(tc, pc, true), "Hidden Cars requires user-modded");

  bool seeded = false;
  const fs::path modded_save = drive::resolve_isolated_save_path(main_save, true, &seeded);
  check(seeded, "first modded run seeds user-modded/save.txt from user/save.txt");
  check(modded_save == root / "user-modded" / "save.txt", "modded path is user-modded/save.txt");

  // Mutate modded save heavily via trainer cheats.
  drive::DriveSave modded;
  check(drive::read_save(modded_save, modded, &err), "read seeded modded save");
  check(modded.credits == 25000, "seeded modded save inherits player credits");
  modded.credits = 20000000;
  modded.wristband_points = 2500;
  modded.collected_wristband = 6;
  check(drive::write_save(modded_save, modded, &err), "write cheated modded save");

  // Verify main save is 100% untouched byte-for-byte!
  check(read_whole_file(main_save) == orig_bytes, "main user/save.txt is 100% untouched");

  fs::remove_all(root, ec);
}

void test_trainer_player_world_vehicle_and_any_car() {
  drive::TrainerState st;
  int64_t credits = 10000;
  int wb_pts = 0;
  int wb_col = 0;

  // Cheats off -> actions rejected & scales remain 1.0.
  check(!st.apply_set_credits(credits, 5000000) && credits == 10000, "set credits rejected with cheats off");
  check(!st.apply_add_credits(credits, 1000000) && credits == 10000, "add credits rejected with cheats off");
  check(!st.apply_set_wristband_level(wb_pts, wb_col, 6) && wb_col == 0, "wristband rejected with cheats off");
  st.config.game_speed = 2.0;
  check(st.effective_game_speed() == 1.0, "game speed stays 1.0 with cheats off");

  // Enable cheats.
  st.config.cheats_enabled = true;
  check(st.apply_set_credits(credits, 1000000) && credits == 1000000, "set credits sets 1,000,000 CR");
  check(st.apply_add_credits(credits, 250000) && credits == 1250000, "add credits adds +250,000 CR");
  check(st.apply_set_wristband_level(wb_pts, wb_col, 6) && wb_col == 6 && wb_pts >= 2000,
        "set wristband level unlocks Gold (tier 6) and threshold points");

  // World: game speed, time of day, day length (12 min..24 h + real PC clock), free camera.
  st.config.game_speed = 0.5;
  check(std::fabs(st.effective_game_speed() - 0.5) < 1e-6, "game speed scales to 0.5x");
  st.config.time_of_day_hour = 19.0;
  check(std::fabs(st.effective_fixed_hour() - 19.0) < 1e-6 && st.effective_day_length_scale() == 0.0,
        "held time of day locks clock at 19:00");
  st.config.time_of_day_hour = -1.0;
  st.config.day_length_minutes = 12.0;
  check(st.effective_day_length_scale() > 2.4, "12-minute day speeds up clock (~2.54x)");
  st.config.day_length_minutes = 1440.0;
  check(st.effective_day_length_scale() < 0.03, "24-hour day slows clock");
  st.config.day_length_minutes = drive::kDayLengthPcClock;
  const double pc_hr = st.effective_fixed_hour();
  check(pc_hr >= 0.0 && pc_hr < 24.0, "PC clock mode returns valid local hour [0, 24)");

  // Free camera fly movement.
  st.config.free_camera = true;
  st.free_cam.reset_from_pose({0.0, 5.0, 0.0}, {0.0, 0.0, 1.0});
  st.free_cam.step(0.1, 1.0, 0.0, 0.5, 0.0, 0.0, false);
  check(st.free_cam.pos[2] > 3.0 && st.free_cam.pos[1] > 6.0, "Free camera flies forward and up");

  // Any Car in Any Event overriding EventRestrictions & TargetClass.
  race::CareerCatalog cat;
  race::CarSpec r1_car;
  r1_car.car_id = 999;
  r1_car.stock_class = 7;  // R1
  r1_car.props.drive_type = shift::data::kDriveRwd;
  r1_car.props.year = 2012;
  cat.cars[999] = r1_car;

  race::CareerEvent ev;
  ev.event_id = 42;
  ev.target_class = 2;  // C class max
  ev.restrictions.buckets[0].drive_type = shift::data::kDriveFwd;
  ev.restrictions.buckets[0].year_max = 1999;

  const std::vector<std::string> classes = {"F", "E", "D", "C", "B", "A", "S", "R3", "R2", "R1"};
  check(!cat.check_eligibility(ev, 999, 7, classes, false).eligible,
        "R1 RWD car rejected from C-class FWD classic event without override");
  st.config.any_car_in_any_event = true;
  check(cat.check_eligibility(ev, 999, 7, classes, st.effective_any_car()).eligible,
        "Any Car in Any Event overrides TargetClass and EventRestrictions");

  // Vehicle: Fast Travel Anywhere, Saved Map Road persistence, GPS & Teleport expiry + race lock.
  st.config.fast_travel_anywhere = true;
  check(st.effective_fast_travel_anywhere(), "Fast Travel Anywhere active");
  st.note_map_road({-1250.5, 42.25, 890.75});
  check(st.save_last_map_road(), "save_last_map_road stores selected road");
  const std::string serialized = drive::format_road_point(*st.config.saved_map_road);
  const auto parsed = drive::parse_road_point(serialized);
  check(parsed.has_value() && std::fabs((*parsed)[0] + 1250.5) < 0.01 && std::fabs((*parsed)[2] - 890.75) < 0.01,
        "saved road coordinates round-trip cleanly");

  // GPS rejected in race, accepted in free roam.
  check(!st.request_gps_to_saved_road(true), "GPS request rejected during active race");
  check(st.request_gps_to_saved_road(false), "GPS request accepted in free roam");
  check(st.consume_gps(false).has_value(), "GPS request consumed once");
  check(!st.consume_gps(false).has_value(), "GPS request does not fire twice");

  // Teleport expires after 10 s when map not open, executes when map open within 10 s.
  check(st.request_teleport_to_saved_road(100.0, false), "Teleport queued at t=100s");
  check(!st.consume_teleport(105.0, false, false).has_value(), "Teleport waits while map closed");
  check(!st.consume_teleport(111.0, true, false).has_value(), "Teleport expires after 10 s");
  check(st.request_teleport_to_saved_road(200.0, false), "Teleport re-queued at t=200s");
  const auto tp = st.consume_teleport(204.0, true, false);
  check(tp.has_value() && std::fabs((*tp)[0] + 1250.5) < 0.01, "Teleport executes when map opens within 10 s");
  check(!st.consume_teleport(204.1, true, false).has_value(), "Teleport does not repeat");
}

void test_festival_pacing_and_rewards() {
  drive::FestivalPacingConfig p;
  p.enabled = true;
  p.price_multiplier = 0.5;
  p.reward_multiplier = 2.0;

  check(p.scale_price(50000, false) == 25000, "Festival Pacing 0.5x halves base car/upgrade price");
  check(p.scale_price(50000, true) == 50000, "Festival Pacing keeps DLC prices unchanged");
  p.price_multiplier = 3.0;
  check(p.scale_price(10000, false) == 30000, "Festival Pacing 3.0x triples base price");
  p.price_multiplier = 0.25;
  check(p.scale_price(10000, false) == 2500, "Festival Pacing 0.25x quarters base price");
  p.enabled = false;
  check(p.scale_price(10000, false) == 10000, "Festival Pacing off leaves price unchanged");

  // Reward multiplier in race::award_race (0.5x..5.0x on credits, wristband, driver XP, affinity XP).
  race::WristbandTables wb;
  wb.points[0][1] = 100;
  race::CareerRecord r1, r2;
  race::RaceBonusInputs b1;
  b1.reward_multiplier = 1.0;
  race::RaceBonusInputs b2;
  b2.reward_multiplier = 3.0;
  const auto a1 = race::award_race(r1, 10, 100, 1, 1, 0, 20000.0, 8, 1, true, 120.0, 60.0, 0, wb, 0, 3000.0, b1);
  const auto a2 = race::award_race(r2, 10, 100, 1, 1, 0, 20000.0, 8, 1, true, 120.0, 60.0, 0, wb, 0, 3000.0, b2);
  check(a2.credits == a1.credits * 3, "Festival Pacing 3.0x triples race credits");
  check(a2.wristband_points == a1.wristband_points * 3, "Festival Pacing 3.0x triples wristband points");
  check(a2.driver_xp == a1.driver_xp * 3, "Festival Pacing 3.0x triples driver XP");
  check(a2.affinity_xp == a1.affinity_xp * 3, "Festival Pacing 3.0x triples loyalty/affinity XP");
}

// Creates a minimal synthetic gamedb.slt with stock cars, the 4 qualified Hidden
// Cars, and excluded traffic cars (Tour Bus 1529, Beetle 282, Camry 1532).
bool create_synthetic_hidden_db(const fs::path& game_root) {
  std::error_code ec;
  fs::create_directories(game_root / "media" / "db", ec);
  const fs::path db_path = game_root / "media" / "db" / "gamedb.slt";
  fs::remove(db_path, ec);

  sqlite3* db = nullptr;
  if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) return false;
  const char* schema = R"SQL(
    CREATE TABLE PowerTrains(Id INT PRIMARY KEY);
    INSERT INTO PowerTrains VALUES(1);
    CREATE TABLE ContentOffers(OfferId INT PRIMARY KEY, PubOfferingID INT, LicenseMask INT);
    CREATE TABLE ContentOffersMapping(ContentId INT, OfferId INT, ContentType INT);
    CREATE TABLE List_CarMake(ID INT PRIMARY KEY, IconPathBase TEXT, ManufacturerCode TEXT);
    INSERT INTO List_CarMake VALUES(1,'Volkswagen','VW'),(2,'MINI','MIN'),(3,'Abarth','ABA'),(4,'Peugeot','PEU'),(5,'Nissan','NIS'),(6,'Traffic','TRF');
    CREATE TABLE Combo_Colors(ID INT PRIMARY KEY, Ordinal INT, Sequence INT, Metallic INT, RGB INT);
    INSERT INTO Combo_Colors VALUES
      (1,309,1,1,13376028),(2,1565,1,1,13376028),(3,1566,1,1,1801944),
      (4,1567,1,1,15263976),(5,1568,1,1,14811156),(6,1568,2,1,2241338);
    CREATE TABLE Combo_Colors_Exceptions(Ordinal INT, Sequence INT, HasStripe INT, ColorStripe INT);
    CREATE TABLE Data_Car(
      Id INT PRIMARY KEY, BaseCost INT, Year INT, ClassID INT, PerformanceIndex REAL,
      MediaName TEXT, IsDrivable INT, IsSelectable INT, IsUnicorn INT, PowertrainID INT,
      MakeID INT, DisplayName TEXT, DriveTypeID INT,
      SpeedRating REAL, HandlingRating REAL, AccelerationRating REAL, LaunchRating REAL, BrakingRating REAL,
      SimPeakPower REAL, SimPeakTorque REAL, CurbWeight REAL
    );
    INSERT INTO Data_Car VALUES
      (309, 6000, 1995, 1, 286, 'VW_Corrado_95', 1, 1, 0, 1, 1, '1995 VW Corrado VR6', 1, 4.9, 4.6, 5.1, 4.8, 4.7, 1320, 2.35, 12.6),
      (1565, 0, 1995, 1, 286, 'VW_Corrado_95', 1, 0, 0, 1, 1, 'Intro Corrado', 1, 5.0, 4.7, 5.2, 4.9, 4.8, 1320, 2.35, 12.6),
      (1566, 0, 2009, 2, 358, 'MIN_CooperS_09', 1, 0, 0, 1, 2, 'Showcase Mini', 1, 5.6, 5.8, 5.9, 5.4, 5.7, 1300, 2.40, 12.1),
      (1567, 0, 2010, 2, 372, 'ABA_500esseesse_10', 1, 0, 0, 1, 3, 'Showcase 500 Abarth', 1, 5.5, 6.0, 6.1, 5.8, 6.0, 1190, 2.30, 10.3),
      (1568, 0, 1984, 3, 488, 'PEU_205T16_84', 1, 0, 0, 1, 4, 'Showcase 205 T16', 3, 6.4, 6.6, 7.4, 8.1, 6.5, 1470, 2.55, 11.4),
      (1529, 1000, 2008, 0, 100, 'TRF_TourBus_08', 1, 0, 0, 1, 6, 'Tour Bus', 2, 2.0, 1.5, 1.8, 1.5, 1.8, 2200, 8.0, 95.0),
      (282, 1000, 2004, 0, 150, 'TRF_Beetle_04', 1, 0, 0, 1, 6, 'Traffic Beetle', 1, 3.0, 3.0, 3.0, 3.0, 3.0, 850, 1.6, 12.0),
      (1532, 1000, 2009, 0, 180, 'TRF_Camry_09', 1, 0, 0, 1, 6, 'Traffic Camry', 1, 3.5, 3.2, 3.4, 3.1, 3.3, 1200, 2.1, 14.5);
  )SQL";
  char* errmsg = nullptr;
  const int rc = sqlite3_exec(db, schema, nullptr, nullptr, &errmsg);
  if (errmsg) sqlite3_free(errmsg);
  sqlite3_close(db);
  return rc == SQLITE_OK;
}

void test_hidden_cars_unlock_and_exclusions() {
  check(drive::is_hidden_car_id(1565) && drive::is_hidden_car_id(1566) &&
            drive::is_hidden_car_id(1567) && drive::is_hidden_car_id(1568),
        "All 4 qualified hidden cars recognized");
  check(!drive::is_hidden_car_id(1529) && !drive::is_hidden_car_id(282) && !drive::is_hidden_car_id(1532),
        "Traffic vehicles (Tour Bus 1529, Beetle 282, Camry 1532) strictly excluded");

  const fs::path root = fs::temp_directory_path() / "shift_s12_hidden_db_test";
  std::error_code ec;
  fs::remove_all(root, ec);
  check(create_synthetic_hidden_db(root), "create synthetic hidden cars DB");

  cars::Install inst;
  inst.media = root / "media";
  cars::PartsDb db;
  std::string err;
  check(db.open(inst, &err), "PartsDb opens synthetic DB");

  // Stock dealer list excludes hidden cars and traffic.
  std::vector<cars::DealerCar> stock;
  check(db.dealer(stock, &err, false), "stock dealer query succeeds");
  check(stock.size() == 1 && stock[0].id == 309, "stock dealer lists only selectable car 309");

  // Hidden Cars unlocked: adds exact 4 retained variants, NEVER traffic!
  std::vector<cars::DealerCar> unlocked;
  check(db.dealer(unlocked, &err, true), "hidden dealer query succeeds");
  check(unlocked.size() == 5, "hidden dealer lists stock + 4 hidden cars (excludes all 3 traffic rows)");
  std::set<int64_t> ids;
  for (const auto& c : unlocked) ids.insert(c.id);
  check(ids.count(1565) && ids.count(1566) && ids.count(1567) && ids.count(1568),
        "hidden dealer contains 1565, 1566, 1567, 1568");
  check(!ids.count(1529) && !ids.count(282) && !ids.count(1532), "hidden dealer excludes traffic cars");

  // Autoshow with Hidden Cars + Festival Pacing 0.5x prices.
  carview::Autoshow show;
  std::string why;
  check(show.open(db, {}, why, 500000, false, true, 0.5), "Autoshow opens with Hidden Cars + 0.5x Pacing");
  // Find the "Hidden & Showcase" tab.
  size_t hidden_tab = 0;
  bool found_hidden_tab = false;
  for (size_t i = 0; i < show.makes().size(); ++i) {
    if (show.makes()[i].make_id == -3) {
      hidden_tab = i;
      found_hidden_tab = true;
      break;
    }
  }
  check(found_hidden_tab, "Autoshow exposes Hidden & Showcase make tab");
  show.select_make(hidden_tab, 500000);
  check(show.visible_cars().size() == 4, "Hidden & Showcase tab lists all 4 hidden variants");
  // Verify 0.5x Festival Pacing scaled Peugeot 205 T16 from 130,000 CR to 65,000 CR.
  bool found_205 = false;
  for (const auto& c : show.visible_cars()) {
    if (c.dealer.id == 1568) {
      found_205 = true;
      check(c.dealer.base_cost == 65000, "Festival Pacing 0.5x halves Showcase 205 T16 fallback cost");
    }
  }
  check(found_205, "Showcase Peugeot 205 T16 present in Autoshow");

  fs::remove_all(root, ec);
}

// ---- Optional 1280x720 Headless Verification Screenshots ----

fs::path find_game_root() {
  if (const char* e = std::getenv("SHIFT_GAME_ROOT"); e && *e && fs::exists(fs::path(e) / "media")) return e;
  for (const char* p : {R"(C:\Users\neri\code\horizon1-recomp\.local\game\base)",
                        R"(C:\Users\neri\code\shift-engine\.local\game\base)"}) {
    if (fs::exists(fs::path(p) / "media")) return p;
  }
  return {};
}

void bind_real_fh1_ui_assets(const fs::path& game_root) {
  if (game_root.empty()) return;
  if (auto fzip = fh1::Archive::open(game_root / "media" / "ui" / "Fonts.zip", nullptr)) {
    auto vf_load = [&](const char* nm) {
      auto r = fzip->read(nm);
      auto f = std::make_shared<fh1::VFont>();
      if (r.ok() && fh1::parse_vfont(r.data, *f, nullptr)) return f;
      return std::shared_ptr<fh1::VFont>{};
    };
    carview::ui::set_vector_fonts(vf_load("aru_vector_aa.dt"), vf_load("bru_vector_aa.dt"),
                                  vf_load("eru_vector_aa.dt"), vf_load("dru_vector_aa.dt"));
  }

  struct TexCache {
    std::unique_ptr<fh1::Archive> hz;
    std::unique_ptr<fh1::Archive> tx;
    std::unique_ptr<fh1::Archive> ui;
    std::map<std::string, std::unique_ptr<fh1::UiScene>> scenes;
    std::set<std::string> missing;
  };
  auto cache = std::make_shared<TexCache>();
  cache->hz = fh1::Archive::open(game_root / "media" / "ui" / "textures" / "Horizon.zip", nullptr);
  cache->tx = fh1::Archive::open(game_root / "media" / "ui" / "Textures.zip", nullptr);
  cache->ui = fh1::Archive::open(game_root / "media" / "UI.zip", nullptr);

  if (cache->hz) {
    for (const auto& [k, m] : {std::pair{"tape.smudge", "DIRTMASKS/SMUDGETAPE01.xds"},
                               std::pair{"tape.crisp", "DIRTMASKS/CRISPTAPE01.xds"}}) {
      auto r = cache->hz->read(m);
      const auto x = r.ok() ? fh1::decode_xds(r.data, false) : fh1::XdsResult{};
      std::array<float, 4> uv{};
      if (!x.levels.empty()) carview::ui::atlas_image(k, x.levels[0].width, x.levels[0].height, x.levels[0].rgba, uv);
    }
  }

  carview::ui4::set_texture_loader([cache](const std::string& path, std::array<float, 4>& uv) -> bool {
    if (carview::ui::atlas_find(path, uv)) return true;
    if (cache->missing.count(path)) return false;
    const bool t = path.rfind("T:", 0) == 0;
    fh1::Archive* a = t ? cache->tx.get() : cache->hz.get();
    if (!a) {
      cache->missing.insert(path);
      return false;
    }
    auto r = a->read((t ? path.substr(2) : path) + ".xds");
    if (!r.ok()) {
      cache->missing.insert(path);
      return false;
    }
    const auto x = fh1::decode_xds(r.data, false);
    if (x.levels.empty() || !carview::ui::atlas_image(path, x.levels[0].width, x.levels[0].height, x.levels[0].rgba, uv)) {
      cache->missing.insert(path);
      return false;
    }
    return true;
  });

  carview::ui4::set_scene_loader([cache](const std::string& name) -> const fh1::UiScene* {
    if (auto it = cache->scenes.find(name); it != cache->scenes.end()) return it->second.get();
    if (!cache->ui) return nullptr;
    auto fbf = cache->ui->read("Scenes/ui4/" + name + ".fbf");
    auto bsg = cache->ui->read("Scenes/ui4/" + name + ".bsg");
    auto bgf = cache->ui->read("Scenes/ui4/" + name + ".bgf");
    auto sc = std::make_unique<fh1::UiScene>();
    if (fbf.ok() && bsg.ok() && fh1::parse_ui_fbf(fbf.data, *sc, nullptr) && fh1::parse_ui_bsg(bsg.data, *sc, nullptr)) {
      if (bgf.ok()) sc->has_bgf = fh1::parse_ui_bgf(bgf.data, sc->bgf, nullptr);
      return (cache->scenes[name] = std::move(sc)).get();
    }
    return nullptr;
  });
}

std::vector<uint8_t> make_backdrop(uint32_t w, uint32_t h, bool studio) {
  std::vector<uint8_t> fb(size_t(w) * h * 4, 255);
  for (uint32_t y = 0; y < h; ++y) {
    const float t = float(y) / float(h - 1);
    for (uint32_t x = 0; x < w; ++x) {
      const float dx = float(x) / float(w) - 0.5f;
      const float vig = std::clamp(1.0f - 0.40f * dx * dx, 0.55f, 1.0f);
      uint8_t* p = &fb[(size_t(y) * w + x) * 4];
      if (studio) {
        p[0] = uint8_t((30.0f + 34.0f * (1.0f - t)) * vig);
        p[1] = uint8_t((34.0f + 38.0f * (1.0f - t)) * vig);
        p[2] = uint8_t((46.0f + 44.0f * (1.0f - t)) * vig);
      } else {
        // Warm Colorado dusk sky over dark asphalt horizon.
        const bool sky = t < 0.54f;
        const float u = sky ? (t / 0.54f) : ((t - 0.54f) / 0.46f);
        p[0] = uint8_t((sky ? (72.0f + 120.0f * u) : (58.0f - 28.0f * u)) * vig);
        p[1] = uint8_t((sky ? (88.0f + 68.0f * u) : (54.0f - 24.0f * u)) * vig);
        p[2] = uint8_t((sky ? (128.0f - 20.0f * u) : (60.0f - 24.0f * u)) * vig);
      }
      p[3] = 255;
    }
  }
  return fb;
}

void write_png(const fs::path& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
  std::error_code ec;
  if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
  stbi_write_png(path.string().c_str(), int(w), int(h), 4, rgba.data(), int(w * 4));
}

void render_verification_screenshots() {
  const char* dir_env = std::getenv("SHIFT_S12_SCREENSHOT_DIR");
  if (!dir_env || !*dir_env) return;
  const fs::path out_dir(dir_env);
  constexpr uint32_t W = 1280, H = 720;

  const fs::path game_root = find_game_root();
  bind_real_fh1_ui_assets(game_root);

  // (a) F10 Trainer UI4 Overlay (Player / World / Vehicle / Debug tabs).
  {
    drive::TrainerState st;
    st.config.cheats_enabled = true;
    st.config.game_speed = 1.0;
    st.config.time_of_day_hour = 18.0;
    st.config.day_length_minutes = 48.0;
    st.config.any_car_in_any_event = true;
    st.config.fast_travel_anywhere = true;
    st.config.reveal_collectibles = true;
    st.config.saved_map_road = std::array<double, 3>{-1790.46, 34.16, 1863.44};
    st.last_map_road = st.config.saved_map_road;
    st.pacing.enabled = true;
    st.pacing.price_multiplier = 0.5;
    st.pacing.reward_multiplier = 2.0;
    st.hidden_cars = true;

    carview::TrainerOverlay ov;
    ov.open();
    ov.set_tab(carview::TrainerOverlay::Tab::kWorld);
    ov.move(5);  // highlight ANY CAR IN ANY EVENT

    carview::ui::List list;
    carview::draw_trainer_ui4(list, float(W), float(H), ov, st, 5250000, 6, "user-modded");
    auto fb = make_backdrop(W, H, false);
    carview::ui::rasterize(list.items(), fb, W, H);
    write_png(out_dir / "s12_trainer_overlay.png", fb, W, H);
  }

  // (b) Live Camera Editor & Immersive Camera Settings UI (with PlayStation glyphs showcase!).
  {
    drive::CameraEditorConfig ed;
    ed.chase_height_m = 0.50;
    ed.chase_distance_m = 0.75;
    ed.chase_fov_scale = 1.15;
    ed.chase_stiffness = 1.50;
    ed.driving_view = drive::DrivingView::kNormal;
    drive::ImmersiveCameraConfig imm;
    imm.apply_preset(drive::ImmersivePreset::kStrong);
    imm.lean = 1.25;  // shows CUSTOM
    drive::HeadTrackingConfig ht;
    ht.enabled = true;
    ht.port = 4242;

    carview::CameraEditorOverlay ov;
    ov.open(ed, imm, ht);
    ov.set_tab(carview::CameraEditorOverlay::Tab::kCameraEditor);
    ov.move(2);  // highlight CHASE FIELD OF VIEW

    drive::HeadPose live_pose;
    live_pose.yaw_deg = 12.4f;
    live_pose.pitch_deg = -2.1f;
    live_pose.roll_deg = 1.8f;

    carview::ui4::set_glyph_set(carview::ui4::GlyphSet::kPlayStation);
    carview::ui::List list;
    carview::draw_camera_editor_ui4(list, float(W), float(H), ov, live_pose);
    carview::ui4::set_glyph_set(carview::ui4::GlyphSet::kXbox);

    auto fb = make_backdrop(W, H, false);
    carview::ui::rasterize(list.items(), fb, W, H);
    write_png(out_dir / "s12_camera_editor_immersive.png", fb, W, H);
  }

  // (c) Hidden Cars selectable in UI4 Autoshow (4 qualified variants + Festival Pacing prices).
  {
    const fs::path synth_root = fs::temp_directory_path() / "shift_s12_autoshow_png_db";
    std::error_code ec;
    fs::remove_all(synth_root, ec);
    const fs::path db_root = !game_root.empty() && fs::exists(game_root / "media" / "db" / "gamedb.slt")
                                 ? game_root
                                 : (create_synthetic_hidden_db(synth_root), synth_root);
    cars::Install inst;
    inst.media = db_root / "media";
    cars::PartsDb db;
    std::string err;
    if (db.open(inst, &err)) {
      carview::Autoshow show;
      std::string why;
      if (show.open(db, {}, why, 5250000, false, true, 0.5)) {
        for (size_t i = 0; i < show.makes().size(); ++i) {
          if (show.makes()[i].make_id == -3) {
            show.select_make(i, 5250000);
            break;
          }
        }
        // Highlight Showcase Peugeot 205 T16 or 2nd item so all 4 hidden cars are clearly listed.
        if (show.visible_cars().size() >= 4) show.move(3);

        std::vector<garage::ClassBand> bands;
        if (game_root.empty() || !garage::load_class_bands(game_root, bands, nullptr)) {
          bands = {{0.20, 200, "E"}, {0.30, 300, "D"}, {0.40, 400, "C"}, {0.50, 500, "B"},
                   {0.60, 600, "A"}, {0.70, 700, "S"}, {0.80, 800, "R3"}, {0.90, 900, "R2"}, {1.00, 999, "R1"}};
        }
        carview::ui::List list;
        carview::draw_autoshow_hud(list, float(W), float(H), show, bands, 5250000,
                                   "HIDDEN CARS UNLOCKED (4 QUALIFIED VARIANTS)  |  FESTIVAL PACING 0.5X PRICES");
        auto fb = make_backdrop(W, H, true);
        carview::ui::rasterize(list.items(), fb, W, H);
        write_png(out_dir / "s12_hidden_cars_autoshow.png", fb, W, H);
      }
    }
    fs::remove_all(synth_root, ec);
  }
}

}  // namespace

int main() {
  test_save_isolation();
  test_trainer_player_world_vehicle_and_any_car();
  test_festival_pacing_and_rewards();
  test_hidden_cars_unlock_and_exclusions();
  render_verification_screenshots();
  if (g_failures > 0) {
    std::fprintf(stderr, "trainer_pacing_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("trainer_pacing_tests: PASS\n");
  return 0;
}
