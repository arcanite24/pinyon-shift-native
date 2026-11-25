// The title update's screens against the player's game files (LABELS
// corpus; skipped, exit 77, without SHIFT_GAME_ROOT and SHIFT_STATE_ROOT):
// the 1000 Club's scenes load through the v4 overlay with every node the
// screens bind, every string they bind resolves in the EN tables with the
// packages, the packages' pause rows resolve, and every club car has the
// challenges the screens have rows for. With SHIFT_CLUB_SHOT_DIR the screens
// are drawn to PNGs there (over SHIFT_CLUB_BACKDROP_DIR/<name>.png, the
// original's captures, when given: the calibration loop).
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "cars/physics.h"
#include "cars/sources.h"
#include "cars/strings.h"
#include "garage/rally_kit.h"
#include "club_menu.h"
#include "fh1data/archive.h"
#include "fh1data/stringtables.h"
#include "fh1data/ui_overlay.h"
#include "fh1data/ui_scene.h"
#include "fh1data/xds.h"
#include "freeroam/club.h"
#include "freeroam/speed_zones.h"
#include "game_hud.h"
#include "hud.h"
#include "hud_scene.h"
#include "hud_screen.h"
#include "pause_screen.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include "ui.h"

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

void check_ref(const carview::StringRef& r, const std::string& what) {
  const auto t = carview::lookup(r);
  check(t.has_value() && !t->empty(), what + " resolves (" + std::string(r.table) + "/" + std::string(r.id) + ")");
}

// The packages' loose UI texture folders (as the drive finds them).
std::vector<std::filesystem::path> dlc_texture_dirs(const std::filesystem::path& state_root) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  const auto r = state_root / "user" / "0000000000000000" / "4D5309C9" / "00000002";
  if (!std::filesystem::is_directory(r, ec)) return out;
  for (const auto& pkg : std::filesystem::directory_iterator(r, ec)) {
    const auto zips = pkg.path() / "Media" / "DLCZips";
    if (!std::filesystem::is_directory(zips, ec)) continue;
    for (const auto& z : std::filesystem::directory_iterator(zips, ec)) {
      const auto d = z.path() / "Media" / "UI" / "Textures" / "Horizon";
      if (std::filesystem::is_directory(d, ec)) out.push_back(d);
    }
  }
  return out;
}

}  // namespace

