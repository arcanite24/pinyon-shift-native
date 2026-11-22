// Dedicated UI4 1:1 Visual & UX Parity test suite and 1280x720 headless
// screenshot generator (Slice 10.5).
// Runs unit checks without disc files; when SHIFT_GAME_ROOT and
// SHIFT_UI4_SCREENSHOT_DIR are set, loads real Fonts.zip / UI.zip /
// Horizon.zip / Textures.zip / MapView and writes crisp 1280x720 PNGs for
// visual inspection across Groups 1, 2, and 3.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <sqlite3.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "achievements.h"
#include "autoshow.h"
#include "cars/physics.h"
#include "cars/sources.h"
#include "events_page.h"
#include "fh1data/archive.h"
#include "fh1data/spa.h"
#include "fh1data/ui_scene.h"
#include "fh1data/vfont.h"
#include "fh1data/xds.h"
#include "freeroam/outposts.h"
#include "freeroam/road_discovery.h"
#include "game_hud.h"
#include "garage/garage.h"
#include "garage/pi_calc.h"
#include "hub_scene.h"
#include "hud.h"
#include "hud_scene.h"
#include "livery_editor.h"
#include "map_view.h"
#include "race/event.h"
#include "photo_mode.h"
#include "replay_view.h"
#include "settings_page.h"
#include "ui.h"
#include "ui4_chrome.h"
#include "ui_widgets.h"
#include "upgrade_shop.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

void test_ui4_chrome_primitives() {
  carview::ui::List l;
  carview::ui4::subtle_vignette(l, 1280, 720, 0.3f);
  carview::ui4::title_banner(l, 1.0f, 90.0f, 44.0f, "AUTOSHOW", "HORIZON FESTIVAL", true, 540.0f);
  carview::ui4::tab_strip(l, 1.0f, 90.0f, 120.0f, 900.0f, {"ALL", "EVENTS", "STREET", "OUTPOSTS"}, 1, true);
  carview::ui4::panel(l, 90.0f, 170.0f, 520.0f, 420.0f, 1.0f, carview::ui4::kCharcoalDark, carview::ui4::kPink);
  carview::ui4::class_badge(l, 110.0f, 190.0f, 26.0f, "A", 584);
  carview::ui4::stat_bar(l, 110.0f, 232.0f, 360.0f, 22.0f, "SPEED", 6.4, 7.2, true, 1.0f);
  carview::ui4::offwhite_strip(l, 110.0f, 270.0f, 280.0f, 298.0f, 1.0f, true);
  carview::ui4::CareerStripData cs;
  cs.band_name = "BLUE WRISTBAND";
  cs.band_next = "PURPLE";
  cs.band_level = 3;
  cs.band_collect_ready = true;
  cs.car_name = "Nissan Skyline GT-R V-spec II";
  cs.car_class = "B";
  cs.car_pi = 461;
  cs.credits = 117700;
  carview::ui4::career_strip(l, cs, 1280, 720);
  carview::ui4::button_bar(l, {{"A", "SELECT"}, {"X", "FILTER"}, {"LB", "PREV"}, {"B", "BACK"}}, 1280, 720);
  check(l.items().size() > 30, "ui4_chrome primitives emit rich UI instances");

  std::vector<uint8_t> fb(1280 * 720 * 4, 0);
  carview::ui::rasterize(l.items(), fb, 1280, 720);
  size_t lit = 0;
  for (size_t i = 3; i < fb.size(); i += 4) lit += fb[i] > 0;
  check(lit > 25000, "ui4_chrome primitives rasterize cleanly at 1280x720");
}

