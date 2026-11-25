// The UI overlay (fh1data/ui_overlay.h): the title update's media.zip over
// the disc's UI archives, file by file. Without a game the resolver must
// fail cleanly; with --corpus (SHIFT_GAME_ROOT and SHIFT_STATE_ROOT; exit 77
// without them) it checks the v4 scenes, textures and LoadingDefs.xml win
// over the disc's and that the disc alone still resolves.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "fh1data/archive.h"
#include "fh1data/ui_overlay.h"
#include "fh1data/ui_scene.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

using Origin = fh1::UiOverlay::Origin;

const char* name_of(Origin o) { return o == Origin::kV4 ? "v4" : o == Origin::kDisc ? "disc" : "none"; }

void unit() {
  fh1::UiOverlay none({}, {});
  check(!none.has_v4(), "no v4 without a media.zip");
  check(none.v4_scene_names().empty(), "no v4 scene names without a media.zip");
  check(!none.scene_file("925_pause_menu", "fbf").ok(), "no scene without a game");
  check(!none.texture("T:UI4/Common/Icon_MissingRibbon_SM").ok(), "no texture without a game");
  check(!none.ui_file("LoadingDefs.xml").ok(), "no ui file without a game");
  fh1::UiScene sc;
  std::string e;
  Origin fo = Origin::kV4, bo = Origin::kV4;
  check(!none.load_scene("925_pause_menu", sc, &e, &fo, &bo), "load_scene fails without a game");
  check(fo == Origin::kNone && bo == Origin::kNone, "origins report none without a game");
  check(!e.empty(), "load_scene says why it failed");
  check(fh1::UiOverlay::v4_media_zip_of({}).empty(), "no state root, no media.zip");
  check(fh1::UiOverlay::v4_media_zip_of("does-not-exist").empty(), "a missing media.zip is empty");
}

// The BGF timelines (docs/UI_SCENE_ANIMATION.md) of every scene in `zip`
// under Scenes/ui4/: each reads to its sections' ends, its curves' control
// times are all 0 (the evaluator's linear case), each component's slides
// start with its Master Slide and every reference is in range.
void timelines(fh1::Archive& zip, const char* what) {
  int scenes = 0, read = 0, curves = 0, slides = 0, bad_ctrl = 0, bad_master = 0;
  for (const auto& m : zip.members()) {
    const std::string& n = m.name;
    if (n.size() < 4 || n.compare(n.size() - 4, 4, ".bgf") != 0) continue;
    std::string low = n;
    for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (low.rfind("scenes/ui4/", 0) != 0 && low.rfind("ui/scenes/ui4/", 0) != 0) continue;
    auto r = zip.read(n);
    fh1::UiBgfDocument doc;
    if (!r.ok() || !fh1::parse_ui_bgf(r.data, doc, nullptr)) continue;
    ++scenes;
    if (!doc.has_timeline) {
      std::fprintf(stderr, "FAIL: %s %s: the timeline does not read\n", what, n.c_str());
      ++failures;
      continue;
    }
    ++read;
    curves += int(doc.curves.size());
    slides += int(doc.slides.size());
    for (const auto& c : doc.curves)
      for (const auto& k : c.keys)
        if (k.c1_time != 0.0f || k.c2_time != 0.0f) ++bad_ctrl;
    std::vector<uint32_t> seen;
    for (const auto& sl : doc.slides) {
      if (std::find(seen.begin(), seen.end(), sl.component) != seen.end()) continue;
      seen.push_back(sl.component);
      if (!sl.master()) ++bad_master;
    }
  }
  std::printf("ui_overlay corpus: %s: %d scene BGFs, %d timelines, %d curves, %d slides\n", what, scenes, read,
              curves, slides);
  check(scenes > 0 && read == scenes, "every scene BGF's timeline reads");
  check(bad_ctrl == 0, "every curve key's control times are 0");
  check(bad_master == 0, "every component's slides start with its Master Slide");
}