int main() {
  const char* root_env = std::getenv("SHIFT_GAME_ROOT");
  const char* state_env = std::getenv("SHIFT_STATE_ROOT");
  if (!root_env || !*root_env || !state_env || !*state_env) {
    std::printf("club_rally_corpus_tests: SKIP (SHIFT_GAME_ROOT and SHIFT_STATE_ROOT not set)\n");
    return 77;
  }
  const std::filesystem::path game_root = root_env, state_root = state_env;

  // 1. The scenes through the v4 overlay (the drive's loader).
  auto overlay = std::make_shared<fh1::UiOverlay>(game_root, fh1::UiOverlay::v4_media_zip_of(state_root));
  check(overlay->has_v4(), "the title update's media.zip opens");
  std::map<std::string, std::unique_ptr<fh1::UiScene>> scenes;
  carview::set_ui_scenes([&](const std::string& name) -> const fh1::UiScene* {
    auto& slot = scenes[name];
    if (!slot) {
      auto sc = std::make_unique<fh1::UiScene>();
      std::string e;
      if (overlay->load_scene(name, *sc, &e))
        slot = std::move(sc);
      else
        std::fprintf(stderr, "scene %s: %s\n", name.c_str(), e.c_str());
    }
    return slot.get();
  });
  carview::install_scene_help_bar();
  for (const auto& [scene, nodes] : carview::club_screen_nodes()) {
    const carview::SceneDraw sd(carview::ui_scene(std::string(scene)));
    check(sd.ok(), std::string(scene) + " loads");
    if (!sd.ok()) continue;
    for (const char* p : nodes) {
      const std::string_view path(p);
      const int n = path.find('/') != std::string_view::npos ? sd.find_path(path) : sd.find(path);
      check(n >= 0, std::string(scene) + ": node " + p);
    }
  }

  // 2. The strings (disc, v4 and the packages, EN).
  cars::Install inst;
  std::string why;
  check(cars::discover_install(game_root, state_root, {}, inst, &why), "discover_install: " + why);
  // The Horizon Rally cars the achievements 208 and 215 count: the five with
  // OptionalRallyUpgrades rows (V4_CONTENT: Escort, 037, Celica, Lancer, WRX),
  // not the event-only twins.
  {
    cars::PartsDb db;
    check(db.open(inst, &why), "parts db: " + why);
    std::vector<int64_t> ids;
    for (int64_t id = 1; id <= 4000; ++id)
      if (garage::is_horizon_rally_car(db, id)) ids.push_back(id);
    check(ids.size() == 5, "five Horizon Rally cars: " + std::to_string(ids.size()));
    for (int64_t id : ids) check(id < 1626 || id > 1635, "twin " + std::to_string(id) + " is no Rally car");
  }
  fh1::StringTables tables;
  check(cars::load_string_tables(inst, tables, &why, "EN"), "EN string tables load");
  carview::set_game_text([&](const std::string& v) -> std::string {
    if (auto t = tables.resolve(v)) return *t;
    return v;
  });
  for (const auto& r : carview::club_text_refs()) check_ref(r, "club text " + std::string(r.fallback));
  for (const auto& r : carview::rally_entry_text_refs()) check_ref(r, "rally entry " + std::string(r.id));
  using PM = carview::PauseMenu;
  check_ref(carview::pause_row_text(PM::kClub, false), "pause row 1000 CLUB");
  check_ref(carview::pause_row_text(PM::kRally, false), "pause row HORIZON RALLY");
  check(carview::text_of(carview::pause_row_text(PM::kClub, false)) == "1000 CLUB", "1000 CLUB row text");
  check(carview::text_of(carview::pause_row_text(PM::kRally, false)) == "HORIZON RALLY", "HORIZON RALLY row text");
  // The achievements' descriptions with their counts in place.
  {
    freeroam::club::ClubMedalStats st;
    const auto lines = carview::club_achievement_lines(st);
    check(carview::club_achievement_desc(lines[8]).find("13") != std::string::npos, "Ford description has 13");
    // The tables leave the count out of IDS_CarsRequired (" NEW CARS
    // REQUIRED"); the screens put it in front.
    check(carview::club_required_text(lines[3]) == "3 NEW CARS REQUIRED",
          "Shelby: 3 required (" + carview::club_required_text(lines[3]) + ")");
    check(carview::club_required_text(lines[1]) == "14 NEW CARS REQUIRED",
          "Ferrari: 14 required (" + carview::club_required_text(lines[1]) + ")");
    check(carview::club_required_text(lines[0]) == "1 MEDAL REQUIRED",
          "first medal: 1 required (" + carview::club_required_text(lines[0]) + ")");
  }

  // 3. The package's challenges fit the screens' rows (6 at most a car).
  freeroam::club::ClubData data;
  check(freeroam::club::load_club(state_root.string(), data, &why), "1000 Club data loads: " + why);
  size_t over = 0, empty = 0;
  for (const auto& [car, list] : data.cars) {
    over += list.size() > 6;
    empty += list.empty();
  }
  check(!data.cars.empty() && over == 0 && empty == 0, "every club car has 1..6 challenges");
  // The pause rows' packages are found (offers 1600 and 2700).
  bool rally = false, club = false;
  for (const auto& p : cars::list_packs(state_root)) {
    rally = rally || (p.puboffer == 1600 && p.owned());
    club = club || (p.puboffer == 2700 && p.owned());
  }
  check(rally && club, "the Rally and 1000 Club packages are owned");

  // ColoradoDirt's own speed traps and zones (the Rally package's gamemodes,
  // Colorado's gates): 22 and 9.
  {
    std::filesystem::path dirt_dir;
    std::error_code ec;
    const auto pkgs = state_root / "user" / "0000000000000000" / "4D5309C9" / "00000002";
    if (std::filesystem::is_directory(pkgs, ec))
      for (const auto& pkg : std::filesystem::directory_iterator(pkgs, ec))
        if (dirt_dir.empty()) dirt_dir = freeroam::colorado_dirt_gamemodes(pkg.path());
    check(!dirt_dir.empty(), "the Rally package has ColoradoDirt's speed_camera.xml");
    freeroam::SpeedActivitiesData dirt;
    std::string serr;
    check(freeroam::load_speed_activities(game_root, dirt, &serr, dirt_dir), "ColoradoDirt speed activities: " + serr);
    check(dirt.traps.size() == 22 && dirt.zones.size() == 9, "ColoradoDirt: 22 traps and 9 zones (" +
                                                                 std::to_string(dirt.traps.size()) + ", " +
                                                                 std::to_string(dirt.zones.size()) + ")");
    // Against the disc's Colorado set: the ids, scoreboards and minimum
    // speeds that differ.
    freeroam::SpeedActivitiesData base;
    check(freeroam::load_speed_activities(game_root, base, &serr), "Colorado speed activities: " + serr);
    int differ = 0;
    for (const auto& t : dirt.traps) {
      const auto* b = base.find_trap(t.id);
      if (!b || b->min_speed_mph != t.min_speed_mph || b->scoreboard_id != t.scoreboard_id) ++differ;
    }
    for (const auto& z : dirt.zones) {
      const auto* b = base.find_zone(z.id);
      if (!b || b->min_speed_mph != z.min_speed_mph || b->scoreboard_id != z.scoreboard_id) ++differ;
    }
    // Found: the package's set names the same 22 traps and 9 zones with the
    // same scoreboards and minimum speeds as the disc's (SPEC_QUESTIONS Q146).
    check(differ == 0, "ColoradoDirt's traps and zones match Colorado's (" + std::to_string(differ) + " differ)");
  }

  // 4. Optional renders.
  if (const char* shot_env = std::getenv("SHIFT_CLUB_SHOT_DIR"); shot_env && *shot_env) {
    const std::filesystem::path out_dir = shot_env;
    const char* bd = std::getenv("SHIFT_CLUB_BACKDROP_DIR");
    const std::filesystem::path backdrop_dir = bd ? bd : "";
    std::filesystem::create_directories(out_dir);
    if (auto fzip = fh1::Archive::open(game_root / "media" / "ui" / "Fonts.zip", nullptr))
      carview::ui::load_fonts_from_zip(*fzip);
    const auto dlc = dlc_texture_dirs(state_root);
    std::set<std::string> failed;
    carview::set_ui_textures([&](const std::string& path, std::array<float, 4>& uv) -> bool {
      if (carview::ui::atlas_find(path, uv)) return true;
      if (failed.count(path)) return false;
      fh1::UiOverlay::File f = overlay->texture(path);
      std::vector<uint8_t> bytes = f.ok() ? std::move(f.read.data) : std::vector<uint8_t>{};
      if (bytes.empty() && path.rfind("T:", 0) != 0) {
        std::error_code ec;
        for (const auto& d : dlc) {
          const auto p = d / (path + ".xds");
          if (!std::filesystem::is_regular_file(p, ec)) continue;
          std::ifstream in(p, std::ios::binary);
          bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
          break;
        }
      }
      const auto x = !bytes.empty() ? fh1::decode_xds(bytes, false) : fh1::XdsResult{};
      if (x.levels.empty() ||
          !carview::ui::atlas_image(path, x.levels[0].width, x.levels[0].height, x.levels[0].rgba, uv)) {
        failed.insert(path);
        return false;
      }
      return true;
    });
    constexpr uint32_t W = 1280, H = 720;
    auto backdrop = [&](const std::string& name) {
      std::vector<uint8_t> fb(size_t(W) * H * 4, 0);
      for (size_t i = 0; i < fb.size(); i += 4) fb[i] = 70, fb[i + 1] = 92, fb[i + 2] = 64, fb[i + 3] = 255;
      if (!backdrop_dir.empty()) {
        int w = 0, h = 0, n = 0;
        if (uint8_t* px = stbi_load((backdrop_dir / (name + ".png")).string().c_str(), &w, &h, &n, 4)) {
          if (w == int(W) && h == int(H)) fb.assign(px, px + size_t(W) * H * 4);
          stbi_image_free(px);
        }
      }
      return fb;
    };
    carview::DriveHud d;
    d.band_name = "GREEN WRISTBAND";
    d.band_next = "BLUE";
    d.band_level = 3;
    d.band_collect_ready = true;
    d.car_name = "Mazda MX-5 Miata";
    d.car_class = "A";
    d.car_pi = 600;
    d.credits = 167700;
    // The capture's state: the first visit, the MX-5 driven, no medal.
    carview::ClubMenuData cd;
    cd.tutorial = 1;
    cd.achievements = carview::club_achievement_lines(cd.stats);
    const char* titles[5] = {"Near Misses", "Smash Signs", "Speed Zone", "Timed Road Trip", "Carson Street Race"};
    for (int i = 0; i < 3; ++i) {
      carview::ClubCarEntry e;
      e.car_id = i == 0 ? 1 : 100 + i;
      e.name = i == 0 ? "MX-5 Miata" : "Car " + std::to_string(i);
      e.year_make = i == 0 ? "1994 Mazda" : "2012 Make";
      e.class_letter = "A";
      e.pi = 600;
      e.medals.available = 5;
      e.counts.fill(true);
      for (const char* t : titles) e.challenge_titles.push_back(t), e.completed.push_back(false);
      e.current = i == 0;
      cd.garage.push_back(e);
    }
    cd.current = 0;
    auto shot = [&](const std::string& name, const carview::ClubMenu& m, const carview::ClubMenuData& data) {
      carview::ui::List l;
      check(carview::draw_club_menu(l, m, data, float(W), float(H), &d), "draws " + name);
      std::vector<uint8_t> fb = backdrop(name);
      carview::ui::rasterize(l.items(), fb, W, H);
      const auto p = out_dir / (name + ".png");
      check(stbi_write_png(p.string().c_str(), int(W), int(H), 4, fb.data(), int(W * 4)) != 0, "write " + name);
    };
    carview::ClubMenu m;
    m.show(cd);
    shot("club_tutorial", m, cd);
    m.popup = carview::ClubMenu::Popup::kNone;
    shot("club_home", m, cd);
    m.press(carview::ClubMenu::Button::kAccept, cd);
    shot("club_carlist_tut", m, cd);
    m.popup = carview::ClubMenu::Popup::kNone;
    shot("club_carlist", m, cd);
    m.press(carview::ClubMenu::Button::kAccept, cd);
    shot("club_select_action", m, cd);
    // After the first medal: the achievements and the current car's list.
    carview::ClubMenuData later = cd;
    later.tutorial = 4;
    later.stats.total_medals = 1;
    later.stats.cars_with_medal = 1;
    later.achievements = carview::club_achievement_lines(later.stats);
    later.current_begun = true;
    for (int i = 0; i < 5; ++i) {
      carview::ClubChallengeLine c;
      c.title = titles[i];
      c.description = "Get 10 near misses with traffic.";
      c.hint = "Busy roads around Carson have plenty of traffic.";
      c.completed = i == 0;
      later.challenges.push_back(c);
    }
    carview::ClubMenu h;
    h.show(later);
    shot("club_home_begun", h, later);
    h.screen = carview::ClubMenu::Screen::kAchievements;
    h.achievement = 1;
    shot("club_achievements", h, later);
    h.screen = carview::ClubMenu::Screen::kDetails;
    shot("club_details", h, later);
    // The driving HUD's items and prompts.
    carview::ClubHud hud;
    hud.details_hint = true;
    hud.instruction = carview::text_of(carview::club_text::kTutorialGoToDetails);
    carview::ClubHudItem it;
    it.number = 3;
    it.text = "Top Speed  120 mph";
    hud.items.push_back(it);
    it.number = 1;
    it.result = carview::ClubHudItem::Result::kPassed;
    it.result_age = 0.5;
    hud.items.push_back(it);
    {
      carview::ui::List l;
      check(carview::draw_club_hud(l, hud, float(W), float(H)), "draws the club HUD");
      std::vector<uint8_t> fb = backdrop("club_begin");
      carview::ui::rasterize(l.items(), fb, W, H);
      const auto p = out_dir / "club_begin.png";
      check(stbi_write_png(p.string().c_str(), int(W), int(H), 4, fb.data(), int(W * 4)) != 0, "write club_begin");
    }
    // The Rally entry's confirmation (rally_confirm.png) and gate.
    for (auto [p, name] : {std::pair{carview::RallyEntryPopup::kConfirm, "rally_confirm"},
                           std::pair{carview::RallyEntryPopup::kGate, "rally_gate"}}) {
      carview::ui::List l;
      check(carview::draw_rally_entry_popup(l, p, float(W), float(H)), std::string("draws ") + name);
      std::vector<uint8_t> fb = backdrop(name);
      carview::ui::rasterize(l.items(), fb, W, H);
      const auto path = out_dir / (std::string(name) + ".png");
      check(stbi_write_png(path.string().c_str(), int(W), int(H), 4, fb.data(), int(W * 4)) != 0,
            std::string("write ") + name);
    }
  }

  std::printf("club_rally_corpus_tests: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
