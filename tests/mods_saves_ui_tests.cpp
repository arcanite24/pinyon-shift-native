// Slice 13A UI tests: the F6 > MODS overlay (load order, switches, moves,
// options, the save-isolation indicator) and the PROFILE > SAVE BACKUPS &
// SAVE SYNC screen. With SHIFT_GAME_ROOT (or the sibling horizon1-recomp
// checkout) and SHIFT_UI4_SCREENSHOT_DIR set, also writes 1280x720 PNGs of
// both screens with the game's fonts and UI textures.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "cars/mod_manager.h"
#include "fh1data/archive.h"
#include "fh1data/vfont.h"
#include "fh1data/xds.h"
#include "mods_page.h"
#include "ui.h"
#include "ui4_chrome.h"
#if __has_include("save_sync_ui.h")
#include "save_sync_ui.h"
#define HAVE_SAVE_UI 1
#endif

namespace fs = std::filesystem;

namespace {

int g_failed = 0;
#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failed;                                                           \
    }                                                                       \
  } while (0)

fs::path g_root;

void write(const fs::path& p, const std::string& text) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << text;
}

size_t lit(const carview::ui::List& l) {
  std::vector<uint8_t> fb(1280u * 720u * 4u, 0);
  carview::ui::rasterize(l.items(), fb, 1280, 720);
  size_t n = 0;
  for (size_t i = 3; i < fb.size(); i += 4) n += fb[i] > 0;
  return n;
}

fs::path make_mods() {
  const fs::path root = g_root / "mods";
  write(root / "hd_colorado" / "mod.toml",
        "name = \"hd_colorado\"\ntitle = \"HD Colorado Textures\"\nauthor = \"TexturePack Crew\"\nversion = \"2.1\"\n"
        "description = \"4x replacement textures for Colorado's roads, rocks and festival signage.\"\n"
        "touches_save = false\npriority = 10\n");
  write(root / "hd_colorado" / "textures" / "0123456789abcdef.png", "x");
  write(root / "hd_colorado" / "textures" / "fedcba9876543210.dds", "x");
  write(root / "skyline_mod" / "mod.toml",
        "name = \"skyline_mod\"\ntitle = \"Skyline Mod Edition\"\nauthor = \"Neri\"\nversion = \"1.0\"\n"
        "description = \"Adds the Skyline Mod Edition to the Autoshow (a database patch reusing the R34 model).\"\n");
  write(root / "skyline_mod" / "db" / "01_car.sql", "SELECT 1;");
  write(root / "skyline_mod" / "stringtables" / "EN" / "CarNames.toml", "IDS_X = \"x\"\n");
  write(root / "pacing" / "mod.toml",
        "name = \"pacing\"\ntitle = \"Festival Pacing\"\nauthor = \"Pinyon Shift\"\nversion = \"1.0\"\n"
        "description = \"Prices and rewards multipliers.\"\n[options.rewards]\nlabel = \"REWARDS\"\n"
        "choices = [\"1x\", \"2x\", \"5x\"]\ndefault = 0\n");
  write(root / "pacing" / "xml_patches" / "gamedb.zip" / "rewards.xml", "<R/>");
  write(root / "hello_telemetry" / "mod.toml",
        "name = \"hello_telemetry\"\ntitle = \"Hello Telemetry\"\nversion = \"0.3\"\nlibrary = \"code/hello.dll\"\n"
        "touches_save = false\n");
  write(root / "old_patch" / "mod.toml", "name = \"old_patch\"\ntitle = \"Old Balance Patch\"\nrequires = [\"ghost_mod\"]\n");
  write(root / "xe" / "xe-install.json", "{\"version\": \"1.01\"}");
  write(root / "xe" / "game" / "media" / "db" / "gamedb.slt", "x");
  cars::modmgr::Config cfg;
  cfg.enabled = {{"xe", false}, {"pacing", false}};
  std::string err;
  cars::modmgr::write_config(cars::modmgr::config_path(root), cfg, &err);
  return root;
}

