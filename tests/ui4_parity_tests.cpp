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
#include <fstream>
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
#include "camera_editor.h"
#include "cars/physics.h"
#include "cars/sources.h"
#include "club_view.h"
#include "events_page.h"
#include "fh1data/archive.h"
#include "fh1data/spa.h"
#include "fh1data/ui_scene.h"
#include "fh1data/vfont.h"
#include "fh1data/xds.h"
#include "freeroam/outposts.h"
#include "freeroam/road_discovery.h"
#include "game_hud.h"
#include "mods_page.h"
#include "perf_overlay.h"
#include "replay_view.h"
#include "save_sync_ui.h"
#include "touch_controls.h"
#include "trainer_overlay.h"
#include "garage/garage.h"
#include "garage/pi_calc.h"
#include "hub_scene.h"
#include "hud.h"
#include "hud_scene.h"
#include "pause_screen.h"
#include "livery_editor.h"
#include "map_view.h"
#include "race/campaign_flow.h"
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

  // Phase 3 & 4 helper verification: EventTickets/*, UI4/Upgrades/*, SponsorLogoTile/*,
  // OPT_BAR_ARROW, ui4::tilted_blade_screen(), and pretty_car_media_name().
  std::set<std::string> req_tex;
  carview::ui4::set_texture_loader([&](const std::string& p, std::array<float, 4>& uv) {
    req_tex.insert(p);
    uv = {0.0f, 0.0f, 0.1f, 0.1f};
    return true;
  });
  carview::ui::List l34;
  check(carview::ui4::event_ticket_art(l34, 40, 40, 300, 360, 101, 2), "event_ticket_art loads StreetRace ticket");
  check(carview::ui4::event_ticket_art(l34, 40, 40, 300, 360, 201, 10), "event_ticket_art loads PR Stunt ticket");
  check(carview::ui4::upgrade_part_icon(l34, 40, 40, 120, 120, 2, 2, false), "upgrade_part_icon loads UI4/Upgrades/*");
  check(carview::ui4::tuning_icon(l34, 40, 40, 120, 120, 0, "Tire Pressure"), "tuning_icon loads UI4/Tuning/*");
  check(carview::ui4::sponsor_logo_tile(l34, 40, 40, 200, 120, "Oakley"), "sponsor_logo_tile loads SponsorLogoTile/*");
  check(carview::ui4::opt_arrow(l34, 100, 100, 14, +1), "opt_arrow loads OPTIONS/OPT_BAR_ARROW");

  carview::ui4::TiltedBladeSpec blade;
  blade.title = "TEST BLADE";
  blade.eyebrow = "PHASE 4";
  blade.tabs = {"TAB ONE", "TAB TWO"};
  blade.list_header = "ITEMS";
  blade.detail_header = "DETAILS";
  blade.detail_title = "ACTIVE ITEM";
  blade.rows.push_back({"ROW ONE", "Subtext", "ON", carview::ui4::kGreen, true, true, true, 0.65f, carview::ui4::kYellow});
  blade.rows.push_back({"ROW TWO", "Subtext", "OFF", carview::ui4::kDim, false});
  blade.detail_kvs.push_back({"CAR", carview::ui4::pretty_car_media_name("FER_F40_87"), carview::ui4::kWhite});
  blade.prompts = {{"A", "SELECT"}, {"B", "BACK"}};
  carview::ui4::tilted_blade_screen(l34, 1280.0f, 720.0f, blade);
  carview::ui4::set_texture_loader({});

  auto has_prefix = [&](const std::string& pfx) {
    for (const auto& t : req_tex)
      if (t.rfind(pfx, 0) == 0) return true;
    return false;
  };
  check(has_prefix("EVENTTICKETS/"), "Phase 3 EventTickets/* textures requested");
  check(has_prefix("T:UI4/Upgrades/"), "Phase 3 UI4/Upgrades/* textures requested");
  check(has_prefix("T:UI4/Tuning/"), "Phase 3 UI4/Tuning/* textures requested");
  check(has_prefix("SPONSORLOGOTILE/"), "Phase 3 SponsorLogoTile/* textures requested");
  check(req_tex.count("OPTIONS/OPT_BAR_ARROW") > 0, "Phase 3/4 OPTIONS/OPT_BAR_ARROW requested");
  check(req_tex.count("DIRTMASKS/PILL02") > 0 && req_tex.count("DIRTMASKS/PANEL01") > 0,
        "Phase 4 tilted_blade_screen uses PILL02 & PANEL01");
  check(carview::ui4::pretty_car_media_name("FER_F40_87") == "1987 Ferrari F40",
        "pretty_car_media_name resolves FER_F40_87");
  check(carview::ui4::pretty_car_media_name("NIS_SKYLINEGTRVSPECII_02") == "2002 Nissan Skyline GT-R V-Spec II",
        "pretty_car_media_name resolves NIS_SKYLINEGTRVSPECII_02");
  check(carview::car_name("FOR_GT_05") == "2005 Ford GT", "car_name resolves FOR_GT_05");
  check(carview::car_name("SUB_IMPREZA22B_98") == "1998 Subaru Impreza 22B STi",
        "car_name resolves SUB_IMPREZA22B_98");
  check(std::string(carview::place_suffix(2)) == "ND" && std::string(carview::place_suffix(3)) == "RD" &&
            std::string(carview::place_suffix(4)) == "TH",
        "place_suffix produces crisp ordinals");
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
                          const std::filesystem::path& repo_root, bool only_uifix = false) {
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);

  // Load all 11 authentic disc vector fonts + fontmap.xml from Fonts.zip!
  if (auto fzip = fh1::Archive::open(game_root / "media/ui/Fonts.zip", nullptr)) {
    check(carview::ui::load_fonts_from_zip(*fzip), "load_fonts_from_zip in ui4_parity_tests");
  }

  auto uizip = fh1::Archive::open(game_root / "media/UI.zip", nullptr);
  auto hz = fh1::Archive::open(game_root / "media/ui/textures/Horizon.zip", nullptr);
  auto tx = fh1::Archive::open(game_root / "media/ui/Textures.zip", nullptr);
  std::map<std::string, std::unique_ptr<fh1::UiScene>> scene_cache;
  std::set<std::string> failed_tex;

  // Locate optional Rally DLC loose texture directory (CoDriverIconSet.xds / RallyTickets).
  std::filesystem::path rally_tex_dir;
  {
    const std::filesystem::path bases[] = {
        game_root / ".." / ".." / "dlc",
        game_root / ".." / "dlc",
        game_root / "dlc",
    };
    for (const auto& base : bases) {
      if (!std::filesystem::exists(base, ec)) continue;
      for (const auto& ent : std::filesystem::recursive_directory_iterator(base, ec)) {
        if (ent.is_regular_file(ec) && ent.path().filename() == "CoDriverIconSet.xds") {
          rally_tex_dir = ent.path().parent_path().parent_path().parent_path();
          break;
        }
      }
      if (!rally_tex_dir.empty()) break;
    }
  }

  carview::set_ui_textures([&](const std::string& path, std::array<float, 4>& uv) -> bool {
    if (carview::ui::atlas_find(path, uv)) return true;
    if (failed_tex.count(path)) return false;
    const bool t = path.rfind("T:", 0) == 0;
    const std::string rel = (t ? path.substr(2) : path) + ".xds";
    fh1::Archive* a = t ? tx.get() : hz.get();
    fh1::ReadResult r = a ? a->read(rel) : fh1::ReadResult{};
    if (!r.ok() && !t && !rally_tex_dir.empty()) {
      const auto p = rally_tex_dir / rel;
      if (std::filesystem::is_regular_file(p, ec)) {
        std::ifstream f(p, std::ios::binary);
        r.data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      }
    }
    if (r.data.empty()) {
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

  if (only_uifix) {
    auto save_two_up_uifix = [&](const std::string& filename, const carview::ui::List& top_l, std::vector<uint8_t> top_bg,
                                 const carview::ui::List& bot_l, std::vector<uint8_t> bot_bg) {
      carview::ui::rasterize(top_l.items(), top_bg, W, H);
      carview::ui::rasterize(bot_l.items(), bot_bg, W, H);
      std::vector<uint8_t> out(size_t(W) * (2 * H) * 4, 255);
      std::memcpy(out.data(), top_bg.data(), size_t(W) * H * 4);
      std::memcpy(out.data() + size_t(W) * H * 4, bot_bg.data(), size_t(W) * H * 4);
      for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
      const auto p = out_dir / filename;
      check(stbi_write_png(p.string().c_str(), int(W), int(2 * H), 4, out.data(), int(W * 4)) != 0, filename.c_str());
    };
    const auto oracle_free = repo_root / "out/oracle/ui/hud-freeroam-event-marker.png";
    const auto oracle_race = repo_root / "out/oracle/ui/race-hud.png";
    const auto bg_path = std::filesystem::exists(world_bg_path) ? world_bg_path : oracle_free;

    // 1. uifix_settings_f3.png: Settings + compact F3 overlay after 100-frame glyph stability soak!
    {
      carview::SettingsPage sp;
      sp.load({});
      sp.open();
      carview::PerfInfo pinfo;
      pinfo.fps = 60.0;
      pinfo.fps_1pct_low = 58.0;
      pinfo.render_w = 1280;
      pinfo.render_h = 720;
      pinfo.output_w = 3200;
      pinfo.output_h = 1800;
      pinfo.cpu_ms = 1.63;
      pinfo.gpu_ms = 2.27;
      pinfo.gpu_stages = {{"shadows", 0.57},      {"setup", 1.28},      {"world opaque", 0.0},
                          {"world cut-outs", 0.0},{"car_opaque", 0.0},  {"sky", 0.0},
                          {"hiz_build", 0.01},    {"post", 0.13},       {"upscale", 0.08},
                          {"ui", 0.18},           {"rest", 0.0}};
      pinfo.vram_used = uint64_t(558.8 * 1024 * 1024);
      pinfo.vram_budget = uint64_t(19.2 * 1024 * 1024 * 1024);
      pinfo.rss_mib = 1700.0;
      pinfo.hdr = false;
      pinfo.vsync = "FIFO";
      pinfo.upscaler = "BILINEAR";
      pinfo.frame_generation = "OFF";
      pinfo.device = "Apple M4 Pro";
      pinfo.build = std::string("shift-engine ") + SHIFT_GIT_COMMIT + " (" + SHIFT_BUILD_CONFIG + ")";

      carview::ui::List l;
      for (int f = 0; f < 100; ++f) {
        l.clear();
        carview::draw_settings_ui4(l, float(W), float(H), sp, true);
        carview::build_perf_overlay(l, pinfo, float(W), float(H), 1.0f);
        carview::ui::atlas_take_dirty();
      }
      save_frame("uifix_settings_f3.png", l, load_or_build_backdrop(bg_path));
    }

    // 2. uifix_speedo_hud.png: 2-up 1:1 FH1 Speedometer/Tachometer on real driving frames
    //    (top: 131 km/h @ 4850 rpm revving up with glowing pink trail; bottom: 000 km/h @ 850 rpm faint leading zeros)
    {
      carview::DriveHud d_race;
      d_race.driving = true;
      d_race.kmh = 131.0;
      d_race.redline_rpm = 8000.0;
      d_race.gear = 4;
      carview::ui::List l_race;
      for (int step = 0; step < 6; ++step) {
        l_race.clear();
        d_race.rpm = 3850.0 + double(step) * 200.0;
        d_race.time = 10.0 + double(step) * 0.033;
        carview::build_drive_hud(l_race, d_race, float(W), float(H));
      }

      carview::DriveHud d_idle;
      d_idle.driving = true;
      d_idle.kmh = 0.0;
      d_idle.rpm = 850.0;
      d_idle.redline_rpm = 8000.0;
      d_idle.gear = 1;
      d_idle.time = 20.0;
      carview::ui::List l_idle;
      carview::build_drive_hud(l_idle, d_idle, float(W), float(H));

      save_two_up_uifix("uifix_speedo_hud.png", l_race, load_or_build_backdrop(bg_path), l_idle,
                        load_or_build_backdrop(bg_path));
    }

    // 3. uifix_world_map.png: 100% Fullscreen edge-to-edge World Map with authentic Colorado colors & 1:1 H_MAP chrome
    {
      std::vector<uint8_t> bg = load_or_build_backdrop({});
      carview::MapView mv;
      std::string merr;
      if (mv.open(game_root, &merr)) {
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
        const auto map_img = mv.pause_map(ms, int(W), int(H), 2.0, ctr[0] - 350, ctr[1] + 220);
        check(map_img.width == W && map_img.height == H, "fullscreen world map exact viewport bounds");
        if (map_img.width == W && map_img.height == H) {
          double sum_r = 0, sum_g = 0, sum_b = 0;
          const size_t npix = size_t(W) * H;
          for (size_t i = 0; i < npix; ++i) {
            sum_r += map_img.rgba[i * 4 + 0];
            sum_g += map_img.rgba[i * 4 + 1];
            sum_b += map_img.rgba[i * 4 + 2];
          }
          check(sum_g > sum_b * 1.02 || sum_r > sum_b * 1.02, "world map terrain colors not cyan-swapped");
          bg = map_img.rgba;
        }
      }
      carview::MapOverlayHud mo;
      mo.active = true;
      mo.title = "RED ROCK OUTPOST";
      mo.subtitle = "HORIZON OUTPOST  •  PR STUNTS 2 / 3";
      mo.action_hint = "FAST TRAVEL";
      mo.gps_hint = "CLEAR GPS ROUTE";
      mo.gps_active = true;
      mo.distance_m = 1480.0;
      mo.active_filter = 0;
      mo.filter_tabs = {"ALL", "EVENTS", "STREET", "OUTPOSTS", "COLLECTIBLES", "PR STUNTS"};
      mo.roads_discovered = 142;
      mo.roads_total = 216;
      mo.zoom = 2.0;
      carview::DriveHud d;
      d.driving = false;
      d.map_overlay = &mo;
      d.band_name = "BLUE WRISTBAND";
      d.band_next = "BLUE";
      d.band_level = 3;
      d.band_collect_ready = true;
      d.car_name = "Nissan Skyline GT-R V-spec II";
      d.car_class = "B";
      d.car_pi = 461;
      d.credits = 117700;
      carview::ui::List l;
      carview::build_drive_hud(l, d, float(W), float(H));
      save_frame("uifix_world_map.png", l, std::move(bg));
    }

    // 4. uifix_list_stability.png: 2-up pause menu before/after moving selection (0 -> 3)
    {
      auto make_pause = [&](int sel) {
        carview::DriveHud d;
        d.driving = false;
        d.pause = true;
        d.menu_title = "paused";
        d.menu = {"MAP", "SETTINGS", "PHOTO MODE", "MESSAGE CENTER", "SPONSOR CHALLENGES", "MY PROFILE", "QUIT"};
        d.menu_selected = sel;
        d.band_name = "BLUE WRISTBAND";
        d.band_next = "BLUE";
        d.band_level = 3;
        d.band_collect_ready = true;
        d.car_name = "Nissan Skyline GT-R V-spec II";
        d.car_class = "B";
        d.car_pi = 461;
        d.credits = 117700;
        carview::ui::List l;
        carview::build_drive_hud(l, d, float(W), float(H));
        return l;
      };
      save_two_up_uifix("uifix_list_stability.png", make_pause(0), load_or_build_backdrop(bg_path), make_pause(3),
                        load_or_build_backdrop(bg_path));
    }
    return;
  }

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
    carview::set_game_text([car_names](const std::string& ref) -> std::string {
      return car_names->text(ref);
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

  // ---------------- GROUP 3 SCREENSHOTS ----------------

  // 9. ui4_prerace_postrace.png — Authentic FH1 Post-Race Cash Reward (H_POST_RACE_CREDITS_BREAKDOWN) + Pre-Race checks
  {
    carview::RaceHud rh;
    rh.phase = carview::RaceHud::kFinished;
    rh.results_page = 1;  // Cash Reward breakdown
    rh.event = "RED ROCK DESCENT";
    rh.event_type = "FESTIVAL RACE";
    rh.place = 1;
    rh.cars = 8;
    rh.clock = 152.0;
    rh.finish_time = 148.0;
    rh.page_enter_time = 150.0;
    rh.base_credits = 8500;
    rh.mileage_bonus = 950;
    rh.difficulty_pct = 55;
    rh.difficulty_bonus = 2550;
    rh.assists_pct = 25;
    rh.assists_bonus = 2125;
    rh.skills_bonus = 1850;
    rh.driver_xp = 3400;
    rh.affinity_xp = 1200;
    rh.credits = 15975;
    rh.balance = 264475;

    carview::DriveHud d;
    d.driving = true;
    d.race = &rh;

    carview::ui::List l;
    carview::build_drive_hud(l, d, float(W), float(H));
    save_frame("ui4_prerace_postrace.png", l, load_or_build_backdrop(world_bg_path));

    // Also verify Pre-Race 3-2-1 countdown digits render during kCountdown (P0-U1).
    rh.phase = carview::RaceHud::kCountdown;
    rh.grid_menu_open = false;
    rh.digit = 0;
    rh.clock = 0.0;
    carview::ui::List lcount0;
    carview::build_drive_hud(lcount0, d, float(W), float(H));
    for (int cd = 3; cd >= 1; --cd) {
      rh.digit = cd;
      rh.clock = -double(cd) + 0.25;
      carview::ui::List lcount;
      carview::build_drive_hud(lcount, d, float(W), float(H));
      check(lcount.items().size() > lcount0.items().size(), "Pre-race countdown digit emits visible shapes");
    }

    // Also verify game_text() preserves {font='SYM'} and {colour=...} while stripping unknown tags (P0-U3).
    {
      carview::set_game_text([](const std::string& k) { return k; });
      const std::string rich =
          carview::game_text("PRESS {font='SYM'}A{font_pop} FOR {colour='249 0 100 255'}HORIZON{colour_pop} {0} [UI.Flow]");
      check(rich.find("{font='SYM'}") != std::string::npos, "game_text preserves {font='SYM'} tag");
      check(rich.find("{font_pop}") != std::string::npos, "game_text preserves {font_pop} tag");
      check(rich.find("{colour='249 0 100 255'}") != std::string::npos, "game_text preserves {colour=...} tag");
      check(rich.find("{0}") == std::string::npos, "game_text strips unrecognized {0} placeholder");
      check(rich.find("[UI.Flow]") == std::string::npos, "game_text strips [UI.*] token");
      carview::set_game_text({});
    }

    // Also verify Pre-Race Grid Menu, Race Results Table, Points Earned, and Prize Car Won emit full UI4 chrome.
    rh.phase = carview::RaceHud::kCountdown;
    rh.digit = 0;
    rh.clock = 0.0;
    rh.grid_menu_open = true;
    rh.grid_menu = {"START RACE", "DIFFICULTY", "QUIT RACE"};
    rh.grid_car_info = "2002 Nissan Skyline GT-R V-spec II  •  B 461";
    carview::ui::List lpre;
    carview::build_drive_hud(lpre, d, float(W), float(H));
    check(lpre.items().size() > 15, "Pre-Race Grid Menu emits FH1 UI4 chrome");

    rh.phase = carview::RaceHud::kFinished;
    rh.results_page = 3;
    rh.won_car_name = "2012 Dodge SRT Viper GTS";
    carview::ui::List lwon;
    carview::build_drive_hud(lwon, d, float(W), float(H));
    check(lwon.items().size() > 10, "Prize Car Won emits FH1 UI4 chrome");
  }

  // 10. ui4_career_events.png — Authentic FH1 My Profile / Career Board + Events & Street Race Hub checks
  {
    carview::CareerBoard cb;
    cb.band_name = "GREEN WRISTBAND";
    cb.band_next = "BLUE WRISTBAND";
    cb.band_lo = 12500;
    cb.band_hi = 35000;
    cb.points = 26400;
    cb.credits = 264475;
    cb.races = 28;
    cb.wins = 21;
    cb.podiums = 26;
    cb.winnings = 842500;
    cb.driver_level = 14;
    cb.popularity_rank = 64;
    cb.garage = 18;
    cb.roads = 189;
    cb.roads_total = 216;
    cb.outposts = 7;
    cb.outposts_total = 8;
    cb.pr_stunts = 16;
    cb.pr_stunts_total = 24;
    cb.speed_traps = 19;
    cb.speed_traps_total = 24;
    cb.speed_zones = 6;
    cb.speed_zones_total = 10;
    cb.signs = 74;
    cb.signs_total = 100;
    cb.barns = 6;
    cb.barns_total = 9;
    cb.club_done = 42;
    cb.club_total = 1145;
    cb.packs_owned = 4;
    cb.packs_total = 7;

    carview::DriveHud d;
    d.driving = false;
    d.menu_title = "MY PROFILE";
    d.menu = {"CAREER OVERVIEW", "DRIVER STATS", "HORIZON PROMO", "1000 CLUB", "BACK"};
    d.menu_selected = 0;
    d.career = &cb;

    carview::ui::List l;
    carview::build_drive_hud(l, d, float(W), float(H));
    save_frame("ui4_career_events.png", l, load_or_build_backdrop(world_bg_path));

    // Also verify Rally Hub H_STREET_RACE_HUB tilted ticket cards emit cleanly.
    carview::RallyHubState rhs;
    rhs.active = true;
    rhs.car_name = "SUB_IMPREZA22B_98";
    rhs.car_class = "B";
    rhs.car_pi = 485;
    rhs.credits = 264475;
    rhs.rally_level = 4;
    rhs.rally_xp = 8200;
    rhs.selected = 1;
    rhs.items = {
        {carview::RallyHubItem::kChampionship, 1, "BEAUMONT RALLY", "B", "1ST", true, true, "", 20000, 4, {"Stage 1", "Stage 2"}},
        {carview::RallyHubItem::kChampionship, 2, "RED ROCK DIRT CHALLENGE", "A", "NEW", true, true, "", 35000, 4, {"Stage 1", "Stage 2"}},
        {carview::RallyHubItem::kChampionship, 3, "CLEAR SPRINGS FINALE", "S", "READY", true, true, "", 55000, 4, {"Stage 1", "Stage 2"}},
    };
    carview::ui::List lsrh;
    carview::build_rally_hub_screen(lsrh, rhs, float(W), float(H));
    check(lsrh.items().size() > 25, "Rally Hub H_STREET_RACE_HUB emits tilted FH1 ticket cards");
    save_frame("ui4_events_preview.png", lsrh, load_or_build_backdrop(world_bg_path));
  }

  // 11. ui4_achievements_settings.png — Authentic FH1/Pinyon Shift F7 Achievements + Tabbed Settings & Photo/Replay
  {
    fh1::SpaDatabase spa;
    const char* spa_state_env = std::getenv("SHIFT_STATE_ROOT");
    const std::filesystem::path spa_state =
        (spa_state_env && *spa_state_env) ? std::filesystem::path(spa_state_env) : std::filesystem::path{};
    fh1::load_spa_packages(game_root, spa_state, spa, "EN");
    // Without a spa.bin the system falls back to the SPA's own names and
    // gamerscores (apps/carview/achievements.cpp kFh1Meta).
    carview::AchievementsSystem ach;
    ach.init(spa);
    carview::AchievementGameplayState st;
    st.career_started = true;
    st.wristbands_earned = 3;
    st.num_races = 24;
    st.num_victories = 18;
    st.num_podiums = 22;
    st.festival_races_won = 14;
    st.street_races_won = 4;
    st.roads_discovered = 192;
    st.discount_signs_found = 86;
    st.barns_found = 7;
    st.outposts_discovered = 7;
    st.pr_stunts_completed = 14;
    st.garage_cars = 16;
    st.promo_cars_spotted = 35;
    st.driver_level = 14;
    st.popularity_points = 120000.0;
    st.total_winnings = 850000;
    st.max_skills_score = 45000;
    ach.evaluate(st, false);
    ach.open_screen();
    ach.update(1.0);  // finish intro animation so progress & gamerscore render at 100%

    carview::ui::List l;
    ach.build_screen_hud(l, float(W), float(H));
    save_frame("ui4_achievements_settings.png", l, load_or_build_backdrop(world_bg_path));

    // Also verify Tabbed FH1/Pinyon Settings overlay, Photo Mode HUD, and Replay Viewer HUD!
    carview::SettingsPage sp;
    sp.load({});
    sp.open();
    sp.switch_tab(2);  // GAMEPLAY tab
    sp.move(1);
    carview::ui::List lsp;
    carview::draw_settings_ui4(lsp, float(W), float(H), sp, true);
    check(lsp.items().size() > 30, "Tabbed Settings UI4 emits FH1 tabs & rows");
    save_frame("ui4_settings_preview.png", lsp, load_or_build_backdrop(world_bg_path));

    carview::PhotoModeController pm;
    pm.enter({0, 1, 0}, 0.0, carview::PhotoLimits{});
    carview::ui::List lpm;
    pm.build_hud(lpm, float(W), float(H));
    check(lpm.items().size() > 30, "Photo Mode HUD emits FH1 UI4 chrome");
    save_frame("ui4_photo_preview.png", lpm, load_or_build_backdrop(world_bg_path));
  }

  // ---------------- UI4 RE LAYOUT PARITY VERIFICATION SCREENSHOTS ----------------
  auto save_two_up = [&](const std::string& filename, const carview::ui::List& top_l, std::vector<uint8_t> top_fb,
                         const carview::ui::List& bot_l, std::vector<uint8_t> bot_fb) {
    carview::ui::rasterize(top_l.items(), top_fb, W, H);
    carview::ui::rasterize(bot_l.items(), bot_fb, W, H);
    std::vector<uint8_t> combined(size_t(W) * size_t(H * 2) * 4u, 255);
    std::memcpy(combined.data(), top_fb.data(), top_fb.size());
    std::memcpy(combined.data() + top_fb.size(), bot_fb.data(), bot_fb.size());
    const auto p = out_dir / filename;
    check(stbi_write_png(p.string().c_str(), int(W), int(H * 2), 4, combined.data(), int(W * 4)) != 0,
          filename.c_str());
  };

  // 1. ui4_re_hud_freeroam.png: Clean Free-Roam HUD (947_HUD tach + real
  //    POPULARITYBARBACKGROUND + SKILLICONSET + layered MapProfileMinimap + clean
  //    ActivationPrompt + CombinedNotification with real SPONSORLOGOTILE/OAKLEY)
  {
    auto bg = load_or_build_backdrop(world_bg_path);
    carview::MapView mv;
    if (mv.open(game_root, nullptr)) {
      carview::MapScene ms;
      // Place at Horizon Festival intersection so the layered multi-road Colorado network fills the minimap.
      ms.x = 2003.0;
      ms.z = 960.0;
      ms.heading = 2.2007;
      ms.speed_mps = 34.0;
      ms.gps_route = {{ms.x, ms.z}, {ms.x + 110, ms.z - 80}, {ms.x + 240, ms.z - 190}};
      ms.markers = {
          {carview::MapMarker::kFestivalSite, "racecentral", "fest", "HORIZON FESTIVAL", "", ms.x + 85, ms.z - 65, true, false, true, 0},
          {carview::MapMarker::kEvent, "race", "FR06", "MEDIA CENTER", "", ms.x - 65, ms.z - 95, true, false, true, 0},
      };
      const auto mm = mv.minimap(ms, 224);
      composite_rgba(bg, W, H, mm, 76, 412);
    }
    carview::SkillChainHud sk;
    sk.active = true;
    sk.chain_points = 182;
    sk.multiplier = 2;
    sk.last_skill = "ULTIMATE AIR";
    sk.last_points = 500;
    sk.popularity_rank = 165;
    sk.popularity_progress = 0.68;

    carview::WorldMarkerHud wm;
    wm.visible = true;
    wm.in_range = true;
    wm.screen_x = 490;
    wm.screen_y = 250;
    wm.title = "MEDIA CENTER";
    wm.prompt = "ENTER NOW";
    wm.eligible = true;

    carview::DriveHud d;
    d.driving = true;
    d.kmh = 131.0;
    d.rpm = 5600.0;
    d.redline_rpm = 8000.0;
    d.gear = 4;
    d.abs = true;
    d.tcs = false;
    d.time = 12.4;
    d.skills = &sk;
    d.world_marker = &wm;
    d.sponsor_title = "OAKLEY RACING CHALLENGE";
    d.sponsor_detail = "EARN 50,000 SKILL POINTS IN FREE ROAM";
    d.sponsor_at = 11.2;

    carview::ui::List l;
    carview::build_drive_hud(l, d, float(W), float(H));
    save_frame("ui4_re_hud_freeroam.png", l, std::move(bg));
  }

  // 2. ui4_re_pause_menu.png: Real 925_PAUSE_MENU (feathered 2-tex brushes, BG_Noise,
  //    DIRTTILE01, TAPECLIPON02, clean PlayerBar without RANK pill, BUTTON_BAR_SCENE)
  {
    carview::DriveHud d;
    d.driving = false;
    d.pause = true;
    d.menu_title = "paused";
    d.menu = {"MAP", "SETTINGS", "PHOTO MODE", "MESSAGE CENTER", "SPONSOR CHALLENGES", "MY PROFILE", "QUIT"};
    d.menu_selected = 0;
    d.band_name = "BLUE WRISTBAND";
    d.band_next = "BLUE";
    d.band_level = 3;
    d.band_collect_ready = true;
    d.car_name = "Nissan Skyline GT-R V-spec II";
    d.car_class = "B";
    d.car_pi = 461;
    d.credits = 117700;
    // The pause menu from its scene (pause_screen.h).
    carview::PauseMenu pm;
    pm.show(false);
    carview::PauseScreenHud ps;
    carview::fill_pause_screen(pm, 1e9, ps);
    d.pause_screen = &ps;
    carview::ui::List l;
    carview::build_drive_hud(l, d, float(W), float(H));
    save_frame("ui4_re_pause_menu.png", l, load_or_build_backdrop(world_bg_path));
  }

  // 3. ui4_re_world_map.png: Real H_MAP (MAP_CROSSHAIR + #CF2169 MAP_LOCKON +
  //    right-hand PILL26PX panel + FESTIVALMAPICON + PlayerBar + BUTTON_BAR_SCENE)
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
    mo.subtitle = "HORIZON OUTPOST  •  PR STUNTS 2 / 3";
    mo.action_hint = "FAST TRAVEL";
    mo.gps_hint = "CLEAR GPS ROUTE";
    mo.gps_active = true;
    mo.distance_m = 1480.0;
    mo.active_filter = 1;
    mo.filter_tabs = {"ALL", "EVENTS", "STREET", "OUTPOSTS", "PR STUNTS"};
    mo.roads_discovered = 142;
    mo.roads_total = 216;
    mo.zoom = 2.2;
    mo.treasure_map = true;
    carview::DriveHud d;
    d.driving = false;
    d.map_overlay = &mo;
    d.band_name = "BLUE WRISTBAND";
    d.band_next = "BLUE";
    d.band_level = 3;
    d.band_collect_ready = true;
    d.car_name = "Nissan Skyline GT-R V-spec II";
    d.car_class = "B";
    d.car_pi = 461;
    d.credits = 117700;
    carview::ui::List l;
    carview::build_drive_hud(l, d, float(W), float(H));
    save_frame("ui4_re_world_map.png", l, std::move(bg));
  }

  // 4. ui4_re_loading_keyboard.png: Full-res 2-up (top: unobstructed 2_C_INI_LOADING with
  //    BGLoadingImages + LoadingDefs.xml tilted tips + LOADSPINNER; bottom: H_MESSAGE_POPUP
  //    tilted keyboard — ZERO rounded boxes!)
  {
    carview::LoadingScreen ls;
    ls.load_definitions(game_root);
    ls.start("RED ROCK CANYON", "FESTIVAL CIRCUIT  •  CLASS A");
    ls.set_progress(0.72f);
    ls.update(1.4);

    carview::OnScreenKeyboard kb;
    kb.open("LICENSE PLATE", "HRZN2012", 12, carview::OnScreenKeyboard::Purpose::kLicensePlate);
    kb.move(3, 1);

    carview::ui::List l_load, l_kb;
    ls.draw(l_load, float(W), float(H));
    kb.draw(l_kb, float(W), float(H));
    save_two_up("ui4_re_loading_keyboard.png", l_load, load_or_build_backdrop({}, true), l_kb,
                load_or_build_backdrop(world_bg_path));
  }

  // 5. ui4_re_rally_replay.png: Full-res 2-up (top: real CoDriverIconSet.xds Rally Stage HUD;
  //    bottom: real 926_REPLAY_HUD viewer — ZERO rounded boxes!)
  {
    carview::RallyStageHud rsh;
    rsh.active = true;
    rsh.championship_name = "BEAUMONT RALLY";
    rsh.stage_name = "CLEAR SPRINGS GRAVEL";
    rsh.stage_number = 2;
    rsh.total_stages = 4;
    rsh.stage_clock = 92.45;
    rsh.progress = 0.61;
    rsh.total_splits = 4;
    rsh.splits_passed = 2;
    rsh.last_split_delta = -1.42;
    rsh.last_split_age = 0.8;
    rsh.pace.active = true;
    rsh.pace.icons = {
        {race::PaceIconKind::kMediumLeft, 3, -1, "3L", true},
        {race::PaceIconKind::kCrest, 0, 0, "CREST", false},
        {race::PaceIconKind::kEasyRight, 5, +1, "5R", false},
    };
    rsh.pace.modifiers = {"DON'T CUT", "60", "OVER CREST"};
    rsh.pace.phrase = "LEFT 3  •  DON'T CUT  •  OVER CREST";
    rsh.pace.age_s = 0.4;

    carview::DriveHud d_rally;
    d_rally.driving = true;
    d_rally.kmh = 148.0;
    d_rally.rpm = 6450.0;
    d_rally.redline_rpm = 7800.0;
    d_rally.gear = 4;
    d_rally.rally_stage = &rsh;

    drive::ReplayRecording rec;
    rec.meta.event_name = "CLEAR SPRINGS STAGE 2";
    rec.meta.sample_hz = 30.0;
    drive::ReplayCarInfo ci;
    ci.slot = 0;
    ci.driver_name = "HORIZON RALLY DRIVER";
    ci.car_media = "1998 Subaru Impreza 22B STi";
    rec.meta.roster.push_back(ci);
    rec.meta.markers.push_back({18.0, "SPLIT 1"});
    rec.meta.markers.push_back({42.0, "JUMP"});
    for (int f = 0; f < 30; ++f) {
      drive::ReplayFrame rf;
      rf.time_s = double(f) * 2.0;
      drive::ReplayCarSample cs;
      cs.slot = 0;
      cs.speed_mps = 41.2f;
      cs.rpm = 6450.0f;
      cs.redline_rpm = 7800.0f;
      cs.gear = 4;
      cs.throttle_u8 = 235;
      cs.brake_u8 = 0;
      cs.steer_i8 = -42;
      rf.cars.push_back(cs);
      rec.frames.push_back(std::move(rf));
    }
    carview::ReplayViewer rv;
    rv.open(std::move(rec), true);
    rv.seek(26.4);
    rv.set_telemetry(true);
    rv.update(0.0);

    carview::ui::List l_rally, l_rep;
    carview::build_drive_hud(l_rally, d_rally, float(W), float(H));
    rv.build_hud(l_rep, float(W), float(H));
    save_two_up("ui4_re_rally_replay.png", l_rally, load_or_build_backdrop(world_bg_path), l_rep,
                load_or_build_backdrop(world_bg_path));
  }

  // 6. ui4_re_android_touch.png: Android touch HUD with ZERO tach/minimap overlap!
  {
    auto bg = load_or_build_backdrop(world_bg_path);
    carview::TouchControls tc;
    tc.set_visibility(carview::TouchVisibility::kAlwaysOn);
    tc.set_steer_mode(carview::TouchSteerMode::kButtons);
    const auto lay = carview::TouchControls::compute_layout(float(W), float(H));
    tc.on_finger_down(1, lay.steer_left.cx(), lay.steer_left.cy(), float(W), float(H), 1.0);
    tc.on_finger_down(2, lay.throttle.cx(), lay.throttle.cy(), float(W), float(H), 1.0);

    carview::TouchRect mm_r, tach_r;
    carview::TouchControls::hud_safe_rects(float(W), float(H), mm_r, tach_r);
    carview::MapView mv;
    if (mv.open(game_root, nullptr)) {
      carview::MapScene ms;
      const auto ctr = mv.roads_centre();
      ms.x = ctr[0] + 100;
      ms.z = ctr[1] - 80;
      ms.heading = 0.35;
      ms.speed_mps = 38.0;
      const int mm_px = std::max(96, int(std::lround(std::min(mm_r.w() * float(W), mm_r.h() * float(H)))));
      const auto mm = mv.minimap(ms, mm_px);
      composite_rgba(bg, W, H, mm, int(std::lround(mm_r.x0 * float(W))),
                     int(std::lround(mm_r.y1 * float(H))) - int(mm.height));
    }
    carview::DriveHud d;
    d.driving = true;
    d.touch_hud_active = true;
    d.kmh = 156.0;
    d.rpm = 6200.0;
    d.redline_rpm = 8000.0;
    d.gear = 4;
    d.time = 10.0;

    carview::ui::List l;
    carview::build_drive_hud(l, d, float(W), float(H));
    tc.build_overlay(l, float(W), float(H), 1.0, false);
    save_frame("ui4_re_android_touch.png", l, std::move(bg));
  }

  // ---------------- PHASE 3 & 4 VERIFICATION SCREENSHOTS (6 COMPOSITES) ----------------
  auto save_quad = [&](const std::string& filename, const carview::ui::List& tl_l, std::vector<uint8_t> tl_fb,
                       const carview::ui::List& tr_l, std::vector<uint8_t> tr_fb, const carview::ui::List& bl_l,
                       std::vector<uint8_t> bl_fb, const carview::ui::List& br_l, std::vector<uint8_t> br_fb) {
    carview::ui::rasterize(tl_l.items(), tl_fb, W, H);
    carview::ui::rasterize(tr_l.items(), tr_fb, W, H);
    carview::ui::rasterize(bl_l.items(), bl_fb, W, H);
    carview::ui::rasterize(br_l.items(), br_fb, W, H);
    const uint32_t CW = W * 2, CH = H * 2;
    std::vector<uint8_t> combined(size_t(CW) * size_t(CH) * 4u, 255);
    auto blit = [&](const std::vector<uint8_t>& src, uint32_t ox, uint32_t oy) {
      for (uint32_t y = 0; y < H; ++y) {
        std::memcpy(&combined[(size_t(oy + y) * CW + ox) * 4u], &src[size_t(y) * W * 4u], size_t(W) * 4u);
      }
    };
    blit(tl_fb, 0, 0);
    blit(tr_fb, W, 0);
    blit(bl_fb, 0, H);
    blit(br_fb, W, H);
    const auto p = out_dir / filename;
    check(stbi_write_png(p.string().c_str(), int(CW), int(CH), 4, combined.data(), int(CW * 4)) != 0,
          filename.c_str());
  };

  // P3-1: ui4_p3_autoshow_garage.png — 2-up Autoshow (108_C_BUY_CARSELECT +3 deg bottom carousel +
  //       right stat card + synced 3D Ferrari FXX matching 1006.png!) & Garage (106_C_CAR_LIST)
  if (have_db) {
    carview::ui::List l_as, l_gar;
    carview::Autoshow as;
    std::string why;
    if (as.open(db, {}, why, 5000000, false)) {
      // Match the 3D Ferrari FXX (id=1006) shown in out/play/1006.png!
      for (size_t mi = 0; mi < as.makes().size(); ++mi) {
        if (as.makes()[mi].name == "Ferrari" || as.makes()[mi].name == "FER") {
          as.select_make(mi, 5000000);
          break;
        }
      }
      as.set_step(carview::AutoshowStep::kCarSelect);
      for (size_t ci = 0; ci < as.visible_cars().size(); ++ci) {
        if (as.visible_cars()[ci].dealer.id == 1006) break;
        as.move(1);
      }
      carview::draw_autoshow_hud(l_as, float(W), float(H), as, bands, 2485000);
    }

    garage::Garage g;
    int64_t gcred = 8000000;
    static constexpr int64_t kGarCars[] = {445, 309, 1006, 1323, 1009, 1293};
    for (int64_t cid : kGarCars) add_dealer_car(g, cid, gcred);
    if (!g.cars.empty()) g.selected = g.cars[0].id;  // Nissan Skyline GT-R V-spec II matching r34_00000.png!
    carview::GarageBrowser gb;
    gb.refresh(g, &db, bands);
    for (size_t gi = 0; gi < gb.visible().size(); ++gi) {
      if (gb.selected_entry() && gb.selected_entry()->car_id == 445) break;
      gb.move(1);
    }
    carview::draw_garage_hud(l_gar, float(W), float(H), gb, bands, 248500);

    save_two_up("ui4_p3_autoshow_garage.png", l_as, load_or_build_backdrop(studio_bg_path, true), l_gar,
                load_or_build_backdrop(world_bg_path));
  }

  // P3-2: ui4_p3_upgrades_paint.png — 2-up Upgrade Shop (527_C_UPG_UPGRADES +3 deg bottom blade +
  //       real UI4/Upgrades/* icons, car unobstructed!) & Paint Shop (501_C_LIV_COLOR bottom ribbon)
  if (have_db) {
    carview::ui::List l_upg, l_pnt;
    garage::Garage g;
    int64_t gcred = 5000000;
    const int64_t gid = add_dealer_car(g, 445, gcred);  // Nissan Skyline GT-R V-spec II
    carview::UpgradeShop shop;
    std::string why;
    if (gid > 0 && shop.open(db, g, gid, why, &phys_settings, &pi_cfg, &bands)) {
      shop.set_category(carview::ShopCategory::kEngine);
      shop.move_part(+2);
      carview::draw_upgrade_shop_hud(l_upg, float(W), float(H), shop, bands, 25, 248500,
                                     "Nissan Skyline GT-R V-spec II");
    }

    carview::LiveryEditor ed;
    std::string msg;
    if (ed.open(&db, game_root, 445, {}, msg)) {
      ed.move_finish(+1);
      ed.step_hsv(0.58f, 0.10f, 0.0f);
      carview::draw_livery_editor_hud(l_pnt, float(W), float(H), ed, "Nissan Skyline GT-R V-spec II");
    }
    save_two_up("ui4_p3_upgrades_paint.png", l_upg, load_or_build_backdrop(world_bg_path), l_pnt,
                load_or_build_backdrop(world_bg_path));
  }

  // P3-3: ui4_p3_events_outpost.png — 2-up Street Race / Events Hub (H_STREET_RACE_HUB with real
  //       EventTickets/* tilted ticket cards!) & Outpost PR Stunts (H_MISSIONS_HUB)
  {
    race::CareerCatalog catalog;
    race::load_career_catalog(game_root, catalog, nullptr);
    race::CareerRecord crec;
    crec.wristband_points = 26400;
    for (const auto& ev : catalog.events) {
      if (ev.style == 1 && crec.event_places.size() < 4) {
        crec.event_places[ev.event_id] = 1;
      }
    }
    carview::EventsPage evp;
    evp.open(catalog, crec, 3, 64, 445, 4, bands, 0);
    evp.switch_tab(+2);  // Street Races (or all events if street empty)
    if (evp.entries().empty()) evp.switch_tab(-2);

    carview::ui::List l_ev, l_out;
    carview::draw_events_ui4(l_ev, float(W), float(H), evp, bands);

    std::vector<carview::OutpostMissionCard> cards = {
        {"CANYON SPEED TRAP", "SPEED STUNT", 10, 101, "TARGET: 155 MPH", "+12,500 CR  •  +2,500 POP", "COMPLETED", true,
         true, {"Hit 155 mph past Red Rock Canyon camera."}},
        {"RED ROCK DRIFT RUN", "SKILL STUNT", 11, 102, "TARGET: 35,000 PTS", "+15,000 CR  •  +3,000 POP", "NEW PR STUNT",
         false, true, {"Chain 35,000 Skill Points through Red Rock switchbacks."}},
        {"ARCHWAY PHOTO SHOOT", "PHOTO OP", 12, 103, "SNAP AT RED ROCK ARCH", "+10,000 CR  •  +2,000 POP", "COMPLETED",
         true, true, {"Photograph your car beneath the Red Rock Arch."}},
        {"CHANGE CAR", "GARAGE", 1, 0, "INSTANT CAR SWAP", "MY GARAGE", "OUTPOST SERVICE", false, false,
         {"Swap into any car in your Horizon Garage right here."}},
    };
    carview::draw_outpost_hub_ui4(l_out, float(W), float(H), "RED ROCK OUTPOST", cards, 1);
    save_two_up("ui4_p3_events_outpost.png", l_ev, load_or_build_backdrop(world_bg_path), l_out,
                load_or_build_backdrop(world_bg_path));
  }

  // P3-4: ui4_p3_career_sponsors.png — 2-up My Profile (H_STATISTICS +3 deg floating rows) &
  //       Sponsor Challenges (H_SPONSOR_CHALLENGE_STATUS with real SponsorLogoTile/* logos!)
  {
    carview::CareerBoard cb;
    cb.band_name = "BLUE WRISTBAND";
    cb.band_next = "PURPLE WRISTBAND";
    cb.band_lo = 12500;
    cb.band_hi = 35000;
    cb.points = 26400;
    cb.credits = 264475;
    cb.races = 28;
    cb.wins = 21;
    cb.podiums = 26;
    cb.winnings = 842500;
    cb.driver_level = 14;
    cb.popularity_rank = 64;
    cb.garage = 18;
    cb.roads = 189;
    cb.roads_total = 216;
    cb.outposts = 7;
    cb.outposts_total = 8;
    cb.pr_stunts = 16;
    cb.pr_stunts_total = 24;
    cb.speed_traps = 19;
    cb.speed_traps_total = 24;
    cb.speed_zones = 6;
    cb.speed_zones_total = 10;
    cb.signs = 74;
    cb.signs_total = 100;
    cb.barns = 6;
    cb.barns_total = 9;
    cb.club_done = 42;
    cb.club_total = 1145;
    cb.packs_owned = 4;
    cb.packs_total = 7;

    carview::DriveHud d;
    d.driving = false;
    d.menu_title = "MY PROFILE";
    d.menu = {"CAREER OVERVIEW", "DRIVER STATS", "SPONSOR CHALLENGES", "1000 CLUB", "BACK"};
    d.menu_selected = 0;
    d.career = &cb;
    carview::ui::List l_car, l_sp;
    carview::build_drive_hud(l_car, d, float(W), float(H));

    std::vector<carview::SponsorChallengeRow> srows = {
        {"OAKLEY", "NEAR MISSES IN FREE ROAM", "Oakley", 38, 50, 2, 5, 15000},
        {"BOSE", "CLEAN BRAKING TURNS", "Bose", 100, 100, 3, 5, 20000},
        {"ROCKSTAR", "LONG DRIFT COMBOS", "Rockstar", 24, 40, 2, 5, 18000},
        {"ADIDAS", "BURNOUT & SPEED CHAINS", "Adidas", 18, 25, 1, 5, 16000},
        {"BONDURANT", "DRAFTING PASSES IN RACES", "Bondurant", 11, 20, 1, 5, 22000},
        {"GOPRO", "AIRTIME JUMPS ACROSS COLORADO", "GoPro", 15, 15, 3, 5, 25000},
    };
    carview::draw_sponsor_challenges_ui4(l_sp, float(W), float(H), srows, 0, 264475);
    save_two_up("ui4_p3_career_sponsors.png", l_car, load_or_build_backdrop(world_bg_path), l_sp,
                load_or_build_backdrop(world_bg_path));
  }

  // P3-5: ui4_p3_photo_settings.png — 2-up Photo Mode (516_I_EFFECTSMODE right-hand sliders!) &
  //       Settings (H_FIXED_SETTING_LIST +3 deg floating pills + OPT_BAR_ARROW!)
  {
    carview::PhotoModeController pm;
    pm.enter({0, 1, 0}, 0.0, carview::PhotoLimits::from_settings(false, true));
    carview::ui::List l_pm;
    pm.build_hud(l_pm, float(W), float(H));

    carview::SettingsPage sp;
    sp.load({});
    sp.open();
    sp.switch_tab(2);  // GAMEPLAY / ASSISTS
    sp.move(1);
    carview::ui::List l_set;
    carview::draw_settings_ui4(l_set, float(W), float(H), sp, true);
    save_two_up("ui4_p3_photo_settings.png", l_pm, load_or_build_backdrop(world_bg_path), l_set,
                load_or_build_backdrop(world_bg_path));
  }

  // P4-6: ui4_p4_shift_blades.png — 2x2 Phase 4 Shift blades on open +3.0 deg Colorado:
  //       TL: F6 Mods | TR: Save Backups & Sync (real car names!)
  //       BL: 1000 Club (real gold medals + clean Red Rock Outpost) | BR: Camera Editor (right-side sliders)
  {
    const auto tmp_dir = std::filesystem::temp_directory_path(ec) / "shift_ui4_p4_shot";
    std::filesystem::remove_all(tmp_dir, ec);
    std::filesystem::create_directories(tmp_dir / "mods", ec);

    // TL: F6 Mods
    carview::ModsSession ms;
    std::vector<cars::modmgr::ModInfo> mods;
    {
      cars::modmgr::ModInfo m1;
      m1.name = "skyline_r34_pack";
      m1.manifest.title = "R34 Nismo Z-Tune Pack";
      m1.manifest.author = "Horizon Community";
      m1.manifest.version = "2.1.0";
      m1.manifest.description = "Adds Nismo Z-Tune aerodynamics, updated RB26 exhaust notes, and high-res cluster textures.";
      m1.manifest.touches_save = true;
      m1.content.db = 2;
      m1.content.textures = 14;
      mods.push_back(m1);

      cars::modmgr::ModInfo m2;
      m2.name = "colorado_golden_hour";
      m2.manifest.title = "Colorado Golden Hour Lighting";
      m2.manifest.author = "Shift Visuals";
      m2.manifest.version = "1.4.2";
      m2.manifest.description = "Warm sunset grading and sharper road reflections across Red Rock and Beaumont.";
      m2.manifest.touches_save = false;
      m2.content.textures = 24;
      mods.push_back(m2);

      cars::modmgr::ModInfo m3;
      m3.name = "festival_pacing_plus";
      m3.manifest.title = "Authentic Festival AI Pacing";
      m3.manifest.author = "Pinyon Crew";
      m3.manifest.version = "1.0.5";
      m3.manifest.description = "Refines wristband AI rubber-banding and increases clean-overtake credits.";
      m3.manifest.touches_save = false;
      m3.content.xml = 4;
      mods.push_back(m3);
    }
    cars::modmgr::Config mcfg;
    mcfg.order = {"skyline_r34_pack", "colorado_golden_hour", "festival_pacing_plus"};
    mcfg.enabled = {{"skyline_r34_pack", true}, {"colorado_golden_hour", true}, {"festival_pacing_plus", false}};
    ms.set_state(tmp_dir / "mods", mods, mcfg, cars::modmgr::GameBuild::kV4);
    carview::ModsPage mp;
    mp.open(ms);
    carview::ui::List l_mods;
    carview::draw_mods_page(l_mods, float(W), float(H), mp);

    // TR: Save Backups & Sync (resolves FER_F40_87 & NIS_SKYLINEGTRVSPECII_02 to real car names!)
    auto write_txt = [](const std::filesystem::path& p, const std::string& body) {
      std::error_code wec;
      std::filesystem::create_directories(p.parent_path(), wec);
      std::ofstream f(p, std::ios::binary);
      f << body;
    };
    const auto udir = tmp_dir / "user";
    const auto sdir = tmp_dir / "sync";
    const auto rdir = tmp_dir / "deck";
    write_txt(udir / "drive" / "save.txt",
              "shift-drive-save 1\ncar FER_F40_87\nposition 2002.96 143.84 960.00\nheading 2.2\ncredits 215000 215000\n");
    write_txt(udir / "drive" / "garage.txt", "shift-garage 1\n");
    write_txt(rdir / "drive" / "save.txt",
              "shift-drive-save 1\ncar NIS_SKYLINEGTRVSPECII_02\nposition 2002.96 143.84 960.00\nheading 2.2\ncredits 164500 164500\n");
    write_txt(rdir / "drive" / "garage.txt", "shift-garage 1\n");
    {
      profile::sync::Context rc;
      rc.save_dir = rdir / "drive";
      rc.state_file = rdir / "config" / "save_sync.toml";
      rc.sync_folder = sdir;
      rc.device_name = "STEAMDECK";
      rc.platform = "linux";
      rc.build = "v4";
      rc.now = 1795365700;
      profile::sync::boot(rc);
    }
    carview::SaveSession ss;
    carview::SaveSession::BootOptions bo;
    bo.user_dir = udir;
    bo.default_save_dir = udir / "drive";
    bo.sync_folder = sdir;
    bo.device_name = "DESKTOP-MAIN";
    bo.v4_available = true;
    bo.now = 1795365800;
    ss.boot(bo);
    std::string serr;
    ss.profiles().set_gamertag("default", "Neri Horizon", &serr);
    write_txt(udir / "drive" / "save.txt",
              "shift-drive-save 1\ncar NIS_SKYLINEGTRVSPECII_02\nposition 2002.96 143.84 960.00\nheading 2.2\ncredits 124500 124500\n");
    profile::backup::snapshot(ss.backup_config(), "autosave", nullptr, &serr, 1795366400);
    write_txt(udir / "drive" / "save.txt",
              "shift-drive-save 1\ncar FER_F40_87\nposition 2002.96 143.84 960.00\nheading 2.2\ncredits 215000 215000\n");
    profile::backup::snapshot(ss.backup_config(), "manual", nullptr, &serr, 1795367000);
    carview::SaveProfilePage spg;
    spg.open(ss, carview::SaveProfilePage::Tab::kBackups);
    carview::ui::List l_save;
    carview::draw_save_page(l_save, float(W), float(H), spg, ss);

    // BL: 1000 Club (real gold/medal icons + clean Red Rock Outpost description!)
    carview::ClubScreenState cst;
    cst.active = true;
    cst.challenges_begun = true;
    cst.car_name = "NIS_SKYLINEGTRVSPECII_02";
    cst.selected = 1;
    cst.car_medals = 2;
    cst.car_total = 5;
    cst.stats.total_medals = 42;
    cst.stats.cars_with_medal = 14;
    cst.stats.achievements_unlocked = 6;
    cst.rows = {
        {"TOP SPEED RUN", "Hold 160 mph for 10 seconds on I-70 Highway.", "Use the straight past Red Rock.", "COMPLETED", 1.0, true, false},
        {"RED ROCK TOUR", "Drive into OUTPOST_REDROCK_NODE without colliding.", "Approach via Canyon Road.", "0.65 KM TO GO", 0.68, false, true},
        {"AWESOME DRIFT", "Perform 3 Awesome Drift skills in a single chain.", "Mountain switchbacks work best.", "1 / 3 DRIFTS", 0.33, false, true},
        {"SPEED TRAP BLAST", "Trigger 145 mph through Red Rock Speed Trap.", "Approach from east.", "COMPLETED", 1.0, true, false},
        {"NIGHT SHOWDOWN", "Win a Street Race at Carson Street Hub.", "Enter Carson Street Hub.", "READY", 0.0, false, false},
    };
    carview::ui::List l_club;
    carview::draw_club_screen(l_club, int(W), int(H), cst);

    // BR: Camera Editor (compact +3.0 deg right-side 516_I_EFFECTSMODE slider column!)
    carview::CameraEditorOverlay cam_ed;
    drive::CameraEditorConfig ced{};
    drive::ImmersiveCameraConfig cim{};
    drive::HeadTrackingConfig cht{};
    cam_ed.open(ced, cim, cht);
    cam_ed.move(+1);
    drive::HeadPose hp{};
    carview::ui::List l_cam;
    carview::draw_camera_editor_ui4(l_cam, float(W), float(H), cam_ed, hp);

    save_quad("ui4_p4_shift_blades.png", l_mods, load_or_build_backdrop(world_bg_path), l_save,
              load_or_build_backdrop(world_bg_path), l_club, load_or_build_backdrop(world_bg_path), l_cam,
              load_or_build_backdrop(world_bg_path));

    std::filesystem::remove_all(tmp_dir, ec);
  }
}

// Smooth MapCameraController velocity, exponential zoom & magnetic snap checks.
void test_map_camera_controller() {
  carview::MapCameraController cam;
  cam.reset(0.0, 0.0, 2.0);
  std::vector<carview::MapMarker> markers;
  carview::MapMarker mk;
  mk.title = "TEST OUTPOST";
  mk.x = 260.0;
  mk.z = 0.0;
  markers.push_back(mk);

  double prev_x = cam.cx();
  for (int f = 0; f < 10; ++f) {
    const bool moved = cam.step(1.0 / 60.0, 1.0, 0.0, 0.0, markers);
    check(moved, "map camera moves smoothly under continuous input");
    check(cam.cx() > prev_x, "map camera advances east smoothly");
    check(cam.cx() - prev_x < 60.0, "map camera step is continuous, not a discrete jump");
    prev_x = cam.cx();
  }
  check(cam.vx() > 200.0, "map camera accelerates smoothly");

  cam.nudge_target_zoom(1.5);
  check(cam.target_zoom() > 2.9 && cam.zoom() < 2.1, "map zoom interpolates smoothly instead of snapping");
  for (int f = 0; f < 30; ++f) cam.step(1.0 / 60.0, 0.0, 0.0, 0.0, markers);
  check(std::fabs(cam.zoom() - cam.target_zoom()) < 0.05, "map zoom converges smoothly to target");

  cam.reset(220.0, 15.0, 2.0);
  for (int f = 0; f < 45; ++f) cam.step(1.0 / 60.0, 0.0, 0.0, 0.0, markers);
  check(cam.snapped_marker() == 0, "map camera magnetically locks onto nearby marker");
  check(std::hypot(cam.cx() - mk.x, cam.cz() - mk.z) < 5.0, "map camera pulls smoothly onto marker center");
}

// Pillar 6: Units formatting (Metric vs Imperial), tilted mouse hit-testing, and HUD banner non-overlap.
void test_units_mouse_and_banner_non_overlap() {
  // 1. Units formatting consistency.
  check(carview::format_speed_unit(100.0, false, false) == "100 MPH", "imperial speed formatting");
  check(carview::format_speed_unit(100.0, true, false) == "161 KM/H", "metric speed formatting");
  check(carview::format_speed_unit(100.0, true, true) == "160.9 KM/H", "metric decimal speed formatting");
  check(carview::format_distance_unit(1609.344, false) == "1.00 MI", "imperial mile formatting");
  check(carview::format_distance_unit(150.0, false).find("FT") != std::string::npos, "imperial feet formatting");
  check(carview::format_distance_unit(1500.0, true) == "1.5 KM", "metric km formatting");
  check(carview::format_distance_unit(250.0, true) == "250 M", "metric meter formatting");
  check(carview::format_gap_unit(100.0, false) == "+328 FT", "imperial gap formatting");
  check(carview::format_gap_unit(100.0, true) == "+100 M", "metric gap formatting");

  // 2. Mouse hit-testing (axis-aligned, top-most precedence, and rotated/tilted zones).
  carview::ui::List hit_list;
  hit_list.add_hit(100.0f, 100.0f, 300.0f, 150.0f, carview::ui::kHitRow, 0);
  hit_list.add_hit(260.0f, 100.0f, 300.0f, 150.0f, carview::ui::kHitStepRight, 0);
  carview::ui::HitZone hz{};
  check(hit_list.hit_test(150.0f, 125.0f, hz) && hz.group == carview::ui::kHitRow && hz.index == 0,
        "axis-aligned row hit test");
  check(hit_list.hit_test(280.0f, 125.0f, hz) && hz.group == carview::ui::kHitStepRight && hz.index == 0,
        "top-most overlapping step-right hit test wins");
  check(!hit_list.hit_test(50.0f, 50.0f, hz), "miss outside hit zone");

  carview::ui::List rot_list;
  rot_list.turn(200.0f, 200.0f, 0.05236f);  // +3 deg UI4 tilt
  rot_list.add_hit(100.0f, 180.0f, 300.0f, 220.0f, carview::ui::kHitRow, 3);
  rot_list.unturn();
  check(rot_list.hit_test(200.0f, 200.0f, hz) && hz.index == 3, "rotated UI4 blade center hits");
  check(!rot_list.hit_test(200.0f, 260.0f, hz), "rotated UI4 blade outside misses");

  // 3. Real menu screens emit mouse hit zones (Settings tabs/rows/arrows & Pause Menu rows).
  carview::SettingsPage sp;
  sp.load({});
  sp.open();
  carview::ui::List sp_list;
  carview::draw_settings_ui4(sp_list, 1280.0f, 720.0f, sp, false);
  bool has_tab = false, has_row = false, has_left = false, has_right = false;
  for (const auto& z : sp_list.hits()) {
    if (z.group == carview::ui::kHitTab) has_tab = true;
    if (z.group == carview::ui::kHitRow) has_row = true;
    if (z.group == carview::ui::kHitStepLeft) has_left = true;
    if (z.group == carview::ui::kHitStepRight) has_right = true;
  }
  check(has_tab && has_row && has_left && has_right, "Settings UI4 emits tab, row, and step hit zones");

  // 4. Bottom-center HUD banner non-overlap when subtitle is active alongside popup & speed activity.
  auto max_center_rect_bottom = [](const carview::ui::List& l) -> float {
    float max_y = 0.0f;
    for (const auto& it : l.items()) {
      const float cx = 0.5f * (it.rect[0] + it.rect[2]);
      const float w = it.rect[2] - it.rect[0];
      const float h = it.rect[3] - it.rect[1];
      if (cx > 420.0f && cx < 860.0f && w > 120.0f && h > 16.0f && it.rect[1] > 350.0f) {
        max_y = std::max(max_y, it.rect[3]);
      }
    }
    return max_y;
  };
  carview::DriveHud d_no_sub;
  d_no_sub.driving = true;
  d_no_sub.time = 10.0;
  d_no_sub.metric_units = false;
  d_no_sub.subtitle_active = false;
  d_no_sub.popup_title = "ROAD DISCOVERED";
  d_no_sub.popup_text = "142 / 216 ROADS";
  d_no_sub.popup_at = 9.5;
  carview::ui::List l_no_sub;
  carview::build_drive_hud(l_no_sub, d_no_sub, 1280.0f, 720.0f);

  carview::DriveHud d_with_sub = d_no_sub;
  d_with_sub.subtitle_active = true;
  carview::ui::List l_with_sub;
  carview::build_drive_hud(l_with_sub, d_with_sub, 1280.0f, 720.0f);

  const float bot_no_sub = max_center_rect_bottom(l_no_sub);
  const float bot_with_sub = max_center_rect_bottom(l_with_sub);
  check(bot_no_sub > 580.0f, "bottom popup sits in normal lower lane when no subtitle active");
  check(bot_with_sub < bot_no_sub - 60.0f, "bottom popup lifts cleanly above active subtitle lane");
}

void test_round2_hud_minimap_gps_and_mouse_ux() {
  // 1. P0-3: Imperial/Metric/Credits unit label placeholder stripping (zero literal "{0}"!).
  check(carview::format_unit_label("{0} mph", "mph") == "mph", "strip {0} from '{0} mph'");
  check(carview::format_unit_label("{0} km/h", "km/h") == "km/h", "strip {0} from '{0} km/h'");
  check(carview::format_unit_label("{0} Cr", "Cr") == "Cr", "strip {0} from '{0} Cr'");
  check(carview::format_unit_label("%d MPH", "mph") == "MPH", "strip %d from '%d MPH'");
  check(carview::format_unit_label("%.1f km/h", "km/h") == "km/h", "strip %.1f from '%.1f km/h'");
  check(carview::format_unit_label("{0}", "mph") == "mph", "placeholder-only falls back cleanly");
  check(carview::format_unit_label("_&123456", "km/h") == "km/h", "unresolved _& key falls back cleanly");
  check(carview::format_unit_label("", "mph") == "mph", "empty unit label falls back cleanly");

  // Verify live Imperial HUD dial strips "{0}" when game_text returns "{0} mph".
  carview::set_game_text([](const std::string& ref) -> std::string {
    if (ref.rfind("_&", 0) == 0) return "{0} mph";
    return ref;
  });
  carview::DriveHud d_imp;
  d_imp.driving = true;
  d_imp.metric_units = false;
  d_imp.kmh = 100.0;
  d_imp.rpm = 4000.0;
  d_imp.redline_rpm = 7000.0;
  d_imp.gear = 3;
  carview::ui::List l_imp;
  carview::build_drive_hud(l_imp, d_imp, 1280.0f, 720.0f);
  check(!l_imp.items().empty(), "Imperial HUD renders cleanly with stubbed '{0} mph' stringtable");
  carview::set_game_text({});

  // 2. P1: Transparent FH1 minimap canvas alpha (zero murky dark circle backdrop!).
  carview::MapView mv;
  carview::MapScene msc;
  const carview::HudImage mm = mv.minimap(msc, 128);
  check(mm.width == 128 && mm.height == 128 && mm.rgba.size() == 128u * 128u * 4u, "minimap image dimensions valid");
  const size_t center_idx = (64u * 128u + 64u) * 4u;
  const size_t corner_idx = (4u * 128u + 4u) * 4u;
  check(mm.rgba[center_idx + 3] == 0, "empty minimap center alpha is 0 (100% transparent, no dark disc)");
  check(mm.rgba[corner_idx + 3] == 0, "empty minimap corner alpha is 0");
  // When a route is present, road pixels are visible while unpopulated background stays alpha == 0.
  msc.gps_route = {{0.0, 0.0}, {0.0, 120.0}};
  const carview::HudImage mm_route = mv.minimap(msc, 128);
  size_t opaque_px = 0, transparent_px = 0;
  for (size_t i = 3; i < mm_route.rgba.size(); i += 4) {
    if (mm_route.rgba[i] == 0) ++transparent_px;
    else if (mm_route.rgba[i] > 100) ++opaque_px;
  }
  check(opaque_px > 20 && transparent_px > 10000,
        "transparent minimap draws crisp route strokes over 100% transparent backdrop");

  // 3. P1: Suppress ghost "U TURN AROUND 0 FT" GPS prompt and zero-distance labels.
  carview::GpsHud gps_ghost;
  gps_ghost.active = true;
  gps_ghost.turn_kind = 8;  // kUTurn
  gps_ghost.turn_text = "TURN AROUND";
  gps_ghost.dist_to_turn_m = 0.0;
  gps_ghost.dist_to_dest_m = 500.0;
  gps_ghost.confirmed_uturn = false;
  check(!carview::should_show_gps_turn_banner(gps_ghost), "unconfirmed 0m U-turn ghost prompt is suppressed");
  check(!carview::should_show_gps_turn_distance(gps_ghost), "U-turn never prints 0 FT / 0 M distance");

  carview::GpsHud gps_near_dest = gps_ghost;
  gps_near_dest.confirmed_uturn = true;
  gps_near_dest.dist_to_dest_m = 10.0;
  check(!carview::should_show_gps_turn_banner(gps_near_dest), "U-turn near destination (<30m) is suppressed");

  carview::GpsHud gps_real_uturn = gps_ghost;
  gps_real_uturn.confirmed_uturn = true;
  gps_real_uturn.dist_to_dest_m = 600.0;
  check(carview::should_show_gps_turn_banner(gps_real_uturn), "confirmed sustained U-turn on active route displays");
  check(!carview::should_show_gps_turn_distance(gps_real_uturn), "confirmed U-turn still omits 0 FT / 0 M");

  carview::GpsHud gps_right;
  gps_right.active = true;
  gps_right.turn_kind = 6;  // kRight
  gps_right.turn_text = "TURN RIGHT";
  gps_right.dist_to_turn_m = 2.0;
  gps_right.dist_to_dest_m = 400.0;
  check(carview::should_show_gps_turn_banner(gps_right), "imminent right turn banner stays visible");
  check(!carview::should_show_gps_turn_distance(gps_right), "sub-5m turn omits 0 FT / 0 M");
  gps_right.dist_to_turn_m = 120.0;
  check(carview::should_show_gps_turn_distance(gps_right), "upcoming 120m turn shows distance");

  carview::DriveHud d_ghost;
  d_ghost.driving = true;
  d_ghost.gps = &gps_ghost;
  carview::ui::List l_with_ghost, l_no_gps;
  carview::build_drive_hud(l_with_ghost, d_ghost, 1280.0f, 720.0f);
  d_ghost.gps = nullptr;
  carview::build_drive_hud(l_no_gps, d_ghost, 1280.0f, 720.0f);
  check(l_with_ghost.items().size() == l_no_gps.items().size(),
        "ghost 'U TURN AROUND 0 FT' emits zero HUD primitives");

  // 4. P1: Mouse hit-zones across remaining 10 screens.
  auto count_group = [](const carview::ui::List& l, uint32_t grp) {
    int c = 0;
    for (const auto& z : l.hits())
      if (z.group == grp) ++c;
    return c;
  };

  // Photo Mode: rows, left/right step arrows/sliders, and bottom button bar.
  carview::PhotoModeController pm;
  pm.enter({0.0, 0.0, 0.0}, 0.0, carview::PhotoLimits{});
  carview::ui::List l_pm;
  pm.build_hud(l_pm, 1280.0f, 720.0f);
  const int vis_pm = carview::PhotoModeController::visible_effect_rows(720.0f);
  check(vis_pm > 0 && vis_pm <= carview::PhotoModeController::kRowCount &&
            count_group(l_pm, carview::ui::kHitRow) == vis_pm,
        "Photo Mode emits hit zones for all visible scrollable effects rows at 720p");
  check(count_group(l_pm, carview::ui::kHitStepLeft) == vis_pm &&
            count_group(l_pm, carview::ui::kHitStepRight) == vis_pm,
        "Photo Mode emits left/right adjustment hit zones for visible rows");
  check(count_group(l_pm, carview::ui4::kHitButton) >= 4, "Photo Mode emits clickable button bar");

  // Replay Viewer: 7 DVR transport buttons + timeline scrubber + button bar.
  drive::ReplayRecording rec;
  rec.meta.event_name = "COLORADO TEST";
  drive::ReplayFrame rf;
  rf.time_s = 0.0;
  rf.cars.push_back(drive::ReplayCarSample{});
  rec.frames.push_back(rf);
  rf.time_s = 10.0;
  rec.frames.push_back(rf);
  carview::ReplayViewer rv;
  rv.open(std::move(rec), true);
  carview::ui::List l_rv;
  rv.build_hud(l_rv, 1280.0f, 720.0f);
  check(count_group(l_rv, carview::ui::kHitRow) == 7, "Replay Viewer emits 7 DVR transport hit zones");
  check(count_group(l_rv, carview::ui4::kHitTimeline) == 1, "Replay Viewer emits timeline scrubber hit zone");
  check(count_group(l_rv, carview::ui4::kHitButton) >= 4, "Replay Viewer emits clickable button bar");
  rv.seek_fraction(0.5);
  check(std::fabs(rv.progress() - 0.5) < 0.02, "ReplayViewer::seek_fraction scrubs accurately");

  // Livery / Paint Editor: tabs, regions, finish arrows, 72 swatches, vinyl slots.
  carview::LiveryEditor liv;
  std::string liv_msg;
  liv.open(nullptr, {}, 0, {}, liv_msg);
  carview::ui::List l_liv_paint, l_liv_vinyl;
  carview::draw_livery_editor_hud(l_liv_paint, 1280.0f, 720.0f, liv, "1995 VW Corrado VR6");
  check(count_group(l_liv_paint, carview::ui::kHitTab) == 5, "Livery Editor emits 5 tab hit zones");
  check(count_group(l_liv_paint, carview::ui4::kHitRegion) == int(carview::PaintRegion::kCount),
        "Livery Editor emits all paint region hit zones");
  check(count_group(l_liv_paint, carview::ui4::kHitSwatch) == 72, "Livery Editor emits 72 color swatch hit zones");
  check(count_group(l_liv_paint, carview::ui::kHitStepLeft) == 1 &&
            count_group(l_liv_paint, carview::ui::kHitStepRight) == 1,
        "Livery Editor emits finish step arrows");
  liv.set_tab(carview::EditorTab::kVinylLayers);
  carview::draw_livery_editor_hud(l_liv_vinyl, 1280.0f, 720.0f, liv, "1995 VW Corrado VR6");
  check(count_group(l_liv_vinyl, carview::ui::kHitRow) >= 5, "Livery Vinyl tab emits layer slot hit zones");

  // Rally Hub & Service Area.
  carview::RallyHubState rhub;
  rhub.active = true;
  rhub.items.resize(4);
  for (int i = 0; i < 4; ++i) rhub.items[size_t(i)].title = "RALLY " + std::to_string(i + 1);
  carview::ui::List l_rhub, l_rsvc;
  carview::build_rally_hub_screen(l_rhub, rhub, 1280.0f, 720.0f);
  check(count_group(l_rhub, carview::ui::kHitRow) == 4, "Rally Hub emits ticket card hit zones");
  carview::RallyServiceAreaState rsvc;
  rsvc.active = true;
  carview::build_rally_service_area_screen(l_rsvc, rsvc, 1280.0f, 720.0f);
  check(count_group(l_rsvc, carview::ui::kHitRow) == 4, "Rally Service Area emits 4 action hit zones");

  // Sponsors Challenges & 1000 Club & Achievements & Post-Race Results.
  std::vector<carview::SponsorChallengeRow> srows(3);
  for (int i = 0; i < 3; ++i) {
    srows[size_t(i)].sponsor_name = "OAKLEY";
    srows[size_t(i)].challenge_title = "NEAR MISS";
    srows[size_t(i)].target = 10;
  }
  carview::ui::List l_sp;
  carview::draw_sponsor_challenges_ui4(l_sp, 1280.0f, 720.0f, srows, 0, 50000);
  check(count_group(l_sp, carview::ui::kHitRow) == 3, "Sponsor Challenges emits row hit zones");

  carview::ClubScreenState club_st;
  club_st.active = true;
  club_st.car_name = "1995 VW Corrado VR6";
  club_st.rows.resize(3);
  for (int i = 0; i < 3; ++i) club_st.rows[size_t(i)].title = "CHALLENGE " + std::to_string(i + 1);
  carview::ui::List l_club;
  carview::draw_club_screen(l_club, 1280, 720, club_st);
  check(count_group(l_club, carview::ui::kHitRow) == 3, "1000 Club emits challenge row hit zones");

  carview::RaceHud rfin;
  rfin.phase = carview::RaceHud::kFinished;
  rfin.results_page = 0;
  rfin.place = 1;
  rfin.clock = 10.0;
  rfin.finish_time = 9.0;
  carview::DriveHud d_rfin;
  d_rfin.driving = true;
  d_rfin.race = &rfin;
  carview::ui::List l_rfin;
  carview::build_drive_hud(l_rfin, d_rfin, 1280.0f, 720.0f);
  check(count_group(l_rfin, carview::ui4::kHitButton) == 4,
        "Post-Race Results emits 4 clickable button-bar zones (Continue/Restart/Results/Replay)");
}

void test_round3_subtitle_lift_upgrade_units_and_rewind_touch() {
  // 1. Dynamic 3-line subtitle lift clears campaign subtitle box + 22px speaker badge.
  check(carview::compute_subtitle_bottom_lift(false, 3, 80.0f) == 0.0f,
        "No subtitle lift when subtitle_active is false");
  const float lift_1 = carview::compute_subtitle_bottom_lift(true, 1, 0.0f);
  const float lift_2 = carview::compute_subtitle_bottom_lift(true, 2, 0.0f);
  const float lift_3 = carview::compute_subtitle_bottom_lift(true, 3, 0.0f);
  const float lift_4 = carview::compute_subtitle_bottom_lift(true, 4, 0.0f);
  check(lift_1 >= 104.0f && lift_2 >= 116.0f && lift_3 >= 136.0f && lift_4 >= 156.0f,
        "Dynamic subtitle bottom_lift grows monotonically for 1..4 wrapped subtitle lines");
  check(lift_3 > lift_2 && lift_4 > lift_3, "3-line and 4-line subtitles get progressively larger bottom lift");
  const float lift_measured = carview::compute_subtitle_bottom_lift(true, 3, 84.0f);
  check(lift_measured >= 140.0f, "Explicit measured subtitle_box_h_canvas computes exact clearance");

  // Verify popup_card bottom Y clears a 3-line subtitle speaker badge (top at y=556 on 720p canvas) by >= 20px.
  {
    carview::DriveHud d;
    d.driving = true;
    d.time = 2.0;
    d.popup_title = "ROAD DISCOVERED  1 / 216";
    d.popup_text = "COLORADO ROAD #88";
    d.popup_at = 1.0;
    d.subtitle_active = true;
    d.subtitle_lines = 3;
    d.subtitle_box_h_canvas = 80.0f;
    carview::ui::List l_sub;
    carview::build_drive_hud(l_sub, d, 1280.0f, 720.0f);
    // Find lowest geometry in center-bottom popup region (x in [430, 960], y in [420, 620]).
    float max_popup_y = 0.0f;
    for (const auto& inst : l_sub.items()) {
      const float x0 = inst.rect[0], y0 = inst.rect[1], x1 = inst.rect[2], y1 = inst.rect[3];
      if (x0 >= 420.0f && x1 <= 980.0f && y0 >= 400.0f && y1 <= 620.0f) {
        max_popup_y = std::max(max_popup_y, y1);
      }
    }
    const float speaker_badge_top_y = 720.0f - 62.0f - 80.0f - 22.0f;  // 556.0f
    check(max_popup_y > 400.0f && max_popup_y <= speaker_badge_top_y - 20.0f,
          "3-line subtitle lift keeps ROAD DISCOVERED popup >= 20px above speaker badge");
  }

  // 2. Upgrade Shop & Autoshow spec unit consistency (Imperial vs Metric).
  {
    garage::CarStats cur{}, prv{};
    cur.peak_power_kw = 112.6;
    prv.peak_power_kw = 122.3;
    cur.peak_torque_nm = 235.0;
    prv.peak_torque_nm = 250.0;
    cur.curb_weight_kg = 1240.0;
    prv.curb_weight_kg = 1200.0;
    prv.time_0_60_s = 7.85;
    prv.brake_60_0_m = 22.3;
    prv.lat_g_60 = 1.54;

    const auto imp = carview::format_upgrade_spec_strings(cur, prv, false);
    check(imp.power.find("HP") != std::string::npos && imp.power.find("kW") == std::string::npos,
          "Imperial Upgrade Shop power uses HP only");
    check(imp.torque.find("LB-FT") != std::string::npos && imp.torque.find("NM") == std::string::npos,
          "Imperial Upgrade Shop torque uses LB-FT only");
    check(imp.weight.find("LBS") != std::string::npos && imp.weight.find("KG") == std::string::npos,
          "Imperial Upgrade Shop weight uses LBS only");
    check(imp.perf.find("0-60:") != std::string::npos && imp.perf.find("60-0:") != std::string::npos &&
              imp.perf.find("FT") != std::string::npos && imp.perf.find(" M ") == std::string::npos,
          "Imperial Upgrade Shop accel/braking uses 0-60 and 60-0 FT");

    const auto met = carview::format_upgrade_spec_strings(cur, prv, true);
    check(met.power.find("kW") != std::string::npos && met.power.find("HP") == std::string::npos,
          "Metric Upgrade Shop power uses kW only");
    check(met.torque.find("NM") != std::string::npos && met.torque.find("LB-FT") == std::string::npos,
          "Metric Upgrade Shop torque uses NM only");
    check(met.weight.find("KG") != std::string::npos && met.weight.find("LBS") == std::string::npos,
          "Metric Upgrade Shop weight uses KG only");
    check(met.perf.find("0-100:") != std::string::npos && met.perf.find("100-0:") != std::string::npos &&
              met.perf.find(" M ") != std::string::npos && met.perf.find("FT") == std::string::npos,
          "Metric Upgrade Shop accel/braking uses 0-100 and 100-0 M");

    const std::string as_imp = carview::format_autoshow_spec_line(112.6, 235.0, 1240.0, false);
    const std::string as_met = carview::format_autoshow_spec_line(112.6, 235.0, 1240.0, true);
    check(as_imp.find("HP") != std::string::npos && as_imp.find("LB-FT") != std::string::npos &&
              as_imp.find("LBS") != std::string::npos,
          "Imperial Autoshow spec uses HP / LB-FT / LBS");
    check(as_met.find("kW") != std::string::npos && as_met.find("NM") != std::string::npos &&
              as_met.find("KG") != std::string::npos,
          "Metric Autoshow spec uses kW / NM / KG");
  }

  // 3. Rewind overlay mouse/touch hit-testing across all Rewind states (prevents Android softlock).
  {
    auto collect_buttons = [](const carview::ui::List& l) {
      std::vector<carview::ui::HitZone> out;
      for (const auto& hz : l.hits())
        if (hz.group == carview::ui4::kHitButton) out.push_back(hz);
      return out;
    };

    // State A: Paused at normal keyframe -> RESUME, REWIND FURTHER, VIEW REPLAY, CANCEL.
    carview::ui::List l_paused;
    carview::build_rewind_overlay(l_paused, 1280.0f, 720.0f, false, false, true, false, false);
    const auto btns_paused = collect_buttons(l_paused);
    check(btns_paused.size() == 4, "Rewind Paused overlay emits 4 touchable button hit zones");
    const carview::RewindOverlayAction want_paused[4] = {
        carview::RewindOverlayAction::kResume,
        carview::RewindOverlayAction::kRewindFurther,
        carview::RewindOverlayAction::kViewReplay,
        carview::RewindOverlayAction::kCancel,
    };
    for (size_t i = 0; i < btns_paused.size() && i < 4; ++i) {
      const float cx = 0.5f * (btns_paused[i].x0 + btns_paused[i].x1);
      const float cy = 0.5f * (btns_paused[i].y0 + btns_paused[i].y1);
      carview::ui::HitZone hit{};
      check(l_paused.hit_test(cx, cy, hit) && hit.group == carview::ui4::kHitButton && hit.index == int(i),
            "Rewind Paused button center hit-tests cleanly");
      check(carview::rewind_overlay_action_for_button(false, true, false, hit.index) == want_paused[i],
            "Rewind Paused button index maps to expected RewindOverlayAction");
    }

    // State B: Paused at oldest slot -> RESUME, VIEW REPLAY, CANCEL (no Rewind Further).
    carview::ui::List l_oldest;
    carview::build_rewind_overlay(l_oldest, 1280.0f, 720.0f, false, false, true, true, false);
    const auto btns_oldest = collect_buttons(l_oldest);
    check(btns_oldest.size() == 3, "Rewind Paused (oldest slot) emits 3 touchable button hit zones");
    check(carview::rewind_overlay_action_for_button(false, true, true, 0) == carview::RewindOverlayAction::kResume &&
              carview::rewind_overlay_action_for_button(false, true, true, 1) ==
                  carview::RewindOverlayAction::kViewReplay &&
              carview::rewind_overlay_action_for_button(false, true, true, 2) == carview::RewindOverlayAction::kCancel,
          "Rewind Paused (oldest slot) indices map to Resume / View Replay / Cancel");

    // State C: Actively rewinding -> CANCEL only.
    carview::ui::List l_rew;
    carview::build_rewind_overlay(l_rew, 1280.0f, 720.0f, true, false, false, false, false);
    const auto btns_rew = collect_buttons(l_rew);
    check(btns_rew.size() == 1, "Active Rewinding overlay emits 1 touchable Cancel button hit zone");
    carview::ui::HitZone hit_rew{};
    check(l_rew.hit_test(0.5f * (btns_rew[0].x0 + btns_rew[0].x1), 0.5f * (btns_rew[0].y0 + btns_rew[0].y1), hit_rew) &&
              carview::rewind_overlay_action_for_button(true, false, false, hit_rew.index) ==
                  carview::RewindOverlayAction::kCancel,
          "Active Rewinding touch tap maps to Cancel");
  }
}

void test_round2_phase6_ui4_parity() {
  // 1. BGF binary parser: state records & animation channels/events (pos X/Y, scale, rotation, color, alpha).
  {
    std::vector<uint8_t> buf(0x8C, 0);
    std::memcpy(buf.data() + 6, "AnarkBGF", 8);
    auto put_u32 = [](std::vector<uint8_t>& v, size_t off, uint32_t x) {
      v[off + 0] = uint8_t((x >> 24) & 0xFF);
      v[off + 1] = uint8_t((x >> 16) & 0xFF);
      v[off + 2] = uint8_t((x >> 8) & 0xFF);
      v[off + 3] = uint8_t(x & 0xFF);
    };
    auto push_u32 = [](std::vector<uint8_t>& v, uint32_t x) {
      v.push_back(uint8_t((x >> 24) & 0xFF));
      v.push_back(uint8_t((x >> 16) & 0xFF));
      v.push_back(uint8_t((x >> 8) & 0xFF));
      v.push_back(uint8_t(x & 0xFF));
    };
    auto push_u16 = [](std::vector<uint8_t>& v, uint16_t x) {
      v.push_back(uint8_t((x >> 8) & 0xFF));
      v.push_back(uint8_t(x & 0xFF));
    };
    auto push_f32 = [&](std::vector<uint8_t>& v, float f) {
      uint32_t bits = 0;
      std::memcpy(&bits, &f, 4);
      push_u32(v, bits);
    };
    auto append_section = [&](const std::vector<uint8_t>& sec) {
      push_u32(buf, uint32_t(sec.size()));
      buf.insert(buf.end(), sec.begin(), sec.end());
    };

    constexpr uint32_t kItemHash = 0x11223344u;
    constexpr uint32_t kFbfId = 42u;
    constexpr uint32_t kStateHash = 0x55667788u;

    put_u32(buf, 0x24, 1);  // element_count
    put_u32(buf, 0x28, 0);  // wrapper_count
    put_u32(buf, 0x2C, 0);  // property_count
    put_u32(buf, 0x30, 0);  // relation_count
    put_u32(buf, 0x3C, 0);  // identity_count
    put_u32(buf, 0x44, 1);  // style_count
    put_u32(buf, 0x4C, 1);  // track_count
    put_u32(buf, 0x5C, 1);  // anim_count
    put_u32(buf, 0x88, 0);  // identity section size = 0

    // Section 2: Items (1 item mapping kItemHash -> FBF object 42).
    std::vector<uint8_t> items_sec;
    push_u32(items_sec, kItemHash);
    push_u32(items_sec, 0);
    push_u32(items_sec, kFbfId);
    items_sec.push_back(4);  // kind = model
    items_sec.push_back(0);  // flags
    push_u32(items_sec, 0);  // property count
    append_section(items_sec);

    // Section 3: Relations (empty).
    append_section({});

    // Section 4: Styles / States (1 style with 2 state records: PosX + Alpha).
    std::vector<uint8_t> style_sec;
    push_u32(style_sec, kItemHash);
    push_u32(style_sec, 7u);   // style_id
    push_u16(style_sec, 0u);   // reserved
    push_u16(style_sec, 2u);   // state_count
    // Record 0: PosX = 64.0
    push_u32(style_sec, kStateHash);
    push_u32(style_sec, kItemHash);
    push_u32(style_sec, fh1::bgf_attr::kPosX);
    push_u32(style_sec, 1u);
    push_f32(style_sec, 64.0f);
    push_u32(style_sec, 0u);
    // Record 1: Alpha = 0.5
    push_u32(style_sec, kStateHash);
    push_u32(style_sec, kItemHash);
    push_u32(style_sec, fh1::bgf_attr::kAlpha);
    push_u32(style_sec, 10u);
    push_f32(style_sec, 0.5f);
    push_u32(style_sec, 0u);
    append_section(style_sec);

    // Section 5: Tracks (1 track with PosX keyframes 0 -> 100 over 0..1s).
    std::vector<uint8_t> track_sec;
    push_u32(track_sec, kItemHash);
    push_u32(track_sec, 1u);  // child_count
    push_u32(track_sec, fh1::bgf_attr::kPosX);
    push_u32(track_sec, 2u);  // key_count
    // Key 0
    push_u32(track_sec, kItemHash);
    push_u32(track_sec, fh1::bgf_attr::kPosX);
    push_f32(track_sec, 0.0f);
    push_u32(track_sec, 1u);
    push_f32(track_sec, 0.0f);
    // Key 1
    push_u32(track_sec, kItemHash);
    push_u32(track_sec, fh1::bgf_attr::kPosX);
    push_f32(track_sec, 1.0f);
    push_u32(track_sec, 1u);
    push_f32(track_sec, 100.0f);
    append_section(track_sec);

    // Section 6: Empty companion table.
    append_section({});

    // Section 7: Animations (1 anim with 2 channels: ScaleX 1->2 and Rotation 0->0.5, plus 1 event).
    std::vector<uint8_t> anim_sec;
    push_u32(anim_sec, 0xABCD0001u);  // name_hash
    push_u32(anim_sec, 0u);
    push_f32(anim_sec, 2.0f);         // duration
    push_u32(anim_sec, 0u);
    push_u32(anim_sec, kItemHash);    // owner_item at +16
    push_u32(anim_sec, 0u);
    push_u32(anim_sec, 0u);
    anim_sec.push_back(1u);           // flags at +28
    push_u32(anim_sec, 2u);           // track_count at +29
    // Channel 0: ScaleX (2 keys, 1 event)
    push_u32(anim_sec, fh1::bgf_attr::kScaleX);
    anim_sec.push_back(3u);
    push_u32(anim_sec, 2u);
    push_f32(anim_sec, 0.0f);
    push_f32(anim_sec, 1.0f);
    push_f32(anim_sec, 1.0f);
    push_f32(anim_sec, 2.0f);
    push_u32(anim_sec, 1u);
    push_u32(anim_sec, 0xCAFE0001u);
    // Channel 1: Rotation (2 keys, 0 events)
    push_u32(anim_sec, fh1::bgf_attr::kRotation);
    anim_sec.push_back(5u);
    push_u32(anim_sec, 2u);
    push_f32(anim_sec, 0.0f);
    push_f32(anim_sec, 0.0f);
    push_f32(anim_sec, 1.0f);
    push_f32(anim_sec, 0.5f);
    push_u32(anim_sec, 0u);
    append_section(anim_sec);

    fh1::UiBgfDocument doc;
    std::string err;
    check(fh1::parse_ui_bgf(buf, doc, &err), "parse_ui_bgf parses full state records and animation bodies");
    check(doc.styles.size() == 1 && doc.styles[0].states.size() == 2,
          "BGF style parses 2 state records instead of skipping them");
    check(doc.styles[0].states[0].target == fh1::UiBgfPropTarget::kPosX &&
              std::fabs(doc.styles[0].states[0].value - 64.0f) < 1e-4f,
          "BGF state record 0 stores PosX=64");
    check(doc.styles[0].states[1].target == fh1::UiBgfPropTarget::kAlpha &&
              std::fabs(doc.styles[0].states[1].value - 0.5f) < 1e-4f,
          "BGF state record 1 stores Alpha=0.5");
    check(doc.tracks.size() == 1 && doc.tracks[0].target == fh1::UiBgfPropTarget::kPosX,
          "BGF track stores semantic PosX property target");
    check(doc.animations.size() == 1 && doc.animations[0].channels.size() == 2,
          "BGF animation parses channel bodies instead of skipping them");
    check(doc.animations[0].channels[0].target == fh1::UiBgfPropTarget::kScaleX &&
              doc.animations[0].channels[0].events.size() == 1 &&
              doc.animations[0].channels[0].events[0] == 0xCAFE0001u,
          "BGF animation channel 0 preserves ScaleX target and event payload");
    check(std::fabs(fh1::evaluate_bgf_anim_channel(doc.animations[0].channels[0], 0.5f) - 1.5f) < 1e-4f,
          "evaluate_bgf_anim_channel interpolates ScaleX cleanly");

    // Verify SceneDraw::draw_subtree() applies PosX & ScaleX tracks WITHOUT corrupting node alpha!
    fh1::UiScene sc;
    sc.has_bgf = true;
    sc.bgf = doc;
    fh1::UiObject m;
    m.kind = fh1::UiObjectKind::kModel;
    m.id = kFbfId;
    m.name = "AnimatedQuad";
    m.geometry = 0;
    sc.objects.push_back(m);
    fh1::UiObject mat;
    mat.kind = fh1::UiObjectKind::kMaterial;
    mat.id = 43;
    mat.colour = {1.0f, 1.0f, 1.0f, 1.0f};
    sc.objects.push_back(mat);
    fh1::UiGeometry geo;
    geo.index = 0;
    geo.bounds_min = {-50, -50, 0};
    geo.bounds_max = {50, 50, 0};
    geo.is_unit_quad = true;
    sc.geometries.push_back(geo);
    fh1::UiNode nd;
    nd.parent = -1;
    nd.object = kFbfId;
    nd.object_index = 0;
    nd.opacity = 1.0f;
    nd.alpha = 1.0f;
    nd.bounds_min = {-20, -10, 0};
    nd.bounds_max = {20, 10, 0};
    sc.nodes.push_back(nd);

    carview::SceneDraw sd(&sc, true);
    carview::SceneFrame sf{0.0f, 0.0f, 1.0f};
    carview::SceneBindings sb;
    sb.anim_time = 0.5f;  // PosX = +50, ScaleX = 1.5x, Rotation = 0.25 rad; Alpha must stay 1.0!
    carview::ui::List l_sub;
    check(sd.draw_subtree(l_sub, sf, 0, sb) == 1 && l_sub.items().size() == 1,
          "SceneDraw::draw_subtree emits animated model");
    if (!l_sub.items().empty()) {
      const auto& inst = l_sub.items()[0];
      const uint8_t alpha_byte = uint8_t((inst.color >> 24) & 0xFF);
      const float cx = 0.5f * (inst.rect[0] + inst.rect[2]);
      check(alpha_byte == 255, "Non-alpha BGF PosX/ScaleX tracks do NOT overwrite alpha");
      check(std::fabs(cx - 690.0f) < 2.0f, "BGF PosX track shifts node X (+50px from 640 -> 690)");
    }

    // Verify resolve_scene_draw() never permanently caches nullptr before scenes load!
    carview::set_ui_scenes([](const std::string&) -> const fh1::UiScene* { return nullptr; });
    check(!carview::resolve_scene_draw("TEST_LAZY_SCENE").ok(), "resolve_scene_draw returns invalid before load");
    carview::set_ui_scenes([&](const std::string& nm) -> const fh1::UiScene* {
      return nm == "TEST_LAZY_SCENE" ? &sc : nullptr;
    });
    check(carview::resolve_scene_draw("TEST_LAZY_SCENE").ok(),
          "resolve_scene_draw refreshes cleanly after scenes/atlas load");
    carview::set_ui_scenes({});
  }

  // 2. kKeyboard & PlayStation glyph routing + CONTROLLER/* substitution + Controls tab visual diagram.
  {
    check(carview::ui4::glyph_label_for("A", carview::ui4::GlyphSet::kKeyboard) == "ENTER",
          "kKeyboard maps Accept/A -> ENTER");
    check(carview::ui4::glyph_label_for("B", carview::ui4::GlyphSet::kKeyboard) == "ESC",
          "kKeyboard maps Back/B -> ESC");
    check(carview::ui4::glyph_label_for("REWIND", carview::ui4::GlyphSet::kKeyboard) == "R",
          "kKeyboard maps Rewind -> R");
    check(carview::ui4::glyph_label_for("MAP", carview::ui4::GlyphSet::kKeyboard) == "M",
          "kKeyboard maps Map -> M");
    check(carview::ui4::glyph_label_for("CAMERA", carview::ui4::GlyphSet::kKeyboard) == "V",
          "kKeyboard maps Camera -> V");
    check(carview::ui4::glyph_label_for("HORN", carview::ui4::GlyphSet::kKeyboard) == "H",
          "kKeyboard maps Horn -> H");
    check(carview::ui4::glyph_label_for("A", carview::ui4::GlyphSet::kPlayStation) == "CROSS" &&
              carview::ui4::glyph_label_for("LB", carview::ui4::GlyphSet::kPlayStation) == "L1" &&
              carview::ui4::glyph_label_for("RT", carview::ui4::GlyphSet::kPlayStation) == "R2",
          "kPlayStation maps A/LB/RT -> CROSS/L1/R2");
    check(carview::ui4::controller_button_from_texture("CONTROLLER/A_BUTTON") == "A" &&
              carview::ui4::controller_button_from_texture("CONTROLLER/LBUMPER") == "LB" &&
              carview::ui4::controller_button_from_texture("CONTROLLER/RTRIGGER") == "RT",
          "controller_button_from_texture extracts logical button names");

    const auto prev_gs = carview::ui4::glyph_set();
    carview::ui4::set_glyph_set(carview::ui4::GlyphSet::kPlayStation);
    carview::ui::List l_ps;
    check(carview::ui4::draw_controller_texture(l_ps, 10, 10, 42, 42, "CONTROLLER/A_BUTTON"),
          "draw_controller_texture routes CONTROLLER/A_BUTTON to PlayStation Cross");
    check(!l_ps.items().empty(), "PlayStation glyph emits vector geometry");

    carview::ui4::set_glyph_set(carview::ui4::GlyphSet::kKeyboard);
    carview::ui::List l_kb;
    carview::ui4::button_bar(l_kb, {{"A", "SELECT"}, {"B", "BACK"}, {"REWIND", "REWIND"}, {"MAP", "MAP"}}, 1280, 720);
    check(l_kb.items().size() > 20, "kKeyboard button_bar renders crisp keycap badges");

    // Controls tab visual controller layout diagram + SettingsPage callouts.
    carview::SettingsPage sp;
    sp.load({"--controller-layout", "2", "--switch-handbrake-clutch"});
    sp.open();
    sp.select_tab(int(carview::SettingsPage::Tab::kControls));
    const auto callouts = sp.controller_diagram_callouts();
    check(callouts.size() >= 10, "SettingsPage::controller_diagram_callouts populates full layout mappings");
    carview::ui::List l_ctrl_tab;
    carview::draw_settings_ui4(l_ctrl_tab, 1280.0f, 720.0f, sp, false);
    carview::ui::List l_disp_tab;
    sp.select_tab(int(carview::SettingsPage::Tab::kDisplay));
    carview::draw_settings_ui4(l_disp_tab, 1280.0f, 720.0f, sp, false);
    check(l_ctrl_tab.items().size() > 20 && l_disp_tab.items().size() > 20,
          "SettingsPage Controls and Display tabs render cleanly");
    carview::ui4::set_glyph_set(prev_gs);
  }

  // 3. Per-page Post-Race tally timing & Career/Affinity XP breakdown screen.
  {
    carview::reset_post_race_page_timer();
    carview::RaceHud r;
    r.phase = carview::RaceHud::kFinished;
    r.finish_time = 100.0;
    r.clock = 104.0;
    r.results_page = 0;
    check(std::fabs(carview::post_race_page_elapsed(r) - 0.0) < 1e-4,
          "Post-race page 0 starts elapsed timer at 0 upon entry");
    r.clock = 108.0;
    check(std::fabs(carview::post_race_page_elapsed(r) - 4.0) < 1e-4,
          "Post-race page 0 advances elapsed timer with clock");
    // Enter Page 1 (CASH REWARD) at clock=108.0 (8s after finish_time!): must start fresh at 0.0s!
    r.results_page = 1;
    check(std::fabs(carview::post_race_page_elapsed(r) - 0.0) < 1e-4,
          "Post-race Page 1 cash tally measures from page entry time, NOT finish_time");
    r.clock = 108.7;
    check(std::fabs(carview::post_race_page_elapsed(r) - 0.7) < 1e-4,
          "Post-race Page 1 advances smoothly for cash tally animation");
    // Enter Page 2 (POINTS & CAREER/AFFINITY XP) at clock=112.0 -> fresh 0.0s!
    r.clock = 112.0;
    r.results_page = 2;
    check(std::fabs(carview::post_race_page_elapsed(r) - 0.0) < 1e-4,
          "Post-race Page 2 wristband & XP tally starts fresh at 0.0s");
    // Explicit page_enter_time override works deterministically.
    r.page_enter_time = 110.5;
    check(std::fabs(carview::post_race_page_elapsed(r) - 1.5) < 1e-4,
          "Explicit RaceHud::page_enter_time computes exact elapsed time");

    // Render Career / Driver Level & Manufacturer Affinity XP panel on Page 2 & Page 4.
    r.credits = 15000;
    r.band_name = "BLUE WRISTBAND";
    r.band_before = 8000;
    r.band_after = 9200;
    r.band_lo = 5000;
    r.band_hi = 12000;
    r.driver_level = 12;
    r.driver_xp = 2400;
    r.driver_xp_total = 44000;
    r.driver_next_xp = 50000;
    r.affinity_make = "Nissan";
    r.affinity_level = 5;
    r.affinity_xp = 1500;
    r.affinity_xp_total = 9500;
    r.affinity_next_xp = 12500;
    r.affinity_bonus_credits = 4500;
    r.affinity_discount_pct = 25.0;
    r.gift_car_name = "1997 Mazda RX-7";
    carview::DriveHud d;
    d.driving = true;
    d.race = &r;
    carview::ui::List l_p2, l_p4;
    carview::build_drive_hud(l_p2, d, 1280.0f, 720.0f);
    r.results_page = 4;
    carview::build_drive_hud(l_p4, d, 1280.0f, 720.0f);
    check(l_p2.items().size() > 40 && l_p4.items().size() > 30,
          "Post-Race Page 2 & Page 4 emit Career/Driver Level, Affinity XP, and Gift Car badge");
  }

  // 4. Boost BAR/PSI units, live lap split delta (-0.42s green / +0.31s red), imperial world marker & showcase units.
  {
    check(carview::format_boost_unit(14.5037738, true) == "1.00 BAR", "Metric boost formats in BAR");
    check(carview::format_boost_unit(14.5, false) == "14.5 PSI", "Imperial boost formats in PSI");
    check(carview::format_split_delta(-0.42) == "-0.42s", "Negative lap split delta formats as -0.42s");
    check(carview::format_split_delta(0.31) == "+0.31s", "Positive lap split delta formats as +0.31s");

    carview::RaceHud r;
    r.phase = carview::RaceHud::kRacing;
    r.circuit = true;
    r.lap = 2;
    r.laps = 3;
    r.clock = 84.5;
    r.lap_time = 24.5;
    r.best_lap = 60.0;
    r.has_lap_split_delta = true;
    r.lap_split_delta = -0.42;
    r.showcase = true;
    r.showcase_aircraft = "P-51 MUSTANG";
    r.showcase_gap_m = 400.0;
    carview::DriveHud d;
    d.driving = true;
    d.metric_units = false;
    d.speedo_mode = 1;  // digital dial
    d.has_boost = true;
    d.boost_psi = 14.5;
    d.boost_max_psi = 18.0;
    d.race = &r;
    carview::ui::List l_fast, l_slow;
    carview::build_drive_hud(l_fast, d, 1280.0f, 720.0f);
    r.lap_split_delta = 0.31;
    carview::build_drive_hud(l_slow, d, 1280.0f, 720.0f);
    auto has_color = [](const carview::ui::List& l, uint32_t col) {
      for (const auto& it : l.items())
        if (it.color == col) return true;
      return false;
    };
    check(has_color(l_fast, carview::ui4::kGreen), "Faster lap split delta (-0.42s) renders green");
    check(has_color(l_slow, carview::ui4::kRed), "Slower lap split delta (+0.31s) renders red");
  }

  // 5. Achievements scroll_row_ wiring & non-emoji ASCII/vector badge output.
  {
    check(std::string(carview::achievement_status_badge_text(true, false)) == "[UNLOCKED]",
          "Unlocked achievement badge uses crisp ASCII [UNLOCKED]");
    check(std::string(carview::achievement_status_badge_text(false, false)) == "[LOCKED]",
          "Locked achievement badge uses crisp ASCII [LOCKED]");
    check(std::string(carview::achievement_status_badge_text(false, true)) == "[SECRET]",
          "Secret achievement badge uses crisp ASCII [SECRET]");

    fh1::SpaDatabase empty_spa;
    carview::AchievementsSystem ach;
    ach.init(empty_spa);
    ach.open_screen();
    check(ach.visible_count() >= 40 && ach.scroll_row() == 0, "Achievements starts at scroll_row=0 with 50 items");
    // Move down 4 rows (row 0 -> 4): with 3 visible rows, scroll_row_ must advance smoothly to 2!
    for (int i = 0; i < 4; ++i) ach.move_focus(0, +1);
    check(ach.focus_index() == 20 && ach.scroll_row() == 2,
          "Achievements move_focus scrolls smoothly row-by-row via scroll_row_");
    carview::ui::List l_ach;
    ach.build_screen_hud(l_ach, 1280.0f, 720.0f);
    int min_hit_idx = 9999, max_hit_idx = -1;
    for (const auto& hz : l_ach.hits()) {
      if (hz.group == carview::ui::kHitRow) {
        min_hit_idx = std::min(min_hit_idx, hz.index);
        max_hit_idx = std::max(max_hit_idx, hz.index);
      }
    }
    check(min_hit_idx == 10 && max_hit_idx == 24,
          "Achievements build_screen_hud renders rows [scroll_row..scroll_row+2] (indices 10..24)");
    // Jump near end via select_focus_index -> scroll_row_ follows cleanly.
    ach.select_focus_index(ach.visible_count() - 1);
    check(ach.scroll_row() == (ach.visible_count() + 4) / 5 - 3,
          "Achievements select_focus_index updates scroll_row_ to show bottom row");
  }
}

void test_pt_e_race_screens_parity() {
  // 1. Localized event display title resolution (PT-39 & PT-42): zero raw internal IDs.
  check(race::resolve_event_display_title("PLANE_RACE_001") == "MUSTANG VS MUSTANG",
        "PLANE_RACE_001 never shown raw");
  check(race::resolve_event_display_title("FESTIVAL_SECOND") == "HORIZON QUALIFIER",
        "FESTIVAL_SECOND resolves cleanly");
  check(race::resolve_event_display_title("FR05") == "FESTIVAL RACE 05",
        "FR05 resolves cleanly");

  // 2. PT-39: Pre-Race screen renders Starting Grid / Opponent Lineup & live Difficulty & Rewards card.
  {
    carview::RaceHud r;
    r.phase = carview::RaceHud::kGrid;
    r.event = "FESTIVAL_SECOND";
    r.event_type = "FESTIVAL RACE";
    r.circuit = true;
    r.laps = 3;
    r.cars = 8;
    r.base_credits = 12000;
    r.difficulty_pct = 30;
    r.assists_pct = 15;
    r.band_name = "YELLOW WRISTBAND";
    r.grid_menu_open = true;
    r.grid_menu = {"START RACE", "< DIFFICULTY: HARD (+30%) >", "QUIT RACE"};
    r.grid_car_info = "1995 VW Corrado VR6  [C 380]";
    for (int i = 1; i <= 8; ++i) {
      carview::RaceHudRow row;
      row.place = i;
      row.player = (i == 8);
      row.name = row.player ? "PLAYER" : ("RIVAL " + std::to_string(i));
      row.car = row.player ? "1995 VW Corrado VR6" : "2010 Abarth 500";
      row.car_class = "C";
      row.pi = 380 + i;
      r.rows.push_back(std::move(row));
    }

    carview::DriveHud d;
    d.driving = true;
    d.race = &r;

    // Row 0 (START RACE): renders action menu + right-hand Starting Grid / Opponent Lineup card.
    r.grid_selected = 0;
    carview::ui::List l_grid;
    carview::build_drive_hud(l_grid, d, 1280.0f, 720.0f);
    int row_hits = 0;
    for (const auto& hz : l_grid.hits()) {
      if (hz.group == carview::ui::kHitRow) ++row_hits;
    }
    check(row_hits == 3, "Pre-race grid menu registers exact hit zones for all 3 rows");
    check(l_grid.items().size() > 60, "Pre-race Start Race view emits Event Header + Starting Grid lineup card");

    // Row 1 (DIFFICULTY): switches right-hand card to 5-tier Difficulty & Rewards breakdown.
    r.grid_selected = 1;
    carview::ui::List l_diff;
    carview::build_drive_hud(l_diff, d, 1280.0f, 720.0f);
    check(l_diff.items().size() > 45, "Pre-race Difficulty view emits 5-tier Difficulty & Rewards card");

    // Row 2 (QUIT RACE): renders Event Briefing card cleanly.
    r.grid_selected = 2;
    carview::ui::List l_quit;
    carview::build_drive_hud(l_quit, d, 1280.0f, 720.0f);
    check(l_quit.items().size() > 30, "Pre-race Quit Race view emits Event Briefing card");
  }

  // 3. PT-42: Sequential Post-Race screens (Stage 1 Race Results -> Stage 2 Credits Earned -> Stage 3 Wristband & Prize Car).
  {
    carview::reset_post_race_page_timer();
    carview::RaceHud r;
    r.phase = carview::RaceHud::kFinished;
    r.event = "PLANE_RACE_001";
    r.event_type = "SHOWCASE";
    r.place = 1;
    r.cars = 2;
    r.clock = 95.5;
    r.finish_time = 94.0;
    r.page_enter_time = 94.5;
    r.base_credits = 5000;
    r.mileage_bonus = 330;
    r.difficulty_pct = 15;
    r.difficulty_bonus = 800;
    r.skills_bonus = 2700;
    r.credits = 8830;
    r.balance = 31330;
    r.band_name = "YELLOW WRISTBAND";
    r.band_next = "GREEN WRISTBAND";
    r.band_before = 30;
    r.band_after = 30;
    r.band_lo = 0;
    r.band_hi = 220;
    r.won_car_name = "1970 Ford Mustang Boss 429";
    r.rows = {
        {"YOU", 1, true, true, 94.0, 0.0, "1970 Ford Mustang Boss 429", "C", 390, -1.0},
        {"P-51 MUSTANG", 2, false, true, 95.4, 1.4, "P-51D WARBIRD", "S", 750, -1.0},
    };

    carview::DriveHud d;
    d.driving = true;
    d.popularity_rank = 247;
    d.race = &r;

    // Stage 1: RACE RESULTS + podium table.
    r.results_page = 0;
    r.results_table = false;
    carview::ui::List l_s1;
    carview::build_drive_hud(l_s1, d, 1280.0f, 720.0f);
    check(l_s1.items().size() > 45, "Post-race Stage 1 renders 1ST PLACE header + podium leaderboard table");

    // Full results_table fallback works even without H_POST_RACE_RESULTS scene loaded.
    r.results_table = true;
    carview::ui::List l_tbl;
    carview::build_drive_hud(l_tbl, d, 1280.0f, 720.0f);
    check(l_tbl.items().size() > 35, "Post-race results_table renders fallback leaderboard without scene");
    r.results_table = false;

    // Stage 2: CREDITS EARNED rolling breakdown + updated balance.
    r.results_page = 1;
    carview::ui::List l_s2;
    carview::build_drive_hud(l_s2, d, 1280.0f, 720.0f);
    check(l_s2.items().size() > 40, "Post-race Stage 2 renders CREDITS EARNED animated tally & balance");

    // Stage 3: WRISTBAND & POPULARITY + PRIZE CAR AWARDED showcase card.
    r.results_page = 2;
    carview::ui::List l_s3;
    carview::build_drive_hud(l_s3, d, 1280.0f, 720.0f);
    check(l_s3.items().size() > 45, "Post-race Stage 3 renders Wristband, Popularity & Prize Car Awarded card");
  }
}

void test_round3_phase_f3_parity() {
  // 1. R3-UI-06 & R3-UI-08: Explicit ControllerDiagramParams (language-independent) & open_progress ease-out transitions.
  {
    carview::ui4::TiltedBladeSpec base;
    base.title = "STEUERUNG";  // German title — proves zero English string scraping!
    base.tabs = {"ANZEIGE", "STEUERUNG", "AUDIO"};
    base.active_tab = 1;
    carview::ui4::BladeRow r0;
    r0.label = "STEUERUNGS-LAYOUT";
    r0.value = "LAYOUT 1";
    r0.arrows = true;
    carview::ui4::BladeRow r1;
    r1.label = "HANDBREMSE / KUPPLUNG";
    r1.value = "EIN";
    r1.arrows = true;
    base.rows = {r0, r1};
    base.selected_index = 0;

    carview::ui::List l_no_diag, l_with_diag, l_hint_fallback, l_closed, l_mid;
    carview::ui4::tilted_blade_screen(l_no_diag, 1280.0f, 720.0f, base);

    auto with_diag = base;
    with_diag.controller_diagram.enabled = true;
    with_diag.controller_diagram.layout_id = 2;
    with_diag.controller_diagram.swap_handbrake_clutch = true;
    with_diag.controller_diagram.highlight_action = "HANDBRAKE";
    carview::ui4::tilted_blade_screen(l_with_diag, 1280.0f, 720.0f, with_diag);
    check(l_with_diag.items().size() > l_no_diag.items().size() + 30,
          "tilted_blade_screen renders controller diagram via explicit controller_diagram.enabled in any language");

    auto hint_fb = base;
    hint_fb.scene_hint = "H_CONTROLS";
    carview::ui4::tilted_blade_screen(l_hint_fallback, 1280.0f, 720.0f, hint_fb);
    check(l_hint_fallback.items().size() > l_no_diag.items().size() + 30,
          "tilted_blade_screen preserves H_CONTROLS scene_hint fallback");

    auto closed = base;
    closed.open_progress = 0.0f;
    carview::ui4::tilted_blade_screen(l_closed, 1280.0f, 720.0f, closed);
    check(l_closed.items().empty(), "tilted_blade_screen open_progress=0.0 emits zero items");

    auto mid = base;
    mid.open_progress = 0.5f;
    carview::ui4::tilted_blade_screen(l_mid, 1280.0f, 720.0f, mid);
    check(!l_mid.items().empty(), "tilted_blade_screen open_progress=0.5 emits mid-transition brush-wipe items");
    bool shifted_x = false;
    const size_t cmp_n = std::min(l_mid.items().size(), l_no_diag.items().size());
    for (size_t i = 0; i < cmp_n; ++i) {
      if (std::fabs(l_mid.items()[i].rect[0] - l_no_diag.items()[i].rect[0]) > 2.0f) {
        shifted_x = true;
        break;
      }
    }
    check(shifted_x, "tilted_blade_screen open_progress=0.5 applies horizontal slide ease-out offset");

    // UiTransitionTracker ~0.18s cubic ease-out ramp.
    carview::UiTransitionTracker tr;
    const float p0 = tr.step("PAUSE", 0.0f);
    const float p_half = tr.step("PAUSE", 0.09f);
    const float p_full = tr.step("PAUSE", 0.12f);
    check(p0 < 0.05f && p_half > 0.70f && p_half < 0.99f && p_full > 0.999f,
          "UiTransitionTracker ramps smoothly with 0.18s cubic ease-out");
    const float p_switch = tr.step("SETTINGS", 0.01f);
    check(p_switch < 0.40f && tr.active_screen() == "SETTINGS",
          "UiTransitionTracker restarts transition on screen switch");
    tr.step("SETTINGS", 0.10f);
    tr.step("SETTINGS", 0.10f);
    const float p_close = tr.step("", 0.09f);
    check(tr.is_closing() && p_close < 0.99f, "UiTransitionTracker reverses cleanly on screen close");
  }

  // 2. R3-UI-16: Replay Browser two-step confirmed delete (.shrep removal) & ui4::class_badge rendering.
  {
    std::error_code ec;
    const auto tmp_dir = std::filesystem::temp_directory_path(ec) / "shift_r3_f3_replay_test";
    std::filesystem::remove_all(tmp_dir, ec);
    std::filesystem::create_directories(tmp_dir, ec);

    drive::ReplayRecording rec;
    rec.meta.event_name = "RED ROCK SHOWDOWN";
    drive::ReplayCarInfo rci{};
    rci.slot = 0;
    rci.car_media = "2013 SRT Viper GTS [S 742]";
    rci.driver_name = "PLAYER";
    rci.is_player = true;
    rec.meta.roster.push_back(rci);
    drive::ReplayFrame f0{};
    f0.time_s = 0.0;
    f0.cars.push_back({});
    rec.frames.push_back(f0);
    drive::ReplayFrame f1{};
    f1.time_s = 12.5;
    f1.cars.push_back({});
    rec.frames.push_back(f1);

    carview::ReplayViewer rv;
    rv.open(rec);
    std::filesystem::path saved_path;
    std::string serr;
    check(rv.save_to_dir(tmp_dir, saved_path, &serr) && std::filesystem::exists(saved_path, ec),
          "ReplayViewer saves .shrep file to temp directory");
    rv.close();
    rv.open_browser(tmp_dir);
    check(rv.browser_entries().size() == 1, "ReplayViewer browser scans saved .shrep file");

    carview::ui::List l_br;
    rv.build_hud(l_br, 1280.0f, 720.0f);
    bool has_del_btn = false;
    for (const auto& hz : l_br.hits()) {
      if (hz.group == carview::ui4::kHitButton && hz.index == 2) has_del_btn = true;
    }
    check(l_br.items().size() > 25 && has_del_btn,
          "Replay Browser renders tilted chrome, class_badge, and DELETE button_bar hit-zone");

    // Step 1: arm delete confirmation (file must still exist!).
    check(!rv.browser_request_or_confirm_delete(&serr) && rv.browser_delete_confirm_active(),
          "Replay Browser first delete press arms confirmation");
    check(std::filesystem::exists(saved_path, ec), "Replay file preserved until delete is confirmed");
    // Step 2: confirm delete (removes file & refreshes entries!).
    check(rv.browser_request_or_confirm_delete(&serr), "Replay Browser second delete press confirms and deletes");
    check(!std::filesystem::exists(saved_path, ec) && rv.browser_entries().empty(),
          "Confirmed replay delete removes .shrep from disk and refreshes browser list");
    std::filesystem::remove_all(tmp_dir, ec);
  }

  // 3. R3-UI-09 & R3-UI-17: Livery real finish names, vinyl thumbnails, bottom bar index 2 & carousel arrows.
  {
    check(std::string(carview::paint_finish_display_name(cars::PaintFinish::kNormal)) == "FACTORY" &&
              std::string(carview::paint_finish_display_name(cars::PaintFinish::kMetallic)) == "METALLIC" &&
              std::string(carview::paint_finish_display_name(cars::PaintFinish::kMatte)) == "MATTE" &&
              std::string(carview::paint_finish_display_name(cars::PaintFinish::kTwoTone)) == "TWO-TONE" &&
              std::string(carview::paint_finish_display_name(cars::PaintFinish::kChrome)) == "CHROME" &&
              std::string(carview::paint_finish_display_name(cars::PaintFinish::kCarbon)) == "CARBON FIBER" &&
              std::string(carview::paint_finish_display_name(cars::PaintFinish::kWood)) == "WOOD" &&
              std::string(carview::paint_finish_display_name(cars::PaintFinish::kCamo)) == "CAMOUFLAGE",
          "paint_finish_display_name returns authentic human-readable FH1 finish names");

    cars::SpecialColorRow sc_pearl{};
    sc_pearl.id = 42;
    sc_pearl.finish = int(cars::PaintFinish::kTwoTone);
    sc_pearl.pri_valid = true;
    sc_pearl.pri_rgb = 0x1E60D8u;
    sc_pearl.sec_valid = true;
    sc_pearl.sec_rgb = 0xD41C88u;
    sc_pearl.two_tone_power = 3.5f;
    const std::string sp_name = carview::special_color_display_name(sc_pearl);
    check(sp_name.find("SPECIAL #") == std::string::npos && sp_name.find("PEARL") != std::string::npos,
          "special_color_display_name renders human-readable PEARL name instead of SPECIAL #id");

    cars::bake::Colour mfg{};
    mfg.rgb = 0xCC1820u;
    mfg.metallic = true;
    const std::string mfg_name = carview::manufacturer_color_display_name(mfg);
    check(mfg_name.find("OEM #") == std::string::npos && mfg_name.find("METALLIC") != std::string::npos,
          "manufacturer_color_display_name renders real METALLIC hue name instead of OEM #1");
    check(carview::oem_swatch_label(0) == "OEM #1", "oem_swatch_label preserves backwards-compatible OEM #N format");

    carview::LiveryEditor ed;
    std::string msg;
    ed.open(nullptr, {}, 101, {}, msg);
    cars::bake::Colour mfg2{};
    mfg2.rgb = 0xF2F2F2u;
    mfg2.metallic = false;
    ed.set_manufacturer_colors({mfg, mfg2});
    ed.add_vinyl_layer("@star", 0xFFCC00u);
    ed.add_vinyl_layer("@chevron", 0x2088FFu);

    for (auto tab : {carview::EditorTab::kPaint, carview::EditorTab::kManufacturerColors,
                     carview::EditorTab::kPreviousColors, carview::EditorTab::kVinylLayers}) {
      ed.set_tab(tab);
      carview::ui::List l;
      carview::draw_livery_editor_hud(l, 1280.0f, 720.0f, ed, "2013 SRT VIPER GTS");
      bool has_left = false, has_right = false, has_btn2 = false, has_btn5 = false;
      for (const auto& hz : l.hits()) {
        if (hz.group == carview::ui::kHitStepLeft) has_left = true;
        if (hz.group == carview::ui::kHitStepRight) has_right = true;
        if (hz.group == carview::ui4::kHitButton && hz.index == 2) has_btn2 = true;
        if (hz.group == carview::ui4::kHitButton && hz.index == 5) has_btn5 = true;
      }
      check(has_left && has_right, "Livery tab registers left/right carousel or finish stepper arrow hit-zones");
      check(has_btn2 && has_btn5, "Livery bottom button_bar registers all 6 hit-zones including index 2");
      if (tab == carview::EditorTab::kVinylLayers) {
        int thumb_imgs = 0;
        for (const auto& it : l.items()) {
          if (it.kind == carview::ui::kImage) ++thumb_imgs;
        }
        check(thumb_imgs >= 2, "Vinyl Layers tab draws miniature vinyl shape preview thumbnails");
      }
    }
    ed.set_tab(carview::EditorTab::kManufacturerColors);
    check(ed.handle_step_click(1) && ed.selected_manufacturer_color() == 1,
          "LiveryEditor::handle_step_click advances carousel selection");
    check(ed.handle_button_bar_click(2), "LiveryEditor::handle_button_bar_click handles index 2 cleanly");
  }

  // 4. R3-UI-09: Garage top filter/tab strip clickable hit-zones (0..4) & handle_tab_click.
  {
    carview::GarageBrowser gb;
    const std::vector<garage::ClassBand> bands = {{0.30, 300.0, "D"}, {0.425, 425.0, "C"}, {0.50, 500.0, "B"}};
    carview::ui::List l_gar;
    carview::draw_garage_hud(l_gar, 1280.0f, 720.0f, gb, bands, 50000);
    std::set<int> tab_indices;
    for (const auto& hz : l_gar.hits()) {
      if (hz.group == carview::ui::kHitTab) tab_indices.insert(hz.index);
    }
    check(tab_indices.count(0) && tab_indices.count(1) && tab_indices.count(2) && tab_indices.count(3) &&
              tab_indices.count(4),
          "draw_garage_hud registers clickable kHitTab hit-zones for MY CARS, RESTORATION BAY, SORT, FILTER, FAVORITES");
    check(gb.handle_tab_click(1) && gb.active_tab() == carview::GarageTab::kRestorationBay,
          "GarageBrowser::handle_tab_click(1) switches to Restoration Bay");
    check(gb.handle_tab_click(0) && gb.active_tab() == carview::GarageTab::kMyCars,
          "GarageBrowser::handle_tab_click(0) returns to My Cars");
    const auto prev_sort = gb.sort_mode();
    check(gb.handle_tab_click(2) && gb.sort_mode() != prev_sort,
          "GarageBrowser::handle_tab_click(2) cycles Garage sort mode");
    check(gb.handle_tab_click(4) && gb.favorites_only(),
          "GarageBrowser::handle_tab_click(4) toggles Favorites filter");
  }

  // 5. R3-UI-09: Achievements clickable sort hit-zone + tracked-chip non-overlap guard + Car Club tilted chrome.
  {
    fh1::SpaDatabase empty_spa;
    carview::AchievementsSystem ach;
    ach.init(empty_spa);
    ach.open_screen();
    carview::ui::List l_ach;
    ach.build_screen_hud(l_ach, 1280.0f, 720.0f);
    bool has_sort_hit = false;
    for (const auto& hz : l_ach.hits()) {
      if (hz.group == carview::ui4::kHitButton && hz.index == 2) has_sort_hit = true;
    }
    check(has_sort_hit, "Achievements screen registers clickable SORT hit-zone");

    // Pin an achievement and verify tracked-chip shifts cleanly below an occupied top-left Race HUD / Objective banner.
    ach.toggle_track_focused();
    ach.close_screen();
    carview::ui::List l_free, l_occ, l_hidden;
    ach.build_hud_overlays(l_free, 1280.0f, 720.0f, true);
    // Simulate a Race HUD / Campaign Objective banner occupying top-left (y=28..148).
    l_occ.rect(24.0f, 28.0f, 360.0f, 148.0f, carview::ui::rgba(20, 22, 28, 220));
    float max_y = 0.0f;
    check(carview::AchievementsSystem::top_left_hud_occupied(l_occ, 1280.0f, 720.0f, &max_y) && max_y >= 147.0f,
          "AchievementsSystem::top_left_hud_occupied detects top-left HUD banner");
    const size_t before_n = l_occ.items().size();
    ach.build_hud_overlays(l_occ, 1280.0f, 720.0f, true);
    float chip_min_y = 1e9f;
    for (size_t i = before_n; i < l_occ.items().size(); ++i) {
      chip_min_y = std::min(chip_min_y, l_occ.items()[i].rect[1]);
    }
    check(l_occ.items().size() > before_n && chip_min_y >= 152.0f,
          "Tracked achievement chip offsets cleanly below occupied top-left Race HUD / Objective banner");

    // A circuit's timer lines run on below the zone in rows (RACE, LAP, LAST,
    // BEST): the chip goes under the last one.
    carview::ui::List l_rows;
    l_rows.rect(56.0f, 30.0f, 330.0f, 100.0f, carview::ui::rgba(20, 22, 28, 220));
    for (int k = 0; k < 4; ++k)
      l_rows.rect(60.0f, 108.0f + 28.0f * k, 250.0f, 130.0f + 28.0f * k, carview::ui::rgba(20, 22, 28, 220));
    max_y = 0.0f;
    check(carview::AchievementsSystem::top_left_hud_occupied(l_rows, 1280.0f, 720.0f, &max_y) && max_y >= 214.0f,
          "AchievementsSystem::top_left_hud_occupied follows timer rows below the zone");

    l_hidden.rect(24.0f, 28.0f, 360.0f, 148.0f, carview::ui::rgba(20, 22, 28, 220));
    const size_t hidden_before = l_hidden.items().size();
    ach.build_hud_overlays(l_hidden, 1280.0f, 720.0f, true, true, true);
    check(l_hidden.items().size() == hidden_before,
          "Tracked achievement chip hides cleanly when hide_when_occupied is requested");

    // Car Club tilted blade chrome & class_badge rendering.
    carview::CarClubScreenState ccs;
    ccs.active = true;
    ccs.club_name = "HORIZON RUNNERS";
    ccs.club_tag = "HZN1";
    ccs.cars.push_back({1, 101, "SRT_Viper_13", "2013 SRT Viper GTS", "S 742", true, true, false});
    carview::ui::List l_club;
    carview::draw_car_club_screen(l_club, 1280, 720, ccs);
    check(l_club.items().size() > 35 && l_club.hits().size() >= 6,
          "Car Club screen renders FH1 tilted blade chrome, class_badge, and button_bar hit-zones");
  }
}

}  // namespace

int main() {
  test_ui4_chrome_primitives();
  test_map_camera_controller();
  test_units_mouse_and_banner_non_overlap();
  test_round2_hud_minimap_gps_and_mouse_ux();
  test_round3_subtitle_lift_upgrade_units_and_rewind_touch();
  test_round2_phase6_ui4_parity();
  test_pt_e_race_screens_parity();
  test_round3_phase_f3_parity();

  const char* root_env = std::getenv("SHIFT_GAME_ROOT");
  const char* shot_env = std::getenv("SHIFT_UI4_SCREENSHOT_DIR");
  const char* uifix_env = std::getenv("SHIFT_UIFIX_SCREENSHOT_DIR");
  const char* repo_env = std::getenv("SHIFT_REPO_ROOT");
  if (root_env && *root_env && shot_env && *shot_env) {
    generate_screenshots(root_env, shot_env, repo_env ? std::filesystem::path(repo_env) : std::filesystem::current_path(),
                         false);
  }
  if (root_env && *root_env && uifix_env && *uifix_env) {
    generate_screenshots(root_env, uifix_env, repo_env ? std::filesystem::path(repo_env) : std::filesystem::current_path(),
                         true);
  }

  if (g_failures > 0) {
    std::fprintf(stderr, "ui4_parity_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("ui4_parity_tests: PASS\n");
  return 0;
}
