// Scene-driven screens against the player's game files (LABELS corpus;
// skipped, exit 77, without SHIFT_GAME_ROOT): the pause menu's scene
// (925_PAUSE_MENU) and the two screens it opens (H_SPONSOR_CHALLENGE_STATUS,
// 802_MESSAGE_CENTER) have every node the screens bind, every string they
// bind resolves in the EN string tables (SHIFT_STATE_ROOT adds v4 and the
// packages), the sponsor rows resolve from the game's sponsor data, and the
// navigation cues name non-blank ui4audio.xml events. With
// SHIFT_SCENE_SCREEN_SHOT_DIR the screens are also drawn to PNGs there, over
// SHIFT_SCENE_SCREEN_BACKDROP (a 1280x720 PNG) when given.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "audio/event_configs.h"
#include "cars/sources.h"
#include "cars/strings.h"
#include "fh1data/archive.h"
#include "fh1data/stringtables.h"
#include "fh1data/ui_scene.h"
#include "fh1data/xds.h"
#include "freeroam/skills.h"
#include "game_hud.h"
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

using carview::PauseMenu;
using carview::SceneDraw;

int child(const SceneDraw& sd, int parent, std::string_view name) {
  if (parent < 0) return -1;
  for (int c : sd.children_of(parent))
    if (sd.object_name(c) == name) return c;
  return -1;
}

void check_nodes(const SceneDraw& sd, std::string_view scene, const std::vector<const char*>& paths) {
  for (const char* p : paths) {
    const std::string_view path(p);
    const int n = path.find('/') != std::string_view::npos ? sd.find_path(path) : sd.find(path);
    check(n >= 0, std::string(scene) + ": node " + p);
  }
}

void check_ref(const carview::StringRef& r, const std::string& what) {
  const auto t = carview::lookup(r);
  check(t.has_value() && !t->empty(), what + " resolves (" + std::string(r.table) + "/" +
                                          (r.id.empty() ? std::to_string(r.raw) : std::string(r.id)) + ")");
}

}  // namespace