void test_mods_page(carview::ModsSession& session, carview::ModsPage& page) {
  const fs::path root = make_mods();
  const fs::path main = g_root / "drive";
  write(main / "save.txt", "shift-drive-save 1\ncar A\nposition 0 0 0\nheading 0\n");
  const auto dir = session.boot(root, main, cars::modmgr::GameBuild::kBase, {});
  session.activate({}, {}, {});
  // skyline_mod touches the save: the game plays the modded copy.
  CHECK(dir == main / "user-modded");
  CHECK(session.mods().size() == 6);
  page.open(session);
  CHECK(page.is_open());
  auto rows = page.rows();
  CHECK(rows.size() == 6);
  CHECK(rows[0].name == "hd_colorado");  // priority 10 first
  CHECK(page.save_label() == "MODDED COPY" && page.save_isolated());
  // Switch the save-changing mod off: the indicator says the next start
  // plays the player's own save.
  int sky = -1;
  for (size_t i = 0; i < rows.size(); ++i)
    if (rows[i].name == "skyline_mod") sky = int(i);
  CHECK(sky >= 0);
  while (page.selected() != sky) page.nav(carview::Nav::kDown);
  page.nav(carview::Nav::kAccept);
  CHECK(page.dirty() && page.save_label() == "YOUR SAVE" && !page.save_isolated());
  CHECK(page.save_detail().find("next start switches save") != std::string::npos ||
        page.save_detail().find("The next start switches save") != std::string::npos);
  page.nav(carview::Nav::kAccept);  // back on
  // Move it to the top of the load order.
  while (page.selected() > 0) page.nav(carview::Nav::kTabPrev);
  CHECK(page.rows()[0].name == "skyline_mod");
  // Options cycle with left / right.
  for (size_t i = 0; i < page.rows().size(); ++i)
    if (page.rows()[i].name == "pacing")
      while (page.selected() != int(i)) page.nav(carview::Nav::kDown);
  page.nav(carview::Nav::kRight);
  CHECK(page.config().options.at("pacing").at("rewards") == "2x");
  page.nav(carview::Nav::kAccept);  // switch pacing on
  // Blocked and native mods are labelled.
  bool blocked = false, native = false;
  for (const auto& r : page.rows()) {
    if (r.name == "old_patch") blocked = r.status == "BLOCKED" && r.reason == "needs ghost_mod";
    if (r.name == "hello_telemetry") native = r.native;
  }
  CHECK(blocked && native);
  CHECK(page.nav(carview::Nav::kX));  // asks for a texture reload
  std::string err;
  CHECK(page.save(&err) && !page.dirty());
  cars::modmgr::Config back;
  CHECK(cars::modmgr::read_config(cars::modmgr::config_path(root), back));
  CHECK(!back.order.empty() && back.order[0] == "skyline_mod" && back.enabled.at("pacing"));

  carview::ui::List l;
  std::set<std::string> requested;
  carview::ui4::set_texture_loader([&](const std::string& p, std::array<float, 4>& uv) {
    requested.insert(p);
    uv = {0.0f, 0.0f, 0.1f, 0.1f};
    return true;
  });
  carview::draw_mods_page(l, 1280, 720, page);
  carview::ui4::set_texture_loader({});
  CHECK(l.items().size() > 60);
  CHECK(lit(l) > 80000);
  CHECK(requested.count("DIRTMASKS/PILL02") > 0);
  CHECK(requested.count("DIRTMASKS/PANEL01") > 0);
  CHECK(carview::ui4::pretty_car_media_name("FER_F40_87") == "1987 Ferrari F40");
  CHECK(carview::ui4::pretty_car_media_name("NIS_SKYLINEGTRVSPECII_02") == "2002 Nissan Skyline GT-R V-Spec II");
  page.nav(carview::Nav::kBack);
  CHECK(!page.is_open());
  page.open(session);
}

