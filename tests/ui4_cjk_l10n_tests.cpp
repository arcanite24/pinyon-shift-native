#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "cars/sources.h"
#include "cars/strings.h"
#include "fh1data/archive.h"
#include "fh1data/cjk_font.h"
#include "fh1data/stringtables.h"
#include "fh1data/ui_scene.h"
#include "fh1data/vfont.h"
#include "fh1data/xds.h"
#include "fmv_player.h"
#include "game_hud.h"
#include "hud_scene.h"
#include "story_intro.h"
#include "ui.h"
#include "ui_widgets.h"

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

void test_bgf_track_and_stacker() {
  fh1::UiBgfTrack tr;
  tr.keys.push_back({1, 2, 0.0f, 0.0f, 0, 0});
  tr.keys.push_back({1, 2, 1.0f, 100.0f, 0, 0});
  tr.keys.push_back({1, 2, 2.0f, 50.0f, 0, 0});
  check(std::fabs(fh1::evaluate_bgf_track(tr, 0.0f) - 0.0f) < 1e-4f, "track eval at t=0");
  check(std::fabs(fh1::evaluate_bgf_track(tr, 0.5f) - 50.0f) < 1e-4f, "track eval at t=0.5");
  check(std::fabs(fh1::evaluate_bgf_track(tr, 1.5f) - 75.0f) < 1e-4f, "track eval at t=1.5");

  const auto ys = fh1::reflow_stacker(125.0f, 50.0f, 5);
  check(ys.size() == 5 && std::fabs(ys[0] - 125.0f) < 1e-4f && std::fabs(ys[4] - 325.0f) < 1e-4f,
        "SUPER_STACKER 50px pitch reflow");

  // Two-texture kImageMasked border-transparent crop + repeat CPU rasterizer check.
  std::vector<uint8_t> white_px(16 * 16 * 4, 255);
  std::vector<uint8_t> grad_px(16 * 16 * 4, 255);
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) {
      const uint8_t a = x < 8 ? 255 : 0;
      grad_px[(size_t(y) * 16 + size_t(x)) * 4 + 3] = a;
    }
  std::array<float, 4> uv_w{}, uv_g{};
  check(carview::ui::atlas_image("test.mask.white", 16, 16, white_px, uv_w), "atlas white");
  check(carview::ui::atlas_image("test.mask.grad", 16, 16, grad_px, uv_g), "atlas grad");
  carview::ui::List ml;
  // Border-transparent crop at ou=0.5 clips the right half (t.x > 0.5 -> t0.x > 1.0 -> transparent).
  ml.image_masked(0, 0, 64, 16, uv_w, uv_w, carview::ui::rgba(249, 0, 100, 255), {1.0f, 1.0f, 0.5f, 0.0f},
                  carview::ui::kMaskBorderTransparent);
  std::vector<uint8_t> mfb(64 * 16 * 4, 0);
  carview::ui::rasterize(ml.items(), mfb, 64, 16);
  check(mfb[(8 * 64 + 10) * 4 + 3] > 200 && mfb[(8 * 64 + 54) * 4 + 3] == 0,
        "kImageMasked border-transparent crop masks right half cleanly");

  // FontMap XML parser + resolution rules (including space suffix stripping).
  fh1::UiFontMap fm;
  const char* sample_xml = R"(
    <fontmap>
      <mapping fontname="Horizon_D" target="D" />
      <mapping fontname="D_Swiss_721_Roman_Condensed" target="A" />
      <mapping fontname="DG2_lcd" target="DG2" />
    </fontmap>
  )";
  check(fh1::parse_fontmap_xml(sample_xml, fm), "parse_fontmap_xml succeeds");
  check(fm.resolve("d_swiss_721_roman_condensed roman") == "A", "fontmap strips style suffix and resolves A");
  check(fm.resolve("d bold") == "D", "fontmap resolves 'd bold' -> D");
  check(fm.resolve("dg2_lcd") == "DG2", "fontmap resolves DG2");
  check(fm.resolve("b_helvetica63-extendedmedium medium") == "B", "fontmap leading-letter fallback -> B");

  // Transient LRU eviction check: fill transient zone past 4096x1536 and verify evictions occur without failing.
  carview::ui::atlas_clear_transient();
  const uint64_t ev_before = carview::ui::atlas_transient_evictions();
  std::vector<uint8_t> big_tile(512 * 512 * 4, 200);
  bool all_ok = true;
  for (int i = 0; i < 30; ++i) {
    std::array<float, 4> tuv{};
    if (!carview::ui::atlas_image("EventTickets/test_" + std::to_string(i), 512, 512, big_tile, tuv, true))
      all_ok = false;
  }
  check(all_ok, "transient LRU cache never overflows across 30x 512x512 EventTickets");
  check(carview::ui::atlas_transient_evictions() > ev_before, "transient LRU cache evicted oldest shelves");
  carview::ui::atlas_clear_transient();
}