int main() {
  const char* root_env = std::getenv("SHIFT_GAME_ROOT");
  if (!root_env || !*root_env) {
    std::printf("scene_screen_tests: SKIP (SHIFT_GAME_ROOT not set)\n");
    return 77;
  }
  const std::filesystem::path game_root = root_env;
  const char* state_env = std::getenv("SHIFT_STATE_ROOT");
  const std::filesystem::path state_root = state_env ? state_env : "";

  // Scenes through the shared loader.
  auto ui_zip = fh1::Archive::open(game_root / "media" / "UI.zip", nullptr);
  check(ui_zip != nullptr, "UI.zip opens");
  if (!ui_zip) return 1;
  std::map<std::string, std::unique_ptr<fh1::UiScene>> scenes;
  carview::set_ui_scenes([&](const std::string& name) -> const fh1::UiScene* {
    auto& slot = scenes[name];
    if (!slot) {
      auto sc = std::make_unique<fh1::UiScene>();
      std::string e;
      if (fh1::load_ui_scene(*ui_zip, name, *sc, &e)) slot = std::move(sc);
      else std::fprintf(stderr, "scene %s: %s\n", name.c_str(), e.c_str());
    }
    return slot.get();
  });

  // EN string tables (disc, v4, packages).
  cars::Install inst;
  std::string why;
  check(cars::discover_install(game_root, state_root, {}, inst, &why), "discover_install: " + why);
  fh1::StringTables tables;
  check(cars::load_string_tables(inst, tables, &why, "EN"), "EN string tables load");
  carview::set_game_text([&](const std::string& v) -> std::string {
    if (auto t = tables.resolve(v)) return *t;
    return v;
  });

  // 1. The scenes and the nodes the screens bind.
  {
    const SceneDraw sd(carview::ui_scene(std::string(carview::kPauseScene)));
    check(sd.ok(), "925_PAUSE_MENU loads");
    for (int i = 0; i < 7; ++i) {
      const int btn = sd.find("Button" + std::to_string(i));
      check(btn >= 0, "925_PAUSE_MENU: Button" + std::to_string(i));
      const int body = child(sd, btn, "Button");
      for (const char* c : {"Text", "Dirt_Tile", "Crisp_Tape", "Smudge_Tape", "Clipon"})
        check(child(sd, body, c) >= 0, "925_PAUSE_MENU: Button" + std::to_string(i) + "/Button/" + c);
    }
    check_nodes(sd, "925_PAUSE_MENU",
                {"Option_List/Option_List", "Layer2", "TEXT_SCREEN_TITLE", "Next_Level", "Points", "Wristband",
                 "TEXT_CAR", "TEXT_PI", "CLASS_A", "CLASS_SM", "TEXT_CR", "BottomRow", "BG_Noise"});
  }
  {
    const SceneDraw sd(carview::ui_scene(std::string(carview::kSponsorScene)));
    check(sd.ok(), "H_SPONSOR_CHALLENGE_STATUS loads");
    for (int i = 0; i < 6; ++i) {
      const std::string name = i == 0 ? "SponsorBar" : "SponsorBar" + std::to_string(i + 1);
      const int bar = sd.find(name);
      check(bar >= 0, "H_SPONSOR_CHALLENGE_STATUS: " + name);
      const int detail = child(sd, bar, "SponsorDetail"), rank = child(sd, bar, "Rank");
      const int pb = child(sd, bar, "ProgressBar");
      check(child(sd, detail, "TEXT_SPONSOR") >= 0 && child(sd, detail, "TEXT_TITLE") >= 0,
            name + ": SponsorDetail texts");
      check(child(sd, rank, "TEXT_TITLE") >= 0, name + ": Rank/TEXT_TITLE");
      check(child(sd, pb, "middle") >= 0 && child(sd, pb, "TEXT_PROGRESS") >= 0 && child(sd, pb, "Group") >= 0,
            name + ": ProgressBar parts");
      check(child(sd, bar, "SponsorLogo") >= 0, name + ": SponsorLogo");
      for (const char* pane : {"Sponsor_Back", "Progress_Back"})
        check(child(sd, child(sd, bar, pane), "BigPane01") >= 0, name + ": " + pane + "/BigPane01");
    }
    check_nodes(sd, "H_SPONSOR_CHALLENGE_STATUS",
                {"TitleTapeText", "TEXT_POSITION_TITLE", "TEXT_POSITION", "TEXT_TH", "TEXT_DETAILS_TITLE",
                 "RewardTitle/TEXT_CAR_NAME", "Reward/TEXT_CAR_NAME", "TEXT_CHALLENGE", "ButtonBar",
                 "NAV_POSITION_INDICATOR"});
  }
  {
    const SceneDraw sd(carview::ui_scene(std::string(carview::kMessageScene)));
    check(sd.ok(), "802_MESSAGE_CENTER loads");
    check_nodes(sd, "802_MESSAGE_CENTER",
                {"TitleTapeText", "TEXT_MESSAGE_TITLE", "TEXT_MESSAGE", "MESSAGE_MENU", "LOADING_ICON", "HELP_BUTTON_BAR",
                 "NAV_POSITION_INDICATOR"});
  }

  // 2. Every string the screens bind resolves in the EN tables.
  {
    PauseMenu pm;
    for (bool race : {false, true}) {
      pm.show(race);
      for (PauseMenu::Action a : pm.items()) {
        const carview::StringRef r = carview::pause_row_text(a, race);
        check_ref(r, std::string(race ? "race" : "free roam") + " row " + std::string(r.fallback));
      }
    }
    using namespace carview::pause_text;
    for (const carview::StringRef* r :
         {&kSelect, &kResume, &kBack, &kCollectWristband, &kCredits, &kSponsorTitle, &kPopularityChart, &kRank,
          &kReward, &kDetails, &kChallengeCompleted, &kMessageTitle, &kNoMessages, &kYouHaveNoMessages,
          &kCheckBackLater})
      check_ref(*r, std::string(r->fallback));
    // The free roam rows read as the original's (the SETTINGS slot's own text
    // is the retail MULTIPLAYER).
    pm.show(false);
    carview::HudState st;
    pm.fill(st);
    check((st.menu == std::vector<std::string>{"MAP", "SETTINGS", "PHOTO MODE", "MESSAGE CENTER", "SPONSOR CHALLENGES",
                                               "MY PROFILE", "QUIT"}),
          "free roam rows read MAP .. QUIT from the tables");
  }

  // 3. The sponsor rows from the game's sponsor data.
  freeroam::SkillsData skills;
  check(freeroam::load_skills_data(game_root, skills, &why), "skills data loads: " + why);
  std::map<std::string, freeroam::SponsorProgress> progress;
  progress["Air"] = {102, 2};
  const auto rows = carview::sponsor_challenge_rows(skills, &progress, 166, 3);
  check(rows.size() >= 6, "at least six sponsor challenges");
  for (const auto& r : rows) {
    check(!r.sponsor_name.empty() && r.sponsor_name.rfind("IDS_", 0) != 0, "sponsor name resolves: " + r.sponsor_name);
    check(!r.challenge_title.empty() && r.challenge_title.rfind("IDS_", 0) != 0,
          "challenge name resolves: " + r.challenge_title);
    check(!r.description.empty() && r.description.find("{0}") == std::string::npos,
          "details resolve with the target: " + r.challenge_title);
    check(r.logo_texture.rfind("SPONSORLOGOTILE/", 0) == 0, "logo texture: " + r.challenge_title);
  }
  if (!rows.empty()) {
    check(rows[0].fame && rows[0].current == 166 && rows[0].tier == 4 && rows[0].target == 150,
          "popularity challenge: chart 166th, rank 4 (150th)");
    check(rows[0].description.find("150TH") != std::string::npos, "popularity details name the 150TH: " + rows[0].description);
  }
  for (const auto& r : rows)
    if (r.challenge_title == "AIR CHALLENGE")
      check(r.current == 102 && r.tier == 3 && r.target == 150, "AIR CHALLENGE: 102/150, rank 3");

  // 4. The navigation cues name non-blank ui4audio.xml events.
  {
    audio::UiAudioMap map;
    check(map.load(game_root, &why), "ui4audio.xml loads: " + why);
    for (auto e : {carview::UiEvent::kOpen, carview::UiEvent::kClose, carview::UiEvent::kMove, carview::UiEvent::kAccept,
                   carview::UiEvent::kBack}) {
      const std::string_view play = carview::ui_play_name(e);
      const auto* entry = map.resolve(play, carview::kPauseScene);
      check(entry && !entry->is_blank(), "cue " + std::string(play) + " plays a sound");
    }
  }

  // 5. Optional renders.
  if (const char* shot_env = std::getenv("SHIFT_SCENE_SCREEN_SHOT_DIR"); shot_env && *shot_env) {
    const std::filesystem::path out_dir = shot_env;
    std::filesystem::create_directories(out_dir);
    if (auto fzip = fh1::Archive::open(game_root / "media" / "ui" / "Fonts.zip", nullptr))
      carview::ui::load_fonts_from_zip(*fzip);
    auto hz = fh1::Archive::open(game_root / "media" / "ui" / "textures" / "Horizon.zip", nullptr);
    auto tx = fh1::Archive::open(game_root / "media" / "ui" / "Textures.zip", nullptr);
    std::set<std::string> failed;
    carview::set_ui_textures([&](const std::string& path, std::array<float, 4>& uv) -> bool {
      if (carview::ui::atlas_find(path, uv)) return true;
      if (failed.count(path)) return false;
      const bool t = path.rfind("T:", 0) == 0;
      fh1::Archive* a = t ? tx.get() : hz.get();
      const fh1::ReadResult r = a ? a->read((t ? path.substr(2) : path) + ".xds") : fh1::ReadResult{};
      const auto x = r.ok() ? fh1::decode_xds(r.data, false) : fh1::XdsResult{};
      if (x.levels.empty() || !carview::ui::atlas_image(path, x.levels[0].width, x.levels[0].height, x.levels[0].rgba, uv)) {
        failed.insert(path);
        return false;
      }
      return true;
    });
    constexpr uint32_t W = 1280, H = 720;
    std::vector<uint8_t> backdrop(size_t(W) * H * 4, 0);
    for (size_t i = 0; i < backdrop.size(); i += 4) backdrop[i] = 70, backdrop[i + 1] = 92, backdrop[i + 2] = 64, backdrop[i + 3] = 255;
    if (const char* bg = std::getenv("SHIFT_SCENE_SCREEN_BACKDROP"); bg && *bg) {
      int w = 0, h = 0, n = 0;
      if (uint8_t* px = stbi_load(bg, &w, &h, &n, 4)) {
        if (w == int(W) && h == int(H)) backdrop.assign(px, px + size_t(W) * H * 4);
        stbi_image_free(px);
      }
    }
    carview::DriveHud d;
    d.driving = false;
    d.pause = true;
    d.menu_title = "paused";
    d.band_name = "GREEN WRISTBAND";
    d.band_next = "BLUE";
    d.band_level = 3;
    d.band_collect_ready = true;
    d.car_name = "Mazda MX-5 Miata";
    d.car_class = "A";
    d.car_pi = 600;
    d.credits = 167700;
    d.popularity_rank = 166;
    auto shot = [&](const std::string& file, PauseMenu& pm, bool race) {
      carview::PauseScreenHud ps;
      carview::fill_pause_screen(pm, 1e9, ps);
      ps.in_race = race;
      ps.sponsors = &rows;
      d.pause_screen = &ps;
      carview::ui::List l;
      carview::build_drive_hud(l, d, float(W), float(H));
      std::vector<uint8_t> fb = backdrop;
      carview::ui::rasterize(l.items(), fb, W, H);
      const auto p = out_dir / file;
      check(stbi_write_png(p.string().c_str(), int(W), int(H), 4, fb.data(), int(W * 4)) != 0, "write " + file);
    };
    PauseMenu pm;
    pm.show(false);
    shot("pause_roam.png", pm, false);
    pm.move(2);
    shot("pause_roam_photo.png", pm, false);
    pm.open_extras();
    shot("pause_extras.png", pm, false);
    pm.back();
    pm.open_screen(PauseMenu::Screen::kSponsorChallenges);
    shot("sponsor_challenges.png", pm, false);
    pm.back();
    pm.open_screen(PauseMenu::Screen::kMessageCentre);
    shot("message_centre.png", pm, false);
    pm.show(true);
    shot("pause_race.png", pm, true);

    // The driving HUD, over the original's captures when
    // SHIFT_SCENE_SCREEN_HUD_DIR names their folder (the calibration loop:
    // the engine's HUD drawn on the original's).
    const char* hud_env = std::getenv("SHIFT_SCENE_SCREEN_HUD_DIR");
    const std::filesystem::path hud_dir = hud_env ? hud_env : "";
    auto hud_shot = [&](const std::string& file, const std::string& orig, const carview::DriveHud& hd) {
      std::vector<uint8_t> fb = backdrop;
      if (!hud_dir.empty()) {
        int w = 0, h = 0, n = 0;
        const std::filesystem::path src = std::filesystem::path(orig).is_absolute() ? orig : hud_dir / orig;
        if (uint8_t* px = stbi_load(src.string().c_str(), &w, &h, &n, 4)) {
          if (w == int(W) && h == int(H)) fb.assign(px, px + size_t(W) * H * 4);
          stbi_image_free(px);
        }
      }
      carview::ui::List l;
      carview::build_drive_hud(l, hd, float(W), float(H));
      carview::ui::rasterize(l.items(), fb, W, H);
      const auto p = out_dir / file;
      check(stbi_write_png(p.string().c_str(), int(W), int(H), 4, fb.data(), int(W * 4)) != 0, "write " + file);
    };
    carview::DriveHud roam;
    roam.driving = true;
    roam.time = 10.0;
    roam.kmh = 57;
    roam.rpm = 6900;
    roam.redline_rpm = 7000;
    roam.gear = 3;
    roam.abs_enabled = roam.tcs_enabled = true;
    hud_shot("hud_roam.png", "05_roam_hud_day_drive_orig.png", roam);
    carview::DriveHud r34 = roam;
    r34.kmh = 0;
    r34.rpm = 900;
    r34.redline_rpm = 8000;
    r34.gear = 1;
    hud_shot("hud_roam_r34.png", "15_countdown_orig.png", r34);

    // The event prompt in an activation zone (09_event_marker_prompt_orig.png).
    carview::WorldMarkerHud wm;
    wm.visible = wm.in_range = true;
    wm.sponsor = carview::event_sponsor(43);
    check(wm.sponsor == "HORIZON presents", "Events/IDS_Sponsor_43 reads HORIZON presents: " + wm.sponsor);
    wm.title = carview::event_name_without_sponsor(carview::text_of({"Events", "IDS_Name_43", 0, "?"}), wm.sponsor);
    check(wm.title == "The Gauntlet", "event 43's name without its sponsor: " + wm.title);
    wm.prompt = carview::text_of(carview::hud_text::kSignMeUp);
    wm.icon = std::getenv("SHIFT_HUD_PROMPT_ICON") ? std::getenv("SHIFT_HUD_PROMPT_ICON") : "ICONS/ACTIVATIONZONEICONS/P2P2";
    carview::DriveHud prompt_d = roam;
    prompt_d.kmh = 0, prompt_d.rpm = 900, prompt_d.gear = 1;
    prompt_d.world_marker = &wm;
    hud_shot("hud_prompt.png", "09_event_marker_prompt_orig.png", prompt_d);

    // A sprint (16_race_hud_orig.png) and a circuit (the oracle's
    // race-hud-notification.png, by absolute path in SHIFT_SCENE_SCREEN_ORACLE).
    carview::RaceHud sprint;
    sprint.phase = carview::RaceHud::kRacing;
    sprint.place = 8, sprint.cars = 8, sprint.clock = 16.851, sprint.progress = 0.0;
    const char* names[] = {"Peter Houston", "Ana Ruiz", "Tom Hale", "Lee Park", "Sam Cole", "Josh Pollock",
                           "Modesto Estrada", "User"};
    for (int i = 0; i < 8; ++i) {
      carview::RaceHudRow row;
      row.name = names[i], row.place = i + 1, row.player = i == 7;
      sprint.rows.push_back(row);
    }
    carview::DriveHud race_d = roam;
    race_d.kmh = 73, race_d.rpm = 5200, race_d.gear = 2;
    race_d.race = &sprint;
    carview::SkillChainHud sprint_sk;
    sprint_sk.active = true, sprint_sk.chain_points = 250, sprint_sk.multiplier_f = 1.1f;
    sprint_sk.last_skill = "GREAT ONE EIGHTY", sprint_sk.last_points = 150, sprint_sk.last_skill_at = 9.5;
    race_d.skills = &sprint_sk;
    hud_shot("hud_race.png", "16_race_hud_orig.png", race_d);
    carview::RaceHud circuit;
    circuit.phase = carview::RaceHud::kRacing;
    circuit.circuit = true, circuit.lap = 4, circuit.laps = 4, circuit.place = 4, circuit.cars = 8;
    circuit.clock = 164.698, circuit.lap_time = 39.697, circuit.last_lap = 40.336, circuit.best_lap = 40.336;
    const char* cnames[] = {"Alan Brewer", "Kim Novak", "Peter Bankov", "User", "Don Funk", "Ira Bell", "Joe Kerr",
                            "Max Lane"};
    for (int i = 0; i < 8; ++i) {
      carview::RaceHudRow row;
      row.name = cnames[i], row.place = i + 1, row.player = i == 3;
      circuit.rows.push_back(row);
    }
    carview::DriveHud circuit_d = r34;
    circuit_d.kmh = 143, circuit_d.rpm = 7600, circuit_d.gear = 3;
    circuit_d.race = &circuit;
    carview::SkillChainHud circuit_sk;
    circuit_sk.active = true, circuit_sk.chain_points = 583, circuit_sk.multiplier_f = 1.4f;
    circuit_sk.last_skill = "GREAT DRAFTING", circuit_sk.last_points = 150, circuit_sk.last_skill_at = 9.5;
    circuit_d.skills = &circuit_sk;
    const char* oracle = std::getenv("SHIFT_SCENE_SCREEN_ORACLE");
    hud_shot("hud_race_circuit.png", oracle ? std::string(oracle) + "/race-hud-notification.png" : "", circuit_d);
    // A chain just banked: the total and the popularity disc.
    carview::SkillChainHud banked_sk;
    banked_sk.banked_total = 100, banked_sk.banked_at = 9.0, banked_sk.popularity_rank = 165;
    banked_sk.prev_popularity_rank = 165, banked_sk.pop_anim_progress = 0.3, banked_sk.pop_rank_anim_t = 1.0;
    circuit_d.skills = &banked_sk;
    circuit.lap = 1, circuit.place = 7, circuit.clock = 38.635, circuit.lap_time = 34.668;
    circuit.last_lap = circuit.best_lap = -1;
    hud_shot("hud_race_banked.png", oracle ? std::string(oracle) + "/race-hud-skill-banked.png" : "", circuit_d);
  }

  if (g_failures > 0) {
    std::fprintf(stderr, "scene_screen_tests: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
  }
  std::printf("scene_screen_tests: PASS (%d checks)\n", g_checks);
  return 0;
}