std::vector<uint8_t> backdrop(const std::vector<fs::path>& candidates) {
  constexpr uint32_t W = 1280, H = 720;
  std::vector<uint8_t> out(size_t(W) * H * 4, 255);
  for (const auto& p : candidates) {
    int sw = 0, sh = 0, sc = 0;
    uint8_t* px = fs::exists(p) ? stbi_load(p.string().c_str(), &sw, &sh, &sc, 4) : nullptr;
    if (!px) continue;
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) {
        const int sx = std::min(sw - 1, int(x * uint32_t(sw) / W)), sy = std::min(sh - 1, int(y * uint32_t(sh) / H));
        std::copy_n(&px[(size_t(sy) * size_t(sw) + size_t(sx)) * 4], 3, &out[(size_t(y) * W + x) * 4]);
      }
    stbi_image_free(px);
    return out;
  }
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      const float t = float(y) / H;
      uint8_t* d = &out[(size_t(y) * W + x) * 4];
      d[0] = uint8_t(66 + 78 * (1 - t)), d[1] = uint8_t(82 + 74 * (1 - t)), d[2] = uint8_t(76 + 82 * (1 - t));
    }
  return out;
}

fs::path find_game_root() {
  if (const char* g = std::getenv("SHIFT_GAME_ROOT")) return g;
  for (const char* p : {R"(C:\Users\neri\code\horizon1-recomp\.local\game\base)", "../horizon1-recomp/.local/game/base"})
    if (fs::exists(fs::path(p) / "media" / "UI.zip")) return p;
  return {};
}

std::map<std::string, std::unique_ptr<fh1::Archive>> g_archives;

void load_game_ui(const fs::path& game) {
  if (auto fzip = fh1::Archive::open(game / "media/ui/Fonts.zip", nullptr)) {
    auto vf = [&](const char* nm) {
      auto r = fzip->read(nm);
      auto f = std::make_shared<fh1::VFont>();
      if (r.ok() && fh1::parse_vfont(r.data, *f, nullptr)) return f;
      return std::shared_ptr<fh1::VFont>{};
    };
    carview::ui::set_vector_fonts(vf("aru_vector_aa.dt"), vf("bru_vector_aa.dt"), vf("eru_vector_aa.dt"),
                                  vf("dru_vector_aa.dt"));
  }
  g_archives["hz"] = fh1::Archive::open(game / "media/ui/textures/Horizon.zip", nullptr);
  g_archives["tx"] = fh1::Archive::open(game / "media/ui/Textures.zip", nullptr);
  static std::set<std::string> failed;
  carview::ui4::set_texture_loader([](const std::string& path, std::array<float, 4>& uv) -> bool {
    if (carview::ui::atlas_find(path, uv)) return true;
    if (failed.count(path)) return false;
    const bool t = path.rfind("T:", 0) == 0;
    fh1::Archive* a = (t ? g_archives["tx"] : g_archives["hz"]).get();
    auto r = a ? a->read((t ? path.substr(2) : path) + ".xds") : fh1::ReadResult{{}, "none"};
    const auto x = r.ok() ? fh1::decode_xds(r.data, false) : fh1::XdsResult{};
    if (x.levels.empty() || !carview::ui::atlas_image(path, x.levels[0].width, x.levels[0].height, x.levels[0].rgba, uv)) {
      failed.insert(path);
      return false;
    }
    return true;
  });
  if (auto& hz = g_archives["hz"])
    for (const auto& [k, m] : {std::pair{"tape.smudge", "DIRTMASKS/SMUDGETAPE01.xds"},
                               std::pair{"tape.crisp", "DIRTMASKS/CRISPTAPE01.xds"}}) {
      auto r = hz->read(m);
      const auto x = r.ok() ? fh1::decode_xds(r.data, false) : fh1::XdsResult{};
      std::array<float, 4> uv{};
      if (!x.levels.empty()) carview::ui::atlas_image(k, x.levels[0].width, x.levels[0].height, x.levels[0].rgba, uv);
    }
}