void test_multiline_and_style_tags() {
  const std::string tagged = "Line 1 {font='E'}{colour='255 0 96 255'}HORIZON{font_pop}\nLine 2 {{colour='80 200 255 255'}}COLORADO";
  const std::string plain = carview::ui::List::strip_style_tags(tagged);
  check(plain == "Line 1 HORIZON\nLine 2 COLORADO", "strip_style_tags removes single & doubled tags, preserves \\n");

  carview::ui::TextStyle ts;
  ts.size = 20.0f;
  const float w_multi = carview::ui::List::measure(tagged, ts);
  const float h_multi = carview::ui::List::measure_height(tagged, ts);
  check(w_multi > 60.0f, "multi-line measure returns positive width");
  check(h_multi > 40.0f && h_multi < 60.0f, "multi-line height reflects 2 lines");

  carview::ui::List l;
  l.text(20.0f, 20.0f, tagged, ts);
  check(!l.empty(), "styled multi-line text emits UI items");
}

void test_synthetic_cjk_and_widgets() {
  auto cjk = std::make_shared<fh1::CjkFont>();
  std::vector<uint8_t> bmp(24 * 24, 0);
  for (int y = 4; y < 20; ++y)
    for (int x = 4; x < 20; ++x) bmp[size_t(y) * 24 + size_t(x)] = 220;
  cjk->set_synthetic_glyph(0x3042, 24, 20, 1, 20, 1, bmp);  // 'あ'
  cjk->set_synthetic_glyph(0x65E5, 24, 20, 1, 20, 1, bmp);  // '日'
  check(cjk->valid() && cjk->has_glyph(0x3042), "synthetic CJK glyph registered");

  carview::ui::set_cjk_font(cjk);
  check(carview::ui::cjk_font_loaded(), "carview::ui reports CJK font loaded");
  carview::ui::List l;
  carview::ui::TextStyle ts;
  ts.size = 24.0f;
  ts.keep_case = true;
  l.text(10.0f, 10.0f, "\xE3\x81\x82\xE6\x97\xA5", ts);  // "あ日"
  std::vector<uint8_t> fb(128 * 64 * 4, 0);
  carview::ui::rasterize(l.items(), fb, 128, 64);
  int lit = 0;
  for (size_t i = 0; i < fb.size(); i += 4)
    if (fb[i] > 100) ++lit;
  check(lit > 100, "synthetic CJK bitmap glyphs rasterize cleanly");
  carview::ui::set_cjk_font(nullptr);

  // LoadingScreen tip cycling + progress.
  carview::LoadingScreen ls;
  ls.start("HORIZON FESTIVAL", "COLORADO");
  ls.set_tips({"Tip A", "Tip B"});
  ls.set_progress(0.65f);
  check(ls.active() && ls.current_tip() == "Tip A", "LoadingScreen starts on Tip A");
  ls.update(5.2);
  check(ls.current_tip() == "Tip B", "LoadingScreen cycles to Tip B after 5s");
  carview::ui::List ll;
  ls.draw(ll, 640.0f, 360.0f);
  check(!ll.empty(), "LoadingScreen::draw emits UI items");

  // OnScreenKeyboard typing, plate uppercase, backspace, confirm.
  carview::OnScreenKeyboard kb;
  kb.open("LICENSE PLATE", "FH1", 8, carview::OnScreenKeyboard::Purpose::kLicensePlate);
  kb.type_char('a');
  kb.type_char('b');
  check(kb.text() == "FH1AB", "LicensePlate uppercases typed characters");
  kb.backspace();
  check(kb.text() == "FH1A", "OnScreenKeyboard backspace removes last char");
  kb.move(0, 1);  // row 1 ('Q')
  kb.press_selected();
  check(kb.text() == "FH1AQ", "OnScreenKeyboard D-pad select types 'Q'");
  carview::ui::List kl;
  kb.draw(kl, 640.0f, 360.0f);
  check(!kl.empty(), "OnScreenKeyboard::draw emits UI items");
  kb.confirm();
  check(kb.confirmed() && !kb.is_open(), "OnScreenKeyboard confirm closes and sets confirmed");
}