// Loads a backdrop PNG and scales it bilinearly into 1280x720 RGBA8, or builds
// a warm Colorado sky-to-road gradient if the PNG is absent.
std::vector<uint8_t> load_or_build_backdrop(const std::filesystem::path& png_path, bool studio = false) {
  constexpr uint32_t W = 1280, H = 720;
  std::vector<uint8_t> out(size_t(W) * H * 4, 255);
  if (!png_path.empty() && std::filesystem::exists(png_path)) {
    int sw = 0, sh = 0, sc = 0;
    if (uint8_t* px = stbi_load(png_path.string().c_str(), &sw, &sh, &sc, 4)) {
      for (uint32_t y = 0; y < H; ++y) {
        const float fy = (float(y) + 0.5f) * float(sh) / float(H) - 0.5f;
        const int y0 = std::clamp(int(std::floor(fy)), 0, sh - 1);
        const int y1 = std::min(y0 + 1, sh - 1);
        const float ty = fy - float(y0);
        for (uint32_t x = 0; x < W; ++x) {
          const float fx = (float(x) + 0.5f) * float(sw) / float(W) - 0.5f;
          const int x0 = std::clamp(int(std::floor(fx)), 0, sw - 1);
          const int x1 = std::min(x0 + 1, sw - 1);
          const float tx = fx - float(x0);
          const uint8_t* p00 = &px[(size_t(y0) * size_t(sw) + size_t(x0)) * 4];
          const uint8_t* p10 = &px[(size_t(y0) * size_t(sw) + size_t(x1)) * 4];
          const uint8_t* p01 = &px[(size_t(y1) * size_t(sw) + size_t(x0)) * 4];
          const uint8_t* p11 = &px[(size_t(y1) * size_t(sw) + size_t(x1)) * 4];
          uint8_t* dst = &out[(size_t(y) * W + x) * 4];
          for (int c = 0; c < 3; ++c) {
            const float top = p00[c] * (1.0f - tx) + p10[c] * tx;
            const float bot = p01[c] * (1.0f - tx) + p11[c] * tx;
            dst[c] = uint8_t(std::clamp(std::lround(top * (1.0f - ty) + bot * ty), 0L, 255L));
          }
          dst[3] = 255;
        }
      }
      stbi_image_free(px);
      return out;
    }
  }
  for (uint32_t y = 0; y < H; ++y) {
    const float t = float(y) / float(H - 1);
    for (uint32_t x = 0; x < W; ++x) {
      const float dx = (float(x) / float(W) - 0.5f);
      const float vig = std::clamp(1.0f - 0.45f * dx * dx, 0.55f, 1.0f);
      uint8_t r, g, b;
      if (studio) {
        r = uint8_t((32.0f + 26.0f * (1.0f - t)) * vig);
        g = uint8_t((36.0f + 28.0f * (1.0f - t)) * vig);
        b = uint8_t((46.0f + 34.0f * (1.0f - t)) * vig);
      } else {
        r = uint8_t((66.0f + 78.0f * (1.0f - t)) * vig);
        g = uint8_t((82.0f + 74.0f * (1.0f - t)) * vig);
        b = uint8_t((76.0f + 82.0f * (1.0f - t)) * vig);
      }
      uint8_t* dst = &out[(size_t(y) * W + x) * 4];
      dst[0] = r, dst[1] = g, dst[2] = b, dst[3] = 255;
    }
  }
  return out;
}

void composite_rgba(std::vector<uint8_t>& dst, uint32_t dw, uint32_t dh, const carview::HudImage& src, int ox, int oy) {
  if (src.rgba.empty() || src.width == 0 || src.height == 0) return;
  for (uint32_t y = 0; y < src.height; ++y) {
    const int dy = oy + int(y);
    if (dy < 0 || dy >= int(dh)) continue;
    for (uint32_t x = 0; x < src.width; ++x) {
      const int dx = ox + int(x);
      if (dx < 0 || dx >= int(dw)) continue;
      const uint8_t* s = &src.rgba[(size_t(y) * src.width + x) * 4];
      uint8_t* d = &dst[(size_t(dy) * dw + size_t(dx)) * 4];
      const float a = float(s[3]) / 255.0f;
      for (int c = 0; c < 3; ++c) d[c] = uint8_t(std::lround(s[c] * a + d[c] * (1.0f - a)));
      d[3] = 255;
    }
  }
}