void save_png(const fs::path& p, const carview::ui::List& l, std::vector<uint8_t> fb) {
  carview::ui::rasterize(l.items(), fb, 1280, 720);
  CHECK(stbi_write_png(p.string().c_str(), 1280, 720, 4, fb.data(), 1280 * 4) != 0);
  std::printf("  wrote %s\n", p.string().c_str());
}

}  // namespace

#ifdef HAVE_SAVE_UI
void test_save_page(carview::SaveSession& session, carview::SaveProfilePage& page) {
  const fs::path user_dir = g_root / "save_ui_user";
  const fs::path sync_dir = g_root / "save_ui_sync";
  const fs::path remote_dev = g_root / "save_ui_deck";
  std::error_code ec;
  fs::create_directories(sync_dir, ec);

  // Seed a local save and a second device's diverged save so both Backups and
  // Save Sync Conflict UI have rich authentic data to test & screenshot!
  auto write_save_txt = [](const fs::path& dir, const std::string& car, int64_t cr) {
    write(dir / "save.txt", "shift-drive-save 1\ncar " + car + "\nposition 2002.96 143.84 960.00\nheading 2.2008\ncredits " +
                                std::to_string(cr) + " " + std::to_string(cr) + "\n");
    write(dir / "garage.txt", "shift-garage 1\n");
  };
  write_save_txt(user_dir / "drive", "NIS_SkylineGTRVSpecII_02", 117700);
  write_save_txt(remote_dev / "drive", "NIS_SkylineGTRVSpecII_02", 164500);

  // Publish Steam Deck save into sync folder.
  {
    profile::sync::Context rc;
    rc.save_dir = remote_dev / "drive";
    rc.state_file = remote_dev / "config" / "save_sync.toml";
    rc.sync_folder = sync_dir;
    rc.device_name = "STEAMDECK";
    rc.platform = "linux";
    rc.build = "v4";
    rc.now = 1795365700;
    CHECK(profile::sync::boot(rc).published);
  }
  // Also publish an older Windows laptop save.
  {
    const fs::path lap = g_root / "save_ui_laptop";
    write_save_txt(lap / "drive", "VW_Corrado_95", 82300);
    profile::sync::Context lc;
    lc.save_dir = lap / "drive";
    lc.state_file = lap / "config" / "save_sync.toml";
    lc.sync_folder = sync_dir;
    lc.device_name = "ROG-ALLY";
    lc.platform = "windows";
    lc.build = "base";
    lc.now = 1795360000;
    CHECK(profile::sync::boot(lc).published);
  }

  // Boot local SaveSession (v4 desktop) -> takes start backup & detects conflict!
  carview::SaveSession::BootOptions bo;
  bo.user_dir = user_dir;
  bo.default_save_dir = user_dir / "drive";
  bo.sync_folder = sync_dir;
  bo.device_name = "DESKTOP-MAIN";
  bo.v4_available = true;
  bo.now = 1795365800;
  session.boot(bo);
  CHECK(session.sync_report().conflict);

  // Add a couple more rolling backups and a second profile for realistic UI.
  std::string err;
  CHECK(session.profiles().set_gamertag("default", "Neri Horizon", &err));
  CHECK(session.profiles().create("weekend", "Colorado Pro", &err));
  write_save_txt(user_dir / "drive", "NIS_SkylineGTRVSpecII_02", 124500);
  CHECK(profile::backup::snapshot(session.backup_config(), "autosave", nullptr, &err, 1795366400));
  write_save_txt(user_dir / "drive", "FER_F40_87", 215000);
  CHECK(profile::backup::snapshot(session.backup_config(), "manual", nullptr, &err, 1795367000));

  page.open(session, carview::SaveProfilePage::Tab::kBackups);
  CHECK(page.is_open());
  CHECK(page.backups().size() == 3);
  CHECK(page.remotes().size() == 2);

  // Test Profiles tab + OnScreenKeyboard Gamertag edit!
  page.set_tab(carview::SaveProfilePage::Tab::kProfiles);
  page.nav(carview::Nav::kX, session);  // open OSK to edit default gamertag
  CHECK(page.osk_open());
  page.backspace();
  page.type_char('X');
  // Cancel so we keep "Neri Horizon", then verify close.
  page.nav(carview::Nav::kBack, session);
  CHECK(!page.osk_open());

  // Switch back to Save Backups tab and rasterize.
  page.set_tab(carview::SaveProfilePage::Tab::kBackups);
  page.set_notice("AUTOMATIC BACKUPS AT START, EVERY 10 MIN & CLEAN EXIT");
  carview::ui::List l;
  std::set<std::string> save_tex;
  carview::ui4::set_texture_loader([&](const std::string& p, std::array<float, 4>& uv) {
    save_tex.insert(p);
    uv = {0.0f, 0.0f, 0.1f, 0.1f};
    return true;
  });
  carview::draw_save_page(l, 1280, 720, page, session);
  CHECK(l.items().size() > 50);
  CHECK(lit(l) > 80000);
  CHECK(save_tex.count("DIRTMASKS/PILL02") > 0);
  CHECK(save_tex.count("DIRTMASKS/PANEL01") > 0);

  // Rasterize Save Sync Conflict tab.
  page.set_tab(carview::SaveProfilePage::Tab::kSync);
  l.clear();
  carview::draw_save_page(l, 1280, 720, page, session);
  carview::ui4::set_texture_loader({});
  CHECK(l.items().size() > 50);
  CHECK(lit(l) > 80000);

  // Leave page open on Backups tab for main() screenshot export.
  page.set_tab(carview::SaveProfilePage::Tab::kBackups);
}
#endif