// The decoded timelines against what is known of them.
void known_timelines(fh1::UiOverlay& ui) {
  auto doc_of = [&](const char* scene, fh1::UiBgfDocument& doc) {
    const auto f = ui.scene_file(scene, "bgf");
    return f.ok() && fh1::parse_ui_bgf(f.read.data, doc, nullptr) && doc.has_timeline;
  };
  fh1::UiBgfDocument d;
  // 120_P_PRE_SPLASH's root slides, the contract's states (START, MENU, ...).
  check(doc_of("120_P_PRE_SPLASH", d), "120_P_PRE_SPLASH's timeline");
  for (const char* n : {"Master Slide", "Start", "Start_Outro", "Start_Intro", "Menu_Intro", "Menu", "OUTRO"})
    check(d.find_slide(0, n) >= 0, (std::string("120_P_PRE_SPLASH has the slide ") + n).c_str());
  // The pause menu's INTRO and OUTRO: 300 ms, played once.
  check(doc_of("925_PAUSE_MENU", d), "925_PAUSE_MENU's timeline");
  for (const char* n : {"INTRO", "OUTRO"}) {
    const int s = d.find_slide(0, n);
    check(s >= 0 && d.slides[size_t(s)].duration_ms() == 300.0f && !d.slides[size_t(s)].loops(),
          (std::string("925_PAUSE_MENU's ") + n + " is 300 ms, once").c_str());
  }
  // H_OPENING_TITLES' PLAY: 33 s; each card's opacity curve has four keys
  // (Q164's timing, read again from the curves).
  check(doc_of("H_OPENING_TITLES", d), "H_OPENING_TITLES's timeline");
  const int play = d.find_slide(0, "PLAY");
  check(play >= 0 && d.slides[size_t(play)].end_ms == 33000, "H_OPENING_TITLES' PLAY is 33 s");
  const std::pair<uint32_t, std::array<float, 4>> cards[] = {{5, {4500, 5000, 8000, 8500}},
                                                             {11, {12000, 12500, 15500, 16000}},
                                                             {17, {18621, 19121, 22121, 22621}},
                                                             {23, {28165, 28665, 31665, 32165}}};
  for (const auto& [item, keys] : cards) {
    bool found = false;
    for (const auto& c : d.curves) {
      if (c.item != item || c.attr != fh1::ui_attr::kOpacity || c.keys.size() != 4) continue;
      found = true;
      for (size_t k = 0; k < 4; ++k) found = found && c.keys[k].time_ms == keys[k];
    }
    check(found, ("H_OPENING_TITLES: card item " + std::to_string(item) + "'s opacity keys").c_str());
  }
}

// Loads `scene` and checks its .fbf and .bsg came from `fbf` and `bsg`.
void scene(fh1::UiOverlay& ui, const char* scene, Origin fbf, Origin bsg) {
  fh1::UiScene sc;
  std::string e;
  Origin fo = Origin::kNone, bo = Origin::kNone;
  const bool ok = ui.load_scene(scene, sc, &e, &fo, &bo);
  if (!ok || fo != fbf || bo != bsg) {
    std::fprintf(stderr, "FAIL: scene %s: loaded=%d fbf=%s (want %s) bsg=%s (want %s) %s\n", scene, ok,
                 name_of(fo), name_of(fbf), name_of(bo), name_of(bsg), e.c_str());
    ++failures;
  }
}

void texture(fh1::UiOverlay& ui, const char* path, Origin want) {
  const fh1::UiOverlay::File f = ui.texture(path);
  if (!f.ok() || f.origin != want || f.read.data.empty()) {
    std::fprintf(stderr, "FAIL: texture %s: origin=%s (want %s) bytes=%zu\n", path, name_of(f.origin), name_of(want),
                 f.read.data.size());
    ++failures;
  }
}