void generate_screenshots(const std::filesystem::path& game_root, const std::filesystem::path& out_dir,
                          const std::filesystem::path& repo_root) {
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);

  // Load authentic disc vector fonts.
  if (auto fzip = fh1::Archive::open(game_root / "media/ui/Fonts.zip", nullptr)) {
    auto vf_load = [&](const char* nm) {
      auto r = fzip->read(nm);
      auto f = std::make_shared<fh1::VFont>();
      if (r.ok() && fh1::parse_vfont(r.data, *f, nullptr)) return f;
      return std::shared_ptr<fh1::VFont>{};
    };
    carview::ui::set_vector_fonts(vf_load("aru_vector_aa.dt"), vf_load("bru_vector_aa.dt"),
                                  vf_load("eru_vector_aa.dt"), vf_load("dru_vector_aa.dt"));
  }

  auto uizip = fh1::Archive::open(game_root / "media/UI.zip", nullptr);
  auto hz = fh1::Archive::open(game_root / "media/ui/textures/Horizon.zip", nullptr);
  auto tx = fh1::Archive::open(game_root / "media/ui/Textures.zip", nullptr);
  std::map<std::string, std::unique_ptr<fh1::UiScene>> scene_cache;
  std::set<std::string> failed_tex;

  carview::set_ui_textures([&](const std::string& path, std::array<float, 4>& uv) -> bool {
    if (carview::ui::atlas_find(path, uv)) return true;
    if (failed_tex.count(path)) return false;
    const bool t = path.rfind("T:", 0) == 0;
    fh1::Archive* a = t ? tx.get() : hz.get();
    if (!a) {
      failed_tex.insert(path);
      return false;
    }
    auto r = a->read((t ? path.substr(2) : path) + ".xds");
    if (!r.ok()) {
      failed_tex.insert(path);
      return false;
    }
    const auto x = fh1::decode_xds(r.data, false);
    if (x.levels.empty() || !carview::ui::atlas_image(path, x.levels[0].width, x.levels[0].height, x.levels[0].rgba, uv)) {
      failed_tex.insert(path);
      return false;
    }
    return true;
  });

  carview::set_ui_scenes([&](const std::string& name) -> const fh1::UiScene* {
    if (auto it = scene_cache.find(name); it != scene_cache.end()) return it->second.get();
    if (!uizip) return nullptr;
    auto fbf = uizip->read("Scenes/ui4/" + name + ".fbf");
    auto bsg = uizip->read("Scenes/ui4/" + name + ".bsg");
    auto bgf = uizip->read("Scenes/ui4/" + name + ".bgf");
    auto sc = std::make_unique<fh1::UiScene>();
    if (fbf.ok() && bsg.ok() && fh1::parse_ui_fbf(fbf.data, *sc, nullptr) && fh1::parse_ui_bsg(bsg.data, *sc, nullptr)) {
      if (bgf.ok()) sc->has_bgf = fh1::parse_ui_bgf(bgf.data, sc->bgf, nullptr);
      return (scene_cache[name] = std::move(sc)).get();
    }
    return nullptr;
  });

  if (hz) {
    for (const auto& [k, m] : {std::pair{"tape.smudge", "DIRTMASKS/SMUDGETAPE01.xds"},
                               std::pair{"tape.crisp", "DIRTMASKS/CRISPTAPE01.xds"}}) {
      auto r = hz->read(m);
      const auto x = r.ok() ? fh1::decode_xds(r.data, false) : fh1::XdsResult{};
      std::array<float, 4> uv{};
      if (!x.levels.empty()) carview::ui::atlas_image(k, x.levels[0].width, x.levels[0].height, x.levels[0].rgba, uv);
    }
  }

  constexpr uint32_t W = 1280, H = 720;
  const auto world_bg_path = repo_root / "out/play/r34_00000.png";
  const auto studio_bg_path = repo_root / "out/play/1006.png";
  auto save_frame = [&](const std::string& filename, const carview::ui::List& list, std::vector<uint8_t> fb) {
    carview::ui::rasterize(list.items(), fb, W, H);
    const auto p = out_dir / filename;
    check(stbi_write_png(p.string().c_str(), int(W), int(H), 4, fb.data(), int(W * 4)) != 0, filename.c_str());
  };

  // ---------------- GROUP 1 SCREENSHOTS ----------------

  // 1. ui4_pause.png — Authentic 925_PAUSE_MENU over Colorado R34 frame
  {
    carview::DriveHud d;
    d.driving = false;
    d.pause = true;
    d.menu_title = "paused";
    d.menu = {"MAP", "EVENTS", "PHOTO MODE", "GARAGE", "UPGRADES", "MY PROFILE", "SETTINGS", "TITLE SCREEN"};
    d.menu_selected = 0;
    d.band_name = "BLUE WRISTBAND";
    d.band_next = "BLUE";
    d.band_level = 3;
    d.band_collect_ready = true;
    d.car_name = "Nissan Skyline GT-R V-spec II";
    d.car_class = "B";
    d.car_pi = 461;
    d.credits = 117700;
    d.popularity_rank = 166;
    carview::ui::List l;
    carview::build_drive_hud(l, d, float(W), float(H));
    save_frame("ui4_pause.png", l, load_or_build_backdrop(world_bg_path));
  }

  // 2. ui4_map.png — Authentic H_MAP with real Colorado roads, high-res map & FH1 chrome
  {
    carview::MapView mv;
    std::vector<uint8_t> bg = load_or_build_backdrop({}, true);
    if (mv.open(game_root, nullptr)) {
      carview::MapScene ms;
      const auto ctr = mv.roads_centre();
      ms.x = ctr[0] + 150.0;
      ms.z = ctr[1] - 120.0;
      ms.heading = 0.65;
      ms.reveal = true;
      ms.markers = {
          {carview::MapMarker::kFestivalSite, "racecentral", "festival", "HORIZON FESTIVAL", "RACE CENTRAL", ctr[0], ctr[1], true, false, true, 0},
          {carview::MapMarker::kOutpost, "gas_station", "redrock", "RED ROCK OUTPOST", "HORIZON OUTPOST  •  PR STUNTS 2/3", ctr[0] - 850, ctr[1] + 500, false, false, true, 1},
          {carview::MapMarker::kEvent, "race", "FR06", "RED ROCK RAMPAGE", "FESTIVAL RACE  •  CLASS B", ctr[0] + 420, ctr[1] - 320, true, false, true, 0},
          {carview::MapMarker::kStreetHub, "streetrace", "hub1", "CARSON STREET HUB", "STREET RACES", ctr[0] - 420, ctr[1] - 640, false, false, true, 1},
      };
      ms.selected_marker = 1;
      ms.gps_route = {{ms.x, ms.z}, {ctr[0] - 250, ctr[1] + 180}, {ctr[0] - 850, ctr[1] + 500}};
      ms.has_gps_dest = true;
      ms.gps_dest_x = ctr[0] - 850;
      ms.gps_dest_z = ctr[1] + 500;
      const auto map_img = mv.pause_map(ms, int(W), int(H), 1.6, ctr[0] - 150, ctr[1] + 80);
      if (map_img.width == W && map_img.height == H) bg = map_img.rgba;
    }
    carview::MapOverlayHud mo;
    mo.active = true;
    mo.title = "RED ROCK OUTPOST";
    mo.subtitle = "HORIZON OUTPOST  •  PR STUNTS 2 / 3 COMPLETED";
    mo.action_hint = "FAST TRAVEL";
    mo.gps_hint = "CLEAR GPS ROUTE";
    mo.gps_active = true;
    mo.distance_m = 1480.0;
    mo.active_filter = 0;
    mo.filter_tabs = {"ALL", "EVENTS", "STREET", "OUTPOSTS", "PR STUNTS"};
    mo.roads_discovered = 142;
    mo.roads_total = 216;
    mo.zoom = 2.2;
    mo.treasure_map = true;
    carview::DriveHud d;
    d.driving = false;
    d.map_overlay = &mo;
    carview::ui::List l;
    carview::build_drive_hud(l, d, float(W), float(H));
    save_frame("ui4_map.png", l, std::move(bg));
  }

  // 3. ui4_outpost.png — Regional Outpost Hub over Colorado R34 frame
  {
    carview::DriveHud d;
    d.driving = false;
    d.pause = false;
    d.menu_title = "RED ROCK OUTPOST: REGIONAL HORIZON HUB";
    d.menu = {"PR STUNT: CANYON SPEED TRAP", "PR STUNT: RED ROCK DRIFT RUN", "PR STUNT: PHOTO OP - ARCHWAY",
              "CHANGE CAR (MY GARAGE)", "NEARBY EVENTS", "FAST TRAVEL TO HORIZON FESTIVAL"};
    d.menu_selected = 0;
    d.detail_title = "CANYON SPEED TRAP (PR STUNT)";
    d.detail_lines = {
        "TARGET SPEED: 155 MPH PAST RED ROCK CAMERA",
        "TIME LIMIT: 01:30.00",
        "REWARD: +12,500 CR  •  +2,500 POPULARITY",
        "READY TO LAUNCH FROM THIS OUTPOST",
        "OUTPOST PROGRESS: 2 / 3 PR STUNTS COMPLETED",
        "DISCOVERED: FREE FAST TRAVEL UNLOCKED",
    };
    d.band_name = "BLUE WRISTBAND";
    d.band_next = "PURPLE";
    d.band_points = 12450;
    d.band_hi = 15000;
    d.band_level = 3;
    d.car_name = "Nissan Skyline GT-R V-spec II";
    d.car_class = "B";
    d.car_pi = 461;
    d.credits = 117700;
    d.popularity_rank = 166;
    carview::ui::List l;
    carview::build_drive_hud(l, d, float(W), float(H));
    save_frame("ui4_outpost.png", l, load_or_build_backdrop(world_bg_path));
  }

  // 4. ui4_hud_minimap.png — Free-roam 947_HUD + borderless feathered minimap + GPS + PR Stunt + Radio + Skill Chain
  {
    auto bg = load_or_build_backdrop(world_bg_path);
    carview::MapView mv;
    if (mv.open(game_root, nullptr)) {
      carview::MapScene ms;
      const auto ctr = mv.roads_centre();
      ms.x = ctr[0] + 100;
      ms.z = ctr[1] - 80;
      ms.heading = 0.35;
      ms.speed_mps = 38.0;
      ms.gps_route = {{ms.x, ms.z}, {ms.x + 120, ms.z + 220}, {ms.x + 280, ms.z + 450}};
      ms.markers = {{carview::MapMarker::kEvent, "race", "FR06", "MEDIA CENTER", "", ms.x + 30, ms.z + 90, true, false, true, 0}};
      const auto mm = mv.minimap(ms, 224);
      composite_rgba(bg, W, H, mm, 76, 412);
    }
    carview::GpsHud gps;
    gps.active = true;
    gps.turn_kind = 6;
    gps.turn_text = "TURN RIGHT";
    gps.dest_label = "RED ROCK OUTPOST";
    gps.dist_to_turn_m = 240.0;
    gps.dist_to_dest_m = 1480.0;

    carview::PrStuntHud ps;
    ps.active = true;
    ps.kind_badge = "SPEED STUNT";
    ps.title = "RED ROCK CANYON RUN";
    ps.instruction = "HIT 155 MPH THROUGH THE CANYON SPEED CAMERA";
    ps.status_line = "CURRENT 142 MPH  •  13 MPH TO TARGET";
    ps.time_remaining_s = 48.3;
    ps.progress = 0.78;
    ps.in_zone = false;

    carview::SkillChainHud sk;
    sk.active = true;
    sk.chain_points = 1450;
    sk.multiplier = 3;
    sk.last_skill = "GREAT DRIFT";
    sk.last_points = 500;
    sk.popularity_rank = 166;
    sk.popularity_progress = 0.62;

    carview::WorldMarkerHud wm;
    wm.visible = true;
    wm.in_range = true;
    wm.screen_x = 510;
    wm.screen_y = 245;
    wm.title = "MEDIA CENTER";
    wm.subtitle = "FESTIVAL RACE  •  CLASS B";
    wm.prompt = "ENTER NOW";
    wm.badge = "NEW";

    carview::DriveHud d;
    d.driving = true;
    d.kmh = 142.0;
    d.rpm = 6400.0;
    d.redline_rpm = 8000.0;
    d.gear = 4;
    d.abs = true;
    d.tcs = false;
    d.time = 12.4;
    d.gps = &gps;
    d.pr_stunt = &ps;
    d.skills = &sk;
    d.world_marker = &wm;
    d.driver_level = 14;
    d.driver_xp = 28400;
    d.affinity_level = 6;

    carview::RadioPopupGpuState rad;
    rad.station_name = "Radio1";
    rad.station_label = "HORIZON BASS ARENA";
    rad.title = "Language";
    rad.artist = "Porter Robinson";
    rad.alpha = 1.0f;

    carview::ui::List l;
    carview::build_drive_hud(l, d, float(W), float(H));
    carview::draw_radio_popup_gpu(l, rad, float(W), float(H));
    save_frame("ui4_hud_minimap.png", l, std::move(bg));
  }

  // ---------------- GROUP 2 SCREENSHOTS ----------------

  // Load real disc PartsDb + ClassBands + PhysicsSettings + PiConfig for authentic shop screens!
  cars::Install install;
  cars::PartsDb db;
  std::vector<garage::ClassBand> bands;
  cars::PhysicsSettings phys_settings;
  garage::PiConfig pi_cfg;
  std::string db_err;
  const bool have_db = cars::discover_install(game_root, {}, {}, install, &db_err) && db.open(install, &db_err);
  if (have_db) {
    garage::load_class_bands(game_root, bands, &db_err);
    cars::load_physics_settings(install.media / "physics.zip", phys_settings, &db_err);
    garage::load_pi_config(install.media / "physics.zip", pi_cfg, &db_err);
    // Register real localized display names via race::CarNames (StringTables + Data_Car).
    auto car_names = std::make_shared<race::CarNames>();
    std::string cn_err;
    car_names->load(game_root, {}, &cn_err, "en", &db);
    carview::set_car_names([car_names](const std::string& media) -> std::string {
      return car_names->short_name(media);
    });
  }

  // Helper to buy a real base-game car from dealer catalog into a garage.
  std::vector<cars::DealerCar> dealer_cars;
  if (have_db) db.dealer(dealer_cars, nullptr);
  auto add_dealer_car = [&](garage::Garage& g, int64_t car_id, int64_t& credits) -> int64_t {
    const cars::DealerCar* found = nullptr;
    for (const auto& dc : dealer_cars) {
      if (dc.id == car_id) {
        found = &dc;
        break;
      }
    }
    if (!found) return 0;
    std::map<std::string, int64_t> slots;
    if (!db.stock_slots(found->id, slots, nullptr)) return 0;
    int64_t gid = 0;
    if (!garage::buy_car(g, found->id, found->base_cost, found->performance_index, found->class_id,
                         int(found->first_colour), slots, credits, &gid)) {
      return 0;
    }
    return gid;
  };

  // 5. ui4_autoshow.png — Authentic FH1 Autoshow Make Carousel + Car Tiles + 5-Stat & Swatch Card
  if (have_db) {
    carview::Autoshow as;
    std::string why;
    if (as.open(db, {}, why, 500000, false)) {
      // Pick Nissan (or first make with multiple cars & logos).
      for (size_t mi = 0; mi < as.makes().size(); ++mi) {
        if (as.makes()[mi].name == "Nissan" || as.makes()[mi].name == "NIS") {
          as.select_make(mi, 500000);
          break;
        }
      }
      if (as.visible_cars().size() > 2) as.move(1);
      carview::ui::List l;
      carview::draw_autoshow_hud(l, float(W), float(H), as, bands, 500000);
      save_frame("ui4_autoshow.png", l, load_or_build_backdrop(studio_bg_path, true));
    }
  }

  // 6. ui4_garage.png — Authentic FH1 My Garage Horizontal Car-Card Carousel
  if (have_db) {
    garage::Garage g;
    int64_t gcred = 5000000;
    // Populate a rich realistic 6-car Horizon garage (Corrado 309, R34 445, plus 4 varied makes from catalog).
    static constexpr int64_t kPreferredCars[] = {309, 445, 1323, 1009, 1006, 1293};
    for (int64_t cid : kPreferredCars) add_dealer_car(g, cid, gcred);
    for (size_t i = 0; i < dealer_cars.size() && g.cars.size() < 6; i += 17) {
      add_dealer_car(g, dealer_cars[i].id, gcred);
    }
    if (g.cars.size() >= 2) g.selected = g.cars[1].id;
    carview::GarageBrowser gb;
    gb.refresh(g, &db, bands);
    if (gb.visible().size() > 1) gb.move(1);
    carview::ui::List l;
    carview::draw_garage_hud(l, float(W), float(H), gb, bands, 248500);
    save_frame("ui4_garage.png", l, load_or_build_backdrop(studio_bg_path, true));
  }

  // 7. ui4_upgrades_tuning.png — Authentic FH1 Dak's Garage Upgrade Shop + Live PI & 5-Stat Diff
  if (have_db) {
    garage::Garage g;
    int64_t gcred = 5000000;
    const int64_t gid = add_dealer_car(g, 445, gcred);  // Nissan Skyline GT-R V-spec II
    carview::UpgradeShop shop;
    std::string why;
    if (gid > 0 && shop.open(db, g, gid, why, &phys_settings, &pi_cfg, &bands)) {
      shop.move_part(+2);  // hover Sport/Race upgrade so green live PI & 5-stat gain diff shows!
      carview::ui::List l;
      carview::draw_upgrade_shop_hud(l, float(W), float(H), shop, bands, 25, 248500, "Nissan Skyline GT-R V-spec II");
      save_frame("ui4_upgrades_tuning.png", l, load_or_build_backdrop(studio_bg_path, true));

      // Also verify Tuning tab renders cleanly without crashing.
      shop.set_category(carview::ShopCategory::kTuning);
      carview::ui::List lt;
      carview::draw_upgrade_shop_hud(lt, float(W), float(H), shop, bands, 25, 248500, "Nissan Skyline GT-R V-spec II");
      check(lt.items().size() > 25, "UpgradeShop Tuning tab emits FH1 slider instances");
    }
  }

  // 8. ui4_paint_vinyl.png — Authentic FH1 Paint Shop Region Icons + 12x7 HSV Swatch Grid
  if (have_db) {
    carview::LiveryEditor ed;
    std::string msg;
    if (ed.open(&db, game_root, 445, {}, msg)) {
      ed.move_finish(+1);  // Metallic
      ed.step_hsv(0.58f, 0.10f, 0.0f);  // Bayside Blue
      carview::ui::List l;
      carview::draw_livery_editor_hud(l, float(W), float(H), ed, "Nissan Skyline GT-R V-spec II");
      save_frame("ui4_paint_vinyl.png", l, load_or_build_backdrop(studio_bg_path, true));

      // Also verify Special Colors & Vinyl Layer Stack tabs render cleanly.
      ed.add_vinyl_layer("", 0xF5045Cu);
      ed.add_vinyl_layer("", 0xFFFFFFu);
      ed.set_tab(carview::EditorTab::kVinylLayers);
      carview::ui::List lv;
      carview::draw_livery_editor_hud(lv, float(W), float(H), ed, "Nissan Skyline GT-R V-spec II");
      check(lv.items().size() > 20, "LiveryEditor Vinyl Layers tab emits FH1 stack instances");
    }
  }
}

}  // namespace

int main() {
  test_ui4_chrome_primitives();

  const char* root_env = std::getenv("SHIFT_GAME_ROOT");
  const char* shot_env = std::getenv("SHIFT_UI4_SCREENSHOT_DIR");
  const char* repo_env = std::getenv("SHIFT_REPO_ROOT");
  if (root_env && *root_env && shot_env && *shot_env) {
    generate_screenshots(root_env, shot_env, repo_env ? std::filesystem::path(repo_env) : std::filesystem::current_path());
  }

  if (g_failures > 0) {
    std::fprintf(stderr, "ui4_parity_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("ui4_parity_tests: PASS\n");
  return 0;
}