int main() {
  std::error_code ec;
  g_root = fs::temp_directory_path(ec) / "shift_mods_saves_ui_tests";
  fs::remove_all(g_root, ec);
  fs::create_directories(g_root, ec);
  carview::ModsSession mods_session;
  carview::ModsPage mods_page;
  test_mods_page(mods_session, mods_page);
#ifdef HAVE_SAVE_UI
  carview::SaveSession save_session;
  carview::SaveProfilePage save_page;
  test_save_page(save_session, save_page);
#endif
  const char* shots = std::getenv("SHIFT_UI4_SCREENSHOT_DIR");
  const fs::path game = find_game_root();
  if (shots && *shots && !game.empty()) {
    const fs::path out(shots);
    fs::create_directories(out, ec);
    load_game_ui(game);
    const std::vector<fs::path> bgs = {R"(C:\Users\neri\code\shift-engine\out\play\r34_00000.png)",
                                       R"(C:\Users\neri\code\shift-engine\out\play\h_00000.png)"};
    carview::ui::List l;
    // Select the Skyline mod so its details (database patch, save copy) show.
    while (mods_page.selected() != 0) mods_page.nav(carview::Nav::kUp);
    mods_page.set_notice("F5 / X RELOADS REPLACEMENT TEXTURES LIVE");
    carview::draw_mods_page(l, 1280, 720, mods_page);
    save_png(out / "slice13_a_f6_mods.png", l, backdrop(bgs));
#ifdef HAVE_SAVE_UI
    l.clear();
    carview::draw_save_page(l, 1280, 720, save_page, save_session);
    save_png(out / "slice13_b_save_backups.png", l, backdrop(bgs));
    save_page.set_tab(carview::SaveProfilePage::Tab::kSync);
    l.clear();
    carview::draw_save_page(l, 1280, 720, save_page, save_session);
    save_png(out / "slice13_c_save_sync.png", l, backdrop(bgs));
#endif
  }
  fs::remove_all(g_root, ec);
  if (g_failed) {
    std::fprintf(stderr, "mods_saves_ui_tests: %d failed\n", g_failed);
    return 1;
  }
  std::printf("mods_saves_ui_tests: all passed\n");
  return 0;
}