int corpus(const std::filesystem::path& game_root, const std::filesystem::path& state_root) {
  const auto media = fh1::UiOverlay::v4_media_zip_of(state_root);
  if (media.empty()) {
    std::fprintf(stderr, "FAIL: no title-update-v4/media.zip under %s\n", state_root.string().c_str());
    return 1;
  }
  fh1::UiOverlay ui(game_root, media);
  check(ui.has_v4(), "v4 media.zip opens");

  // v4 ships 37 scenes with an .fbf (40 names; three ship only a .bsg or .bgf).
  const std::vector<std::string> names = ui.v4_scene_names();
  std::printf("ui_overlay corpus: %zu v4 scenes\n", names.size());
  check(names.size() == 37, "v4 ships 37 scenes with an .fbf");
  int loaded = 0;
  for (const auto& n : names) {
    fh1::UiScene sc;
    std::string e;
    Origin fo = Origin::kNone;
    if (ui.load_scene(n, sc, &e, &fo) && fo == Origin::kV4) ++loaded;
    else std::fprintf(stderr, "FAIL: v4 scene %s: %s\n", n.c_str(), e.c_str());
  }
  check(loaded == int(names.size()), "every v4 scene loads from v4");

  // Whole v4 overrides of disc scenes, either case of name.
  scene(ui, "925_pause_menu", Origin::kV4, Origin::kV4);
  scene(ui, "947_HUD", Origin::kV4, Origin::kV4);
  // v4 ships lap_time_leaderboard's .fbf and .bgf but not its .bsg, and only
  // 032_d_pop_message's .bsg: the other file stays the disc's.
  scene(ui, "lap_time_leaderboard", Origin::kV4, Origin::kDisc);
  scene(ui, "032_d_pop_message", Origin::kDisc, Origin::kV4);
  // v4-only scenes (the 1000 Club and Rally screens).
  scene(ui, "h_car_challenge_hub", Origin::kV4, Origin::kV4);
  scene(ui, "h_rally_hub", Origin::kV4, Origin::kV4);
  // A scene v4 does not ship.
  scene(ui, "030_bonus_content", Origin::kDisc, Origin::kDisc);

  // Textures: "T:" paths over Textures.zip, others over Horizon.zip.
  texture(ui, "T:UI4/Common/Icon_MissingRibbon_SM", Origin::kV4);
  texture(ui, "T:UI4/Common/Icon_MissingRibbon", Origin::kDisc);
  texture(ui, "DLC/CarChallenge/MedalHudUnlock", Origin::kV4);
  texture(ui, "Loading/BGLoadingImages/Image11", Origin::kV4);
  texture(ui, "Loading/BGLoadingImages/ControllerLoadingBackground", Origin::kDisc);
  {
    const auto v4 = ui.texture("T:UI4/Common/Icon_MissingRibbon_SM");
    fh1::UiOverlay disc(game_root, {});
    const auto d = disc.texture("T:UI4/Common/Icon_MissingRibbon_SM");
    check(d.ok() && d.origin == Origin::kDisc, "the disc alone resolves the disc texture");
    check(v4.read.data != d.read.data, "the v4 texture is not the disc's");
  }

  // The timelines: the disc's scenes, v4's, and the known ones as mounted.
  if (auto zip = fh1::Archive::open(game_root / "media" / "UI.zip", nullptr)) timelines(*zip, "disc UI.zip");
  else check(false, "UI.zip opens");
  if (auto zip = fh1::Archive::open(media, nullptr)) timelines(*zip, "v4 media.zip");
  else check(false, "v4 media.zip opens");
  known_timelines(ui);

  // LoadingDefs.xml: v4's copy (more tips) over the disc's loose file.
  {
    const auto v4 = ui.ui_file("LoadingDefs.xml");
    fh1::UiOverlay disc(game_root, {});
    const auto d = disc.ui_file("LoadingDefs.xml");
    check(v4.ok() && v4.origin == Origin::kV4, "LoadingDefs.xml resolves to v4");
    check(d.ok() && d.origin == Origin::kDisc, "LoadingDefs.xml resolves to the disc without v4");
    check(v4.read.data != d.read.data, "v4's LoadingDefs.xml is not the disc's");
  }

  // The disc alone: the same scenes resolve to the disc, and v4-only files
  // are absent.
  {
    fh1::UiOverlay disc(game_root, {});
    check(!disc.has_v4(), "no v4 without a state root");
    scene(disc, "925_pause_menu", Origin::kDisc, Origin::kDisc);
    scene(disc, "947_hud", Origin::kDisc, Origin::kDisc);
    int v4_only = 0, absent = 0;
    for (const auto& n : names) {
      if (disc.scene_file(n, "fbf").ok()) continue;
      ++v4_only;
      fh1::UiScene sc;
      std::string e;
      if (!disc.load_scene(n, sc, &e)) ++absent;
    }
    std::printf("ui_overlay corpus: %d v4-only scenes\n", v4_only);
    check(v4_only > 0, "v4 ships scenes the disc lacks");
    check(absent == v4_only, "v4-only scenes do not load from the disc alone");
  }
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--corpus") == 0) {
    const char* game = std::getenv("SHIFT_GAME_ROOT");
    const char* state = std::getenv("SHIFT_STATE_ROOT");
    if (!game || !*game || !state || !*state) {
      std::printf("ui_overlay_tests --corpus: SHIFT_GAME_ROOT and SHIFT_STATE_ROOT not set, skipped\n");
      return 77;
    }
    if (corpus(game, state)) {
      std::fprintf(stderr, "ui_overlay_tests --corpus: %d failure(s)\n", failures);
      return 1;
    }
    std::printf("ui_overlay_tests --corpus: all passed\n");
    return 0;
  }
  unit();
  if (failures) {
    std::fprintf(stderr, "ui_overlay_tests: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("ui_overlay_tests: all passed\n");
  return 0;
}