std::string lookup_str(const fh1::StringTables& tbls, std::string_view table, std::string_view id,
                       const char* fallback) {
  const uint32_t key = (uint32_t(fh1::string_hash(table)) << 16) | fh1::string_hash(id);
  if (auto v = tbls.resolve("_&" + std::to_string(key)); v && !v->empty()) return *v;
  return fallback;
}

void test_disc_ui4_cjk_and_18_languages(const std::filesystem::path& game_root,
                                        const std::filesystem::path& screenshot_dir) {
  std::error_code ec;
  if (!std::filesystem::exists(game_root / "media/UI.zip", ec)) return;

  // 1. Parse real disc UI4 scenes (.fbf + .bsg + .bgf) including contracts & non-quad geometries.
  auto uizip = fh1::Archive::open(game_root / "media/UI.zip", nullptr);
  check(uizip != nullptr, "opened media/UI.zip");
  if (!uizip) return;

  std::map<std::string, std::unique_ptr<fh1::UiScene>> scene_cache;
  auto load_scene = [&](const std::string& name) -> const fh1::UiScene* {
    if (auto it = scene_cache.find(name); it != scene_cache.end()) return it->second.get();
    auto fbf = uizip->read("Scenes/ui4/" + name + ".fbf");
    auto bsg = uizip->read("Scenes/ui4/" + name + ".bsg");
    auto bgf = uizip->read("Scenes/ui4/" + name + ".bgf");
    auto sc = std::make_unique<fh1::UiScene>();
    std::string err;
    if (fbf.ok() && bsg.ok() && fh1::parse_ui_fbf(fbf.data, *sc, &err) && fh1::parse_ui_bsg(bsg.data, *sc, &err)) {
      if (bgf.ok()) sc->has_bgf = fh1::parse_ui_bgf(bgf.data, sc->bgf, nullptr);
      return (scene_cache[name] = std::move(sc)).get();
    }
    return nullptr;
  };

  for (const char* sc_name : {"925_PAUSE_MENU", "947_HUD", "H_MAP", "BUTTON_BAR_SCENE", "PRE_AND_POST_RACE",
                              "H_STREET_RACE_HUB", "2_C_INI_LOADING"}) {
    const fh1::UiScene* sc = load_scene(sc_name);
    check(sc != nullptr && sc->has_bgf, sc_name);
    if (sc) {
      check(!sc->geometries.empty(), "scene parsed FBF geometry descriptors");
      check(!sc->bgf.items.empty(), "scene parsed BGF items");
    }
  }
  if (const fh1::UiScene* pause = load_scene("925_PAUSE_MENU")) {
    check(pause->bgf.has_contract("SUPER_STACKER"),
          "925_PAUSE_MENU parsed real SUPER_STACKER contract via 0xACA9EC63");
    check(pause->bgf.has_contract("PAUSE_MENU_BUTTON"),
          "925_PAUSE_MENU parsed real PAUSE_MENU_BUTTON contracts via 0xACA9EC63");
    check(!pause->bgf.styles.empty() && !pause->bgf.animations.empty(),
          "925_PAUSE_MENU parsed BGF styles and animations");
  }

  // 2. Verify all 18+ player languages load from disc string tables!
  cars::Install inst;
  std::string ierr;
  check(cars::discover_install(game_root, {}, "", inst, &ierr), "discovered disc install");
  static constexpr const char* kLanguages[] = {"EN", "GB", "FR", "DE", "IT", "ES", "MX", "BR", "NL",
                                               "DA", "NB", "SV", "FI", "PL", "CZ", "HU", "RU", "JP", "KO", "CHT"};
  for (const char* lang : kLanguages) {
    fh1::StringTables st;
    std::string lerr;
    check(cars::load_string_tables(inst, st, &lerr, lang), lang);
    check(st.size() >= 113, "language loaded all 113 disc string tables");
  }

  // 3. Verify all 3 disc CJK bitmap fonts (JPB, KORB, CHTB) parse & decode real localized glyphs!
  auto jp_font = std::make_shared<fh1::CjkFont>();
  auto ko_font = std::make_shared<fh1::CjkFont>();
  auto cht_font = std::make_shared<fh1::CjkFont>();
  check(jp_font->load_for_language(game_root, "JP"), "loaded JPB.abc / JPB.sbm");
  check(ko_font->load_for_language(game_root, "KO"), "loaded KORB.abc / KORB.sbm");
  check(cht_font->load_for_language(game_root, "CHT"), "loaded CHTB.abc / CHTB.sbm");
  {
    std::vector<uint8_t> cov;
    int w = 0, h = 0;
    check(jp_font->decode_glyph(0x3042, cov, w, h) && w == 48 && h == 48, "JPB decodes U+3042 (あ) 48x48");
    check(ko_font->decode_glyph(0xD55C, cov, w, h) && w == 48 && h == 48, "KORB decodes U+D55C (한) 48x48");
    check(cht_font->decode_glyph(0x8CFD, cov, w, h) && w == 48 && h == 48, "CHTB decodes U+8CFD (賽) 48x48");
  }

  // 3b. Verify Fonts.zip fontmap.xml + ALL vector fonts (aru/bru/dru/eru, c, sym, dg1..dg5) load!
  if (auto fzip = fh1::Archive::open(game_root / "media/ui/Fonts.zip", nullptr)) {
    check(carview::ui::load_fonts_from_zip(*fzip), "load_fonts_from_zip loads core fonts + fontmap.xml");
    check(carview::ui::font_role_loaded(carview::ui::Font::kA) &&
              carview::ui::font_role_loaded(carview::ui::Font::kB) &&
              carview::ui::font_role_loaded(carview::ui::Font::kC) &&
              carview::ui::font_role_loaded(carview::ui::Font::kD) &&
              carview::ui::font_role_loaded(carview::ui::Font::kE) &&
              carview::ui::font_role_loaded(carview::ui::Font::kSym) &&
              carview::ui::font_role_loaded(carview::ui::Font::kDg1) &&
              carview::ui::font_role_loaded(carview::ui::Font::kDg2) &&
              carview::ui::font_role_loaded(carview::ui::Font::kDg3) &&
              carview::ui::font_role_loaded(carview::ui::Font::kDg4) &&
              carview::ui::font_role_loaded(carview::ui::Font::kDg5),
          "all 11 FH1 vector fonts (A..E, SYM, DG1..DG5) loaded from Fonts.zip");
    check(carview::ui::resolve_font_name("horizon_dg2") == carview::ui::Font::kDg2, "resolve horizon_dg2");
    check(carview::ui::resolve_font_name("d bold") == carview::ui::Font::kD, "resolve 'd bold'");
    check(carview::ui::resolve_font_name("b_swiss_721_medium medium") == carview::ui::Font::kB,
          "resolve 'b_swiss_721_medium medium'");
  }

  // 4. Optional headless verification screenshots (1280x720) when SHIFT_SLICE9_SCREENSHOT_DIR is set.
  if (screenshot_dir.empty()) return;
  std::filesystem::create_directories(screenshot_dir, ec);

  // Fonts already loaded above via load_fonts_from_zip!
  auto hz = fh1::Archive::open(game_root / "media/ui/textures/Horizon.zip", nullptr);
  auto tx = fh1::Archive::open(game_root / "media/ui/Textures.zip", nullptr);
  carview::set_ui_textures([&](const std::string& path, std::array<float, 4>& uv) -> bool {
    if (carview::ui::atlas_find(path, uv)) return true;
    const bool t = path.rfind("T:", 0) == 0;
    fh1::Archive* a = t ? tx.get() : hz.get();
    if (!a) return false;
    auto r = a->read((t ? path.substr(2) : path) + ".xds");
    if (!r.ok()) return false;
    const auto x = fh1::decode_xds(r.data, false);
    if (x.levels.empty()) return false;
    return carview::ui::atlas_image(path, x.levels[0].width, x.levels[0].height, x.levels[0].rgba, uv);
  });
  carview::set_ui_scenes([&](const std::string& name) -> const fh1::UiScene* { return load_scene(name); });
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
  auto save_canvas = [&](const std::string& filename, const carview::ui::List& list,
                         std::vector<uint8_t> bg = {}) {
    if (bg.size() != size_t(W) * H * 4) {
      bg.assign(size_t(W) * H * 4, 255);
      // Warm Colorado sunset backdrop gradient.
      for (uint32_t y = 0; y < H; ++y) {
        const float t = float(y) / float(H - 1);
        const uint8_t r = uint8_t(30 + 65 * (1.0f - t));
        const uint8_t g = uint8_t(26 + 38 * (1.0f - t));
        const uint8_t b = uint8_t(42 + 48 * t);
        for (uint32_t x = 0; x < W; ++x) {
          uint8_t* p = &bg[(size_t(y) * W + x) * 4];
          p[0] = r;
          p[1] = g;
          p[2] = b;
          p[3] = 255;
        }
      }
    }
    carview::ui::rasterize(list.items(), bg, W, H);
    const auto out_p = screenshot_dir / filename;
    check(stbi_write_png(out_p.string().c_str(), int(W), int(H), 4, bg.data(), int(W * 4)) != 0,
          "wrote verification PNG");
  };

  // Screenshot (a): Real decoded FMV frame + Viper opening intro HUD overlay.
  {
    std::vector<uint8_t> bg;
    const auto fmv_path = carview::FmvPlayer::resolve_movie(game_root, carview::FmvMovieId::kFmv01Opening);
    if (!fmv_path.empty()) carview::FmvPlayer::decode_frame_at(fmv_path, 16.0, int(W), int(H), bg);
    carview::StoryIntro story;
    story.begin_new_career(game_root, true);
    story.update(1.0, {1100.0, 520.0}, false, false);
    carview::ui::List list;
    story.draw_hud(list, float(W), float(H));
    // Also draw live dial bottom-right to show Viper drive context.
    carview::DriveHud dh;
    dh.kmh = 214.0;
    dh.rpm = 6200.0;
    dh.redline_rpm = 6500.0;
    dh.gear = 4;
    carview::build_drive_hud(list, dh, float(W), float(H));
    save_canvas("slice9_fmv_viper_intro.png", list, std::move(bg));
  }

  // Screenshot (b): Authentic UI4 925_PAUSE_MENU + GPU vector-font radio popup.
  {
    carview::PauseMenuUi4State pm;
    pm.items = {"RESUME", "MAP", "EVENTS", "GARAGE", "UPGRADES", "SETTINGS", "QUIT"};
    pm.selected = 1;
    pm.wristband_goal = "COLLECT BLUE WRISTBAND";
    pm.wristband_label = "HORIZON 2012";
    pm.wristband_colour = carview::ui::rgba(0x0A, 0x9B, 0xFA);
    pm.car_name = "2013 SRT Viper GTS";
    pm.car_class = "S";
    pm.car_pi = 682;
    pm.credits_text = "117,700 Cr";
    pm.help_bar = {{"A", "SELECT"}, {"B", "RESUME"}};

    carview::RadioPopupGpuState rp;
    rp.station_name = "Radio1";
    rp.station_label = "HORIZON BASS ARENA";
    rp.title = "Levels (Skrillex Remix)";
    rp.artist = "Avicii";
    rp.alpha = 1.0f;

    carview::ui::List list;
    carview::draw_pause_menu_ui4(list, pm, float(W), float(H));
    carview::draw_radio_popup_gpu(list, rp, float(W), float(H));
    save_canvas("slice9_ui4_pause_radio.png", list);
  }

  // Screenshot (c): Animated loading screen with cycling tip + virtual on-screen keyboard overlay.
  {
    carview::LoadingScreen ls;
    ls.start("RED ROCK CANYON", "FESTIVAL CIRCUIT  •  CLASS A");
    ls.set_tips({"{colour='245 4 92 255'}PRO TIP:{colour='248 248 250 255'} Draft behind rivals and chain near-misses\nto multiply your Horizon Popularity score."});
    ls.set_progress(0.72f);
    ls.update(1.4);

    carview::OnScreenKeyboard kb;
    kb.open("LICENSE PLATE / GAMERTAG", "HRZN2012", 12, carview::OnScreenKeyboard::Purpose::kLicensePlate);
    kb.move(3, 1);  // highlight 'R'

    carview::ui::List list;
    ls.draw(list, float(W), float(H));
    kb.draw(list, float(W), float(H));
    save_canvas("slice9_loading_keyboard.png", list);
  }

  // Screenshot (d): Real Japanese (JPB), Korean (KORB), and Traditional Chinese (CHTB) localized UI4 HUD & menus!
  {
    fh1::StringTables jp_st, ko_st, cht_st;
    cars::load_string_tables(inst, jp_st, nullptr, "JP");
    cars::load_string_tables(inst, ko_st, nullptr, "KO");
    cars::load_string_tables(inst, cht_st, nullptr, "CHT");

    // Render 3 crisp side-by-side panels (JP left, KO centre, CHT right) onto one 1280x720 canvas!
    std::vector<uint8_t> fb(size_t(W) * H * 4, 255);
    for (uint32_t y = 0; y < H; ++y) {
      for (uint32_t x = 0; x < W; ++x) {
        uint8_t* p = &fb[(size_t(y) * W + x) * 4];
        p[0] = 16;
        p[1] = 18;
        p[2] = 26;
        p[3] = 255;
      }
    }

    struct LangColumn {
      const char* tag;
      std::shared_ptr<fh1::CjkFont> font;
      const fh1::StringTables* st;
      float x0;
    };
    const LangColumn cols[3] = {
        {"JAPANESE (JPB)", jp_font, &jp_st, 32.0f},
        {"KOREAN (KORB)", ko_font, &ko_st, 448.0f},
        {"TRAD. CHINESE (CHTB)", cht_font, &cht_st, 864.0f},
    };

    for (const auto& col : cols) {
      carview::ui::set_cjk_font(col.font);
      carview::ui::List list;
      const float x = col.x0;
      const float cw = 384.0f;
      list.rect(x, 36.0f, x + cw, 684.0f, carview::ui::rgba(22, 26, 36, 235), 8.0f);
      list.rect(x, 36.0f, x + cw, 86.0f, carview::ui::rgba(0xF5, 0x04, 0x5C), 6.0f);
      carview::ui::TextStyle hdr{.size = 20.0f, .color = carview::ui::rgba(255, 255, 255), .font = carview::ui::Font::kE, .keep_case = true};
      list.text(x + 18.0f, 52.0f, col.tag, hdr);

      const std::string menu_labels[] = {
          lookup_str(*col.st, "PauseMenu", "IDS_Map", "MAP"),
          lookup_str(*col.st, "PauseMenu", "IDS_Options", "OPTIONS"),
          lookup_str(*col.st, "PauseMenu", "IDS_Photo", "PHOTO"),
          lookup_str(*col.st, "PauseMenu", "IDS_MyProfile", "MY PROFILE"),
          lookup_str(*col.st, "PauseMenu", "IDS_Quit", "QUIT"),
      };
      for (int i = 0; i < 5; ++i) {
        const float ry = 108.0f + float(i) * 58.0f;
        const bool sel = (i == 0);
        list.rect(x + 16.0f, ry, x + cw - 16.0f, ry + 48.0f,
                  sel ? carview::ui::rgba(0xF5, 0x04, 0x5C) : carview::ui::rgba(14, 16, 22, 230), 4.0f);
        carview::ui::TextStyle rt{.size = 22.0f, .color = carview::ui::rgba(250, 250, 252), .font = carview::ui::Font::kE, .keep_case = true};
        list.text(x + 30.0f, ry + 13.0f, menu_labels[i], rt);
      }

      // Localized HUD & Post-race strings from real disc string tables!
      std::string bal = lookup_str(*col.st, "OnlineRewards", "IDS_Balance", "BALANCE");
      while (!bal.empty() && (bal.back() == ':' || bal.back() == ' ')) bal.pop_back();
      if (bal.size() >= 3 && bal.compare(bal.size() - 3, 3, "\xEF\xBC\x9A") == 0) bal.resize(bal.size() - 3);
      std::string cr_fmt = lookup_str(*col.st, "UnitStrings", "IDS_Units_Credits", "{0} Cr");
      if (auto p0 = cr_fmt.find("{0}"); p0 != std::string::npos)
        cr_fmt.replace(p0, 3, "117,700");
      else
        cr_fmt = "117,700 " + cr_fmt;
      const std::string hud_lines =
          lookup_str(*col.st, "InGame", "IDS_Laps", "LAPS") + " 2/4   •   " +
          lookup_str(*col.st, "InGame", "IDS_Place", "PLACE") + " 1/8\n" +
          lookup_str(*col.st, "ScreenTitles", "IDS_PostRace_Winnings", "CASH REWARD") + "\n" +
          bal + ": " + cr_fmt + "\n" +
          lookup_str(*col.st, "Activities", "IDS_EnterNow", "ENTER NOW");

      list.rect(x + 16.0f, 418.0f, x + cw - 16.0f, 590.0f, carview::ui::rgba(12, 14, 20, 225), 6.0f);
      carview::ui::TextStyle hts{.size = 19.0f, .color = carview::ui::rgba(242, 244, 248), .font = carview::ui::Font::kA, .keep_case = true};
      list.text(x + 28.0f, 434.0f, hud_lines, hts);

      // Localized Help Bar inside column.
      const std::string b_sel = lookup_str(*col.st, "HelpButtons", "IDS_Select", "SELECT");
      const std::string b_res = lookup_str(*col.st, "HelpButtons", "IDS_Resume", "RESUME");
      list.rect(x + 16.0f, 614.0f, x + cw - 16.0f, 662.0f, carview::ui::rgba(30, 34, 44, 235), 4.0f);
      carview::ui::TextStyle bts{.size = 17.0f, .color = carview::ui::rgba(248, 248, 250), .font = carview::ui::Font::kB, .keep_case = true};
      list.text(x + 28.0f, 628.0f, "[A] " + b_sel + "    [B] " + b_res, bts);

      carview::ui::rasterize(list.items(), fb, W, H);
    }
    carview::ui::set_cjk_font(nullptr);
    const auto out_p = screenshot_dir / "slice9_cjk_localized.png";
    check(stbi_write_png(out_p.string().c_str(), int(W), int(H), 4, fb.data(), int(W * 4)) != 0,
          "wrote CJK localized verification PNG");
  }
}

}  // namespace

int main() {
  test_bgf_track_and_stacker();
  test_multiline_and_style_tags();
  test_synthetic_cjk_and_widgets();

  const char* env_root = std::getenv("SHIFT_GAME_ROOT");
  std::filesystem::path game_root =
      env_root && *env_root ? std::filesystem::path(env_root)
                            : std::filesystem::path("C:/Users/neri/code/horizon1-recomp/.local/game/base");
  const char* env_shot = std::getenv("SHIFT_SLICE9_SCREENSHOT_DIR");
  std::filesystem::path shot_dir = env_shot && *env_shot ? std::filesystem::path(env_shot) : std::filesystem::path{};

  test_disc_ui4_cjk_and_18_languages(game_root, shot_dir);

  if (g_failures > 0) {
    std::fprintf(stderr, "ui4_cjk_l10n_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("ui4_cjk_l10n_tests: all checks passed\n");
  return 0;
}
